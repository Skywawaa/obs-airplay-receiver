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
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <process.h>
#  include <debugapi.h>

typedef SOCKET sock_t;
#  define INVALID_SOCK  INVALID_SOCKET

static void sock_close(sock_t s) { closesocket(s); }

static void sock_set_nonblocking(sock_t s)
{
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
}

typedef CRITICAL_SECTION wrtc_mutex_t;
static void mutex_init(wrtc_mutex_t *m)    { InitializeCriticalSection(m); }
static void mutex_lock(wrtc_mutex_t *m)    { EnterCriticalSection(m); }
static void mutex_unlock(wrtc_mutex_t *m)  { LeaveCriticalSection(m); }
static void mutex_destroy(wrtc_mutex_t *m) { DeleteCriticalSection(m); }

struct thread_trampoline {
    void (*fn)(void *);
    void *arg;
};

static void __cdecl thread_entry(void *raw)
{
    struct thread_trampoline *t = (struct thread_trampoline *)raw;
    void (*fn)(void *) = t->fn;
    void *arg          = t->arg;
    free(t);

    fprintf(stderr, "[WebRTC] thread_wrapper: before __try\n");
    fflush(stderr);
    __try {
        fprintf(stderr, "[WebRTC] thread_wrapper: calling fn\n");
        fflush(stderr);
        fn(arg);
        fprintf(stderr, "[WebRTC] thread_wrapper: fn returned\n");
        fflush(stderr);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        fprintf(stderr, "[ERROR] Exception in background thread: code=0x%08lx\n",
                GetExceptionCode());
        fflush(stderr);
    }
}

static void thread_start(void (*fn)(void *), void *arg)
{
    struct thread_trampoline *t = malloc(sizeof(*t));
    if (!t) {
        fprintf(stderr, "[ERROR] thread_start: out of memory\n");
        return;
    }
    t->fn  = fn;
    t->arg = arg;
    uintptr_t rc = _beginthread(thread_entry, 0, t);
    if (rc == (uintptr_t)(-1L)) {
        fprintf(stderr, "[ERROR] _beginthread failed\n");
        free(t);
    }
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

static void sock_set_nonblocking(sock_t s)
{
    int flags = fcntl(s, F_GETFL, 0);
    if (flags >= 0)
        fcntl(s, F_SETFL, flags | O_NONBLOCK);
}

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
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

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

/* New viewers may join between packets or miss the first reinjected IDR.
 * Send a short bounded burst of cached IDRs after a viewer request so the
 * decoder has a better chance to lock without a permanent periodic flash. */
#define KEYFRAME_BURST_COUNT       1
#define KEYFRAME_BURST_INTERVAL_US 200000

/* Temporary stack buffer for one SWR resample call (per-channel samples) */
#define RESAMPLE_TMP_CAP (OPUS_FRAME_SIZE * 4)

/* Max seconds to wait for mediasoup server before giving up retries */
#define CONNECT_MAX_TRIES 120

/*
 * Maximum age for cached IDR reinjection.
 * If too old, reinjecting only the IDR (without all intermediate P-frames)
 * can produce macroblock noise/artifacts after reload. In that case, wait
 * for a natural fresh IDR from the source.
 */
#define KEYFRAME_CACHE_MAX_AGE_US 500000

#define TRANSCODE_TARGET_FPS          60
#define TRANSCODE_GOP_FRAMES          30
#define TRANSCODE_BITRATE_BPS   4000000

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
    bool            opus_disabled;
    enum AVSampleFormat opus_sample_fmt;  /* actual fmt after avcodec_open2 */

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
    int64_t  keyframe_cache_pts_us;
    bool     needs_keyframe;  /* inject cached KF before next P-frame */
    int      pending_keyframe_burst;
    int64_t  next_keyframe_burst_pts_us;

    uint64_t rtcp_keyframe_req_count;
    uint64_t injected_keyframe_count;
    uint64_t idr_received_count;
    int64_t  last_idr_pts_us;

    webrtc_video_mode_t video_mode;
    webrtc_video_encoder_preference_t video_encoder_preference;
    const AVCodec *selected_video_encoder;
    bool transcode_warning_logged;

    /* Optional transcode pipeline (decode incoming H264 then re-encode). */
    const AVCodec *video_dec_codec;
    AVCodecContext *video_dec_ctx;
    AVFrame *video_dec_frame;

    AVCodecContext *video_enc_ctx;
    AVFrame *video_enc_frame;
    AVPacket *video_enc_pkt;
    struct SwsContext *video_sws;
    enum AVPixelFormat video_src_fmt;
    bool transcode_active;
    int64_t transcode_frame_index;
};

/* Forward declarations for helpers referenced by transcode code before
 * the concrete RFC6184 packetizer definitions below. */
static void rtp_send_nal(struct webrtc_output *out,
                         const uint8_t *nal, int nal_size,
                         uint32_t ts, bool marker);
