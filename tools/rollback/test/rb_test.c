/* The rollback engine's own code (../rollback.c) on a PC, against a simulated
 * Wii: MEM1, SlippWii's MEM2 scratch and the locked cache at their Wii
 * addresses, the DMA engine as copies, and the MMU's changed bits set by every
 * write the "game" makes. A game of random writes, captures and loads at
 * random depths, as Slippi asks for them; after every load, each byte of MEM1
 * is checked: the saved ones are the loaded frame's, the rest (the parts left
 * out, the blocks preserved, memory outside the regions) as they were.
 *
 *   python tools/rollback/test/run.py [frames] [seed]
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RB_HOST 1
#include "../rollback.c"
#include "data.inc"	/* DATA_LIST: glue.s's regions and excludes (run.py) */

#define MEM1 0x80000000u
#define MEM1_SIZE 0x01800000u
#define ENGINE_AT 0x80190000u	/* below 2 MB: not written by the game */
#define DATA_AT 0x80191000u
#define WRAPPER_AT 0x80192000u
#define HEAP_LO 0x80BE7600u	/* a Wii game's main heap */
#define HEAP_HI 0x811DE400u

/* ---- the hardware ---------------------------------------------------------- */

static u64 tb;
static u32 bats[8] = { 0x800001FF, 0x00000002, 0xC0001FFF, 0x0000002A, 0x810000FF, 0x01000002, 0, 0 };
static u32 sdr1, sr8, last_cmd;

u64 timebase(void) { return tb += 61; }
u32 irq_off(void) { return 0x8000; }
void irq_restore(u32 msr) { (void)msr; }
void flush_code(u32 a, u32 n) { (void)a; (void)n; }
void store_lines(u32 a, u32 n) { (void)a; (void)n; }
void kill_lines(u32 a, u32 n) { (void)a; (void)n; }
void flush_lines(u32 a, u32 n) { (void)a; (void)n; }
void zero_lines(u32 a, u32 n) { memset((void *)(uintptr_t)a, 0, n * 32); }
u32 rd_bat(int i) { return bats[i]; }
/* as the Homebrew Channel leaves them: SBE on, MEM2 cached at 0x90000000 and
 * uncached at 0xD0000000 by the extra DBATs */
static const u32 xbats[8] = { 0x90001FFF, 0x10000002, 0xD0001FFF, 0x1000002A, 0, 0, 0, 0 };
u32 rd_hid4(void) { return 0x83900000; }
u32 rd_xbat(int i) { return xbats[i]; }
void set_dbat0(u32 u, u32 l) { bats[0] = u; bats[1] = l; }
void set_dbat2(u32 u, u32 l) { bats[4] = u; bats[5] = l; }
void set_dbat3(u32 u, u32 l) { bats[6] = u; bats[7] = l; }
u32 rd_sdr1(void) { return sdr1; }
void wr_sdr1(u32 v) { sdr1 = v; }
u32 rd_sr(u32 ea) { (void)ea; return sr8; }
void wr_sr(u32 ea, u32 v) { (void)ea; sr8 = v; }
void tlb_flush(void) {}
void tlb_kill(const u16 *pages, u32 n) { (void)pages; (void)n; }
void set_dec(u32 t) { (void)t; }
u32 pad_shim_at(void) { return 0; }
u32 early_shim_at(void) { return 0; }
u32 load_shim_at(void) { return 0; }
void lc_on(void) {}
void lc_off(void) {}
u32 lc_queue(void) { return 0; }
int host_dvd_status(void) { return 0; }

static void *phys(u32 p)
{
	if (p < MEM1_SIZE || (p >= 0x10000000u && p < 0x14000000u))
		return (void *)(uintptr_t)(p | 0x80000000u);
	fprintf(stderr, "DMA to physical %08x\n", p);
	exit(2);
}

void lc_load(u32 lc, u32 p, u32 blocks) { memcpy((void *)(uintptr_t)lc, phys(p), blocks * 32); }
void lc_store(u32 lc, u32 p, u32 blocks) { memcpy(phys(p), (void *)(uintptr_t)lc, blocks * 32); }

/* the kernel: 0xD9 says yes */
void host_exi(void *buf, int len, int mode)
{
	u8 *b = buf;
	(void)len;
	if (mode == 1)
		last_cmd = b[0];
	else
		b[0] = last_cmd == CMD_ROLLBACK;
}

