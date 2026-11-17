/* SlippWii: resolving the "slp:" links of Slippi's game files from the disc (hsd_links.h).
 *
 * From the version after 3.6.4, two of the files Slippi ships (GameSetup_gui.dat and
 * SlippiCSS_gui.dat) no longer hold Nintendo's assets: where one was, the file has an external
 * symbol "slp:<disc file>:<root symbol>/<field>/<field>...[:at=..][:cut=..][:sha=..]", and Slippi
 * Dolphin copies the asset in from the disc when the game loads the file. On a Wii that is done
 * once, here, when the menu files are made from the disc.
 *
 * This follows slippi-rust-extensions' `hsd` crate (archive.rs, link.rs; GPLv3, the Slippi team)
 * in every rule that decides a byte of the result, so the file made here is the one Dolphin makes
 * (tools/hsd_links.py is the same in Python; tools/test_hsd_links.py compares all three). Only what
 * an unmodified disc needs is here: a link is followed by its path in the file it names. The
 * caller checks the result's size and CRC, which is what catches another disc; the links' own
 * hashes are not computed.
 *
 * An archive: a 0x20-byte header (file size, data size, the numbers of pointer fields, root
 * symbols and external symbols), the data, a table of the data's pointer fields, the two symbol
 * tables, the names. The data is cut into extents at every address something points to; an asset
 * is an extent and all it points to.
 */
#include <stdlib.h>
#include <string.h>
#include "hsd_links.h"

#define HEADER 0x20u
#define CHAIN_END 0xFFFFFFFFu
#define ALIGN 32u
#define MAX_SOURCES 12
#define MAX_HOPS 64
#define MAX_CUT 8

typedef struct {
	u32 start, len;
	u32 first, count;	/* its pointers in Arc.ptr */
} Ext;

typedef struct {
	u32 field;	/* offset in its extent */
	u32 target;	/* extent */
} Ptr;

typedef struct {
	u8 *data;	/* the data section, pointer and chain fields zeroed */
	u32 data_size;
	Ext *ext;
	u32 n_ext;
	Ptr *ptr;	/* by extent */
	u32 n_ptr;
	const u8 *roots, *refs, *strings;	/* the tables, in the file */
	u32 n_roots, n_refs, strings_size;
	u32 *root_ext;	/* each root's extent */
} Arc;

