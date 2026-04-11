/*
 * Minimal binary plist writer and reader (bplist00 format)
 *
 * bplist00 layout:
 *   - 8-byte magic "bplist00"
 *   - Object table: each object encoded inline
 *   - Offset table: uint32 or uint8 offsets to each object
 *   - Trailer (32 bytes): metadata
 *
 * Each object starts with a marker byte:
 *   0x00  null
 *   0x08  false
 *   0x09  true
 *   0x1n  int (2^n bytes of int data follow)
 *   0x4n  data (n = length, or n=0xF then length follows)
 *   0x5n  ASCII string (n = length, or n=0xF then length follows)
 *   0x6n  UTF-16 string
 *   0xAn  array (n = count)
 *   0xDn  dict (n = count)
 */

#include "bplist.h"
#include "../log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---------- Writer ---------- */

#define BPLIST_INITIAL 1024
#define MAX_OBJECTS    256

struct bplist_writer {
	uint8_t *buf;
	size_t len;
	size_t cap;

	int entry_count;    /* expected dict entries */
	int entries_added;

	/* Object offsets (into the flat buffer below we build later) */
	uint64_t offsets[MAX_OBJECTS];
	int object_count;

	/* Keys and values buffered separately so we can emit the dict header
	 * first, then keys, then values */
	struct {
		char *key;
		int type; /* 0=int, 1=string, 2=data, 3=bool */
		int64_t int_val;
		char *str_val;
		uint8_t *data_val;
		size_t data_len;
	} items[MAX_OBJECTS];
};

struct bplist_writer *bplist_writer_create(void)
{
	struct bplist_writer *bp = calloc(1, sizeof(*bp));
	return bp;
}

void bplist_writer_destroy(struct bplist_writer *bp)
{
	if (!bp)
		return;
	for (int i = 0; i < bp->entries_added; i++) {
		free(bp->items[i].key);
		free(bp->items[i].str_val);
		free(bp->items[i].data_val);
	}
	free(bp->buf);
	free(bp);
}

void bplist_begin_dict(struct bplist_writer *bp, int entry_count)
{
	bp->entry_count = entry_count;
}

void bplist_dict_add_int(struct bplist_writer *bp, const char *key,
			 int64_t value)
{
	if (bp->entries_added >= MAX_OBJECTS)
		return;
	int i = bp->entries_added++;
	bp->items[i].key = strdup(key);
	bp->items[i].type = 0;
	bp->items[i].int_val = value;
}

void bplist_dict_add_string(struct bplist_writer *bp, const char *key,
			    const char *value)
{
	if (bp->entries_added >= MAX_OBJECTS)
		return;
	int i = bp->entries_added++;
	bp->items[i].key = strdup(key);
	bp->items[i].type = 1;
	bp->items[i].str_val = strdup(value);
}

void bplist_dict_add_data(struct bplist_writer *bp, const char *key,
			  const uint8_t *data, size_t len)
{
	if (bp->entries_added >= MAX_OBJECTS)
		return;
	int i = bp->entries_added++;
	bp->items[i].key = strdup(key);
	bp->items[i].type = 2;
	bp->items[i].data_val = malloc(len);
	if (bp->items[i].data_val) {
		memcpy(bp->items[i].data_val, data, len);
		bp->items[i].data_len = len;
	}
}

void bplist_dict_add_bool(struct bplist_writer *bp, const char *key, bool v)
{
	if (bp->entries_added >= MAX_OBJECTS)
		return;
	int i = bp->entries_added++;
	bp->items[i].key = strdup(key);
	bp->items[i].type = 3;
	bp->items[i].int_val = v ? 1 : 0;
}

/* ---------- Buffer helpers ---------- */

static void buf_ensure(struct bplist_writer *bp, size_t need)
{
	if (bp->len + need <= bp->cap)
		return;
	if (bp->cap == 0)
		bp->cap = BPLIST_INITIAL;
	while (bp->len + need > bp->cap)
		bp->cap *= 2;
	bp->buf = realloc(bp->buf, bp->cap);
}

