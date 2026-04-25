/*
 * airplay-stream.c
 * Standalone AirPlay receiver — no OBS.
 *
 * Uses UxPlay's raop library for the full AirPlay 2 mirroring
 * protocol (FairPlay, pairing, encryption) and feeds the raw
 * H.264 / AAC data to webrtc-output.c for low-latency browser playback.
 *
 * The audio is decoded from AAC-ELD to PCM here (using FFmpeg)
 * before handing it to webrtc-output, which then re-encodes to Opus.
 */

#include "airplay-stream.h"
#include "webrtc-output.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* UxPlay raop API */
#include "raop.h"
#include "dnssd.h"
#include "stream.h"
#include "logger.h"

/* FFmpeg (audio decode only) */
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <iphlpapi.h>
#  include <process.h>
#  include <windows.h>
#  define getpid _getpid
#  define SLEEP_MS(ms) Sleep(ms)
#else
#  include <unistd.h>
#  define SLEEP_MS(ms) usleep((ms)*1000)
#endif

/* ------------------------------------------------------------------ */
/* Internal state                                                       */
/* ------------------------------------------------------------------ */

struct audio_dec {
    const AVCodec   *codec;
    AVCodecContext  *ctx;
    AVFrame         *frame;
    AVPacket        *pkt;
    struct SwrContext *swr;
    float           *pcm_buf;
    size_t           pcm_buf_size;
};

struct airplay_ctx {
    raop_t          *raop;
    dnssd_t         *dnssd;
    struct webrtc_output *webrtc;
    struct audio_dec adec;
    int              open_connections;
};

/* Single global instance (one server per process) */
static struct airplay_ctx *g_ctx = NULL;

/* ------------------------------------------------------------------ */
/* MAC address helpers                                                  */
/* ------------------------------------------------------------------ */

static void generate_random_mac(char *mac, size_t len)
{
    srand((unsigned)(time(NULL) * (unsigned long)getpid()));
    int octet = (rand() % 64) << 2 | 0x02; /* locally administered */
    snprintf(mac, len, "%02x:%02x:%02x:%02x:%02x:%02x",
             octet,       rand() % 256, rand() % 256,
             rand() % 256, rand() % 256, rand() % 256);
}

static void parse_hw_addr(const char *str, char *hw, int *hw_len)
{
    *hw_len = 0;
    for (size_t i = 0; i < strlen(str) && *hw_len < 6; i += 3)
        hw[(*hw_len)++] = (char)strtol(str + i, NULL, 16);
}

/* Log local encoder capabilities for future low-latency transcode mode.
 * This checks FFmpeg encoder availability in the current build/runtime. */
static void log_video_encoder_capabilities(void)
{
    const AVCodec *enc_nvenc = avcodec_find_encoder_by_name("h264_nvenc");
    const AVCodec *enc_qsv   = avcodec_find_encoder_by_name("h264_qsv");
    const AVCodec *enc_amf   = avcodec_find_encoder_by_name("h264_amf");
    const AVCodec *enc_vtb   = avcodec_find_encoder_by_name("h264_videotoolbox");
    const AVCodec *enc_x264  = avcodec_find_encoder_by_name("libx264");
    const AVCodec *enc_sw    = avcodec_find_encoder(AV_CODEC_ID_H264);

    fprintf(stdout,
            "[Video] H264 encoder availability: NVENC=%s QSV=%s AMF=%s VTB=%s "
            "libx264=%s software=%s\n",
            enc_nvenc ? "yes" : "no",
            enc_qsv   ? "yes" : "no",
            enc_amf   ? "yes" : "no",
            enc_vtb   ? "yes" : "no",
            enc_x264  ? "yes" : "no",
            enc_sw    ? "yes" : "no");

    if (enc_nvenc) {
        fprintf(stdout,
                "[Video] Preferred hardware encoder candidate: h264_nvenc\n");
    } else if (enc_qsv) {
        fprintf(stdout,
                "[Video] Preferred hardware encoder candidate: h264_qsv\n");
    } else if (enc_amf) {
        fprintf(stdout,
                "[Video] Preferred hardware encoder candidate: h264_amf\n");
    } else if (enc_vtb) {
        fprintf(stdout,
                "[Video] Preferred hardware encoder candidate: h264_videotoolbox\n");
    } else if (enc_x264 || enc_sw) {
        fprintf(stdout,
                "[Video] No hardware H264 encoder detected; software encode fallback will be used.\n");
    } else {
        fprintf(stdout,
                "[Video] No H264 encoder detected in FFmpeg build.\n");
    }
}

