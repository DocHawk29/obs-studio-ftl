/******************************************************************************
    Copyright (C) 2014 by Hugh Bailey <obs.jim@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include <obs-module.h>
#include <obs-avc.h>
#include <util/platform.h>
#include <util/circlebuf.h>
#include <util/dstr.h>
#include <util/threading.h>
#include <inttypes.h>
//#include "librtmp/rtmp.h"
//#include "librtmp/log.h"
#include "libftl/ftl.h"
#include "flv-mux.h"
#include "net-if.h"

#ifdef _WIN32
#include <Iphlpapi.h>
#else
#include <sys/ioctl.h>
#endif

#define do_log(level, format, ...) \
	blog(level, "[ftl stream: '%s'] " format, \
			obs_output_get_name(stream->output), ##__VA_ARGS__)

#define warn(format, ...)  do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...)  do_log(LOG_INFO,    format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG,   format, ##__VA_ARGS__)

#define OPT_DROP_THRESHOLD "drop_threshold_ms"
#define OPT_MAX_SHUTDOWN_TIME_SEC "max_shutdown_time_sec"
#define OPT_BIND_IP "bind_ip"

//#define TEST_FRAMEDROPS

struct ftl_stream {
	obs_output_t     *output;

	pthread_mutex_t  packets_mutex;
	struct circlebuf packets;
	bool             sent_headers;

	volatile bool    connecting;
	pthread_t        connect_thread;

	volatile bool    active;
	volatile bool    disconnected;
	pthread_t        send_thread;

	int              max_shutdown_time_sec;

	os_sem_t         *send_sem;
	os_event_t       *stop_event;
	uint64_t         stop_ts;

	struct dstr      path, key;
	uint32_t         channel_id;
	struct dstr      username, password;
	struct dstr      encoder_name;
	struct dstr      bind_ip;

	/* frame drop variables */
	int64_t          drop_threshold_usec;
	int64_t          min_drop_dts_usec;
	int              min_priority;

	int64_t          last_dts_usec;

	uint64_t         total_bytes_sent;
	int              dropped_frames;

//	RTMP             rtmp;
  SOCKET           sb_socket;
  uint32_t         audio_ssrc, video_ssrc, scale_width, scale_height;
	ftl_stream_configuration_t* stream_config;
	ftl_stream_video_component_t* video_component;
	ftl_stream_audio_component_t* audio_component;
};


void log_libftl_messages(ftl_log_severity_t log_level, const char * message);
static bool init_connect(struct ftl_stream *stream);
static void *connect_thread(void *data);

static const char *ftl_stream_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("FTLStream");
}

static void log_ftl(int level, const char *format, va_list args)
{
//	if (level > RTMP_LOGWARNING)
//		return;

	blogva(LOG_INFO, format, args);
}

static inline size_t num_buffered_packets(struct ftl_stream *stream);

static inline void free_packets(struct ftl_stream *stream)
{
	size_t num_packets;

	pthread_mutex_lock(&stream->packets_mutex);

	num_packets = num_buffered_packets(stream);
	if (num_packets)
		info("Freeing %d remaining packets", (int)num_packets);

	while (stream->packets.size) {
		struct encoder_packet packet;
		circlebuf_pop_front(&stream->packets, &packet, sizeof(packet));
		obs_free_encoder_packet(&packet);
	}
	pthread_mutex_unlock(&stream->packets_mutex);
}

static inline bool stopping(struct ftl_stream *stream)
{
	return os_event_try(stream->stop_event) != EAGAIN;
}

static inline bool connecting(struct ftl_stream *stream)
{
	return os_atomic_load_bool(&stream->connecting);
}

static inline bool active(struct ftl_stream *stream)
{
	return os_atomic_load_bool(&stream->active);
}

static inline bool disconnected(struct ftl_stream *stream)
{
	return os_atomic_load_bool(&stream->disconnected);
}

