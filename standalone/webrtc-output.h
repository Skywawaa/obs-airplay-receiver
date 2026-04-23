#pragma once

/*
 * webrtc-output.h
 * LiveKit WHIP publisher for ultra-low-latency (< 100 ms) browser playback.
 *
 * The C application publishes H.264 + Opus to a LiveKit SFU once via WHIP.
 * Browsers connect to the SFU using the LiveKit JS SDK (served from the
 * built-in HTTP server on http_port).  The SDK handles all reconnections
 * automatically -- no page reload needed.
 *
 * GET http://localhost:<http_port>/        -- LiveKit JS SDK player page
 * GET http://localhost:<http_port>/token   -- subscriber JWT (JSON)
 *
 * Video : H.264 Annex-B NAL units are packetised into RTP per RFC 6184.
 * Audio : float32 interleaved PCM is resampled to 48 kHz, encoded to
 *         Opus (libopus / FFmpeg native), and packetised per RFC 7587.
 *
 * Requires libdatachannel >= 0.17, FFmpeg (Opus + SWR), OpenSSL >= 1.1,
 * and a running LiveKit server (https://livekit.io).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct webrtc_output;

/*
 * Create a WebRTC/LiveKit output that:
 *   - Publishes a sendonly WHIP stream to livekit_url (e.g.
 *     "http://localhost:7880") using api_key / api_secret for JWT auth.
 *   - Serves the LiveKit JS SDK viewer at http://localhost:<http_port>/.
 *   - Auto-reconnects to LiveKit on any failure; browser viewers are
 *     never interrupted.
 *
 * Pass NULL for livekit_url / api_key / api_secret to use the LiveKit
 * --dev defaults ("http://localhost:7880" / "devkey" / "secret").
 *
 * Returns NULL on failure.
 */
struct webrtc_output *webrtc_output_create(int         http_port,
                                           const char *livekit_url,
                                           const char *api_key,
                                           const char *api_secret);

/*
 * Shut down the server and free all resources.
 */
void webrtc_output_destroy(struct webrtc_output *out);

/*
 * Write one H.264 Annex-B video frame.
 *   data   : raw Annex-B bytes (start codes 00 00 00 01 or 00 00 01)
 *   size   : byte count
 *   pts_us : presentation timestamp in microseconds
 *
 * Silently dropped when no connection to the LiveKit SFU is active.
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
 * frame delivered to the browser.  Call this whenever the AirPlay session
 * restarts (device reconnects) so the browser H.264 decoder can sync to
 * the new stream even when the first incoming frame is not an IDR.
 *
 * Safe to call at any time; has no effect if no peer is connected or if
 * no keyframe has been cached yet.
 */
void webrtc_output_request_keyframe(struct webrtc_output *out);
