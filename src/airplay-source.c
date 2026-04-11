/*
 * OBS AirPlay Source - creates the OBS source that displays AirPlay content
 */

#include "airplay-source.h"
#include "airplay/airplay-server.h"
#include "video-decoder.h"

#include <obs-module.h>
#include <util/platform.h>

#include <stdlib.h>
#include <string.h>

/* ---------- Forward declarations ---------- */
static const char *airplay_get_name(void *unused);
static void *airplay_create(obs_data_t *settings, obs_source_t *source);
static void airplay_destroy(void *data);
static void airplay_update(void *data, obs_data_t *settings);
static obs_properties_t *airplay_get_properties(void *data);
static void airplay_get_defaults(obs_data_t *settings);
static uint32_t airplay_get_width(void *data);
static uint32_t airplay_get_height(void *data);
static void airplay_video_tick(void *data, float seconds);

/* ---------- Callback from AirPlay server when a frame arrives ---------- */
static void on_video_frame(void *userdata, const uint8_t *h264_data,
			   size_t h264_size, uint64_t pts)
{
	struct airplay_source *ctx = userdata;
	if (!ctx || !ctx->running)
		return;

	/* Decode H.264 to raw frame */
	struct decoded_frame frame = {0};
	if (!video_decoder_decode(ctx->decoder, h264_data, h264_size, pts,
				  &frame))
		return;

	/* Push frame to OBS */
	pthread_mutex_lock(&ctx->frame_mutex);

	struct obs_source_frame obs_frame = {0};
	obs_frame.format = VIDEO_FORMAT_NV12;
	obs_frame.width = frame.width;
	obs_frame.height = frame.height;
	obs_frame.timestamp = pts;

	obs_frame.data[0] = frame.data[0];
	obs_frame.data[1] = frame.data[1];
	obs_frame.linesize[0] = frame.linesize[0];
	obs_frame.linesize[1] = frame.linesize[1];

	obs_source_output_video(ctx->source, &obs_frame);

	ctx->width = frame.width;
	ctx->height = frame.height;
	ctx->connected = true;
	ctx->frame_ready = true;

	pthread_mutex_unlock(&ctx->frame_mutex);
}

static void on_disconnect(void *userdata)
{
	struct airplay_source *ctx = userdata;
	if (!ctx)
		return;

	ctx->connected = false;
	ctx->frame_ready = false;

	/* Clear the displayed frame */
	obs_source_output_video(ctx->source, NULL);

	blog(LOG_INFO, "[AirPlay] Client disconnected");
}

/* ---------- OBS Source API ---------- */

static const char *airplay_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "AirPlay Receiver";
}

static void *airplay_create(obs_data_t *settings, obs_source_t *source)
{
	struct airplay_source *ctx = bzalloc(sizeof(struct airplay_source));
	ctx->source = source;
	pthread_mutex_init(&ctx->frame_mutex, NULL);

	/* Init decoder */
	ctx->decoder = video_decoder_create();
	if (!ctx->decoder) {
		blog(LOG_ERROR, "[AirPlay] Failed to create video decoder");
		bfree(ctx);
		return NULL;
	}

	airplay_update(ctx, settings);
	return ctx;
}

static void airplay_stop_server(struct airplay_source *ctx)
{
	if (ctx->server) {
		ctx->running = false;
		airplay_server_stop(ctx->server);
		airplay_server_destroy(ctx->server);
		ctx->server = NULL;
	}
}

static void airplay_start_server(struct airplay_source *ctx)
{
	airplay_stop_server(ctx);

	struct airplay_server_config cfg = {0};
	strncpy(cfg.name, ctx->server_name, sizeof(cfg.name) - 1);
	cfg.port = ctx->airplay_port;
	cfg.on_video_frame = on_video_frame;
	cfg.on_disconnect = on_disconnect;
	cfg.userdata = ctx;

	ctx->server = airplay_server_create(&cfg);
	if (!ctx->server) {
		blog(LOG_ERROR, "[AirPlay] Failed to create server");
		return;
	}

	ctx->running = true;
	if (!airplay_server_start(ctx->server)) {
		blog(LOG_ERROR, "[AirPlay] Failed to start server");
		ctx->running = false;
		airplay_server_destroy(ctx->server);
		ctx->server = NULL;
		return;
	}

	blog(LOG_INFO, "[AirPlay] Server started: '%s' on port %d",
	     ctx->server_name, ctx->airplay_port);
}

static void airplay_destroy(void *data)
{
	struct airplay_source *ctx = data;
	if (!ctx)
		return;

	airplay_stop_server(ctx);

	if (ctx->decoder)
		video_decoder_destroy(ctx->decoder);

	pthread_mutex_destroy(&ctx->frame_mutex);
	bfree(ctx);
}

static void airplay_update(void *data, obs_data_t *settings)
{
	struct airplay_source *ctx = data;

	const char *name = obs_data_get_string(settings, "server_name");
	uint16_t port = (uint16_t)obs_data_get_int(settings, "port");

	bool need_restart = false;

	if (strcmp(ctx->server_name, name) != 0) {
		strncpy(ctx->server_name, name, sizeof(ctx->server_name) - 1);
		need_restart = true;
	}

	if (ctx->airplay_port != port) {
		ctx->airplay_port = port;
		need_restart = true;
	}

	if (need_restart || !ctx->server)
		airplay_start_server(ctx);
}

static obs_properties_t *airplay_get_properties(void *data)
{
	UNUSED_PARAMETER(data);

	obs_properties_t *props = obs_properties_create();

	obs_properties_add_text(props, "server_name", "AirPlay Server Name",
				OBS_TEXT_DEFAULT);

	obs_properties_add_int(props, "port", "Port", 1024, 65535, 1);

	obs_properties_add_bool(props, "hw_decode",
				"Hardware Decoding (if available)");

	return props;
}

static void airplay_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "server_name",
				    "OBS AirPlay Receiver");
	obs_data_set_default_int(settings, "port", 7000);
	obs_data_set_default_bool(settings, "hw_decode", true);
}

static uint32_t airplay_get_width(void *data)
{
	struct airplay_source *ctx = data;
	return ctx->width > 0 ? ctx->width : 1920;
}

static uint32_t airplay_get_height(void *data)
{
	struct airplay_source *ctx = data;
	return ctx->height > 0 ? ctx->height : 1080;
}

static void airplay_video_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct airplay_source *ctx = data;
	UNUSED_PARAMETER(ctx);
}

/* ---------- Register ---------- */
void airplay_source_register(void)
{
	struct obs_source_info info = {0};

	info.id = "airplay_receiver_source";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE;
	info.get_name = airplay_get_name;
	info.create = airplay_create;
	info.destroy = airplay_destroy;
	info.update = airplay_update;
	info.get_properties = airplay_get_properties;
	info.get_defaults = airplay_get_defaults;
	info.get_width = airplay_get_width;
	info.get_height = airplay_get_height;
	info.video_tick = airplay_video_tick;
	info.icon_type = OBS_ICON_TYPE_DESKTOP_CAPTURE;

	obs_register_source(&info);
}
