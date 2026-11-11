/* slippi_online/so_prefetch.c
 * Disc prefetch between games (see so_prefetch.h).
 */
#include "so_prefetch.h"
#include "so_platform.h"
#include "so_util.h"
#include "ISO.h"
#include "string.h"
#include "common.h"

#include "so_prefetch_tables.h"

/* The engine's scratch (rollback.c RB_SCRATCH, so_platform.h): its tables
 * and rings take the first 0x50000; the page copies, the rest. */
#define PF_BASE (0x11570000u + 0x60000u)
#define PF_END (0x11570000u + SO_PPC_SCRATCH_SIZE)
#define PF_FST_MAX 0x10000	/* Melee's: 0x7529 */
#define PF_FILES 64
#define PF_CHUNK 0x10000
#define PF_MISS_HOLD_MS 40	/* after a game read missed: its reads go first */
#define PF_PRIORITY 0x48	/* below the main loop, DI and the network threads */

typedef struct {
	u32 offset, size, done;	/* disc offset and size; bytes here so far */
	u8 *data;
	u8 prio;
} PfFile;

static struct {
	volatile int state;	/* 0 not open yet, 1 open, 2 closed (a game) */
	volatile u32 wants;	/* bumped by every pick */
	u32 planned;		/* the wants the list was made from */
	volatile u32 miss_until_ms;
	u32 used;		/* bytes of the region given out (the FST first) */
	u32 fst_n;
	const u8 *fst;
	PfFile f[PF_FILES];
	u32 n;
	/* the picks: [0] this console's, [1] the opponent's; and the last game's */
	s16 ch[2], color[2], last_ch[2], last_color[2];
	s16 stage;
	s16 guess;	/* this console's character, from the game's own read of it on the CSS */
	u32 kind_off[27];	/* each kind's main file (pf_kinds dat), for that */
	/* served since the last so_pf_log */
	u32 hits, hit_kb, misses;
	u32 read_kb, read_ms;
} pf = { .ch = { -1, -1 }, .last_ch = { -1, -1 }, .stage = -1, .guess = -1 };

static u8 stack[0x2000] __attribute__((aligned(32)));

/* ---- the disc's file table -------------------------------------------------- */

/* A file's disc offset and size by its path ("audio/us/fox.ssm"); 0 when absent */
static int fst_find(const char *path, u32 *off, u32 *size)
{
	const u8 *fst = pf.fst;
	const char *names = (const char *)fst + pf.fst_n * 12, *p = path;
	u32 i = 1, end = pf.fst_n;
	if (!fst)
		return 0;
	while (*p) {
		const char *slash = strchr(p, '/');
		u32 len = slash ? (u32)(slash - p) : (u32)strlen(p), j = i;
		int found = 0;
		while (j < end) {
			u32 w0 = so_get_u32(fst + j * 12), a = so_get_u32(fst + j * 12 + 4), b = so_get_u32(fst + j * 12 + 8);
			const char *nm = names + (w0 & 0xFFFFFF);
			int dir = (w0 >> 24) != 0;
			if ((const u8 *)nm < fst + PF_FST_MAX && !strncmp(nm, p, len) && nm[len] == 0) {
				if (slash && dir) {
					i = j + 1;
					end = b;
					p = slash + 1;
					found = 1;
					break;
				}
				if (!slash && !dir) {
					*off = a;
					*size = b;
					return 1;
				}
				return 0;
			}
			j = dir ? b : j + 1;
		}
		if (!found)
			return 0;
	}
	return 0;
}

/* (with the FatFS lock held) */
static int read_fst(void)
{
	u8 *buf = (u8 *)PF_BASE;
	u32 off, size;
	if (!ISOReadInto(buf, 0x20, 0x420))
		return 0;
	off = so_get_u32(buf + 4);
	size = so_get_u32(buf + 8);
	if (size < 12 || size > PF_FST_MAX || !ISOReadInto(buf, size, off))
		return 0;
	sync_after_write(buf, size);
	pf.fst = buf;
	pf.fst_n = so_get_u32(buf + 8);
	if (pf.fst_n * 12 > size) {
		pf.fst = 0;
		return 0;
	}
	pf.used = PF_FST_MAX;
	return 1;
}

/* ---- the plan --------------------------------------------------------------- */

