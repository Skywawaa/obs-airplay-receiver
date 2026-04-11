#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

struct video_decoder;

struct decoded_frame {
	uint8_t *data[4];    /* Plane pointers (Y, UV for NV12) */
	int linesize[4];     /* Line sizes per plane */
	int width;
	int height;
	int64_t pts;
};

/* Create a new H.264 decoder */
struct video_decoder *video_decoder_create(void);

/* Destroy the decoder */
void video_decoder_destroy(struct video_decoder *dec);

/* Decode H.264 Annex-B data into a raw frame.
 * Returns true if a frame was produced.
 * The frame data is valid until the next decode call. */
bool video_decoder_decode(struct video_decoder *dec, const uint8_t *h264_data,
			  size_t h264_size, uint64_t pts,
			  struct decoded_frame *out_frame);

/* Flush any pending frames */
bool video_decoder_flush(struct video_decoder *dec,
			 struct decoded_frame *out_frame);