static void rtp_send_h264(struct webrtc_output *out,
                          const uint8_t *data, size_t size,
                          uint32_t ts);

static const char *video_mode_name(webrtc_video_mode_t mode)
{
    switch (mode) {
    case WEBRTC_VIDEO_MODE_TRANSCODE_AUTO: return "transcode-auto";
    case WEBRTC_VIDEO_MODE_PASSTHROUGH:
    default: return "passthrough";
    }
}

static const char *video_encoder_pref_name(webrtc_video_encoder_preference_t pref)
{
    switch (pref) {
    case WEBRTC_VIDEO_ENCODER_NVENC: return "nvenc";
    case WEBRTC_VIDEO_ENCODER_QSV: return "qsv";
    case WEBRTC_VIDEO_ENCODER_AMF: return "amf";
    case WEBRTC_VIDEO_ENCODER_VIDEOTOOLBOX: return "videotoolbox";
    case WEBRTC_VIDEO_ENCODER_LIBX264: return "libx264";
    case WEBRTC_VIDEO_ENCODER_SOFTWARE: return "software";
    case WEBRTC_VIDEO_ENCODER_AUTO:
    default: return "auto";
    }
}

static const AVCodec *find_video_encoder_by_preference(
    webrtc_video_encoder_preference_t pref)
{
    switch (pref) {
    case WEBRTC_VIDEO_ENCODER_NVENC:
        return avcodec_find_encoder_by_name("h264_nvenc");
    case WEBRTC_VIDEO_ENCODER_QSV:
        return avcodec_find_encoder_by_name("h264_qsv");
    case WEBRTC_VIDEO_ENCODER_AMF:
        return avcodec_find_encoder_by_name("h264_amf");
    case WEBRTC_VIDEO_ENCODER_VIDEOTOOLBOX:
        return avcodec_find_encoder_by_name("h264_videotoolbox");
    case WEBRTC_VIDEO_ENCODER_LIBX264:
        return avcodec_find_encoder_by_name("libx264");
    case WEBRTC_VIDEO_ENCODER_SOFTWARE:
        return avcodec_find_encoder(AV_CODEC_ID_H264);
    case WEBRTC_VIDEO_ENCODER_AUTO:
    default: {
        const AVCodec *enc = avcodec_find_encoder_by_name("h264_nvenc");
        if (enc) return enc;
        enc = avcodec_find_encoder_by_name("h264_qsv");
        if (enc) return enc;
        enc = avcodec_find_encoder_by_name("h264_amf");
        if (enc) return enc;
        enc = avcodec_find_encoder_by_name("h264_videotoolbox");
        if (enc) return enc;
        enc = avcodec_find_encoder_by_name("libx264");
        if (enc) return enc;
        return avcodec_find_encoder(AV_CODEC_ID_H264);
    }
    }
}

static bool encoder_supports_pix_fmt(const AVCodec *codec,
                                     enum AVPixelFormat fmt)
{
    if (!codec || !codec->pix_fmts)
        return true;
    for (const enum AVPixelFormat *p = codec->pix_fmts;
         *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == fmt)
            return true;
    }
    return false;
}

static enum AVPixelFormat pick_encoder_pix_fmt(const AVCodec *codec,
                                               enum AVPixelFormat src_fmt)
{
    if (!codec)
        return src_fmt;
    if (encoder_supports_pix_fmt(codec, src_fmt))
        return src_fmt;
    if (codec->pix_fmts && codec->pix_fmts[0] != AV_PIX_FMT_NONE)
        return codec->pix_fmts[0];
    return AV_PIX_FMT_YUV420P;
}

static bool transcode_init_decoder(struct webrtc_output *out)
{
    out->video_dec_codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!out->video_dec_codec) {
        fprintf(stderr, "[WebRTC] No H264 decoder available for transcode mode\n");
        return false;
    }

    out->video_dec_ctx = avcodec_alloc_context3(out->video_dec_codec);
    if (!out->video_dec_ctx)
        return false;

    if (avcodec_open2(out->video_dec_ctx, out->video_dec_codec, NULL) < 0) {
        fprintf(stderr, "[WebRTC] Failed to open H264 decoder for transcode mode\n");
        return false;
    }

    out->video_dec_frame = av_frame_alloc();
    return out->video_dec_frame != NULL;
}

