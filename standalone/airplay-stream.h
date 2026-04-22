#pragma once

/*
 * airplay-stream.h
 * Standalone AirPlay receiver that streams to an MPEG-TS TCP server.
 * No OBS dependency — output goes to VLC / any media player.
 */

#include <stdbool.h>

struct airplay_stream_config {
    /* AirPlay server advertisement name (shows on Apple device) */
    char server_name[256];

    /* TCP port on which the MPEG-TS stream will be served */
    int  stream_port;

    /* Requested video resolution (0 = device native) */
    int  width;
    int  height;

    /* Requested frame rate (0 = unlimited) */
    int  fps;

    /*
     * Enable hardware-accelerated audio decode/encode via Windows
     * Media Foundation (aac_mf).  Falls back to software if the
     * hardware codec is unavailable.  Default: false.
     */
    bool hw_accel;
};

/*
 * Start the AirPlay receiver and MPEG-TS output server.
 * Blocks briefly during initialisation then returns.
 * Returns true on success.
 */
bool airplay_stream_start(const struct airplay_stream_config *cfg);

/*
 * Stop the AirPlay receiver and close the MPEG-TS server.
 */
void airplay_stream_stop(void);
