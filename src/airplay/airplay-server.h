#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Callback types */
typedef void (*airplay_video_callback)(void *userdata,
				       const uint8_t *h264_data,
				       size_t h264_size, uint64_t pts);

typedef void (*airplay_disconnect_callback)(void *userdata);

struct airplay_server_config {
	char name[256];
	uint16_t port;
	airplay_video_callback on_video_frame;
	airplay_disconnect_callback on_disconnect;
	void *userdata;
};

struct airplay_server;

/* Create the AirPlay server */
struct airplay_server *
airplay_server_create(const struct airplay_server_config *cfg);

/* Start listening and advertising */
bool airplay_server_start(struct airplay_server *srv);

/* Stop the server */
void airplay_server_stop(struct airplay_server *srv);

/* Destroy the server */
void airplay_server_destroy(struct airplay_server *srv);

/* Check if a client is currently connected */
bool airplay_server_is_connected(struct airplay_server *srv);
