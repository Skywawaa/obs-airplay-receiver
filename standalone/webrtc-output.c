/*
 * webrtc-output.c
 * mediasoup SFU output — low-latency H.264 + Opus browser playback.
 *
 * Architecture:
 *   - At startup a background thread connects to the mediasoup Node.js
 *     server (GET /rtp-params) to learn the UDP port numbers of the two
 *     PlainTransports (one for video, one for audio).
 *   - Once the ports are known, two UDP sockets send unencrypted plain RTP
 *     directly to the mediasoup worker process running on localhost.
 *   - Video frames (H.264 Annex-B) are parsed into NAL units and
 *     packetised per RFC 6184 (single-NAL or FU-A).
 *   - Audio (float32 PCM) is resampled to 48 kHz via FFmpeg SWR,
 *     accumulated into 960-sample chunks, encoded to Opus, and wrapped
 *     in RTP per RFC 7587.
 *   - Fixed SSRCs and payload types must match the mediasoup server config.
 *   - A mutex serialises all state mutations and UDP sends.
 */

#include "webrtc-output.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* Platform abstractions                                                */
/* ------------------------------------------------------------------ */

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <process.h>

typedef SOCKET sock_t;
#  define INVALID_SOCK  INVALID_SOCKET

static void sock_close(sock_t s) { closesocket(s); }

typedef CRITICAL_SECTION wrtc_mutex_t;
static void mutex_init(wrtc_mutex_t *m)    { InitializeCriticalSection(m); }
static void mutex_lock(wrtc_mutex_t *m)    { EnterCriticalSection(m); }
static void mutex_unlock(wrtc_mutex_t *m)  { LeaveCriticalSection(m); }
static void mutex_destroy(wrtc_mutex_t *m) { DeleteCriticalSection(m); }

static void thread_start(void (*fn)(void *), void *arg) {
    _beginthread((_beginthread_proc_type)fn, 0, arg);
}

#  define SLEEP_MS(ms) Sleep(ms)

#else /* POSIX */

#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
#  include <pthread.h>

typedef int  sock_t;
#  define INVALID_SOCK (-1)

static void sock_close(sock_t s) { close(s); }

typedef pthread_mutex_t wrtc_mutex_t;
static void mutex_init(wrtc_mutex_t *m)    { pthread_mutex_init(m,NULL); }
static void mutex_lock(wrtc_mutex_t *m)    { pthread_mutex_lock(m); }
static void mutex_unlock(wrtc_mutex_t *m)  { pthread_mutex_unlock(m); }
static void mutex_destroy(wrtc_mutex_t *m) { pthread_mutex_destroy(m); }

static void thread_start(void (*fn)(void *), void *arg) {
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &a, (void *(*)(void *))fn, arg);
    pthread_attr_destroy(&a);
}

#  define SLEEP_MS(ms) usleep((ms)*1000)

#endif /* _WIN32 */

/* ------------------------------------------------------------------ */
/* FFmpeg (Opus encode + SWR resample)                                 */
/* ------------------------------------------------------------------ */

#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>

/* ------------------------------------------------------------------ */
/* Constants                                                            */
/* ------------------------------------------------------------------ */

#define RTP_MAX_PAYLOAD  1200   /* max bytes per RTP payload */

/*
 * Fixed RTP payload types — must match mediaCodecs in server.js.
 */
#define H264_PT          96
#define OPUS_PT          111

/*
 * Fixed SSRCs — must match the producer encodings in server.js.
 * Any non-zero values work; mediasoup matches on SSRC.
 */
#define VIDEO_SSRC       0x00ABCDEF
#define AUDIO_SSRC       0x00FEDCBA

#define H264_CLOCK_RATE  90000  /* Hz */
#define OPUS_SAMPLE_RATE 48000  /* Hz (WebRTC requirement) */
#define OPUS_FRAME_SIZE  960    /* samples/channel at 48 kHz = 20 ms */
#define OPUS_CHANNELS    2

