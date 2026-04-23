/*
 * webrtc-output.c
 * WebRTC output server — low-latency H.264 + Opus browser playback.
 *
 * Architecture:
 *   - A background thread runs a minimal HTTP server for WebRTC signalling.
 *     GET /         → serves the embedded HTML player page.
 *     POST /offer   → processes the browser's SDP offer, creates a
 *                     libdatachannel PeerConnection, waits for ICE
 *                     gathering, and returns the SDP answer.
 *   - Video frames (H.264 Annex-B) are parsed into individual NAL units
 *     and packetised into RTP per RFC 6184 (single-NAL or FU-A).
 *   - Audio (float32 PCM, typically 44100 Hz) is resampled to 48 kHz via
 *     FFmpeg SWR, accumulated into 960-sample chunks, encoded to Opus,
 *     and wrapped in RTP per RFC 7587.
 *   - A single mutex serialises all state mutations and RTP sends.
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
/* BCryptGenRandom (fallback SSRC entropy) */
#  include <bcrypt.h>

typedef SOCKET sock_t;
#  define INVALID_SOCK  INVALID_SOCKET

static void      sock_set_nonblock(sock_t s) { u_long m=1; ioctlsocket(s,FIONBIO,&m); }
static void      sock_close(sock_t s)        { closesocket(s); }
static bool      sock_would_block(void)      { return WSAGetLastError()==WSAEWOULDBLOCK; }

typedef CRITICAL_SECTION wrtc_mutex_t;
static void mutex_init(wrtc_mutex_t *m)    { InitializeCriticalSection(m); }
static void mutex_lock(wrtc_mutex_t *m)    { EnterCriticalSection(m); }
static void mutex_unlock(wrtc_mutex_t *m)  { LeaveCriticalSection(m); }
static void mutex_destroy(wrtc_mutex_t *m) { DeleteCriticalSection(m); }

/* Auto-reset event */
typedef struct { HANDLE h; } wrtc_event_t;
static wrtc_event_t *event_create(void) {
    wrtc_event_t *e = (wrtc_event_t *)malloc(sizeof(*e));
    if (e) e->h = CreateEvent(NULL, FALSE, FALSE, NULL);
    return e;
}
static void event_signal(wrtc_event_t *e)           { SetEvent(e->h); }
static bool event_wait_ms(wrtc_event_t *e, int ms)  { return WaitForSingleObject(e->h,(DWORD)ms)==WAIT_OBJECT_0; }
static void event_destroy(wrtc_event_t *e)          { CloseHandle(e->h); free(e); }

static void thread_start(void (*fn)(void *), void *arg) {
    _beginthread((_beginthread_proc_type)fn, 0, arg);
}

#  define SLEEP_MS(ms)       Sleep(ms)
#  define STRNCASECMP(a,b,n) _strnicmp(a,b,n)

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

static void sock_set_nonblock(sock_t s) { fcntl(s,F_SETFL,fcntl(s,F_GETFL,0)|O_NONBLOCK); }
static void sock_close(sock_t s)        { close(s); }
static bool sock_would_block(void)      { return errno==EAGAIN||errno==EWOULDBLOCK; }

typedef pthread_mutex_t wrtc_mutex_t;
static void mutex_init(wrtc_mutex_t *m)    { pthread_mutex_init(m,NULL); }
static void mutex_lock(wrtc_mutex_t *m)    { pthread_mutex_lock(m); }
static void mutex_unlock(wrtc_mutex_t *m)  { pthread_mutex_unlock(m); }
static void mutex_destroy(wrtc_mutex_t *m) { pthread_mutex_destroy(m); }

typedef struct {
    pthread_mutex_t mtx;
    pthread_cond_t  cnd;
    bool            sig;
} wrtc_event_t;