static void ftl_stream_destroy(void *data)
{
	struct ftl_stream *stream = data;

	info("ftl_stream_destroy\n");

	if (stopping(stream) && !connecting(stream)) {
		pthread_join(stream->send_thread, NULL);

	} else if (connecting(stream) || active(stream)) {
		if (stream->connecting)
			pthread_join(stream->connect_thread, NULL);

		stream->stop_ts = 0;
		os_event_signal(stream->stop_event);

		if (active(stream)) {
			os_sem_post(stream->send_sem);
			obs_output_end_data_capture(stream->output);
			pthread_join(stream->send_thread, NULL);
		}
	}

	if (stream) {
		free_packets(stream);
		dstr_free(&stream->path);
		dstr_free(&stream->key);
		dstr_free(&stream->username);
		dstr_free(&stream->password);
		dstr_free(&stream->encoder_name);
		dstr_free(&stream->bind_ip);
		os_event_destroy(stream->stop_event);
		os_sem_destroy(stream->send_sem);
		pthread_mutex_destroy(&stream->packets_mutex);
		circlebuf_free(&stream->packets);
		bfree(stream);
	}
}

static void *ftl_stream_create(obs_data_t *settings, obs_output_t *output)
{
	ftl_status_t status_code;	
	struct ftl_stream *stream = bzalloc(sizeof(struct ftl_stream));
	info("ftl_stream_create\n");
	
	stream->output = output;
	pthread_mutex_init_value(&stream->packets_mutex);
/*
	RTMP_Init(&stream->rtmp);
	RTMP_LogSetCallback(log_rtmp);
	RTMP_LogSetLevel(RTMP_LOGWARNING);
*/
	ftl_init();
	ftl_register_log_handler(log_libftl_messages);

	status_code = ftl_create_stream_configuration(&(stream->stream_config));
	if (status_code != FTL_SUCCESS) {
		blog(LOG_WARNING, "Failed to initialize stream configuration: errno %d\n", status_code);
		goto fail;
	}

	if (pthread_mutex_init(&stream->packets_mutex, NULL) != 0)
		goto fail;
	if (os_event_init(&stream->stop_event, OS_EVENT_TYPE_MANUAL) != 0)
		goto fail;

	UNUSED_PARAMETER(settings);
	return stream;

fail:
	ftl_stream_destroy(stream);

	return NULL;
}

static void ftl_stream_stop(void *data, uint64_t ts)
{
	struct ftl_stream *stream = data;
	info("ftl_stream_stop\n");

	if (stopping(stream))
		return;

	if (connecting(stream))
		pthread_join(stream->connect_thread, NULL);

	stream->stop_ts = ts / 1000ULL;
	os_event_signal(stream->stop_event);

	if (active(stream)) {
		if (stream->stop_ts == 0)
			os_sem_post(stream->send_sem);
	}
}

/*
static inline void set_rtmp_str(AVal *val, const char *str)
{
	bool valid  = (str && *str);
	val->av_val = valid ? (char*)str       : NULL;
	val->av_len = valid ? (int)strlen(str) : 0;
}

static inline void set_rtmp_dstr(AVal *val, struct dstr *str)
{
	bool valid  = !dstr_is_empty(str);
	val->av_val = valid ? str->array    : NULL;
	val->av_len = valid ? (int)str->len : 0;
}
*/
static inline bool get_next_packet(struct ftl_stream *stream,
		struct encoder_packet *packet)
{
	bool new_packet = false;

	pthread_mutex_lock(&stream->packets_mutex);
	if (stream->packets.size) {
		circlebuf_pop_front(&stream->packets, packet,
				sizeof(struct encoder_packet));
		new_packet = true;
	}
	pthread_mutex_unlock(&stream->packets_mutex);

	return new_packet;
}


static bool discard_recv_data(struct ftl_stream *stream, size_t size)
{
//	RTMP *rtmp = &stream->rtmp;
	uint8_t buf[512];
#ifdef _WIN32
	int ret;
#else
	ssize_t ret;
#endif

	do {
		size_t bytes = size > 512 ? 512 : size;
		size -= bytes;

#ifdef _WIN32
		ret = recv(stream->sb_socket, buf, (int)bytes, 0);
#else
		ret = recv(stream->sb_socket, buf, bytes, 0);
#endif

		if (ret <= 0) {
#ifdef _WIN32
			int error = WSAGetLastError();
#else
			int error = errno;
#endif
			if (ret < 0) {
				do_log(LOG_ERROR, "recv error: %d (%d bytes)",
						error, (int)size);
			}
			return false;
		}
	} while (size > 0);

	return true;
}



