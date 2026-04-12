/*
 * OBS AirPlay Source - Uses UxPlay's raop library for the full
 * AirPlay 2 mirroring protocol (FairPlay, pairing, encryption)
 * and FFmpeg for video/audio decoding.
 */

#include "airplay-source.h"
#include "video-decoder.h"
#include "audio-decoder.h"

#include <obs-module.h>
#include <util/platform.h>

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* UxPlay's raop API */
#include "raop.h"
#include "dnssd.h"
#include "stream.h"
#include "logger.h"

#ifdef _WIN32
#include <winsock2.h>
#include <iphlpapi.h>
#include <process.h>
#define getpid _getpid
#endif

struct airplay_source {
	obs_source_t *source;

	/* UxPlay components */
	raop_t *raop;
	dnssd_t *dnssd;

	/* Decoders */
	struct video_decoder *vdec;
	struct audio_decoder *adec;

	/* OBS output frames */
	struct obs_source_frame vframe;
	struct obs_source_audio aframe;

	/* Settings */
	char server_name[256];
	bool use_random_mac;
	int cfg_width;
	int cfg_height;
	int cfg_fps;
	int cfg_max_fps;

	/* State */
	int width;
	int height;
	int open_connections;
};

/* ---------- MAC address helpers ---------- */

static void generate_random_mac(char *mac, size_t len)
{
	srand((unsigned)(time(NULL) * getpid()));
	int octet = (rand() % 64) << 2 | 0x02; /* locally administered */
	snprintf(mac, len, "%02x:%02x:%02x:%02x:%02x:%02x",
		 octet, rand() % 256, rand() % 256,
		 rand() % 256, rand() % 256, rand() % 256);
}

#ifdef _WIN32
static bool get_system_mac(char *mac, size_t len)
{
	ULONG buf_len = 15000;
	PIP_ADAPTER_ADDRESSES addrs = (PIP_ADAPTER_ADDRESSES)malloc(buf_len);
	if (!addrs) return false;

	if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, NULL,
				 addrs, &buf_len) != NO_ERROR) {
		free(addrs);
		return false;
	}

	bool found = false;
	for (PIP_ADAPTER_ADDRESSES p = addrs; p; p = p->Next) {
		if ((p->IfType == IF_TYPE_ETHERNET_CSMACD ||
		     p->IfType == IF_TYPE_IEEE80211) &&
		    p->PhysicalAddressLength == 6) {
			snprintf(mac, len, "%02x:%02x:%02x:%02x:%02x:%02x",
				 p->PhysicalAddress[0], p->PhysicalAddress[1],
				 p->PhysicalAddress[2], p->PhysicalAddress[3],
				 p->PhysicalAddress[4], p->PhysicalAddress[5]);
			found = true;
			break;
		}
	}
	free(addrs);
	return found;
}
#endif

static void parse_hw_addr(const char *str, char *hw, int *hw_len)
{
	*hw_len = 0;
	for (size_t i = 0; i < strlen(str) && *hw_len < 6; i += 3) {
		hw[(*hw_len)++] = (char)strtol(str + i, NULL, 16);
	}
}

/* ---------- UxPlay callbacks ---------- */

static void cb_conn_init(void *cls)
{
	struct airplay_source *ctx = cls;
	ctx->open_connections++;
	blog(LOG_INFO, "[AirPlay] Connection init (open: %d)",
	     ctx->open_connections);
}

static void cb_conn_destroy(void *cls)
{
	struct airplay_source *ctx = cls;
	ctx->open_connections--;
	blog(LOG_INFO, "[AirPlay] Connection destroy (open: %d)",
	     ctx->open_connections);
	if (ctx->open_connections <= 0)
		obs_source_output_video(ctx->source, NULL);
}

static void cb_conn_reset(void *cls, int timeouts, bool reset_video)
{
	struct airplay_source *ctx = cls;
	blog(LOG_WARNING,
	     "[AirPlay] Connection reset (timeouts=%d reset_video=%d)",
	     timeouts, reset_video);
	raop_stop(ctx->raop);
}

static void cb_conn_teardown(void *cls, bool *t96, bool *t110)
{
	(void)cls;
	(void)t96;
	(void)t110;
	blog(LOG_INFO, "[AirPlay] Connection teardown");
}

static int video_frame_count = 0;

