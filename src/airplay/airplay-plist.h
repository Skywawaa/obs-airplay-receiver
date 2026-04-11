#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/*
 * Minimal binary plist builder for AirPlay.
 * AirPlay uses Apple binary plists for /info and other endpoints.
 * This implements just enough to build the responses we need.
 */

struct plist_builder;

/* Create a new plist builder */
struct plist_builder *plist_builder_create(void);

/* Destroy the builder */
void plist_builder_destroy(struct plist_builder *pb);

/* Start a dictionary */
void plist_dict_begin(struct plist_builder *pb);
void plist_dict_end(struct plist_builder *pb);

/* Add key-value pairs to current dictionary */
void plist_dict_add_string(struct plist_builder *pb, const char *key,
			   const char *value);
void plist_dict_add_int(struct plist_builder *pb, const char *key,
			int64_t value);
void plist_dict_add_bool(struct plist_builder *pb, const char *key,
			 bool value);
void plist_dict_add_data(struct plist_builder *pb, const char *key,
			 const uint8_t *data, size_t len);
void plist_dict_add_real(struct plist_builder *pb, const char *key,
			 double value);

/* Start/end an array value for the current key */
void plist_array_begin(struct plist_builder *pb);
void plist_array_end(struct plist_builder *pb);

/* Get the serialized binary plist data */
uint8_t *plist_builder_finalize(struct plist_builder *pb, size_t *out_len);

/*
 * Simple plist reader - parse a binary plist into a key-value structure.
 * Only handles flat dictionaries with string/int/data values.
 */
struct plist_dict_entry {
	char key[256];
	enum { PLIST_TYPE_STRING, PLIST_TYPE_INT, PLIST_TYPE_DATA, PLIST_TYPE_BOOL } type;
	union {
		char str_val[512];
		int64_t int_val;
		struct {
			uint8_t *data;
			size_t length;
		} data_val;
		bool bool_val;
	};
};

struct plist_dict_reader {
	struct plist_dict_entry entries[64];
	int count;
};

/* Parse a binary plist dictionary */
bool plist_parse_dict(const uint8_t *data, size_t len,
		      struct plist_dict_reader *out);

/* Free parsed dict resources */
void plist_dict_reader_free(struct plist_dict_reader *reader);

/* Find a string value by key */
const char *plist_dict_get_string(const struct plist_dict_reader *reader,
				  const char *key);

/* Find an integer value by key */
bool plist_dict_get_int(const struct plist_dict_reader *reader,
			const char *key, int64_t *out);