static int send_packet(struct ftl_stream *stream,
		struct encoder_packet *packet, bool is_header, size_t idx)
{
	uint8_t *data;
	size_t  size;
	int     recv_size = 0;
	int     ret = 0;

#ifdef _WIN32
	ret = ioctlsocket(stream->sb_socket, FIONREAD,
			(u_long*)&recv_size);
#else
	ret = ioctl(stream->sb_socket, FIONREAD, &recv_size);
#endif

	if (ret >= 0 && recv_size > 0) {
		if (!discard_recv_data(stream, (size_t)recv_size))
			return -1;
	}

	flv_packet_mux(packet, &data, &size, is_header);
#ifdef TEST_FRAMEDROPS
	os_sleep_ms(rand() % 40);
#endif
	//ret = RTMP_Write(&stream->rtmp, (char*)data, (int)size, (int)idx);
	bfree(data);

	obs_free_encoder_packet(packet);

	stream->total_bytes_sent += size;
	return ret;
}

//static inline bool send_headers(struct ftl_stream *stream);

static void *send_thread(void *data)
{
	struct ftl_stream *stream = data;

	os_set_thread_name("ftl-stream: send_thread");

	while (os_sem_wait(stream->send_sem) == 0) {
		struct encoder_packet packet;

		if (stopping(stream) && stream->stop_ts == 0) {
			break;
		}

		if (!get_next_packet(stream, &packet))
			continue;

		if (stopping(stream)) {
			if (packet.sys_dts_usec >= (int64_t)stream->stop_ts) {
				obs_free_encoder_packet(&packet);
				break;
			}
		}

/*
		if (!stream->sent_headers) {
			if (!send_headers(stream)) {
				os_atomic_set_bool(&stream->disconnected, true);
				break;
			}
		}
*/

		if (send_packet(stream, &packet, false, packet.track_idx) < 0) {
			os_atomic_set_bool(&stream->disconnected, true);
			break;
		}
	}

	if (disconnected(stream)) {
		info("Disconnected from %s", stream->path.array);
	} else {
		info("User stopped the stream");
	}

	//RTMP_Close(&stream->rtmp);

	if (!stopping(stream)) {
		pthread_detach(stream->send_thread);
		obs_output_signal_stop(stream->output, OBS_OUTPUT_DISCONNECTED);
	} else {
		obs_output_end_data_capture(stream->output);
	}

	free_packets(stream);
	os_event_reset(stream->stop_event);
	os_atomic_set_bool(&stream->active, false);
	stream->sent_headers = false;
	return NULL;
}
/*
static bool send_meta_data(struct ftl_stream *stream, size_t idx, bool *next)
{
	uint8_t *meta_data;
	size_t  meta_data_size;
	bool    success = true;

	*next = flv_meta_data(stream->output, &meta_data,
			&meta_data_size, false, idx);

	if (*next) {
		success = RTMP_Write(&stream->rtmp, (char*)meta_data,
				(int)meta_data_size, (int)idx) >= 0;
		bfree(meta_data);
	}

	return success;
}

static bool send_audio_header(struct ftl_stream *stream, size_t idx,
		bool *next)
{
	obs_output_t  *context  = stream->output;
	obs_encoder_t *aencoder = obs_output_get_audio_encoder(context, idx);
	uint8_t       *header;

	struct encoder_packet packet   = {
		.type         = OBS_ENCODER_AUDIO,
		.timebase_den = 1
	};

	if (!aencoder) {
		*next = false;
		return true;
	}

	obs_encoder_get_extra_data(aencoder, &header, &packet.size);
	packet.data = bmemdup(header, packet.size);
	return send_packet(stream, &packet, true, idx) >= 0;
}

static bool send_video_header(struct ftl_stream *stream)
{
	obs_output_t  *context  = stream->output;
	obs_encoder_t *vencoder = obs_output_get_video_encoder(context);
	uint8_t       *header;
	size_t        size;

	struct encoder_packet packet   = {
		.type         = OBS_ENCODER_VIDEO,
		.timebase_den = 1,
		.keyframe     = true
	};

	obs_encoder_get_extra_data(vencoder, &header, &size);
	packet.size = obs_parse_avc_header(&packet.data, header, size);
	return send_packet(stream, &packet, true, 0) >= 0;
}

static inline bool send_headers(struct ftl_stream *stream)
{
	stream->sent_headers = true;
	size_t i = 0;
	bool next = true;

	if (!send_audio_header(stream, i++, &next))
		return false;
	if (!send_video_header(stream))
		return false;

	while (next) {
		if (!send_audio_header(stream, i++, &next))
			return false;
	}

	return true;
}
*/
static inline bool reset_semaphore(struct ftl_stream *stream)
{
	os_sem_destroy(stream->send_sem);
	return os_sem_init(&stream->send_sem, 0) == 0;
}