static void cb_video_process(void *cls, raop_ntp_t *ntp,
			     h264_decode_struct *data)
{
	(void)ntp;
	struct airplay_source *ctx = cls;
	if (!ctx->vdec)
		return;

	if (video_frame_count < 5) {
		blog(LOG_INFO, "[AirPlay] video_process: %d bytes, %d NALs, pts=%llu",
		     data->data_len, data->nal_count, (unsigned long long)data->pts);
		if (data->data_len >= 16) {
			blog(LOG_INFO, "[AirPlay] H264 first 16 bytes: "
			     "%02x %02x %02x %02x %02x %02x %02x %02x "
			     "%02x %02x %02x %02x %02x %02x %02x %02x",
			     data->data[0], data->data[1], data->data[2], data->data[3],
			     data->data[4], data->data[5], data->data[6], data->data[7],
			     data->data[8], data->data[9], data->data[10], data->data[11],
			     data->data[12], data->data[13], data->data[14], data->data[15]);
		}
	}

	struct decoded_frame frame = {0};
	if (!video_decoder_decode(ctx->vdec, data->data, data->data_len,
				  data->pts, &frame)) {
		if (video_frame_count < 5)
			blog(LOG_WARNING, "[AirPlay] video decode FAILED");
		return;
	}
	video_frame_count++;

	struct obs_source_frame obs_frame = {0};
	obs_frame.format = VIDEO_FORMAT_RGBA;
	obs_frame.width = frame.width;
	obs_frame.height = frame.height;
	obs_frame.timestamp = data->pts * 1000;

	obs_frame.data[0] = frame.data[0];
	obs_frame.linesize[0] = frame.linesize[0];

	obs_source_output_video(ctx->source, &obs_frame);

	ctx->width = frame.width;
	ctx->height = frame.height;
}

static void cb_audio_process(void *cls, raop_ntp_t *ntp,
			     audio_decode_struct *data)
{
	(void)ntp;
	struct airplay_source *ctx = cls;
	if (!ctx->adec)
		return;

	struct decoded_audio audio = {0};
	if (!audio_decoder_decode(ctx->adec, data->data, data->data_len,
				  data->ntp_time, &audio))
		return;

	struct obs_source_audio obs_audio = {0};
	obs_audio.data[0] = audio.data;
	obs_audio.frames = audio.samples;
	obs_audio.speakers = (audio.channels == 2) ? SPEAKERS_STEREO
						    : SPEAKERS_MONO;
	obs_audio.format = AUDIO_FORMAT_FLOAT;
	obs_audio.samples_per_sec = audio.sample_rate;
	obs_audio.timestamp = data->ntp_time * 1000;

	obs_source_output_audio(ctx->source, &obs_audio);
}

static void cb_audio_flush(void *cls) { (void)cls; }
static void cb_video_flush(void *cls) { (void)cls; }
static void cb_audio_set_volume(void *cls, float v) { (void)cls; (void)v; }
static void cb_audio_set_metadata(void *cls, const void *b, int l)
{
	(void)cls; (void)b; (void)l;
}

static void cb_audio_get_format(void *cls, unsigned char *ct,
				unsigned short *spf, bool *usingScreen,
				bool *isMedia, uint64_t *audioFormat)
{
	(void)cls;
	*ct = 1; /* AAC-ELD */
	(void)spf;
	(void)usingScreen;
	(void)isMedia;
	(void)audioFormat;
}

static void cb_video_report_size(void *cls, float *ws, float *hs,
				 float *w, float *h)
{
	struct airplay_source *ctx = cls;
	ctx->width = (int)*ws;
	ctx->height = (int)*hs;
	blog(LOG_INFO, "[AirPlay] Video size: %.0fx%.0f (source) %.0fx%.0f",
	     *ws, *hs, *w, *h);
}

static void cb_log(void *cls, int level, const char *msg)
{
	(void)cls;
	int obs_level = LOG_DEBUG;
	if (level <= RAOP_LOG_ERR) obs_level = LOG_ERROR;
	else if (level <= RAOP_LOG_WARNING) obs_level = LOG_WARNING;
	else if (level <= RAOP_LOG_INFO) obs_level = LOG_INFO;
	blog(obs_level, "[AirPlay-UxPlay] %s", msg);
}

/* ---------- Server lifecycle ---------- */