static webrtc_video_mode_t map_video_mode(airplay_video_mode_t mode)
{
    switch (mode) {
    case AIRPLAY_VIDEO_MODE_TRANSCODE_AUTO:
        return WEBRTC_VIDEO_MODE_TRANSCODE_AUTO;
    case AIRPLAY_VIDEO_MODE_PASSTHROUGH:
    default:
        return WEBRTC_VIDEO_MODE_PASSTHROUGH;
    }
}

static webrtc_video_encoder_preference_t map_video_encoder_preference(
    airplay_video_encoder_preference_t pref)
{
    switch (pref) {
    case AIRPLAY_VIDEO_ENCODER_NVENC:
        return WEBRTC_VIDEO_ENCODER_NVENC;
    case AIRPLAY_VIDEO_ENCODER_QSV:
        return WEBRTC_VIDEO_ENCODER_QSV;
    case AIRPLAY_VIDEO_ENCODER_AMF:
        return WEBRTC_VIDEO_ENCODER_AMF;
    case AIRPLAY_VIDEO_ENCODER_VIDEOTOOLBOX:
        return WEBRTC_VIDEO_ENCODER_VIDEOTOOLBOX;
    case AIRPLAY_VIDEO_ENCODER_LIBX264:
        return WEBRTC_VIDEO_ENCODER_LIBX264;
    case AIRPLAY_VIDEO_ENCODER_SOFTWARE:
        return WEBRTC_VIDEO_ENCODER_SOFTWARE;
    case AIRPLAY_VIDEO_ENCODER_AUTO:
    default:
        return WEBRTC_VIDEO_ENCODER_AUTO;
    }
}

/* ------------------------------------------------------------------ */
/* Audio decoder lifecycle                                              */
/* ------------------------------------------------------------------ */

static bool adec_init(struct audio_dec *d, bool hw_accel)
{
    memset(d, 0, sizeof(*d));

#ifdef _WIN32
    /* On Windows, try the Media Foundation AAC decoder first when
     * hardware acceleration is requested.  aac_mf can leverage
     * hardware decoders (Intel/AMD/NVIDIA) when available. */
    bool hw_codec_found = false;
    if (hw_accel) {
        d->codec = avcodec_find_decoder_by_name("aac_mf");
        if (d->codec) {
            fprintf(stdout, "[AirPlay] Audio decoder: aac_mf (hardware)\n");
            hw_codec_found = true;
        } else {
            fprintf(stdout,
                    "[AirPlay] aac_mf not available, falling back to software\n");
        }
    }
#else
    (void)hw_accel;
    bool hw_codec_found = false;
#endif

    /* Try libfdk_aac (best AAC-ELD support), fall back to built-in */
    if (!d->codec)
        d->codec = avcodec_find_decoder_by_name("libfdk_aac");
    if (!d->codec)
        d->codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    if (!d->codec) {
        fprintf(stderr, "[AirPlay] No AAC decoder available\n");
        return false;
    }

    if (!hw_codec_found)
        fprintf(stdout, "[AirPlay] Audio decoder: %s (software)\n", d->codec->name);

    d->ctx = avcodec_alloc_context3(d->codec);
    if (!d->ctx) return false;

    /* AudioSpecificConfig for AAC-ELD 44100 Hz stereo: F8 E8 50 00 */
    static const uint8_t asc[] = {0xF8, 0xE8, 0x50, 0x00};
    d->ctx->extradata = (uint8_t *)av_mallocz(sizeof(asc) +
                                              AV_INPUT_BUFFER_PADDING_SIZE);
    if (d->ctx->extradata) {
        memcpy(d->ctx->extradata, asc, sizeof(asc));
        d->ctx->extradata_size = sizeof(asc);
    }
    d->ctx->sample_rate = 44100;
    av_channel_layout_default(&d->ctx->ch_layout, 2);
    d->ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;

    if (avcodec_open2(d->ctx, d->codec, NULL) < 0) {
        fprintf(stderr, "[AirPlay] Failed to open AAC decoder\n");
        avcodec_free_context(&d->ctx);
        return false;
    }

    d->frame = av_frame_alloc();
    d->pkt   = av_packet_alloc();
    if (!d->frame || !d->pkt) {
        avcodec_free_context(&d->ctx);
        av_frame_free(&d->frame);
        av_packet_free(&d->pkt);
        return false;
    }

    d->pcm_buf_size = 1024 * 2 * sizeof(float);
    d->pcm_buf      = (float *)malloc(d->pcm_buf_size);

    return true;
}

