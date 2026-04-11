#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/*
 * FairPlay SAP v1 setup (used by AirPlay).
 *
 * FairPlay v1 has a fixed challenge/response pattern that
 * was reverse-engineered and is used in open-source
 * AirPlay receivers (RPiPlay, UxPlay, Shairplay).
 *
 * Phase 1: Client -> Server (16 bytes)
 *   Server responds with 142 bytes depending on the mode byte
 *
 * Phase 2: Client -> Server (164 bytes)
 *   Server decrypts the client challenge and responds with 32 bytes
 */

struct fairplay;

struct fairplay *fairplay_create(void);
void fairplay_destroy(struct fairplay *fp);

/* Process an fp-setup request body.
 * Returns malloc'd response buffer (caller must free) and sets *out_len.
 * Returns NULL on error. */
uint8_t *fairplay_handle_setup(struct fairplay *fp, const uint8_t *req,
			       size_t req_len, size_t *out_len);