static wrtc_event_t *event_create(void) {
    wrtc_event_t *e = (wrtc_event_t *)calloc(1, sizeof(*e));
    if (e) { pthread_mutex_init(&e->mtx,NULL); pthread_cond_init(&e->cnd,NULL); }
    return e;
}
static void event_signal(wrtc_event_t *e) {
    pthread_mutex_lock(&e->mtx);
    e->sig = true;
    pthread_cond_signal(&e->cnd);
    pthread_mutex_unlock(&e->mtx);
}
static bool event_wait_ms(wrtc_event_t *e, int ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&e->mtx);
    while (!e->sig)
        if (pthread_cond_timedwait(&e->cnd, &e->mtx, &ts) != 0) break;
    bool got = e->sig;
    e->sig = false;
    pthread_mutex_unlock(&e->mtx);
    return got;
}
static void event_destroy(wrtc_event_t *e) {
    pthread_cond_destroy(&e->cnd);
    pthread_mutex_destroy(&e->mtx);
    free(e);
}

static void thread_start(void (*fn)(void *), void *arg) {
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &a, (void *(*)(void *))fn, arg);
    pthread_attr_destroy(&a);
}

#  define SLEEP_MS(ms)       usleep((ms)*1000)
#  define STRNCASECMP(a,b,n) strncasecmp(a,b,n)

#endif /* _WIN32 */

/* ------------------------------------------------------------------ */
/* FFmpeg (Opus encode + SWR resample)                                 */
/* ------------------------------------------------------------------ */

#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>

/* ------------------------------------------------------------------ */
/* OpenSSL (secure random for SSRC generation)                         */
/* ------------------------------------------------------------------ */

#include <openssl/rand.h>

/* ------------------------------------------------------------------ */
/* libdatachannel C API                                                 */
/* ------------------------------------------------------------------ */

#include <rtc/rtc.h>

/* ------------------------------------------------------------------ */
/* Constants                                                            */
/* ------------------------------------------------------------------ */

#define RTP_MAX_PAYLOAD  1200   /* max bytes per RTP payload */
#define H264_PT_DEFAULT  96     /* fallback dynamic PT for H.264 */
#define OPUS_PT_DEFAULT  111    /* fallback dynamic PT for Opus   */
#define H264_CLOCK_RATE  90000  /* Hz */
#define OPUS_SAMPLE_RATE 48000  /* Hz (WebRTC requirement) */
#define OPUS_FRAME_SIZE  960    /* samples/channel at 48 kHz = 20 ms */
#define OPUS_CHANNELS    2
/* Per-channel sample accumulation buffer (covers several Opus frames) */
#define AUDIO_BUF_FRAMES 8
#define AUDIO_BUF_CAP    (OPUS_FRAME_SIZE * AUDIO_BUF_FRAMES)

/* Maximum SDP body size accepted from the browser */
#define MAX_SDP_OFFER_SIZE  65536

/* Temporary stack buffer for one SWR resample call (per-channel samples) */
#define RESAMPLE_TMP_CAP    (OPUS_FRAME_SIZE * 4)

/* ------------------------------------------------------------------ */
/* Embedded HTML player                                                 */
/* ------------------------------------------------------------------ */

