#pragma once

#include "airplay-server.h"
#include <stdbool.h>
#include <stdint.h>

struct airplay_mirror;

struct airplay_mirror_config {
	uint16_t port;
	airplay_video_callback on_video_frame;
	airplay_disconnect_callback on_disconnect;
	void *userdata;
};

/* Create the mirror receiver */
struct airplay_mirror *
airplay_mirror_create(const struct airplay_mirror_config *cfg);

/* Start accepting mirror connections */
bool airplay_mirror_start(struct airplay_mirror *mirror);

/* Stop the mirror receiver */
void airplay_mirror_stop(struct airplay_mirror *mirror);

/* Destroy */
void airplay_mirror_destroy(struct airplay_mirror *mirror);

/* Get the actual port */
uint16_t airplay_mirror_get_port(struct airplay_mirror *mirror);
