/*
 * AirPlay Mirror Stream Receiver
 *
 * Receives the H.264 video stream from AirPlay screen mirroring.
 *
 * AirPlay mirror stream format:
 *   The stream is a sequence of frames, each preceded by a header:
 *
 *   Byte 0-3:   Payload size (big-endian uint32)
 *   Byte 4-5:   Payload type (0x00 = video, 0x01 = codec info/SPS+PPS)
 *   Byte 6-7:   Reserved / flags
 *   Byte 8-15:  NTP timestamp (uint64 big-endian)
 *   Byte 16+:   H.264 NAL units (with 4-byte length prefixes)
 *
 *   Codec info packet (type 0x01):
 *   Contains SPS and PPS NAL units needed to initialize the decoder.
 */

#include "airplay-mirror.h"
#include "../network/net-utils.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

#define MIRROR_HEADER_SIZE 128
#define MAX_FRAME_SIZE     (4 * 1024 * 1024) /* 4 MB max frame */

/* AirPlay mirror packet types */
#define MIRROR_PKT_VIDEO      0
#define MIRROR_PKT_CODEC_DATA 1
#define MIRROR_PKT_HEARTBEAT  2

struct airplay_mirror {
	struct airplay_mirror_config config;
	socket_t listen_sock;
	volatile bool running;
	uint16_t port;

	/* SPS/PPS data for prepending to keyframes */
	uint8_t *sps_pps;
	size_t sps_pps_size;

#ifdef _WIN32
	HANDLE thread;
#else
	pthread_t thread;
#endif
};

/* ---------- Parse mirror stream header ---------- */

struct mirror_packet_header {
	uint32_t payload_size;
	uint16_t payload_type;
	uint16_t flags;
	uint64_t timestamp;
};

static bool read_mirror_header(socket_t sock,
			       struct mirror_packet_header *hdr)
{
	uint8_t buf[128];

	/* Read first 4 bytes to get payload size */
	if (!net_recv_exact(sock, buf, 4))
		return false;

	hdr->payload_size = read_be32(buf);

	/* Validate */
	if (hdr->payload_size > MAX_FRAME_SIZE)
		return false;

	/* Read the remaining header bytes (12 more for a 16-byte header) */
	if (!net_recv_exact(sock, buf + 4, 12))
		return false;

	hdr->payload_type = read_be16(buf + 4);
	hdr->flags = read_be16(buf + 6);

	/* Timestamp: bytes 8-15 */
	hdr->timestamp = ((uint64_t)read_be32(buf + 8) << 32) |
			 (uint64_t)read_be32(buf + 12);

	return true;
}

/* ---------- Convert H.264 length-prefixed NALUs to Annex-B ---------- */

static uint8_t *convert_to_annexb(const uint8_t *data, size_t data_size,
				  const uint8_t *sps_pps,
				  size_t sps_pps_size, bool is_keyframe,
				  size_t *out_size)
{
	/* Calculate output size:
	 * Each NALU: replace 4-byte length with 4-byte start code
	 * Plus SPS/PPS for keyframes */
	size_t max_size = data_size + 1024; /* extra space for start codes */
	if (is_keyframe && sps_pps)
		max_size += sps_pps_size + 8;

	uint8_t *out = malloc(max_size);
	if (!out)
		return NULL;

	size_t pos = 0;

	/* Prepend SPS/PPS before keyframes */
	if (is_keyframe && sps_pps && sps_pps_size > 0) {
		memcpy(out + pos, sps_pps, sps_pps_size);
		pos += sps_pps_size;
	}

	/* Convert each NALU */
	size_t offset = 0;
	while (offset + 4 <= data_size) {
		uint32_t nalu_size = read_be32(data + offset);
		offset += 4;

		if (nalu_size == 0 || offset + nalu_size > data_size)
			break;

		/* Annex-B start code */
		out[pos++] = 0x00;
		out[pos++] = 0x00;
		out[pos++] = 0x00;
		out[pos++] = 0x01;

		/* NALU data */
		memcpy(out + pos, data + offset, nalu_size);
		pos += nalu_size;
		offset += nalu_size;
	}

	*out_size = pos;
	return out;
}