static const char HTML_PLAYER[] =
"<!DOCTYPE html>\n"
"<html lang=\"en\">\n"
"<head>\n"
"<meta charset=\"UTF-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
"<title>AirPlay WebRTC</title>\n"
"<style>\n"
"*{margin:0;padding:0;box-sizing:border-box}\n"
"body{background:#000;overflow:hidden}\n"
"#v{width:100vw;height:100vh;object-fit:contain;display:block}\n"
"#s{position:fixed;top:10px;left:50%;transform:translateX(-50%);\n"
"   background:rgba(0,0,0,.7);color:#fff;padding:5px 16px;\n"
"   border-radius:20px;font:13px/1.6 sans-serif;pointer-events:none;\n"
"   transition:opacity .4s}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<video id=\"v\" autoplay playsinline></video>\n"
"<div id=\"s\">Connecting\u2026</div>\n"
"<script>\n"
"const s=document.getElementById('s');\n"
"const v=document.getElementById('v');\n"
"let pc=null,retry=null;\n"
"async function connect(){\n"
"  if(retry){clearTimeout(retry);retry=null;}\n"
"  if(pc){try{pc.close();}catch(e){}pc=null;}\n"
"  s.style.opacity='1';s.textContent='Connecting\u2026';\n"
"  try{\n"
"    pc=new RTCPeerConnection();\n"
"    pc.ontrack=e=>{v.srcObject=e.streams[0];};\n"
"    pc.oniceconnectionstatechange=()=>{\n"
"      const st=pc.iceConnectionState;\n"
"      if(st==='connected'||st==='completed'){\n"
"        s.textContent='Connected';setTimeout(()=>{s.style.opacity='0';},2000);\n"
"      } else if(st==='failed'||st==='disconnected'||st==='closed'){\n"
"        s.style.opacity='1';s.textContent='Reconnecting\u2026';\n"
"        retry=setTimeout(connect,2000);\n"
"      }\n"
"    };\n"
"    pc.addTransceiver('video',{direction:'recvonly'});\n"
"    pc.addTransceiver('audio',{direction:'recvonly'});\n"
"    const offer=await pc.createOffer();\n"
"    await pc.setLocalDescription(offer);\n"
"    await new Promise(r=>{\n"
"      if(pc.iceGatheringState==='complete'){r();return;}\n"
"      pc.onicegatheringstatechange=()=>{if(pc.iceGatheringState==='complete')r();};\n"
"    });\n"
"    const resp=await fetch('/offer',{\n"
"      method:'POST',\n"
"      headers:{'Content-Type':'application/sdp'},\n"
"      body:pc.localDescription.sdp\n"
"    });\n"
"    if(!resp.ok)throw new Error('Server returned '+resp.status);\n"
"    const answer=await resp.text();\n"
"    await pc.setRemoteDescription({type:'answer',sdp:answer});\n"
"  } catch(e){\n"
"    s.style.opacity='1';\n"
"    s.textContent='Error: '+e.message+' \u2014 retrying\u2026';\n"
"    retry=setTimeout(connect,3000);\n"
"  }\n"
"}\n"
"connect();\n"
"</script>\n"
"</body>\n"
"</html>\n";

/*
 * Fill buf with size bytes of cryptographically secure random data.
 * Primary: OpenSSL RAND_bytes.
 * Fallback: BCryptGenRandom (Windows) or /dev/urandom (POSIX).
 */
static void secure_random(unsigned char *buf, size_t size)
{
    if (RAND_bytes(buf, (int)size) == 1)
        return;

#ifdef _WIN32
    BCryptGenRandom(NULL, buf, (ULONG)size, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        (void)fread(buf, 1, size, f);
        fclose(f);
    }
#endif
}
/* ------------------------------------------------------------------ */

struct webrtc_output {
    int           http_port;
    wrtc_mutex_t  lock;
    volatile bool running;

    /* HTTP signalling */
    sock_t        http_listen;

    /* Active libdatachannel peer connection (-1 = none) */
    int           pc;
    int           video_tr;
    int           audio_tr;
    bool          pc_open;         /* true once ICE/DTLS connected */
    wrtc_event_t *gather_evt;      /* signalled when ICE gathering complete */

    /* RTP state */
    int           video_pt;        /* negotiated payload type for H.264 */
    int           audio_pt;        /* negotiated payload type for Opus */
    uint16_t      video_seq;
    uint16_t      audio_seq;
    uint32_t      video_ssrc;
    uint32_t      audio_ssrc;

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
};

/* ------------------------------------------------------------------ */
/* SDP helper: find RTP payload type for a named codec                 */
/* ------------------------------------------------------------------ */