#ifdef _WIN32
#define socklen_t int
#endif

#define MIN_SENDBUF_SIZE 65535

static void adjust_sndbuf_size(struct ftl_stream *stream, int new_size)
{
	/*
	int cur_sendbuf_size = new_size;
	socklen_t int_size = sizeof(int);

	getsockopt(stream->sb_socket, SOL_SOCKET, SO_SNDBUF,
			(char*)&cur_sendbuf_size, &int_size);

	if (cur_sendbuf_size < new_size) {
		cur_sendbuf_size = new_size;
		setsockopt(stream->sb_socket, SOL_SOCKET, SO_SNDBUF,
				(const char*)&cur_sendbuf_size, int_size);
	}
	*/
}

static int init_send(struct ftl_stream *stream)
{
	int ret;
	size_t idx = 0;
	bool next = true;

#if defined(_WIN32)
	adjust_sndbuf_size(stream, MIN_SENDBUF_SIZE);
#endif

	reset_semaphore(stream);

	ret = pthread_create(&stream->send_thread, NULL, send_thread, stream);
	if (ret != 0) {
		//RTMP_Close(&stream->rtmp);
		warn("Failed to create send thread");
		return OBS_OUTPUT_ERROR;
	}

	os_atomic_set_bool(&stream->active, true);
	/*
	while (next) {
		if (!send_meta_data(stream, idx++, &next)) {
			warn("Disconnected while attempting to connect to "
			     "server.");
			return OBS_OUTPUT_DISCONNECTED;
		}
	}
	*/
	obs_output_begin_data_capture(stream->output, 0);

	return OBS_OUTPUT_SUCCESS;
}

#ifdef _WIN32
/*
static void win32_log_interface_type(struct ftl_stream *stream)
{
	RTMP *rtmp = &stream->rtmp;
	MIB_IPFORWARDROW route;
	uint32_t dest_addr, source_addr;
	char hostname[256];
	HOSTENT *h;

	if (rtmp->Link.hostname.av_len >= sizeof(hostname) - 1)
		return;

	strncpy(hostname, rtmp->Link.hostname.av_val, sizeof(hostname));
	hostname[rtmp->Link.hostname.av_len] = 0;

	h = gethostbyname(hostname);
	if (!h)
		return;

	dest_addr = *(uint32_t*)h->h_addr_list[0];

	if (rtmp->m_bindIP.addrLen == 0)
		source_addr = 0;
	else if (rtmp->m_bindIP.addr.ss_family == AF_INET)
		source_addr = (*(struct sockaddr_in*)&rtmp->m_bindIP)
			.sin_addr.S_un.S_addr;
	else
		return;

	if (!GetBestRoute(dest_addr, source_addr, &route)) {
		MIB_IFROW row;
		memset(&row, 0, sizeof(row));
		row.dwIndex = route.dwForwardIfIndex;

		if (!GetIfEntry(&row)) {
			uint32_t speed =row.dwSpeed / 1000000;
			char *type;
			struct dstr other = {0};

			if (row.dwType == IF_TYPE_ETHERNET_CSMACD) {
				type = "ethernet";
			} else if (row.dwType == IF_TYPE_IEEE80211) {
				type = "802.11";
			} else {
				dstr_printf(&other, "type %lu", row.dwType);
				type = other.array;
			}

			info("Interface: %s (%s, %lu mbps)", row.bDescr, type,
					speed);

			dstr_free(&other);
		}
	}
}
*/
#endif

