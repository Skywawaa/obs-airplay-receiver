/*
 * AirPlay Server - Main coordinator for the AirPlay receiver
 *
 * Manages:
 *   - mDNS advertisement (_airplay._tcp)
 *   - HTTP control server (port 7000)
 *   - Mirror stream receiver
 *
 * AirPlay Protocol Flow (screen mirroring):
 *   1. Device discovers us via mDNS
 *   2. Device connects to HTTP port
 *   3. GET /info - we return device capabilities
 *   4. POST /pair-setup / /pair-verify - authentication (simplified)
 *   5. POST /fp-setup - FairPlay setup
 *   6. SETUP (RTSP-like) - configure mirroring stream
 *   7. POST /stream - binary plist with stream config
 *   8. Device sends H.264 NALUs on a data channel
 */

#include "airplay-server.h"
#include "airplay-mirror.h"
#include "airplay-plist.h"
#include "bplist.h"
#include "fairplay.h"
#include "../log.h"
#include "../network/http-server.h"
#include "../network/mdns-publish.h"
#include "../network/net-utils.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

struct airplay_server {
	struct airplay_server_config config;
	struct http_server *http;
	struct mdns_publisher *mdns;
	struct airplay_mirror *mirror;
	struct fairplay *fp;

	uint8_t device_id[6]; /* MAC address as device ID */
	char device_id_str[18];
	bool connected;
};

/* ---------- Build /info response ---------- */

static void build_info_response(struct airplay_server *srv,
				struct http_response *resp)
{
	struct bplist_writer *bp = bplist_writer_create();
	if (!bp)
		return;

	bplist_begin_dict(bp, 14);

	bplist_dict_add_string(bp, "deviceid", srv->device_id_str);
	bplist_dict_add_string(bp, "model", "AppleTV5,3");
	bplist_dict_add_string(bp, "name", srv->config.name);
	bplist_dict_add_string(bp, "srcvers", "366.0");
	bplist_dict_add_string(bp, "vv", "2");

	/* Features bitmask - enable screen mirroring + audio */
	bplist_dict_add_int(bp, "features", 0x527FFFF7);
	bplist_dict_add_int(bp, "statusFlags", 0x44);

	bplist_dict_add_int(bp, "width", 1920);
	bplist_dict_add_int(bp, "height", 1080);

	bplist_dict_add_string(bp, "pi",
			       "b08f5a79-db29-4571-b11a-e3e1e03f1e89");
	bplist_dict_add_string(bp, "pk",
		"99FD4299889422515FBD27949E4E1E21B2AF50A454499E3D4BE75A4E0F55FE63");

	bplist_dict_add_string(bp, "osBuildVersion", "17K499");
	bplist_dict_add_string(bp, "protovers", "1.1");
	bplist_dict_add_int(bp, "keepAliveLowPower", 1);

	size_t plist_len = 0;
	uint8_t *plist_data = bplist_writer_finalize(bp, &plist_len);
	bplist_writer_destroy(bp);

	if (plist_data) {
		resp->body = plist_data;
		resp->body_length = plist_len;
		http_response_add_header(resp, "Content-Type",
					 "application/x-apple-binary-plist");
	}
}

/* ---------- Handle FairPlay setup (stub) ---------- */

static void handle_fp_setup(struct airplay_server *srv,
			     const struct http_request *req,
			     struct http_response *resp)
{
	if (!req->body || req->body_length < 16) {
		ap_warn("fp-setup: body too small (%zu bytes)",
			req->body_length);
		resp->status_code = 400;
		return;
	}

	size_t reply_len = 0;
	uint8_t *reply = fairplay_handle_setup(srv->fp, req->body,
					       req->body_length, &reply_len);

	if (reply && reply_len > 0) {
		resp->body = reply;
		resp->body_length = reply_len;
		http_response_add_header(resp, "Content-Type",
					 "application/octet-stream");
	} else {
		resp->status_code = 400;
	}
}

/* ---------- Handle pair-setup ---------- */

static void handle_pair_setup(struct airplay_server *srv,
			      const struct http_request *req,
			      struct http_response *resp)
{
	/*
	 * Pair-setup implements a variant of SRP (Secure Remote Password).
	 * For a basic implementation, we can return an empty success response
	 * for some older devices. Full pairing requires Ed25519 + SRP.
	 */
	resp->status_code = 200;
	resp->status_text = "OK";
	http_response_add_header(resp, "Content-Type",
				 "application/octet-stream");
}

/* ---------- Handle pair-verify ---------- */

static void handle_pair_verify(struct airplay_server *srv,
			       const struct http_request *req,
			       struct http_response *resp)
{
	/*
	 * Pair-verify is the verification step after pair-setup.
	 * It uses Curve25519 key exchange + Ed25519 signatures.
	 */
	resp->status_code = 200;
	resp->status_text = "OK";

	/* Return a minimal 32-byte response for phase 1 */
	if (req->body_length > 0 && req->body[0] == 1) {
		resp->body = calloc(1, 32);
		resp->body_length = 32;
	}

	http_response_add_header(resp, "Content-Type",
				 "application/octet-stream");
}