static bool transcode_init_encoder_for_frame(struct webrtc_output *out,
                                             const AVFrame *src_frame)
{
    if (!out->selected_video_encoder || !src_frame)
        return false;

    out->video_enc_ctx = avcodec_alloc_context3(out->selected_video_encoder);
    if (!out->video_enc_ctx)
        return false;

    enum AVPixelFormat enc_fmt = pick_encoder_pix_fmt(
        out->selected_video_encoder,
        (enum AVPixelFormat)src_frame->format);

    out->video_src_fmt = (enum AVPixelFormat)src_frame->format;
    out->video_enc_ctx->width  = src_frame->width;
    out->video_enc_ctx->height = src_frame->height;
    out->video_enc_ctx->pix_fmt = enc_fmt;
    out->video_enc_ctx->time_base = (AVRational){1, TRANSCODE_TARGET_FPS};
    out->video_enc_ctx->framerate = (AVRational){TRANSCODE_TARGET_FPS, 1};
    out->video_enc_ctx->gop_size = TRANSCODE_GOP_FRAMES;
    out->video_enc_ctx->max_b_frames = 0;
    out->video_enc_ctx->bit_rate = TRANSCODE_BITRATE_BPS;

    av_opt_set(out->video_enc_ctx->priv_data, "preset", "p4", 0);
    av_opt_set(out->video_enc_ctx->priv_data, "tune", "ll", 0);
    av_opt_set(out->video_enc_ctx->priv_data, "rc", "cbr", 0);
    av_opt_set(out->video_enc_ctx->priv_data, "forced-idr", "1", 0);
    av_opt_set(out->video_enc_ctx->priv_data, "annexb", "1", 0);

    if (avcodec_open2(out->video_enc_ctx, out->selected_video_encoder, NULL) < 0) {
        fprintf(stderr, "[WebRTC] Failed to open video encoder %s\n",
                out->selected_video_encoder->name);
        return false;
    }

    out->video_enc_pkt = av_packet_alloc();
    if (!out->video_enc_pkt)
        return false;

    if (out->video_src_fmt != out->video_enc_ctx->pix_fmt) {
        out->video_sws = sws_getContext(
            src_frame->width,
            src_frame->height,
            out->video_src_fmt,
            out->video_enc_ctx->width,
            out->video_enc_ctx->height,
            out->video_enc_ctx->pix_fmt,
            SWS_FAST_BILINEAR,
            NULL,
            NULL,
            NULL);
        if (!out->video_sws) {
            fprintf(stderr, "[WebRTC] Failed to create swscale context\n");
            return false;
        }

        out->video_enc_frame = av_frame_alloc();
        if (!out->video_enc_frame)
            return false;
        out->video_enc_frame->format = out->video_enc_ctx->pix_fmt;
        out->video_enc_frame->width  = out->video_enc_ctx->width;
        out->video_enc_frame->height = out->video_enc_ctx->height;
        if (av_frame_get_buffer(out->video_enc_frame, 32) < 0)
            return false;
    }

    out->transcode_frame_index = 0;
    out->transcode_active = true;

    fprintf(stdout,
            "[WebRTC] Transcode initialized: %s %dx%d pix_fmt=%d gop=%d\n",
            out->selected_video_encoder->name,
            out->video_enc_ctx->width,
            out->video_enc_ctx->height,
            (int)out->video_enc_ctx->pix_fmt,
            out->video_enc_ctx->gop_size);

    return true;
}

static void transcode_destroy(struct webrtc_output *out)
{
    if (!out) return;
    if (out->video_sws) {
        sws_freeContext(out->video_sws);
        out->video_sws = NULL;
    }
    if (out->video_enc_frame) {
        av_frame_free(&out->video_enc_frame);
    }
    if (out->video_enc_pkt) {
        av_packet_free(&out->video_enc_pkt);
    }
    if (out->video_enc_ctx) {
        avcodec_free_context(&out->video_enc_ctx);
    }
    if (out->video_dec_frame) {
        av_frame_free(&out->video_dec_frame);
    }
    if (out->video_dec_ctx) {
        avcodec_free_context(&out->video_dec_ctx);
    }
    out->transcode_active = false;
}

static void rtp_send_h264_access_unit(struct webrtc_output *out,
                                      const uint8_t *data,
                                      size_t size,
                                      uint32_t ts)
{
    if (!out || !data || size == 0)
        return;

    if ((size >= 4 && data[0] == 0 && data[1] == 0 &&
         ((data[2] == 1) || (data[2] == 0 && data[3] == 1)))) {
        rtp_send_h264(out, data, size, ts);
        return;
    }

    /* AVCC packet fallback (length-prefixed NAL units, 4-byte lengths). */
    size_t off = 0;
    while (off + 4 <= size) {
        uint32_t nalu_len = ((uint32_t)data[off] << 24) |
                            ((uint32_t)data[off + 1] << 16) |
                            ((uint32_t)data[off + 2] << 8) |
                            (uint32_t)data[off + 3];
        off += 4;
        if (nalu_len == 0 || off + nalu_len > size)
            break;
        bool marker = (off + nalu_len == size);
        rtp_send_nal(out, data + off, (int)nalu_len, ts, marker);
        off += nalu_len;
    }
}

