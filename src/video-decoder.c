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
};

struct video_decoder *video_decoder_create(void)
{
	struct video_decoder *dec = calloc(1, sizeof(struct video_decoder));
	if (!dec)
		return NULL;

	/* Find H.264 decoder - try hardware first */
	dec->codec = avcodec_find_decoder_by_name("h264_cuvid"); /* NVIDIA */
	if (!dec->codec)
		dec->codec = avcodec_find_decoder_by_name("h264_qsv"); /* Intel */
	if (!dec->codec)
		dec->codec = avcodec_find_decoder_by_name("h264_d3d11va"); /* D3D11 */
	if (!dec->codec)
		dec->codec = avcodec_find_decoder(AV_CODEC_ID_H264); /* Software fallback */

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

	/* Configure for low latency */
	dec->ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
	dec->ctx->flags2 |= AV_CODEC_FLAG2_FAST;
	dec->ctx->thread_count = 4;

	if (avcodec_open2(dec->ctx, dec->codec, NULL) < 0) {
		fprintf(stderr, "[AirPlay] Failed to open decoder, trying software\n");

		/* Fallback to software decoder */
		avcodec_free_context(&dec->ctx);
		dec->codec = avcodec_find_decoder(AV_CODEC_ID_H264);
		if (!dec->codec) {
			free(dec);
			return NULL;
		}

		dec->ctx = avcodec_alloc_context3(dec->codec);
		dec->ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
		dec->ctx->flags2 |= AV_CODEC_FLAG2_FAST;
		dec->ctx->thread_count = 4;

		if (avcodec_open2(dec->ctx, dec->codec, NULL) < 0) {
			avcodec_free_context(&dec->ctx);
			free(dec);
			return NULL;
		}
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

	int ret = avcodec_send_packet(dec->ctx, dec->pkt);
	if (ret < 0) {
		/* Not an error if we just need more data */
		if (ret == AVERROR(EAGAIN))
			return false;
		return false;
	}

	ret = avcodec_receive_frame(dec->ctx, dec->frame);
	if (ret < 0)
		return false;

	/* Convert to NV12 if not already */
	if (dec->frame->format == AV_PIX_FMT_NV12) {
		/* Already NV12 - use directly */
		out_frame->data[0] = dec->frame->data[0];
		out_frame->data[1] = dec->frame->data[1];
		out_frame->linesize[0] = dec->frame->linesize[0];
		out_frame->linesize[1] = dec->frame->linesize[1];
	} else {
		/* Convert to NV12 */
		if (!convert_to_nv12(dec, dec->frame))
			return false;

		out_frame->data[0] = dec->nv12_data[0];
		out_frame->data[1] = dec->nv12_data[1];
		out_frame->linesize[0] = dec->nv12_linesize[0];
		out_frame->linesize[1] = dec->nv12_linesize[1];
	}

	out_frame->width = dec->frame->width;
	out_frame->height = dec->frame->height;
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
