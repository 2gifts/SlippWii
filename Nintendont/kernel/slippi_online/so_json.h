/* slippi_online/so_json.h
 * Minimal JSON for the matchmaking messages and user.json.
 *
 * Reading works in place, without allocation: a SoJson is a view of one value
 * inside the original text, and lookups walk the text. The messages are a few
 * hundred bytes, so this costs less than building a tree.
 *
 * Writing appends to a fixed buffer. Keys are written in the order given;
 * callers list them sorted, as Slippi Dolphin's nlohmann::json dump does.
 */
#ifndef SO_JSON_H
#define SO_JSON_H

#include "global.h"

enum {
	SO_JSON_INVALID = 0,
	SO_JSON_NULL,
	SO_JSON_BOOL,
	SO_JSON_NUMBER,
	SO_JSON_STRING,
	SO_JSON_ARRAY,
	SO_JSON_OBJECT,
};

typedef struct {
	const char *start;	/* first character of the value */
	const char *end;	/* one past its last character */
} SoJson;

/* Parses a whole document; returns 0 when `text` holds exactly one valid
 * value (surrounding whitespace allowed). */
int so_json_parse(SoJson *doc, const char *text, u32 len);

int so_json_type(const SoJson *v);

/* Object member lookup; returns 0 and fills `out` when `key` is present. */
int so_json_get(const SoJson *object, const char *key, SoJson *out);
/* Array access. */
int so_json_count(const SoJson *array);
int so_json_at(const SoJson *array, int index, SoJson *out);

/* Value readers return 0 on success, < 0 on a type mismatch. so_json_string
 * unescapes into `out` (UTF-8, always NUL-terminated) and fails rather than
 * truncate. */
int so_json_string(const SoJson *v, char *out, int size);
int so_json_long(const SoJson *v, long *out);
int so_json_bool(const SoJson *v, int *out);

/* Convenience lookups with a default for missing or mistyped members, the
 * way Dolphin's json.value(key, default) behaves. */
void so_json_get_string(const SoJson *object, const char *key, char *out, int size, const char *fallback);
long so_json_get_long(const SoJson *object, const char *key, long fallback);
int so_json_get_bool(const SoJson *object, const char *key, int fallback);

/* ---- writer -------------------------------------------------------------- */

#define SO_JSON_MAX_DEPTH 8

typedef struct {
	char *buf;
	int size;
	int len;
	int overflow;
	int depth;
	int after_key;	/* a key was written; its value comes next */
	u16 count[SO_JSON_MAX_DEPTH];	/* members written at each depth */
} SoJsonWriter;

void so_jw_init(SoJsonWriter *w, char *buf, int size);
void so_jw_object_begin(SoJsonWriter *w);
void so_jw_object_end(SoJsonWriter *w);
void so_jw_array_begin(SoJsonWriter *w);
void so_jw_array_end(SoJsonWriter *w);
void so_jw_key(SoJsonWriter *w, const char *key);
void so_jw_string(SoJsonWriter *w, const char *value);
void so_jw_long(SoJsonWriter *w, long value);
void so_jw_bool(SoJsonWriter *w, int value);
/* Returns the text length, or -1 when it did not fit. */
int so_jw_finish(SoJsonWriter *w);

#endif