static void transcode_process_video(struct webrtc_output *out,
                                    const uint8_t *data,
                                    size_t size,
                                    int64_t pts_us)
{
    AVPacket in_pkt;
    memset(&in_pkt, 0, sizeof(in_pkt));
    in_pkt.data = (uint8_t *)data;
    in_pkt.size = (int)size;

    if (avcodec_send_packet(out->video_dec_ctx, &in_pkt) < 0)
        return;

    while (avcodec_receive_frame(out->video_dec_ctx, out->video_dec_frame) == 0) {
        AVFrame *enc_input = out->video_dec_frame;

        if (!out->transcode_active) {
            if (!transcode_init_encoder_for_frame(out, out->video_dec_frame)) {
                if (!out->transcode_warning_logged) {
                    out->transcode_warning_logged = true;
                    fprintf(stderr,
                            "[WebRTC] Transcode init failed, falling back to passthrough\n");
                }
                out->video_mode = WEBRTC_VIDEO_MODE_PASSTHROUGH;
                return;
            }
        }

        if (out->video_sws && out->video_enc_frame) {
            sws_scale(out->video_sws,
                      (const uint8_t * const *)out->video_dec_frame->data,
                      out->video_dec_frame->linesize,
                      0,
                      out->video_dec_ctx->height,
                      out->video_enc_frame->data,
                      out->video_enc_frame->linesize);
            enc_input = out->video_enc_frame;
        }

        enc_input->pts = out->transcode_frame_index++;
        if (out->needs_keyframe) {
            enc_input->pict_type = AV_PICTURE_TYPE_I;
            enc_input->key_frame = 1;
            out->needs_keyframe = false;
        } else {
            enc_input->pict_type = AV_PICTURE_TYPE_NONE;
            enc_input->key_frame = 0;
        }

        if (avcodec_send_frame(out->video_enc_ctx, enc_input) < 0)
            continue;

        while (avcodec_receive_packet(out->video_enc_ctx, out->video_enc_pkt) == 0) {
            uint32_t ts = (uint32_t)
                ((pts_us * (int64_t)H264_CLOCK_RATE) / 1000000LL);
            rtp_send_h264_access_unit(out,
                                      out->video_enc_pkt->data,
                                      (size_t)out->video_enc_pkt->size,
                                      ts);
            av_packet_unref(out->video_enc_pkt);
        }
    }
}

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
    if (v <= 0 || a <= 0) {
        fprintf(stdout, "[WebRTC] invalid ports v=%d a=%d\n", v, a);
        return false;
    }

    *video_port = v;
    *audio_port = a;
    return true;
}

/* ------------------------------------------------------------------ */
/* RTP header builder                                                   */
/* ------------------------------------------------------------------ */

/*
 * Poll GET /keyframe-needed on the mediasoup HTTP server.
 * Returns true if a new viewer is waiting for a keyframe.
 */
static bool http_get_keyframe_needed(int port)
{
    sock_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCK) return false;

#ifdef _WIN32
    DWORD tv_ms = 2000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv_ms, sizeof(tv_ms));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv_ms, sizeof(tv_ms));
#else
    struct timeval tv = {2, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        sock_close(s);
        return false;
    }

    const char *req =
        "GET /keyframe-needed HTTP/1.0\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: close\r\n"
        "\r\n";
    if (send(s, req, (int)strlen(req), 0) < 0) {
        sock_close(s);
        return false;
    }

    char buf[512];
    int  total = 0, r;
    while (total < (int)(sizeof(buf) - 1) &&
           (r = (int)recv(s, buf + total,
                          sizeof(buf) - 1 - (size_t)total, 0)) > 0)
        total += r;
    sock_close(s);

    if (total <= 0) return false;
    buf[total] = '\0';

    char *body = strstr(buf, "\r\n\r\n");
    if (!body) return false;
    body += 4;

    return strstr(body, "\"needed\":true")   != NULL ||
           strstr(body, "\"needed\": true")  != NULL;
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

/*
 * Drain incoming RTCP packets from the video socket and detect keyframe
 * feedback messages (PSFB PLI/FIR). Returns true if any request is found.
 * Called with out->lock held.
 */