/* Per-channel sample accumulation buffer (covers several Opus frames) */
#define AUDIO_BUF_FRAMES 8
#define AUDIO_BUF_CAP    (OPUS_FRAME_SIZE * AUDIO_BUF_FRAMES)

/* One video frame offset at 60 fps in 90 kHz ticks (≈ 33 ms). */
#define H264_FRAME_TICKS (H264_CLOCK_RATE / 60)

/* Temporary stack buffer for one SWR resample call (per-channel samples) */
#define RESAMPLE_TMP_CAP (OPUS_FRAME_SIZE * 4)

/* Max seconds to wait for mediasoup server before giving up retries */
#define CONNECT_MAX_TRIES 120

/* ------------------------------------------------------------------ */
/* State                                                                */
/* ------------------------------------------------------------------ */

struct webrtc_output {
    int           mediasoup_port;
    wrtc_mutex_t  lock;
    volatile bool running;
    bool          ready;           /* true once UDP sockets are connected */

    /* UDP sockets for plain RTP → mediasoup PlainTransport */
    sock_t        video_sock;
    sock_t        audio_sock;

    /* RTP state */
    uint16_t      video_seq;
    uint16_t      audio_seq;

    /* Opus encoder (FFmpeg) */
    const AVCodec   *opus_codec;
    AVCodecContext  *opus_ctx;
    AVFrame         *opus_frame;
    AVPacket        *opus_pkt;

    /* SWR resampler: input rate → 48 kHz */
    struct SwrContext *swr;
    int               swr_in_rate;
    int               swr_in_ch;

    /* Per-channel audio accumulation buffer at 48 kHz (interleaved stereo) */
    float    audio_buf[AUDIO_BUF_CAP * OPUS_CHANNELS];
    int      audio_buf_n;     /* samples buffered PER CHANNEL */
    int64_t  audio_rtp_ts;   /* next Opus RTP timestamp (samples @ 48 kHz) */

    /* Keyframe cache: last IDR frame (SPS+PPS+IDR, Annex-B) for reconnects */
    uint8_t *keyframe_cache;
    size_t   keyframe_cache_size;
    bool     needs_keyframe;  /* inject cached KF before next P-frame */
};

/* ------------------------------------------------------------------ */
/* Simple HTTP GET helper (for /rtp-params)                            */
/* ------------------------------------------------------------------ */

/*
 * Do a blocking HTTP/1.0 GET to http://127.0.0.1:port/rtp-params.
 * Parses the JSON response for "videoPort" and "audioPort" integer values.
 * Returns true and fills *video_port / *audio_port on success.
 */
static bool http_get_rtp_params(int port, int *video_port, int *audio_port)
{
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    sock_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCK) return false;

    /* 3-second connect timeout */
#ifdef _WIN32
    DWORD tv_ms = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv_ms, sizeof(tv_ms));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv_ms, sizeof(tv_ms));
#else
    struct timeval tv = {3, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* 127.0.0.1 */

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        sock_close(s);
        return false;
    }

    const char *req =
        "GET /rtp-params HTTP/1.0\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: close\r\n"
        "\r\n";
    if (send(s, req, (int)strlen(req), 0) < 0) {
        sock_close(s);
        return false;
    }

    /* Read the full response (at most 2 KB) */
    char buf[2048];
    int  total = 0;
    int  r;
    while (total < (int)(sizeof(buf) - 1) &&
           (r = (int)recv(s, buf + total, sizeof(buf) - 1 - (size_t)total, 0)) > 0)
        total += r;
    sock_close(s);

    if (total <= 0) return false;
    buf[total] = '\0';

    /* Skip HTTP headers */
    char *body = strstr(buf, "\r\n\r\n");
    if (!body) return false;
    body += 4;

    /* Minimal JSON extraction: find "videoPort": N and "audioPort": N */
    const char *vp = strstr(body, "\"videoPort\"");
    const char *ap = strstr(body, "\"audioPort\"");
    if (!vp || !ap) return false;

    vp = strchr(vp, ':');
    ap = strchr(ap, ':');
    if (!vp || !ap) return false;

    int v = atoi(vp + 1);
    int a = atoi(ap + 1);
    if (v <= 0 || a <= 0) return false;

    *video_port = v;
    *audio_port = a;
    return true;
}