static int sdp_find_pt(const char *sdp, const char *codec)
{
    for (const char *p = sdp; *p; ) {
        const char *tag = strstr(p, "a=rtpmap:");
        if (!tag) break;
        const char *num = tag + 9;
        int pt = 0;
        while (*num >= '0' && *num <= '9')
            pt = pt * 10 + (*num++ - '0');
        if (*num == ' ') {
            num++;
            size_t clen = strlen(codec);
            if (STRNCASECMP(num, codec, clen) == 0 &&
                (num[clen] == '/' || num[clen] == '\r' ||
                 num[clen] == '\n' || num[clen] == '\0'))
                return pt;
        }
        p = num; /* advance past this line */
    }
    return -1;
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
 * Send one raw NAL unit (without Annex-B start code) as RTP.
 * Uses Single NAL Unit Packet if size <= RTP_MAX_PAYLOAD,
 * otherwise FU-A fragmentation.
 * Called with out->lock held.
 */
static void rtp_send_nal(struct webrtc_output *out,
                         const uint8_t *nal, int nal_size,
                         uint32_t ts, bool marker)
{
    if (nal_size <= 0 || out->video_tr < 0) return;

    /* Stack-allocated packet buffer: 12 (RTP hdr) + 2 (FU-A hdr) + payload */
    uint8_t pkt[12 + 2 + RTP_MAX_PAYLOAD];
    int     pt   = out->video_pt;
    uint32_t ssrc = out->video_ssrc;

    if (nal_size <= RTP_MAX_PAYLOAD) {
        /* Single NAL Unit Packet */
        rtp_write_header(pkt, pt, marker, out->video_seq++, ts, ssrc);
        memcpy(pkt + 12, nal, (size_t)nal_size);
        rtcSendMessage(out->video_tr, (const char *)pkt, 12 + nal_size);
    } else {
        /* FU-A fragmentation */
        uint8_t fu_ind  = (nal[0] & 0xE0) | 28; /* NRI preserved, type = 28 */
        uint8_t nal_typ = nal[0] & 0x1F;
        int     offset  = 1;  /* skip original NAL header byte */
        bool    first   = true;

        while (offset < nal_size) {
            int  chunk     = nal_size - offset;
            bool last_frag = (chunk <= RTP_MAX_PAYLOAD - 2);
            if (chunk > RTP_MAX_PAYLOAD - 2)
                chunk = RTP_MAX_PAYLOAD - 2;

            bool m = last_frag && marker;
            rtp_write_header(pkt, pt, m, out->video_seq++, ts, ssrc);
            pkt[12] = fu_ind;
            pkt[13] = nal_typ
                    | (first     ? 0x80 : 0x00)   /* S bit */
                    | (last_frag ? 0x40 : 0x00);   /* E bit */
            memcpy(pkt + 14, nal + offset, (size_t)chunk);
            rtcSendMessage(out->video_tr, (const char *)pkt, 14 + chunk);

            offset += chunk;
            first   = false;
        }
    }
}

/*
 * Parse an H.264 Annex-B buffer into individual NAL units and send
 * each via RTP.  Called with out->lock held.
 */
static void rtp_send_h264(struct webrtc_output *out,
                           const uint8_t *data, size_t size,
                           uint32_t ts)
{
    if (!out->pc_open || out->video_tr < 0) return;

    /* Locate start codes and record the offset of the first NAL byte
     * (i.e. the byte immediately after the start code).  Track each
     * start-code length so that we can compute the exact NAL boundary
     * without relying on trailing-zero trimming alone. */
    size_t  starts[256];
    uint8_t sc_lens[256]; /* 3 or 4 */
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
        /* Compute end: start of the next start-code pattern (not the byte
         * after it).  Using the recorded sc_len gives the exact boundary
         * regardless of whether the next code is 3 or 4 bytes. */
        size_t e = (n + 1 < nals)
                   ? starts[n + 1] - sc_lens[n + 1]
                   : size;
        /* Trim any remaining trailing zero bytes (these can legitimately
         * appear as extra zero_byte() elements before a start code per the
         * Annex-B spec, and are never part of the RBSP payload). */
        while (e > s && data[e - 1] == 0)
            e--;
        if (e <= s) continue;
        rtp_send_nal(out, data + s, (int)(e - s), ts, (n == nals - 1));
    }
}

/* ------------------------------------------------------------------ */
/* libdatachannel callbacks                                             */
/* ------------------------------------------------------------------ */

static void on_gathering_state(int pc, rtcGatheringState state, void *ptr)
{
    (void)pc;
    if (state == RTC_GATHERING_COMPLETE)
        event_signal(((struct webrtc_output *)ptr)->gather_evt);
}

static void on_pc_state(int pc, rtcState state, void *ptr)
{
    (void)pc;
    struct webrtc_output *out = (struct webrtc_output *)ptr;
    if (state == RTC_CONNECTED) {
        mutex_lock(&out->lock);
        out->pc_open = true;
        mutex_unlock(&out->lock);
        fprintf(stdout, "[WebRTC] Peer connection established — streaming\n");
    } else if (state == RTC_FAILED ||
               state == RTC_DISCONNECTED ||
               state == RTC_CLOSED) {
        mutex_lock(&out->lock);
        out->pc_open = false;
        mutex_unlock(&out->lock);
        fprintf(stdout, "[WebRTC] Peer connection closed (state=%d)\n",
                (int)state);
    }
}