/* ---------- Mirror receiver setup helper ---------- */

static uint16_t ensure_mirror_started(struct airplay_server *srv)
{
	if (!srv->mirror) {
		struct airplay_mirror_config mcfg = {0};
		mcfg.port = srv->config.port + 1;
		mcfg.on_video_frame = srv->config.on_video_frame;
		mcfg.on_audio_frame = srv->config.on_audio_frame;
		mcfg.on_disconnect = srv->config.on_disconnect;
		mcfg.userdata = srv->config.userdata;

		srv->mirror = airplay_mirror_create(&mcfg);
		if (srv->mirror)
			airplay_mirror_start(srv->mirror);
	}

	return srv->mirror ? airplay_mirror_get_port(srv->mirror) : 0;
}

/* ---------- Handle SETUP (RTSP-like with binary plist) ---------- */

static void handle_rtsp_setup(struct airplay_server *srv, socket_t client,
			      const struct http_request *req,
			      struct http_response *resp)
{
	(void)client;

	uint16_t mirror_port = ensure_mirror_started(srv);
	uint16_t event_port = srv->config.port + 2;
	uint16_t timing_port = srv->config.port + 3;

	/* Log what iOS sent us in the bplist body */
	if (req->body && req->body_length >= 8 &&
	    memcmp(req->body, "bplist00", 8) == 0) {
		struct bplist_reader *br =
			bplist_reader_create(req->body, req->body_length);
		if (br) {
			int n = bplist_reader_dict_count(br);
			ap_info("SETUP body: bplist with %d keys", n);
			for (int i = 0; i < n; i++) {
				const char *k = bplist_reader_get_key(br, i);
				if (k)
					ap_info("  key[%d]: %s", i, k);
			}
			bplist_reader_destroy(br);
		} else {
			ap_warn("SETUP body: bplist parse failed");
		}
	}

	/* Build binary plist response with event/timing/data ports */
	struct bplist_writer *bp = bplist_writer_create();
	bplist_begin_dict(bp, 3);
	bplist_dict_add_int(bp, "eventPort", event_port);
	bplist_dict_add_int(bp, "timingPort", timing_port);
	bplist_dict_add_int(bp, "dataPort", mirror_port);

	size_t out_len = 0;
	uint8_t *out = bplist_writer_finalize(bp, &out_len);
	bplist_writer_destroy(bp);

	if (out) {
		resp->body = out;
		resp->body_length = out_len;
		http_response_add_header(resp, "Content-Type",
					 "application/x-apple-binary-plist");
		ap_info("SETUP response: %zu bytes (mirror=%d event=%d timing=%d)",
			out_len, mirror_port, event_port, timing_port);
	}

	srv->connected = true;
}

/* ---------- Handle POST /stream (alternate flow) ---------- */

static void handle_stream_setup(struct airplay_server *srv, socket_t client,
				const struct http_request *req,
				struct http_response *resp)
{
	(void)client;
	(void)req;

	uint16_t mirror_port = ensure_mirror_started(srv);

	struct bplist_writer *bp = bplist_writer_create();
	bplist_begin_dict(bp, 2);
	bplist_dict_add_int(bp, "dataPort", mirror_port);
	bplist_dict_add_int(bp, "eventPort", 0);

	size_t out_len = 0;
	uint8_t *out = bplist_writer_finalize(bp, &out_len);
	bplist_writer_destroy(bp);

	if (out) {
		resp->body = out;
		resp->body_length = out_len;
		http_response_add_header(resp, "Content-Type",
					 "application/x-apple-binary-plist");
	}

	srv->connected = true;
}

/* ---------- HTTP request handler ---------- */

static void airplay_http_handler(void *userdata, socket_t client,
				 const struct http_request *req,
				 struct http_response *resp)
{
	struct airplay_server *srv = (struct airplay_server *)userdata;

	ap_info("%s %s (body %zu bytes)", req->method, req->uri,
		req->body_length);

	resp->status_code = 200;
	resp->status_text = "OK";

	/* Add standard AirPlay headers */
	http_response_add_header(resp, "Server", "AirTunes/366.0");
	http_response_add_header(resp, "CSeq",
				 http_request_get_header(req, "CSeq")
					 ? http_request_get_header(req, "CSeq")
					 : "0");

