/*
 * Minimal binary plist implementation for AirPlay
 *
 * Apple's binary plist format (bplist00) is used throughout AirPlay.
 * This implements a simple builder that can construct the plist responses
 * needed by the AirPlay server, and a basic parser for incoming plists.
 *
 * For full compliance, a complete plist library would be needed,
 * but this covers the AirPlay use cases.
 */

#include "airplay-plist.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* We'll use a simplified XML plist format for building, since Apple
 * devices accept both XML and binary. XML is much easier to construct. */

#define PLIST_INITIAL_SIZE 4096

struct plist_builder {
	char *buf;
	size_t len;
	size_t capacity;
	int indent;
};

static void pb_ensure(struct plist_builder *pb, size_t need)
{
	while (pb->len + need >= pb->capacity) {
		pb->capacity *= 2;
		pb->buf = realloc(pb->buf, pb->capacity);
	}
}

static void pb_append(struct plist_builder *pb, const char *str)
{
	size_t slen = strlen(str);
	pb_ensure(pb, slen + 1);
	memcpy(pb->buf + pb->len, str, slen);
	pb->len += slen;
	pb->buf[pb->len] = '\0';
}

static void pb_appendf(struct plist_builder *pb, const char *fmt, ...)
{
	char tmp[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(tmp, sizeof(tmp), fmt, args);
	va_end(args);
	pb_append(pb, tmp);
}

static void pb_indent(struct plist_builder *pb)
{
	for (int i = 0; i < pb->indent; i++)
		pb_append(pb, "\t");
}

struct plist_builder *plist_builder_create(void)
{
	struct plist_builder *pb = calloc(1, sizeof(struct plist_builder));
	if (!pb)
		return NULL;

	pb->capacity = PLIST_INITIAL_SIZE;
	pb->buf = calloc(1, pb->capacity);
	pb->len = 0;
	pb->indent = 0;

	/* XML plist header */
	pb_append(pb, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
	pb_append(pb, "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
		      "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n");
	pb_append(pb, "<plist version=\"1.0\">\n");

	return pb;
}

void plist_builder_destroy(struct plist_builder *pb)
{
	if (!pb)
		return;
	free(pb->buf);
	free(pb);
}

void plist_dict_begin(struct plist_builder *pb)
{
	pb_indent(pb);
	pb_append(pb, "<dict>\n");
	pb->indent++;
}

void plist_dict_end(struct plist_builder *pb)
{
	pb->indent--;
	pb_indent(pb);
	pb_append(pb, "</dict>\n");
}

void plist_dict_add_string(struct plist_builder *pb, const char *key,
			   const char *value)
{
	pb_indent(pb);
	pb_appendf(pb, "<key>%s</key>\n", key);
	pb_indent(pb);
	pb_appendf(pb, "<string>%s</string>\n", value);
}

void plist_dict_add_int(struct plist_builder *pb, const char *key,
			int64_t value)
{
	pb_indent(pb);
	pb_appendf(pb, "<key>%s</key>\n", key);
	pb_indent(pb);
	pb_appendf(pb, "<integer>%lld</integer>\n", (long long)value);
}

void plist_dict_add_bool(struct plist_builder *pb, const char *key, bool value)
{
	pb_indent(pb);
	pb_appendf(pb, "<key>%s</key>\n", key);
	pb_indent(pb);
	pb_appendf(pb, "<%s/>\n", value ? "true" : "false");
}

void plist_dict_add_real(struct plist_builder *pb, const char *key,
			 double value)
{
	pb_indent(pb);
	pb_appendf(pb, "<key>%s</key>\n", key);
	pb_indent(pb);
	pb_appendf(pb, "<real>%f</real>\n", value);
}

void plist_dict_add_data(struct plist_builder *pb, const char *key,
			 const uint8_t *data, size_t len)
{
	static const char b64[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	pb_indent(pb);
	pb_appendf(pb, "<key>%s</key>\n", key);
	pb_indent(pb);
	pb_append(pb, "<data>");

	/* Base64 encode */
	for (size_t i = 0; i < len; i += 3) {
		uint32_t n = ((uint32_t)data[i]) << 16;
		if (i + 1 < len)
			n |= ((uint32_t)data[i + 1]) << 8;
		if (i + 2 < len)
			n |= (uint32_t)data[i + 2];

		char out[5];
		out[0] = b64[(n >> 18) & 0x3F];
		out[1] = b64[(n >> 12) & 0x3F];
		out[2] = (i + 1 < len) ? b64[(n >> 6) & 0x3F] : '=';
		out[3] = (i + 2 < len) ? b64[n & 0x3F] : '=';
		out[4] = '\0';
		pb_append(pb, out);
	}

	pb_append(pb, "</data>\n");
}

void plist_array_begin(struct plist_builder *pb)
{
	pb_indent(pb);
	pb_append(pb, "<array>\n");
	pb->indent++;
}

void plist_array_end(struct plist_builder *pb)
{
	pb->indent--;
	pb_indent(pb);
	pb_append(pb, "</array>\n");
}

uint8_t *plist_builder_finalize(struct plist_builder *pb, size_t *out_len)
{
	pb_append(pb, "</plist>\n");

	uint8_t *result = (uint8_t *)malloc(pb->len);
	if (result) {
		memcpy(result, pb->buf, pb->len);
		*out_len = pb->len;
	} else {
		*out_len = 0;
	}

	return result;
}

/* ---------- Plist Parser (simple XML-based) ---------- */

static const char *skip_ws(const char *p)
{
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;
	return p;
}

static bool extract_tag_content(const char *xml, const char *tag,
				char *buf, size_t buf_size)
{
	char open_tag[64], close_tag[64];
	snprintf(open_tag, sizeof(open_tag), "<%s>", tag);
	snprintf(close_tag, sizeof(close_tag), "</%s>", tag);

	const char *start = strstr(xml, open_tag);
	if (!start)
		return false;
	start += strlen(open_tag);

	const char *end = strstr(start, close_tag);
	if (!end)
		return false;

	size_t len = (size_t)(end - start);
	if (len >= buf_size)
		len = buf_size - 1;
	memcpy(buf, start, len);
	buf[len] = '\0';

	return true;
}

bool plist_parse_dict(const uint8_t *data, size_t len,
		      struct plist_dict_reader *out)
{
	memset(out, 0, sizeof(*out));

	/* Check if it's binary plist */
	if (len >= 8 && memcmp(data, "bplist00", 8) == 0) {
		/* Binary plist - we don't fully parse these yet.
		 * For AirPlay, most incoming data we need to parse
		 * is actually the stream setup which we handle separately. */
		return false;
	}

	/* Try XML plist parsing */
	const char *xml = (const char *)data;

	/* Simple state machine to extract key-value pairs from <dict> */
	const char *p = strstr(xml, "<dict>");
	if (!p)
		return false;
	p += 6;

	while (out->count < 64) {
		p = skip_ws(p);

		/* Look for <key> */
		const char *key_start = strstr(p, "<key>");
		if (!key_start)
			break;
		key_start += 5;

		const char *key_end = strstr(key_start, "</key>");
		if (!key_end)
			break;

		struct plist_dict_entry *entry = &out->entries[out->count];

		size_t klen = (size_t)(key_end - key_start);
		if (klen >= sizeof(entry->key))
			klen = sizeof(entry->key) - 1;
		memcpy(entry->key, key_start, klen);
		entry->key[klen] = '\0';

		p = key_end + 6;
		p = skip_ws(p);

		/* Determine value type */
		if (strncmp(p, "<string>", 8) == 0) {
			entry->type = PLIST_TYPE_STRING;
			const char *vs = p + 8;
			const char *ve = strstr(vs, "</string>");
			if (ve) {
				size_t vlen = (size_t)(ve - vs);
				if (vlen >= sizeof(entry->str_val))
					vlen = sizeof(entry->str_val) - 1;
				memcpy(entry->str_val, vs, vlen);
				entry->str_val[vlen] = '\0';
				p = ve + 9;
			}
		} else if (strncmp(p, "<integer>", 9) == 0) {
			entry->type = PLIST_TYPE_INT;
			entry->int_val = atoll(p + 9);
			const char *ve = strstr(p, "</integer>");
			if (ve)
				p = ve + 10;
		} else if (strncmp(p, "<true/>", 7) == 0) {
			entry->type = PLIST_TYPE_BOOL;
			entry->bool_val = true;
			p += 7;
		} else if (strncmp(p, "<false/>", 8) == 0) {
			entry->type = PLIST_TYPE_BOOL;
			entry->bool_val = false;
			p += 8;
		} else {
			/* Skip unknown types */
			const char *next_key = strstr(p, "<key>");
			const char *dict_end = strstr(p, "</dict>");
			if (next_key && (!dict_end || next_key < dict_end))
				p = next_key;
			else
				break;
			continue;
		}

		out->count++;
	}

	return out->count > 0;
}

void plist_dict_reader_free(struct plist_dict_reader *reader)
{
	for (int i = 0; i < reader->count; i++) {
		if (reader->entries[i].type == PLIST_TYPE_DATA &&
		    reader->entries[i].data_val.data) {
			free(reader->entries[i].data_val.data);
		}
	}
	memset(reader, 0, sizeof(*reader));
}

const char *plist_dict_get_string(const struct plist_dict_reader *reader,
				  const char *key)
{
	for (int i = 0; i < reader->count; i++) {
		if (strcmp(reader->entries[i].key, key) == 0 &&
		    reader->entries[i].type == PLIST_TYPE_STRING) {
			return reader->entries[i].str_val;
		}
	}
	return NULL;
}

bool plist_dict_get_int(const struct plist_dict_reader *reader,
			const char *key, int64_t *out)
{
	for (int i = 0; i < reader->count; i++) {
		if (strcmp(reader->entries[i].key, key) == 0 &&
		    reader->entries[i].type == PLIST_TYPE_INT) {
			*out = reader->entries[i].int_val;
			return true;
		}
	}
	return false;
}
