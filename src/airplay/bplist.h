#pragma once

/*
 * Minimal binary plist writer (bplist00)
 * Produces byte-for-byte compatible output with CoreFoundation's binary
 * property list format. Supports dict with string keys and int/string values
 * and arrays - enough to respond to AirPlay SETUP requests.
 *
 * Format reference:
 *   https://opensource.apple.com/source/CF/CF-1153.18/CFBinaryPList.c
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

struct bplist_writer;

struct bplist_writer *bplist_writer_create(void);
void bplist_writer_destroy(struct bplist_writer *bp);

/* Begin a top-level dict with a known number of entries */
void bplist_begin_dict(struct bplist_writer *bp, int entry_count);
void bplist_dict_add_int(struct bplist_writer *bp, const char *key,
			 int64_t value);
void bplist_dict_add_string(struct bplist_writer *bp, const char *key,
			    const char *value);
void bplist_dict_add_data(struct bplist_writer *bp, const char *key,
			  const uint8_t *data, size_t len);
void bplist_dict_add_bool(struct bplist_writer *bp, const char *key, bool v);

/* Finalize - returns malloc'd buffer with full bplist00 data.
 * Caller must free. */
uint8_t *bplist_writer_finalize(struct bplist_writer *bp, size_t *out_len);

/*
 * Minimal reader - parses a top-level dict from bplist00 data.
 * Only extracts keys and int/string/data values.
 */
struct bplist_reader;

struct bplist_reader *bplist_reader_create(const uint8_t *data, size_t len);
void bplist_reader_destroy(struct bplist_reader *br);

/* Iterate keys in the top-level dict.
 * Returns the key at index i (0-based), or NULL if out of range. */
const char *bplist_reader_get_key(struct bplist_reader *br, int i);
int bplist_reader_dict_count(struct bplist_reader *br);

/* Get int value for a key. Returns true if found and int-typed. */
bool bplist_reader_get_int(struct bplist_reader *br, const char *key,
			   int64_t *out);

/* Get string value for a key. Returns pointer into internal buffer (copy if
 * you need to keep it). Returns NULL if not found or wrong type. */
const char *bplist_reader_get_string(struct bplist_reader *br,
				     const char *key);