static int try_connect(struct ftl_stream *stream)
{
	ftl_status_t status_code;
	
	if (dstr_is_empty(&stream->path)) {
		warn("URL is empty");
		return OBS_OUTPUT_BAD_PATH;
	}

	info("Connecting to FTL Ingest URL %s...", stream->path.array);

	ftl_set_ingest_location(stream->stream_config, stream->path.array);
	ftl_set_authetication_key(stream->stream_config, stream->channel_id, stream->key.array);	

	stream->video_component = ftl_create_video_component(FTL_VIDEO_H264, 96, stream->video_ssrc, stream->scale_width, stream->scale_height);
	ftl_attach_video_component_to_stream(stream->stream_config, stream->video_component);

	stream->audio_component = ftl_create_audio_component(FTL_AUDIO_OPUS, 97, stream->audio_ssrc);
	blog(LOG_WARNING, "FTL ssrc: video %d, audio %d\n", stream->audio_ssrc, stream->video_ssrc);
	ftl_attach_audio_component_to_stream(stream->stream_config, stream->audio_component);

	status_code = ftl_activate_stream(stream->stream_config);

	int obs_status = map_ftl_error_to_obs_error(status_code);

	if (status_code != FTL_SUCCESS) {
		blog(LOG_ERROR, "Failed to initialize FTL Stream");
		ftl_destory_stream(&(stream->stream_config));
		stream->stream_config = 0;
		return obs_status;
	}

/*
	memset(&stream->rtmp.Link, 0, sizeof(stream->rtmp.Link));
	if (!RTMP_SetupURL(&stream->rtmp, stream->path.array))
		return OBS_OUTPUT_BAD_PATH;

	RTMP_EnableWrite(&stream->rtmp);

	dstr_copy(&stream->encoder_name, "FMLE/3.0 (compatible; obs-studio/");

#ifdef HAVE_OBSCONFIG_H
	dstr_cat(&stream->encoder_name, OBS_VERSION);
#else
	dstr_catf(&stream->encoder_name, "%d.%d.%d",
			LIBOBS_API_MAJOR_VER,
			LIBOBS_API_MINOR_VER,
			LIBOBS_API_PATCH_VER);
#endif

	dstr_cat(&stream->encoder_name, "; FMSc/1.0)");

	set_rtmp_dstr(&stream->rtmp.Link.pubUser,   &stream->username);
	set_rtmp_dstr(&stream->rtmp.Link.pubPasswd, &stream->password);
	set_rtmp_dstr(&stream->rtmp.Link.flashVer,  &stream->encoder_name);
	stream->rtmp.Link.swfUrl = stream->rtmp.Link.tcUrl;

	if (dstr_is_empty(&stream->bind_ip) ||
	    dstr_cmp(&stream->bind_ip, "default") == 0) {
		memset(&stream->rtmp.m_bindIP, 0, sizeof(stream->rtmp.m_bindIP));
	} else {
		bool success = netif_str_to_addr(&stream->rtmp.m_bindIP.addr,
				&stream->rtmp.m_bindIP.addrLen,
				stream->bind_ip.array);
		if (success)
			info("Binding to IP");
	}

	RTMP_AddStream(&stream->rtmp, stream->key.array);
*/
/*
	for (size_t idx = 1;; idx++) {
		obs_encoder_t *encoder = obs_output_get_audio_encoder(
				stream->output, idx);
		const char *encoder_name;

		if (!encoder)
			break;

		encoder_name = obs_encoder_get_name(encoder);
		//RTMP_AddStream(&stream->rtmp, encoder_name);

	}
	*/
/*
	stream->rtmp.m_outChunkSize       = 4096;
	stream->rtmp.m_bSendChunkSizeInfo = true;
	stream->rtmp.m_bUseNagle          = true;

#ifdef _WIN32
	win32_log_interface_type(stream);
#endif

	if (!RTMP_Connect(&stream->rtmp, NULL))
		return OBS_OUTPUT_CONNECT_FAILED;
	if (!RTMP_ConnectStream(&stream->rtmp, 0))
		return OBS_OUTPUT_INVALID_STREAM;
*/

	info("Connection to %s successful", stream->path.array);

	return init_send(stream);
}

