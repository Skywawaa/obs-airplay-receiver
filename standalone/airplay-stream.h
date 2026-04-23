#pragma once

/*
 * airplay-stream.h
 * Standalone AirPlay receiver with WebRTC browser output via mediasoup SFU.
 * No OBS dependency — stream to any modern browser at < 100 ms latency.
 */

#include <stdbool.h>

struct airplay_stream_config {
    /* AirPlay server advertisement name (shows on Apple device) */
    char server_name[256];

    /*
     * Port of the mediasoup signalling server (e.g. 8888).
     * Start the mediasoup server first:
     *   cd mediasoup-server && npm install && node server.js
     * Then navigate to http://localhost:<webrtc_port>/ in any modern browser.
     */
    int webrtc_port;

    /* Requested video resolution (0 = device native) */
    int  width;
    int  height;

    /* Requested frame rate (0 = unlimited) */
    int  fps;

    /*
     * Enable hardware-accelerated audio decode via Windows Media Foundation
     * (aac_mf).  Falls back to software if unavailable.  Default: false.
     */
    bool hw_accel;
};

/*
 * Start the AirPlay receiver and WebRTC server.
 * Blocks briefly during initialisation then returns.
 * Returns true on success.
 */
bool airplay_stream_start(const struct airplay_stream_config *cfg);

/*
 * Stop the AirPlay receiver and close the WebRTC server.
 */
void airplay_stream_stop(void);
