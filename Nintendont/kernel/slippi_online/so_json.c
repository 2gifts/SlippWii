/* slippi_online/so_json.c
 * Minimal JSON (see so_json.h).
 */
#include "so_json.h"
#include "so_util.h"
#include "string.h"

#define MAX_NESTING 16

/* ---- reader -------------------------------------------------------------- */

static const char *skip_ws(const char *p, const char *end)
{
	while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
		p++;
	return p;
}

static int is_digit(char c)
{
	return c >= '0' && c <= '9';
}

static int hex_value(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static const char *skip_string(const char *p, const char *end)
{
	p++;	/* opening quote */
	while (p < end) {
		char c = *p++;
		if (c == '"')
			return p;
		if ((u8)c < 0x20)
			return NULL;
		if (c == '\\') {
			if (p >= end)
				return NULL;
			if (*p == 'u') {
				int i;
				if (end - p < 5)
					return NULL;
				for (i = 1; i <= 4; i++)
					if (hex_value(p[i]) < 0)
						return NULL;
				p += 5;
			} else if (strchr("\"\\/bfnrt", *p)) {
				p++;
			} else {
				return NULL;
			}
		}
	}
	return NULL;
}

static const char *skip_number(const char *p, const char *end)
{
	const char *digits;
	if (p < end && *p == '-')
		p++;
	digits = p;
	while (p < end && is_digit(*p))
		p++;
	if (p == digits)
		return NULL;
	if (p < end && *p == '.') {
		digits = ++p;
		while (p < end && is_digit(*p))
			p++;
		if (p == digits)
			return NULL;
	}
	if (p < end && (*p == 'e' || *p == 'E')) {
		p++;
		if (p < end && (*p == '+' || *p == '-'))
			p++;
		digits = p;
		while (p < end && is_digit(*p))
			p++;
		if (p == digits)
			return NULL;
	}
	return p;
}

static const char *skip_literal(const char *p, const char *end, const char *word)
{
	int n = strlen(word);
	if (end - p < n || memcmp(p, word, n) != 0)
		return NULL;
	return p + n;
}

/* Returns the end of the value starting at p (no leading whitespace). */
static const char *skip_value(const char *p, const char *end, int depth)
{
	char close;
	if (p >= end || depth > MAX_NESTING)
		return NULL;
	switch (*p) {
	case '"':
		return skip_string(p, end);
	case 't':
		return skip_literal(p, end, "true");
	case 'f':
		return skip_literal(p, end, "false");
	case 'n':
		return skip_literal(p, end, "null");
	case '{':
	case '[':
		close = *p == '{' ? '}' : ']';
		p = skip_ws(p + 1, end);
		if (p < end && *p == close)
			return p + 1;
		for (;;) {
			if (close == '}') {
				if (p >= end || *p != '"' || !(p = skip_string(p, end)))
					return NULL;
				p = skip_ws(p, end);
				if (p >= end || *p != ':')
					return NULL;
				p = skip_ws(p + 1, end);
			}
			if (!(p = skip_value(p, end, depth + 1)))
				return NULL;
			p = skip_ws(p, end);
			if (p < end && *p == ',') {
				p = skip_ws(p + 1, end);
				continue;
			}
			if (p < end && *p == close)
				return p + 1;
			return NULL;
		}
	default:
		return skip_number(p, end);
	}
}

int so_json_parse(SoJson *doc, const char *text, u32 len)
{
	const char *end = text + len;
	const char *p = skip_ws(text, end), *after;
	after = skip_value(p, end, 0);
	if (!after || skip_ws(after, end) != end) {
		doc->start = doc->end = NULL;
		return -1;
	}
	doc->start = p;
	doc->end = after;
	return 0;
}

int so_json_type(const SoJson *v)
{
	if (!v->start || v->start >= v->end)
		return SO_JSON_INVALID;
	switch (*v->start) {
	case '{': return SO_JSON_OBJECT;
	case '[': return SO_JSON_ARRAY;
	case '"': return SO_JSON_STRING;
	case 't':
	case 'f': return SO_JSON_BOOL;
	case 'n': return SO_JSON_NULL;
	default: return SO_JSON_NUMBER;
	}
}

/* Values inside a parsed document are known to be valid, so the walkers
 * below only need to find boundaries. */

static int key_equals(const char *quoted, const char *quoted_end, const char *key)
{
	char text[64];
	SoJson k;
	k.start = quoted;
	k.end = quoted_end;
	return so_json_string(&k, text, sizeof(text)) == 0 && strcmp(text, key) == 0;
}

int so_json_get(const SoJson *object, const char *key, SoJson *out)
{
	const char *p, *end = object->end;
	if (so_json_type(object) != SO_JSON_OBJECT)
		return -1;
	p = skip_ws(object->start + 1, end);
	while (p < end && *p == '"') {
		const char *key_end = skip_string(p, end), *value;
		int match = key_equals(p, key_end, key);
		value = skip_ws(skip_ws(key_end, end) + 1, end);	/* past ':' */
		p = skip_value(value, end, 0);
		if (match) {
			out->start = value;
			out->end = p;
			return 0;
		}
		p = skip_ws(p, end);
		if (p < end && *p == ',')
			p = skip_ws(p + 1, end);
	}
	return -1;
}

int so_json_at(const SoJson *array, int index, SoJson *out)
{
	const char *p, *end = array->end;
	int i = 0;
	if (so_json_type(array) != SO_JSON_ARRAY || index < 0)
		return -1;
	p = skip_ws(array->start + 1, end);
	while (p < end && *p != ']') {
		const char *after = skip_value(p, end, 0);
		if (i == index) {
			out->start = p;
			out->end = after;
			return 0;
		}
		i++;
		p = skip_ws(after, end);
		if (p < end && *p == ',')
			p = skip_ws(p + 1, end);
	}
	return -1;
}

int so_json_count(const SoJson *array)
{
	SoJson item;
	int n = 0;
	while (so_json_at(array, n, &item) == 0)
		n++;
	return n;
}

static int put_utf8(char *out, int size, int len, u32 cp)
{
	char bytes[4];
	int n, i;
	if (cp < 0x80) {
		bytes[0] = (char)cp;
		n = 1;
	} else if (cp < 0x800) {
		bytes[0] = (char)(0xC0 | (cp >> 6));
		bytes[1] = (char)(0x80 | (cp & 0x3F));
		n = 2;
	} else if (cp < 0x10000) {
		bytes[0] = (char)(0xE0 | (cp >> 12));
		bytes[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		bytes[2] = (char)(0x80 | (cp & 0x3F));
		n = 3;
	} else {
		bytes[0] = (char)(0xF0 | (cp >> 18));
		bytes[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
		bytes[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
		bytes[3] = (char)(0x80 | (cp & 0x3F));
		n = 4;
	}
	if (len + n >= size)
		return -1;
	for (i = 0; i < n; i++)
		out[len + i] = bytes[i];
	return len + n;
}

static u32 read_hex4(const char *p)
{
	return (u32)((hex_value(p[0]) << 12) | (hex_value(p[1]) << 8) | (hex_value(p[2]) << 4) | hex_value(p[3]));
}

int so_json_string(const SoJson *v, char *out, int size)
{
	const char *p, *end;
	int len = 0;
	if (size <= 0)
		return -1;
	out[0] = '\0';
	if (so_json_type(v) != SO_JSON_STRING)
		return -1;
	p = v->start + 1;
	end = v->end - 1;	/* closing quote */
	while (p < end) {
		char c = *p++;
		if (c == '\\') {
			c = *p++;
			if (c == 'u') {
				u32 cp = read_hex4(p);
				p += 4;
				if (cp >= 0xD800 && cp < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
					u32 low = read_hex4(p + 2);
					if (low >= 0xDC00 && low < 0xE000) {
						cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
						p += 6;
					}
				}
				if ((len = put_utf8(out, size, len, cp)) < 0)
					goto overflow;
				continue;
			}
			switch (c) {
			case 'b': c = '\b'; break;
			case 'f': c = '\f'; break;
			case 'n': c = '\n'; break;
			case 'r': c = '\r'; break;
			case 't': c = '\t'; break;
			default: break;	/* " \ / */
			}
		}
		if (len + 1 >= size)
			goto overflow;
		out[len++] = c;
	}
	out[len] = '\0';
	return 0;
overflow:
	out[0] = '\0';
	return -1;
}

int so_json_long(const SoJson *v, long *out)
{
	const char *end;
	if (so_json_type(v) != SO_JSON_NUMBER)
		return -1;
	/* Fractions and exponents are truncated away; the fields read here are
	 * integers. */
	*out = so_parse_long(v->start, &end);
	return 0;
}

int so_json_bool(const SoJson *v, int *out)
{
	if (so_json_type(v) != SO_JSON_BOOL)
		return -1;
	*out = *v->start == 't';
	return 0;
}

void so_json_get_string(const SoJson *object, const char *key, char *out, int size, const char *fallback)
{
	SoJson v;
	if (so_json_get(object, key, &v) == 0 && so_json_string(&v, out, size) == 0)
		return;
	strlcpy(out, fallback, size);
}

long so_json_get_long(const SoJson *object, const char *key, long fallback)
{
	SoJson v;
	long value;
	if (so_json_get(object, key, &v) == 0 && so_json_long(&v, &value) == 0)
		return value;
	return fallback;
}

int so_json_get_bool(const SoJson *object, const char *key, int fallback)
{
	SoJson v;
	int value;
	if (so_json_get(object, key, &v) == 0 && so_json_bool(&v, &value) == 0)
		return value;
	return fallback;
}

/* ---- writer -------------------------------------------------------------- */

static void jw_put(SoJsonWriter *w, const char *text, int n)
{
	if (w->len + n >= w->size) {
		w->overflow = 1;
		return;
	}
	memcpy(w->buf + w->len, text, n);
	w->len += n;
	w->buf[w->len] = '\0';
}

static void jw_putc(SoJsonWriter *w, char c)
{
	jw_put(w, &c, 1);
}

/* Separates array items and object members; a value right after its key
 * needs no comma. */
static void jw_value_prefix(SoJsonWriter *w)
{
	if (w->after_key) {
		w->after_key = 0;
		return;
	}
	if (w->depth > 0 && w->count[w->depth - 1]++ > 0)
		jw_putc(w, ',');
}

void so_jw_init(SoJsonWriter *w, char *buf, int size)
{
	memset(w, 0, sizeof(*w));
	w->buf = buf;
	w->size = size;
	if (size > 0)
		buf[0] = '\0';
}

static void jw_open(SoJsonWriter *w, char c)
{
	jw_value_prefix(w);
	jw_putc(w, c);
	if (w->depth >= SO_JSON_MAX_DEPTH) {
		w->overflow = 1;
		return;
	}
	w->count[w->depth++] = 0;
}

static void jw_close(SoJsonWriter *w, char c)
{
	if (w->depth > 0)
		w->depth--;
	jw_putc(w, c);
}

void so_jw_object_begin(SoJsonWriter *w) { jw_open(w, '{'); }
void so_jw_object_end(SoJsonWriter *w) { jw_close(w, '}'); }
void so_jw_array_begin(SoJsonWriter *w) { jw_open(w, '['); }
void so_jw_array_end(SoJsonWriter *w) { jw_close(w, ']'); }

static void jw_quoted(SoJsonWriter *w, const char *s)
{
	static const char hex[] = "0123456789abcdef";
	jw_putc(w, '"');
	for (; *s; s++) {
		u8 c = (u8)*s;
		char esc[6];
		if (c == '"' || c == '\\') {
			esc[0] = '\\';
			esc[1] = (char)c;
			jw_put(w, esc, 2);
		} else if (c < 0x20) {
			/* nlohmann's short forms, else \u00XX */
			const char *shorts = "\b\f\n\r\t", *names = "bfnrt";
			const char *hit = c ? strchr(shorts, c) : NULL;
			esc[0] = '\\';
			if (hit) {
				esc[1] = names[hit - shorts];
				jw_put(w, esc, 2);
			} else {
				esc[1] = 'u';
				esc[2] = '0';
				esc[3] = '0';
				esc[4] = hex[c >> 4];
				esc[5] = hex[c & 15];
				jw_put(w, esc, 6);
			}
		} else {
			jw_putc(w, (char)c);	/* UTF-8 passes through */
		}
	}
	jw_putc(w, '"');
}

void so_jw_key(SoJsonWriter *w, const char *key)
{
	jw_value_prefix(w);
	jw_quoted(w, key);
	jw_putc(w, ':');
	w->after_key = 1;
}

void so_jw_string(SoJsonWriter *w, const char *value)
{
	jw_value_prefix(w);
	jw_quoted(w, value);
}

void so_jw_long(SoJsonWriter *w, long value)
{
	char text[24];
	int n;
	jw_value_prefix(w);
	n = so_snprintf(text, sizeof(text), "%ld", value);
	jw_put(w, text, n);
}

void so_jw_bool(SoJsonWriter *w, int value)
{
	jw_value_prefix(w);
	if (value)
		jw_put(w, "true", 4);
	else
		jw_put(w, "false", 5);
}

int so_jw_finish(SoJsonWriter *w)
{
	if (w->overflow || w->depth != 0 || w->after_key)
		return -1;
	return w->len;
}