static void adec_destroy(struct audio_dec *d)
{
    if (d->swr)   swr_free(&d->swr);
    free(d->pcm_buf);
    if (d->frame) av_frame_free(&d->frame);
    if (d->pkt)   av_packet_free(&d->pkt);
    if (d->ctx)   avcodec_free_context(&d->ctx);
    memset(d, 0, sizeof(*d));
}

/* Decode one AAC-ELD packet → interleaved float32 PCM.
 * Returns number of samples per channel, or 0 on failure. */
static int adec_decode(struct audio_dec *d,
                       const uint8_t *aac_data, size_t aac_size,
                       uint64_t pts,
                       float **pcm_out, int *channels_out)
{
    d->pkt->data = (uint8_t *)aac_data;
    d->pkt->size = (int)aac_size;
    d->pkt->pts  = (int64_t)pts;

    if (avcodec_send_packet(d->ctx, d->pkt) < 0)
        return 0;
    if (avcodec_receive_frame(d->ctx, d->frame) < 0)
        return 0;

    int ch      = d->ctx->ch_layout.nb_channels;
    int samples = d->frame->nb_samples;

    /* Create SWR context on first use (or if ch/fmt changed) */
    if (!d->swr) {
        AVChannelLayout out_layout;
        av_channel_layout_default(&out_layout, ch);

        if (swr_alloc_set_opts2(&d->swr,
                                &out_layout, AV_SAMPLE_FMT_FLT,
                                d->ctx->sample_rate,
                                &d->ctx->ch_layout,
                                d->ctx->sample_fmt,
                                d->ctx->sample_rate,
                                0, NULL) < 0 || !d->swr) {
            return 0;
        }
        if (swr_init(d->swr) < 0) {
            swr_free(&d->swr);
            return 0;
        }
    }

    /* Ensure output buffer is large enough */
    size_t needed = (size_t)(samples * ch) * sizeof(float);
    if (needed > d->pcm_buf_size) {
        float *tmp = (float *)realloc(d->pcm_buf, needed);
        if (!tmp) return 0;
        d->pcm_buf      = tmp;
        d->pcm_buf_size = needed;
    }

    uint8_t *out_ptr = (uint8_t *)d->pcm_buf;
    int conv = swr_convert(d->swr, &out_ptr, samples,
                           (const uint8_t **)d->frame->data, samples);
    if (conv <= 0)
        return 0;

    *pcm_out      = d->pcm_buf;
    *channels_out = ch;
    return conv;
}

/* ------------------------------------------------------------------ */
/* UxPlay callbacks                                                     */
/* ------------------------------------------------------------------ */

static void cb_conn_init(void *cls)
{
    struct airplay_ctx *ctx = (struct airplay_ctx *)cls;
    ctx->open_connections++;
    fprintf(stdout, "[AirPlay] Device connected (open: %d)\n",
            ctx->open_connections);
    /* A new AirPlay session is starting.  The browser's H.264 decoder needs a
     * keyframe to sync to the fresh stream; request one so the cached IDR is
     * re-injected before the first P-frame if the device doesn't open with one. */
    if (ctx->open_connections == 1)
        webrtc_output_request_keyframe(ctx->webrtc);
}

static void cb_conn_destroy(void *cls)
{
    struct airplay_ctx *ctx = (struct airplay_ctx *)cls;
    ctx->open_connections--;
    fprintf(stdout, "[AirPlay] Device disconnected (open: %d)\n",
            ctx->open_connections);
}

