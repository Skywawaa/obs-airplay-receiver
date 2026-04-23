/*
 * webrtc-output.c
 * LiveKit WHIP publisher — low-latency H.264 + Opus browser playback via SFU.
 *
 * Architecture:
 *   - A publisher thread maintains a WHIP PeerConnection to a LiveKit SFU.
 *     On start / disconnect: creates a sendonly PC, gathers ICE locally,
 *     POSTs the SDP offer to the LiveKit WHIP endpoint (Bearer JWT), sets
 *     the returned SDP answer, and monitors the connection.  On any failure
 *     or disconnect it retries automatically — browser viewers are unaffected.
 *   - A background HTTP server on http_port serves:
 *       GET /        -> embedded LiveKit JS SDK player page
 *       GET /token   -> JSON {"url":"ws://...","token":"<subscriber JWT>"}
 *   - Video (H.264 Annex-B) is packetised into RTP per RFC 6184.
 *   - Audio (float32 PCM, typically 44100 Hz) is resampled to 48 kHz,
 *     encoded to Opus, and packetised per RFC 7587.
 *
 * Requires libdatachannel >= 0.17, FFmpeg (Opus + SWR), OpenSSL >= 1.1.
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
#  include <netdb.h>
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
/* OpenSSL (HMAC-SHA256 for JWT signing; RAND_bytes for SSRCs)         */
/* ------------------------------------------------------------------ */

#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

/* ------------------------------------------------------------------ */
/* libdatachannel C API (ICE / DTLS / SRTP transport)                  */
/* ------------------------------------------------------------------ */

#include <rtc/rtc.h>

/* ------------------------------------------------------------------ */
/* Constants                                                            */
/* ------------------------------------------------------------------ */

#define RTP_MAX_PAYLOAD  1200   /* max bytes per RTP payload */
#define H264_PT_DEFAULT  96     /* dynamic PT for H.264 */
#define OPUS_PT_DEFAULT  111    /* dynamic PT for Opus   */
#define H264_CLOCK_RATE  90000  /* Hz */
#define OPUS_SAMPLE_RATE 48000  /* Hz (WebRTC requirement) */
#define OPUS_FRAME_SIZE  960    /* samples/channel at 48 kHz = 20 ms */
#define OPUS_CHANNELS    2
#define AUDIO_BUF_FRAMES 8
#define AUDIO_BUF_CAP    (OPUS_FRAME_SIZE * AUDIO_BUF_FRAMES)

/* One video-frame offset at 60 fps in 90 kHz ticks (~33 ms).
 * Used to back-date injected keyframes so the decoder sees them
 * before the following P-frame. */
#define H264_FRAME_TICKS (H264_CLOCK_RATE / 60)

#define MAX_SDP_SIZE     65536

/* Temporary stack buffer for one SWR resample call (per-channel samples) */
#define RESAMPLE_TMP_CAP (OPUS_FRAME_SIZE * 4)

/* LiveKit room and publisher participant identity */
#define LIVEKIT_ROOM         "airplay"
#define LIVEKIT_PUB_IDENTITY "airplay-publisher"

/* ------------------------------------------------------------------ */
/* Base64URL encoding (RFC 4648 section 5, no padding)                 */
/* ------------------------------------------------------------------ */