static bool ftl_stream_start(void *data)
{
	struct ftl_stream *stream = data;

	info("ftl_stream_start\n");

	if (!obs_output_can_begin_data_capture(stream->output, 0))
		return false;
	if (!obs_output_initialize_encoders(stream->output, 0))
		return false;

	os_atomic_set_bool(&stream->connecting, true);
	return pthread_create(&stream->connect_thread, NULL, connect_thread,
			stream) == 0;
}

static inline bool add_packet(struct ftl_stream *stream,
		struct encoder_packet *packet)
{
	circlebuf_push_back(&stream->packets, packet,
			sizeof(struct encoder_packet));
	stream->last_dts_usec = packet->dts_usec;
	return true;
}

static inline size_t num_buffered_packets(struct ftl_stream *stream)
{
	return stream->packets.size / sizeof(struct encoder_packet);
}
/*
static void drop_frames(struct ftl_stream *stream)
{
	struct circlebuf new_buf            = {0};
	int              drop_priority      = 0;
	uint64_t         last_drop_dts_usec = 0;
	int              num_frames_dropped = 0;

	debug("Previous packet count: %d", (int)num_buffered_packets(stream));

	circlebuf_reserve(&new_buf, sizeof(struct encoder_packet) * 8);

	while (stream->packets.size) {
		struct encoder_packet packet;
		circlebuf_pop_front(&stream->packets, &packet, sizeof(packet));

		last_drop_dts_usec = packet.dts_usec;

		// do not drop audio data or video keyframes 
		if (packet.type          == OBS_ENCODER_AUDIO ||
		    packet.drop_priority == OBS_NAL_PRIORITY_HIGHEST) {
			circlebuf_push_back(&new_buf, &packet, sizeof(packet));

		} else {
			if (drop_priority < packet.drop_priority)
				drop_priority = packet.drop_priority;

			num_frames_dropped++;
			obs_free_encoder_packet(&packet);
		}
	}

	circlebuf_free(&stream->packets);
	stream->packets           = new_buf;
	stream->min_priority      = drop_priority;
	stream->min_drop_dts_usec = last_drop_dts_usec;

	stream->dropped_frames += num_frames_dropped;
	debug("New packet count: %d", (int)num_buffered_packets(stream));
}

static void check_to_drop_frames(struct ftl_stream *stream)
{
	struct encoder_packet first;
	int64_t buffer_duration_usec;

	if (num_buffered_packets(stream) < 5)
		return;

	circlebuf_peek_front(&stream->packets, &first, sizeof(first));

	//do not drop frames if frames were just dropped within this time
	if (first.dts_usec < stream->min_drop_dts_usec)
		return;

	// if the amount of time stored in the buffered packets waiting to be
	// sent is higher than threshold, drop frames 
	buffer_duration_usec = stream->last_dts_usec - first.dts_usec;

	if (buffer_duration_usec > stream->drop_threshold_usec) {
		drop_frames(stream);
		debug("dropping %" PRId64 " worth of frames",
				buffer_duration_usec);
	}
}

static bool add_video_packet(struct ftl_stream *stream,
		struct encoder_packet *packet)
{
	check_to_drop_frames(stream);

	// if currently dropping frames, drop packets until it reaches the
	// desired priority 
	if (packet->priority < stream->min_priority) {
		stream->dropped_frames++;
		return false;
	} else {
		stream->min_priority = 0;
	}

	return add_packet(stream, packet);
}
*/