static void cb_conn_reset(void *cls, int timeouts, bool reset_video)
{
    (void)cls;
    fprintf(stderr, "[AirPlay] Connection reset (timeouts=%d reset=%d)\n",
            timeouts, reset_video);
    /* Do NOT call raop_stop() here.  Stopping the RAOP listener would prevent
     * the AirPlay device from reconnecting automatically and would require the
     * user to restart the program.  UxPlay handles the session reset internally;
     * the device will re-establish the AirPlay connection on its own. */
}

static void cb_conn_teardown(void *cls, bool *t96, bool *t110)
{
    (void)cls; (void)t96; (void)t110;
    fprintf(stdout, "[AirPlay] Connection teardown\n");
}

static void cb_video_process(void *cls, raop_ntp_t *ntp,
                             h264_decode_struct *data)
{
    (void)ntp;
    struct airplay_ctx *ctx = (struct airplay_ctx *)cls;
    if (ctx->webrtc)
        webrtc_output_write_video(ctx->webrtc,
                                  data->data, (size_t)data->data_len,
                                  (int64_t)data->pts);
}

static void cb_audio_process(void *cls, raop_ntp_t *ntp,
                             audio_decode_struct *data)
{
    (void)ntp;
    struct airplay_ctx *ctx = (struct airplay_ctx *)cls;

    float *pcm   = NULL;
    int    ch    = 0;
    int    samps = adec_decode(&ctx->adec,
                               data->data, (size_t)data->data_len,
                               data->ntp_time, &pcm, &ch);
    if (samps <= 0)
        return;

    if (ctx->webrtc)
        webrtc_output_write_audio(ctx->webrtc, pcm, samps, ch,
                                  ctx->adec.ctx->sample_rate,
                                  (int64_t)data->ntp_time);
}

static void cb_audio_flush(void *cls)    { (void)cls; }
static void cb_video_flush(void *cls)    { (void)cls; }
static void cb_audio_set_volume(void *cls, float v) { (void)cls; (void)v; }
static void cb_audio_set_metadata(void *cls, const void *b, int l)
{
    (void)cls; (void)b; (void)l;
}

static void cb_audio_get_format(void *cls, unsigned char *ct,
                                unsigned short *spf, bool *usingScreen,
                                bool *isMedia, uint64_t *audioFormat)
{
    (void)cls; (void)spf; (void)usingScreen;
    (void)isMedia; (void)audioFormat;
    *ct = 1; /* AAC-ELD */
}

static void cb_video_report_size(void *cls, float *ws, float *hs,
                                 float *w, float *h)
{
    (void)cls;
    fprintf(stdout, "[AirPlay] Video size: %.0fx%.0f (requested %.0fx%.0f)\n",
            *ws, *hs, *w, *h);
}

