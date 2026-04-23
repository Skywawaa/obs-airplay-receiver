#pragma once

/*
 * webrtc-output.h
 * mediasoup SFU output for ultra-low-latency (< 100 ms) browser playback.
 *
 * The C application sends plain RTP (H.264 + Opus) over UDP to a local
 * mediasoup SFU server.  Browsers connect to the SFU using the standard
 * WebRTC API via the mediasoup-client JS SDK (served by the Node.js server).
 *
 * Architecture:
 *   airplay-stream.exe  --UDP plain RTP-->  mediasoup Node.js server
 *                                                    |
 *                                          WebRTC (DTLS/SRTP)
 *                                                    |
 *                                              Browser(s)
 *
 * Start the mediasoup server before starting airplay-stream:
 *   cd mediasoup-server && npm install && node server.js
 * Then open the player at http://localhost:<http_port>/.
 *
 * Video : H.264 Annex-B NAL units are packetised into RTP per RFC 6184.
 * Audio : float32 interleaved PCM is resampled to 48 kHz, encoded to
 *         Opus (FFmpeg), and packetised per RFC 7587.
 *
 * Requires FFmpeg (Opus + SWR) for audio encoding.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct webrtc_output;

/*
 * Create the mediasoup RTP output.  mediasoup_port is the HTTP port of the
 * mediasoup signalling server (e.g. 8888 — the same port you open in the
 * browser).  The function returns immediately; RTP will begin flowing once
 * the mediasoup server is reachable (retried in background).
 * Returns NULL on failure.
 */
struct webrtc_output *webrtc_output_create(int mediasoup_port);

/*
 * Shut down and free all resources.
 */
void webrtc_output_destroy(struct webrtc_output *out);

/*
 * Write one H.264 Annex-B video frame.
 *   data   : raw Annex-B bytes (start codes 00 00 00 01 or 00 00 01)
 *   size   : byte count
 *   pts_us : presentation timestamp in microseconds
 *
 * Silently dropped until the mediasoup connection is ready.
 */
void webrtc_output_write_video(struct webrtc_output *out,
                               const uint8_t *data, size_t size,
                               int64_t pts_us);

/*
 * Write decoded PCM audio (float32 interleaved, e.g. L R L R …).
 *   pcm         : sample buffer
 *   samples     : number of samples PER CHANNEL
 *   channels    : channel count (typically 2)
 *   sample_rate : input sample rate (typically 44100 from AirPlay)
 *   pts_us      : presentation timestamp in microseconds (unused internally)
 *
 * Audio is resampled to 48 kHz and encoded to Opus in 20 ms frames.
 */
void webrtc_output_write_audio(struct webrtc_output *out,
                               const float *pcm, int samples,
                               int channels, int sample_rate,
                               int64_t pts_us);

/*
 * Request that a cached IDR keyframe be injected before the next video
 * frame sent to mediasoup.  Call this whenever the AirPlay session restarts
 * so existing browser consumers see an immediate picture update.
 *
 * Safe to call at any time; has no effect if no keyframe has been cached.
 */
void webrtc_output_request_keyframe(struct webrtc_output *out);
