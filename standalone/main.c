/*
 * main.c — airplay-stream.exe
 *
 * Standalone AirPlay receiver that streams to browsers via mediasoup SFU.
 *
 * Prerequisites:
 *   cd mediasoup-server && npm install && node server.js [PORT=8888]
 *
 * Then run:
 *   airplay-stream [--name <name>] [--port <port>]
 *                  [--width <w>] [--height <h>] [--fps <fps>]
 *
 * Then open in any modern browser:
 *   http://localhost:8888/
 *
 * Press Ctrl+C to stop.
 */

#include "airplay-stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <debugapi.h>
static volatile int g_running = 1;
static BOOL WINAPI ctrl_handler(DWORD type)
{
    (void)type;
    g_running = 0;
    return TRUE;
}
static LONG WINAPI unhandled_exception_filter(LPEXCEPTION_POINTERS info)
{
    (void)info;
    fprintf(stderr, "[ERROR] Unhandled exception in background thread. This is likely a bug in the application.\n");
    fflush(stderr);
    /* Exit gracefully instead of crashing */
    g_running = 0;
    return EXCEPTION_EXECUTE_HANDLER;
}
static void setup_signal(void)
{
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
    SetUnhandledExceptionFilter(unhandled_exception_filter);
}
static void wait_for_exit(void)
{
    while (g_running)
        Sleep(200);
}
#else
#  include <signal.h>
#  include <unistd.h>
static volatile int g_running = 1;
static void sig_handler(int sig) { (void)sig; g_running = 0; }
static void setup_signal(void)
{
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
}
static void wait_for_exit(void)
{
    while (g_running)
        usleep(200000);
}
#endif

/* ------------------------------------------------------------------ */
/* Argument parsing                                                     */
/* ------------------------------------------------------------------ */

static void print_usage(const char *prog)
{
    fprintf(stdout,
        "Usage: %s [options]\n"
        "\n"
        "Prerequisites:\n"
        "  cd mediasoup-server && npm install && node server.js\n"
        "  (set PORT=<port> env var to match --port below)\n"
        "\n"
        "Options:\n"
        "  --name  <name>   AirPlay server name shown on Apple devices\n"
        "                   (default: \"AirPlay Stream\")\n"
        "  --port  <port>   Port of the mediasoup signalling server\n"
        "                   (default: 8888). Open http://localhost:<port>/\n"
        "                   in any modern browser for < 100 ms latency.\n"
        "  --width  <px>    Requested video width  (default: device native)\n"
        "  --height <px>    Requested video height (default: device native)\n"
        "  --fps    <fps>   Requested frame rate   (default: 60)\n"
        "  --video-mode <mode>\n"
        "                   Video path mode: passthrough | transcode-auto\n"
        "                   (default: passthrough)\n"
        "  --video-encoder <enc>\n"
        "                   Preferred H264 encoder when --video-mode\n"
        "                   transcode-auto is used:\n"
        "                   auto | nvenc | qsv | amf | videotoolbox |\n"
        "                   libx264 | software (default: auto)\n"
        "  --hw-accel       Enable hardware-accelerated audio codec\n"
        "                   (Windows: uses aac_mf via Media Foundation,\n"
        "                    falls back to software if unavailable)\n"
        "  --help           Show this help message\n"
        "\n"
        "Open in browser: http://localhost:<port>/\n",
        prog);
}

static const char *video_mode_to_string(airplay_video_mode_t mode)
{
    switch (mode) {
    case AIRPLAY_VIDEO_MODE_TRANSCODE_AUTO: return "transcode-auto";
    case AIRPLAY_VIDEO_MODE_PASSTHROUGH:
    default: return "passthrough";
    }
}

static const char *video_encoder_pref_to_string(airplay_video_encoder_preference_t pref)
{
    switch (pref) {
    case AIRPLAY_VIDEO_ENCODER_NVENC: return "nvenc";
    case AIRPLAY_VIDEO_ENCODER_QSV: return "qsv";
    case AIRPLAY_VIDEO_ENCODER_AMF: return "amf";
    case AIRPLAY_VIDEO_ENCODER_VIDEOTOOLBOX: return "videotoolbox";
    case AIRPLAY_VIDEO_ENCODER_LIBX264: return "libx264";
    case AIRPLAY_VIDEO_ENCODER_SOFTWARE: return "software";
    case AIRPLAY_VIDEO_ENCODER_AUTO:
    default: return "auto";
    }
}