static void cb_log(void *cls, int level, const char *msg)
{
    (void)cls;
    if (level <= RAOP_LOG_WARNING)
        fprintf(stderr, "[UxPlay] %s\n", msg);
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

bool airplay_stream_start(const struct airplay_stream_config *cfg)
{
    if (g_ctx) {
        fprintf(stderr, "[AirPlay] Already running\n");
        return false;
    }

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    log_video_encoder_capabilities();

    struct airplay_ctx *ctx =
        (struct airplay_ctx *)calloc(1, sizeof(struct airplay_ctx));
    if (!ctx) return false;

    /* Initialise audio decoder */
    if (!adec_init(&ctx->adec, cfg->hw_accel)) {
        free(ctx);
        return false;
    }

    /* Create WebRTC output (connects to mediasoup SFU) */
    struct webrtc_output_options wrtc_options;
    wrtc_options.video_mode = map_video_mode(cfg->video_mode);
    wrtc_options.video_encoder_preference =
        map_video_encoder_preference(cfg->video_encoder_preference);

    ctx->webrtc = webrtc_output_create_with_options(cfg->webrtc_port,
                                                    &wrtc_options);
    if (!ctx->webrtc) {
        fprintf(stderr, "[AirPlay] Failed to start WebRTC server on port %d\n",
                cfg->webrtc_port);
        adec_destroy(&ctx->adec);
        free(ctx);
        return false;
    }

    /* Set up UxPlay callbacks */
    raop_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.cls                = ctx;
    cbs.conn_init          = cb_conn_init;
    cbs.conn_destroy       = cb_conn_destroy;
    cbs.conn_reset         = cb_conn_reset;
    cbs.conn_teardown      = cb_conn_teardown;
    cbs.audio_process      = cb_audio_process;
    cbs.video_process      = cb_video_process;
    cbs.audio_flush        = cb_audio_flush;
    cbs.video_flush        = cb_video_flush;
    cbs.audio_set_volume   = cb_audio_set_volume;
    cbs.audio_get_format   = cb_audio_get_format;
    cbs.video_report_size  = cb_video_report_size;
    cbs.audio_set_metadata = cb_audio_set_metadata;

    ctx->raop = raop_init(2, &cbs);
    if (!ctx->raop) {
        fprintf(stderr, "[AirPlay] raop_init failed\n");
        goto fail;
    }

    raop_set_plist(ctx->raop, "max_ntp_timeouts", 5);

    if (cfg->width  > 0) raop_set_plist(ctx->raop, "width",       cfg->width);
    if (cfg->height > 0) raop_set_plist(ctx->raop, "height",      cfg->height);
    if (cfg->fps    > 0) {
        raop_set_plist(ctx->raop, "refreshRate", cfg->fps);
        raop_set_plist(ctx->raop, "maxFPS",      cfg->fps);
    }

    {
        unsigned short tcp[3] = {0, 0, 0};
        unsigned short udp[3] = {0, 0, 0};
        raop_set_tcp_ports(ctx->raop, tcp);
        raop_set_udp_ports(ctx->raop, udp);
    }

    raop_set_log_callback(ctx->raop, cb_log, NULL);
    raop_set_log_level(ctx->raop, RAOP_LOG_WARNING);

    unsigned short port = raop_get_port(ctx->raop);
    if (raop_start(ctx->raop, &port) < 0) {
        fprintf(stderr, "[AirPlay] raop_start failed\n");
        goto fail;
    }
    raop_set_port(ctx->raop, port);
    fprintf(stdout, "[AirPlay] RAOP listening on port %d\n", port);

    SLEEP_MS(100);

    /* Generate random MAC address */
    char mac_str[18] = {0};
    generate_random_mac(mac_str, sizeof(mac_str));

    char hw[6];
    int  hw_len = 0;
    parse_hw_addr(mac_str, hw, &hw_len);

    int err = 0;
    ctx->dnssd = dnssd_init(cfg->server_name,
                            (int)strlen(cfg->server_name),
                            hw, hw_len, &err);
    if (err || !ctx->dnssd) {
        fprintf(stderr, "[AirPlay] dnssd_init failed (err=%d)\n", err);
        goto fail;
    }

    raop_set_dnssd(ctx->raop, ctx->dnssd);
    dnssd_register_raop(ctx->dnssd, port);

    unsigned short ap_port = (port != 65535) ? port + 1 : port - 1;
    dnssd_register_airplay(ctx->dnssd, ap_port);

    fprintf(stdout,
            "[AirPlay] Server '%s' ready — mirror from your Apple device\n",
            cfg->server_name);

    g_ctx = ctx;
    return true;

fail:
    if (ctx->raop)  raop_destroy(ctx->raop);
    if (ctx->dnssd) dnssd_destroy(ctx->dnssd);
    if (ctx->webrtc) webrtc_output_destroy(ctx->webrtc);
    adec_destroy(&ctx->adec);
    free(ctx);
    return false;
}

void airplay_stream_stop(void)
{
    struct airplay_ctx *ctx = g_ctx;
    if (!ctx) return;
    g_ctx = NULL;

    fprintf(stdout, "[AirPlay] Stopping…\n");

    if (ctx->dnssd) {
        dnssd_unregister_raop(ctx->dnssd);
        dnssd_unregister_airplay(ctx->dnssd);
        dnssd_destroy(ctx->dnssd);
    }
    if (ctx->raop) {
        raop_stop(ctx->raop);
        raop_destroy(ctx->raop);
    }

    if (ctx->webrtc)
        webrtc_output_destroy(ctx->webrtc);
    adec_destroy(&ctx->adec);
    free(ctx);

    fprintf(stdout, "[AirPlay] Stopped\n");
}