/* ------------------------------------------------------------------ */
/* RTP header builder                                                   */
/* ------------------------------------------------------------------ */

static void rtp_write_header(uint8_t *buf, int pt, bool marker,
                              uint16_t seq, uint32_t ts, uint32_t ssrc)
{
    buf[0]  = 0x80;
    buf[1]  = (uint8_t)((pt & 0x7F) | (marker ? 0x80 : 0x00));
    buf[2]  = (uint8_t)(seq >> 8);
    buf[3]  = (uint8_t)(seq);
    buf[4]  = (uint8_t)(ts >> 24);
    buf[5]  = (uint8_t)(ts >> 16);
    buf[6]  = (uint8_t)(ts >>  8);
    buf[7]  = (uint8_t)(ts);
    buf[8]  = (uint8_t)(ssrc >> 24);
    buf[9]  = (uint8_t)(ssrc >> 16);
    buf[10] = (uint8_t)(ssrc >>  8);
    buf[11] = (uint8_t)(ssrc);
}

/* ------------------------------------------------------------------ */
/* H.264 RTP packetisation — RFC 6184                                  */
/* ------------------------------------------------------------------ */

/*
 * Return true if the Annex-B buffer contains at least one IDR NAL unit.
 */
static bool h264_has_idr(const uint8_t *data, size_t size)
{
    for (size_t i = 0; i + 2 < size; ) {
        int sc = 0;
        if (i + 3 < size &&
            data[i]==0 && data[i+1]==0 && data[i+2]==0 && data[i+3]==1)
            sc = 4;
        else if (data[i]==0 && data[i+1]==0 && data[i+2]==1)
            sc = 3;
        if (sc) {
            size_t nal_start = i + (size_t)sc;
            if (nal_start < size && (data[nal_start] & 0x1F) == 5)
                return true;
            i = nal_start;
        } else {
            i++;
        }
    }
    return false;
}

/*
 * Send one raw NAL unit (without Annex-B start code) as RTP.
 * Uses Single NAL Unit Packet if size <= RTP_MAX_PAYLOAD,
 * otherwise FU-A fragmentation.
 * Called with out->lock held.
 */
static void rtp_send_nal(struct webrtc_output *out,
                         const uint8_t *nal, int nal_size,
                         uint32_t ts, bool marker)
{
    if (nal_size <= 0 || out->video_sock == INVALID_SOCK) return;

    uint8_t pkt[12 + 2 + RTP_MAX_PAYLOAD];

    if (nal_size <= RTP_MAX_PAYLOAD) {
        /* Single NAL Unit Packet */
        rtp_write_header(pkt, H264_PT, marker, out->video_seq++,
                         ts, (uint32_t)VIDEO_SSRC);
        memcpy(pkt + 12, nal, (size_t)nal_size);
        send(out->video_sock, (const char *)pkt, 12 + nal_size, 0);
    } else {
        /* FU-A fragmentation */
        uint8_t fu_ind  = (nal[0] & 0xE0) | 28; /* NRI preserved, type=28 */
        uint8_t nal_typ = nal[0] & 0x1F;
        int     offset  = 1;  /* skip original NAL header byte */
        bool    first   = true;

        while (offset < nal_size) {
            int  chunk     = nal_size - offset;
            bool last_frag = (chunk <= RTP_MAX_PAYLOAD - 2);
            if (chunk > RTP_MAX_PAYLOAD - 2)
                chunk = RTP_MAX_PAYLOAD - 2;

            bool m = last_frag && marker;
            rtp_write_header(pkt, H264_PT, m, out->video_seq++,
                             ts, (uint32_t)VIDEO_SSRC);
            pkt[12] = fu_ind;
            pkt[13] = nal_typ
                    | (first     ? 0x80 : 0x00)  /* S bit */
                    | (last_frag ? 0x40 : 0x00);  /* E bit */
            memcpy(pkt + 14, nal + offset, (size_t)chunk);
            send(out->video_sock, (const char *)pkt, 14 + chunk, 0);

            offset += chunk;
            first   = false;
        }
    }
}