static bool start_server(struct airplay_source *ctx)
{
	raop_callbacks_t cbs;
	memset(&cbs, 0, sizeof(cbs));
	cbs.cls = ctx;
	cbs.conn_init = cb_conn_init;
	cbs.conn_destroy = cb_conn_destroy;
	cbs.conn_reset = cb_conn_reset;
	cbs.conn_teardown = cb_conn_teardown;
	cbs.audio_process = cb_audio_process;
	cbs.video_process = cb_video_process;
	cbs.audio_flush = cb_audio_flush;
	cbs.video_flush = cb_video_flush;
	cbs.audio_set_volume = cb_audio_set_volume;
	cbs.audio_get_format = cb_audio_get_format;
	cbs.video_report_size = cb_video_report_size;
	cbs.audio_set_metadata = cb_audio_set_metadata;

	ctx->raop = raop_init(2, &cbs);
	if (!ctx->raop) {
		blog(LOG_ERROR, "[AirPlay] raop_init failed");
		return false;
	}

	raop_set_plist(ctx->raop, "max_ntp_timeouts", 5);

	if (ctx->cfg_width > 0)
		raop_set_plist(ctx->raop, "width", ctx->cfg_width);
	if (ctx->cfg_height > 0)
		raop_set_plist(ctx->raop, "height", ctx->cfg_height);
	if (ctx->cfg_fps > 0)
		raop_set_plist(ctx->raop, "refreshRate", ctx->cfg_fps);
	if (ctx->cfg_max_fps > 0)
		raop_set_plist(ctx->raop, "maxFPS", ctx->cfg_max_fps);

	unsigned short tcp[3] = {0, 0, 0};
	unsigned short udp[3] = {0, 0, 0};
	raop_set_tcp_ports(ctx->raop, tcp);
	raop_set_udp_ports(ctx->raop, udp);
	raop_set_log_callback(ctx->raop, cb_log, NULL);
	raop_set_log_level(ctx->raop, RAOP_LOG_INFO);

	unsigned short port = raop_get_port(ctx->raop);
	blog(LOG_INFO, "[AirPlay] calling raop_start...");
	int start_ret = raop_start(ctx->raop, &port);
	blog(LOG_INFO, "[AirPlay] raop_start returned %d", start_ret);
	if (start_ret < 0) {
		blog(LOG_ERROR, "[AirPlay] raop_start failed: %d", start_ret);
		raop_destroy(ctx->raop);
		ctx->raop = NULL;
		return false;
	}
	raop_set_port(ctx->raop, port);
	blog(LOG_INFO, "[AirPlay] raop_start OK on port %d", port);

	/* Give the httpd thread a moment to stabilize */
	Sleep(100);

	/* MAC address - always use random to avoid crash in GetAdaptersAddresses */
	char mac_str[18] = {0};
	generate_random_mac(mac_str, sizeof(mac_str));
	blog(LOG_INFO, "[AirPlay] using MAC: %s", mac_str);

	char hw[6];
	int hw_len = 0;
	parse_hw_addr(mac_str, hw, &hw_len);

	blog(LOG_INFO, "[AirPlay] calling dnssd_init...");
	int err = 0;
	ctx->dnssd = dnssd_init(ctx->server_name,
				(int)strlen(ctx->server_name),
				hw, hw_len, &err);
	if (err || !ctx->dnssd) {
		blog(LOG_ERROR, "[AirPlay] dnssd_init failed (err=%d)", err);
		raop_destroy(ctx->raop);
		ctx->raop = NULL;
		return false;
	}
	blog(LOG_INFO, "[AirPlay] dnssd_init OK");

	raop_set_dnssd(ctx->raop, ctx->dnssd);

	blog(LOG_INFO, "[AirPlay] calling dnssd_register_raop port=%d...", port);
	dnssd_register_raop(ctx->dnssd, port);
	blog(LOG_INFO, "[AirPlay] dnssd_register_raop OK");

	unsigned short airplay_port = (port != 65535) ? port + 1 : port - 1;
	blog(LOG_INFO, "[AirPlay] calling dnssd_register_airplay port=%d...", airplay_port);
	dnssd_register_airplay(ctx->dnssd, airplay_port);
	blog(LOG_INFO, "[AirPlay] dnssd_register_airplay OK");

	blog(LOG_INFO, "[AirPlay] Server started: '%s' port=%d mac=%s",
	     ctx->server_name, port, mac_str);

	return true;
}

static void stop_server(struct airplay_source *ctx)
{
	if (ctx->raop) {
		raop_destroy(ctx->raop);
		ctx->raop = NULL;
	}
	if (ctx->dnssd) {
		dnssd_unregister_raop(ctx->dnssd);
		dnssd_unregister_airplay(ctx->dnssd);
		dnssd_destroy(ctx->dnssd);
		ctx->dnssd = NULL;
	}
}

/* ---------- Helpers ---------- */

static void parse_resolution(const char *res_str, int *w, int *h)
{
	*w = 0;
	*h = 0;
	if (!res_str || strcmp(res_str, "0x0") == 0)
		return;
	sscanf(res_str, "%dx%d", w, h);
}

/* ---------- OBS Source API ---------- */

static const char *airplay_get_name(void *unused)
{
	(void)unused;
	return "AirPlay Receiver";
}

