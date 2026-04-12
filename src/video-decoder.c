/*
 * H.264 Video Decoder using FFmpeg (libavcodec)
 *
 * Decodes H.264 Annex-B NAL units into NV12 frames
 * that can be directly fed to OBS.
 */

#include "video-decoder.h"

#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct video_decoder {
	const AVCodec *codec;
	AVCodecContext *ctx;
	AVFrame *frame;
	AVPacket *pkt;
	struct SwsContext *sws;

	/* NV12 output buffer */
	uint8_t *nv12_data[4];
	int nv12_linesize[4];
	int nv12_width;
	int nv12_height;

	bool initialized;
	int log_count;
};

struct video_decoder *video_decoder_create(void)
{
	struct video_decoder *dec = calloc(1, sizeof(struct video_decoder));
	if (!dec)
		return NULL;

	/* Use software H.264 decoder for maximum compatibility */
	dec->codec = avcodec_find_decoder(AV_CODEC_ID_H264);

	if (!dec->codec) {
		fprintf(stderr, "[AirPlay] No H.264 decoder found!\n");
		free(dec);
		return NULL;
	}

	fprintf(stderr, "[AirPlay] Using decoder: %s\n", dec->codec->name);

	dec->ctx = avcodec_alloc_context3(dec->codec);
	if (!dec->ctx) {
		free(dec);
		return NULL;
	}

	/* Single-threaded for lowest latency */
	dec->ctx->thread_count = 1;

	if (avcodec_open2(dec->ctx, dec->codec, NULL) < 0) {
		fprintf(stderr, "[AirPlay] Failed to open H264 decoder\n");
		avcodec_free_context(&dec->ctx);
		free(dec);
		return NULL;
	}

	dec->frame = av_frame_alloc();
	dec->pkt = av_packet_alloc();

	if (!dec->frame || !dec->pkt) {
		if (dec->frame)
			av_frame_free(&dec->frame);
		if (dec->pkt)
			av_packet_free(&dec->pkt);
		avcodec_free_context(&dec->ctx);
		free(dec);
		return NULL;
	}

	dec->initialized = true;
	return dec;
}

void video_decoder_destroy(struct video_decoder *dec)
{
	if (!dec)
		return;

	if (dec->sws)
		sws_freeContext(dec->sws);

	if (dec->nv12_data[0])
		av_freep(&dec->nv12_data[0]);

	if (dec->frame)
		av_frame_free(&dec->frame);
	if (dec->pkt)
		av_packet_free(&dec->pkt);
	if (dec->ctx)
		avcodec_free_context(&dec->ctx);

	free(dec);
}

static bool ensure_nv12_buffer(struct video_decoder *dec, int width, int height)
{
	if (dec->nv12_width == width && dec->nv12_height == height &&
	    dec->nv12_data[0])
		return true;

	/* Free old buffer */
	if (dec->nv12_data[0])
		av_freep(&dec->nv12_data[0]);

	/* Allocate NV12 buffer */
	int ret = av_image_alloc(dec->nv12_data, dec->nv12_linesize, width,
				 height, AV_PIX_FMT_NV12, 32);
	if (ret < 0)
		return false;

	dec->nv12_width = width;
	dec->nv12_height = height;

	/* Update sws context */
	if (dec->sws) {
		sws_freeContext(dec->sws);
		dec->sws = NULL;
	}

	return true;
}

static bool convert_to_nv12(struct video_decoder *dec, AVFrame *frame)
{
	if (!ensure_nv12_buffer(dec, frame->width, frame->height))
		return false;

	/* Create/update sws context if needed */
	if (!dec->sws) {
		dec->sws = sws_getContext(
			frame->width, frame->height,
			(enum AVPixelFormat)frame->format, frame->width,
			frame->height, AV_PIX_FMT_NV12, SWS_FAST_BILINEAR,
			NULL, NULL, NULL);

		if (!dec->sws)
			return false;
	}

	/* Convert */
	sws_scale(dec->sws, (const uint8_t *const *)frame->data,
		  frame->linesize, 0, frame->height, dec->nv12_data,
		  dec->nv12_linesize);

	return true;
}

bool video_decoder_decode(struct video_decoder *dec, const uint8_t *h264_data,
			  size_t h264_size, uint64_t pts,
			  struct decoded_frame *out_frame)
{
	if (!dec || !dec->initialized)
		return false;

	dec->pkt->data = (uint8_t *)h264_data;
	dec->pkt->size = (int)h264_size;
	dec->pkt->pts = (int64_t)pts;
	dec->pkt->dts = (int64_t)pts;

	/* Match mika314's decode pattern exactly:
	 * receive first (get previous frame), then send (feed new data).
	 * Output is always 1 frame behind input - this is normal. */
	int got_picture = 0;
	int ret = avcodec_receive_frame(dec->ctx, dec->frame);
	if (ret == 0)
		got_picture = 1;
	if (ret == AVERROR(EAGAIN))
		ret = 0;
	if (ret == 0)
		ret = avcodec_send_packet(dec->ctx, dec->pkt);

	if (ret < 0 && ret != AVERROR(EAGAIN))
		return false;

	if (!got_picture)
		return false;

	if (dec->frame->width == 0 || dec->frame->height == 0)
		return false;

	/* Convert to RGBA for OBS output */
	int w = dec->frame->width;
	int h = dec->frame->height;

	if (w != dec->nv12_width || h != dec->nv12_height || !dec->sws) {
		if (dec->sws)
			sws_freeContext(dec->sws);
		if (dec->nv12_data[0])
			av_freep(&dec->nv12_data[0]);

		dec->sws = sws_getContext(w, h,
					 (enum AVPixelFormat)dec->frame->format,
					 w, h, AV_PIX_FMT_RGBA,
					 SWS_FAST_BILINEAR, NULL, NULL, NULL);
		if (!dec->sws)
			return false;

		int size = av_image_alloc(dec->nv12_data, dec->nv12_linesize,
					  w, h, AV_PIX_FMT_RGBA, 32);
		if (size < 0)
			return false;

		dec->nv12_width = w;
		dec->nv12_height = h;
	}

	sws_scale(dec->sws, (const uint8_t *const *)dec->frame->data,
		  dec->frame->linesize, 0, h, dec->nv12_data,
		  dec->nv12_linesize);

	out_frame->data[0] = dec->nv12_data[0];
	out_frame->linesize[0] = dec->nv12_linesize[0];
	out_frame->width = w;
	out_frame->height = h;
	out_frame->pts = dec->frame->pts;

	return true;
}

bool video_decoder_flush(struct video_decoder *dec,
			 struct decoded_frame *out_frame)
{
	if (!dec || !dec->initialized)
		return false;

	/* Send flush signal */
	avcodec_send_packet(dec->ctx, NULL);

	int ret = avcodec_receive_frame(dec->ctx, dec->frame);
	if (ret < 0)
		return false;

	if (dec->frame->format != AV_PIX_FMT_NV12) {
		if (!convert_to_nv12(dec, dec->frame))
			return false;
	}

	out_frame->data[0] = dec->nv12_data[0];
	out_frame->data[1] = dec->nv12_data[1];
	out_frame->linesize[0] = dec->nv12_linesize[0];
	out_frame->linesize[1] = dec->nv12_linesize[1];
	out_frame->width = dec->frame->width;
	out_frame->height = dec->frame->height;
	out_frame->pts = dec->frame->pts;

	return true;
}