	if (strcmp(req->uri, "/info") == 0 ||
	    strcmp(req->uri, "/server-info") == 0) {
		build_info_response(srv, resp);

	} else if (strcmp(req->uri, "/pair-setup") == 0) {
		handle_pair_setup(srv, req, resp);

	} else if (strcmp(req->uri, "/pair-verify") == 0) {
		handle_pair_verify(srv, req, resp);

	} else if (strcmp(req->uri, "/fp-setup") == 0 ||
		   strcmp(req->uri, "/fp-setup2") == 0) {
		handle_fp_setup(srv, req, resp);

	} else if (strcmp(req->uri, "/stream") == 0) {
		handle_stream_setup(srv, client, req, resp);

	} else if (strcmp(req->method, "SETUP") == 0) {
		handle_rtsp_setup(srv, client, req, resp);

	} else if (strcmp(req->method, "GET_PARAMETER") == 0 ||
		   strcmp(req->method, "SET_PARAMETER") == 0) {
		/* Keep-alive / parameter exchange */
		resp->status_code = 200;

	} else if (strcmp(req->method, "RECORD") == 0) {
		/* Start streaming confirmation */
		resp->status_code = 200;
		http_response_add_header(resp, "Audio-Latency", "0");

	} else if (strcmp(req->uri, "/feedback") == 0) {
		/* Client feedback - just acknowledge */
		resp->status_code = 200;

	} else if (strcmp(req->method, "TEARDOWN") == 0 ||
		   strcmp(req->uri, "/stop") == 0) {
		/* Stop mirroring */
		srv->connected = false;
		if (srv->config.on_disconnect)
			srv->config.on_disconnect(srv->config.userdata);
		resp->status_code = 200;

	} else if (strcmp(req->uri, "/command") == 0) {
		resp->status_code = 200;

	} else {
		/* Unknown endpoint - return 404 for GET, 200 for others */
		if (strcmp(req->method, "GET") == 0) {
			resp->status_code = 404;
			resp->status_text = "Not Found";
		}
	}
}

/* ---------- Public API ---------- */

struct airplay_server *
airplay_server_create(const struct airplay_server_config *cfg)
{
	struct airplay_server *srv = calloc(1, sizeof(struct airplay_server));
	if (!srv)
		return NULL;

	net_init();

	memcpy(&srv->config, cfg, sizeof(struct airplay_server_config));

	/* Get device MAC for ID */
	if (!net_get_mac_address(srv->device_id)) {
		/* Generate a random ID */
		srand((unsigned)time(NULL));
		for (int i = 0; i < 6; i++)
			srv->device_id[i] = (uint8_t)(rand() & 0xFF);
		srv->device_id[0] = (srv->device_id[0] & 0xFE) | 0x02;
	}

	snprintf(srv->device_id_str, sizeof(srv->device_id_str),
		 "%02X:%02X:%02X:%02X:%02X:%02X", srv->device_id[0],
		 srv->device_id[1], srv->device_id[2], srv->device_id[3],
		 srv->device_id[4], srv->device_id[5]);

	srv->fp = fairplay_create();

	return srv;
}

bool airplay_server_start(struct airplay_server *srv)
{
	if (!srv)
		return false;

	/* Start HTTP server */
	struct http_server_config http_cfg = {0};
	http_cfg.port = srv->config.port;
	http_cfg.handler = airplay_http_handler;
	http_cfg.userdata = srv;

	srv->http = http_server_create(&http_cfg);
	if (!srv->http)
		return false;

	/* Get actual port */
	uint16_t actual_port = http_server_get_port(srv->http);

	/* Start mDNS advertisement */
	struct mdns_service svc = {0};
	strncpy(svc.name, srv->config.name, sizeof(svc.name) - 1);
	strncpy(svc.type, "_airplay._tcp", sizeof(svc.type) - 1);
	svc.port = actual_port;

	/* AirPlay TXT records */
	char *txt[] = {
		"deviceid=FF:FF:FF:FF:FF:FF", /* placeholder, filled below */
		"features=0x527FFFF7,0x1E",
		"flags=0x44",
		"model=AppleTV5,3",
		"pk=99FD4299889422515FBD27949E4E1E21B2AF50A454499E3D4BE75A4E0F55FE63",
		"pi=b08f5a79-db29-4571-b11a-e3e1e03f1e89",
		"srcvers=366.0",
		"vv=2",
	};

	/* Fill in actual device ID */
	char devid_txt[64];
	snprintf(devid_txt, sizeof(devid_txt), "deviceid=%s",
		 srv->device_id_str);
	txt[0] = devid_txt;

	svc.txt_records = txt;
	svc.txt_count = 8;

	srv->mdns = mdns_publisher_create(&svc);
	if (!srv->mdns) {
		/* mDNS failure is non-fatal - manual connection still works */
		fprintf(stderr,
			"[AirPlay] Warning: mDNS publishing failed. "
			"Devices may not auto-discover this receiver.\n");
	}

	return true;
}

void airplay_server_stop(struct airplay_server *srv)
{
	if (!srv)
		return;

	if (srv->mirror) {
		airplay_mirror_stop(srv->mirror);
		airplay_mirror_destroy(srv->mirror);
		srv->mirror = NULL;
	}

	if (srv->mdns) {
		mdns_publisher_destroy(srv->mdns);
		srv->mdns = NULL;
	}

	if (srv->http) {
		http_server_stop(srv->http);
		srv->http = NULL;
	}

	srv->connected = false;
}

void airplay_server_destroy(struct airplay_server *srv)
{
	if (!srv)
		return;

	airplay_server_stop(srv);

	if (srv->http)
		http_server_destroy(srv->http);

	if (srv->fp)
		fairplay_destroy(srv->fp);

	free(srv);
}

bool airplay_server_is_connected(struct airplay_server *srv)
{
	return srv && srv->connected;
}