static int parse_args(int argc, char **argv,
                      struct airplay_stream_config *cfg)
{
    /* Defaults */
    strncpy(cfg->server_name, "AirPlay Stream",
            sizeof(cfg->server_name) - 1);
    cfg->webrtc_port = 8888;
    cfg->width       = 0;   /* device native */
    cfg->height      = 0;
    cfg->fps         = 60;
    cfg->video_mode  = AIRPLAY_VIDEO_MODE_PASSTHROUGH;
    cfg->video_encoder_preference = AIRPLAY_VIDEO_ENCODER_AUTO;
    cfg->hw_accel    = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 ||
            strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return -1;
        }
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            strncpy(cfg->server_name, argv[++i],
                    sizeof(cfg->server_name) - 1);
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            cfg->webrtc_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            cfg->width = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            cfg->height = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            cfg->fps = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--video-mode") == 0 && i + 1 < argc) {
            const char *mode = argv[++i];
            if (strcmp(mode, "passthrough") == 0) {
                cfg->video_mode = AIRPLAY_VIDEO_MODE_PASSTHROUGH;
            } else if (strcmp(mode, "transcode-auto") == 0) {
                cfg->video_mode = AIRPLAY_VIDEO_MODE_TRANSCODE_AUTO;
            } else {
                fprintf(stderr, "Invalid --video-mode: %s\n", mode);
                return 1;
            }
        } else if (strcmp(argv[i], "--video-encoder") == 0 && i + 1 < argc) {
            const char *enc = argv[++i];
            if (strcmp(enc, "auto") == 0) {
                cfg->video_encoder_preference = AIRPLAY_VIDEO_ENCODER_AUTO;
            } else if (strcmp(enc, "nvenc") == 0) {
                cfg->video_encoder_preference = AIRPLAY_VIDEO_ENCODER_NVENC;
            } else if (strcmp(enc, "qsv") == 0) {
                cfg->video_encoder_preference = AIRPLAY_VIDEO_ENCODER_QSV;
            } else if (strcmp(enc, "amf") == 0) {
                cfg->video_encoder_preference = AIRPLAY_VIDEO_ENCODER_AMF;
            } else if (strcmp(enc, "videotoolbox") == 0) {
                cfg->video_encoder_preference = AIRPLAY_VIDEO_ENCODER_VIDEOTOOLBOX;
            } else if (strcmp(enc, "libx264") == 0) {
                cfg->video_encoder_preference = AIRPLAY_VIDEO_ENCODER_LIBX264;
            } else if (strcmp(enc, "software") == 0) {
                cfg->video_encoder_preference = AIRPLAY_VIDEO_ENCODER_SOFTWARE;
            } else {
                fprintf(stderr, "Invalid --video-encoder: %s\n", enc);
                return 1;
            }
        } else if (strcmp(argv[i], "--hw-accel") == 0) {
            cfg->hw_accel = true;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    struct airplay_stream_config cfg;
    int rc = parse_args(argc, argv, &cfg);
    if (rc != 0)
        return (rc < 0) ? 0 : rc; /* -1 = --help (success), >0 = error */

    setup_signal();

    char res_str[32];
    if (cfg.width > 0 && cfg.height > 0)
        snprintf(res_str, sizeof(res_str), "%dx%d", cfg.width, cfg.height);
    else
        snprintf(res_str, sizeof(res_str), "device native");

    fprintf(stdout,
            "airplay-stream — AirPlay to mediasoup WebRTC streamer\n"
            "-------------------------------------------------------\n"
            "Server name    : %s\n"
            "mediasoup port : %d\n"
            "Resolution     : %s\n"
            "FPS            : %d\n"
            "Video mode     : %s\n"
            "Video encoder  : %s\n"
            "HW accel       : %s\n\n",
            cfg.server_name,
            cfg.webrtc_port,
            res_str,
            cfg.fps,
            video_mode_to_string(cfg.video_mode),
            video_encoder_pref_to_string(cfg.video_encoder_preference),
            cfg.hw_accel ? "enabled (aac_mf)" : "disabled (software)");

    if (!airplay_stream_start(&cfg)) {
        fprintf(stderr, "Failed to start AirPlay stream server.\n");
        return 1;
    }

    fprintf(stdout,
            "\nReady.  Open in browser:\n"
            "  http://localhost:%d/\n\n"
            "Press Ctrl+C to stop.\n\n",
            cfg.webrtc_port);

    wait_for_exit();

    airplay_stream_stop();
    return 0;
}