static int b64url_encode(const unsigned char *src, size_t len,
                          char *dst, size_t cap)
{
    static const char T[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t o = 0;
    for (size_t i = 0; i < len; ) {
        unsigned int v = (unsigned int)src[i++] << 16;
        int n = 1;
        if (i < len) { v |= (unsigned int)src[i++] << 8; n++; }
        if (i < len) { v |= (unsigned int)src[i++];       n++; }
        if (o + 4 >= cap) return -1;
        dst[o++] = T[(v >> 18) & 63];
        dst[o++] = T[(v >> 12) & 63];
        if (n >= 2) dst[o++] = T[(v >>  6) & 63];
        if (n >= 3) dst[o++] = T[(v      ) & 63];
    }
    if (o >= cap) return -1;
    dst[o] = '\0';
    return (int)o;
}

/* ------------------------------------------------------------------ */
/* LiveKit JWT generation (HS256)                                       */
/* ------------------------------------------------------------------ */

/*
 * Build a signed LiveKit access token.
 *   can_publish=true  -> publisher token  (used by the C app WHIP client)
 *   can_subscribe=true -> subscriber token (returned to the browser viewer)
 * Returns byte count written to out (> 0) or -1 on error.
 */
static int livekit_jwt(const char *api_key, const char *api_secret,
                        const char *room,    const char *identity,
                        bool can_publish,    bool can_subscribe,
                        char *out,           size_t out_cap)
{
    /* Header */
    static const char HDR[] = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
    char hdr_b64[64];
    if (b64url_encode((const unsigned char *)HDR, sizeof(HDR) - 1,
                      hdr_b64, sizeof(hdr_b64)) < 0)
        return -1;

    /* Payload */
    time_t now = time(NULL);
    char payload[1024];
    int plen = snprintf(payload, sizeof(payload),
        "{"
        "\"iss\":\"%s\","
        "\"sub\":\"%s\","
        "\"nbf\":%ld,"
        "\"exp\":%ld,"
        "\"video\":{"
            "\"room\":\"%s\","
            "\"roomJoin\":true,"
            "\"canPublish\":%s,"
            "\"canSubscribe\":%s"
        "}"
        "}",
        api_key, identity,
        (long)now, (long)(now + 86400L), /* 24 h validity */
        room,
        can_publish  ? "true" : "false",
        can_subscribe ? "true" : "false");
    if (plen <= 0 || plen >= (int)sizeof(payload)) return -1;

    char payload_b64[1024];
    if (b64url_encode((const unsigned char *)payload, (size_t)plen,
                      payload_b64, sizeof(payload_b64)) < 0)
        return -1;

    /* Signing input: <hdr_b64>.<payload_b64> */
    char signing[1536];
    int  slen = snprintf(signing, sizeof(signing),
                         "%s.%s", hdr_b64, payload_b64);
    if (slen <= 0 || slen >= (int)sizeof(signing)) return -1;

    /* HMAC-SHA256 */
    unsigned char sig[32];
    unsigned int  sig_len = 0;
    HMAC(EVP_sha256(),
         api_secret, (int)strlen(api_secret),
         (const unsigned char *)signing, (size_t)slen,
         sig, &sig_len);

    char sig_b64[64];
    if (b64url_encode(sig, sig_len, sig_b64, sizeof(sig_b64)) < 0)
        return -1;

    int n = snprintf(out, out_cap, "%s.%s", signing, sig_b64);
    return (n > 0 && (size_t)n < out_cap) ? n : -1;
}

/* ------------------------------------------------------------------ */
/* Embedded HTML player (LiveKit JS SDK)                               */
/* ------------------------------------------------------------------ */

/*
 * The player fetches /token to get the LiveKit WebSocket URL and a
 * freshly-signed subscriber JWT.  The LiveKit SDK handles reconnection
 * transparently -- no page reload required on publisher restarts or
 * brief network interruptions.
 */
static const char HTML_PLAYER[] =
"<!DOCTYPE html>\n"
"<html lang=\"en\">\n"
"<head>\n"
"<meta charset=\"UTF-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
"<title>AirPlay Stream</title>\n"
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
"<audio id=\"a\" autoplay style=\"display:none\"></audio>\n"
"<div id=\"s\">Connecting\u2026</div>\n"
"<script type=\"module\">\n"
"import{Room,RoomEvent,Track}\n"
"  from'https://cdn.jsdelivr.net/npm/livekit-client@2/dist/livekit-client.esm.mjs';\n"
"const s=document.getElementById('s');\n"
"const v=document.getElementById('v');\n"
"const a=document.getElementById('a');\n"
"async function connect(){\n"
"  s.style.opacity='1';s.textContent='Connecting\u2026';\n"
"  try{\n"
"    const r=await fetch('/token');\n"
"    if(!r.ok)throw new Error('token '+r.status);\n"
"    const{url,token}=await r.json();\n"
"    const room=new Room({adaptiveStream:false,dynacast:false});\n"
"    room.on(RoomEvent.TrackSubscribed,(track)=>{\n"
"      if(track.kind===Track.Kind.Video)track.attach(v);\n"
"      else if(track.kind===Track.Kind.Audio)track.attach(a);\n"
"    });\n"
"    room.on(RoomEvent.TrackUnsubscribed,(track)=>{track.detach();});\n"
"    room.on(RoomEvent.Connected,()=>{\n"
"      s.textContent='Connected';\n"
"      setTimeout(()=>{s.style.opacity='0';},2000);\n"
"    });\n"
"    room.on(RoomEvent.Reconnecting,()=>{\n"
"      s.style.opacity='1';s.textContent='Reconnecting\u2026';\n"
"    });\n"
"    room.on(RoomEvent.Reconnected,()=>{\n"
"      s.textContent='Connected';\n"
"      setTimeout(()=>{s.style.opacity='0';},2000);\n"
"    });\n"
"    room.on(RoomEvent.Disconnected,()=>{\n"
"      s.style.opacity='1';\n"
"      s.textContent='Disconnected \u2014 retrying\u2026';\n"
"      setTimeout(connect,3000);\n"
"    });\n"
"    await room.connect(url,token,{autoSubscribe:true});\n"
"  }catch(e){\n"
"    s.style.opacity='1';\n"
"    s.textContent='Error: '+e.message+' \u2014 retrying\u2026';\n"
"    setTimeout(connect,3000);\n"
"  }\n"
"}\n"
"connect();\n"
"</script>\n"
"</body>\n"
"</html>\n";

/* ------------------------------------------------------------------ */
/* Cryptographically secure random bytes                                */
/* ------------------------------------------------------------------ */

static void secure_random(unsigned char *buf, size_t size)
{
    if (RAND_bytes(buf, (int)size) == 1)
        return;
#ifdef _WIN32
    BCryptGenRandom(NULL, buf, (ULONG)size, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) { (void)fread(buf, 1, size, f); fclose(f); }
#endif
}

/* ------------------------------------------------------------------ */
/* webrtc_output state                                                  */
/* ------------------------------------------------------------------ */

struct webrtc_output {
    /* Configuration */
    int  http_port;
    char livekit_url[512];    /* HTTP URL, e.g. "http://localhost:7880" */
    char livekit_ws_url[512]; /* WS  URL, e.g. "ws://localhost:7880"   */
    char api_key[128];
    char api_secret[256];

    wrtc_mutex_t  lock;
    volatile bool running;

    /* HTTP server socket */
    sock_t        http_listen;

    /* Active libdatachannel peer connection (-1 = none) */
    int           pc;
    int           video_tr;
    int           audio_tr;
    bool          pc_open;       /* true once ICE/DTLS connected to SFU */
    wrtc_event_t *gather_evt;    /* signalled when ICE gathering complete */
    wrtc_event_t *connect_evt;   /* signalled on any connection state change */

    /* RTP state */
    int           video_pt;
    int           audio_pt;
    uint16_t      video_seq;
    uint16_t      audio_seq;
    uint32_t      video_ssrc;
    uint32_t      audio_ssrc;

    /* Opus encoder (FFmpeg) */
    const AVCodec   *opus_codec;
    AVCodecContext  *opus_ctx;
    AVFrame         *opus_frame;
    AVPacket        *opus_pkt;

    /* SWR resampler: input rate -> 48 kHz */
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
/* URL helpers                                                          */
/* ------------------------------------------------------------------ */

/*
 * Parse "http[s]://host[:port][/path]" into separate components.
 * *port defaults to 7880 (LiveKit default) when not present in the URL.
 * Returns true on success.
 */
static bool url_parse(const char *url,
                      char *host, size_t hcap,
                      int  *port,
                      char *path, size_t pcap)
{
    const char *p = url;
    int scheme_default = 7880; /* LiveKit default */

    if (strncmp(p, "http://", 7) == 0)       { p += 7; scheme_default = 80; }
    else if (strncmp(p, "https://", 8) == 0) { p += 8; scheme_default = 443; }

    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');

    if (colon && (!slash || colon < slash)) {
        /* explicit host:port */
        size_t hlen = (size_t)(colon - p);
        if (hlen >= hcap) hlen = hcap - 1;
        memcpy(host, p, hlen);
        host[hlen] = '\0';
        *port = atoi(colon + 1);
    } else {
        /* host only — fall back to scheme-based default */
        size_t hlen = slash ? (size_t)(slash - p) : strlen(p);
        if (hlen >= hcap) hlen = hcap - 1;
        memcpy(host, p, hlen);
        host[hlen] = '\0';
        *port = scheme_default;
    }

    if (slash) {
        strncpy(path, slash, pcap - 1);
        path[pcap - 1] = '\0';
    } else {
        strncpy(path, "/", pcap - 1);
        path[pcap - 1] = '\0';
    }

    return host[0] != '\0';
}

/*
 * Derive the LiveKit WebSocket URL from the HTTP URL:
 *   "http://"  -> "ws://"
 *   "https://" -> "wss://"
 * Path is stripped -- the JS SDK manages its own WebSocket paths.
 */
static void derive_ws_url(const char *http_url, char *ws_url, size_t cap)
{
    char host[256]; int port; char path[512];
    if (!url_parse(http_url, host, sizeof(host), &port, path, sizeof(path))) {
        snprintf(ws_url, cap, "ws://localhost:7880");
        return;
    }
    const char *scheme = (strncmp(http_url, "https://", 8) == 0) ? "wss" : "ws";
    snprintf(ws_url, cap, "%s://%s:%d", scheme, host, port);
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
/* H.264 RTP packetisation -- RFC 6184                                 */
/* ------------------------------------------------------------------ */

/*
 * Return true if the Annex-B buffer contains at least one IDR NAL unit
 * (NAL type 5).
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
    if (nal_size <= 0 || out->video_tr < 0) return;

    /* Stack-allocated packet buffer: 12 (RTP hdr) + 2 (FU-A hdr) + payload */
    uint8_t pkt[12 + 2 + RTP_MAX_PAYLOAD];
    int      pt   = out->video_pt;
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
            starts[nals]  = i + (size_t)sc;
            sc_lens[nals] = (uint8_t)sc;
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
/* libdatachannel callbacks                                             */
/* ------------------------------------------------------------------ */

static void on_gathering_state(int pc, rtcGatheringState state, void *ptr)
{
    struct webrtc_output *out = (struct webrtc_output *)ptr;
    if (state != RTC_GATHERING_COMPLETE)
        return;
    /* Only signal for the current peer connection to avoid stale wakeups. */
    mutex_lock(&out->lock);
    bool current = (out->pc == pc);
    mutex_unlock(&out->lock);
    if (current)
        event_signal(out->gather_evt);
}

static void on_pc_state(int pc, rtcState state, void *ptr)
{
    struct webrtc_output *out = (struct webrtc_output *)ptr;
    mutex_lock(&out->lock);
    /* Ignore callbacks from a previous (already replaced) peer connection. */
    if (out->pc != pc) {
        mutex_unlock(&out->lock);
        return;
    }
    if (state == RTC_CONNECTED) {
        out->pc_open        = true;
        out->needs_keyframe = true;
        mutex_unlock(&out->lock);
        fprintf(stdout, "[WebRTC] Connected to LiveKit SFU -- publishing\n");
        event_signal(out->connect_evt);
    } else if (state == RTC_FAILED ||
               state == RTC_DISCONNECTED ||
               state == RTC_CLOSED) {
        bool was_open = out->pc_open;
        out->pc_open = false;
        mutex_unlock(&out->lock);
        fprintf(stdout, "[WebRTC] Connection %s (state=%d)\n",
                was_open ? "lost" : "failed", (int)state);
        event_signal(out->connect_evt);
    } else {
        mutex_unlock(&out->lock);
    }
}

/* ------------------------------------------------------------------ */
/* HTTP helpers (shared by WHIP client and HTTP server)                */
/* ------------------------------------------------------------------ */

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

/* Read from socket until \r\n\r\n is found; returns bytes read or -1. */
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

/* ------------------------------------------------------------------ */
/* WHIP HTTP client: POST SDP offer to LiveKit, receive SDP answer     */
/* ------------------------------------------------------------------ */

/*
 * POST offer_sdp to url/whip with a Bearer JWT.
 * Writes the SDP answer into answer_out (null-terminated).
 * Returns true on success (HTTP 200/201 with a non-empty body).
 */
static bool whip_post(const char *url, const char *token,
                      const char *offer_sdp,
                      char *answer_out, int answer_cap)
{
    char host[256]; int port; char path[512];
    if (!url_parse(url, host, sizeof(host), &port, path, sizeof(path))) {
        fprintf(stderr, "[WebRTC] Invalid WHIP URL: %s\n", url);
        return false;
    }

    /* Use /whip as the WHIP endpoint when no specific path is given */
    if (strcmp(path, "/") == 0)
        strncpy(path, "/whip", sizeof(path) - 1);

    /* Resolve hostname */
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        fprintf(stderr, "[WebRTC] DNS resolve failed for %s\n", host);
        return false;
    }

    sock_t s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCK) { freeaddrinfo(res); return false; }

    if (connect(s, res->ai_addr, (socklen_t)res->ai_addrlen) != 0) {
        fprintf(stderr, "[WebRTC] Cannot connect to %s:%d\n", host, port);
        freeaddrinfo(res);
        sock_close(s);
        return false;
    }
    freeaddrinfo(res);

    /* 10-second send/receive timeouts */
#ifdef _WIN32
    DWORD tv_ms = 10000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv_ms, sizeof(tv_ms));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv_ms, sizeof(tv_ms));
