/* SlippWii: a minimal VCDIFF decoder (see vcdiff.h); a port of
 * tools/vcdiff.py. */
#include "vcdiff.h"

#include <string.h>

enum { VCD_SOURCE = 0x01, VCD_TARGET = 0x02, VCD_ADLER32 = 0x04 };
enum { NOOP = 0, ADD = 1, RUN = 2, COPY = 3 };

typedef struct {
	unsigned char kind1, size1, mode1, kind2, size2, mode2;
} Code;

static Code table[256];
static int table_ready;

static void build_table(void)
{
	int i = 0, s, mode, add;
	table[i++] = (Code){ RUN, 0, 0, NOOP, 0, 0 };
	for (s = 0; s < 18; s++)
		table[i++] = (Code){ ADD, (unsigned char)s, 0, NOOP, 0, 0 };
	for (mode = 0; mode < 9; mode++) {
		table[i++] = (Code){ COPY, 0, (unsigned char)mode, NOOP, 0, 0 };
		for (s = 4; s < 19; s++)
			table[i++] = (Code){ COPY, (unsigned char)s, (unsigned char)mode, NOOP, 0, 0 };
	}
	for (mode = 0; mode < 6; mode++)
		for (add = 1; add < 5; add++)
			for (s = 4; s < 7; s++)
				table[i++] = (Code){ ADD, (unsigned char)add, 0, COPY, (unsigned char)s, (unsigned char)mode };
	for (mode = 6; mode < 9; mode++)
		for (add = 1; add < 5; add++)
			table[i++] = (Code){ ADD, (unsigned char)add, 0, COPY, 4, (unsigned char)mode };
	for (mode = 0; mode < 9; mode++)
		table[i++] = (Code){ COPY, 4, (unsigned char)mode, ADD, 1, 0 };
	table_ready = 1;
}

typedef struct {
	const unsigned char *p, *end;
	int bad;
} Reader;

static unsigned byte(Reader *r)
{
	if (r->p >= r->end) {
		r->bad = 1;
		return 0;
	}
	return *r->p++;
}

static size_t varint(Reader *r)
{
	size_t v = 0;
	int n = 0;
	for (;;) {
		unsigned b = byte(r);
		if (r->bad || ++n > 5) {
			r->bad = 1;
			return 0;
		}
		v = (v << 7) | (b & 0x7F);
		if (!(b & 0x80))
			return v;
	}
}

static const unsigned char *take(Reader *r, size_t n)
{
	const unsigned char *at = r->p;
	if ((size_t)(r->end - r->p) < n) {
		r->bad = 1;
		return r->p;
	}
	r->p += n;
	return at;
}

static unsigned long adler32(const unsigned char *p, size_t n)
{
	unsigned long a = 1, b = 0;
	while (n) {
		size_t k = n < 3800 ? n : 3800;
		n -= k;
		while (k--) {
			a += *p++;
			b += a;
		}
		a %= 65521;
		b %= 65521;
	}
	return (b << 16) | a;
}

unsigned long vcdiff_crc32(unsigned long crc, const unsigned char *p, size_t n)
{
	static unsigned long crc_table[256];
	static int crc_ready;
	if (!crc_ready) {
		unsigned long c;
		int i, k;
		for (i = 0; i < 256; i++) {
			c = (unsigned long)i;
			for (k = 0; k < 8; k++)
				c = c & 1 ? 0xEDB88320UL ^ (c >> 1) : c >> 1;
			crc_table[i] = c;
		}
		crc_ready = 1;
	}
	crc = ~crc & 0xFFFFFFFFUL;
	while (n--)
		crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
	return ~crc & 0xFFFFFFFFUL;
}

#define FAIL(text) do { if (error) *error = (text); return -1; } while (0)

/* The file header; 0, or -1. */
static int header(Reader *r, const char **error)
{
	const unsigned char *magic = take(r, 4);
	unsigned flags;
	if (r->bad || memcmp(magic, "\xD6\xC3\xC4\x00", 4) != 0)
		FAIL("not a VCDIFF patch");
	flags = byte(r);
	if (flags & 0x01)
		FAIL("compressed patch (not supported)");
	if (flags & 0x02)
		FAIL("custom code table (not supported)");
	if (flags & 0x04)
		take(r, varint(r));
	if (r->bad)
		FAIL("damaged patch");
	return 0;
}