static void buf_append(struct bplist_writer *bp, const void *data, size_t len)
{
	buf_ensure(bp, len);
	memcpy(bp->buf + bp->len, data, len);
	bp->len += len;
}

static void buf_byte(struct bplist_writer *bp, uint8_t b)
{
	buf_append(bp, &b, 1);
}

/* ---------- Object encoders ---------- */

static void emit_int(struct bplist_writer *bp, int64_t v)
{
	if (v >= 0 && v < 0x80) {
		buf_byte(bp, 0x10); /* 1 byte int */
		buf_byte(bp, (uint8_t)v);
	} else if (v >= -0x8000 && v < 0x8000) {
		buf_byte(bp, 0x11); /* 2 byte int */
		uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v};
		buf_append(bp, b, 2);
	} else if (v >= -0x80000000LL && v < 0x80000000LL) {
		buf_byte(bp, 0x12); /* 4 byte int */
		uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16),
				(uint8_t)(v >> 8), (uint8_t)v};
		buf_append(bp, b, 4);
	} else {
		buf_byte(bp, 0x13); /* 8 byte int */
		uint8_t b[8];
		for (int i = 0; i < 8; i++)
			b[i] = (uint8_t)(v >> (56 - i * 8));
		buf_append(bp, b, 8);
	}
}

static void emit_string(struct bplist_writer *bp, const char *s)
{
	size_t slen = strlen(s);
	if (slen < 15) {
		buf_byte(bp, (uint8_t)(0x50 | slen));
	} else {
		buf_byte(bp, 0x5F);
		emit_int(bp, (int64_t)slen);
	}
	buf_append(bp, s, slen);
}

static void emit_data(struct bplist_writer *bp, const uint8_t *d, size_t len)
{
	if (len < 15) {
		buf_byte(bp, (uint8_t)(0x40 | len));
	} else {
		buf_byte(bp, 0x4F);
		emit_int(bp, (int64_t)len);
	}
	buf_append(bp, d, len);
}

static void emit_bool(struct bplist_writer *bp, bool v)
{
	buf_byte(bp, v ? 0x09 : 0x08);
}

/* ---------- Finalize ---------- */

uint8_t *bplist_writer_finalize(struct bplist_writer *bp, size_t *out_len)
{
	if (!bp)
		return NULL;

	*out_len = 0;
	bp->len = 0;

	/*
	 * Layout:
	 *   magic (8)
	 *   obj0 = dict (markers + key refs + value refs)
	 *   obj1..N = string keys
	 *   objN+1..2N = values
	 *   offset table
	 *   trailer
	 *
	 * Object indices:
	 *   0 = top dict
	 *   1..N = keys
	 *   N+1..2N = values
	 */

	int n = bp->entries_added;
	int total_objs = 1 + 2 * n;

	/* Emit magic */
	buf_append(bp, "bplist00", 8);

	uint64_t obj_offsets[MAX_OBJECTS * 2 + 1];

	/* Object 0: top dict */
	obj_offsets[0] = bp->len;

	/* Dict marker: 0xDn where n is count, or 0xDF + int count */
	if (n < 15) {
		buf_byte(bp, (uint8_t)(0xD0 | n));
	} else {
		buf_byte(bp, 0xDF);
		emit_int(bp, (int64_t)n);
	}

	/* Key refs (1 byte each since total_objs < 256) */
	for (int i = 0; i < n; i++)
		buf_byte(bp, (uint8_t)(1 + i));

	/* Value refs */
	for (int i = 0; i < n; i++)
		buf_byte(bp, (uint8_t)(1 + n + i));

	/* Emit keys */
	for (int i = 0; i < n; i++) {
		obj_offsets[1 + i] = bp->len;
		emit_string(bp, bp->items[i].key);
	}

	/* Emit values */
	for (int i = 0; i < n; i++) {
		obj_offsets[1 + n + i] = bp->len;

		switch (bp->items[i].type) {
		case 0: /* int */
			emit_int(bp, bp->items[i].int_val);
			break;
		case 1: /* string */
			emit_string(bp, bp->items[i].str_val);
			break;
		case 2: /* data */
			emit_data(bp, bp->items[i].data_val,
				  bp->items[i].data_len);
			break;
		case 3: /* bool */
			emit_bool(bp, bp->items[i].int_val != 0);
			break;
		}
	}

	/* Offset table: use 1 or 2 byte offsets depending on file size */
	uint64_t offset_table_start = bp->len;
	uint8_t offset_size = (bp->len < 256) ? 1 : 2;

	for (int i = 0; i < total_objs; i++) {
		if (offset_size == 1) {
			buf_byte(bp, (uint8_t)obj_offsets[i]);
		} else {
			uint8_t b[2] = {(uint8_t)(obj_offsets[i] >> 8),
					(uint8_t)obj_offsets[i]};
			buf_append(bp, b, 2);
		}
	}

	/* Trailer (32 bytes):
	 *   6 bytes zero
	 *   1 byte offset_size
	 *   1 byte object_ref_size
	 *   8 bytes num_objects (BE)
	 *   8 bytes top_object_index (BE) = 0
	 *   8 bytes offset_table_offset (BE)
	 */
	uint8_t trailer[32] = {0};
	trailer[6] = offset_size;
	trailer[7] = 1; /* object ref size = 1 byte */

	/* num objects */
	uint64_t num = total_objs;
	for (int i = 0; i < 8; i++)
		trailer[8 + i] = (uint8_t)(num >> (56 - i * 8));

	/* top object = 0 (already zero) */

	/* offset table offset */
	for (int i = 0; i < 8; i++)
		trailer[24 + i] =
			(uint8_t)(offset_table_start >> (56 - i * 8));

	buf_append(bp, trailer, 32);

	/* Return a copy */
	uint8_t *out = malloc(bp->len);
	if (!out)
		return NULL;
	memcpy(out, bp->buf, bp->len);
	*out_len = bp->len;
	return out;
}