#else
    struct timeval tv = {10, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    int offer_len = (int)strlen(offer_sdp);

    /* Build request headers (token ~400 bytes; 2048 is ample) */
    char req_hdr[2048];
    int  hdr_len = snprintf(req_hdr, sizeof(req_hdr),
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Authorization: Bearer %s\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host, port, token, offer_len);

    if (hdr_len <= 0 || hdr_len >= (int)sizeof(req_hdr)) {
        sock_close(s);
        return false;
    }

    if (send(s, req_hdr, hdr_len, 0) < 0 ||
        send(s, offer_sdp, offer_len, 0) < 0) {
        sock_close(s);
        return false;
    }

    /* Read response headers */
    char resp_hdr[4096];
    int  resp_n = recv_headers(s, resp_hdr, (int)sizeof(resp_hdr));
    if (resp_n < 0) {
        fprintf(stderr, "[WebRTC] No response from WHIP endpoint\n");
        sock_close(s);
        return false;
    }

    /* Verify HTTP status (201 Created or 200 OK) */
    int status = 0;
    if (sscanf(resp_hdr, "HTTP/%*s %d", &status) != 1 ||
        (status != 200 && status != 201)) {
        fprintf(stderr, "[WebRTC] WHIP POST returned HTTP %d\n", status);
        sock_close(s);
        return false;
    }

    int body_len = parse_content_length(resp_hdr);

    /* Recover body bytes already read into the header buffer */
    int received = 0;
    const char *hdr_end = strstr(resp_hdr, "\r\n\r\n");
    if (hdr_end) {
        const char *body_start = hdr_end + 4;
        int prefetched = resp_n - (int)(body_start - resp_hdr);
        if (prefetched > 0) {
            if (prefetched > answer_cap - 1) prefetched = answer_cap - 1;
            memcpy(answer_out, body_start, (size_t)prefetched);
            received = prefetched;
        }
    }

    /* Read remaining body */
    while (received < body_len && received < answer_cap - 1) {
        int r = (int)recv(s, answer_out + received,
                          (size_t)(answer_cap - 1 - received), 0);
        if (r <= 0) break;
        received += r;
    }
    answer_out[received] = '\0';

    sock_close(s);

    if (received == 0) {
        fprintf(stderr, "[WebRTC] WHIP response had empty body\n");
        return false;
    }
    return true;
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
 * Ensure the SWR resampler is configured for (in_rate, in_ch) -> 48 kHz stereo.
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
        int     plen = out->opus_pkt->size;
        uint8_t pkt[12 + 1276]; /* max Opus payload per RFC 7587 */
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
/* Publisher thread -- WHIP lifecycle + auto-reconnect                 */
/* ------------------------------------------------------------------ */

static void publisher_thread(void *arg)
{
    struct webrtc_output *out = (struct webrtc_output *)arg;

    while (out->running) {
        /* --- Tear down any existing peer connection --- */
        mutex_lock(&out->lock);
        int old_pc        = out->pc;
        out->pc           = -1;
        out->video_tr     = -1;
        out->audio_tr     = -1;
        out->pc_open      = false;
        out->audio_rtp_ts = 0;
        out->audio_buf_n  = 0;
        if (out->opus_ctx)
            avcodec_flush_buffers(out->opus_ctx);
        mutex_unlock(&out->lock);

        if (old_pc >= 0)
            rtcDeletePeerConnection(old_pc);

        /* --- Create new sendonly peer connection --- */
        rtcConfiguration cfg;
        memset(&cfg, 0, sizeof(cfg));
        /* Disable auto-negotiation so we control offer/answer timing */
        cfg.disableAutoNegotiation = true;

        int pc = rtcCreatePeerConnection(&cfg);
        if (pc < 0) {
            fprintf(stderr,
                    "[WebRTC] rtcCreatePeerConnection failed (%d)\n", pc);
            SLEEP_MS(3000);
            continue;
        }

        rtcSetUserPointer(pc, out);
        rtcSetStateChangeCallback(pc, on_pc_state);
        rtcSetGatheringStateChangeCallback(pc, on_gathering_state);

        /* Add sendonly video and audio tracks with fixed payload types.
         * Using mids "0" and "1" matches standard WebRTC bundle practice. */
        char vdesc[512], adesc[512];
        snprintf(vdesc, sizeof(vdesc),
            "video 9 UDP/TLS/RTP/SAVPF %d\r\n"
            "a=mid:0\r\n"
            "a=rtpmap:%d H264/90000\r\n"
            "a=fmtp:%d level-asymmetry-allowed=1;packetization-mode=1;"
                "profile-level-id=42e01f\r\n"
            "a=sendonly\r\n"
            "a=ssrc:%u cname:airplay\r\n",
            H264_PT_DEFAULT, H264_PT_DEFAULT, H264_PT_DEFAULT,
            out->video_ssrc);
        snprintf(adesc, sizeof(adesc),
            "audio 9 UDP/TLS/RTP/SAVPF %d\r\n"
            "a=mid:1\r\n"
            "a=rtpmap:%d opus/48000/2\r\n"
            "a=fmtp:%d minptime=10;useinbandfec=1\r\n"
            "a=sendonly\r\n"
            "a=ssrc:%u cname:airplay\r\n",
            OPUS_PT_DEFAULT, OPUS_PT_DEFAULT, OPUS_PT_DEFAULT,
            out->audio_ssrc);

        int vtr = rtcAddTrack(pc, vdesc);
        int atr = rtcAddTrack(pc, adesc);
        if (vtr < 0 || atr < 0) {
            fprintf(stderr,
                    "[WebRTC] rtcAddTrack failed (v=%d a=%d)\n", vtr, atr);
            rtcDeletePeerConnection(pc);
            SLEEP_MS(3000);
            continue;
        }

        /* Store handles before triggering ICE so on_gathering_state can
         * compare out->pc == pc and signal gather_evt correctly. */
        mutex_lock(&out->lock);
        out->pc       = pc;
        out->video_tr = vtr;
        out->audio_tr = atr;
        out->video_pt = H264_PT_DEFAULT;
        out->audio_pt = OPUS_PT_DEFAULT;
        mutex_unlock(&out->lock);

        /* Generate the offer and start ICE gathering */
        if (rtcSetLocalDescription(pc, "offer") < 0) {
            fprintf(stderr, "[WebRTC] rtcSetLocalDescription failed\n");
            mutex_lock(&out->lock);
            out->pc = out->video_tr = out->audio_tr = -1;
            mutex_unlock(&out->lock);
            rtcDeletePeerConnection(pc);
            SLEEP_MS(3000);
            continue;
        }

        /* Wait up to 5 s for full ICE candidate gathering */
        if (!event_wait_ms(out->gather_evt, 5000))
            fprintf(stderr, "[WebRTC] Warning: ICE gathering timed out\n");

        char *offer = (char *)malloc(MAX_SDP_SIZE);
        if (!offer || rtcGetLocalDescription(pc, offer, MAX_SDP_SIZE) < 0) {
            fprintf(stderr, "[WebRTC] rtcGetLocalDescription failed\n");
            free(offer);
            mutex_lock(&out->lock);
            out->pc = out->video_tr = out->audio_tr = -1;
            mutex_unlock(&out->lock);
            rtcDeletePeerConnection(pc);
            SLEEP_MS(3000);
            continue;
        }

        /* Sign a publisher JWT and POST the offer to the WHIP endpoint */
        char pub_token[2048];
        if (livekit_jwt(out->api_key, out->api_secret,
                        LIVEKIT_ROOM, LIVEKIT_PUB_IDENTITY,
                        true, false,
                        pub_token, sizeof(pub_token)) < 0) {
            fprintf(stderr, "[WebRTC] JWT generation failed\n");
            free(offer);
            mutex_lock(&out->lock);
            out->pc = out->video_tr = out->audio_tr = -1;
            mutex_unlock(&out->lock);
            rtcDeletePeerConnection(pc);
            SLEEP_MS(3000);
            continue;
        }

        char *answer = (char *)malloc(MAX_SDP_SIZE);
        bool  ok = answer && whip_post(out->livekit_url, pub_token,
                                       offer, answer, MAX_SDP_SIZE);
        free(offer);

        if (!ok) {
            fprintf(stderr,
                    "[WebRTC] WHIP publish to %s failed -- "
                    "is LiveKit running? Retrying in 3 s\n",
                    out->livekit_url);
            free(answer);
            mutex_lock(&out->lock);
            out->pc = out->video_tr = out->audio_tr = -1;
            mutex_unlock(&out->lock);
            rtcDeletePeerConnection(pc);
            SLEEP_MS(3000);
            continue;
        }

        /* Apply the SDP answer received from LiveKit */
        if (rtcSetRemoteDescription(pc, answer, "answer") < 0) {
            fprintf(stderr,
                    "[WebRTC] rtcSetRemoteDescription (answer) failed\n");
            free(answer);
            mutex_lock(&out->lock);
            out->pc = out->video_tr = out->audio_tr = -1;
            mutex_unlock(&out->lock);
            rtcDeletePeerConnection(pc);
            SLEEP_MS(3000);
            continue;
        }
        free(answer);

        fprintf(stdout,
                "[WebRTC] WHIP offer sent to %s -- waiting for ICE\n",
                out->livekit_url);

        /* Wait for connection or failure (up to 10 s) */
        if (!event_wait_ms(out->connect_evt, 10000))
            fprintf(stderr, "[WebRTC] Warning: ICE connection timed out\n");

        /* Monitor the connection until it drops or we are asked to stop */
        while (out->running) {
            mutex_lock(&out->lock);
            bool open = out->pc_open && (out->pc == pc);
            mutex_unlock(&out->lock);
            if (!open) break;
            SLEEP_MS(500);
        }

        if (!out->running) break;

        fprintf(stdout, "[WebRTC] Connection lost -- reconnecting in 2 s\n");
        SLEEP_MS(2000);
    }

    fprintf(stdout, "[WebRTC] Publisher thread exiting\n");
}

/* ------------------------------------------------------------------ */
/* HTTP server thread (viewer page + /token endpoint)                  */
/* ------------------------------------------------------------------ */

static void handle_http_conn(struct webrtc_output *out, sock_t cs)
{
    char hbuf[2048];
    int  hbuf_n = recv_headers(cs, hbuf, (int)sizeof(hbuf));
    if (hbuf_n < 0) return;

    bool is_get = (strncmp(hbuf, "GET ", 4) == 0);
    if (!is_get) {
        http_respond(cs, 405, "text/plain", "Method Not Allowed", -1);
        return;
    }

    const char *path = hbuf + 4;
    const char *spc  = strchr(path, ' ');
    int plen = spc ? (int)(spc - path) : 0;

    /* GET / -> HTML player page (LiveKit JS SDK) */
    if (plen == 1 && path[0] == '/') {
        http_respond(cs, 200, "text/html; charset=UTF-8",
                     HTML_PLAYER, (int)(sizeof(HTML_PLAYER) - 1));
        return;
    }

    /* GET /token -> JSON {url, token} for the browser subscriber */
    if (plen == 6 && strncmp(path, "/token", 6) == 0) {
        /* Generate a unique subscriber identity with a random suffix so that
         * multiple simultaneous viewers get distinct participant IDs. */
        unsigned char rnd[4];
        secure_random(rnd, sizeof(rnd));
        char identity[32];
        snprintf(identity, sizeof(identity), "viewer-%02x%02x%02x%02x",
                 rnd[0], rnd[1], rnd[2], rnd[3]);

        char sub_token[2048];
        if (livekit_jwt(out->api_key, out->api_secret,
                        LIVEKIT_ROOM, identity,
                        false, true,
                        sub_token, sizeof(sub_token)) < 0) {
            http_respond(cs, 500, "text/plain", "JWT generation failed", -1);
            return;
        }

        char body[4096];
        int  blen = snprintf(body, sizeof(body),
                             "{\"url\":\"%s\",\"token\":\"%s\"}",
                             out->livekit_ws_url, sub_token);
        if (blen <= 0 || blen >= (int)sizeof(body)) {
            http_respond(cs, 500, "text/plain", "Response too large", -1);
            return;
        }
        http_respond(cs, 200, "application/json", body, blen);
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
/* Public API                                                           */
/* ------------------------------------------------------------------ */

struct webrtc_output *webrtc_output_create(int http_port,
                                           const char *livekit_url,
                                           const char *api_key,
                                           const char *api_secret)
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

    /* Store LiveKit configuration (fall back to dev defaults) */
    strncpy(out->livekit_url,
            livekit_url ? livekit_url : "http://localhost:7880",
            sizeof(out->livekit_url) - 1);
    strncpy(out->api_key,
            api_key     ? api_key     : "devkey",
            sizeof(out->api_key) - 1);
    strncpy(out->api_secret,
            api_secret  ? api_secret  : "secret",
            sizeof(out->api_secret) - 1);
    derive_ws_url(out->livekit_url,
                  out->livekit_ws_url, sizeof(out->livekit_ws_url));

    /* Cryptographically secure random SSRCs (low bits forced non-zero) */
    uint32_t rnd[2] = {0, 0};
    secure_random((unsigned char *)rnd, sizeof(rnd));
    out->video_ssrc = rnd[0] | 1u;
    out->audio_ssrc = rnd[1] | 1u;

    mutex_init(&out->lock);

    out->gather_evt  = event_create();
    out->connect_evt = event_create();
    if (!out->gather_evt || !out->connect_evt) goto fail;

    if (!opus_encoder_init(out)) goto fail;

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    /* Start the HTTP server for the viewer page and /token endpoint */
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
    thread_start(publisher_thread, out);

    fprintf(stdout,
            "[WebRTC] Ready -- open http://localhost:%d/ in a browser\n"
            "[WebRTC] Publishing to LiveKit at %s"
            " (room: " LIVEKIT_ROOM ")\n",
            http_port, out->livekit_url);
    return out;

fail:
    webrtc_output_destroy(out);
    return NULL;
}

void webrtc_output_destroy(struct webrtc_output *out)
{
    if (!out) return;

    out->running = false;

    /* Wake the publisher thread so it can exit its polling loop promptly */
    if (out->connect_evt) event_signal(out->connect_evt);

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
    free(out->keyframe_cache);

    if (out->gather_evt)  event_destroy(out->gather_evt);
    if (out->connect_evt) event_destroy(out->connect_evt);
    mutex_destroy(&out->lock);
    free(out);
}

void webrtc_output_write_video(struct webrtc_output *out,
                               const uint8_t *data, size_t size,
                               int64_t pts_us)
{
    if (!out || !data || size == 0) return;

    mutex_lock(&out->lock);

    /* Always cache the most recent IDR frame so a browser that connects
     * while the AirPlay stream is already in progress receives an
     * immediate picture without waiting for the next natural keyframe. */
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
                    " (%zu bytes); reconnecting browsers may not get"
                    " immediate video\n", size);
        }
    }

    if (out->pc_open && out->video_tr >= 0) {
        /* Convert microsecond PTS to 90 kHz RTP timestamp */
        uint32_t ts = (uint32_t)
            ((pts_us * (int64_t)H264_CLOCK_RATE) / 1000000LL);

        /* A freshly received IDR satisfies any pending reconnect request */
        if (is_idr)
            out->needs_keyframe = false;

        /* Inject the cached keyframe before the first non-IDR frame after
         * a (re)connect so the decoder can start producing output
         * immediately. */
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
    if (out->pc_open)
        out->needs_keyframe = true;
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

    /* Resample: interleaved float at in_rate -> interleaved float at 48 kHz */
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

        /* Encode as many complete Opus frames as are available */
        while (out->audio_buf_n >= OPUS_FRAME_SIZE)
            flush_opus_frame(out);
    }

    mutex_unlock(&out->lock);
}