/*
 * Parse an H.264 Annex-B buffer into individual NAL units and send each
 * via plain RTP to the mediasoup PlainTransport.
 * Called with out->lock held.
 */
static void rtp_send_h264(struct webrtc_output *out,
                           const uint8_t *data, size_t size,
                           uint32_t ts)
{
    if (!out->ready || out->video_sock == INVALID_SOCK) return;

    size_t  starts[256];
    uint8_t sc_lens[256];
    int     nals = 0;

    for (size_t i = 0; i + 2 < size && nals < 256; ) {
        int sc = 0;
        if (i + 3 < size &&
            data[i]==0 && data[i+1]==0 && data[i+2]==0 && data[i+3]==1) {
            sc = 4;
        } else if (data[i]==0 && data[i+1]==0 && data[i+2]==1) {
            sc = 3;
        }
        if (sc) {
            starts[nals]   = i + (size_t)sc;
            sc_lens[nals]  = (uint8_t)sc;
            nals++;
            i += (size_t)sc;
        } else {
            i++;
        }
    }

    for (int n = 0; n < nals; n++) {
        size_t s = starts[n];
        size_t e = (n + 1 < nals)
                   ? starts[n + 1] - sc_lens[n + 1]
                   : size;
        while (e > s && data[e - 1] == 0)
            e--;
        if (e <= s) continue;
        rtp_send_nal(out, data + s, (int)(e - s), ts, (n == nals - 1));
    }
}

/* ------------------------------------------------------------------ */
/* Opus encoder helpers                                                 */
/* ------------------------------------------------------------------ */

static bool opus_encoder_init(struct webrtc_output *out)
{
    out->opus_codec = avcodec_find_encoder_by_name("libopus");
    if (!out->opus_codec)
        out->opus_codec = avcodec_find_encoder(AV_CODEC_ID_OPUS);
    if (!out->opus_codec) {
        fprintf(stderr, "[WebRTC] No Opus encoder available\n");
        return false;
    }

    out->opus_ctx = avcodec_alloc_context3(out->opus_codec);
    if (!out->opus_ctx) return false;

    out->opus_ctx->sample_rate = OPUS_SAMPLE_RATE;
    out->opus_ctx->sample_fmt  = AV_SAMPLE_FMT_FLT;
    av_channel_layout_default(&out->opus_ctx->ch_layout, OPUS_CHANNELS);
    out->opus_ctx->bit_rate   = 64000;
    out->opus_ctx->frame_size = OPUS_FRAME_SIZE;
    av_opt_set(out->opus_ctx->priv_data, "application", "lowdelay", 0);

    if (avcodec_open2(out->opus_ctx, out->opus_codec, NULL) < 0) {
        fprintf(stderr, "[WebRTC] Failed to open Opus encoder\n");
        avcodec_free_context(&out->opus_ctx);
        return false;
    }

    out->opus_frame = av_frame_alloc();
    out->opus_pkt   = av_packet_alloc();
    if (!out->opus_frame || !out->opus_pkt) goto fail_enc;

    out->opus_frame->nb_samples  = OPUS_FRAME_SIZE;
    out->opus_frame->sample_rate = OPUS_SAMPLE_RATE;
    out->opus_frame->format      = AV_SAMPLE_FMT_FLT;
    av_channel_layout_copy(&out->opus_frame->ch_layout,
                           &out->opus_ctx->ch_layout);
    if (av_frame_get_buffer(out->opus_frame, 0) < 0) goto fail_enc;

    fprintf(stdout, "[WebRTC] Opus encoder: %s\n", out->opus_codec->name);
    return true;

fail_enc:
    av_frame_free(&out->opus_frame);
    av_packet_free(&out->opus_pkt);
    avcodec_free_context(&out->opus_ctx);
    return false;
}