/* ---------- Parse SPS/PPS from codec data ---------- */

static uint8_t *parse_codec_data(const uint8_t *data, size_t data_size,
				 size_t *out_size)
{
	/*
	 * Codec data format (AVCDecoderConfigurationRecord-like):
	 * The exact format varies, but generally contains:
	 *   - Version, profile, compatibility, level
	 *   - SPS count + SPS NALUs
	 *   - PPS count + PPS NALUs
	 * Each NALU is preceded by a 2-byte length.
	 *
	 * We extract them and convert to Annex-B format.
	 */

	if (data_size < 7) {
		*out_size = 0;
		return NULL;
	}

	size_t max_out = data_size + 64;
	uint8_t *out = malloc(max_out);
	if (!out) {
		*out_size = 0;
		return NULL;
	}

	size_t pos = 0;
	size_t offset = 5; /* skip version, profile, compat, level, length_size */

	/* Number of SPS */
	if (offset >= data_size) goto done;
	int sps_count = data[offset] & 0x1F;
	offset++;

	for (int i = 0; i < sps_count && offset + 2 <= data_size; i++) {
		uint16_t sps_len = read_be16(data + offset);
		offset += 2;

		if (offset + sps_len > data_size)
			break;

		/* Start code + SPS */
		out[pos++] = 0x00;
		out[pos++] = 0x00;
		out[pos++] = 0x00;
		out[pos++] = 0x01;
		memcpy(out + pos, data + offset, sps_len);
		pos += sps_len;
		offset += sps_len;
	}

	/* Number of PPS */
	if (offset >= data_size) goto done;
	int pps_count = data[offset];
	offset++;

	for (int i = 0; i < pps_count && offset + 2 <= data_size; i++) {
		uint16_t pps_len = read_be16(data + offset);
		offset += 2;

		if (offset + pps_len > data_size)
			break;

		/* Start code + PPS */
		out[pos++] = 0x00;
		out[pos++] = 0x00;
		out[pos++] = 0x00;
		out[pos++] = 0x01;
		memcpy(out + pos, data + offset, pps_len);
		pos += pps_len;
		offset += pps_len;
	}

done:
	*out_size = pos;
	if (pos == 0) {
		free(out);
		return NULL;
	}
	return out;
}

/* ---------- Mirror receiver thread ---------- */

static void handle_mirror_client(struct airplay_mirror *mirror, socket_t client)
{
	fprintf(stderr, "[AirPlay] Mirror client connected\n");

	while (mirror->running) {
		struct mirror_packet_header hdr;
		if (!read_mirror_header(client, &hdr))
			break;

		if (hdr.payload_size == 0)
			continue;

		/* Read the payload */
		uint8_t *payload = malloc(hdr.payload_size);
		if (!payload)
			break;

		if (!net_recv_exact(client, payload, hdr.payload_size)) {
			free(payload);
			break;
		}

		switch (hdr.payload_type) {
		case MIRROR_PKT_CODEC_DATA: {
			/* SPS/PPS codec configuration */
			size_t sps_pps_size;
			uint8_t *sps_pps = parse_codec_data(
				payload, hdr.payload_size, &sps_pps_size);

			if (sps_pps) {
				free(mirror->sps_pps);
				mirror->sps_pps = sps_pps;
				mirror->sps_pps_size = sps_pps_size;

				/* Send SPS/PPS to decoder immediately */
				if (mirror->config.on_video_frame) {
					mirror->config.on_video_frame(
						mirror->config.userdata,
						sps_pps, sps_pps_size,
						hdr.timestamp);
				}
			}
			break;
		}

		case MIRROR_PKT_VIDEO: {
			/* H.264 video frame */
			bool is_keyframe = false;

			/* Check first NALU type */
			if (hdr.payload_size > 4) {
				uint8_t nalu_type = payload[4] & 0x1F;
				is_keyframe = (nalu_type == 5); /* IDR */
			}

			/* Convert from length-prefixed to Annex-B */
			size_t annexb_size;
			uint8_t *annexb = convert_to_annexb(
				payload, hdr.payload_size, mirror->sps_pps,
				mirror->sps_pps_size, is_keyframe, &annexb_size);

			if (annexb && mirror->config.on_video_frame) {
				mirror->config.on_video_frame(
					mirror->config.userdata, annexb,
					annexb_size, hdr.timestamp);
			}

			free(annexb);
			break;
		}

		case MIRROR_PKT_HEARTBEAT:
			/* Just acknowledge */
			break;

		default:
			break;
		}

		free(payload);
	}

	fprintf(stderr, "[AirPlay] Mirror client disconnected\n");

	if (mirror->config.on_disconnect)
		mirror->config.on_disconnect(mirror->config.userdata);
}