/* ------------------------------------------------------------------ */
/* SDP offer processing: create PC, set remote desc, wait, get answer  */
/* ------------------------------------------------------------------ */

static bool handle_offer(struct webrtc_output *out,
                         const char *offer_sdp,
                         char *answer_out, int answer_cap)
{
    /* Extract PTs from the browser's offer so that we use the same values
     * in both the SDP answer and our RTP packet headers. */
    int h264_pt = sdp_find_pt(offer_sdp, "H264");
    int opus_pt  = sdp_find_pt(offer_sdp, "opus");
    if (h264_pt < 0) h264_pt = H264_PT_DEFAULT;
    if (opus_pt  < 0) opus_pt  = OPUS_PT_DEFAULT;

    mutex_lock(&out->lock);

    /* Tear down existing peer connection if any */
    if (out->pc >= 0) {
        rtcDeletePeerConnection(out->pc);
        out->pc       = -1;
        out->video_tr = -1;
        out->audio_tr = -1;
        out->pc_open  = false;
    }
    out->video_pt = h264_pt;
    out->audio_pt = opus_pt;
    /* Reset audio RTP timestamp so the new session starts from 0 */
    out->audio_rtp_ts = 0;
    out->audio_buf_n  = 0;

    mutex_unlock(&out->lock);

    /* Create peer connection (no STUN needed for same-machine use) */
    rtcConfiguration cfg;
    memset(&cfg, 0, sizeof(cfg));
    int pc = rtcCreatePeerConnection(&cfg);
    if (pc < 0) {
        fprintf(stderr, "[WebRTC] rtcCreatePeerConnection failed (%d)\n", pc);
        return false;
    }

    rtcSetUserPointer(pc, out);
    rtcSetStateChangeCallback(pc, on_pc_state);
    rtcSetGatheringStateChangeCallback(pc, on_gathering_state);

    /* Build SDP media section strings using the PT we extracted.
     * Buffers are 512 bytes; the largest possible string is well under 256. */
    char vdesc[512], adesc[512];
    int vdesc_len = snprintf(vdesc, sizeof(vdesc),
        "video 9 UDP/TLS/RTP/SAVPF %d\r\n"
        "a=mid:video\r\n"
        "a=rtpmap:%d H264/90000\r\n"
        "a=fmtp:%d level-asymmetry-allowed=1;packetization-mode=1;"
            "profile-level-id=42e01f\r\n"
        "a=sendonly\r\n"
        "a=ssrc:%u cname:airplay\r\n",
        h264_pt, h264_pt, h264_pt, out->video_ssrc);
    int adesc_len = snprintf(adesc, sizeof(adesc),
        "audio 9 UDP/TLS/RTP/SAVPF %d\r\n"
        "a=mid:audio\r\n"
        "a=rtpmap:%d opus/48000/2\r\n"
        "a=fmtp:%d minptime=10;useinbandfec=1\r\n"
        "a=sendonly\r\n"
        "a=ssrc:%u cname:airplay\r\n",
        opus_pt, opus_pt, opus_pt, out->audio_ssrc);
    if (vdesc_len <= 0 || vdesc_len >= (int)sizeof(vdesc) ||
        adesc_len <= 0 || adesc_len >= (int)sizeof(adesc)) {
        fprintf(stderr, "[WebRTC] SDP media description too long\n");
        rtcDeletePeerConnection(pc);
        return false;
    }

    int vtr = rtcAddTrack(pc, vdesc);
    int atr = rtcAddTrack(pc, adesc);
    if (vtr < 0 || atr < 0) {
        fprintf(stderr, "[WebRTC] rtcAddTrack failed (video=%d audio=%d)\n",
                vtr, atr);
        rtcDeletePeerConnection(pc);
        return false;
    }

    /* Store handles so write_video / write_audio can use them */
    mutex_lock(&out->lock);
    out->pc       = pc;
    out->video_tr = vtr;
    out->audio_tr = atr;
    mutex_unlock(&out->lock);

    /* Hand the browser's offer to libdatachannel.
     * When disableAutoNegotiation=false (default), this triggers automatic
     * answer generation and ICE gathering. */
    if (rtcSetRemoteDescription(pc, offer_sdp, "offer") < 0) {
        fprintf(stderr, "[WebRTC] rtcSetRemoteDescription failed\n");
        mutex_lock(&out->lock);
        out->pc = out->video_tr = out->audio_tr = -1;
        mutex_unlock(&out->lock);
        rtcDeletePeerConnection(pc);
        return false;
    }

    /* Wait up to 5 seconds for ICE gathering to complete */
    if (!event_wait_ms(out->gather_evt, 5000))
        fprintf(stderr, "[WebRTC] Warning: ICE gathering timed out\n");

    if (rtcGetLocalDescription(pc, answer_out, answer_cap) < 0) {
        fprintf(stderr, "[WebRTC] rtcGetLocalDescription failed\n");
        mutex_lock(&out->lock);
        out->pc = out->video_tr = out->audio_tr = -1;
        mutex_unlock(&out->lock);
        rtcDeletePeerConnection(pc);
        return false;
    }

    fprintf(stdout,
            "[WebRTC] SDP answer ready — waiting for browser ICE connection\n");
    return true;
}