/* ---------- Reader ---------- */

struct bplist_reader {
	const uint8_t *data;
	size_t len;

	int offset_size;
	int ref_size;
	uint64_t num_objects;
	uint64_t top_object;
	uint64_t offset_table;

	/* Top dict info */
	uint64_t dict_offset;
	int dict_count;
	const uint8_t *key_refs;
	const uint8_t *value_refs;

	/* Cached key strings */
	char keys[64][128];
};

static uint64_t read_big(const uint8_t *p, int n)
{
	uint64_t v = 0;
	for (int i = 0; i < n; i++)
		v = (v << 8) | p[i];
	return v;
}

static uint64_t reader_obj_offset(struct bplist_reader *br, uint64_t ref)
{
	if (ref >= br->num_objects)
		return 0;
	const uint8_t *p = br->data + br->offset_table + ref * br->offset_size;
	return read_big(p, br->offset_size);
}

/* Decode an int object at the given offset.
 * Returns true and fills *out if successful. */
static bool reader_decode_int(struct bplist_reader *br, uint64_t off,
			      int64_t *out)
{
	if (off >= br->len)
		return false;
	uint8_t marker = br->data[off];
	if ((marker & 0xF0) != 0x10)
		return false;
	int n = 1 << (marker & 0x0F);
	if (off + 1 + n > br->len)
		return false;
	*out = (int64_t)read_big(br->data + off + 1, n);
	return true;
}

/* Decode a string object. Returns a pointer into cached buf.
 * For ASCII only; caller must not modify. */
static const char *reader_decode_string(struct bplist_reader *br, uint64_t off,
					char *buf, size_t buf_size)
{
	if (off >= br->len)
		return NULL;
	uint8_t marker = br->data[off];
	uint8_t type = marker & 0xF0;

	if (type != 0x50 && type != 0x60)
		return NULL;

	size_t slen = marker & 0x0F;
	uint64_t data_off = off + 1;
	if (slen == 15) {
		int64_t len64;
		if (!reader_decode_int(br, off + 1, &len64))
			return NULL;
		slen = (size_t)len64;
		/* Skip the length int */
		int n = 1 << (br->data[off + 1] & 0x0F);
		data_off = off + 2 + n;
	}

	if (type == 0x50) {
		if (data_off + slen > br->len || slen >= buf_size)
			return NULL;
		memcpy(buf, br->data + data_off, slen);
		buf[slen] = '\0';
		return buf;
	}

	/* UTF-16: each char is 2 bytes, just take low byte as ASCII */
	if (data_off + slen * 2 > br->len || slen >= buf_size)
		return NULL;
	for (size_t i = 0; i < slen; i++)
		buf[i] = (char)br->data[data_off + i * 2 + 1];
	buf[slen] = '\0';
	return buf;
}