static bool video_sock_poll_keyframe_feedback(struct webrtc_output *out)
{
    if (!out || out->video_sock == INVALID_SOCK)
        return false;

    uint8_t buf[2048];
    bool requested = false;

    for (;;) {
        int n = (int)recv(out->video_sock, (char *)buf, sizeof(buf), 0);
        if (n <= 0)
            break;

        size_t off = 0;
        while (off + 4 <= (size_t)n) {
            const uint8_t v_p_count = buf[off + 0];
            const uint8_t pt        = buf[off + 1];
            const uint16_t length_words =
                (uint16_t)(((uint16_t)buf[off + 2] << 8) | buf[off + 3]);
            const size_t pkt_len = ((size_t)length_words + 1U) * 4U;

            if (pkt_len < 4 || off + pkt_len > (size_t)n)
                break;

            /* RTCP PSFB (PT=206), FMT in low 5 bits of first byte.
             * PLI: FMT=1, FIR: FMT=4. */
            if (pt == 206) {
                const uint8_t fmt = (uint8_t)(v_p_count & 0x1F);
                if (fmt == 1 || fmt == 4)
                    requested = true;
            }

            off += pkt_len;
        }
    }

    return requested;
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
    out->opus_codec = avcodec_find_encoder(AV_CODEC_ID_OPUS);
    if (!out->opus_codec)
        out->opus_codec = avcodec_find_encoder_by_name("libopus");
    if (!out->opus_codec) {
        fprintf(stderr, "[WebRTC] No Opus encoder found\n");
        return false;
    }

    out->opus_ctx = avcodec_alloc_context3(out->opus_codec);
    if (!out->opus_ctx) return false;

    out->opus_ctx->sample_rate = OPUS_SAMPLE_RATE;
    out->opus_ctx->bit_rate    = 64000;
    av_channel_layout_default(&out->opus_ctx->ch_layout, OPUS_CHANNELS);

    out->opus_ctx->sample_fmt = AV_SAMPLE_FMT_FLT;

    fprintf(stdout, "[WebRTC] Opening Opus encoder (%s)...\n",
            out->opus_codec->name);
    fflush(stdout);

    if (avcodec_open2(out->opus_ctx, out->opus_codec, NULL) < 0) {
        fprintf(stderr, "[WebRTC] avcodec_open2 (Opus) failed\n");
        avcodec_free_context(&out->opus_ctx);
        return false;
    }

    out->opus_sample_fmt    = out->opus_ctx->sample_fmt;
    int actual_frame_size = out->opus_ctx->frame_size;

    fprintf(stdout, "[WebRTC] Opus open OK: fmt=%d frame_size=%d\n",
            (int)out->opus_sample_fmt, actual_frame_size);
    fflush(stdout);

    out->opus_frame = av_frame_alloc();
    out->opus_pkt   = av_packet_alloc();
    if (!out->opus_frame || !out->opus_pkt) {
        fprintf(stderr, "[WebRTC] av_frame/pkt alloc failed\n");
        goto fail;
    }

    out->opus_frame->nb_samples  = actual_frame_size;
    out->opus_frame->sample_rate = OPUS_SAMPLE_RATE;
    out->opus_frame->format      = (int)out->opus_sample_fmt;
    av_channel_layout_default(&out->opus_frame->ch_layout, OPUS_CHANNELS);

    if (av_frame_get_buffer(out->opus_frame, 0) < 0) {
        fprintf(stderr, "[WebRTC] av_frame_get_buffer (Opus) failed\n");
        goto fail;
    }

    fprintf(stdout, "[WebRTC] Opus encoder ready: %s\n", out->opus_codec->name);
    fflush(stdout);
    return true;

fail:
    if (out->opus_frame) av_frame_free(&out->opus_frame);
    if (out->opus_pkt)   av_packet_free(&out->opus_pkt);
    if (out->opus_ctx) {
        avcodec_send_frame(out->opus_ctx, NULL);
        avcodec_free_context(&out->opus_ctx);
    }
    return false;
}

    out->opus_ctx = avcodec_alloc_context3(out->opus_codec);
    if (!out->opus_ctx) return false;

    out->opus_ctx->sample_rate = OPUS_SAMPLE_RATE;
    out->opus_ctx->bit_rate    = 64000;
    av_channel_layout_default(&out->opus_ctx->ch_layout, OPUS_CHANNELS);

    out->opus_ctx->sample_fmt = AV_SAMPLE_FMT_FLT;

    fprintf(stdout, "[WebRTC] Opening Opus encoder (%s)...\n",
            out->opus_codec->name);
    fflush(stdout);

    if (avcodec_open2(out->opus_ctx, out->opus_codec, NULL) < 0) {
        fprintf(stderr, "[WebRTC] avcodec_open2 (Opus) failed\n");
        avcodec_free_context(&out->opus_ctx);
        return false;
    }

    out->opus_sample_fmt    = out->opus_ctx->sample_fmt;
    int actual_frame_size = out->opus_ctx->frame_size;

    fprintf(stdout, "[WebRTC] Opus open OK: fmt=%d frame_size=%d\n",
            (int)out->opus_sample_fmt, actual_frame_size);
    fflush(stdout);

    out->opus_frame = av_frame_alloc();
    out->opus_pkt   = av_packet_alloc();
    if (!out->opus_frame || !out->opus_pkt) {
        fprintf(stderr, "[WebRTC] av_frame/pkt alloc failed\n");
        goto fail;
    }

    out->opus_frame->nb_samples  = actual_frame_size;
    out->opus_frame->sample_rate = OPUS_SAMPLE_RATE;
    out->opus_frame->format      = (int)out->opus_sample_fmt;
    av_channel_layout_default(&out->opus_frame->ch_layout, OPUS_CHANNELS);

    int samples_bytes = actual_frame_size * OPUS_CHANNELS *
            av_get_bytes_per_sample(out->opus_sample_fmt);
    uint8_t *audio_data = av_malloc((size_t)samples_bytes);
    if (!audio_data) {
        fprintf(stderr, "[WebRTC] av_malloc for audio failed\n");
        goto fail;
    }
    out->opus_frame->data[0] = audio_data;
    out->opus_frame->buf[0] = av_buffer_create(audio_data, (size_t)samples_bytes,
            av_buffer_default_free, NULL, 0);

    fprintf(stdout, "[WebRTC] Opus encoder ready: %s\n", out->opus_codec->name);
    fflush(stdout);
    return true;