static void map(u32 at, u32 size)
{
	if (VirtualAlloc((void *)(uintptr_t)at, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != (void *)(uintptr_t)at) {
		fprintf(stderr, "cannot map %08x\n", at);
		exit(2);
	}
}

/* ---- the game ----------------------------------------------------------------- */

static u32 rng = 1;
static u32 rnd(u32 n) { rng = rng * 1664525u + 1013904223u; return (rng >> 8) % n; }

static u8 *saved;	/* per byte of MEM1: in the savestate (the regions less the excludes) */
static u8 *keep_map;	/* per byte: preserved by this load */
static u8 *snap[CAPS];	/* MEM1 at each capture the engine keeps, oldest first */
static s32 snap_frame[CAPS];
static int snaps;

/* ---- the OS's alarms --------------------------------------------------------- */

/* The disc's and serial interface's alarms (in the parts left out: not
 * rewound) and two of the game's (rewound): OSAlarm is handler, tag, fire
 * (s64), prev, next, period (s64), start. The OS keeps them queued by fire. */
static const u32 io_alarms[] = { 0x804A74C8, 0x804A74F0, 0x804A7518, 0x804A7610, 0x804A7E38, 0x804A7E60 };
static const u32 game_alarms[] = { 0x80432A00, 0x80432A40 };
#define N_ALARMS 8
static s64 now_time;

static u32 alarm_at(int i) { return i < 6 ? io_alarms[i] : game_alarms[i - 6]; }
static int is_alarm(u32 a)
{
	int i;
	for (i = 0; i < N_ALARMS; i++)
		if (a >= alarm_at(i) && a < alarm_at(i) + 0x28)
			return 1;
	return 0;
}

static void os_w32(u32 a, u32 v)	/* the OS's write: the MMU marks the page */
{
	u32 p = (a - MEM1) >> 12;
	*(u32 *)(uintptr_t)a = v;
	if (p >= FIRST)
		*(u32 *)(uintptr_t)M2(HTAB + t16(T_PTE_OF)[p] * 8 + 4) |= 0x80;
}

static void os_insert(u32 a, s64 fire)
{
	u32 prev = 0, next = rd32(ALARM_QUEUE);
	os_w32(a + 8, (u32)((u64)fire >> 32));
	os_w32(a + 12, (u32)fire);
	while (next && alarm_fire(next) <= fire) {
		prev = next;
		next = rd32(next + 0x14);
	}
	os_w32(a + 0x10, prev);
	os_w32(a + 0x14, next);
	if (prev)
		os_w32(prev + 0x14, a);
	else
		os_w32(ALARM_QUEUE, a);
	if (next)
		os_w32(next + 0x10, a);
	else
		os_w32(ALARM_QUEUE + 4, a);
}

static void os_remove(u32 a)
{
	u32 prev = rd32(a + 0x10), next = rd32(a + 0x14);
	if (prev)
		os_w32(prev + 0x14, next);
	else
		os_w32(ALARM_QUEUE, next);
	if (next)
		os_w32(next + 0x10, prev);
	else
		os_w32(ALARM_QUEUE + 4, prev);
}

/* a frame's alarm work: some set, some cancelled, the due ones fired (the
 * first game alarm is periodic, as the controller sampling alarm) */
static void os_alarms(void)
{
	int i;
	now_time += 1000;
	for (i = 0; i < N_ALARMS; i++) {
		u32 a = alarm_at(i);
		if (!rd32(a) && rnd(3) == 0) {
			os_w32(a, 0x80001000 + i);
			os_w32(a + 0x18, i == 6 ? 0 : 0);
			os_insert(a, now_time + 100 + rnd(5000));
		} else if (rd32(a) && i != 6 && rnd(6) == 0) {
			os_remove(a);
			os_w32(a, 0);
		}
	}
	while (rd32(ALARM_QUEUE) && alarm_fire(rd32(ALARM_QUEUE)) <= now_time) {
		u32 a = rd32(ALARM_QUEUE);
		os_remove(a);
		if (a == game_alarms[0])
			os_insert(a, alarm_fire(a) + 1000);
		else
			os_w32(a, 0);
	}
}

/* After a load: the queue is sorted and linked both ways, and holds the I/O
 * alarms armed before the load and the game alarms armed at the frame loaded,
 * these as they were then. */
static int check_alarms(const u8 *before, const u8 *expect)
{
	u32 a = rd32(ALARM_QUEUE), prev = 0, count = 0, want = 0;
	int i;
	for (i = 0; i < N_ALARMS; i++) {
		u32 at = alarm_at(i);
		const u8 *src = i < 6 ? before : expect;
		u32 handler = *(const u32 *)(src + at - MEM1);
		if (handler)
			want++;
		if (i >= 6 && (rd32(at) != handler || rd32(at + 8) != *(const u32 *)(expect + at + 8 - MEM1) ||
			       rd32(at + 12) != *(const u32 *)(expect + at + 12 - MEM1))) {
			printf("ALARM %08x not the frame's\n", at);
			return 0;
		}
	}
	while (a) {
		if (!is_alarm(a) || rd32(a + 0x10) != prev || !rd32(a) || (prev && alarm_fire(prev) > alarm_fire(a)) || ++count > 16) {
			printf("ALARM QUEUE broken at %08x\n", a);
			return 0;
		}
		prev = a;
		a = rd32(a + 0x14);
	}
	if (rd32(ALARM_QUEUE + 4) != prev || count != want) {
		printf("ALARM QUEUE: %u queued, %u armed\n", count, want);
		return 0;
	}
	return 1;
}

/* the bytes the test must not write: the engine, the scene, the heap's
 * bounds, the queues the engine walks (left empty) */
static int reserved(u32 a)
{
	return (a >= ENGINE_AT && a < WRAPPER_AT + 0x100) || (a >= 0x80479D30 && a < 0x80479D34) ||
		(a >= 0x804D76B8 && a < 0x804D76C0) || (a >= 0x804D7360 && a < 0x804D7368) ||
		(a >= 0x804D6018 && a < 0x804D601C) || (a >= 0x804C6320 && a < 0x804C6330) ||
		(a >= 0x804D77F0 && a < 0x804D7810) || (a >= ALARM_QUEUE && a < ALARM_QUEUE + 8) ||
		(a >= EXI_BRANCH && a < EXI_BRANCH + 4) || is_alarm(a);
}

static Engine *game_engine;
static u32 dsi_faults;

static void game_write(u32 a, u32 len)
{
	u32 i;
	for (i = 0; i < len; i++, a++) {
		if (a >= MEM1 + MEM1_SIZE || reserved(a))
			continue;
		{
			u32 p = (a - MEM1) >> 12;
			if (p >= FIRST) {
				/* the MMU: a read-only page faults first (background saves: rb_dsi) */
				volatile u32 *w1 = (volatile u32 *)(uintptr_t)M2(HTAB + t16(T_PTE_OF)[p] * 8 + 4);
				if ((*w1 & 3) == 3 || (*w1 & 3) == 0) {	/* read only, or no access (Ks 1) */
					rb_dsi(game_engine, a);
					dsi_faults++;
					if ((*w1 & 3) != 2) {
						printf("a write to %08x: its page still read-only after rb_dsi\n", a);
						exit(1);
					}
				}
			}
		}
		*(u8 *)(uintptr_t)a = (u8)rnd(256);
		{
			u32 p = (a - MEM1) >> 12;
			if (p >= FIRST)	/* the MMU marks the page */
				*(u32 *)(uintptr_t)M2(HTAB + t16(T_PTE_OF)[p] * 8 + 4) |= 0x80;
		}
	}
}

/* one frame's writes: mostly the regions, some anywhere */
static void play_frame(const u32 *list)
{
	int w, n = 50 + rnd(400);
	for (w = 0; w < n; w++) {
		u32 a, len = rnd(4) == 0 ? 32 + rnd(256) : 1 + rnd(8);
		if (rnd(10) < 8) {
			u32 r = rnd(4), lo = list[2 * r], hi = list[2 * r + 1];
			a = lo + rnd(hi - lo);
		} else {
			a = MEM1 + rnd(MEM1_SIZE);
		}
		game_write(a, len);
	}
}

/* Slippi's SSRB: [cmd][frame][ODB addr, size][RXB addr, size][SSCB addr, size][0] */
static void ssrb(u8 *b, u8 cmd, s32 frame)
{
	static const u32 blocks[6] = { HEAP_LO + 0x1000, 0x6E0, HEAP_LO + 0x2000, 0x129, HEAP_LO + 0x3004, 0x3A9 };
	int i;
	memset(b, 0, 33);
	b[0] = cmd;
	for (i = 0; i < 4; i++)
		b[1 + i] = (u8)((u32)frame >> (24 - 8 * i));
	for (i = 0; i < 6; i++) {
		b[5 + i * 4] = (u8)(blocks[i] >> 24);
		b[6 + i * 4] = (u8)(blocks[i] >> 16);
		b[7 + i * 4] = (u8)(blocks[i] >> 8);
		b[8 + i * 4] = (u8)blocks[i];
	}
}

static int check_load(s32 frame, const u8 *before, const u8 *expect, const u8 *blocks)
{
	u32 i, bad = 0, first = 0, k;
	memset(keep_map, 0, MEM1_SIZE);
	for (k = 0; k < 3; k++) {
		u32 at = be32(blocks + k * 8) - MEM1, size = be32(blocks + k * 8 + 4);
		memset(keep_map + at, 1, size);
	}
	for (i = 0; i < MEM1_SIZE; i++) {
		u8 now = *(u8 *)(uintptr_t)(MEM1 + i);
		u8 want = saved[i] && !keep_map[i] ? expect[i] : before[i];
		if (reserved(MEM1 + i))
			continue;
		if (now != want && !bad++)
			first = i;
	}
	if (bad) {
		u32 a = MEM1 + first;
		printf("LOAD of frame %d: %u bytes wrong, the first at %08x (now %02x, frame's %02x, before %02x, saved %u, kept %u)\n",
			(int)frame, bad, a, *(u8 *)(uintptr_t)a, expect[first], before[first], saved[first], keep_map[first]);
		return 0;
	}
	return 1;
}

/* ---- frames a refresh sooner: direct_track's decisions, with no console -------- */

static int direct_fails, direct_checks;
#define CHECK(cond) do { direct_checks++; if (!(cond)) { printf("direct: FAILED line %d: %s\n", __LINE__, #cond); direct_fails++; } } while (0)
#define FLAG (*(volatile s32 *)(uintptr_t)VB_FLAG)
#define WORD (*SCRATCH_WORD(T_DIRECT))

/* A game's start, the kernel's word as given (1 under its ping limit, 3 over it; 0x200 the first game
 * against this opponent) */
static void direct_game(Engine *e, u32 word)
{
	u16 carry;
	direct_end(e);	/* (the game before it: rb_frame, at its end) */
	carry = e->deep_carry;	/* (the engine's own memory outlives a game) */
	memset(e, 0, sizeof(*e));
	e->deep_carry = carry;
	e->poll_cb = 1;
	WORD = word;
	FLAG = 0;
	direct_begin(e);
}

/* A frame drawn at this tick: level or a refresh behind, its pass's work in ms (over 12: a rollback of
 * two frames, 3 loops) */
static void direct_frame(Engine *e, u32 tick, int behind, u32 work_ms)
{
	e->pass_n[work_ms > 12 ? 2 : 0]++;	/* (count_frame's, at the pass's end) */
	e->game_frames = tick;
	*(volatile u32 *)(uintptr_t)RETRACE_COUNT += 1;
	e->pass_poll_rc = (u16)(*(volatile u32 *)(uintptr_t)RETRACE_COUNT - (behind ? 2 : 1));
	e->pass_start = (u32)tb - work_ms * 1000 * 61;
	direct_track(e);
}

static void direct_frames(Engine *e, u32 from, u32 to, int behind, u32 work_ms)
{
	u32 t;
	for (t = from; t <= to; t++)
		direct_frame(e, t, behind, work_ms);
}

/* The visual buffer fix's check takes the skip (the game's own code), and the next frame starts */
static void direct_take(Engine *e)
{
	FLAG = 0;
	count_frame(e);
}

static int test_direct(void)
{
	static Engine engine;
	Engine *e = &engine;

	/* under the ping limit, a first game: the skip in the countdown, after DIRECT_START frames behind */
	direct_game(e, 1 | 0x200);
	direct_frames(e, 41, 48, 1, 6);
	CHECK(!e->direct_skip && FLAG == 0);
	direct_frame(e, 49, 1, 6);
	CHECK(e->direct_skip == 1 && FLAG == -1 && (e->direct_is & 1) && e->direct_skips == 1);
	/* a rollback loads a state from before the asking: the flag is back, and the skip is not "taken" */
	FLAG = 0;
	direct_after_load(e);
	CHECK(FLAG == -1);
	count_frame(e);
	CHECK(e->direct_skip == 1);
	/* the rollback's own frame is drawn (its loops go round the check): still asked */
	FLAG = 0;
	direct_frame(e, 51, 1, 15);
	CHECK(e->direct_skip == 1 && FLAG == -1 && e->direct_skips == 1);
	/* taken; level to tick 300 */
	direct_take(e);
	CHECK(e->direct_skip == 0 && FLAG == 0);
	direct_frames(e, 52, 300, 0, 6);
	CHECK((e->direct_is & 1) && !e->direct_skip && FLAG == 0 && e->direct_overruns == 0 && e->direct_skips == 1);
	/* a state from the asking's time, loaded after the skip was taken: not taken twice */
	FLAG = -1;
	direct_after_load(e);
	CHECK(FLAG == 0);
	/* a frame late in play: behind to the game's end, no skip again */
	direct_frame(e, 301, 1, 21);
	CHECK(e->direct_overruns == 1 && (e->direct_is & 8) && !(e->direct_is & 1) && !e->direct_skip && e->deep_game == 1);
	direct_frames(e, 302, 2400, 1, 6);
	CHECK(!e->direct_skip && FLAG == 0 && e->direct_skips == 1);

	/* the games after it, the same opponent: the run lost in play and its long pass are against a skip
	 * for two games, and not in the third (none of them with such a pass) */
	direct_game(e, 1);
	CHECK(e->deep_carry == 10);
	direct_frames(e, 41, 2400, 1, 6);
	CHECK(!e->direct_skip && e->direct_skips == 0 && e->direct_behind == 2360);
	direct_game(e, 1);
	CHECK(e->deep_carry == 5);
	direct_frames(e, 41, 2400, 1, 6);
	CHECK(e->direct_skips == 0);
	direct_game(e, 1);
	CHECK(e->deep_carry == 2);
	direct_frames(e, 41, 49, 1, 6);
	CHECK(e->direct_skip == 1);
	/* passes over DIRECT_FIT_US are counted, level or behind; shorter ones are not */
	direct_take(e);
	e->deep_carry = 0;
	direct_frames(e, 50, 60, 0, 18);
	CHECK(e->deep_game == 0);
	direct_frame(e, 61, 0, 20);
	CHECK(e->deep_game == 1);
	/* one in a game leaves the next one level (and the one the pad's read moved at is none) */
	direct_frames(e, 62, 2400, 0, 6);
	direct_game(e, 1);
	CHECK(e->deep_carry == 2);
	direct_frame(e, 40, 1, 33);
	CHECK(e->deep_game == 0);
	direct_frames(e, 41, 49, 1, 6);
	CHECK(e->direct_skip == 1);
	/* two do not, be it a game behind: the next is behind, and the one after it level again */
	direct_take(e);
	e->deep_carry = 0;
	direct_frames(e, 50, 60, 0, 6);
	direct_frame(e, 61, 0, 20);
	direct_frame(e, 62, 1, 20);
	CHECK(e->deep_game == 2 && e->direct_overruns == 1 && !(e->direct_is & 8));
	direct_frames(e, 63, 2400, 1, 6);
	CHECK(e->direct_skips == 1);
	direct_game(e, 1);
	CHECK(e->deep_carry == 4);
	direct_frames(e, 41, 2400, 1, 6);
	CHECK(e->direct_skips == 0);
	direct_game(e, 1);
	CHECK(e->deep_carry == 2);
	direct_frames(e, 41, 49, 1, 6);
	CHECK(e->direct_skip == 1);
	/* a game of them, and every game after it with one: behind for as long */
	direct_take(e);
	direct_frames(e, 50, 2400, 0, 6);
	e->deep_game = 12;
	direct_game(e, 1);
	CHECK(e->deep_carry == 25);
	{
		int g;
		for (g = 0; g < 8; g++) {
			direct_frames(e, 41, 1000, 1, 6);
			direct_frame(e, 1001, 1, 20);
			CHECK(e->direct_skips == 0);
			direct_game(e, 1);
		}
		CHECK(e->deep_carry == 4);	/* (25, 14, 9, 6, 5, 4, and 4 from there) */
	}
	/* a game of deep rollbacks (1 pass in 10 of 3 loops), none of them too long and no frame late: the
	 * next two behind all the same */
	e->deep_carry = 0;
	direct_frames(e, 41, 49, 1, 6);
	CHECK(e->direct_skip == 1);
	direct_take(e);
	{
		u32 t;
		for (t = 50; t <= 2400; t++)
			direct_frame(e, t, 0, t % 10 ? 6 : 15);
	}
	CHECK(e->deep_game == 0 && e->direct_overruns == 0);
	direct_game(e, 1);
	CHECK(e->deep_carry == 8);
	direct_frames(e, 41, 2400, 1, 6);
	CHECK(e->direct_skips == 0);
	direct_game(e, 1);
	CHECK(e->deep_carry == 4);
	direct_frames(e, 41, 2400, 1, 6);
	CHECK(e->direct_skips == 0);
	direct_game(e, 1);
	CHECK(e->deep_carry == 2);
	direct_frames(e, 41, 49, 1, 6);
	CHECK(e->direct_skip == 1);
	/* ... and 1 in 20 is not one */
	direct_take(e);
	e->deep_carry = 0;
	{
		u32 t;
		for (t = 50; t <= 2400; t++)
			direct_frame(e, t, 0, t % 20 ? 6 : 15);
	}
	direct_game(e, 1);
	CHECK(e->deep_carry == 0);
	/* a game too short to say (its opponent left in the countdown): the number stays */
	e->deep_carry = 6;
	direct_frames(e, 41, 300, 1, 6);
	direct_game(e, 1);
	CHECK(e->deep_carry == 6);

	/* a new game's first ticks, before the engine runs it (poll_early_on), are not the last game's */
	e->poll_cb = 0;	/* (the game's end: poll_early_off) */
	direct_frames(e, 1, 39, 1, 25);
	CHECK(e->deep_game == 0);
	/* a game with two such passes by the time sync's end: no second skip in it */
	e->deep_carry = 0;
	direct_game(e, 1);
	direct_frames(e, 41, 49, 1, 6);
	CHECK(e->direct_skip == 1);
	direct_take(e);
	direct_frames(e, 50, 99, 0, 6);
	direct_frames(e, 100, 101, 0, 20);
	direct_frame(e, 102, 1, 6);
	CHECK(e->direct_overruns == 1 && !(e->direct_is & 8) && e->deep_game == 2);
	direct_frames(e, 103, 400, 1, 6);
	CHECK(!e->direct_skip && e->direct_skips == 1);
	e->deep_carry = e->deep_game = 0;

	/* over the ping limit, a first game against someone: counted only, whatever the games before it were */
	e->deep_carry = 50;
	direct_game(e, 3 | 0x200);
	CHECK(e->deep_carry == 0 && e->deep_game == 0);
	direct_frames(e, 41, 400, 1, 6);
	CHECK(!e->direct_skip && e->direct_skips == 0 && FLAG == 0);
	/* ... unless its ping, measured a few frames in, is under the limit after all */
	direct_game(e, 3 | 0x200);
	direct_frames(e, 41, 52, 1, 6);
	CHECK(!e->direct_skip);
	WORD = 1 | 0x200;
	direct_frame(e, 53, 1, 6);
	CHECK(e->direct_skip == 1);
	/* over the limit, an opponent already played whose last game fitted: from the countdown */
	direct_take(e);
	direct_game(e, 3);
	direct_frames(e, 41, 49, 1, 6);
	CHECK(e->direct_skip == 1);

	/* the start's time sync ends the run before tick 125: once more there, and that one stays */
	direct_take(e);
	direct_frames(e, 50, 99, 0, 6);
	direct_frame(e, 100, 1, 6);
	CHECK(e->direct_overruns == 1 && !(e->direct_is & 8) && !e->direct_skip);
	direct_frames(e, 101, 125, 1, 6);
	CHECK(!e->direct_skip);
	direct_frame(e, 126, 1, 6);
	CHECK(e->direct_skip == 1 && e->direct_skips == 2);
	/* never asked from DIRECT_LAST on */
	direct_game(e, 1);
	e->direct_hold = 200;
	direct_frames(e, 41, 400, 1, 6);
	CHECK(!e->direct_skip && e->direct_skips == 0);

	/* asked, and the game is level by itself before the skip is taken: withdrawn */
	direct_game(e, 1);
	direct_frames(e, 41, 49, 1, 6);
	CHECK(e->direct_skip == 1);
	direct_frame(e, 50, 0, 15);
	CHECK(!e->direct_skip && FLAG == 0 && (e->direct_is & 1));

	/* the kernel's word to 0 with a skip asked and not taken yet (the ping, measured in the countdown, too
	 * far over its limit): not asked any more */
	direct_game(e, 3);
	direct_frames(e, 41, 49, 1, 6);
	CHECK(e->direct_skip == 1 && FLAG == -1 && (e->direct_is & 1));
	WORD = 0;
	direct_frame(e, 50, 1, 6);
	CHECK(!e->direct_skip && FLAG == 0 && !(e->direct_is & 1) && e->direct_skips == 1);
	direct_frames(e, 51, 400, 1, 6);
	CHECK(!e->direct_skip && FLAG == 0 && e->direct_overruns == 0);
	/* the setting off: nothing, whatever the frames do */
	direct_game(e, 0);
	direct_frames(e, 41, 300, 1, 6);
	CHECK(!e->direct_skip && FLAG == 0 && !(e->direct_is & 1) && e->direct_skips == 0);
	/* tests' "level whatever comes": a skip whenever a frame is behind, in play too */
	direct_game(e, 2);
	e->deep_carry = 9;
	direct_frames(e, 41, 49, 1, 6);
	CHECK(e->direct_skip == 1);
	direct_take(e);
	direct_frames(e, 50, 700, 0, 6);
	direct_frame(e, 701, 1, 25);
	CHECK(e->direct_skip == 1 && e->direct_skips == 2);
	e->deep_carry = e->deep_game = 0;
	e->direct_is = 0;

	printf("direct: %d checks, %d failed\n", direct_checks, direct_fails);
	return direct_fails;
}

int main(int argc, char **argv)
{
	int frames = argc > 1 ? atoi(argv[1]) : 3000, f, loads = 0, captures = 0, i;
	Engine *e;
	Data *d;
	u8 *before, req[64];
	u32 list[2 * LIST_PAIRS], n;
	rng = argc > 2 ? (u32)atoi(argv[2]) : 1;

	map(MEM1, MEM1_SIZE);
	map(0x91000000u, 0x02000000u);	/* MEM2: the scratch */
	/* MEM2 outlives a reboot, and after a power cut holds anything: the engine's tables and rings are
	 * junk until the engine sets them, but for the words the kernel clears before the game runs
	 * (so_exi.c, so_exi_scratch_boot). (T_MARK was trusted as found: a bit set there was a page no load
	 * took from a later capture, 2026-11-10.) */
	memset((void *)(uintptr_t)M2(RB_SCRATCH), 0xA5, 0x60000);
	{
		static const u32 kernel_clears[] = { T_SLOW_FRAME, T_INPUT_WAIT, T_POLL_EARLY, T_BG_SAVES, T_VI_NUDGE, T_DIRECT,
		                                     T_REMOTE_HEAD, T_REMOTE_CHANGED, T_EVENTS, T_BG_RING };
		for (i = 0; i < (int)(sizeof(kernel_clears) / sizeof(kernel_clears[0])); i++)
			t32(kernel_clears[i])[0] = 0;
		for (i = 64; i <= 100; i += 4)	/* T_BG_DIAG and the DMA wait's HID2 */
			t32(T_GAME_FRAMES + i)[0] = 0;
	}
	map(LC, 0x4000);
	map(0xCC000000u, 0x10000);	/* PI: the GPU FIFO's bounds */
	*(u32 *)(uintptr_t)0xCC00300C = 0x0061CC00;
	*(u32 *)(uintptr_t)0xCC003010 = 0x0065CBE0;
	for (i = 0; i < (int)(MEM1_SIZE / 4); i++)
		((u32 *)(uintptr_t)MEM1)[i] = rng = rng * 1664525u + 1013904223u;
	for (i = 0; i < (int)MEM1_SIZE; i++)
		if (reserved(MEM1 + i))
			*(u8 *)(uintptr_t)(MEM1 + i) = 0;
	*(u8 *)(uintptr_t)0x80479D30 = 8;	/* online, in game */
	*(u8 *)(uintptr_t)0x80479D33 = 2;
	*(u32 *)(uintptr_t)0x804D76B8 = HEAP_LO;
	*(u32 *)(uintptr_t)0x804D76BC = HEAP_HI;
	*(u32 *)(uintptr_t)EXI_BRANCH = 0x48000100;	/* Slippi's branch */
	/* region C as in a game: the OS heaps' descriptors (6), a small heap 0, the
	 * audio heap (1) as two cells, an allocated one and a free one */
	*(u32 *)(uintptr_t)0x804D7360 = 0x8065CC00;
	*(u32 *)(uintptr_t)0x804D7364 = 6;
	*(u32 *)(uintptr_t)0x804D6018 = 1;
	*(u32 *)(uintptr_t)0x8065CC0C = 0x80000;	/* heap 1: size, free, allocated */
	*(u32 *)(uintptr_t)0x8065CC10 = 0x80690000;
	*(u32 *)(uintptr_t)0x8065CC14 = 0x80660000;
	*(u32 *)(uintptr_t)0x80660000 = 0;	/* its cells: prev, next, size */
	*(u32 *)(uintptr_t)0x80660004 = 0;
	*(u32 *)(uintptr_t)0x80660008 = 0x30000;
	*(u32 *)(uintptr_t)0x80690000 = 0;
	*(u32 *)(uintptr_t)0x80690004 = 0;
	*(u32 *)(uintptr_t)0x80690008 = 0x50000;

	e = (Engine *)(uintptr_t)ENGINE_AT;
	d = (Data *)(uintptr_t)DATA_AT;
	memset(e, 0, sizeof(*e));
	memcpy(d->list, DATA_LIST, sizeof(DATA_LIST));

	game_engine = e;
	t32(T_BG_SAVES)[0] = getenv("RB_BG") != NULL;	/* background saves: RB_BG=1 */
	rb_frame(e, WRAPPER_AT, d, 0);	/* the game's first frame: the base */
	if (!e->active) {
		printf("the engine did not start (failed %u, errors %u)\n", e->failed, e->errors);
		return 1;
	}

	/* the savestate's bytes, as the engine's list says (with region C's parts
	 * found at run time): the regions less the excludes */
	memcpy(list, t32(T_LIST), sizeof(list));
	if (list[6] != HEAP_LO || list[7] != HEAP_HI) {
		printf("the heap is not in the list\n");
		return 1;
	}
	saved = calloc(MEM1_SIZE, 1);
	keep_map = calloc(MEM1_SIZE, 1);
	before = malloc(MEM1_SIZE);
	for (n = 0; n < 4; n++)
		memset(saved + list[2 * n] - MEM1, 1, list[2 * n + 1] - list[2 * n]);
	for (n = 8; list[n]; n += 2)
		memset(saved + list[n] - MEM1, 0, list[n + 1] - list[n]);
	for (n = 0x8065C000; n < 0x8071B000;) {	/* region C: what is saved */
		u32 m = n;
		while (n < 0x8071B000 && saved[n - MEM1] == saved[m - MEM1])
			n++;
		printf("region C %08x-%08x %s\n", m, n, saved[m - MEM1] ? "saved" : "left out");
	}
	for (i = 0; i < CAPS; i++)
		snap[i] = malloc(MEM1_SIZE);
	for (f = 2; f < frames; f++) {
		rb_frame(e, WRAPPER_AT, d, 0);
		play_frame(list);
		os_alarms();
		/* Slippi: a capture each predicted frame, a load now and then of one
		 * of the last 7, then the frames again from there */
		ssrb(req, 0xB1, f);
		rb_exi(e, req, 33, 1);
		captures++;
		if (snaps == CAPS) {	/* the engine keeps 8: the oldest goes */
			u8 *oldest = snap[0];
			memmove(snap, snap + 1, sizeof(snap[0]) * (CAPS - 1));
			memmove(snap_frame, snap_frame + 1, sizeof(snap_frame[0]) * (CAPS - 1));
			snap[CAPS - 1] = oldest;
			snaps--;
		}
		memcpy(snap[snaps], (void *)(uintptr_t)MEM1, MEM1_SIZE);
		snap_frame[snaps++] = f;
		if (snaps > 1 && rnd(3) == 0) {
			int back = 1 + rnd(snaps - 1 < 7 ? snaps - 1 : 7);	/* frames back */
			int s = snaps - 1 - back;
			play_frame(list);	/* written since the last capture */
			os_alarms();
			memcpy(before, (void *)(uintptr_t)MEM1, MEM1_SIZE);
			ssrb(req, 0xB2, snap_frame[s]);
			rb_exi(e, req, 33, 1);
			loads++;
			if (!check_load(snap_frame[s], before, snap[s], req + 5) || !check_alarms(before, snap[s])) {
				printf("after %d captures, %d loads (frame %d, %d back; misses %u, errors %u)\n", captures, loads, f, back,
					e->misses, e->errors);
				return 1;
			}
			f = snap_frame[s];	/* Slippi runs the frames again */
			snaps = s + 1;
		}
	}
	if (e->verify_bad) {
		printf("VERIFY: %u differences, the first at %08x\n", e->verify_bad, e->verify_at);
		return 1;
	}
	if (e->unmarked) {	/* every write here sets its page's C bit */
		printf("VERIFY: %u pages changed unmarked, the first at %08x\n", e->unmarked, 0x80000000u + ((u32)e->unmarked_lo[0] << 12));
		return 1;
	}
	{
		/* The check itself: a write that sets no C bit (a DMA's) into a
		 * saved page, after a capture, must be what the next load's
		 * all-pages check (every 16th load) reports. */
		const u32 at = HEAP_LO + 0x20001;
		ssrb(req, 0xB1, f);
		rb_exi(e, req, 33, 1);
		*(volatile u8 *)(uintptr_t)at ^= 0xFF;
		e->loads = (e->loads + 15) & ~15u;
		ssrb(req, 0xB2, f);
		rb_exi(e, req, 33, 1);
		if (e->unmarked != 1 || e->unmarked_at[0] != at) {
			printf("VERIFY: a write without a C bit at %08x was not found (%u found, the first at %08x)\n", at,
				e->unmarked, e->unmarked_at[0]);
			return 1;
		}
	}
	printf("bg: enabled %u hooked %u active %u faults %u spurious %u finishes %u\n", t32(T_BG_SAVES)[0], bg()->hooked,
		bg()->active, bg()->faults, bg()->spurious, bg()->finishes);
	if (test_direct())
		return 1;
	printf("ok: %d frames, %d captures, %d loads; misses %u, errors %u; pages tracked %u; background faults %u\n", frames,
		captures, loads, e->misses, e->errors, e->tracked, dsi_faults);
	return 0;
}
