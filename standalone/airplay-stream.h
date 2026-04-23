#pragma once

/*
 * airplay-stream.h
 * Standalone AirPlay receiver with WebRTC browser output.
 * No OBS dependency — stream to any modern browser at < 100 ms latency.
 */

#include <stdbool.h>

struct airplay_stream_config {
    /* AirPlay server advertisement name (shows on Apple device) */
    char server_name[256];

    /*
     * TCP port for the WebRTC HTTP viewer server (e.g. 8888).
     * Navigate to http://localhost:<webrtc_port>/ in any modern browser
     * for < 100 ms end-to-end latency (via LiveKit SFU).
     */
    int webrtc_port;

    /*
     * LiveKit server HTTP URL (e.g. "http://localhost:7880").
     * Set to "" or leave zero to use the default ("http://localhost:7880").
     */
    char livekit_url[512];

    /*
     * LiveKit API key and secret for JWT signing.
     * Defaults: "devkey" / "secret" (matches LiveKit --dev flag).
     */
    char livekit_api_key[128];
    char livekit_api_secret[256];

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