fail:
    if (out->opus_frame) av_frame_free(&out->opus_frame);
    if (out->opus_pkt)   av_packet_free(&out->opus_pkt);
    if (out->opus_ctx) {
        avcodec_send_frame(out->opus_ctx, NULL);
        avcodec_free_context(&out->opus_ctx);
    }
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
 * Encode one Opus frame block from audio_buf and send as RTP.
 * Called with out->lock held.
 */
static void flush_opus_frame(struct webrtc_output *out)
{
    if (!out->opus_ctx || !out->opus_frame || !out->opus_pkt) return;

    int fs = out->opus_ctx->frame_size;

    if (out->opus_sample_fmt == AV_SAMPLE_FMT_FLTP) {
        float *ch0 = (float *)out->opus_frame->data[0];
        float *ch1 = (float *)out->opus_frame->data[1];
        const float *src = out->audio_buf;
        for (int i = 0; i < fs; i++) {
            ch0[i] = src[i * OPUS_CHANNELS + 0];
            ch1[i] = src[i * OPUS_CHANNELS + 1];
        }
    } else {
        memcpy(out->opus_frame->data[0],
               out->audio_buf,
               (size_t)(fs * OPUS_CHANNELS) * sizeof(float));
    }

    out->opus_frame->pts = out->audio_rtp_ts;

    if (avcodec_send_frame(out->opus_ctx, out->opus_frame) < 0) goto shift;

    while (avcodec_receive_packet(out->opus_ctx, out->opus_pkt) == 0) {
        int plen = out->opus_pkt->size;
        uint8_t pkt[12 + 1276];
        if (plen <= (int)(sizeof(pkt) - 12)) {
            rtp_write_header(pkt, OPUS_PT, false,
                             out->audio_seq++,
                             (uint32_t)out->audio_rtp_ts,
                             (uint32_t)AUDIO_SSRC);
            memcpy(pkt + 12, out->opus_pkt->data, (size_t)plen);
            if (out->audio_sock != INVALID_SOCK)
                send(out->audio_sock, (const char *)pkt, 12 + plen, 0);
        }
        av_packet_unref(out->opus_pkt);
    }

shift:
    out->audio_rtp_ts += (uint32_t)fs;

    int remain = out->audio_buf_n - fs;
    if (remain > 0) {
        memmove(out->audio_buf,
                out->audio_buf + (size_t)(fs * OPUS_CHANNELS),
                (size_t)(remain * OPUS_CHANNELS) * sizeof(float));
    }
    out->audio_buf_n = (remain > 0) ? remain : 0;
}

/* ------------------------------------------------------------------ */
/* Background connection thread                                         */
/* ------------------------------------------------------------------ */

/*
 * Polls the mediasoup server for its RTP port allocation, then creates
 * and connects UDP sockets.
 */