static void ftl_stream_data(void *data, struct encoder_packet *packet)
{
	struct ftl_stream    *stream = data;

	info("ftl_stream_data\n");
	/*
	struct encoder_packet new_packet;
	bool                  added_packet = false;

	if (disconnected(stream) || !active(stream))
		return;

	if (packet->type == OBS_ENCODER_VIDEO)
		obs_parse_avc_packet(&new_packet, packet);
	else
		obs_duplicate_encoder_packet(&new_packet, packet);

	pthread_mutex_lock(&stream->packets_mutex);

	if (!disconnected(stream)) {
		added_packet = (packet->type == OBS_ENCODER_VIDEO) ?
			add_video_packet(stream, &new_packet) :
			add_packet(stream, &new_packet);
	}

	pthread_mutex_unlock(&stream->packets_mutex);

	if (added_packet)
		os_sem_post(stream->send_sem);
	else
		obs_free_encoder_packet(&new_packet);
		*/
}

static void ftl_stream_defaults(obs_data_t *defaults)
{
	/*
	obs_data_set_default_int(defaults, OPT_DROP_THRESHOLD, 600);
	obs_data_set_default_int(defaults, OPT_MAX_SHUTDOWN_TIME_SEC, 5);
	obs_data_set_default_string(defaults, OPT_BIND_IP, "default");
	*/
	//info("ftl_stream_defaults\n");
}


static obs_properties_t *ftl_stream_properties(void *unused)
{

	//info("ftl_stream_properties\n");
	UNUSED_PARAMETER(unused);

	obs_properties_t *props = obs_properties_create();
		/*
	struct netif_saddr_data addrs = {0};
	obs_property_t *p;

	obs_properties_add_int(props, OPT_DROP_THRESHOLD,
			obs_module_text("RTMPStream.DropThreshold"),
			200, 10000, 100);

	p = obs_properties_add_list(props, OPT_BIND_IP,
			obs_module_text("RTMPStream.BindIP"),
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	obs_property_list_add_string(p, obs_module_text("Default"), "default");

	netif_get_addrs(&addrs);
	for (size_t i = 0; i < addrs.addrs.num; i++) {
		struct netif_saddr_item item = addrs.addrs.array[i];
		obs_property_list_add_string(p, item.name, item.addr);
	}
	netif_saddr_data_free(&addrs);
*/
	return props;
}


static uint64_t ftl_stream_total_bytes_sent(void *data)
{
	struct ftl_stream *stream = data;
	info("ftl_stream_total_bytes_sent\n");

	return 0;
	/*
	return stream->total_bytes_sent;
	*/
}

static int ftl_stream_dropped_frames(void *data)
{
	struct ftl_stream *stream = data;
	info("ftl_stream_dropped_frames\n");
	return 0;
	//return stream->dropped_frames;
}



/*********************************************************************/


static void *connect_thread(void *data)
{
	struct ftl_stream *stream = data;
	int ret;

	os_set_thread_name("ftl-stream: connect_thread");

	blog(LOG_WARNING, "ftl-stream: connect thread\n");

	if (!init_connect(stream)) {
		obs_output_signal_stop(stream->output, OBS_OUTPUT_BAD_PATH);
		return NULL;
	}

	ret = try_connect(stream);

	if (ret != OBS_OUTPUT_SUCCESS) {
		obs_output_signal_stop(stream->output, ret);
		info("Connection to %s failed: %d", stream->path.array, ret);
	}

	if (!stopping(stream))
		pthread_detach(stream->connect_thread);

	os_atomic_set_bool(&stream->connecting, false);
	return NULL;
}

void log_libftl_messages(ftl_log_severity_t log_level, const char * message)
{
	UNUSED_PARAMETER(log_level);
  blog(LOG_WARNING, "[libftl] %s", message);
}

