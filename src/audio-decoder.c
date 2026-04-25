/*
 * AAC Audio Decoder using FFmpeg (libavcodec)
 *
 * Decodes AAC audio from AirPlay into float32 PCM
 * that can be fed to OBS via obs_source_output_audio().
 */

#include "audio-decoder.h"

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct audio_decoder {
	const AVCodec *codec;
	AVCodecContext *ctx;
	AVFrame *frame;
	AVPacket *pkt;
	struct SwrContext *swr;

	/* Output buffer */
	float *pcm_buf;
	size_t pcm_buf_size;

	bool initialized;
	bool configured;
};

struct audio_decoder *audio_decoder_create(void)
{
	struct audio_decoder *dec = calloc(1, sizeof(struct audio_decoder));
	if (!dec)
		return NULL;

	/* Try libfdk_aac first (best AAC-ELD support), fall back to built-in */
	dec->codec = avcodec_find_decoder_by_name("libfdk_aac");
	if (!dec->codec)
		dec->codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
	if (!dec->codec) {
		fprintf(stderr, "[AirPlay] No AAC decoder found!\n");
		free(dec);
		return NULL;
	}

	dec->ctx = avcodec_alloc_context3(dec->codec);
	if (!dec->ctx) {
		free(dec);
		return NULL;
	}

	/* AirPlay uses AAC-ELD 44100 Hz stereo
	 * AudioSpecificConfig for AAC-ELD 44100 stereo: F8 E8 50 00 */
	static const uint8_t aac_eld_config[] = {0xF8, 0xE8, 0x50, 0x00};
	dec->ctx->extradata = av_mallocz(sizeof(aac_eld_config) +
					 AV_INPUT_BUFFER_PADDING_SIZE);
	if (dec->ctx->extradata) {
		memcpy(dec->ctx->extradata, aac_eld_config,
		       sizeof(aac_eld_config));
		dec->ctx->extradata_size = sizeof(aac_eld_config);
	}

	dec->ctx->sample_rate = 44100;
	av_channel_layout_default(&dec->ctx->ch_layout, 2);
	dec->ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;

	if (avcodec_open2(dec->ctx, dec->codec, NULL) < 0) {
		fprintf(stderr, "[AirPlay] Failed to open AAC decoder\n");
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

	/* Allocate initial PCM buffer (1024 samples * 2 ch * 4 bytes) */
	dec->pcm_buf_size = 1024 * 2 * sizeof(float);
	dec->pcm_buf = malloc(dec->pcm_buf_size);

	dec->initialized = true;
	return dec;
}

void audio_decoder_destroy(struct audio_decoder *dec)
{
	if (!dec)
		return;

	if (dec->swr)
		swr_free(&dec->swr);

	free(dec->pcm_buf);

	if (dec->frame)
		av_frame_free(&dec->frame);
	if (dec->pkt)
		av_packet_free(&dec->pkt);
	if (dec->ctx)
		avcodec_free_context(&dec->ctx);

	free(dec);
}

bool audio_decoder_configure(struct audio_decoder *dec,
			     const uint8_t *asc_data, size_t asc_size)
{
	if (!dec || !dec->initialized)
		return false;

	/* Set extradata (AudioSpecificConfig) for the decoder */
	if (dec->ctx->extradata)
		av_free(dec->ctx->extradata);

	dec->ctx->extradata = av_mallocz(asc_size + AV_INPUT_BUFFER_PADDING_SIZE);
	if (!dec->ctx->extradata)
		return false;

	memcpy(dec->ctx->extradata, asc_data, asc_size);
	dec->ctx->extradata_size = (int)asc_size;

	dec->configured = true;
	return true;
}

static bool ensure_swr(struct audio_decoder *dec)
{
	if (dec->swr)
		return true;

	/* Create resampler: convert from decoder output to float32 interleaved */
	AVChannelLayout out_layout;
	av_channel_layout_default(&out_layout, dec->ctx->ch_layout.nb_channels);

	int ret = swr_alloc_set_opts2(&dec->swr,
				      &out_layout,
				      AV_SAMPLE_FMT_FLT,
				      dec->ctx->sample_rate,
				      &dec->ctx->ch_layout,
				      dec->ctx->sample_fmt,
				      dec->ctx->sample_rate,
				      0, NULL);

	if (ret < 0 || !dec->swr)
		return false;

	if (swr_init(dec->swr) < 0) {
		swr_free(&dec->swr);
		return false;
	}

	return true;
}

bool audio_decoder_decode(struct audio_decoder *dec, const uint8_t *aac_data,
			  size_t aac_size, uint64_t pts,
			  struct decoded_audio *out)
{
	if (!dec || !dec->initialized)
		return false;

	dec->pkt->data = (uint8_t *)aac_data;
	dec->pkt->size = (int)aac_size;
	dec->pkt->pts = (int64_t)pts;

	int ret = avcodec_send_packet(dec->ctx, dec->pkt);
	if (ret < 0)
		return false;

	ret = avcodec_receive_frame(dec->ctx, dec->frame);
	if (ret < 0)
		return false;

	int channels = dec->ctx->ch_layout.nb_channels;
	int samples = dec->frame->nb_samples;

	/* Ensure SWR context */
	if (!ensure_swr(dec))
		return false;

	/* Ensure output buffer is large enough */
	size_t needed = samples * channels * sizeof(float);
	if (needed > dec->pcm_buf_size) {
		float *tmp = realloc(dec->pcm_buf, needed);
		if (!tmp)
			return false;
		dec->pcm_buf      = tmp;
		dec->pcm_buf_size = needed;
	}

	/* Convert to interleaved float32 */
	uint8_t *out_buf = (uint8_t *)dec->pcm_buf;
	int converted = swr_convert(dec->swr, &out_buf, samples,
				    (const uint8_t **)dec->frame->data,
				    samples);
	if (converted <= 0)
		return false;

	out->data = (uint8_t *)dec->pcm_buf;
	out->data_size = converted * channels * sizeof(float);
	out->samples = converted;
	out->channels = channels;
	out->sample_rate = dec->ctx->sample_rate;
	out->pts = dec->frame->pts;

	return true;
}