static void connect_thread(void *arg)
{
    struct webrtc_output *out = (struct webrtc_output *)arg;

    fprintf(stdout,
            "[WebRTC] Waiting for mediasoup server on port %d …\n",
            out->mediasoup_port);
    fflush(stdout);

    for (int tries = 0; out->running; tries++) {
        if (tries > 0) SLEEP_MS(1000);
        if (tries >= CONNECT_MAX_TRIES) {
            fprintf(stderr,
                    "[WebRTC] mediasoup server not reachable after %d seconds"
                    " — still retrying\n",
                    tries);
            fflush(stderr);
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

        sock_set_nonblocking(vs);

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
        fflush(stdout);
        break; /* proceed to keyframe poll loop */
    }

    /* Poll /keyframe-needed once per second for the lifetime of the output.
     * When a new browser viewer connects, the server sets this flag so we
     * can inject the cached IDR frame for immediate playback. */
    while (out->running) {
        SLEEP_MS(1000);
        if (http_get_keyframe_needed(out->mediasoup_port)) {
            mutex_lock(&out->lock);
            if (out->ready)
                out->needs_keyframe = true;
            mutex_unlock(&out->lock);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

struct webrtc_output *webrtc_output_create_with_options(
    int mediasoup_port,
    const struct webrtc_output_options *options)
{
    struct webrtc_output *out =
        (struct webrtc_output *)calloc(1, sizeof(struct webrtc_output));
    if (!out) return NULL;

    out->mediasoup_port = mediasoup_port;
    out->running        = true;
    out->video_sock     = INVALID_SOCK;
    out->audio_sock     = INVALID_SOCK;
    out->video_mode     = WEBRTC_VIDEO_MODE_PASSTHROUGH;
    out->video_encoder_preference = WEBRTC_VIDEO_ENCODER_AUTO;

    if (options) {
        out->video_mode = options->video_mode;
        out->video_encoder_preference = options->video_encoder_preference;
    }

    if (out->video_mode == WEBRTC_VIDEO_MODE_TRANSCODE_AUTO) {
        out->selected_video_encoder =
            find_video_encoder_by_preference(out->video_encoder_preference);
        if (out->selected_video_encoder) {
            fprintf(stdout,
                    "[WebRTC] Video mode=%s, encoder_pref=%s, selected=%s\n",
                    video_mode_name(out->video_mode),
                    video_encoder_pref_name(out->video_encoder_preference),
                    out->selected_video_encoder->name);
        } else {
            fprintf(stdout,
                    "[WebRTC] Video mode=%s requested but no H264 encoder found; "
                    "falling back to passthrough\n",
                    video_mode_name(out->video_mode));
            out->video_mode = WEBRTC_VIDEO_MODE_PASSTHROUGH;
        }
    } else {
        fprintf(stdout,
                "[WebRTC] Video mode=%s\n",
                video_mode_name(out->video_mode));
    }

    fprintf(stdout, "[WebRTC] Initializing mutex...\n");
    fflush(stdout);
    mutex_init(&out->lock);

    fprintf(stdout, "[WebRTC] Initializing Opus encoder...\n");
    fflush(stdout);
    /* Try Opus encoder init */
    if (!opus_encoder_init(out)) {
        out->opus_disabled = true;
        fprintf(stdout, "[WebRTC] Opus encoder: disabled (not supported)\n");
    }

    fprintf(stdout, "[WebRTC] Starting connect thread...\n");
    fflush(stdout);

    /* Start background thread to connect to mediasoup */
    thread_start(connect_thread, out);
    fflush(stdout);

    return out;
}

struct webrtc_output *webrtc_output_create(int mediasoup_port)
{
    struct webrtc_output_options defaults;
    defaults.video_mode = WEBRTC_VIDEO_MODE_PASSTHROUGH;
    defaults.video_encoder_preference = WEBRTC_VIDEO_ENCODER_AUTO;
    return webrtc_output_create_with_options(mediasoup_port, &defaults);
}

void webrtc_output_destroy(struct webrtc_output *out)
{
    if (!out) return;

    out->running = false;
    SLEEP_MS(1500);  /* wait for connect thread's sleep */

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

    transcode_destroy(out);
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

    if (out->ready && video_sock_poll_keyframe_feedback(out))
    {
        out->needs_keyframe = true;
        out->pending_keyframe_burst = KEYFRAME_BURST_COUNT;
        out->next_keyframe_burst_pts_us = 0;
        out->rtcp_keyframe_req_count++;
        fprintf(stdout,
                "[WebRTC] RTCP keyframe feedback received (count=%llu)\n",
                (unsigned long long)out->rtcp_keyframe_req_count);
    }

    if (out->video_mode == WEBRTC_VIDEO_MODE_TRANSCODE_AUTO &&
        out->selected_video_encoder) {
        if (!out->video_dec_ctx) {
            if (!transcode_init_decoder(out)) {
                if (!out->transcode_warning_logged) {
                    out->transcode_warning_logged = true;
                    fprintf(stderr,
                            "[WebRTC] Transcode decode init failed; falling back to passthrough\n");
                }
                out->video_mode = WEBRTC_VIDEO_MODE_PASSTHROUGH;
            }
        }

        if (out->video_mode != WEBRTC_VIDEO_MODE_TRANSCODE_AUTO ||
            !out->video_dec_ctx) {
            /* Decoder init failed: continue with passthrough path below. */
        } else {
            if (!out->ready || out->video_sock == INVALID_SOCK) {
                mutex_unlock(&out->lock);
                return;
            }
            transcode_process_video(out, data, size, pts_us);
            mutex_unlock(&out->lock);
            return;
        }
    }

    if (out->video_mode == WEBRTC_VIDEO_MODE_TRANSCODE_AUTO &&
        !out->selected_video_encoder && !out->transcode_warning_logged) {
        out->transcode_warning_logged = true;
        fprintf(stderr,
                "[WebRTC] Transcode mode requested but no encoder selected; using passthrough\n");
        out->video_mode = WEBRTC_VIDEO_MODE_PASSTHROUGH;
    }

    /* Always cache the most recent IDR frame so that when the AirPlay
     * stream restarts (webrtc_output_request_keyframe is called), an
     * up-to-date keyframe is available for immediate injection. */
    bool is_idr = h264_has_idr(data, size);
    if (is_idr) {
        out->idr_received_count++;
        out->last_idr_pts_us = pts_us;
        fprintf(stdout,
                "[WebRTC] IDR frame received (count=%llu, pts=%lld us, "
                "time_since_last_idr=%.1f ms)\n",
                (unsigned long long)out->idr_received_count,
                (long long)pts_us,
                out->last_idr_pts_us > 0
                    ? (double)(pts_us - out->keyframe_cache_pts_us) / 1000.0
                    : 0.0);

        uint8_t *new_cache = (uint8_t *)malloc(size);
        if (new_cache) {
            free(out->keyframe_cache);
            out->keyframe_cache      = new_cache;
            out->keyframe_cache_size = size;
            out->keyframe_cache_pts_us = pts_us;
            memcpy(out->keyframe_cache, data, size);
            fprintf(stdout,
                    "[WebRTC] Cached fresh IDR (%zu bytes, pts=%lld us)\n",
                    size, (long long)pts_us);
        } else {
            fprintf(stderr,
                    "[WebRTC] Warning: keyframe cache allocation failed"
                    " (%zu bytes)\n", size);
        }
    }

    if (out->ready && out->video_sock != INVALID_SOCK) {
        uint32_t ts = (uint32_t)
            ((pts_us * (int64_t)H264_CLOCK_RATE) / 1000000LL);

        if (is_idr) {
            out->needs_keyframe = false;
            out->pending_keyframe_burst = 0;
            out->next_keyframe_burst_pts_us = 0;
        }

        /* Inject a cached keyframe when a new viewer arrives and needs sync.
         * Use a short bounded burst to survive races or packet loss right after
         * reconnect, but stop quickly to avoid repeatedly flashing old content. */
        int64_t cache_age_us = pts_us - out->keyframe_cache_pts_us;

        if ((out->needs_keyframe || out->pending_keyframe_burst > 0) &&
            out->keyframe_cache && out->keyframe_cache_size > 0 &&
            cache_age_us >= 0 && cache_age_us <= KEYFRAME_CACHE_MAX_AGE_US &&
            (out->next_keyframe_burst_pts_us == 0 ||
             pts_us >= out->next_keyframe_burst_pts_us)) {
            uint32_t kf_ts = (ts >= H264_FRAME_TICKS)
                             ? ts - H264_FRAME_TICKS : 0;
            rtp_send_h264(out, out->keyframe_cache,
                          out->keyframe_cache_size, kf_ts);
            out->needs_keyframe = false;
            if (out->pending_keyframe_burst > 0)
                out->pending_keyframe_burst--;
            out->next_keyframe_burst_pts_us =
                (out->pending_keyframe_burst > 0)
                ? pts_us + KEYFRAME_BURST_INTERVAL_US
                : 0;
            out->injected_keyframe_count++;
            fprintf(stdout,
                    "[WebRTC] Injected cached IDR for new viewer "
                    "(count=%llu, burstRemaining=%d)\n",
                    (unsigned long long)out->injected_keyframe_count,
                    out->pending_keyframe_burst);
        }

        rtp_send_h264(out, data, size, ts);
    }
    mutex_unlock(&out->lock);
}

void webrtc_output_request_keyframe(struct webrtc_output *out)
{
    if (!out) return;
    mutex_lock(&out->lock);
    if (out->ready) {
        out->needs_keyframe = true;
        out->pending_keyframe_burst = KEYFRAME_BURST_COUNT;
        out->next_keyframe_burst_pts_us = 0;
    }
    mutex_unlock(&out->lock);
}

void webrtc_output_write_audio(struct webrtc_output *out,
                               const float *pcm, int samples,
                               int channels, int sample_rate,
                               int64_t pts_us)
{
    (void)pts_us;

    if (!out || !pcm || samples <= 0) return;

    /* Check if audio is disabled */
    if (out->opus_disabled)
        return;

    mutex_lock(&out->lock);

    if (!out->opus_ctx || !out->opus_frame || !out->opus_pkt) {
        /* Try late init only if not already marked disabled */
        if (!opus_encoder_init(out)) {
            out->opus_disabled = true;
            fprintf(stderr,
                    "[WebRTC] Warning: disabling audio (Opus init failed)\n");
            mutex_unlock(&out->lock);
            return;
        }
    }

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

        if (out->opus_ctx &&
            out->audio_buf_n >= out->opus_ctx->frame_size)
            flush_opus_frame(out);
    }

    mutex_unlock(&out->lock);
}