long vcdiff_target_size(const unsigned char *delta, size_t delta_len, const char **error)
{
	Reader r = { delta, delta + delta_len, 0 };
	size_t total = 0;
	if (header(&r, error) < 0)
		return -1;
	while (r.p < r.end) {
		unsigned win = byte(&r);
		size_t encoding_len, target_len;
		const unsigned char *start;
		if (win & (VCD_SOURCE | VCD_TARGET)) {
			varint(&r);
			varint(&r);
		}
		encoding_len = varint(&r);
		start = r.p;
		target_len = varint(&r);
		total += target_len;
		r.p = start;
		take(&r, encoding_len);
		if (r.bad)
			FAIL("damaged patch");
	}
	return (long)total;
}

int vcdiff_decode(const unsigned char *source, size_t source_len, const unsigned char *delta, size_t delta_len,
	unsigned char *out, size_t out_len, const char **error)
{
	Reader r = { delta, delta + delta_len, 0 };
	size_t done = 0;
	if (!table_ready)
		build_table();
	if (header(&r, error) < 0)
		return -1;
	while (r.p < r.end) {
		unsigned win = byte(&r), indicator;
		const unsigned char *seg = NULL;
		size_t seg_len = 0, seg_pos, target_len, data_len, inst_len, addr_len, t = 0;
		size_t near[4] = { 0, 0, 0, 0 }, same[3 * 256];
		int near_slot = 0, has_sum = (win & VCD_ADLER32) != 0;
		unsigned long sum = 0;
		Reader data, inst, addr;
		unsigned char *target = out + done;

		memset(same, 0, sizeof(same));
		if (win & (VCD_SOURCE | VCD_TARGET)) {
			seg_len = varint(&r);
			seg_pos = varint(&r);
			if (win & VCD_SOURCE) {
				if (seg_pos > source_len || seg_len > source_len - seg_pos)
					FAIL("the patch is for a different (bigger) disc file");
				seg = source + seg_pos;
			} else {
				if (seg_pos > done || seg_len > done - seg_pos)
					FAIL("damaged patch");
				seg = out + seg_pos;
			}
		}
		varint(&r);	/* the delta encoding's length */
		target_len = varint(&r);
		indicator = byte(&r);
		data_len = varint(&r);
		inst_len = varint(&r);
		addr_len = varint(&r);
		if (has_sum) {
			const unsigned char *s = take(&r, 4);
			sum = ((unsigned long)s[0] << 24) | ((unsigned long)s[1] << 16) | ((unsigned long)s[2] << 8) | s[3];
		}
		if (r.bad)
			FAIL("damaged patch");
		if (indicator)
			FAIL("compressed sections (not supported)");
		if (target_len > out_len - done)
			FAIL("damaged patch (target too big)");
		data.p = take(&r, data_len); data.end = data.p + data_len; data.bad = 0;
		inst.p = take(&r, inst_len); inst.end = inst.p + inst_len; inst.bad = 0;
		addr.p = take(&r, addr_len); addr.end = addr.p + addr_len; addr.bad = 0;
		if (r.bad)
			FAIL("damaged patch");

		while (inst.p < inst.end) {
			const Code *code = &table[byte(&inst)];
			int half;
			for (half = 0; half < 2; half++) {
				unsigned kind = half ? code->kind2 : code->kind1;
				size_t size = half ? code->size2 : code->size1;
				unsigned mode = half ? code->mode2 : code->mode1;
				if (kind == NOOP)
					continue;
				if (size == 0)
					size = varint(&inst);
				if (inst.bad || size > target_len - t)
					FAIL("damaged patch");
				if (kind == ADD) {
					const unsigned char *src = take(&data, size);
					if (data.bad)
						FAIL("damaged patch");
					memcpy(target + t, src, size);
					t += size;
				} else if (kind == RUN) {
					unsigned b = byte(&data);
					if (data.bad)
						FAIL("damaged patch");
					memset(target + t, (int)b, size);
					t += size;
				} else {
					size_t here = seg_len + t, a, i;
					if (mode == 0)
						a = varint(&addr);
					else if (mode == 1)
						a = here - varint(&addr);
					else if (mode < 6)
						a = near[mode - 2] + varint(&addr);
					else
						a = same[(mode - 6) * 256 + byte(&addr)];
					if (addr.bad || a >= here)
						FAIL("damaged patch");
					near[near_slot] = a;
					near_slot = (near_slot + 1) % 4;
					same[a % (3 * 256)] = a;
					for (i = 0; i < size; i++, a++)
						target[t++] = a < seg_len ? seg[a] : target[a - seg_len];
				}
			}
		}
		if (t != target_len)
			FAIL("damaged patch (window size)");
		if (has_sum && adler32(target, t) != sum)
			FAIL("the disc file is not the one the patch is for (is it an unmodified NTSC 1.02 disc?)");
		done += t;
	}
	if (done != out_len)
		FAIL("damaged patch (size)");
	return 0;
}