struct bplist_reader *bplist_reader_create(const uint8_t *data, size_t len)
{
	if (!data || len < 40)
		return NULL;
	if (memcmp(data, "bplist00", 8) != 0)
		return NULL;

	struct bplist_reader *br = calloc(1, sizeof(*br));
	if (!br)
		return NULL;

	br->data = data;
	br->len = len;

	/* Trailer at the end */
	const uint8_t *t = data + len - 32;
	br->offset_size = t[6];
	br->ref_size = t[7];
	br->num_objects = read_big(t + 8, 8);
	br->top_object = read_big(t + 16, 8);
	br->offset_table = read_big(t + 24, 8);

	if (br->offset_size == 0 || br->ref_size == 0 ||
	    br->offset_table >= len) {
		free(br);
		return NULL;
	}

	/* Read top object - expect dict */
	br->dict_offset = reader_obj_offset(br, br->top_object);
	if (br->dict_offset >= len) {
		free(br);
		return NULL;
	}

	uint8_t marker = data[br->dict_offset];
	if ((marker & 0xF0) != 0xD0) {
		free(br);
		return NULL;
	}

	br->dict_count = marker & 0x0F;
	uint64_t keys_off = br->dict_offset + 1;
	if (br->dict_count == 15) {
		int64_t count64;
		if (!reader_decode_int(br, br->dict_offset + 1, &count64)) {
			free(br);
			return NULL;
		}
		br->dict_count = (int)count64;
		int n = 1 << (data[br->dict_offset + 1] & 0x0F);
		keys_off = br->dict_offset + 2 + n;
	}

	if (br->dict_count > 64)
		br->dict_count = 64;

	br->key_refs = data + keys_off;
	br->value_refs = data + keys_off + br->dict_count * br->ref_size;

	/* Pre-decode keys */
	for (int i = 0; i < br->dict_count; i++) {
		uint64_t ref = read_big(br->key_refs + i * br->ref_size,
					br->ref_size);
		uint64_t off = reader_obj_offset(br, ref);
		reader_decode_string(br, off, br->keys[i],
				     sizeof(br->keys[0]));
	}

	return br;
}

void bplist_reader_destroy(struct bplist_reader *br)
{
	free(br);
}

int bplist_reader_dict_count(struct bplist_reader *br)
{
	return br ? br->dict_count : 0;
}

const char *bplist_reader_get_key(struct bplist_reader *br, int i)
{
	if (!br || i < 0 || i >= br->dict_count)
		return NULL;
	return br->keys[i];
}

static int reader_find_key(struct bplist_reader *br, const char *key)
{
	for (int i = 0; i < br->dict_count; i++) {
		if (strcmp(br->keys[i], key) == 0)
			return i;
	}
	return -1;
}

bool bplist_reader_get_int(struct bplist_reader *br, const char *key,
			   int64_t *out)
{
	int idx = reader_find_key(br, key);
	if (idx < 0)
		return false;
	uint64_t ref = read_big(br->value_refs + idx * br->ref_size,
				br->ref_size);
	uint64_t off = reader_obj_offset(br, ref);
	return reader_decode_int(br, off, out);
}

const char *bplist_reader_get_string(struct bplist_reader *br,
				     const char *key)
{
	int idx = reader_find_key(br, key);
	if (idx < 0)
		return NULL;
	uint64_t ref = read_big(br->value_refs + idx * br->ref_size,
				br->ref_size);
	uint64_t off = reader_obj_offset(br, ref);

	static char buf[512];
	return reader_decode_string(br, off, buf, sizeof(buf));
}
