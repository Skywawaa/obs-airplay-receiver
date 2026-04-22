#pragma once

/*
 * ts-output.h
 * MPEG-TS over TCP server.
 *
 * Opens a listening TCP socket and accepts a single media player
 * connection at a time (e.g. VLC: tcp://localhost:<port>).
 *
 * Video: H.264 Annex-B NAL units are written directly into an
 *        MPEG-TS container without decode/re-encode.
 * Audio: PCM float32 interleaved samples are encoded to AAC-LC
 *        before muxing (necessary because AAC-ELD cannot be
 *        represented in standard MPEG-TS without ADTS headers).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ts_output;

/*
 * Create a MPEG-TS TCP server that listens on the given port.
 * If hw_accel is true, the AAC-LC encoder will attempt to use a
 * hardware-accelerated codec (aac_mf on Windows) before falling
 * back to the software encoder.
 * Call ts_output_destroy() to shut it down.
 * Returns NULL on failure.
 */
struct ts_output *ts_output_create(int port, bool hw_accel);

/*
 * Shut down the server and free all resources.
 */
void ts_output_destroy(struct ts_output *out);

/*
 * Write one H.264 Annex-B video frame.
 *   data : raw Annex-B bytes (start codes 00 00 00 01 or 00 00 01)
 *   size : byte count
 *   pts  : presentation timestamp in microseconds
 *
 * Silently drops the packet when no player is connected.
 * Transparently handles client reconnects between calls.
 */
void ts_output_write_video(struct ts_output *out,
                           const uint8_t *data, size_t size,
                           int64_t pts);

/*
 * Write decoded PCM audio (float32 interleaved, e.g. L R L R …).
 *   pcm         : sample buffer
 *   samples     : number of samples PER CHANNEL
 *   channels    : channel count (typically 2)
 *   sample_rate : e.g. 44100
 *   pts         : presentation timestamp in microseconds
 *
 * Internally accumulates samples into 1024-sample AAC frames.
 */
void ts_output_write_audio(struct ts_output *out,
                           const float *pcm, int samples,
                           int channels, int sample_rate,
                           int64_t pts);