/* ------------------------------------------------------------------ */
/* Minimal HTTP server (signalling only)                               */
/* ------------------------------------------------------------------ */

/* Read from socket until \r\n\r\n is seen; returns bytes read or -1. */
static int recv_headers(sock_t s, char *buf, int cap)
{
    int n = 0;
    while (n < cap - 1) {
        int r = (int)recv(s, buf + n, (size_t)(cap - 1 - n), 0);
        if (r <= 0) return -1;
        n += r;
        buf[n] = '\0';
        if (strstr(buf, "\r\n\r\n")) return n;
    }
    return -1;
}

static int parse_content_length(const char *hdrs)
{
    for (const char *p = hdrs; *p; ) {
        if (STRNCASECMP(p, "content-length:", 15) == 0)
            return atoi(p + 15);
        const char *nl = strstr(p, "\r\n");
        if (!nl) break;
        p = nl + 2;
    }
    return 0;
}

static void http_respond(sock_t s, int code, const char *ct,
                          const char *body, int blen)
{
    if (blen < 0) blen = (int)strlen(body);
    char hdr[320];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n\r\n",
        code, ct, blen);
    send(s, hdr, hlen, 0);
    if (body && blen > 0)
        send(s, body, blen, 0);
}

static void handle_http_conn(struct webrtc_output *out, sock_t cs)
{
    char hbuf[4096];
    if (recv_headers(cs, hbuf, (int)sizeof(hbuf)) < 0) return;

    bool is_get  = (strncmp(hbuf, "GET ",  4) == 0);
    bool is_post = (strncmp(hbuf, "POST ", 5) == 0);
    if (!is_get && !is_post) {
        http_respond(cs, 405, "text/plain", "Method Not Allowed", -1);
        return;
    }

    const char *path = hbuf + (is_post ? 5 : 4);
    const char *spc  = strchr(path, ' ');
    int plen = spc ? (int)(spc - path) : 0;

    /* GET / → HTML player */
    if (is_get && plen == 1 && path[0] == '/') {
        http_respond(cs, 200, "text/html; charset=UTF-8",
                     HTML_PLAYER, (int)(sizeof(HTML_PLAYER) - 1));
        return;
    }

    /* POST /offer → WebRTC signalling */
    if (is_post && plen == 6 && strncmp(path, "/offer", 6) == 0) {
        int clen = parse_content_length(hbuf);
        if (clen <= 0 || clen > MAX_SDP_OFFER_SIZE) {
            http_respond(cs, 400, "text/plain", "Bad Request", -1);
            return;
        }
        char *offer = (char *)malloc((size_t)clen + 1);
        if (!offer) { http_respond(cs, 500, "text/plain", "OOM", -1); return; }

        /* Set a receive timeout so we don't block forever if the client
         * sends fewer bytes than Content-Length promises. */
#ifdef _WIN32
        DWORD tv_ms = 10000;
        setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO,
                   (const char *)&tv_ms, sizeof(tv_ms));
#else
        struct timeval tv = {10, 0}; /* 10 s */
        setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

        int received = 0;
        while (received < clen) {
            int r = (int)recv(cs, offer + received,
                              (size_t)(clen - received), 0);
            if (r <= 0) { free(offer); return; }
            received += r;
        }
        offer[received] = '\0';

        char *answer = (char *)malloc(16384);
        if (!answer) {
            free(offer);
            http_respond(cs, 500, "text/plain", "OOM", -1);
            return;
        }

        if (handle_offer(out, offer, answer, 16384)) {
            http_respond(cs, 200, "application/sdp", answer, -1);
        } else {
            http_respond(cs, 500, "text/plain", "WebRTC setup failed", -1);
        }
        free(offer);
        free(answer);
        return;
    }

    http_respond(cs, 404, "text/plain", "Not Found", -1);
}

