#pragma once

/*
 * airplay-stream.h
 * Standalone AirPlay receiver with WebRTC browser output via mediasoup SFU.
 * No OBS dependency — stream to any modern browser at < 100 ms latency.
 */

#include <stdbool.h>

typedef enum airplay_video_mode {
    AIRPLAY_VIDEO_MODE_PASSTHROUGH = 0,
    AIRPLAY_VIDEO_MODE_TRANSCODE_AUTO = 1,
} airplay_video_mode_t;

typedef enum airplay_video_encoder_preference {
    AIRPLAY_VIDEO_ENCODER_AUTO = 0,
    AIRPLAY_VIDEO_ENCODER_NVENC,
    AIRPLAY_VIDEO_ENCODER_QSV,
    AIRPLAY_VIDEO_ENCODER_AMF,
    AIRPLAY_VIDEO_ENCODER_VIDEOTOOLBOX,
    AIRPLAY_VIDEO_ENCODER_LIBX264,
    AIRPLAY_VIDEO_ENCODER_SOFTWARE,
} airplay_video_encoder_preference_t;

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

    /* Video forwarding mode: passthrough or auto-transcode preparation. */
    airplay_video_mode_t video_mode;

    /* Preferred H264 encoder for transcode-auto mode. */
    airplay_video_encoder_preference_t video_encoder_preference;

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