static u32 be32(const u8 *p)
{
	return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

static void put32(u8 *p, u32 v)
{
	p[0] = (u8)(v >> 24);
	p[1] = (u8)(v >> 16);
	p[2] = (u8)(v >> 8);
	p[3] = (u8)v;
}

static int by_u32(const void *a, const void *b)
{
	u32 x = *(const u32 *)a, y = *(const u32 *)b;
	return x < y ? -1 : x > y;
}

static void arc_free(Arc *a)
{
	free(a->ext);
	free(a->ptr);
	free(a->root_ext);
	memset(a, 0, sizeof(*a));
}

/* The extent that holds data offset `at`: the last that starts at or before it. */
static u32 extent_of(const Arc *a, u32 at)
{
	u32 lo = 0, hi = a->n_ext;
	while (hi - lo > 1) {
		u32 mid = (lo + hi) / 2;
		if (a->ext[mid].start <= at)
			lo = mid;
		else
			hi = mid;
	}
	return lo;
}

/* The extent that starts at `start` (one does: `start` was a boundary). */
static u32 extent_at(const Arc *a, u32 start)
{
	u32 i = extent_of(a, start);
	/* (an empty extent at the data's end shares no start with another) */
	while (i + 1 < a->n_ext && a->ext[i].start != start)
		i++;
	return i;
}

/* Parses the archive in `raw` (which it changes: the pointer and chain fields of the data become
 * zero, as the extents are copied with them so). 0, or a text for what is wrong. */
static const char *arc_parse(Arc *a, u8 *raw, u32 size, int zero_chains)
{
	u32 data_size, n_reloc, i, n_bounds = 0, end_targeted = 0;
	const u8 *reloc;
	u32 *bounds, *counts;
	memset(a, 0, sizeof(*a));
	if (size < HEADER || be32(raw) != size)
		return "not an archive";
	data_size = be32(raw + 4);
	n_reloc = be32(raw + 8);
	a->n_roots = be32(raw + 12);
	a->n_refs = be32(raw + 16);
	if (data_size > size - HEADER || n_reloc > (size - HEADER - data_size) / 4 ||
	    (u64)HEADER + data_size + (u64)n_reloc * 4 + ((u64)a->n_roots + a->n_refs) * 8 > size)
		return "its tables run past its end";
	a->data = raw + HEADER;
	a->data_size = data_size;
	reloc = a->data + data_size;
	a->roots = reloc + n_reloc * 4;
	a->refs = a->roots + a->n_roots * 8;
	a->strings = a->refs + a->n_refs * 8;
	a->strings_size = (u32)(raw + size - a->strings);

	/* the extents' starts: 0, and every target of a pointer or a root */
	bounds = (u32 *)malloc((n_reloc + a->n_roots + 2) * sizeof(u32));
	a->root_ext = (u32 *)malloc((a->n_roots + 1) * sizeof(u32));
	if (!bounds || !a->root_ext) {
		free(bounds);
		return "out of memory";
	}
	bounds[n_bounds++] = 0;
	for (i = 0; i < n_reloc; i++) {
		u32 field = be32(reloc + i * 4), target;
		if (field > data_size || data_size - field < 4 || (target = be32(a->data + field)) > data_size) {
			free(bounds);
			return "a pointer leads outside its data";
		}
		end_targeted |= target == data_size;
		bounds[n_bounds++] = target;
	}
	for (i = 0; i < a->n_roots; i++) {
		u32 target = be32(a->roots + i * 8);
		if (target > data_size || be32(a->roots + i * 8 + 4) >= a->strings_size) {
			free(bounds);
			return "a root symbol is damaged";
		}
		end_targeted |= target == data_size;
		bounds[n_bounds++] = target;
	}
	qsort(bounds, n_bounds, sizeof(u32), by_u32);
	a->ext = (Ext *)calloc(n_bounds + 1, sizeof(Ext));
	a->ptr = (Ptr *)malloc((n_reloc + 1) * sizeof(Ptr));
	counts = (u32 *)calloc(n_bounds + 1, sizeof(u32));
	if (!a->ext || !a->ptr || !counts) {
		free(bounds);
		free(counts);
		return "out of memory";
	}
	for (i = 0; i < n_bounds; i++) {
		if (i && bounds[i] == bounds[i - 1])
			continue;
		if (bounds[i] == data_size && !end_targeted)
			continue;	/* (never: only targets are here, but as the rule is written) */
		a->ext[a->n_ext++].start = bounds[i];
	}
	free(bounds);
	for (i = 0; i < a->n_ext; i++)
		a->ext[i].len = (i + 1 < a->n_ext ? a->ext[i + 1].start : data_size) - a->ext[i].start;

	/* the pointers, by extent and in the table's order within one */
	for (i = 0; i < n_reloc; i++) {
		u32 field = be32(reloc + i * 4), e = extent_of(a, field);
		if (field - a->ext[e].start + 4 > a->ext[e].len) {
			free(counts);
			return "a pointer straddles two structs";
		}
		a->ext[e].count++;
	}
	for (i = 0; i < a->n_ext; i++) {
		a->ext[i].first = a->n_ptr;
		a->n_ptr += a->ext[i].count;
	}
	for (i = 0; i < n_reloc; i++) {
		u32 field = be32(reloc + i * 4), e = extent_of(a, field);
		Ptr *p = &a->ptr[a->ext[e].first + counts[e]++];
		p->field = field - a->ext[e].start;
		p->target = extent_at(a, be32(a->data + field));
	}
	free(counts);
	for (i = 0; i < a->n_roots; i++)
		a->root_ext[i] = extent_at(a, be32(a->roots + i * 8));
	/* (the fields' values are in the tables now) */
	for (i = 0; i < n_reloc; i++)
		put32(a->data + be32(reloc + i * 4), 0);
	for (i = 0; i < a->n_refs; i++) {
		u32 at = be32(a->refs + i * 8), steps = 0;
		if (be32(a->refs + i * 8 + 4) >= a->strings_size)
			return "an external symbol is damaged";
		for (;;) {
			u32 next;
			if (at > data_size || data_size - at < 4 || steps++ > data_size / 4)
				return "a broken chain of external references";
			next = be32(a->data + at);
			/* (the archive being resolved keeps its chains: hsd_resolve walks them) */
			if (zero_chains)
				put32(a->data + at, 0);
			if (next == CHAIN_END)
				break;
			at = next;
		}
	}
	return NULL;
}

/* ---- a link's text ---------------------------------------------------------------------------- */

typedef struct {
	char file[32], root[64];
	u32 hops[MAX_HOPS], n_hops;
	u32 cut[MAX_CUT], n_cut;
} Locator;

static int hex(const char **p, u32 *out)
{
	const char *s = *p;
	u32 v = 0;
	int digits = 0;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		s += 2;
	for (;; s++, digits++) {
		char ch = *s;
		u32 d = ch >= '0' && ch <= '9' ? (u32)(ch - '0') : ch >= 'a' && ch <= 'f' ? (u32)(ch - 'a' + 10) :
			ch >= 'A' && ch <= 'F' ? (u32)(ch - 'A' + 10) : 16;
		if (d == 16)
			break;
		v = v << 4 | d;
	}
	*p = s;
	*out = v;
	return digits > 0;
}

/* "slp:FILE:ROOT/hop/hop*n...[:option...]": 0, or -1 (an offset in the place of a path too: Slippi's
 * files have none, and it could not be told from a moved asset without the link's hash). */
static int locator_parse(const char *symbol, Locator *l)
{
	const char *s = symbol + 4, *end;
	u32 n;
	memset(l, 0, sizeof(*l));
	end = strchr(s, ':');
	if (!end || end == s || (size_t)(end - s) >= sizeof(l->file))
		return -1;
	memcpy(l->file, s, (size_t)(end - s));
	s = end + 1;
	end = strchr(s, '/');
	if (!end || end == s || (size_t)(end - s) >= sizeof(l->root) || memchr(s, ':', (size_t)(end - s)))
		return -1;
	memcpy(l->root, s, (size_t)(end - s));
	s = end + 1;
	while (*s && *s != ':') {
		u32 field, repeat = 1;
		if (*s == '/') {
			s++;
			continue;
		}
		if (!hex(&s, &field))
			return -1;
		if (*s == '*') {
			repeat = 0;
			for (s++; *s >= '0' && *s <= '9'; s++)
				repeat = repeat * 10 + (u32)(*s - '0');
		}
		if (repeat > MAX_HOPS || l->n_hops + repeat > MAX_HOPS)
			return -1;
		for (n = 0; n < repeat; n++)
			l->hops[l->n_hops++] = field;
	}
	while (*s == ':') {
		s++;
		if (!strncmp(s, "cut=", 4)) {
			for (s += 4;; s++) {
				if (l->n_cut == MAX_CUT || !hex(&s, &l->cut[l->n_cut++]))
					return -1;
				if (*s != ',')
					break;
			}
		} else if (!strncmp(s, "at=", 3) || !strncmp(s, "sha=", 4) || !strncmp(s, "strict=", 7)) {
			while (*s && *s != ':')
				s++;
		} else {
			return -1;
		}
	}
	return *s ? -1 : 0;
}

static int is_cut(const Locator *l, u32 field)
{
	u32 i;
	for (i = 0; i < l->n_cut; i++)
		if (l->cut[i] == field)
			return 1;
	return 0;
}

static const char *name_at(const Arc *a, const u8 *table, u32 i)
{
	return (const char *)a->strings + be32(table + i * 8 + 4);
}

/* The extent a link names in its disc file: 0 and *out, or -1. */
static int follow(const Arc *src, const Locator *l, u32 *out)
{
	u32 i, k, cur = 0;
	for (i = 0; i < src->n_roots; i++)
		if (!strcmp(name_at(src, src->roots, i), l->root))
			break;
	if (i == src->n_roots)
		return -1;
	cur = src->root_ext[i];
	for (k = 0; k < l->n_hops; k++) {
		const Ext *e = &src->ext[cur];
		for (i = 0; i < e->count; i++)
			if (src->ptr[e->first + i].field == l->hops[k])
				break;
		if (i == e->count)
			return -1;
		cur = src->ptr[e->first + i].target;
	}
	*out = cur;
	return 0;
}

/* ---- the result ------------------------------------------------------------------------------- */

typedef struct {
	const u8 *data;
	u32 len, phase;
} OutExt;

typedef struct {
	u32 ext, field, target;
} OutPtr;

typedef struct {
	OutExt *ext;
	u32 n_ext, cap_ext;
	OutPtr *ptr;
	u32 n_ptr, cap_ptr;
} Out;

static int out_ext(Out *o, const u8 *data, u32 len, u32 phase)
{
	if (o->n_ext == o->cap_ext) {
		u32 cap = o->cap_ext ? o->cap_ext * 2 : 4096;
		OutExt *more = (OutExt *)realloc(o->ext, cap * sizeof(OutExt));
		if (!more)
			return -1;
		o->ext = more;
		o->cap_ext = cap;
	}
	o->ext[o->n_ext].data = data;
	o->ext[o->n_ext].len = len;
	o->ext[o->n_ext].phase = phase;
	o->n_ext++;
	return 0;
}

static int out_ptr(Out *o, u32 ext, u32 field, u32 target)
{
	if (o->n_ptr == o->cap_ptr) {
		u32 cap = o->cap_ptr ? o->cap_ptr * 2 : 8192;
		OutPtr *more = (OutPtr *)realloc(o->ptr, cap * sizeof(OutPtr));
		if (!more)
			return -1;
		o->ptr = more;
		o->cap_ptr = cap;
	}
	o->ptr[o->n_ptr].ext = ext;
	o->ptr[o->n_ptr].field = field;
	o->ptr[o->n_ptr].target = target;
	o->n_ptr++;
	return 0;
}

typedef struct {
	char name[32];
	u8 *raw;
	Arc arc;
	u32 *mark, *index;	/* per extent: the link that reached it last, and its extent in the result */
} Source;

u8 *hsd_resolve(u8 *raw, u32 size, const HsdDisc *disc, u32 *out_size, const char **error)
{
	static const char no_memory[] = "out of memory";
	Arc t;
	Out o;
	Source *src = NULL;
	u32 n_src = 0, i, k, link = 0, kept_refs = 0, strings_len = 0, data_size = 0, cursor, n_reloc = 0;
	u32 *offsets = NULL, *queue = NULL, *relocs = NULL;
	u8 *out = NULL, *w;
	const char *why;

	memset(&o, 0, sizeof(o));
	*error = NULL;
	*out_size = 0;
	why = arc_parse(&t, raw, size, 0);
	if (why)
		goto fail;
	src = (Source *)calloc(MAX_SOURCES, sizeof(Source));
	if (!src) {
		why = no_memory;
		goto fail;
	}
	for (i = 0; i < t.n_ext; i++)
		if (out_ext(&o, t.data + t.ext[i].start, t.ext[i].len, t.ext[i].start % ALIGN) < 0) {
			why = no_memory;
			goto fail;
		}
	for (i = 0; i < t.n_ext; i++)
		for (k = 0; k < t.ext[i].count; k++)
			if (out_ptr(&o, i, t.ptr[t.ext[i].first + k].field, t.ptr[t.ext[i].first + k].target) < 0) {
				why = no_memory;
				goto fail;
			}

	for (i = 0; i < t.n_refs; i++) {
		const char *name = name_at(&t, t.refs, i);
		Locator l;
		Source *s = NULL;
		u32 root, head, tail, n, at;
		if (strncmp(name, "slp:", 4) != 0) {
			kept_refs++;
			continue;
		}
		link++;
		if (locator_parse(name, &l) < 0) {
			why = "a link this version does not read";
			goto fail;
		}
		for (k = 0; k < n_src; k++)
			if (!strcmp(src[k].name, l.file))
				s = &src[k];
		if (!s) {
			u32 len = 0;
			if (n_src == MAX_SOURCES) {
				why = "links into too many disc files";
				goto fail;
			}
			s = &src[n_src];
			strcpy(s->name, l.file);
			s->raw = disc->load(disc->ctx, l.file, &len);
			if (!s->raw) {
				why = "a file its links name is not on the disc";
				goto fail;
			}
			n_src++;
			why = arc_parse(&s->arc, s->raw, len, 1);
			if (why)
				goto fail;
			s->mark = (u32 *)calloc(s->arc.n_ext + 1, sizeof(u32));
			s->index = (u32 *)malloc((s->arc.n_ext + 1) * sizeof(u32));
			if (!s->mark || !s->index) {
				why = no_memory;
				goto fail;
			}
		}
		if (follow(&s->arc, &l, &root) < 0) {
			why = "a link leads nowhere on this disc";
			goto fail;
		}
		/* the asset: what the root points to, and so on (not through its cut fields) */
		free(queue);
		queue = (u32 *)malloc((s->arc.n_ext + 1) * sizeof(u32));
		if (!queue) {
			why = no_memory;
			goto fail;
		}
		head = tail = 0;
		queue[tail++] = root;
		s->mark[root] = link;
		while (head < tail) {
			u32 e = queue[head++];
			const Ext *x = &s->arc.ext[e];
			for (k = 0; k < x->count; k++) {
				const Ptr *p = &s->arc.ptr[x->first + k];
				if ((e == root && is_cut(&l, p->field)) || s->mark[p->target] == link)
					continue;
				s->mark[p->target] = link;
				queue[tail++] = p->target;
			}
		}
		/* copied in the order they have in the disc's file */
		qsort(queue, tail, sizeof(u32), by_u32);
		for (n = 0; n < tail; n++) {
			const Ext *x = &s->arc.ext[queue[n]];
			s->index[queue[n]] = o.n_ext;
			if (out_ext(&o, s->arc.data + x->start, x->len, x->start % ALIGN) < 0) {
				why = no_memory;
				goto fail;
			}
		}
		for (n = 0; n < tail; n++) {
			u32 e = queue[n];
			const Ext *x = &s->arc.ext[e];
			for (k = 0; k < x->count; k++) {
				const Ptr *p = &s->arc.ptr[x->first + k];
				if ((e == root && is_cut(&l, p->field)) || s->mark[p->target] != link)
					continue;
				if (out_ptr(&o, s->index[e], p->field, s->index[p->target]) < 0) {
					why = no_memory;
					goto fail;
				}
			}
		}
		/* every place that named the link now points at the asset */
		for (at = be32(t.refs + i * 8);;) {
			u32 next = be32(t.data + at), e = extent_of(&t, at);
			put32(t.data + at, 0);
			if (at - t.ext[e].start + 4 > t.ext[e].len) {
				why = "a link's place straddles two structs";
				goto fail;
			}
			if (out_ptr(&o, e, at - t.ext[e].start, s->index[root]) < 0) {
				why = no_memory;
				goto fail;
			}
			if (next == CHAIN_END)
				break;
			at = next;
		}
	}

	/* ---- written out: the extents one after the other, each at its place modulo 32 */
	offsets = (u32 *)malloc((o.n_ext + 1) * sizeof(u32));
	relocs = (u32 *)malloc((o.n_ptr + 1) * sizeof(u32));
	if (!offsets || !relocs) {
		why = no_memory;
		goto fail;
	}
	cursor = 0;
	for (i = 0; i < o.n_ext; i++) {
		cursor += (o.ext[i].phase + ALIGN - cursor % ALIGN) % ALIGN;
		offsets[i] = cursor;
		cursor += o.ext[i].len;
	}
	data_size = cursor;
	if (link) {
		/* the names anew: the roots', then the external symbols left */
		for (i = 0; i < t.n_roots; i++)
			strings_len += (u32)strlen(name_at(&t, t.roots, i)) + 1;
		for (i = 0; i < t.n_refs; i++)
			if (strncmp(name_at(&t, t.refs, i), "slp:", 4) != 0)
				strings_len += (u32)strlen(name_at(&t, t.refs, i)) + 1;
	} else {
		strings_len = t.strings_size;
	}
	*out_size = HEADER + data_size + o.n_ptr * 4 + (t.n_roots + kept_refs) * 8 + strings_len;
	out = (u8 *)hsd_alloc(*out_size);
	if (!out) {
		why = no_memory;
		goto fail;
	}
	memset(out, 0, HEADER + data_size);
	put32(out, *out_size);
	put32(out + 4, data_size);
	put32(out + 8, o.n_ptr);
	put32(out + 12, t.n_roots);
	put32(out + 16, kept_refs);
	memcpy(out + 0x14, raw + 0x14, 12);	/* the version, and the header's last 8 bytes */
	w = out + HEADER;
	for (i = 0; i < o.n_ext; i++)
		memcpy(w + offsets[i], o.ext[i].data, o.ext[i].len);
	for (i = 0; i < o.n_ptr; i++) {
		u32 at = offsets[o.ptr[i].ext] + o.ptr[i].field;
		put32(w + at, offsets[o.ptr[i].target]);
		relocs[n_reloc++] = at;
	}
	/* an external symbol kept: its chain of places, at their new addresses */
	for (i = 0; i < t.n_refs; i++) {
		u32 at;
		if (strncmp(name_at(&t, t.refs, i), "slp:", 4) == 0)
			continue;
		for (at = be32(t.refs + i * 8);;) {
			u32 next = be32(t.data + at), e = extent_of(&t, at), here = offsets[e] + (at - t.ext[e].start);
			if (next == CHAIN_END) {
				put32(w + here, CHAIN_END);
				break;
			}
			e = extent_of(&t, next);
			put32(w + here, offsets[e] + (next - t.ext[e].start));
			at = next;
		}
	}
	qsort(relocs, n_reloc, sizeof(u32), by_u32);
	w += data_size;
	for (i = 0; i < n_reloc; i++, w += 4)
		put32(w, relocs[i]);
	{
		u8 *names = w + (t.n_roots + kept_refs) * 8;
		u32 at = 0;
		for (i = 0; i < t.n_roots; i++, w += 8) {
			const char *name = name_at(&t, t.roots, i);
			put32(w, offsets[t.root_ext[i]]);
			put32(w + 4, link ? at : be32(t.roots + i * 8 + 4));
			if (link) {
				strcpy((char *)names + at, name);
				at += (u32)strlen(name) + 1;
			}
		}
		for (i = 0; i < t.n_refs; i++) {
			const char *name = name_at(&t, t.refs, i);
			u32 head, e;
			if (strncmp(name, "slp:", 4) == 0)
				continue;
			head = be32(t.refs + i * 8);
			e = extent_of(&t, head);
			put32(w, offsets[e] + (head - t.ext[e].start));
			put32(w + 4, link ? at : be32(t.refs + i * 8 + 4));
			if (link) {
				strcpy((char *)names + at, name);
				at += (u32)strlen(name) + 1;
			}
			w += 8;
		}
		if (!link)
			memcpy(names, t.strings, t.strings_size);
	}
	why = NULL;

fail:
	for (i = 0; src && i < MAX_SOURCES; i++) {
		arc_free(&src[i].arc);
		free(src[i].mark);
		free(src[i].index);
		if (src[i].raw)
			disc->unload(disc->ctx, src[i].raw);
	}
	free(src);
	arc_free(&t);
	free(o.ext);
	free(o.ptr);
	free(offsets);
	free(queue);
	free(relocs);
	if (why) {
		*error = why;
		*out_size = 0;
		return NULL;
	}
	return out;
}