static void opus_encoder_destroy(struct webrtc_output *out)
{
    if (out->swr)        swr_free(&out->swr);
    if (out->opus_frame) av_frame_free(&out->opus_frame);
    if (out->opus_pkt)   av_packet_free(&out->opus_pkt);
    if (out->opus_ctx)   avcodec_free_context(&out->opus_ctx);
}

/*
 * Ensure the SWR resampler is configured for (in_rate, in_ch) → 48 kHz stereo.
 * Called with out->lock held.
 */
static bool ensure_swr(struct webrtc_output *out, int in_rate, int in_ch)
{
    if (out->swr && out->swr_in_rate == in_rate && out->swr_in_ch == in_ch)
        return true;

    if (out->swr) swr_free(&out->swr);

    AVChannelLayout in_layout, out_layout;
    av_channel_layout_default(&in_layout,  in_ch);
    av_channel_layout_default(&out_layout, OPUS_CHANNELS);

    if (swr_alloc_set_opts2(&out->swr,
                             &out_layout, AV_SAMPLE_FMT_FLT, OPUS_SAMPLE_RATE,
                             &in_layout,  AV_SAMPLE_FMT_FLT, in_rate,
                             0, NULL) < 0 || !out->swr) {
        out->swr = NULL;
        return false;
    }
    if (swr_init(out->swr) < 0) {
        swr_free(&out->swr);
        return false;
    }
    out->swr_in_rate = in_rate;
    out->swr_in_ch   = in_ch;
    out->audio_buf_n = 0;
    return true;
}

/*
 * Encode one OPUS_FRAME_SIZE block from audio_buf and send as RTP.
 * Called with out->lock held.
 */
static void flush_opus_frame(struct webrtc_output *out)
{
    if (!out->opus_ctx || !out->opus_frame) return;

    memcpy(out->opus_frame->data[0],
           out->audio_buf,
           (size_t)(OPUS_FRAME_SIZE * OPUS_CHANNELS) * sizeof(float));
    out->opus_frame->pts = out->audio_rtp_ts;

    if (avcodec_send_frame(out->opus_ctx, out->opus_frame) < 0) goto shift;

    while (avcodec_receive_packet(out->opus_ctx, out->opus_pkt) == 0) {
        int     plen = out->opus_pkt->size;
        uint8_t pkt[12 + 1276];
        if (plen <= (int)(sizeof(pkt) - 12)) {
            rtp_write_header(pkt, OPUS_PT, false,
                             out->audio_seq++,
                             (uint32_t)out->audio_rtp_ts,
                             (uint32_t)AUDIO_SSRC);
            memcpy(pkt + 12, out->opus_pkt->data, (size_t)plen);
            if (out->audio_sock != INVALID_SOCK)
                send(out->audio_sock, (const char *)pkt, 12 + plen, 0);
        } else {
            fprintf(stderr,
                    "[WebRTC] Opus packet too large (%d bytes), dropping\n",
                    plen);
        }
        av_packet_unref(out->opus_pkt);
    }

shift:
    out->audio_rtp_ts += OPUS_FRAME_SIZE;

    int remain = out->audio_buf_n - OPUS_FRAME_SIZE;
    if (remain > 0) {
        memmove(out->audio_buf,
                out->audio_buf + (size_t)(OPUS_FRAME_SIZE * OPUS_CHANNELS),
                (size_t)(remain * OPUS_CHANNELS) * sizeof(float));
    }
    out->audio_buf_n = (remain > 0) ? remain : 0;
}