static void http_thread(void *arg)
{
    struct webrtc_output *out = (struct webrtc_output *)arg;
    while (out->running) {
        sock_t cs = accept(out->http_listen, NULL, NULL);
        if (cs == INVALID_SOCK) {
            if (!out->running) break;
            if (sock_would_block()) { SLEEP_MS(10); continue; }
            fprintf(stderr, "[WebRTC] HTTP accept error\n");
            break;
        }
        handle_http_conn(out, cs);
        sock_close(cs);
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
    /* Prefer lowest latency application mode if the encoder supports it */
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
 * Resets the accumulation buffer if the input format changes.
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
    out->audio_buf_n = 0;  /* discard stale samples on format change */
    return true;
}

/*
 * Encode one OPUS_FRAME_SIZE block from audio_buf and send as RTP.
 * Called with out->lock held.
 */
static void flush_opus_frame(struct webrtc_output *out)
{
    if (!out->opus_ctx || !out->opus_frame) return;

    /* Copy interleaved samples into the AVFrame buffer */
    memcpy(out->opus_frame->data[0],
           out->audio_buf,
           (size_t)(OPUS_FRAME_SIZE * OPUS_CHANNELS) * sizeof(float));
    out->opus_frame->pts = out->audio_rtp_ts;

    if (avcodec_send_frame(out->opus_ctx, out->opus_frame) < 0) goto shift;

    while (avcodec_receive_packet(out->opus_ctx, out->opus_pkt) == 0) {
        /* Build: 12-byte RTP header + Opus payload.
         * Max Opus payload per RFC 7587 is 1276 bytes; use a stack buffer. */
        int     plen = out->opus_pkt->size;
        uint8_t pkt[12 + 1276];
        if (plen <= (int)(sizeof(pkt) - 12)) {
            rtp_write_header(pkt, out->audio_pt, false,
                             out->audio_seq++,
                             (uint32_t)out->audio_rtp_ts,
                             out->audio_ssrc);
            memcpy(pkt + 12, out->opus_pkt->data, (size_t)plen);
            if (out->audio_tr >= 0)
                rtcSendMessage(out->audio_tr, (const char *)pkt, 12 + plen);
        } else {
            fprintf(stderr,
                    "[WebRTC] Opus packet too large (%d bytes), dropping\n",
                    plen);
        }
        av_packet_unref(out->opus_pkt);
    }

shift:
    out->audio_rtp_ts += OPUS_FRAME_SIZE;

    /* Shift remaining samples to the front of the buffer */
    int remain = out->audio_buf_n - OPUS_FRAME_SIZE;
    if (remain > 0) {
        memmove(out->audio_buf,
                out->audio_buf + (size_t)(OPUS_FRAME_SIZE * OPUS_CHANNELS),
                (size_t)(remain * OPUS_CHANNELS) * sizeof(float));
    }
    out->audio_buf_n = (remain > 0) ? remain : 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

struct webrtc_output *webrtc_output_create(int http_port)
{
    rtcInitLogger(RTC_LOG_WARNING, NULL);

    struct webrtc_output *out =
        (struct webrtc_output *)calloc(1, sizeof(struct webrtc_output));
    if (!out) return NULL;

    out->http_port   = http_port;
    out->pc          = -1;
    out->video_tr    = -1;
    out->audio_tr    = -1;
    out->running     = true;
    out->video_pt    = H264_PT_DEFAULT;
    out->audio_pt    = OPUS_PT_DEFAULT;
    out->http_listen = INVALID_SOCK;

    /* Cryptographically secure random SSRCs (low bits forced non-zero) */
    uint32_t rnd[2] = {0, 0};
    secure_random((unsigned char *)rnd, sizeof(rnd));
    out->video_ssrc = rnd[0] | 1u;
    out->audio_ssrc = rnd[1] | 1u;

    mutex_init(&out->lock);

    out->gather_evt = event_create();
    if (!out->gather_evt) goto fail;

    if (!opus_encoder_init(out)) goto fail;

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    out->http_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (out->http_listen == INVALID_SOCK) {
        fprintf(stderr, "[WebRTC] socket() failed\n");
        goto fail;
    }

    {
        int opt = 1;
        setsockopt(out->http_listen, SOL_SOCKET, SO_REUSEADDR,
                   (const char *)&opt, sizeof(opt));

        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family      = AF_INET;
        a.sin_port        = htons((unsigned short)http_port);
        a.sin_addr.s_addr = INADDR_ANY;

        if (bind(out->http_listen, (struct sockaddr *)&a, sizeof(a)) != 0) {
            fprintf(stderr, "[WebRTC] bind() failed on port %d\n", http_port);
            goto fail;
        }
        if (listen(out->http_listen, 4) != 0) {
            fprintf(stderr, "[WebRTC] listen() failed\n");
            goto fail;
        }
        sock_set_nonblock(out->http_listen);
    }

    thread_start(http_thread, out);

    fprintf(stdout,
            "[WebRTC] Ready — open http://localhost:%d/ in a browser"
            " for < 100 ms latency\n",
            http_port);
    return out;

fail:
    webrtc_output_destroy(out);
    return NULL;
}

void webrtc_output_destroy(struct webrtc_output *out)
{
    if (!out) return;

    out->running = false;

    mutex_lock(&out->lock);
    if (out->pc >= 0) {
        rtcDeletePeerConnection(out->pc);
        out->pc = out->video_tr = out->audio_tr = -1;
        out->pc_open = false;
    }
    mutex_unlock(&out->lock);

    if (out->http_listen != INVALID_SOCK)
        sock_close(out->http_listen);

    opus_encoder_destroy(out);

    if (out->gather_evt) event_destroy(out->gather_evt);
    mutex_destroy(&out->lock);
    free(out);
}

void webrtc_output_write_video(struct webrtc_output *out,
                               const uint8_t *data, size_t size,
                               int64_t pts_us)
{
    if (!out || !data || size == 0) return;

    mutex_lock(&out->lock);
    if (out->pc_open && out->video_tr >= 0) {
        /* Convert microsecond PTS to 90 kHz RTP timestamp */
        uint32_t ts = (uint32_t)
            ((pts_us * (int64_t)H264_CLOCK_RATE) / 1000000LL);
        rtp_send_h264(out, data, size, ts);
    }
    mutex_unlock(&out->lock);
}

void webrtc_output_write_audio(struct webrtc_output *out,
                               const float *pcm, int samples,
                               int channels, int sample_rate,
                               int64_t pts_us)
{
    (void)pts_us; /* RTP timestamp driven by sample counter, not input PTS */

    if (!out || !pcm || samples <= 0) return;

    mutex_lock(&out->lock);

    if (!out->pc_open || out->audio_tr < 0) {
        mutex_unlock(&out->lock);
        return;
    }

    if (!ensure_swr(out, sample_rate, channels)) {
        mutex_unlock(&out->lock);
        return;
    }

    /* Resample: interleaved float at in_rate → interleaved float at 48 kHz.
     * RESAMPLE_TMP_CAP covers 4× one Opus frame after rate conversion from
     * 44100 Hz (worst case: 960 * 48000/44100 ≈ 1045 samples per call).
     * For unusually large input blocks the excess is simply clipped at
     * AUDIO_BUF_CAP when appending to the accumulation buffer. */
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

        /* Encode as many complete Opus frames as available */
        while (out->audio_buf_n >= OPUS_FRAME_SIZE)
            flush_opus_frame(out);
    }

    mutex_unlock(&out->lock);
}