static bool init_connect(struct ftl_stream *stream)
{
	obs_service_t *service;
	obs_data_t *settings;
	const char *bind_ip, *key;
	char stream_key[25];

	info("init_connect\n");

	if (stopping(stream))
		pthread_join(stream->send_thread, NULL);

	free_packets(stream);

	service = obs_output_get_service(stream->output);
	if (!service)
		return false;

	os_atomic_set_bool(&stream->disconnected, false);
	stream->total_bytes_sent = 0;
	stream->dropped_frames   = 0;
	stream->min_drop_dts_usec= 0;
	stream->min_priority     = 0;

	settings = obs_output_get_settings(stream->output);
	dstr_copy(&stream->path,     obs_service_get_url(service));
	key = obs_service_get_key(service);
	sscanf(key, "%d-%s", &stream->channel_id, stream_key);

	info("key: %s, Stream key %s, channel id %d\n", key, stream->channel_id, stream_key);

	//dstr_copy(&stream->key,      obs_service_get_key(service));
	dstr_copy(&stream->key,      stream_key);
	dstr_copy(&stream->username, obs_service_get_username(service));
	dstr_copy(&stream->password, obs_service_get_password(service));
	dstr_depad(&stream->path);
	dstr_depad(&stream->key);
/*	
	stream->drop_threshold_usec =
		(int64_t)obs_data_get_int(settings, OPT_DROP_THRESHOLD) * 1000;
	stream->max_shutdown_time_sec =
		(int)obs_data_get_int(settings, OPT_MAX_SHUTDOWN_TIME_SEC);
*/
	bind_ip = obs_data_get_string(settings, OPT_BIND_IP);
	dstr_copy(&stream->bind_ip, bind_ip);

	obs_data_release(settings);
	return true;
}

// Returns 0 on success
int map_ftl_error_to_obs_error(int status) {
	/* Map FTL errors to OBS errors */
	int ftl_to_obs_error_code = 0;
	#if 0
	switch (status) {
		case FTL_SUCCESS:
			break;
		case FTL_DNS_FAILURE:
			ftl_to_obs_error_code = OBS_OUTPUT_FTL_DNS_FAILURE;
			break;
		case FTL_CONNECT_ERROR:
			ftl_to_obs_error_code = OBS_OUTPUT_FTL_CONNECT_FAILURE;
			break;
		case FTL_OLD_VERSION:
			ftl_to_obs_error_code = OBS_OUTPUT_FTL_OLD_VERSION;
			break;
		case FTL_STREAM_REJECTED:
			ftl_to_obs_error_code = OBS_OUTPUT_FTL_STREAM_REJECTED;
			break;
		case FTL_UNAUTHORIZED:
			ftl_to_obs_error_code = OBS_OUTPUT_FTL_UNAUTHORIZED;
			break;
		case FTL_AUDIO_SSRC_COLLISION:
			/* SSRC collision, let's back up and try with a new audio SSRC */
			ftl_to_obs_error_code = OBS_OUTPUT_FTL_AUDIO_SSRC_COLLISION;
			break;
		case FTL_VIDEO_SSRC_COLLISION:
			ftl_to_obs_error_code = OBS_OUTPUT_FTL_VIDEO_SSRC_COLLISION;
			break;
		/* Non-specific failures, or internal Tachyon bug */
		default:
			/* Unknown FTL error */
			blog (LOG_ERROR, "tachyon error mapping needs to be updated!");
			ftl_to_obs_error_code = OBS_OUTPUT_ERROR;
	}
#endif
	return ftl_to_obs_error_code;
}

struct obs_output_info ftl_output_info = {
	.id                 = "ftl_output",
	.flags              = OBS_OUTPUT_AV |
	                      OBS_OUTPUT_ENCODED |
	                      OBS_OUTPUT_SERVICE |
	                      OBS_OUTPUT_MULTI_TRACK,
	.get_name           = ftl_stream_getname,
	.create             = ftl_stream_create,
	.destroy            = ftl_stream_destroy,
	.start              = ftl_stream_start,
	.stop               = ftl_stream_stop,
	.encoded_packet     = ftl_stream_data,
	.get_defaults       = ftl_stream_defaults,
	.get_properties     = ftl_stream_properties,
	.get_total_bytes    = ftl_stream_total_bytes_sent,
	.get_dropped_frames = ftl_stream_dropped_frames
};