static void add(const char *name, u8 prio)
{
	u32 off, size, i;
	if (!name || !name[0] || pf.n >= PF_FILES || !fst_find(name, &off, &size) || !size)
		return;
	size = ((size + 31) & ~31u) + 32;	/* (the game's reads run to 32-byte multiples) */
	for (i = 0; i < pf.n; i++)
		if (pf.f[i].offset == off) {
			if (prio < pf.f[i].prio)
				pf.f[i].prio = prio;
			return;
		}
	if (pf.used + ((size + 31) & ~31u) > PF_END - PF_BASE)
		return;
	pf.f[pf.n].offset = off;
	pf.f[pf.n].size = size;
	pf.f[pf.n].done = 0;
	pf.f[pf.n].data = (u8 *)(PF_BASE + pf.used);
	pf.f[pf.n].prio = prio;
	pf.used += (size + 31) & ~31u;
	pf.n++;
}

static void add_char(int character, int color, u8 prio, int all)
{
	int k;
	if (character < 0 || character >= (int)(sizeof(pf_chars) / sizeof(pf_chars[0])))
		return;
	add(pf_chars[character].ssm, prio);
	if (!all)	/* (this console's own: the game preloads them on the CSS) */
		return;
	for (k = 0; k < 2; k++) {
		int kind = pf_chars[character].kind[k];
		const PfKind *d;
		if (kind < 0 || kind >= (int)(sizeof(pf_kinds) / sizeof(pf_kinds[0])))
			continue;
		d = &pf_kinds[kind];
		add(d->dat, prio);
		if (color >= 0 && color < 6)
			add(d->costume[color], prio);
		add(d->ef, prio);
		add(d->aj, prio);
	}
}

static const struct { u16 id; const char *file, *ssm, *ssm2; } stages[] = {
	{ 0x2, "GrIz.dat", "audio/us/pupupu.ssm", 0 },
	{ 0x3, "GrPs.usd", "audio/us/pstadium.ssm", 0 },
	{ 0x8, "GrSt.dat", "audio/us/garden.ssm", "audio/us/greens.ssm" },
	{ 0x1C, "GrOp.dat", "audio/us/pupupu.ssm", 0 },
	{ 0x1F, "GrNBa.dat", "audio/us/last.ssm", 0 },
	{ 0x20, "GrNLa.dat", "audio/us/last.ssm", 0 },
};

static void add_stage(int id, u8 prio)
{
	u32 i;
	for (i = 0; i < sizeof(stages) / sizeof(stages[0]); i++)
		if (id < 0 || stages[i].id == id) {
			add(stages[i].file, prio);
			add(stages[i].ssm, prio);
			add(stages[i].ssm2, prio);
		}
}

static const char *const common[] = {
	"IrAls.usd", "PlCo.dat", "TyDatai.usd", "IrRdMap.usd", "GmIntEz.dat", "SdIntro.dat",
	"GmPause.usd", "IfCoGet.dat", "LbRf.dat", "PdPm.dat", "LbBf.dat",
};

/* (with the FatFS lock held) New files go after the ones there, each with
 * its priority: the opponent's pick first, then what every match loads. */
static void plan(void)
{
	u32 i;
	pf.planned = pf.wants;
	add_char(pf.ch[1], pf.color[1], 0, 1);
	add_stage(pf.stage, 1);
	for (i = 0; i < sizeof(common) / sizeof(common[0]); i++)
		add(common[i], 2);
	add_char(pf.ch[0] >= 0 ? pf.ch[0] : pf.guess, 0, 2, 0);
	if (pf.last_ch[1] != pf.ch[1])
		add_char(pf.last_ch[1], pf.last_color[1], 3, 1);
	add_stage(-1, 4);
	add_char(pf.last_ch[0], pf.last_color[0], 5, 0);
}

static PfFile *next_job(void)
{
	PfFile *best = 0;
	u32 i;
	for (i = 0; i < pf.n; i++)
		if (pf.f[i].done < pf.f[i].size && (!best || pf.f[i].prio < best->prio))
			best = &pf.f[i];
	return best;
}

/* ---- the thread ------------------------------------------------------------- */

static int open_locked(void)
{
	pf.n = 0;
	pf.used = 0;
	pf.fst = 0;
	pf.planned = pf.wants - 1;
	pf.hits = pf.hit_kb = pf.misses = pf.read_kb = pf.read_ms = 0;
	if (!read_fst())
		return 0;
	{
		u32 k, size;
		for (k = 0; k < 27; k++)
			if (!fst_find(pf_kinds[k].dat, &pf.kind_off[k], &size))
				pf.kind_off[k] = 0;
	}
	pf.state = 1;
	return 1;
}