static void *airplay_create(obs_data_t *settings, obs_source_t *source)
{
#ifdef _WIN32
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

	struct airplay_source *ctx = bzalloc(sizeof(struct airplay_source));
	ctx->source = source;
	ctx->width = 1920;
	ctx->height = 1080;

	ctx->vdec = video_decoder_create();
	ctx->adec = audio_decoder_create();

	const char *name = obs_data_get_string(settings, "server_name");
	strncpy(ctx->server_name,
		(name && *name) ? name : "OBS AirPlay",
		sizeof(ctx->server_name) - 1);
	ctx->use_random_mac = obs_data_get_bool(settings, "use_random_mac");

	const char *res = obs_data_get_string(settings, "resolution");
	parse_resolution(res, &ctx->cfg_width, &ctx->cfg_height);

	int fps_val = (int)obs_data_get_int(settings, "fps_preset");
	ctx->cfg_fps = fps_val;
	ctx->cfg_max_fps = fps_val;

	if (!start_server(ctx)) {
		blog(LOG_ERROR, "[AirPlay] Failed to start server");
	}

	return ctx;
}

static void airplay_destroy(void *data)
{
	struct airplay_source *ctx = data;
	if (!ctx) return;

	stop_server(ctx);

	if (ctx->vdec) video_decoder_destroy(ctx->vdec);
	if (ctx->adec) audio_decoder_destroy(ctx->adec);

	bfree(ctx);
}

static void airplay_update(void *data, obs_data_t *settings)
{
	struct airplay_source *ctx = data;
	const char *name = obs_data_get_string(settings, "server_name");
	bool random_mac = obs_data_get_bool(settings, "use_random_mac");
	const char *res = obs_data_get_string(settings, "resolution");
	int fps = (int)obs_data_get_int(settings, "fps_preset");

	int w = 0, h = 0;
	parse_resolution(res, &w, &h);

	bool need_restart = false;

	if (name && strcmp(ctx->server_name, name) != 0) {
		strncpy(ctx->server_name, name,
			sizeof(ctx->server_name) - 1);
		need_restart = true;
	}
	if (ctx->use_random_mac != random_mac) {
		ctx->use_random_mac = random_mac;
		need_restart = true;
	}
	if (ctx->cfg_width != w || ctx->cfg_height != h) {
		ctx->cfg_width = w;
		ctx->cfg_height = h;
		need_restart = true;
	}
	if (ctx->cfg_fps != fps) {
		ctx->cfg_fps = fps;
		ctx->cfg_max_fps = fps;
		need_restart = true;
	}

	if (need_restart) {
		stop_server(ctx);
		start_server(ctx);
	}
}

static obs_properties_t *airplay_get_properties(void *data)
{
	(void)data;
	obs_properties_t *p = obs_properties_create();

	obs_properties_add_text(p, "server_name", "Server Name",
				OBS_TEXT_DEFAULT);

	obs_property_t *res = obs_properties_add_list(p, "resolution",
		"Resolution", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(res, "Source (device native)", "0x0");
	obs_property_list_add_string(res, "1920x1080 (1080p)", "1920x1080");
	obs_property_list_add_string(res, "1280x720 (720p)", "1280x720");
	obs_property_list_add_string(res, "3840x2160 (4K)", "3840x2160");
	obs_property_list_add_string(res, "2560x1440 (1440p)", "2560x1440");

	obs_property_t *fps = obs_properties_add_list(p, "fps_preset",
		"FPS", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(fps, "60 FPS", 60);
	obs_property_list_add_int(fps, "30 FPS", 30);
	obs_property_list_add_int(fps, "120 FPS", 120);
	obs_property_list_add_int(fps, "24 FPS", 24);

	obs_properties_add_bool(p, "use_random_mac",
				"Use Random MAC Address");

	return p;
}

static void airplay_get_defaults(obs_data_t *s)
{
	obs_data_set_default_string(s, "server_name", "OBS AirPlay");
	obs_data_set_default_string(s, "resolution", "0x0");
	obs_data_set_default_int(s, "fps_preset", 60);
	obs_data_set_default_bool(s, "use_random_mac", true);
}

static uint32_t airplay_get_width(void *data)
{
	return ((struct airplay_source *)data)->width;
}

static uint32_t airplay_get_height(void *data)
{
	return ((struct airplay_source *)data)->height;
}

/* ---------- Register ---------- */
void airplay_source_register(void)
{
	struct obs_source_info info = {0};
	info.id = "airplay_receiver_source";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO |
			    OBS_SOURCE_DO_NOT_DUPLICATE;
	info.get_name = airplay_get_name;
	info.create = airplay_create;
	info.destroy = airplay_destroy;
	info.update = airplay_update;
	info.get_properties = airplay_get_properties;
	info.get_defaults = airplay_get_defaults;
	info.get_width = airplay_get_width;
	info.get_height = airplay_get_height;
	info.icon_type = OBS_ICON_TYPE_DESKTOP_CAPTURE;

	obs_register_source(&info);
}