static void *mirror_thread_func(void *arg)
{
	struct airplay_mirror *mirror = (struct airplay_mirror *)arg;

#ifdef _WIN32
	DWORD tv = 1000;
	setsockopt(mirror->listen_sock, SOL_SOCKET, SO_RCVTIMEO,
		   (const char *)&tv, sizeof(tv));
#else
	struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
	setsockopt(mirror->listen_sock, SOL_SOCKET, SO_RCVTIMEO, &tv,
		   sizeof(tv));
#endif

	while (mirror->running) {
		struct sockaddr_in client_addr;
		socklen_t addr_len = sizeof(client_addr);

		socket_t client = accept(mirror->listen_sock,
					 (struct sockaddr *)&client_addr,
					 &addr_len);

		if (client == INVALID_SOCK)
			continue;

		/* Handle one client at a time (screen mirroring is 1:1) */
		handle_mirror_client(mirror, client);
		closesocket(client);
	}

	return NULL;
}

/* ---------- Public API ---------- */

struct airplay_mirror *
airplay_mirror_create(const struct airplay_mirror_config *cfg)
{
	struct airplay_mirror *mirror =
		calloc(1, sizeof(struct airplay_mirror));
	if (!mirror)
		return NULL;

	memcpy(&mirror->config, cfg, sizeof(struct airplay_mirror_config));

	return mirror;
}

bool airplay_mirror_start(struct airplay_mirror *mirror)
{
	if (!mirror)
		return false;

	mirror->listen_sock = net_tcp_listen(mirror->config.port, 2);
	if (mirror->listen_sock == INVALID_SOCK) {
		/* Try any available port */
		mirror->listen_sock = net_tcp_listen(0, 2);
		if (mirror->listen_sock == INVALID_SOCK)
			return false;
	}

	/* Get actual port */
	struct sockaddr_in addr;
	socklen_t len = sizeof(addr);
	if (getsockname(mirror->listen_sock, (struct sockaddr *)&addr, &len) ==
	    0) {
		mirror->port = ntohs(addr.sin_port);
	} else {
		mirror->port = mirror->config.port;
	}

	mirror->running = true;

#ifdef _WIN32
	mirror->thread = CreateThread(
		NULL, 0, (LPTHREAD_START_ROUTINE)mirror_thread_func, mirror, 0,
		NULL);
	if (!mirror->thread) {
		closesocket(mirror->listen_sock);
		return false;
	}
#else
	if (pthread_create(&mirror->thread, NULL, mirror_thread_func, mirror) !=
	    0) {
		closesocket(mirror->listen_sock);
		return false;
	}
#endif

	fprintf(stderr, "[AirPlay] Mirror receiver listening on port %d\n",
		mirror->port);
	return true;
}

void airplay_mirror_stop(struct airplay_mirror *mirror)
{
	if (!mirror)
		return;

	mirror->running = false;

	if (mirror->listen_sock != INVALID_SOCK) {
		closesocket(mirror->listen_sock);
		mirror->listen_sock = INVALID_SOCK;
	}

#ifdef _WIN32
	if (mirror->thread) {
		WaitForSingleObject(mirror->thread, 5000);
		CloseHandle(mirror->thread);
	}
#else
	pthread_join(mirror->thread, NULL);
#endif
}

void airplay_mirror_destroy(struct airplay_mirror *mirror)
{
	if (!mirror)
		return;

	airplay_mirror_stop(mirror);
	free(mirror->sps_pps);
	free(mirror);
}

uint16_t airplay_mirror_get_port(struct airplay_mirror *mirror)
{
	return mirror ? mirror->port : 0;
}
