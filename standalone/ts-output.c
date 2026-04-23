/*
 * ts-output.c
 * MPEG-TS over TCP server using FFmpeg.
 *
 * Architecture:
 *   - A plain TCP listening socket is created at startup (non-blocking).
 *   - Each call to ts_output_write_video / ts_output_write_audio first
 *     checks for a new client via non-blocking accept().
 *   - When a client is present an AVFormatContext (mpegts) is opened
 *     with a custom AVIOContext that writes through the socket.
 *   - On write failure the context is torn down; the next frame
 *     attempt will accept a fresh connection.
 *   - A CRITICAL_SECTION serialises concurrent callback threads.
 *
 * Video path : H.264 Annex-B bytes → MPEG-TS (no decode/re-encode)
 * Audio path : float32 PCM interleaved → accumulate → AAC-LC encode
 *              → MPEG-TS  (AAC-ELD cannot be represented with ADTS
 *              headers so it must be transcoded)
 */

#include "ts-output.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Platform abstractions                                                */
/* ------------------------------------------------------------------ */

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
typedef SOCKET   sock_t;
#  define INVALID_SOCK  INVALID_SOCKET
static void      sock_set_nonblock(sock_t s)
{
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
}
static void      sock_close(sock_t s) { closesocket(s); }
static bool      sock_would_block(void)
{
    return WSAGetLastError() == WSAEWOULDBLOCK;
}
#  include <windows.h>
typedef CRITICAL_SECTION ts_mutex_t;
static void mutex_init(ts_mutex_t *m)  { InitializeCriticalSection(m); }
static void mutex_lock(ts_mutex_t *m)  { EnterCriticalSection(m); }
static void mutex_unlock(ts_mutex_t *m){ LeaveCriticalSection(m); }
static void mutex_destroy(ts_mutex_t *m){ DeleteCriticalSection(m); }
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
typedef int      sock_t;
#  define INVALID_SOCK  (-1)
static void      sock_set_nonblock(sock_t s)
{
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
}
static void      sock_close(sock_t s) { close(s); }
static bool      sock_would_block(void)
{
    return errno == EAGAIN || errno == EWOULDBLOCK;
}
#  include <pthread.h>
typedef pthread_mutex_t ts_mutex_t;
static void mutex_init(ts_mutex_t *m)  { pthread_mutex_init(m, NULL); }
static void mutex_lock(ts_mutex_t *m)  { pthread_mutex_lock(m); }
static void mutex_unlock(ts_mutex_t *m){ pthread_mutex_unlock(m); }
static void mutex_destroy(ts_mutex_t *m){ pthread_mutex_destroy(m); }
#endif

/* ------------------------------------------------------------------ */
/* FFmpeg includes                                                      */
/* ------------------------------------------------------------------ */

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>

/* ------------------------------------------------------------------ */
/* Constants                                                            */
/* ------------------------------------------------------------------ */

#define AAC_FRAME_SAMPLES   1024        /* AAC-LC frame size */
#define MAX_AUDIO_CHANNELS  2
/* buffer capacity: enough for several AAC frames per channel */
#define AUDIO_BUF_CAP       (AAC_FRAME_SAMPLES * 8 * MAX_AUDIO_CHANNELS)
#define AVIO_BUF_SIZE       (64 * 1024)

/* ------------------------------------------------------------------ */
/* ts_output struct                                                     */
/* ------------------------------------------------------------------ */

struct ts_output {
    int         port;
    bool        hw_accel;
    ts_mutex_t  lock;

    /* TCP server / client */
    sock_t      listen_sock;
    sock_t      client_sock;

    /* Custom AVIO write buffer (owned by AVIOContext after open) */
    unsigned char *avio_buf;

    /* FFmpeg mux */
    AVFormatContext *fmt_ctx;
    int              video_idx;
    int              audio_idx;

    /* AAC-LC encoder */
    AVCodecContext  *aac_enc;
    AVFrame         *aac_frame;
    AVPacket        *aac_pkt;

    int  enc_channels;
    int  enc_sample_rate;

    /* Interleaved float PCM accumulation buffer */
    float   audio_buf[AUDIO_BUF_CAP];
    int     audio_buf_n;        /* samples buffered PER channel */
    int64_t audio_pts;          /* next encoded frame PTS (in samples) */

