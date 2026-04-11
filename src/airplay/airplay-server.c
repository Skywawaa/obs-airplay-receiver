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
	struct plist_builder *pb = plist_builder_create();
	if (!pb)
		return;

	plist_dict_begin(pb);

	/* Device identification */
	plist_dict_add_string(pb, "deviceid", srv->device_id_str);
	plist_dict_add_string(pb, "model", "AppleTV5,3");
	plist_dict_add_string(pb, "name", srv->config.name);
	plist_dict_add_string(pb, "srcvers", "366.0");
	plist_dict_add_string(pb, "vv", "2");

	/* Features bitmask - enable screen mirroring */
	/* Bit 7: video, Bit 9: screen mirroring, Bit 14: audio */
	plist_dict_add_int(pb, "features", 0x527FFFF7);
	plist_dict_add_int(pb, "statusFlags", 0x44);

	/* Screen size */
	plist_dict_add_int(pb, "width", 1920);
	plist_dict_add_int(pb, "height", 1080);

	/* Protocol info */
	plist_dict_add_string(pb, "pi", "b08f5a79-db29-4571-b11a-e3e1e03f1e89");
	plist_dict_add_string(pb, "pk",
		"99FD4299889422515FBD27949E4E1E21B2AF50A454499E3D4BE75A4E0F55FE63");

	/* OS info */
	plist_dict_add_string(pb, "osBuildVersion", "17K499");
	plist_dict_add_string(pb, "protovers", "1.1");

	/* Required for modern AirPlay */
	plist_dict_add_int(pb, "keepAliveLowPower", 1);
	plist_dict_add_int(pb, "keepAliveSendStatsAsBody", 1);

	plist_dict_end(pb);

	size_t plist_len;
	uint8_t *plist_data = plist_builder_finalize(pb, &plist_len);
	plist_builder_destroy(pb);

	if (plist_data) {
		resp->body = plist_data;
		resp->body_length = plist_len;
		http_response_add_header(resp, "Content-Type",
					 "text/x-apple-plist+xml");
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

/* ---------- Handle /stream (mirror setup) ---------- */

static void handle_stream_setup(struct airplay_server *srv, socket_t client,
				const struct http_request *req,
				struct http_response *resp)
{
	/*
	 * POST /stream initiates screen mirroring.
	 * The body is a binary plist with stream parameters.
	 * After the HTTP response, the connection switches to
	 * a binary protocol carrying H.264 frames.
	 */

	/* Start the mirror receiver on a separate port */
	if (!srv->mirror) {
		struct airplay_mirror_config mcfg = {0};
		mcfg.port = srv->config.port + 1; /* Mirror data port */
		mcfg.on_video_frame = srv->config.on_video_frame;
		mcfg.on_audio_frame = srv->config.on_audio_frame;
		mcfg.on_disconnect = srv->config.on_disconnect;
		mcfg.userdata = srv->config.userdata;

		srv->mirror = airplay_mirror_create(&mcfg);
		if (srv->mirror) {
			airplay_mirror_start(srv->mirror);
		}
	}

	uint16_t mirror_port = srv->mirror
				       ? airplay_mirror_get_port(srv->mirror)
				       : 0;

	/* Response: plist with data port and event port */
	struct plist_builder *pb = plist_builder_create();
	plist_dict_begin(pb);
	plist_dict_add_int(pb, "dataPort", mirror_port);
	plist_dict_add_int(pb, "eventPort", 0);
	plist_dict_end(pb);

	size_t plist_len;
	uint8_t *plist_data = plist_builder_finalize(pb, &plist_len);
	plist_builder_destroy(pb);

	if (plist_data) {
		resp->body = plist_data;
		resp->body_length = plist_len;
		http_response_add_header(resp, "Content-Type",
					 "text/x-apple-plist+xml");
	}

	srv->connected = true;
}

/* ---------- Handle SETUP (RTSP-like) ---------- */

static void handle_rtsp_setup(struct airplay_server *srv, socket_t client,
			      const struct http_request *req,
			      struct http_response *resp)
{
	/* RTSP SETUP for mirroring */
	const char *ctype = http_request_get_header(req, "Content-Type");

	if (ctype && strstr(ctype, "bplist")) {
		/* Binary plist - stream setup */
		handle_stream_setup(srv, client, req, resp);
		return;
	}

	/* Return a generic setup response */
	struct plist_builder *pb = plist_builder_create();
	plist_dict_begin(pb);
	plist_dict_add_int(pb, "eventPort", srv->config.port + 2);
	plist_dict_add_int(pb, "timingPort", 0);
	plist_dict_end(pb);

	size_t plist_len;
	uint8_t *plist_data = plist_builder_finalize(pb, &plist_len);
	plist_builder_destroy(pb);

	if (plist_data) {
		resp->body = plist_data;
		resp->body_length = plist_len;
		http_response_add_header(resp, "Content-Type",
					 "text/x-apple-plist+xml");
	}
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