static u32 pf_thread(void *arg)
{
	(void)arg;
	for (;;) {
		PfFile *f;
		u32 t0, n;
		if (pf.state == 0 && so_ppc_scratch_free) {
			so_fs_lock();
			if (pf.state == 0 && open_locked())
				so_log("prefetch: open");
			so_fs_unlock();
		}
		if (pf.state != 1 || (s32)(so_time_ms() - pf.miss_until_ms) < 0) {
			so_sleep_us(10000);
			continue;
		}
		so_fs_lock();
		if (pf.state == 1 && pf.planned != pf.wants)
			plan();
		f = pf.state == 1 ? next_job() : 0;
		if (!f) {
			so_fs_unlock();
			so_sleep_us(20000);
			continue;
		}
		n = f->size - f->done > PF_CHUNK ? PF_CHUNK : f->size - f->done;
		t0 = so_time_ms();
		if (ISOReadInto(f->data + f->done, n, f->offset + f->done)) {
			/* (no dirty lines left over the scratch: the engine's DMA would lose to them) */
			sync_after_write(f->data + f->done, n);
			f->done += n;
			pf.read_kb += n >> 10;
		} else {
			f->size = f->done;	/* (no disc: give up on it) */
		}
		pf.read_ms += so_time_ms() - t0;
		so_fs_unlock();
		so_sleep_us(1000);
	}
	return 0;
}

void so_pf_init(void)
{
	static int started;
	if (started)
		return;
	started = 1;
	if (so_thread_start(pf_thread, 0, stack + sizeof(stack), sizeof(stack), PF_PRIORITY) < 0)
		so_log("prefetch: no thread");
}

void so_pf_close(void)
{
	so_fs_lock();
	if (pf.state == 1)
		so_pf_log();
	pf.state = 2;
	pf.n = 0;
	pf.fst = 0;
	if (pf.ch[0] >= 0 || pf.ch[1] >= 0) {	/* the next CSS starts from this game's picks */
		int i;
		for (i = 0; i < 2; i++) {
			pf.last_ch[i] = pf.ch[i];
			pf.last_color[i] = pf.color[i];
			pf.ch[i] = -1;
		}
		pf.stage = -1;
		pf.guess = -1;
	}
	so_fs_unlock();
}

void so_pf_open(void)
{
	if (pf.state != 2 || !so_ppc_scratch_free)
		return;
	so_fs_lock();
	if (pf.state == 2 && !open_locked())
		so_log("prefetch: no file table");
	so_fs_unlock();
}

void so_pf_want_char(int who, u8 character, u8 color)
{
	if (who < 0 || who > 1 || (pf.ch[who] == character && pf.color[who] == color))
		return;
	pf.ch[who] = character;
	pf.color[who] = color;
	pf.wants++;
}

void so_pf_want_stage(u16 stage)
{
	if (pf.stage == (s16)stage)
		return;
	pf.stage = (s16)stage;
	pf.wants++;
}

const u8 *so_pf_lookup(u32 offset, u32 length)
{
	u32 i;
	if (pf.state != 1)
		return 0;
	for (i = 0; i < pf.n; i++) {
		PfFile *f = &pf.f[i];
		if (offset >= f->offset && offset + length <= f->offset + f->done) {
			pf.hits++;
			pf.hit_kb += length >> 10;
			return f->data + (offset - f->offset);
		}
	}
	pf.misses++;
	pf.miss_until_ms = so_time_ms() + PF_MISS_HOLD_MS;
	if (pf.ch[0] < 0)	/* the game loading a character on the CSS: this console's pick */
		for (i = 0; i < 27; i++)
			if (pf.kind_off[i] && pf.kind_off[i] == offset) {
				u32 c;
				for (c = 0; c < sizeof(pf_chars) / sizeof(pf_chars[0]); c++)
					if (pf_chars[c].kind[0] == (s8)i && pf.guess != (s16)c) {
						pf.guess = (s16)c;
						pf.wants++;
					}
				break;
			}
	return 0;
}

void so_pf_log(void)
{
	u32 i, kb = 0, ready = 0;
	for (i = 0; i < pf.n; i++) {
		kb += pf.f[i].done >> 10;
		ready += pf.f[i].done == pf.f[i].size;
	}
	so_log("prefetch: %u reads served (%u KB), %u missed; %u of %u files ready (%u KB), %u KB read in %u ms",
		(unsigned)pf.hits, (unsigned)pf.hit_kb, (unsigned)pf.misses, (unsigned)ready, (unsigned)pf.n, (unsigned)kb,
		(unsigned)pf.read_kb, (unsigned)pf.read_ms);
	pf.hits = pf.hit_kb = pf.misses = 0;
}