    /* PTS normalisation: subtract first observed video PTS */
    int64_t pts_base;
    bool    pts_base_set;

    /* After a new client connects, hold video until the first IDR
     * (keyframe) arrives — players cannot decode without an IDR. */
    bool    waiting_for_keyframe;
};

/* ------------------------------------------------------------------ */
/* Helpers: H.264 keyframe detection                                   */
/* ------------------------------------------------------------------ */

static bool h264_is_keyframe(const uint8_t *data, size_t size)
{
    for (size_t i = 0; i + 4 < size; ) {
        /* 4-byte start code */
        if (data[i] == 0 && data[i+1] == 0 &&
            data[i+2] == 0 && data[i+3] == 1) {
            uint8_t nal_type = data[i+4] & 0x1F;
            if (nal_type == 5) return true; /* IDR */
            i += 4;
            continue;
        }
        /* 3-byte start code */
        if (data[i] == 0 && data[i+1] == 0 && data[i+2] == 1) {
            uint8_t nal_type = data[i+3] & 0x1F;
            if (nal_type == 5) return true; /* IDR */
            i += 3;
            continue;
        }
        i++;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Helpers: AVIO write callback                                         */
/* ------------------------------------------------------------------ */

static int avio_write_cb(void *opaque, const uint8_t *buf, int buf_size)
{
    struct ts_output *out = (struct ts_output *)opaque;
    if (out->client_sock == INVALID_SOCK)
        return AVERROR(EIO);

    int total = 0;
    while (total < buf_size) {
        int sent = (int)send(out->client_sock,
                             (const char *)buf + total,
                             buf_size - total, 0);
        if (sent <= 0)
            return AVERROR(EIO);
        total += sent;
    }
    return total;
}

/* ------------------------------------------------------------------ */
/* Connection management                                               */
/* ------------------------------------------------------------------ */

/* Non-blocking accept: returns true if a client was accepted. */
static bool try_accept(struct ts_output *out)
{
    if (out->listen_sock == INVALID_SOCK)
        return false;

    struct sockaddr_in addr;
#ifdef _WIN32
    int addrlen = sizeof(addr);
#else
    socklen_t addrlen = sizeof(addr);
#endif
    sock_t cs = accept(out->listen_sock,
                       (struct sockaddr *)&addr, &addrlen);
    if (cs == INVALID_SOCK) {
        if (!sock_would_block())
            fprintf(stderr, "[TS] accept error\n");
        return false;
    }

    out->client_sock = cs;
    fprintf(stdout, "[TS] Player connected — streaming on port %d\n",
            out->port);
    return true;
}

/* Open FFmpeg format context for the accepted client socket. */
static bool open_output(struct ts_output *out,
                        int width, int height,
                        int sample_rate, int channels)
{
    /* Allocate MPEG-TS format context (no URL — we use custom AVIO) */
    if (avformat_alloc_output_context2(&out->fmt_ctx, NULL,
                                       "mpegts", NULL) < 0) {
        fprintf(stderr, "[TS] avformat_alloc_output_context2 failed\n");
        return false;
    }
    /* Low-latency flag */
    out->fmt_ctx->flags |= AVFMT_FLAG_NOBUFFER;

    /* Disable interleave-delta enforcement so that the small initial
     * PTS offset introduced by the AAC encoder delay does not cause
     * the muxer to drop or reorder packets (which results in a black
     * screen and silent audio in players such as VLC/mpv). */
    out->fmt_ctx->max_interleave_delta = 0;

    /* Custom AVIO context */
    out->avio_buf = (unsigned char *)av_malloc(AVIO_BUF_SIZE);
    if (!out->avio_buf) goto fail_fmt;

    out->fmt_ctx->pb = avio_alloc_context(
            out->avio_buf, AVIO_BUF_SIZE,
            1,          /* write flag */
            out,        /* opaque */
            NULL,       /* read_packet */
            avio_write_cb,
            NULL);      /* seek */
    if (!out->fmt_ctx->pb) {
        av_free(out->avio_buf);
        out->avio_buf = NULL;
        goto fail_fmt;
    }

    /* ---- Video stream (H.264 passthrough) ---- */
    {
        AVStream *vs = avformat_new_stream(out->fmt_ctx, NULL);
        if (!vs) goto fail_fmt;
        vs->time_base = (AVRational){1, AV_TIME_BASE};
        vs->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        vs->codecpar->codec_id   = AV_CODEC_ID_H264;
        vs->codecpar->width      = (width  > 0) ? width  : 1920;
        vs->codecpar->height     = (height > 0) ? height : 1080;
        vs->avg_frame_rate       = (AVRational){60, 1};
        vs->r_frame_rate         = (AVRational){60, 1};
        out->video_idx = vs->index;
    }

    /* ---- Audio stream (AAC-LC encoder) ---- */
    {
        const AVCodec *aac = NULL;

#ifdef _WIN32
        /* Try hardware-accelerated Windows Media Foundation encoder */
        if (out->hw_accel) {
            aac = avcodec_find_encoder_by_name("aac_mf");
            if (aac)
                fprintf(stdout, "[TS] Using hardware AAC encoder: aac_mf\n");
        }
#endif
        if (!aac)
            aac = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (!aac) {
            fprintf(stderr, "[TS] AAC encoder not found\n");
            goto fail_fmt;
        }

        out->aac_enc = avcodec_alloc_context3(aac);
        if (!out->aac_enc) goto fail_fmt;

        out->aac_enc->sample_rate = sample_rate;
        out->aac_enc->sample_fmt  = AV_SAMPLE_FMT_FLTP;
        av_channel_layout_default(&out->aac_enc->ch_layout, channels);
        out->aac_enc->bit_rate  = 128000;
        out->aac_enc->time_base = (AVRational){1, sample_rate};

        if (avcodec_open2(out->aac_enc, aac, NULL) < 0) {
            fprintf(stderr, "[TS] avcodec_open2 (AAC) failed\n");
            goto fail_aac;
        }

        out->aac_frame = av_frame_alloc();
        out->aac_pkt   = av_packet_alloc();
        if (!out->aac_frame || !out->aac_pkt) goto fail_aac;

        out->aac_frame->nb_samples     = AAC_FRAME_SAMPLES;
        out->aac_frame->sample_rate    = sample_rate;
        out->aac_frame->format         = AV_SAMPLE_FMT_FLTP;
        av_channel_layout_copy(&out->aac_frame->ch_layout,
                               &out->aac_enc->ch_layout);
        if (av_frame_get_buffer(out->aac_frame, 0) < 0)
            goto fail_aac;

        AVStream *as = avformat_new_stream(out->fmt_ctx, NULL);
        if (!as) goto fail_aac;
        as->time_base = (AVRational){1, AV_TIME_BASE};
        if (avcodec_parameters_from_context(as->codecpar,
                                            out->aac_enc) < 0)
            goto fail_aac;
        out->audio_idx = as->index;

        out->enc_channels    = channels;
        out->enc_sample_rate = sample_rate;

        /* Start the audio PTS counter at -initial_padding so that the
         * first encoded output packet (which the encoder delays by that
         * many samples) emerges with PTS = 0, aligned with video. */
        out->audio_pts = -(int64_t)out->aac_enc->initial_padding;
    }

    /* ---- Write MPEG-TS header ---- */
    if (avformat_write_header(out->fmt_ctx, NULL) < 0) {
        fprintf(stderr, "[TS] avformat_write_header failed\n");
        goto fail_aac;
    }

    /* Hold all video output until the first IDR (keyframe) is received.
     * Players such as VLC cannot start H.264 decoding mid-stream. */
    out->waiting_for_keyframe = true;

    return true;

fail_aac:
    if (out->aac_frame) { av_frame_free(&out->aac_frame); out->aac_frame = NULL; }
    if (out->aac_pkt)   { av_packet_free(&out->aac_pkt);  out->aac_pkt   = NULL; }
    if (out->aac_enc)   { avcodec_free_context(&out->aac_enc); out->aac_enc = NULL; }
fail_fmt:
    if (out->fmt_ctx) {
        if (out->fmt_ctx->pb) {
            /* Null out pb->buffer before freeing the context so FFmpeg
             * does not attempt to free it; we free it ourselves via
             * out->avio_buf which we allocated. */
            out->fmt_ctx->pb->buffer = NULL;
            avio_context_free(&out->fmt_ctx->pb);
        }
        avformat_free_context(out->fmt_ctx);
        out->fmt_ctx = NULL;
    }
    av_free(out->avio_buf);
    out->avio_buf = NULL;
    return false;
}

/* Tear down an active output context and close the client socket. */
static void close_output(struct ts_output *out)
{
    if (out->fmt_ctx) {
        /* Best-effort trailer */
        av_write_trailer(out->fmt_ctx);

        if (out->fmt_ctx->pb) {
            out->fmt_ctx->pb->buffer = NULL; /* we own the buffer */
            avio_context_free(&out->fmt_ctx->pb);
        }
        avformat_free_context(out->fmt_ctx);
        out->fmt_ctx = NULL;
    }
    av_free(out->avio_buf);
    out->avio_buf = NULL;
    if (out->aac_frame) { av_frame_free(&out->aac_frame); out->aac_frame = NULL; }
    if (out->aac_pkt)   { av_packet_free(&out->aac_pkt);  out->aac_pkt   = NULL; }
    if (out->aac_enc)   { avcodec_free_context(&out->aac_enc); out->aac_enc = NULL; }

    if (out->client_sock != INVALID_SOCK) {
        sock_close(out->client_sock);
        out->client_sock = INVALID_SOCK;
        fprintf(stdout, "[TS] Player disconnected — waiting for reconnect\n");
    }

    /* Reset audio accumulation and PTS normalisation */
    out->audio_buf_n  = 0;
    out->audio_pts    = 0;
    out->pts_base_set = false;
    /* waiting_for_keyframe is re-armed by open_output on the next
     * connection; it must NOT be cleared here. */
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

struct ts_output *ts_output_create(int port, bool hw_accel)
{
    avformat_network_init();

    struct ts_output *out =
        (struct ts_output *)calloc(1, sizeof(struct ts_output));
    if (!out)
        return NULL;

    out->port        = port;
    out->hw_accel    = hw_accel;
    out->listen_sock = INVALID_SOCK;
    out->client_sock = INVALID_SOCK;
    out->video_idx   = -1;
    out->audio_idx   = -1;
    mutex_init(&out->lock);

    /* Create listening TCP socket */
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    out->listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (out->listen_sock == INVALID_SOCK) {
        fprintf(stderr, "[TS] socket() failed\n");
        goto fail;
    }

    /* Allow address reuse */
    int opt = 1;
    setsockopt(out->listen_sock, SOL_SOCKET, SO_REUSEADDR,
               (const char *)&opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((unsigned short)port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(out->listen_sock,
             (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "[TS] bind() failed on port %d\n", port);
        goto fail;
    }
    if (listen(out->listen_sock, 1) != 0) {
        fprintf(stderr, "[TS] listen() failed\n");
        goto fail;
    }

    sock_set_nonblock(out->listen_sock);

    fprintf(stdout,
            "[TS] Listening on TCP port %d — open "
            "tcp://localhost:%d in VLC\n", port, port);
    return out;

fail:
    ts_output_destroy(out);
    return NULL;
}

void ts_output_destroy(struct ts_output *out)
{
    if (!out)
        return;

    mutex_lock(&out->lock);
    close_output(out);
    if (out->listen_sock != INVALID_SOCK) {
        sock_close(out->listen_sock);
        out->listen_sock = INVALID_SOCK;
    }
    mutex_unlock(&out->lock);

    mutex_destroy(&out->lock);
    free(out);

    avformat_network_deinit();
}

void ts_output_write_video(struct ts_output *out,
                           const uint8_t *data, size_t size,
                           int64_t pts)
{
    if (!out || !data || size == 0)
        return;

    mutex_lock(&out->lock);

    /* Normalise PTS */
    if (!out->pts_base_set) {
        out->pts_base     = pts;
        out->pts_base_set = true;
    }
    int64_t norm_pts = pts - out->pts_base;

    /* Try to acquire a client if not connected */
    if (out->client_sock == INVALID_SOCK) {
        if (!try_accept(out))
            goto done;
        /* Open output context on first connection */
        if (!open_output(out, 0, 0,
                         44100, MAX_AUDIO_CHANNELS)) {
            sock_close(out->client_sock);
            out->client_sock = INVALID_SOCK;
            goto done;
        }
    }

    /* Build AVPacket and write */
    {
        AVPacket *pkt = av_packet_alloc();
        if (!pkt) goto done;

        bool is_key = h264_is_keyframe(data, size);

        /* Skip non-IDR frames until we see the first keyframe after a
         * new client connects.  Without this, players such as VLC
         * receive undecipherable P-frames first and display nothing. */
        if (out->waiting_for_keyframe) {
            if (!is_key) {
                av_packet_free(&pkt);
                goto done;
            }
            out->waiting_for_keyframe = false;
        }

        /* Wrap data without copy */
        pkt->data         = (uint8_t *)data;
        pkt->size         = (int)size;
        pkt->stream_index = out->video_idx;
        pkt->pts          = norm_pts;
        pkt->dts          = norm_pts;
        if (is_key)
            pkt->flags |= AV_PKT_FLAG_KEY;

        int ret = av_interleaved_write_frame(out->fmt_ctx, pkt);
        av_packet_free(&pkt);

        if (ret < 0) {
            fprintf(stderr, "[TS] Video write error — closing connection\n");
            close_output(out);
        }
    }

done:
    mutex_unlock(&out->lock);
}

/* Encode one 1024-sample AAC frame from the accumulation buffer. */
static void flush_audio_frame(struct ts_output *out)
{
    if (!out->aac_enc || !out->aac_frame)
        return;

    int ch = out->enc_channels;

    /* Fill planar AVFrame from interleaved buffer */
    for (int c = 0; c < ch && c < MAX_AUDIO_CHANNELS; c++) {
        float *plane = (float *)out->aac_frame->data[c];
        for (int s = 0; s < AAC_FRAME_SAMPLES; s++)
            plane[s] = out->audio_buf[s * ch + c];
    }
    out->aac_frame->pts = out->audio_pts;

    if (avcodec_send_frame(out->aac_enc, out->aac_frame) < 0)
        return;

    while (avcodec_receive_packet(out->aac_enc, out->aac_pkt) == 0) {
        /* Convert PTS from sample-domain to AV_TIME_BASE */
        int64_t pts_us = av_rescale_q(out->aac_pkt->pts,
                                      (AVRational){1, out->enc_sample_rate},
                                      (AVRational){1, AV_TIME_BASE});

        out->aac_pkt->stream_index = out->audio_idx;
        out->aac_pkt->pts          = pts_us;
        out->aac_pkt->dts          = pts_us;

        int ret = av_interleaved_write_frame(out->fmt_ctx, out->aac_pkt);
        av_packet_unref(out->aac_pkt);

        if (ret < 0) {
            fprintf(stderr, "[TS] Audio write error — closing connection\n");
            close_output(out);
            return;
        }
    }

    /* Advance PTS by one frame */
    out->audio_pts += AAC_FRAME_SAMPLES;

    /* Shift remaining samples to the front */
    int remain = out->audio_buf_n - AAC_FRAME_SAMPLES;
    if (remain > 0) {
        memmove(out->audio_buf,
                out->audio_buf + (size_t)(AAC_FRAME_SAMPLES * ch),
                (size_t)(remain * ch) * sizeof(float));
    }
    out->audio_buf_n = remain;
}

void ts_output_write_audio(struct ts_output *out,
                           const float *pcm, int samples,
                           int channels, int sample_rate,
                           int64_t pts)
{
    if (!out || !pcm || samples <= 0)
        return;

    (void)pts;      /* PTS derived from sample counter */
    (void)sample_rate;

    mutex_lock(&out->lock);

    if (out->client_sock == INVALID_SOCK || !out->fmt_ctx)
        goto done;

    /* Clamp channels to our buffer layout */
    int ch = (channels < MAX_AUDIO_CHANNELS) ? channels : MAX_AUDIO_CHANNELS;

    /* Append samples to accumulation buffer */
    for (int s = 0; s < samples; s++) {
        if (out->audio_buf_n >= (AUDIO_BUF_CAP / ch))
            break; /* overflow protection */
        for (int c = 0; c < ch; c++)
            out->audio_buf[out->audio_buf_n * ch + c] = pcm[s * channels + c];
        out->audio_buf_n++;
    }

    /* Encode as many complete AAC frames as available */
    while (out->audio_buf_n >= AAC_FRAME_SAMPLES && out->fmt_ctx)
        flush_audio_frame(out);

done:
    mutex_unlock(&out->lock);
}