/* ------------------------------------------------------------------ */
/* Background connection thread                                         */
/* ------------------------------------------------------------------ */

/*
 * Polls the mediasoup server for its RTP port allocation, then creates
 * and connects UDP sockets.  Exits once connected or the output is stopped.
 */
static void connect_thread(void *arg)
{
    struct webrtc_output *out = (struct webrtc_output *)arg;

    fprintf(stdout,
            "[WebRTC] Waiting for mediasoup server on port %d …\n",
            out->mediasoup_port);

    for (int tries = 0; out->running; tries++) {
        if (tries > 0) SLEEP_MS(1000);
        if (tries >= CONNECT_MAX_TRIES) {
            fprintf(stderr,
                    "[WebRTC] mediasoup server not reachable after %d seconds"
                    " — still retrying\n",
                    tries);
            /* Reset counter to keep retrying indefinitely */
            tries = 0;
        }

        int vport = 0, aport = 0;
        if (!http_get_rtp_params(out->mediasoup_port, &vport, &aport))
            continue;

        /* Create UDP sockets */
        sock_t vs = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sock_t as = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (vs == INVALID_SOCK || as == INVALID_SOCK) {
            if (vs != INVALID_SOCK) sock_close(vs);
            if (as != INVALID_SOCK) sock_close(as);
            continue;
        }

        struct sockaddr_in va, aa;
        memset(&va, 0, sizeof(va));
        va.sin_family      = AF_INET;
        va.sin_port        = htons((unsigned short)vport);
        va.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        memset(&aa, 0, sizeof(aa));
        aa.sin_family      = AF_INET;
        aa.sin_port        = htons((unsigned short)aport);
        aa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        if (connect(vs, (struct sockaddr *)&va, sizeof(va)) != 0 ||
            connect(as, (struct sockaddr *)&aa, sizeof(aa)) != 0) {
            sock_close(vs);
            sock_close(as);
            continue;
        }

        mutex_lock(&out->lock);
        /* Close any previously opened sockets */
        if (out->video_sock != INVALID_SOCK) sock_close(out->video_sock);
        if (out->audio_sock != INVALID_SOCK) sock_close(out->audio_sock);
        out->video_sock   = vs;
        out->audio_sock   = as;
        /* Reset audio state for fresh connection */
        out->audio_rtp_ts = 0;
        out->audio_buf_n  = 0;
        if (out->opus_ctx) avcodec_flush_buffers(out->opus_ctx);
        out->ready = true;
        mutex_unlock(&out->lock);

        fprintf(stdout,
                "[WebRTC] Connected to mediasoup — video UDP port %d,"
                " audio UDP port %d\n",
                vport, aport);
        return; /* done */
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

struct webrtc_output *webrtc_output_create(int mediasoup_port)
{
    struct webrtc_output *out =
        (struct webrtc_output *)calloc(1, sizeof(struct webrtc_output));
    if (!out) return NULL;

    out->mediasoup_port = mediasoup_port;
    out->running        = true;
    out->video_sock     = INVALID_SOCK;
    out->audio_sock     = INVALID_SOCK;

    mutex_init(&out->lock);

    if (!opus_encoder_init(out)) {
        mutex_destroy(&out->lock);
        free(out);
        return NULL;
    }

    /* Start background thread to connect to mediasoup */
    thread_start(connect_thread, out);

    return out;
}

void webrtc_output_destroy(struct webrtc_output *out)
{
    if (!out) return;

    out->running = false;

    mutex_lock(&out->lock);
    if (out->video_sock != INVALID_SOCK) {
        sock_close(out->video_sock);
        out->video_sock = INVALID_SOCK;
    }
    if (out->audio_sock != INVALID_SOCK) {
        sock_close(out->audio_sock);
        out->audio_sock = INVALID_SOCK;
    }
    out->ready = false;
    mutex_unlock(&out->lock);

    opus_encoder_destroy(out);
    free(out->keyframe_cache);
    mutex_destroy(&out->lock);
    free(out);
}

void webrtc_output_write_video(struct webrtc_output *out,
                               const uint8_t *data, size_t size,
                               int64_t pts_us)
{
    if (!out || !data || size == 0) return;

    mutex_lock(&out->lock);

    /* Always cache the most recent IDR frame so that when the AirPlay
     * stream restarts (webrtc_output_request_keyframe is called), an
     * up-to-date keyframe is available for immediate injection. */
    bool is_idr = h264_has_idr(data, size);
    if (is_idr) {
        uint8_t *new_cache = (uint8_t *)malloc(size);
        if (new_cache) {
            free(out->keyframe_cache);
            out->keyframe_cache      = new_cache;
            out->keyframe_cache_size = size;
            memcpy(out->keyframe_cache, data, size);
        } else {
            fprintf(stderr,
                    "[WebRTC] Warning: keyframe cache allocation failed"
                    " (%zu bytes)\n", size);
        }
    }

    if (out->ready && out->video_sock != INVALID_SOCK) {
        uint32_t ts = (uint32_t)
            ((pts_us * (int64_t)H264_CLOCK_RATE) / 1000000LL);

        if (is_idr)
            out->needs_keyframe = false;

        /* Inject cached keyframe before the first non-IDR frame after a
         * stream restart, so existing mediasoup consumers see video
         * immediately without waiting for the next natural IDR. */
        if (out->needs_keyframe && out->keyframe_cache &&
            out->keyframe_cache_size > 0) {
            uint32_t kf_ts = (ts >= H264_FRAME_TICKS)
                             ? ts - H264_FRAME_TICKS : 0;
            rtp_send_h264(out, out->keyframe_cache, out->keyframe_cache_size,
                          kf_ts);
            out->needs_keyframe = false;
        }

        rtp_send_h264(out, data, size, ts);
    }
    mutex_unlock(&out->lock);
}

void webrtc_output_request_keyframe(struct webrtc_output *out)
{
    if (!out) return;
    mutex_lock(&out->lock);
    if (out->ready)
        out->needs_keyframe = true;
    mutex_unlock(&out->lock);
}

void webrtc_output_write_audio(struct webrtc_output *out,
                               const float *pcm, int samples,
                               int channels, int sample_rate,
                               int64_t pts_us)
{
    (void)pts_us;

    if (!out || !pcm || samples <= 0) return;

    mutex_lock(&out->lock);

    if (!out->ready || out->audio_sock == INVALID_SOCK) {
        mutex_unlock(&out->lock);
        return;
    }

    if (!ensure_swr(out, sample_rate, channels)) {
        mutex_unlock(&out->lock);
        return;
    }

    float   tmp[RESAMPLE_TMP_CAP * OPUS_CHANNELS];
    int     out_max = RESAMPLE_TMP_CAP;

    const uint8_t *in_ptr  = (const uint8_t *)pcm;
    uint8_t       *out_ptr = (uint8_t *)tmp;
    int converted = swr_convert(out->swr, &out_ptr, out_max,
                                &in_ptr, samples);
    if (converted > 0) {
        int space = AUDIO_BUF_CAP - out->audio_buf_n;
        int copy  = (converted < space) ? converted : space;
        if (copy < converted)
            fprintf(stderr,
                    "[WebRTC] Audio buffer full: dropped %d samples\n",
                    converted - copy);
        memcpy(out->audio_buf + (size_t)(out->audio_buf_n * OPUS_CHANNELS),
               tmp,
               (size_t)(copy * OPUS_CHANNELS) * sizeof(float));
        out->audio_buf_n += copy;

        while (out->audio_buf_n >= OPUS_FRAME_SIZE)
            flush_opus_frame(out);
    }

    mutex_unlock(&out->lock);
}
