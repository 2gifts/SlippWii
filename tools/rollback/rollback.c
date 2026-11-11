/* SlippWii rollback (see glue.s): Slippi's savestates on the Wii's CPU.
 *
 * Write tracking. A savestate only needs the pages the game wrote since the
 * last one (about 0.8 MB a frame of the 7.9 MB Slippi saves), and the CPU's
 * MMU can tell which: with the game's memory mapped through a page table
 * instead of its BATs, the hardware sets a page's C (changed) bit on the
 * first write to it. During online games, the first 2 MB of MEM1 (code and
 * the low data) stay on a BAT and are always copied; the rest of MEM1 goes
 * through a page table in MEM2 (SlippWii's scratch, kernel
 * SO_PPC_SCRATCH_SIZE), reached through the Homebrew Channel's own MEM2 BAT
 * (mem2_mapped). The table is read from memory, never through the data
 * cache (changed_pages). DBAT3 maps the locked cache while its DMA engine
 * does the copies.
 *
 * Only the CPU's writes set C bits: what DMA writes (the disc, the GPU's
 * texture copies, the DSP) goes unseen. That is by design for I/O, which is
 * never rewound (excludes.py); the game's own disc loads finish inside their
 * frame (sync_loads.s), so their data never spans a savestate. Test builds
 * (RB_VERIFY) check every restore, and every 16th load all pages, for bytes
 * that changed without a C bit (verify_load).
 *
 * Savestates. At a game's first frame, every page of Slippi's savestate
 * regions is copied to MEM2 (the base). A capture (0xB1) copies the pages
 * written since the last capture, each a new version of its page; a load
 * (0xB2) of frame F writes back every page written since F's capture, from
 * its newest version up to F (or the base), and drops the later captures.
 * Only the bytes Slippi Dolphin saves are written back: not the parts it
 * leaves out (sound, the XFB, ...), nor the blocks the load must preserve
 * (Slippi's ODB, RXB, SSCB, given with the request). Up to 8 captures are
 * kept (Slippi needs 7); the oldest is folded into the base by moving
 * pointers, no copy.
 *
 * The kernel only predicts (and so only asks for savestates) in a game
 * whose engine said it was ready (EXI 0xD9, so_exi.c). Each game's numbers
 * go to the kernel's log over EXI 0xDA kind 4 ("PERF rb ...").
 *
 * Position independent (tools/pic_code.py): no globals or constant data.
 */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef signed char s8;
typedef int s32;
typedef long long s64;
typedef unsigned long long u64;

#define RB_VERSION 1
/* RB_VERIFY_DEEP (tests: build_online_boot.py --rollback-verify-deep): the engine as a player has it, not
 * built for size and with no check of every 4th load, for a crash that came 0-5 frames after a lag spike's
 * rollback of 6-7 frames, in the game's object allocators each time (2026-11-10: three, about one in 150
 * such spikes). Each copy of a page is made with a sum of the page as the CPU has it then (T_SUMS,
 * page_sum: 15-20 us a page, 4-5 ms a capture), and a load VD_DEPTH captures back or more looks for
 * those sums in the pages it put back, and in every other saved page (vd_check): a copy that is not
 * what was saved, a page put back from the wrong one, a page changed with no C bit. vd_report says
 * what it found. */
#define VD_DEPTH 4
#ifndef RB_SCRATCH
#define RB_SCRATCH 0x11570000u	/* physical, 64 KB aligned: 16 MB (SO_PPC_SCRATCH_SIZE) */
#endif
#define M2(phys) ((phys) | 0x80000000u)	/* MEM2, cached: 0x90000000 = 0x10000000 (mem2_mapped) */
#define HTAB RB_SCRATCH			/* the page table: 64 KB, 1024 groups of 8 */
#define T_PTE_OF 0x10000		/* u16 per MEM1 page: its PTE */
#define T_CLASS 0x13000			/* u8 per MEM1 page: 0 not saved, 1 whole, 2 in part */
#define T_MARK 0x14C00			/* bit per MEM1 page: on this load's list (clear between loads, from a game's start) */
#define T_LIST 0x2B000			/* the regions and excludes, the heap's filled in (LIST_PAIRS) */
#define T_BASE 0x15000			/* u16 per MEM1 page: the base's copy */
#define T_HEAD 0x18000			/* u16 per MEM1 page: its newest version */
#define T_TRACK 0x1B000			/* u16[]: the pages saved, those below 2 MB first */
#define T_WORK 0x1E000			/* u16[]: a capture's or load's pages */
#define T_REC_STORE 0x21000		/* versions: u16 copy, page, older version, next */
#define T_REC_PAGE 0x23000
#define T_REC_PREV 0x25000
#define T_REC_NEXT 0x27000
#define T_FREE 0x29000			/* u16[]: the free copies */
#define LIST_PAIRS 64
#define T_ALARMS 0x2B200		/* per capture slot: the alarms queued then (count, up to 15) */
#define T_HIST 0x2C000			/* u32 per 64 KB of MEM1: its pages captured (the report) */
#define T_FENCE 0x2F000			/* 32 bytes the DMA fence stores into (dma_wait) */
#define T_EVENTS 0x2F100		/* captures and loads, latest last (note_event): the kernel reads them */
#define T_SLOW_FRAME (T_EVENTS + 4 + 96 * 4)	/* tests: test_slow_frame_ms, from the kernel */
#define EVENTS 96
#define T_REPLAY_HEAD 0x2F300		/* the replay ring (replay_queue): bytes queued, by the engine */
#define T_REPLAY_TAIL 0x2F320		/* and bytes taken, by the kernel */
#define T_ANSWER 0x2F340		/* the last 0xB0's answer (answer_ready): its frame, and at + 0x20 the answer */
#define T_GAME_FRAMES 0x2F500		/* the frames this game has started: the kernel reads it */
#define T_INPUT_WAIT (T_GAME_FRAMES + 4)	/* the longest a poll waits for a late input, in microseconds (input_wait): the kernel sets it */
#define FIELD_US 16683	/* a refresh, in microseconds */
#ifdef RB_HOST	/* (the PC's test has the scratch once, not also uncached) */
#define SCRATCH_WORD(off) ((volatile u32 *)M2(RB_SCRATCH + (off)))
#else
#define SCRATCH_WORD(off) ((volatile u32 *)(0xD0000000u | (RB_SCRATCH + (off))))
#endif
#define T_REMOTE_CHANGED (T_GAME_FRAMES + 16)	/* the newest frame whose input is not the one before it: the kernel keeps it */
#define T_POLL_EARLY (T_GAME_FRAMES + 20)	/* how long before the retrace the pad is read, in microseconds (poll_early_on), and << 16 how much more in deep rollbacks: the kernel sets it */
#define T_REMOTE_HEAD (T_GAME_FRAMES + 8)	/* the opponent's newest input frame: the kernel keeps it */
#define T_BG_HOT 0x2EB00		/* bit per MEM1 page: it faulted this game, so a capture copies it at once */
#define T_PROF 0x2B700			/* RB_TRACE: 256 x (address >> 8, samples): where rollback loops spend their time */
#define T_POLL_HW (T_GAME_FRAMES + 104)	/* uncached: Hollywood's timer at the last pad read's interrupt, for the kernel's loop (so_exi_poll_due) */
#define T_BG_SAVES (T_GAME_FRAMES + 56)	/* 1: captures copy in the background (bg_*): the kernel sets it (rollback_bg_saves) */
#define T_BG_RING 0x2EA00		/* uncached: n, then 16 x (what << 24 | arg, timebase): the last moments, for a hang */
#define T_BG_DIAG (T_GAME_FRAMES + 64)	/* uncached, for the kernel's hang report: faults, dar, srr0, pte, same, spurious, ticks, state, flushed */
#define T_LC_ON (T_GAME_FRAMES + 52)	/* 1: the locked cache is on (lc_begin): from a game's first use to its end */
#define T_DIRECT (T_GAME_FRAMES + 28)	/* (its low byte) frames on the screen a refresh sooner (direct_track): 1 from the countdown if the last game's frames fit, 3 the same and only against an opponent already played, 2 always (tests); bit 8 a test's once (not read), bit 9 the first game against this opponent: the kernel sets it (rollback_direct) */
#define T_VI_NUDGE (T_GAME_FRAMES + 24)	/* 1: refresh a little faster (vi_nudge), 3: read the pad earlier first (<< 16: how early at most, us): the kernel sets it */
/* The OS's own: its dispatcher goes by these, whatever the hardware's mask says, so a retrace
 * pending when another interrupt comes would run otherwise */
#define OS_MASK_INTERRUPTS ((u32 (*)(u32))0x8034772Cu)
#define OS_UNMASK_INTERRUPTS ((u32 (*)(u32))0x803477B4u)
#define OS_INTERRUPTMASK_PI_VI 0x80u
#define ALARM_QUEUE 0x804D7358		/* OSAlarm.c's AlarmQueue: head, tail */
#define ALARMS_MAX 15
#define T_REPLAY 0x30000		/* the ring: u32 length (or REPLAY_WRAP: go on at the start), the data */
#define REPLAY_SIZE 0x10000u
#define REPLAY_WRAP 0xFFFFFFFFu
#ifdef RB_TRACE
#define T_TRACE 0x40000u		/* RB_TRACE's record */
#endif
#ifdef RB_VERIFY_DEEP
#define T_SUMS 0x40000u		/* u32 per copy: its page's sum when the copy was made (in T_TRACE's place) */
#endif
#define STORES_AT 0x50000		/* 4 KB copies to the end of the 16 MB */
#define STORES ((0x1000000 - STORES_AT) >> 12)
#define RECS 4096
#define NONE 0xFFFF
#define CAPS 8

#define PAGES 6144			/* 24 MB of MEM1 */
#define FIRST 512			/* the pages below 2 MB stay on DBAT0 */
#define VSID 0xABCu
#define BAT_2MB_U 0x8000003Fu		/* 2 MB at 0x80000000 */
#define BAT_LC_U 0xE0000003u		/* the locked cache at 0xE0000000 */
#define BAT_LC_L 0xE0000002u
#define LC 0xE0000000u

#ifdef RB_HOST	/* tools/rollback/test: this code on a PC, its memory simulated */
void host_exi(void *buf, int len, int mode);
int host_dvd_status(void);
#define FN_EXITransferBuffer host_exi
#define DVDGetDriveStatus host_dvd_status
#else
#define FN_EXITransferBuffer ((void (*)(void *, int, int))0x800055F0)
#define DVDGetDriveStatus ((int (*)(void))0x80339B4C)
#endif
#define EXI_BRANCH 0x800055F0u
#define CMD_ROLLBACK 0xD9
#define CMD_EARLY_PAD 0xE0
#define INPUT_BRANCH 0x80376A28u	/* HSD_PadRenewRawStatus: Slippi's TriggerSendInput */
#define FN_LOAD_SAVESTATE 0x8000560Cu	/* Slippi's table of its functions */
#define EARLY_HOOK 0x80019618u		/* fn_800195FC, where Slippi's skip of a renewal lands */
#define EARLY_HOOK_INSN 0x8001000Cu	/* lwz r0, 0xC(r1) */
#define PADRead ((void (*)(void *))0x8034DA00)

/* glue.s's data: Slippi's regions (4, the heap's bounds filled in at run
 * time), then the parts left out (excludes.py), sorted, ending 0, 0 */
typedef struct {
	u32 list[2 * LIST_PAIRS];
} Data;

typedef struct {
	s32 frame;
	u16 first, count;	/* its versions (T_REC_NEXT) */
} Cap;

typedef struct {
	u32 installed;		/* 0x00 */
	u32 orig_exi;		/* 0x04: FN_EXITransferBuffer's target (glue.s reads it) */
	u32 end_tb, ends;	/* 0x08, 0x0C: the timebase at the last frame's end, and how many (frame_end.s) */
	u32 load_waits, load_wait_max;	/* 0x10, 0x14: file loads waited for, the longest (sync_loads.s) */
	/* 0x18-0x6F, RB_VERIFY: the GPU's copies to textures lately, start and
	 * end (gx_copy_dst.s, gx_copy_tex.s): bytes it writes with no C bit */
	u32 copy_size, copies_next;
	u32 copies[10][2];
	/* 0x70-0xA7 (glue.s, pad_shim): a pad already sent to the opponent
	 * (rb_early_pad), PADStatus[4]; TriggerSendInput, where pad_shim goes on to */
	u32 early_valid;
	u32 orig_input;
	u8 early_pad[48];
	/* RB_VERIFY (verify_load): the first restored byte that came back wrong;
	 * pages changed without a C bit, counted, and as runs of pages [lo, hi)
	 * with each run's first differing byte (the runs logged so far: told) */
	u32 verify_bad, verify_at, verify_have, verify_want, verify_reported;
	u32 probing, probed, unmarked, unmarked_told;
	u16 unmarked_lo[8], unmarked_hi[8];
	u32 unmarked_at[8];
	u32 start_tb, last_ends, reported;
	u32 mmu, active;
	u32 bat0u, bat0l, bat2u, bat2l, bat3u, bat3l, sdr1, sr8;
	u32 was_in_game, failed;
	/* frames */
	u32 n, cpu_sum, cpu_max, draw_sum, draw_max;
	u32 hist[8];	/* frames by busy time: < 8, 10, 12, 14, 15, 16, 16.7 ms, more */
	/* the savestates */
	u32 tracked, low;	/* pages saved; of them, below 2 MB (always copied) */
	u32 free_stores, free_recs, rec_free;
	u32 cap_first, caps;
	Cap cap[CAPS];
	u32 captures, cap_pages, cap_pages_max, cap_ticks, cap_ticks_max;
	u32 loads, load_pages, load_pages_max, load_ticks, load_ticks_max, load_depth_max;
	u32 misses, errors, base_ticks;
	u32 disc_busy;	/* savestates with a disc read out */
	u32 ld_scan, ld_whole, ld_part, ld_parts, cap_scan;	/* where a load's (and a capture's) time goes, summed */
	u32 orig_load;		/* FN_LoadSavestate */
	u32 lost;		/* loads of a frame Slippi has no savestate of */
	u32 saved_late;		/* savestates only so_gct_fix_capture_check takes that were then needed */
	u32 game_frames;	/* T_GAME_FRAMES */
	/* a refresh's work, up to its render built, by the engine loops in it: 1, 2, 3, 4,
	 * more. Count, microseconds, and how many took over a refresh. */
	u32 pass_start, pass_loops;
	u32 pass_n[5], pass_us[5], pass_over[5];
	/* the time the CPU waits on the EXI, by what for: the 0xB0 sent, its answer, the rest */
	u32 exi_n[3], exi_us[3];
	u32 replay_head;	/* bytes ever queued in the replay ring: it runs on from game to game */
	s32 poll_frame;		/* the 0xB0 whose answer comes next (answer_ready) */
	u32 busy;		/* from a frame's start until its render is built */
	s32 early_frame;	/* the last pad rb_early_pad sent */
	u32 input_waits, input_arrived, input_wait_ticks;
	u32 wait_misses;	/* waits in a row that did not get the input */
	s32 head_seen;		/* the opponent's newest input at the last poll (input_wait) */
	u32 orig_vi;		/* the OS's VI interrupt handler (rb_vi_interrupt) */
	u32 orig_pe;		/* and GX's for a finished draw (video_through) */
	u32 video_through;	/* how many: retraces and draws let through a load or capture << 16 | frames ending early */
	u32 vi_saved;		/* vi_nudge: the blanking while a frame is ending early */
	u32 poll_cb;		/* HSD's post-retrace callback, the pad's renewal, while it runs from there instead */
	u32 si_rate;		/* the SI's sampling rate, while it is changed for that */
	u32 last_retrace_tb;	/* when the last retrace was handled */
	u32 in_poll;		/* in rb_vi_interrupt's poll */
	u32 poll_tb;		/* when the last poll came */
	u32 poll_us, poll_next;	/* how long before the retrace it comes, and is to (poll_early_track) */
	u32 poll_need;
	u16 poll_calm, poll_loaded;	/* lately: the most any frame needed; frames calm; a load */
	u32 poll_prev_tb;	/* when the poll before it came */
	u32 pass_poll_tb;	/* when the poll of the frame being made, or last made, came */
	u32 built_poll_tb;	/* when the poll of the last frame built came */
	u32 vb_cleared;		/* renders kept from the visual buffer fix's skip (vb_keep_only_if_behind) */
	/* direct_track: the retrace count at the polls (as poll_tb, poll_prev_tb, pass_poll_tb), and its state */
	u16 poll_rc, poll_prev_rc, pass_poll_rc;	/* (its low 16 bits: differences of a few) */
	u8 direct_skip;		/* a frame not to draw, asked of the visual buffer fix and not taken yet */
	u8 direct_is;		/* 1: frames go up at the retrace after their poll's (since the last skip); 4: the last frame drawn was level; 8: a run level ended in play: no skip (more) in this game */
	u16 direct_hold;	/* frames drawn to stay a refresh behind for, before trying again */
	u16 deep_game;		/* this game's passes too long for a frame level (DIRECT_FIT_US) */
	u16 direct_run;		/* frames drawn since the last skip */
	u8 direct_skips, direct_overruns;	/* (the report's) */
	u16 deep_carry;		/* the opponent's games before this one, by such passes (direct_end): on from game to game */
	u16 direct_behind;	/* (the report's) */
	u16 sync_lead;		/* vi_nudge: the pad read no later than this before the retrace, to stay level */
	u16 vi_burst;		/* frames ended early since the kernel last asked (the trace's) */
	u32 vt_count;		/* video_through's let-throughs, wrapping (RB_VERIFY: a check across one is not made) */
	u32 scratch_ok;		/* 1: the kernel gave the MEM2 scratch to the engine this boot (install's 0xD9, state 3);
				 * 0: it is the disc cache's or a big memory card's, never to be touched */
} Engine;
typedef char engine_fits_glue[sizeof(Engine) <= 0x320 ? 1 : -1];	/* glue.s: engine, .space 0x320 */

static void bg_finish(void);
#ifndef RB_HOST
static void bg_mark(u32 what, u32 arg);
#else
#define bg_mark(w, a) ((void)0)
#endif
static void bg_feed(void);
static void bg_start(Engine *e, u32 n);
static int bg_ready(void);
typedef char engine_early_fields[__builtin_offsetof(Engine, early_pad) == 0x78 ? 1 : -1];	/* glue.s, pad_shim */

u64 timebase(void);
u32 irq_off(void);
void irq_restore(u32 msr);
void flush_code(u32 address, u32 bytes);
void store_lines(u32 address, u32 lines);
void kill_lines(u32 address, u32 lines);
void flush_lines(u32 address, u32 lines);
void zero_lines(u32 address, u32 lines);
u32 rd_bat(int i);
u32 rd_hid4(void);
u32 rd_xbat(int i);
void set_dbat0(u32 upper, u32 lower);
void set_dbat2(u32 upper, u32 lower);
void set_dbat3(u32 upper, u32 lower);
u32 rd_sdr1(void);
void wr_sdr1(u32 v);
u32 rd_sr(u32 ea);
void wr_sr(u32 ea, u32 v);
void tlb_flush(void);
void tlb_kill(const u16 *pages, u32 n);
void set_dec(u32 ticks);
u32 pad_shim_at(void);
u32 early_shim_at(void);
u32 vi_shim_at(void);
u32 dsi_stub_at(void);
u32 dsi_tramp_at(void);
u32 dec_shim_at(void);
#define DSI_STUB_TRAMP 40	/* dsi_stub's lis/ori of dsi_tramp's address (glue.s) */
u32 load_shim_at(void);
void lc_on(void);
void lc_off(void);
u32 lc_queue(void);
void lc_load(u32 lc, u32 phys, u32 blocks);
void lc_store(u32 lc, u32 phys, u32 blocks);

static u32 rd32(u32 a) { return *(volatile u32 *)a; }
static u8 rd8(u32 a) { return *(volatile u8 *)a; }
static u16 *t16(u32 off) { return (u16 *)M2(RB_SCRATCH + off); }
static u8 *t8(u32 off) { return (u8 *)M2(RB_SCRATCH + off); }
static u32 *t32(u32 off) { return (u32 *)M2(RB_SCRATCH + off); }
static u32 store_phys(u32 s) { return RB_SCRATCH + STORES_AT + (s << 12); }
static u32 be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
#ifdef RB_TRACE
static void tr_event(u32 kind, u32 a, u32 b);
#define TR(kind, a, b) tr_event(kind, a, b)
#define tr_vi_state(w) ((void)0)	/* (tr_vi_state: the ring fills, and a game's later events are lost) */
static void tr_stamp(u32 kind);
#define TS(kind) tr_stamp(kind)
#else
#define TR(kind, a, b) ((void)0)
#define tr_vi_state(w) ((void)0)
#define TS(kind) ((void)0)
#endif

/* ---- the report ---------------------------------------------------------- */

typedef struct {
	char *p, *end;
} Out;

static void put_c(Out *o, char c)
{
	if (o->p < o->end)
		*o->p++ = c;
}

/* Text without string constants (they would be data): up to 4 characters
 * packed in an immediate, C4('c','p','u','=') */
#define C4(a, b, c, d) (((u32)(a) << 24) | ((u32)(b) << 16) | ((u32)(c) << 8) | (u32)(d))

static void put4(Out *o, u32 w)
{
	int i;
	for (i = 24; i >= 0; i -= 8)
		if ((w >> i) & 0xFF)
			put_c(o, (char)((w >> i) & 0xFF));
}

static void put_u(Out *o, u32 v)
{
	char d[10];
	int n = 0;
	do {
		d[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	while (n)
		put_c(o, d[--n]);
}

static void put_x(Out *o, u32 v)
{
	int i;
	for (i = 28; i >= 0; i -= 4) {
		u32 d = (v >> i) & 0xF;
		put_c(o, (char)(d < 10 ? '0' + d : 'a' + d - 10));
	}
}

/* " name=avg/max" */
static void put_stat(Out *o, u32 name, u32 sum, u32 n, u32 max)
{
	put4(o, name);
	put_c(o, '=');
	put_u(o, n ? sum / n : 0);
	put_c(o, '/');
	put_u(o, max);
}

/* 0xDA kind 4: two text fields, 48 and 64 bytes */
static void send(const char *a, const char *b)
{
	u8 raw[0x80 + 32];
	u8 *buf = (u8 *)(((__UINTPTR_TYPE__)raw + 31) & ~(__UINTPTR_TYPE__)31);
	int i;
	for (i = 0; i < 0x80; i++)
		buf[i] = 0;
	buf[0] = 0xDA;
	buf[1] = 4;
	for (i = 0; i < 47 && a[i]; i++)
		buf[0x10 + i] = (u8)a[i];
	for (i = 0; i < 63 && b[i]; i++)
		buf[0x40 + i] = (u8)b[i];
	FN_EXITransferBuffer(buf, 0x80, 1);
}

/* 0xD9 [version, state]: 0 asks whether to roll back this game (the
 * answer), 1 says ready, 2 failed */
static int tell_kernel(int state)
{
	u8 raw[64];
	u8 *buf = (u8 *)(((__UINTPTR_TYPE__)raw + 31) & ~(__UINTPTR_TYPE__)31);
	buf[0] = CMD_ROLLBACK;
	buf[1] = RB_VERSION;
	buf[2] = (u8)state;
	FN_EXITransferBuffer(buf, 3, 1);
	buf[0] = 0;
	FN_EXITransferBuffer(buf, 1, 0);
	return buf[0] == 1;
}

/* ---- which pages a savestate covers ---------------------------------------- */

static u32 overlap(u32 a0, u32 a1, u32 b0, u32 b1)
{
	u32 lo = a0 > b0 ? a0 : b0, hi = a1 < b1 ? a1 : b1;
	return hi > lo ? hi - lo : 0;
}

/* The savestate's bytes in [p0, p1): the regions less the excludes */
static u32 covered(const u32 *list, u32 p0, u32 p1)
{
	u32 n = 0, r;
	for (r = 0; r < 4; r++) {
		u32 r0 = list[2 * r], r1 = list[2 * r + 1], o = overlap(p0, p1, r0, r1);
		const u32 *x;
		if (!o)
			continue;
		n += o;
		if (r0 < p0)
			r0 = p0;
		if (r1 > p1)
			r1 = p1;
		for (x = list + 8; x[0]; x += 2)
			n -= overlap(r0, r1, x[0], x[1]);
	}
	return n;
}

/* ---- the page table ----------------------------------------------------------- */

/* Map MEM1 page p (0x80000000 + p * 4 KB) in the page table: its primary
 * group, else its secondary one. Returns 0 when both are full. */
static int map_page(u32 p, u32 lower)
{
	u32 ea = 0x80000000u + (p << 12);
	u32 hash = (VSID ^ ((ea >> 12) & 0xFFFF)) & 0x7FFFF;
	u32 api = (ea >> 22) & 0x3F, h, s;
	for (h = 0; h < 2; h++) {
		u32 g = (h ? ~hash : hash) & 0x3FF;
		for (s = 0; s < 8; s++) {
			u32 *w = (u32 *)M2(HTAB + g * 64 + s * 8);
			if (w[0] & 0x80000000u)
				continue;
			w[1] = (p << 12) | lower;
			w[0] = 0x80000000u | (VSID << 7) | (h << 6) | api;
			t16(T_PTE_OF)[p] = (u16)(g * 8 + s);
			return 1;
		}
	}
	return 0;
}

/* Slippi's region C (0x8065C000-0x8071B000) holds the GPU's command FIFO's
 * end, the OS heaps' descriptors (OSInitAlloc), a small heap, and the audio
 * heap (HSD_AudioMalloc: the sound engine's buffers, HSD DevCom's requests;
 * its handle at 0x804D6018). The FIFO is read by the GPU meanwhile, and the
 * sound engine (whose other state Slippi leaves out already) goes on with the
 * DSP and the disc: rewinding half of it hangs the game at a scene change. So
 * the FIFO, the audio heap and its descriptor are left out. The audio heap's
 * bounds are its cells' (OSAlloc's cells tile a heap: prev, next, size),
 * checked against its size. Appends them to the list at `x` (after the static
 * excludes, in order) and ends it; 0 when the layout is not this one. */
static int add_audio_excludes(u32 *x)
{
	u32 heaps = rd32(0x804D7360), n = rd32(0x804D7364), audio = rd32(0x804D6018);
	u32 fifo = 0x80000000u | (rd32(0xCC00300C) & 0x03FFFFE0u), desc, lo = ~0u, hi = 0, l, count = 0;
	x[0] = x[1] = 0;
	if (heaps < 0x8065C000u || heaps >= 0x8071B000u || n > 16 || audio >= n || fifo >= heaps)
		return 0;
	desc = heaps + audio * 12;
	for (l = 1; l <= 2; l++) {	/* the free cells, the allocated ones */
		u32 cell = rd32(desc + l * 4);
		while (cell && count++ < 4096) {
			if (cell < 0x80000000u || cell >= 0x81800000u)
				return 0;
			if (cell < lo)
				lo = cell;
			if (cell + rd32(cell + 8) > hi)
				hi = cell + rd32(cell + 8);
			cell = rd32(cell + 4);
		}
	}
	if (hi - lo != rd32(desc) || lo < heaps + n * 12 || hi > 0x8071B000u)
		return 0;
	*x++ = fifo;	/* the FIFO's end, up to the descriptors */
	*x++ = heaps;
	*x++ = desc;	/* the audio heap's descriptor */
	*x++ = desc + 12;
	*x++ = lo;	/* the audio heap */
	*x++ = hi;
	x[0] = x[1] = 0;
	return 1;
}

/* MEM2 at 0x90000000, cached, by one of Broadway's extra DBATs (HID4's SBE):
 * the scratch is reached there; mapping it again would be two BATs for one
 * address, which the CPU does not define. */
static int mem2_mapped(void)
{
	int i;
	if (!(rd_hid4() & 0x02000000u))
		return 0;
	for (i = 0; i < 8; i += 2) {
		u32 u = rd_xbat(i), l = rd_xbat(i + 1), size = ((u >> 2 & 0x7FF) + 1) << 17;
		if ((u & 2) && (u & 0xFFFE0000u) == 0x90000000u && (l & 0xFFFE0000u) == 0x10000000u &&
		    !(l & 0x28) && RB_SCRATCH + 0x1000000 <= 0x10000000u + size)	/* cached (no I, no W), reaching the scratch */
			return 1;
	}
	return 0;
}

/* The page table on: build it (the C bits clear), then hand MEM1 above 2 MB
 * from the BATs to it. Also classifies every page (T_CLASS) and keeps the
 * region list with the heap's bounds (T_LIST). Interrupts off throughout. */
static void mmu_on(Engine *e, Data *d)
{
	u32 msr = irq_off(), p, lower, *list;
	e->bat0u = rd_bat(0);
	e->bat0l = rd_bat(1);
	e->bat2u = rd_bat(4);
	e->bat2l = rd_bat(5);
	e->bat3u = rd_bat(6);
	e->bat3l = rd_bat(7);
	if (e->bat3u || (e->bat0u & 0xFFFE0000u) != 0x80000000u || !mem2_mapped()) {	/* not the layout we know */
		e->failed++;
		irq_restore(msr);
		return;
	}
	e->sdr1 = rd_sdr1();
	e->sr8 = rd_sr(0x80000000u);

	list = t32(T_LIST);
	for (p = 0; p < 2 * LIST_PAIRS - 2 && (p < 8 || d->list[p] || d->list[p + 1]); p += 2) {
		list[p] = d->list[p];
		list[p + 1] = d->list[p + 1];
	}
	list[p] = list[p + 1] = 0;
	list[6] = rd32(0x804D76B8);	/* the heap */
	list[7] = rd32(0x804D76BC);
	if (!add_audio_excludes(list + p)) {
		e->failed++;
		irq_restore(msr);
		return;
	}
	zero_lines(M2(HTAB), 0x10000 / 32);
	lower = (e->bat0l & 0x78) | 2;	/* the game's WIMG, read/write */
	for (p = 0; p < PAGES; p++) {
		u32 c = covered(list, 0x80000000u + (p << 12), 0x80000000u + ((p + 1) << 12));
		t8(T_CLASS)[p] = (u8)(c == 0 ? 0 : c == 4096 ? 1 : 2);
		if (p >= FIRST && !map_page(p, lower)) {
			e->failed++;
			irq_restore(msr);
			return;
		}
	}
	flush_lines(M2(HTAB), 0x10000 / 32);

	wr_sdr1(HTAB);
	wr_sr(0x80000000u, VSID);
	tlb_flush();
	set_dbat2(0, 0);	/* its 0x81000000-0x81800000 to the page table */
	set_dbat0(BAT_2MB_U, e->bat0l);
	tlb_flush();
	e->mmu = 1;
	irq_restore(msr);
}

static void mmu_off(Engine *e)
{
	u32 msr = irq_off();
	bg_finish();
	set_dbat0(e->bat0u, e->bat0l);
	set_dbat2(e->bat2u, e->bat2l);
	set_dbat3(e->bat3u, e->bat3l);
	tlb_flush();
	wr_sr(0x80000000u, e->sr8);
	wr_sdr1(e->sdr1);
	e->mmu = 0;
	e->active = 0;
	irq_restore(msr);
}

/* ---- the copies ------------------------------------------------------------- */

/* One 4 KB copy through locked-cache buffer i & 3: physical addresses.
 * HID2's queue length need not count the transfer in progress, so a buffer
 * is reused only with at most 4 operations queued: then its last store is
 * done even with one more running (the 3 newer pages hold the other 3). */
static void dma_page(u32 i, u32 dst, u32 src)
{
	u32 lc = LC + (i & 3) * 4096;
	while (lc_queue() > 4)
		;
	lc_load(lc, src, 128);
	lc_store(lc, dst, 128);
}

/* Every transfer queued so far done. An empty queue may still have its last
 * one running, so a 32-byte store goes in behind it, and once the engine has
 * taken that (the queue empty again), all before it are done: it runs them
 * in order. */
#ifndef RB_HOST
static u32 rd_hid2(void)
{
	u32 v;
	__asm__ volatile("mfspr %0, 920" : "=r"(v));
	return v;
}
/* (diagnostics: a wait that never ends notes HID2, its DMA queue and error bits, for the
 * kernel's hang report: T_BG_DIAG + 36) */
static void dma_spin(void)
{
	u32 n = 0;
	while (lc_queue())
		if (++n == 5000000)
			*(volatile u32 *)(0xD0000000u | (RB_SCRATCH + T_BG_DIAG + 36)) = rd_hid2();
}
#else
static void dma_spin(void)
{
	while (lc_queue())
		;
}
#endif

static void dma_wait(void)
{
	dma_spin();
	lc_store(LC, RB_SCRATCH + T_FENCE, 1);
	dma_spin();
}

/* The locked cache stays on from a game's first capture to its end: half the data cache
 * locked, the game was no slower (1-loop passes' start to copy: 5.9-6.6 ms held, 5.3-7.4 not,
 * the same stages), and a capture or load saves turning it on and off (dcbst of 32 KB and
 * dcbz_l of 16 KB each time); background saves (bg_*) need it on while the game runs. */
static void lc_begin(void)
{
	u32 *on = t32(T_LC_ON);
	if (*on)
		return;
	set_dbat3(BAT_LC_U, BAT_LC_L);
	lc_on();
	*on = 1;
}

static void lc_off_now(void)
{
	u32 *on = t32(T_LC_ON);
	if (!*on)
		return;
	dma_wait();
	lc_off();
	set_dbat3(0, 0);
	*on = 0;
}

static void lc_end(void)
{
	dma_wait();
}

#ifdef RB_HOST
#define BG_ENABLED() (t32(T_BG_SAVES)[0])
#else
#define BG_ENABLED() (*SCRATCH_WORD(T_BG_SAVES))
#endif

/* ---- the savestates ----------------------------------------------------------- */

static void copy_range(u32 lo, u32 hi, u32 src);
static void saved_ranges(u32 p, const u32 *keep, u32 src, Engine *e);
#ifdef RB_VERIFY
static void verify_range(u32 lo, u32 hi, u32 src, Engine *e);
#endif

static void free_store(Engine *e, u32 s)
{
	t16(T_FREE)[e->free_stores++] = (u16)s;
}

static void free_rec(Engine *e, u32 r)
{
	t16(T_REC_NEXT)[r] = (u16)e->rec_free;
	e->rec_free = r;
	e->free_recs++;
}

/* ---- the OS alarm queue ---------------------------------------------------------- */

/* The queue (OSAlarm: handler, tag, fire (s64), prev, next, ...) is left out
 * of savestates (glue.s): it links the I/O drivers' alarms, which are not
 * rewound, and the game's (the controller sampling alarm, lb_804329F0), which
 * are. So each capture notes the alarms queued, and each load relinks the
 * queue from the alarms armed after it: the ones queued now and the ones
 * queued at the frame loaded, each as it is now in memory. */
static int alarm_ok(u32 a)
{
	return a >= 0x80000000u && a < 0x81800000u && !(a & 3);
}

#if defined(RB_WALK) && !defined(RB_HOST)
/* ---- RB_WALK: the game's free lists ------------------------------------------------
 *
 * Tests (build_online_boot.py --rollback-walk): the engine as a player has it, and after each load,
 * before each capture and at each frame's end (the game's own thread, between frames) it walks the
 * free lists of the game's three allocators: HSD's memory pieces (class.c: memory_list's entries,
 * each a list as long as its nb_free), HSD's object pools (objalloc.c: alloc_datas, each a list as
 * long as its `free`) and the scene heap's free cells (OSAlloc.c: prev, next, size). The crash after
 * a lag spike's deepest rollback (2026-11-10) was each time an allocation that found data where a
 * list's pointer belongs; this says at which moment a list first is not what it should be:
 *   rb WALK <what: 1 a piece list's pointer, 2 its length, 3 a pool's pointer, 4 its length, 5 a heap
 *            cell> <when: 1 after a load, 2 before a capture, 3 a frame's end> <that frame, or the
 *            game's tick> <the game's tick> <walks so far>
 *   | <the entry, pool or heap> <the pointer that is none, or the length> <the node before it, or the
 *     count> <that node's first three words>
 * and the kernel adds its record of the last polls, captures and loads (so_exi.c). Then, of the page
 * that node is in:
 *   rb WALP <the page> <its class: 1 saved whole, 2 in part> <its C bit> <where in the last load's or
 *            capture's pages, 65535: not among them> <1: a capture's copy of it is kept, 0: only the base's>
 *   | <the node's first word now> <in the page's newest copy> <in the base's> <the last load's frame
 *     << 1, or the last capture's << 1 | 1> <its pages>
 * (the newest copy's word a pointer and the page's not: the load did not put the page back, or not
 * all of it; both alike and no pointer: the copy was made of a page already wrong.) Once a game; at
 * its end, "rb WALK 0 0 <walks> <the most nodes in one>". */
#define WALK_MEMORY_LIST 0x804D7700u	/* memory_list, nb_memory_list */
#define WALK_ALLOC_DATAS 0x804D76E0u	/* alloc_datas */
#define WALK_CURRENT_HEAP 0x804D5E00u	/* current_heap (initialize.c) */
#define WALK_HEAP_ARRAY 0x804D7360u	/* HeapArray: size, free cells, allocated cells */
#define WALK_MOST 0x4000u

#ifdef RB_DIRECT_LOG
/* With the latency probe (build_online_boot.py --latency: RB_DIRECT_LOG) the region has room for this
 * much. HSD's memory pieces only, the lists all three crashes were in; and a list found wrong ends the
 * game there, in a call to 0x57414C48 ("WALH") that the crash report shows (srr0 57414c48) with r3 the
 * entry, r4 the pointer that is none, r5 the node before it, r6 when (1 after a load, 3 a frame's
 * end: not before a capture, for room), r7 that load's frame, or 0; or, the list not
 * as long as its count (r6 | 16), r4 its length and r5 the count. The kernel's record of the last
 * polls, captures and loads follows a crash report as it is. */
#define WALK_TRAP ((void (*)(u32, u32, u32, u32, u32))0x57414C48u)
static void walk_check(Engine *e, u32 when, u32 frame)
{
	u32 list = rd32(WALK_MEMORY_LIST), nb = rd32(WALK_MEMORY_LIST + 4), i;
	if (!e->active)
		return;
	for (i = 0; i < nb; i++) {	/* (interrupts as they are: off after a load; their handlers allocate nothing) */
		u32 entry = rd32(list + i * 4), a, n = 0, prev = 0;
		if (!entry)
			continue;
		for (a = rd32(entry + 0xC); a; prev = a, a = rd32(a), n++)
			if (!alarm_ok(a) || n > WALK_MOST)
				WALK_TRAP(entry, a, prev, when, frame);
		if (n != rd32(entry + 8))
			WALK_TRAP(entry, n, rd32(entry + 8), when | 16, frame);
	}
}
#else
/* A list from `a`, each node's first word its link, `want` nodes long. 0; or 1, w[1] the pointer that
 * is none and w[2] the node before it; or 2, w[1] its length and w[2] `want`. */
static u32 walk_list(u32 a, u32 want, u32 *w, u32 *nodes)
{
	u32 n = 0, prev = 0;
	for (; a; prev = a, a = rd32(a))
		if (!alarm_ok(a) || ++n > WALK_MOST) {
			w[1] = a;
			w[2] = prev;
			return 1;
		}
	*nodes += n;
	if (n != want) {
		w[1] = n;
		w[2] = want;
		return 2;
	}
	return 0;
}

static u32 walk(u32 *w, u32 *nodes)
{
	u32 list = rd32(WALK_MEMORY_LIST), nb = rd32(WALK_MEMORY_LIST + 4), d, i, k;
	for (i = 0; alarm_ok(list) && i < nb && i < 1024; i++) {
		u32 entry = rd32(list + i * 4);
		if (!entry)
			continue;
		w[0] = entry;
		w[1] = list;
		w[2] = i;
		k = alarm_ok(entry) ? walk_list(rd32(entry + 0xC), rd32(entry + 8), w, nodes) : 1;
		if (k)
			return k;
	}
	for (d = rd32(WALK_ALLOC_DATAS), i = 0; d && i < 64; d = rd32(d + 0x28), i++) {
		w[0] = d;
		w[1] = d;
		w[2] = i;
		k = alarm_ok(d) ? walk_list(rd32(d + 4), rd32(d + 0xC), w, nodes) : 1;
		if (k)
			return 2 + k;
	}
	{
		u32 heap = rd32(WALK_HEAP_ARRAY) + rd32(WALK_CURRENT_HEAP) * 12, cell, prev = 0, n = 0;
		w[0] = heap;
		for (cell = alarm_ok(heap) ? rd32(heap + 4) : 0; cell; prev = cell, cell = rd32(cell + 4))
			if (!alarm_ok(cell) || rd32(cell) != prev || ++n > WALK_MOST) {
				w[1] = cell;
				w[2] = prev;
				return 5;
			}
		*nodes += n;
	}
	return 0;
}

static void walk_say(u32 tag, u32 what, u32 when, u32 frame, u32 tick, u32 walks, const u32 *w)
{
	char a[48], b[64];
	Out o;
	u32 v[5], k;
	v[0] = what;
	v[1] = when;
	v[2] = frame;
	v[3] = tick;
	v[4] = walks;
	o.p = a;
	o.end = a + 47;
	put4(&o, C4('r', 'b', ' ', 'W'));
	put4(&o, C4('A', 'L', tag, 0));
	for (k = 0; k < 5; k++) {
		put_c(&o, ' ');
		put_u(&o, v[k]);
	}
	*o.p = 0;
	o.p = b;
	o.end = b + 63;
	for (k = 0; w && k < 6; k++) {
		/* (the node before the pointer that is none: what is in it now) */
		u32 word = k < 3 || tag == 'P' ? w[k] : (what & 1) && alarm_ok(w[2]) ? rd32(w[2] + (k - 3) * 4) : 0;
		put_x(&o, word);
		put_c(&o, ' ');
	}
	*o.p = 0;
	send(a, b);
}

/* The page of a node found wrong (walk's comment: "rb WALP") */
static void walk_page(Engine *e, u32 at)
{
	u32 p = (at - 0x80000000u) >> 12, r = t16(T_HEAD)[p], in = 0xFFFF, i, c, w[6];
	for (i = 0; i < e->verify_want; i++)
		if (t16(T_WORK)[i] == p) {
			in = i;
			break;
		}
	w[0] = rd32(at);
	c = M2(store_phys(r != NONE ? t16(T_REC_STORE)[r] : t16(T_BASE)[p])) + (at & 0xFFCu);
	flush_lines(c, 1);	/* (written by DMA: the cache may hold an older copy) */
	w[1] = rd32(c);
	c = M2(store_phys(t16(T_BASE)[p])) + (at & 0xFFCu);
	flush_lines(c, 1);
	w[2] = rd32(c);
	w[3] = e->verify_at;
	w[4] = e->verify_want;
	w[5] = 0;
	walk_say('P', p, t8(T_CLASS)[p], p >= FIRST ? (rd32(M2(HTAB + t16(T_PTE_OF)[p] * 8 + 4)) >> 7) & 1 : 2, in, r != NONE, w);
}

/* (verify_bad: said this game; verify_reported: walks; verify_have: the most nodes in one; verify_want
 * and verify_at: the last load's or capture's pages, T_WORK's, and its frame << 1, a capture's | 1.
 * The fields are RB_VERIFY's, which this build is not.) */
static void walk_check(Engine *e, u32 when, u32 frame)
{
	u32 w[3], nodes = 0, what, msr;
	if (!e->active || e->verify_bad)
		return;
	msr = irq_off();
	what = walk(w, &nodes);
	irq_restore(msr);
	e->verify_reported++;
	if (nodes > e->verify_have)
		e->verify_have = nodes;
	if (what) {
		e->verify_bad = 1;
		walk_say('K', what, when, frame, e->game_frames, e->verify_reported, w);
		if ((what & 1) && alarm_ok(w[2]) && t8(T_CLASS)[(w[2] - 0x80000000u) >> 12])
			walk_page(e, w[2]);
	}
}
#endif
#endif

#if defined(RB_HWTEST) && !defined(RB_HOST)
/* ---- RB_HWTEST: is a read given what was written just before it? ----------------------
 *
 * Tests (build_online_boot.py --rollback-hwtest). The SDK ends DCStoreRange and DCFlushRange with
 * `sc`: a sync with HID0's address broadcast on, which completes on the bus (sync_bus). The engine
 * ends store_lines and flush_lines with a plain sync, clears its page table's C bits by uncached
 * writes, and then reads that memory at once by another way: the locked cache's DMA (a capture's
 * copies, the page table's scan) or a cache line's fill (the MMU's table walk). If the memory
 * controller keeps a written word for a moment and gives a read the old one, a capture has an
 * old line of a page, or a page's C bit is cleared in memory and set in the MMU's copy: a page
 * that is not what a load should have put back, which is what the crash after a lag spike's
 * rollback was (2026-11-10).
 *
 * So, HW_N times each, in the scratch (MEM2) and in the foot of the game's stack (MEM1, never
 * reached), a new word each time, and the read the first thing on the bus after the write:
 *   0  an uncached write, then the DMA's read                 (the page table's scan after its clears)
 *   1  the same with sync_bus between                         (what should never fail)
 *   2  a cached write, dcbst and a plain sync, then the DMA   (a capture's copy)
 *   3  the same with sync_bus
 *   4  dcbf, an uncached write, then a cached read            (a table walk after a C bit's clear)
 *   5  the same with sync_bus
 *   6  as 2, the line first pushed out of the first level cache by five writes to lines of its set
 *      (in MEM2, where the test has 64 KB; a game's written lines are mostly in the second level)
 *   7  the same with sync_bus
 * and each DMA is waited for with a store out of the locked cache (dma_wait), a write that pushes
 * on whatever the controller held. At each game's start, interrupts off (about 0.4 s):
 *   rb HW <1: MEM2, 2: MEM1> <HW_N> <the first read of another word than the one written: its case
 *         << 28 | its turn> <the word it got> | <such reads: case 0, 1, .. 7> */
#define T_HW 0x40000u			/* 64 KB of the scratch (T_TRACE's and T_SUMS's place) */
#define HW_STACK_FOOT 0x804DF000u	/* 4 KB: the game's stack is 0x804DEC00-0x804EEC00 and never this deep */
#define HW_N 4096
#define HW_LC (LC + 3 * 4096)

/* The sync the SDK's DCStoreRange and DCFlushRange end with (their `sc`, the OS's SystemCallVector):
 * with HID0's address broadcast on, so that it completes on the bus, where the memory controller
 * sees it. */
static void sync_bus(void)
{
	u32 hid0, with;
	__asm__ volatile("mfspr %0, 1008" : "=r"(hid0));
	with = hid0 | 8;
	__asm__ volatile("mtspr 1008, %0" : : "r"(with));
	__asm__ volatile("isync");
	__asm__ volatile("sync");
	__asm__ volatile("mtspr 1008, %0" : : "r"(hid0));
	__asm__ volatile("isync");
}

static void hw_test_at(u32 cached, u32 uncached, u32 phys, u32 lines, u32 which)
{
	char a[48], b[64];
	Out o;
	u32 bad[8], first = 0, first_got = 0, k, i, j;
	flush_lines(cached, lines);
	for (k = 0; k < 8; k++) {
		bad[k] = 0;
		for (i = 0; i < HW_N; i++) {
			u32 at = (i % lines) * 32, v = (k << 28) | (which << 24) | ((i * 2654435761u >> 8) & 0xFFFFFE) | 1, got;
			volatile u32 *c = (volatile u32 *)(cached + at), *u = (volatile u32 *)(uncached + at);
			if (k < 2) {
				*u = v;
			} else if (k < 4 || k >= 6) {
				*c = v;
				if (k >= 6 && lines >= 2048)
					for (j = 1; j <= 5; j++)	/* (lines 4 KB apart: one set of the first level's) */
						*(volatile u32 *)(cached + ((at + j * 4096) & 0xFFFF) + 16) = v;
				store_lines(cached + at, 1);
			} else {
				flush_lines(cached + at, 1);
				*u = v;
			}
			if (k & 1)
				sync_bus();
			if (k == 4 || k == 5) {
				got = *c;
				lc_store(LC, RB_SCRATCH + T_FENCE, 1);	/* (a write on the bus, as after the others) */
				dma_spin();
			} else {
				lc_load(HW_LC, phys + at, 1);
				dma_wait();
				got = *(volatile u32 *)HW_LC;
			}
			if (got != v && !bad[k]++ && !first) {
				first = k << 28 | i;
				first_got = got;
			}
		}
	}
	o.p = a;
	o.end = a + 47;
	put4(&o, C4('r', 'b', ' ', 'H'));
	put4(&o, C4('W', ' ', '0' + which, ' '));
	put_u(&o, HW_N);
	put_c(&o, ' ');
	put_x(&o, first);
	put_c(&o, ' ');
	put_x(&o, first_got);
	*o.p = 0;
	o.p = b;
	o.end = b + 63;
	for (k = 0; k < 8; k++) {
		put_u(&o, bad[k]);
		put_c(&o, ' ');
	}
	*o.p = 0;
	send(a, b);
}

static void hw_test(void)
{
	u32 msr = irq_off();
	lc_begin();
	hw_test_at(M2(RB_SCRATCH + T_HW), 0xD0000000u | (RB_SCRATCH + T_HW), RB_SCRATCH + T_HW, 0x10000 / 32, 1);
	hw_test_at(HW_STACK_FOOT, HW_STACK_FOOT | 0x40000000u, HW_STACK_FOOT & 0x3FFFFFFFu, 4096 / 32, 2);
	irq_restore(msr);
}
#endif

static u32 queued_alarms(u32 *out)
{
	u32 a = rd32(ALARM_QUEUE), n = 0;
	while (alarm_ok(a) && n < ALARMS_MAX) {
		out[n++] = a;
		a = rd32(a + 0x14);
	}
	return n;
}

static s64 alarm_fire(u32 a)
{
	return (s64)(((u64)rd32(a + 8) << 32) | rd32(a + 12));
}

static void relink_alarms(const u32 *now, u32 n_now, const u32 *then, u32 n_then)
{
	u32 c[2 * ALARMS_MAX], n = 0, i, j;
	for (i = 0; i < n_now + n_then; i++) {
		u32 a = i < n_now ? now[i] : then[i - n_now];
		for (j = 0; j < n; j++)
			if (c[j] == a)
				break;
		if (j < n || !alarm_ok(a) || !rd32(a))	/* a duplicate, or not armed (no handler) */
			continue;
		for (j = n; j > 0 && alarm_fire(c[j - 1]) > alarm_fire(a); j--)	/* by fire time */
			c[j] = c[j - 1];
		c[j] = a;
		n++;
	}
	for (i = 0; i < n; i++) {
		*(volatile u32 *)(c[i] + 0x10) = i ? c[i - 1] : 0;
		*(volatile u32 *)(c[i] + 0x14) = i + 1 < n ? c[i + 1] : 0;
	}
	*(volatile u32 *)ALARM_QUEUE = n ? c[0] : 0;
	*(volatile u32 *)(ALARM_QUEUE + 4) = n ? c[n - 1] : 0;
	set_dec(0x100);	/* soon: the OS looks at the queue's head again, sets its timer */
}

/* Folds the oldest capture into the base: each of its versions becomes its
 * page's base (the oldest version, the end of the page's chain). */
static void retire_oldest(Engine *e)
{
	Cap *c = &e->cap[e->cap_first];
	u16 *base = t16(T_BASE), *head = t16(T_HEAD), *prev = t16(T_REC_PREV), *next = t16(T_REC_NEXT);
	u32 r = c->first;
	while (r != NONE) {
		u32 p = t16(T_REC_PAGE)[r], nx = next[r];
		free_store(e, base[p]);
		base[p] = t16(T_REC_STORE)[r];
		if (head[p] == r) {
			head[p] = NONE;
		} else {
			u32 q = head[p];
			while (prev[q] != r)
				q = prev[q];
			prev[q] = NONE;
		}
		free_rec(e, r);
		r = nx;
	}
	e->cap_first = (e->cap_first + 1) % CAPS;
	e->caps--;
}

/* Drops the newest capture (a load went back before it) */
static void drop_newest(Engine *e)
{
	Cap *c = &e->cap[(e->cap_first + e->caps - 1) % CAPS];
	u16 *head = t16(T_HEAD), *next = t16(T_REC_NEXT);
	u32 r = c->first;
	while (r != NONE) {
		u32 p = t16(T_REC_PAGE)[r], nx = next[r];
		head[p] = t16(T_REC_PREV)[r];
		free_store(e, t16(T_REC_STORE)[r]);
		free_rec(e, r);
		r = nx;
	}
	e->caps--;
}

#ifdef RB_HOST
#define PTE_W1_UNCACHED(off) ((volatile u32 *)M2(HTAB + (off) + 4))
#else
#define PTE_W1_UNCACHED(off) ((volatile u32 *)(0xD0000000u | (HTAB + (off) + 4)))	/* MEM2, uncached */
#endif

/* The pages written since the last capture (the C bits, cleared), and the
 * pages below 2 MB, always: into T_WORK. Returns how many. The locked cache
 * must be on (lc_begin).
 *
 * The page table comes into the locked cache by the DMA engine, 16 KB at a time
 * (4 KB in 8-9 us), and is read there: read by the CPU from MEM2, one PTE a
 * round trip, a scan took 313 us of its 343 (seen). The tracked pages are in
 * the table's order (make_base), so a scan walks them with it, 8 KB read ahead.
 *
 * The MMU's table walks go through the data cache: a C bit it set may be in a
 * line there, not in memory, where the DMA engine reads (without this flush:
 * a desync at the first load, seen). Only a PTE whose C bit was set is
 * written, straight to memory, uncached. A line the MMU dirties after the
 * flush and writes back later can only set again a bit cleared here (a page
 * saved once more than needed), never lose one; and a bit set between the
 * copy and that write is on a page already taken (it was set in the copy).
 * The TLB entries go below, so the next write to these pages sets it again. */
static u32 changed_pages(Engine *e)
{
	u16 *track = t16(T_TRACK), *work = t16(T_WORK), *pte_of = t16(T_PTE_OF);
	u32 i, n, part;
	for (n = 0; n < e->low; n++)
		work[n] = track[n];
	i = e->low;
	flush_lines(M2(HTAB), 0x10000 / 32);
	lc_load(LC, HTAB, 128);
	lc_load(LC + 4096, HTAB + 4096, 128);
	for (part = 0; part < 16; part += 2) {	/* 8 KB at a time: in buffers 0-1, 2-3, 0-1, ... */
		u32 end = (part + 2) * 4096, buf = LC + (part & 2) * 4096;
		dma_wait();
		if (part + 2 < 16) {
			u32 next = LC + ((part + 2) & 2) * 4096;
			lc_load(next, HTAB + (part + 2) * 4096, 128);
			lc_load(next + 4096, HTAB + (part + 3) * 4096, 128);
		}
		for (; i < e->tracked; i++) {
			u32 p = track[i], off = pte_of[p] * 8, w1;
			if (off >= end)
				break;
			w1 = *(const volatile u32 *)(buf + off - part * 4096 + 4);
			if (w1 & 0x80) {
				*PTE_W1_UNCACHED(off) = w1 & ~0x80u;
				work[n++] = (u16)p;
			}
		}
	}
	tlb_kill(work + e->low, n - e->low);	/* the pages just cleared */
	return n;
}

#ifdef RB_VERIFY_DEEP
/* 4 KB as the CPU has them, in a word; a page's */
static u32 sum_at(u32 at)
{
	const volatile u32 *w = (const volatile u32 *)at;
	u32 sum = 0, i;
	for (i = 0; i < 1024; i++)
		sum = ((sum << 1) | (sum >> 31)) ^ w[i];
	return sum;
}

static u32 page_sum(u32 p)
{
	return sum_at(0x80000000u + (p << 12));
}

static void vd_line(u32 kind, const u32 *v, u32 nv, u32 n, const u32 *w);
#endif

/* The base: every saved page, at the game's first frame. The page table must
 * be on. Returns 0 when it does not fit. */
static int make_base(Engine *e)
{
	u32 msr, p, i, n = 0;
	u16 *track = t16(T_TRACK), *base = t16(T_BASE), *head = t16(T_HEAD);
	u8 *class = t8(T_CLASS);
	u64 t0 = timebase();
	for (p = 0; p < PAGES; p++)	/* below 2 MB first */
		if (class[p] && p < FIRST)
			track[n++] = (u16)p;
	e->low = n;
	/* the rest in the page table's order, so a scan reads it straight
	 * through: each PTE's page from its API (the page number's top 6 bits)
	 * and its group (the bottom 10, hashed with VSID; map_page) */
	for (i = 0; i < 0x10000 / 8; i++) {
		u32 w0 = *(u32 *)M2(HTAB + i * 8), hash;
		if (!(w0 & 0x80000000u))
			continue;
		hash = (w0 & 0x40) ? ~(i >> 3) : (i >> 3);
		p = ((w0 & 0x3F) << 10) | ((hash ^ VSID) & 0x3FF);
		if (p >= FIRST && p < PAGES && class[p])
			track[n++] = (u16)p;
	}
	e->tracked = n;
	if (n * 2 > STORES || n > RECS) {
		e->errors++;
		return 0;
	}
	for (i = 0; i < STORES; i++)
		t16(T_FREE)[i] = (u16)(STORES - 1 - i);
	e->free_stores = STORES;
	e->rec_free = NONE;
	e->free_recs = 0;
	for (i = RECS; i > 0; i--)
		free_rec(e, i - 1);
	e->cap_first = e->caps = 0;
	for (p = 0; p < PAGES; p++)
		base[p] = head[p] = NONE;

	msr = irq_off();
	for (i = 0; i < n; i++)
		store_lines(0x80000000u + (track[i] << 12), 128);
	lc_begin();
	for (i = 0; i < n; i++) {
		u32 s = t16(T_FREE)[--e->free_stores];
		base[track[i]] = (u16)s;
#ifdef RB_VERIFY_DEEP
		t32(T_SUMS)[s] = page_sum(track[i]);
#endif
		dma_page(i, store_phys(s), track[i] << 12);
	}
	changed_pages(e);	/* clear the C bits: the base is now */
	lc_end();
#ifdef RB_VERIFY_DEEP
	{	/* each copy as it now is in memory against its page's sum: "rb VD b <pages> | <copies not
		 * their pages> [<the first's page> <the copy's sum> <the page's>]" */
		u32 bad = 0, w[3];
		w[0] = w[1] = w[2] = 0;
		for (i = n; i > 0; i--) {	/* (the one reported: the first in the copies' order) */
			u32 s = base[track[i - 1]], sum;
			flush_lines(M2(store_phys(s)), 128);	/* (written by DMA: the cache may hold an older copy) */
			sum = sum_at(M2(store_phys(s)));
			if (sum != t32(T_SUMS)[s]) {
				bad++;
				w[0] = 0x80000000u + (track[i - 1] << 12);
				w[1] = sum;
				w[2] = t32(T_SUMS)[s];
			}
		}
		irq_restore(msr);
		vd_line('b', &n, 1, bad, w);
	}
#else
	irq_restore(msr);
#endif
	e->base_ticks = (u32)(timebase() - t0);
	return 1;
}

#ifdef RB_HOST
#define video_through(e) ((void)(e))
#else
static void video_through(Engine *e);
#endif

/* 0xB1: the pages written since the last capture, as frame `frame`'s */
static void capture(Engine *e, s32 frame)
{
	u32 msr = irq_off(), n, i;
	int bg;
	u16 *work = t16(T_WORK), *head = t16(T_HEAD);
	u64 t0 = timebase();
	Cap *c;
#ifdef RB_VERIFY
	u32 vt0 = e->vt_count;
#endif
	bg_mark(12, (u32)frame);
	/* (not while direct_track has the game level: that is how it has been run from 80 ms up; a game the
	 * kernel's setting leaves alone never has the bit, and saves as it always did) */
	bg = !(e->direct_is & 1) && bg_ready();
	bg_finish();	/* the last capture's copies, and its pages writable: the C bits mean writes again */
	lc_begin();
	n = changed_pages(e);
	video_through(e);
	e->cap_scan += (u32)(timebase() - t0);
	if (e->caps == CAPS)
		retire_oldest(e);
	while ((e->free_stores < n || e->free_recs < n) && e->caps)
		retire_oldest(e);
	if (e->free_stores < n || e->free_recs < n) {	/* cannot happen: stores >= 2 x pages */
		e->errors++;
		lc_end();
		irq_restore(msr);
		return;
	}
	c = &e->cap[(e->cap_first + e->caps) % CAPS];
	{
		u32 *al = t32(T_ALARMS) + ((e->cap_first + e->caps) % CAPS) * 16;
		al[0] = queued_alarms(al + 1);
	}
	e->caps++;
	c->frame = frame;
	c->first = NONE;
	c->count = (u16)n;
	for (i = 0; i < n; i++) {
		u32 p = work[i], s = t16(T_FREE)[--e->free_stores], r = e->rec_free;
		e->rec_free = t16(T_REC_NEXT)[r];
		e->free_recs--;
		t16(T_REC_STORE)[r] = (u16)s;
		t16(T_REC_PAGE)[r] = (u16)p;
		t16(T_REC_PREV)[r] = head[p];
		head[p] = (u16)r;
		t16(T_REC_NEXT)[r] = c->first;
		c->first = (u16)r;
		store_lines(0x80000000u + (p << 12), 128);
#ifdef RB_VERIFY_DEEP
		t32(T_SUMS)[s] = page_sum(p);	/* (a handler let through below writes the cache: neither the sum nor the copy has it) */
#endif
		t32(T_HIST)[p >> 4]++;
		if (!(i & 15))
			video_through(e);
	}
	if (bg) {
		bg_start(e, n);
	} else {
		for (i = 0; i < n; i++) {
			if (!(i & 7))
				video_through(e);
			dma_page(i, store_phys(t16(T_FREE)[e->free_stores + n - 1 - i]), work[i] << 12);
		}
		lc_end();
	}
#ifdef RB_VERIFY
	/* (each 8th, unless a retrace or a draw went through meanwhile: their handlers write the
	 * OS's thread context and VI's counters, which the copies took before) */
	if (e->captures % 8 == 0)
		bg_finish();
	if (e->captures % 8 == 0 && e->vt_count == vt0) {	/* each 8th: each copy's saved bytes against the page */
		u32 none[6];	/* (one by one: -Os would make it a memset, which the engine has none of) */
		none[0] = none[1] = none[2] = none[3] = none[4] = none[5] = 0;
		for (i = 0; i < n; i++) {
			u32 copy = M2(store_phys(t16(T_FREE)[e->free_stores + n - 1 - i]));
			flush_lines(copy, 128);
			saved_ranges(work[i], none, copy, e);
		}
	}
#endif
	{
		u32 t = (u32)(timebase() - t0);
#if defined(RB_WALK) && !defined(RB_HOST) && !defined(RB_DIRECT_LOG)
		e->verify_want = n;	/* (walk_page) */
		e->verify_at = (u32)frame << 1 | 1;
#endif
		e->captures++;
		e->cap_pages += n;
		e->cap_ticks += t;
		if (n > e->cap_pages_max)
			e->cap_pages_max = n;
		if (t > e->cap_ticks_max)
			e->cap_ticks_max = t;
		irq_restore(msr);
		if (t > 3000 * 61)
			TR(11, n, t / 61);
	}
}

/* Page p's saved bytes, as ranges [lo, hi): the regions, less the excludes
 * and the blocks a load preserves (keep: 3 address, size pairs), copied from
 * the copy of the page at src (e null) or checked against it. */
static void saved_ranges(u32 p, const u32 *keep, u32 src, Engine *e)
{
	const u32 *list = t32(T_LIST);
	u32 p0 = 0x80000000u + (p << 12), p1 = p0 + 4096, r;
	for (r = 0; r < 4; r++) {
		u32 cur = list[2 * r] > p0 ? list[2 * r] : p0;
		u32 end = list[2 * r + 1] < p1 ? list[2 * r + 1] : p1;
		while (cur < end) {
			/* the next part left out from cur: its start and end */
			u32 xs = end, xe = end, k;
			const u32 *x;
			for (x = list + 8; x[0]; x += 2)
				if (x[1] > cur && x[0] < xs) {
					xs = x[0] > cur ? x[0] : cur;
					xe = x[1];
				}
			for (k = 0; k < 6; k += 2)
				if (keep[k] && keep[k] + keep[k + 1] > cur && keep[k] < xs) {
					xs = keep[k] > cur ? keep[k] : cur;
					xe = keep[k] + keep[k + 1];
				}
			if (xs > end)
				xs = end;
			if (cur < xs) {
#ifdef RB_VERIFY
				if (e)
					verify_range(cur, xs, src + (cur - p0), e);
				else
#endif
					copy_range(cur, xs, src + (cur - p0));
			}
			cur = xe > cur ? xe : cur;
		}
	}
}

/* [lo, hi) from a copy of its page at src (the same offsets) */
static void copy_range(u32 lo, u32 hi, u32 src)
{
	while (lo < hi && (lo & 3))
		*(u8 *)lo++ = *(u8 *)src++;
	while (lo + 4 <= hi) {
		*(u32 *)lo = *(u32 *)src;
		lo += 4;
		src += 4;
	}
	while (lo < hi)
		*(u8 *)lo++ = *(u8 *)src++;
}

/* Writes back page p's saved bytes from its copy in the locked cache at `lc`. */
static void restore_part(u32 p, u32 lc, const u32 *keep)
{
	saved_ranges(p, keep, lc, 0);
}

#ifdef RB_VERIFY
/* Test builds (RB_VERIFY): after a load, saved bytes are read back and
 * compared with the copies of their pages for the frame; the first difference
 * is reported (verify_bad, verify_at, verify_have, verify_want). */
static void verify_range(u32 lo, u32 hi, u32 src, Engine *e)
{
	if (e->probed)
		return;
	for (; lo < hi; lo++, src++) {
		if (!(lo & 3) && lo + 4 <= hi && *(volatile u32 *)lo == *(volatile u32 *)src) {
			lo += 3;
			src += 3;
			continue;
		}
		if (*(volatile u8 *)lo != *(volatile u8 *)src) {
			if (e->probing) {
				e->probed = lo;
				return;
			}
			if (!e->verify_bad++) {
				e->verify_at = lo;
				e->verify_have = *(volatile u8 *)lo;
				e->verify_want = *(volatile u8 *)src;
			}
			return;
		}
	}
}

static void verify_page(Engine *e, u32 p, const u32 *keep)
{
	u32 r = t16(T_HEAD)[p], copy = M2(store_phys(r != NONE ? t16(T_REC_STORE)[r] : t16(T_BASE)[p])), i;
	u32 p0 = 0x80000000u + (p << 12);
	for (i = 0; i < 10; i++)	/* a page the GPU writes a texture into: not checked */
		if (overlap(p0, p0 + 4096, e->copies[i][0], e->copies[i][1]))
			return;
	flush_lines(copy, 128);	/* (written by DMA: the cache may hold an older copy) */
	saved_ranges(p, keep, copy, e);
}

/* Page p in a run of unmarked pages: a neighbour's run grows, or a new one
 * (8 at most; then only counted). */
static void note_unmarked(Engine *e, u32 p, u32 at)
{
	u32 j, runs = e->unmarked < 8 ? e->unmarked : 8;
	e->unmarked++;
	for (j = 0; j < runs; j++) {
		if (p >= e->unmarked_lo[j] && p < e->unmarked_hi[j])
			return;
		if (p + 1 == e->unmarked_lo[j]) {
			e->unmarked_lo[j] = (u16)p;
			return;
		}
		if (p == e->unmarked_hi[j]) {
			e->unmarked_hi[j] = (u16)(p + 1);
			return;
		}
	}
	for (j = 0; j < 8 && e->unmarked_hi[j]; j++)
		;
	if (j < 8) {
		e->unmarked_lo[j] = (u16)p;
		e->unmarked_hi[j] = (u16)(p + 1);
		e->unmarked_at[j] = at;
	}
}

/* The restored pages; with `all`, then every other saved page too: one that
 * differs was written with no MMU changed bit (by a DMA, say, or the GPU's
 * copies to textures, which nothing reads back) and so not restored: counted,
 * as runs for the report (note_unmarked). T_MARK is clear here. */
static void verify_load(Engine *e, u32 n, const u32 *keep, int all)
{
	u16 *work = t16(T_WORK), *track = t16(T_TRACK);
	u8 *mark = t8(T_MARK);
	u32 i;
	for (i = 0; i < n; i++)
		verify_page(e, work[i], keep);
	if (!all)
		return;
	for (i = 0; i < n; i++)
		mark[work[i] >> 3] |= (u8)(1 << (work[i] & 7));
	e->probing = 1;
	for (i = 0; i < e->tracked; i++) {
		u32 p = track[i];
		if (mark[p >> 3] & (1 << (p & 7)))
			continue;
		e->probed = 0;
		verify_page(e, p, keep);
		if (e->probed)
			note_unmarked(e, p, e->probed);
	}
	e->probing = e->probed = 0;
	for (i = 0; i < n; i++)
		mark[work[i] >> 3] = 0;
}

/* The data BATs and HID4 (its SBE turns on DBAT4-7) as the game runs with
 * the page table: a BAT over MEM1 above 2 MB would hide writes from it. */
static void report_bats(void)
{
	char a[48], b[64];
	Out o;
	int i;
	for (i = 0; i < 2; i++) {
		int k;
		o.p = a;
		o.end = a + 47;
		put4(&o, C4('r', 'b', ' ', 'd'));
		put4(&o, C4('b', 'a', 't', ' '));
		for (k = 0; k < 4; k++) {
			put_x(&o, i ? rd_xbat(k) : rd_bat(k));
			put_c(&o, ' ');
		}
		*o.p = 0;
		o.p = b;
		o.end = b + 63;
		for (k = 4; k < 8; k++) {
			put_x(&o, i ? rd_xbat(k) : rd_bat(k));
			put_c(&o, ' ');
		}
		if (i)
			put_x(&o, rd_hid4());
		*o.p = 0;
		send(a, b);
	}
}

/* After a load (interrupts on again), to the kernel's log: each new run of
 * unmarked pages at once, as the game may not last (its first differing
 * byte, and the word there now), and the first restored byte that came back
 * wrong, once: "rb VERIFY", which stops a sweep (tools/sweep/run_sweep.py). */
static void verify_report(Engine *e, s32 frame, u32 depth)
{
	char a[48], b[64];
	Out o;
	for (; e->unmarked_told < 8 && e->unmarked_hi[e->unmarked_told]; e->unmarked_told++) {
		u32 at = e->unmarked_at[e->unmarked_told];
		o.p = a;
		o.end = a + 47;
		put4(&o, C4('r', 'b', ' ', 'u'));
		put4(&o, C4('n', 'm', 'a', 'r'));
		put4(&o, C4('k', 'e', 'd', ' '));
		put_x(&o, at);
		*o.p = 0;
		o.p = b;
		o.end = b + 63;
		put4(&o, C4('f', 'r', 'a', 'm'));
		put4(&o, C4('e', ' ', 0, 0));
		put_u(&o, (u32)frame);
		put4(&o, C4(' ', 'n', 'o', 'w'));
		put_c(&o, ' ');
		put_x(&o, rd32(at & ~3u));
		{	/* and in the frame's copy */
			u32 p = (at - 0x80000000u) >> 12, rec = t16(T_HEAD)[p];
			u32 copy = M2(store_phys(rec != NONE ? t16(T_REC_STORE)[rec] : t16(T_BASE)[p]));
			put4(&o, C4(' ', 'w', 'a', 's'));
			put_c(&o, ' ');
			put_x(&o, rd32(copy + (at & 0xFFCu)));
		}
		*o.p = 0;
		send(a, b);
	}
	if (!e->verify_bad || e->verify_reported)
		return;
	e->verify_reported = 1;
	o.p = a;
	o.end = a + 47;
	put4(&o, C4('r', 'b', ' ', 'V'));
	put4(&o, C4('E', 'R', 'I', 'F'));
	put4(&o, C4('Y', ' ', 0, 0));
	put_x(&o, e->verify_at);
	put4(&o, C4(' ', 'h', 'a', 'v'));
	put4(&o, C4('e', '=', 0, 0));
	put_u(&o, e->verify_have);
	put4(&o, C4(' ', 'w', 'a', 'n'));
	put4(&o, C4('t', '=', 0, 0));
	put_u(&o, e->verify_want);
	*o.p = 0;
	o.p = b;
	o.end = b + 63;
	put4(&o, C4('f', 'r', 'a', 'm'));
	put4(&o, C4('e', '=', 0, 0));
	put_u(&o, (u32)frame);
	put4(&o, C4(' ', 'd', 'e', 'p'));
	put4(&o, C4('t', 'h', '=', 0));
	put_u(&o, depth);
	*o.p = 0;
	send(a, b);
}

#endif

/* 0xB2: back to frame `frame` (its newest capture), preserving the blocks in
 * the request (address, size; up to 3) */
/* A page a load restores whole: none of it is kept across the load */
static int load_whole(u32 p, const u32 *keep)
{
	u32 p0 = 0x80000000u + (p << 12), j;
	if (t8(T_CLASS)[p] != 1)
		return 0;
	for (j = 0; j < 6; j += 2)
		if (keep[j] && overlap(p0, p0 + 4096, keep[j], keep[j] + keep[j + 1]))
			return 0;
	return 1;
}

#ifdef RB_VERIFY_DEEP
/* Is page p what copy s was made of? 0 if so. Else, in w: the first word of the page that is not the
 * copy's, with the page's and the copy's; or, the two alike (the copy is not what was saved), the page's
 * address | 2, its sum now and the sum the copy was made with. A page the GPU copies a texture into is
 * passed over (it writes when it
 * comes to it, with no C bit: the fighters' shadows, the screen for refraction). */
static int vd_page(Engine *e, u32 p, u32 *w)
{
	u32 r = t16(T_HEAD)[p], s = r != NONE ? t16(T_REC_STORE)[r] : t16(T_BASE)[p], sum, i;
	const volatile u32 *m = (const volatile u32 *)(0x80000000u + (p << 12)), *c = (const volatile u32 *)M2(store_phys(s));
	for (i = 0; i < 10; i++)
		if (overlap((u32)m, (u32)m + 4096, e->copies[i][0], e->copies[i][1]))
			return 0;
	sum = page_sum(p);
	if (sum == t32(T_SUMS)[s])
		return 0;
	flush_lines((u32)c, 128);	/* (written by DMA: the cache may hold an older copy) */
	for (i = 0; i < 1024 && m[i] == c[i]; i++)
		;
	if (i < 1024) {
		w[0] = (u32)&m[i];
		w[1] = m[i];
		w[2] = c[i];
	} else {
		w[0] = (u32)m | 2;
		w[1] = sum;
		w[2] = t32(T_SUMS)[s];
	}
	return 1;
}

/* After a deep load's copies (interrupts off, T_MARK clear): the pages it put back whole; then, with
 * `all`, every other page saved whole. Counts in verify_bad and unmarked, the first of each in
 * verify_at.. and unmarked_at[0..2] (vd_page's three words). */
static void vd_check(Engine *e, u32 n, const u32 *keep, int all)
{
	u16 *work = t16(T_WORK), *track = t16(T_TRACK);
	u8 *mark = t8(T_MARK);
	u32 i, w[3];
	for (i = 0; i < n; i++) {
		if (load_whole(work[i], keep) && vd_page(e, work[i], w) && !e->verify_bad++) {
			e->verify_at = w[0];
			e->verify_have = w[1];
			e->verify_want = w[2];
		}
		mark[work[i] >> 3] |= (u8)(1 << (work[i] & 7));
	}
	for (i = 0; all && i < e->tracked; i++) {
		u32 p = track[i];
		if (!(mark[p >> 3] & (1 << (p & 7))) && load_whole(p, keep) && vd_page(e, p, w) && !e->unmarked++) {
			e->unmarked_at[0] = w[0];
			e->unmarked_at[1] = w[1];
			e->unmarked_at[2] = w[2];
		}
	}
	for (i = 0; i < n; i++)
		mark[work[i] >> 3] = 0;
}

/* A check's lines (interrupts on again):
 *   rb VD l <frame> <the load's depth> <pages> <retraces and draws let through meanwhile>
 *         | <pages put back that are not what was saved> [<vd_page's three words for the first>]
 *   rb VD u <frame> | <other saved pages that are not their newest copies> [the same]
 * (not "rb VERIFY", which ends a sweep: a let-through's handlers leave differences that are none) */
static void vd_line(u32 kind, const u32 *v, u32 nv, u32 n, const u32 *w)
{
	char a[48], b[64];
	Out o;
	u32 k;
	o.p = a;
	o.end = a + 47;
	put4(&o, C4('r', 'b', ' ', 'V'));
	put4(&o, C4('D', ' ', kind, 0));
	for (k = 0; k < nv; k++) {
		put_c(&o, ' ');
		put_u(&o, v[k]);
	}
	*o.p = 0;
	o.p = b;
	o.end = b + 63;
	put_u(&o, n);
	for (k = 0; n && k < 3; k++) {
		put_c(&o, ' ');
		put_x(&o, w[k]);
	}
	*o.p = 0;
	send(a, b);
}

static void vd_report(Engine *e, s32 frame, u32 depth, u32 n, u32 thr)
{
	u32 v[4];
	v[0] = (u32)frame;
	v[1] = depth;
	v[2] = n;
	v[3] = thr;
	vd_line('l', v, 4, e->verify_bad, &e->verify_at);
	if (e->unmarked)
		vd_line('u', v, 1, e->unmarked, e->unmarked_at);
	e->verify_bad = e->unmarked = 0;
}
#endif

static void load(Engine *e, s32 frame, const u8 *blocks)
{
	u32 msr = irq_off(), n, i, k, keep[6], depth;
	u32 alarms_now[ALARMS_MAX], alarms_now_n, alarms_then[ALARMS_MAX], alarms_then_n;
	u16 *work = t16(T_WORK), *head = t16(T_HEAD), *base = t16(T_BASE);
	u8 *mark = t8(T_MARK);
	u64 t0 = timebase(), t_start = t0;
#if defined(RB_VERIFY) || defined(RB_VERIFY_DEEP)
	u32 vt0 = e->vt_count;
#endif
	for (k = 0; k < 3; k++) {
		keep[2 * k] = be32(blocks + k * 8);
		keep[2 * k + 1] = keep[2 * k] ? be32(blocks + k * 8 + 4) : 0;
	}
	for (k = e->caps; k > 0; k--)
		if (e->cap[(e->cap_first + k - 1) % CAPS].frame == frame)
			break;
	if (!k) {	/* Slippi asks for a frame it did not capture: report it at once */
		if (!e->misses++) {
			char a[48], b[64];
			Out o;
			o.p = a;
			o.end = a + 47;
			put4(&o, C4('r', 'b', ' ', 'M'));
			put4(&o, C4('I', 'S', 'S', ' '));
			put_u(&o, (u32)frame);
			*o.p = 0;
			b[0] = 0;
			irq_restore(msr);
			send(a, b);
			return;
		}
		irq_restore(msr);
		return;
	}
	depth = e->caps - k;
	{
		u32 *al = t32(T_ALARMS) + ((e->cap_first + k - 1) % CAPS) * 16, i2;
		alarms_then_n = al[0] > ALARMS_MAX ? 0 : al[0];
		for (i2 = 0; i2 < alarms_then_n; i2++)
			alarms_then[i2] = al[1 + i2];
		alarms_now_n = queued_alarms(alarms_now);
	}

	/* what changed since: written since the last capture, and in the later captures */
	bg_mark(10, (u32)frame);
	bg_finish();
	lc_begin();
	n = changed_pages(e);
	video_through(e);
	e->ld_scan += (u32)(timebase() - t0);
	t0 = timebase();
	for (i = 0; i < n; i++)
		mark[work[i] >> 3] |= (u8)(1 << (work[i] & 7));
	while (e->caps > k) {
		Cap *c = &e->cap[(e->cap_first + e->caps - 1) % CAPS];
		u32 r;
		for (r = c->first; r != NONE; r = t16(T_REC_NEXT)[r]) {
			u32 p = t16(T_REC_PAGE)[r];
			if (!(mark[p >> 3] & (1 << (p & 7)))) {
				mark[p >> 3] |= (u8)(1 << (p & 7));
				work[n++] = (u16)p;
			}
		}
		drop_newest(e);
	}

	/* whole pages straight back; the rest through the locked cache, in part. Their cache
	 * lines go first, all of them: a dcbi competes for the bus with a copy under way, and
	 * interleaved with the copies a load took 4.8 ms, not 3.8. This pass is for dirty lines,
	 * which could be written back over a page once it is restored. */
	for (i = 0; i < n; i++) {
		if (load_whole(work[i], keep))
			kill_lines(0x80000000u + (work[i] << 12), 128);
		if (!(i & 15))
			video_through(e);
	}
	for (i = k = 0; i < n; i++) {
		u32 p = work[i];
		if (!(i & 7))
			video_through(e);
		if (load_whole(p, keep))
			dma_page(k++, p << 12, store_phys(head[p] != NONE ? t16(T_REC_STORE)[head[p]] : base[p]));
	}
	dma_wait();
	/* And this one for clean lines: a load on a mispredicted path may have brought one back
	 * while its page was being restored, and the game would read that old copy. (Seen once
	 * in a verify build, one word in 15 games; a pass costs 0.2 ms.) */
	for (i = 0; i < n; i++)
		if (load_whole(work[i], keep))
			kill_lines(0x80000000u + (work[i] << 12), 128);
	{
		u64 t1 = timebase();
		e->ld_whole += (u32)(t1 - t0);
		t0 = t1;
	}
	for (i = 0; i < n; i++) {
		u32 p = work[i];
		mark[p >> 3] = 0;
		if (load_whole(p, keep))
			continue;
		lc_load(LC, store_phys(head[p] != NONE ? t16(T_REC_STORE)[head[p]] : base[p]), 128);
		dma_wait();
		restore_part(p, LC, keep);
		e->ld_parts++;
		video_through(e);
	}
	lc_end();
#ifdef RB_VERIFY
#ifdef RB_VERIFY_ALL
	if (e->vt_count == vt0)	/* (as in capture: not across a retrace let through) */
		verify_load(e, n, keep, 1);	/* every load, all pages: slow */
#else
	if (e->loads % 4 == 0 && e->vt_count == vt0)	/* each 4th, each 16th all pages: the timing stays a game's */
		verify_load(e, n, keep, e->loads % 16 == 0);
#endif
#endif
#ifdef RB_VERIFY_DEEP
	/* (the pages put back whatever went through meanwhile; every other saved page only if nothing
	 * did: a retrace's handlers write pages that are not the load's) */
	if (depth >= VD_DEPTH)
		vd_check(e, n, keep, e->vt_count == vt0);
#endif
	/* (Memory is frame `frame` again. The copies leave no C bits; the few this code's own
	 * writes leave only have those pages saved once more than they need.) */
	relink_alarms(alarms_now, alarms_now_n, alarms_then, alarms_then_n);
	bg_mark(11, (u32)frame);
	e->ld_part += (u32)(timebase() - t0);
	t0 = t_start;
	{
		u32 t = (u32)(timebase() - t0);
#if defined(RB_WALK) && !defined(RB_HOST) && !defined(RB_DIRECT_LOG)
		e->verify_want = n;	/* (walk_page) */
		e->verify_at = (u32)frame << 1;
#endif
		e->loads++;
		e->load_pages += n;
		e->load_ticks += t;
		if (n > e->load_pages_max)
			e->load_pages_max = n;
		if (t > e->load_ticks_max)
			e->load_ticks_max = t;
		if (depth > e->load_depth_max)
			e->load_depth_max = depth;
		irq_restore(msr);
		TR(4, depth, t / 61);
	}
#ifdef RB_VERIFY
	verify_report(e, frame, depth);
#endif
#ifdef RB_VERIFY_DEEP
	if (depth >= VD_DEPTH)
		vd_report(e, frame, depth, n, e->vt_count - vt0);
#endif
}


/* ---- FN_EXITransferBuffer --------------------------------------------------- */

/* Slippi's branch at 0x800055F0 to its function, pointed at exi_wrapper
 * (again if anything rewrites it) */
static u32 branch_to(u32 at, u32 to)
{
	return 0x48000000u | ((to - at) & 0x03FFFFFCu);
}

/* What ran last, for a report of what went wrong (so_exi.c reads it from MEM2,
 * uncached, so it survives a crash): op, a millisecond clock, the frame. */
enum { EV_CAPTURE, EV_LOAD, EV_SAVED_LATE, EV_LOST };

static void note_event(u32 op, s32 frame)
{
#ifndef RB_HOST
	volatile u32 *ring = (volatile u32 *)(0xD0000000u | (RB_SCRATCH + T_EVENTS));
	u32 i = ring[0];
	ring[1 + i % EVENTS] = op << 30 | ((u32)(timebase() >> 16) & 0x3FF) << 20 | ((u32)frame & 0xFFFFF);
	ring[0] = i + 1;
#else
	(void)op;
	(void)frame;
#endif
}

static void write_code(u32 at, u32 insn)
{
	*(volatile u32 *)at = insn;
	flush_code(at, 4);
}

/* Takes over the b at `at`, keeping its target in *orig. 1 once it is ours;
 * 0 when there is no b (Slippi's codes are not in). */
static int take_branch(u32 at, u32 to, u32 *orig)
{
	u32 insn = rd32(at), off = insn & 0x03FFFFFCu;
	if (insn == branch_to(at, to))
		return 1;
	if ((insn & 0xFC000003u) != 0x48000000u)
		return 0;
	*orig = at + (off & 0x02000000u ? off | 0xFC000000u : off);
	write_code(at, branch_to(at, to));
	return 1;
}

/* ---- background saves (T_BG_SAVES) --------------------------------------------
 *
 * A capture's copies, 1.4 ms of DMA for its ~250 pages, go on while the game runs
 * instead of with it waiting: each page write-protected (PP 11, read-only for
 * supervisor with Ks 0) until its copy is done. The DMA engine's queue holds 15
 * transfers, so it's kept fed: from the decrementer every BG_TICK_US (rb_dec),
 * and at every EXI call and VI interrupt. A write to a page still waiting faults
 * (DSI, protection): rb_dsi copies everything up to it at once and lets the
 * write go on. A capture, load, or the game's end finishes them first (bg_finish).
 * The page table's entries change through the data cache, as the MMU's table
 * walks read them: an uncached write could be hidden by a line there. */
#define T_BG 0x2C800			/* Bg */
#define T_BG_PAGE 0x2C900		/* u16 per page waiting, in copy order */
#define T_BG_STORE 0x2D800		/* u16 per page: its copy's store */
#define T_BG_PENDING 0x2E700		/* bit per MEM1 page: protected, copy not done */
#define T_DSI_SAVE 0x2F600		/* the DSI handler's registers (0x98) and stack, 0x800 */
#define BG_QUEUE_MAX 12			/* DMA transfers waiting before another page goes in */
#define BG_TICK_US 50
#define OS_LOAD_CONTEXT(c) ((void (*)(void *))0x80345174u)(c)
#define OS_SYSTEM_TIME() ((s64 (*)(void))0x8034C410u)()
#define VECTOR_DSI 0x80000300u
#define VECTOR_DSI_INSN 0x7C9043A6u	/* mtsprg 0, r4: the OS's vector's first */
#define DEC_BRANCH 0x80343E40u		/* DecrementerExceptionHandler's b to its callback */
#define DEC_CALLBACK 0x80343BC8u

typedef struct {
	u32 active, n, issued, unprot, ops, hooked, dec_hooked;
	Engine *engine;		/* (bg_finish's video_through: set by bg_start) */
	u32 faults, spurious, finishes, ticks;
	u32 fault_ticks, finish_ticks, fault_max;
} Bg;

static volatile Bg *bg(void) { return (volatile Bg *)t32(T_BG); }

/* (diagnostics) 1 DSI in, 2 out, 3 tick in, 4 tick out to the game, 5 tick on to the OS,
 * 6 start, 7 finish, 8 VI interrupt, 9 feed done, 10 load in, 11 load out, 12 capture in, 13 tick, idle */
#ifndef RB_HOST
static void bg_mark(u32 what, u32 arg)
{
	volatile u32 *r = (volatile u32 *)(0xD0000000u | (RB_SCRATCH + T_BG_RING));
	u32 i = r[0]++ & 15;
	r[1 + i * 2] = what << 24 | (arg & 0xFFFFFF);
	r[2 + i * 2] = (u32)timebase();
}
#endif

static void bg_set_pte(u32 p, int ro)
{
	volatile u32 *w1 = (volatile u32 *)M2(HTAB + t16(T_PTE_OF)[p] * 8 + 4);
	u32 v = *w1;
	*w1 = ro ? ((v & ~0x80u) | 3u) : ((v & ~3u) | 2u);
}

/* Pages [unprot, done) writable again */
static void bg_unprotect_to(volatile Bg *b, u32 done)
{
	u16 *page = t16(T_BG_PAGE);
	u8 *pend = t8(T_BG_PENDING);
	u32 i, from = b->unprot;
	if (done <= from)
		return;
	for (i = from; i < done; i++) {
		u32 p = page[i];
		bg_set_pte(p, 0);
		pend[p >> 3] &= (u8)~(1 << (p & 7));
	}
	tlb_kill(page + from, done - from);
	b->unprot = done;
}

/* Pages surely copied: transfers done are at least those issued less the queue and
 * one running (another's waiting there only makes this smaller) */
static u32 bg_done(volatile Bg *b)
{
	u32 q = lc_queue() + 1, d;
	if (b->ops <= q)
		return 0;
	d = (b->ops - q) / 2;
	return d > b->issued ? b->issued : d;
}

/* The next pages' copies queued, as many as the buffers allow: a buffer is refilled only
 * with at most 4 operations queued, as in dma_page (HID2's queue need not count the one in
 * progress). Up to 2026-11-09 this went on to BG_QUEUE_MAX - 2 = 10 queued with the same 4
 * buffers: copies with another page's bytes, so a load later wrote those into the game.
 * The crash in hsdAllocMemPiece, a free list holding 0x7C0802A6 (mflr r0: some other page's
 * code), ~1 s into games at 45 ms, where background saves run (80 ms ping and up): runs
 * 20261109-000158 and -000425; the same signature, before background saves, 20261104-133249. */
static void bg_issue(volatile Bg *b)
{
	u16 *page = t16(T_BG_PAGE), *store = t16(T_BG_STORE);
	while (b->issued < b->n && lc_queue() <= 4) {
		u32 buf = LC + (b->issued & 3) * 4096;
		lc_load(buf, (u32)page[b->issued] << 12, 128);
		lc_store(buf, store_phys(store[b->issued]), 128);
		b->issued++;
		b->ops += 2;
	}
}

/* Everything issued so far done */
static void bg_complete(volatile Bg *b)
{
	dma_wait();
	bg_unprotect_to(b, b->issued);
	if (b->issued == b->n)
		b->active = 0;
}

/* The decrementer's ticks (dec_shim) only while copies are going. Never back through the
 * shim into the OS's callback: that hung, every time (runs 20261107-1731..1737), while a tick
 * that went back to the game itself (OSLoadContext) never did. So when the copies are done, or
 * an OS alarm is due, the OS's branch goes back and its timer goes off at once: the OS's own
 * handler runs, untouched, and sets its timer. The hook comes back with the next feed. */
#ifndef RB_HOST
static void bg_dec_on(volatile Bg *b)
{
	u32 orig;
	if (!b->dec_hooked && b->hooked) {
		take_branch(DEC_BRANCH, dec_shim_at(), &orig);
		b->dec_hooked = 1;
	}
	set_dec(BG_TICK_US * 61);
}

static void bg_dec_off(volatile Bg *b)
{
	if (b->dec_hooked) {
		write_code(DEC_BRANCH, branch_to(DEC_BRANCH, DEC_CALLBACK));
		b->dec_hooked = 0;
	}
}
#else
static void bg_dec_on(volatile Bg *b) { (void)b; }
static void bg_dec_off(volatile Bg *b) { (void)b; }
#endif

static void bg_feed(void)
{
	volatile Bg *b = bg();
	if (!b->active)
		return;
	bg_unprotect_to(b, bg_done(b));
	bg_issue(b);
	if (b->issued == b->n && !lc_queue())
		bg_complete(b);
	if (b->active && !b->dec_hooked)
		bg_dec_on(b);
}

/* All of them, now (interrupts off) */
static void bg_finish(void)
{
	volatile Bg *b = bg();
	u32 t0;
	if (!b->active)
		return;
	t0 = (u32)timebase();
	while (b->issued < b->n) {
		u32 before = b->issued;
		bg_issue(b);
		if (b->issued == before)
			dma_spin();
		video_through(b->engine);
	}
	bg_complete(b);
	bg_mark(7, (u32)timebase() - t0);
	b->finishes++;
	b->finish_ticks += (u32)timebase() - t0;
}

/* A DSI (glue.s, dsi_stub): a protection fault, so a write to a page waiting for its copy.
 * Interrupts off, its own stack; the write goes on when this returns. */
#ifdef RB_HOST
#define BG_DIAG(k) (t32(T_BG_DIAG) + (k))
#else
#define BG_DIAG(k) SCRATCH_WORD(T_BG_DIAG + 4 * (k))
#endif

void rb_dsi(Engine *e, u32 dar)
{
	volatile Bg *b = bg();
	u32 p = (dar - 0x80000000u) >> 12, t0 = (u32)timebase(), t;
	u32 srr0 = *(volatile u32 *)M2(RB_SCRATCH + T_DSI_SAVE + 4);
	(void)e;
	bg_mark(1, dar >> 8);
	(*BG_DIAG(0))++;
	*BG_DIAG(1) = dar;
	if (*BG_DIAG(2) == srr0) {
		/* the same instruction again and again: its page's entry to memory as well, in case
		 * the MMU reads it from there (diagnostics: how the hang in run 20261107-171450 came) */
		if (++*BG_DIAG(4) >= 64 && p >= FIRST && p < PAGES) {
			flush_lines(M2(HTAB + t16(T_PTE_OF)[p] * 8), 1);
			(*BG_DIAG(8))++;
		}
	} else {
		*BG_DIAG(2) = srr0;
		*BG_DIAG(4) = 0;
	}
	if (b->active && dar >= 0x80000000u && p < PAGES && (t8(T_BG_PENDING)[p >> 3] & (1 << (p & 7)))) {
		u16 *page = t16(T_BG_PAGE), *store = t16(T_BG_STORE);
		u32 i;
		for (i = b->unprot; i < b->n && page[i] != p; i++)
			;
		if (i < b->n && i > b->issued) {	/* not queued yet: next in line, not after all before it */
			u16 tp = page[i], ts = store[i];
			page[i] = page[b->issued];
			store[i] = store[b->issued];
			page[b->issued] = tp;
			store[b->issued] = ts;
			i = b->issued;
		}
		while (b->issued <= i && b->issued < b->n) {
			u32 before = b->issued;
			bg_issue(b);
			if (b->issued == before)
				dma_spin();
		}
		bg_complete(b);
		bg_issue(b);	/* the next ones going while the game goes on */
		t8(T_BG_HOT)[p >> 3] |= (u8)(1 << (p & 7));
		b->faults++;
		t = (u32)timebase() - t0;
		b->fault_ticks += t;
		if (t > b->fault_max)
			b->fault_max = t;
	} else if (dar >= 0x80000000u && p >= FIRST && p < PAGES) {
		u16 one = (u16)p;	/* not waiting: a protection left behind; writable */
		bg_set_pte(p, 0);
		tlb_kill(&one, 1);
		b->spurious++;
		(*BG_DIAG(5))++;
	}
	if (p >= FIRST && p < PAGES)
		*BG_DIAG(3) = *(volatile u32 *)M2(HTAB + t16(T_PTE_OF)[p] * 8 + 4);
	*BG_DIAG(7) = b->active << 31 | (b->issued & 0x7FFF) << 16 | (b->n & 0x7FFF) | lc_queue() << 27;
	bg_mark(2, b->issued << 12 | b->n);
}

/* The decrementer (glue.s, dec_shim): copies fed; while they're going, the next tick in
 * BG_TICK_US and on with the game, unless an OS alarm is due: then the OS's own callback,
 * which sets its timer again */
#if defined(RB_TRACE) && !defined(RB_HOST)
#define PROF_TICK_US 200
/* (diagnostics) a sample: the interrupted instruction, while a rollback runs, by 256 bytes */
static void prof_sample(u32 pc)
{
	u32 *t = t32(T_PROF), key = pc >> 8, h = (key * 2654435761u) >> 24, k;
	for (k = 0; k < 16; k++, h = (h + 1) & 255) {
		if (t[h * 2 + 1] && t[h * 2] != key)
			continue;
		t[h * 2] = key;
		t[h * 2 + 1]++;
		return;
	}
}
#endif

void rb_dec(Engine *e, void *context)
{
	volatile Bg *b = bg();
	u32 head, prof = 0;
	(void)e;
#if defined(RB_TRACE) && !defined(RB_HOST)
	{
		u32 odb = rd32(0x804DB6A0u + OFST_R13_ODB_ADDR);	/* (r13: SDA_BASE) */
		prof = 1;
		if (odb >= 0x80000000u && odb < 0x81800000u && rd8(odb + ODB_ROLLBACK_IS_ACTIVE))
			prof_sample(*(u32 *)((u8 *)context + 0x198));
	}
#endif
	if (b->active) {
		bg_mark(3, b->issued);
		b->ticks++;
		(*BG_DIAG(6))++;
		bg_feed();
	}
	head = rd32(ALARM_QUEUE);
	if (!(b->active || prof) || (head && alarm_fire(head) <= OS_SYSTEM_TIME())) {
		bg_mark(5, (u32)context >> 8 | b->active);
		bg_dec_off(b);
		set_dec(0x100);	/* (the OS's handler: its alarms, its timer) */
	} else {
		set_dec((b->active ? BG_TICK_US : 200) * 61);
		bg_mark(4, (u32)context >> 8);
	}
	OS_LOAD_CONTEXT(context);
}

#ifndef RB_HOST
static u32 lis_r3(u32 v) { return 0x3C600000u | (v >> 16); }
static u32 ori_r3(u32 v) { return 0x60630000u | (v & 0xFFFF); }

/* The DSI vector's first instruction to dsi_stub (real mode: its physical address), and the
 * decrementer's callback through dec_shim; both put back at the game's end */
static void bg_hook(void)
{
	volatile Bg *b = bg();
	u32 stub = dsi_stub_at(), tramp = dsi_tramp_at(), save = M2(RB_SCRATCH + T_DSI_SAVE), orig;
	if (b->hooked || rd32(VECTOR_DSI) != VECTOR_DSI_INSN || rd32(DEC_BRANCH) != branch_to(DEC_BRANCH, DEC_CALLBACK))
		return;
	write_code(stub + DSI_STUB_TRAMP, lis_r3(tramp));
	write_code(stub + DSI_STUB_TRAMP + 4, ori_r3(tramp));
	write_code(tramp, lis_r3(save));
	write_code(tramp + 4, ori_r3(save));
	(void)orig;
	write_code(VECTOR_DSI, 0x48000002u | (stub & 0x03FFFFFCu));	/* ba: real mode, physical */
	b->hooked = 1;
}

static void bg_unhook(void)
{
	volatile Bg *b = bg();
	if (!b->hooked)
		return;
	write_code(VECTOR_DSI, VECTOR_DSI_INSN);
	write_code(DEC_BRANCH, branch_to(DEC_BRANCH, DEC_CALLBACK));
	b->hooked = 0;
	b->dec_hooked = 0;
}
#else
static void bg_hook(void) { bg()->hooked = 1; }
static void bg_unhook(void) { bg()->hooked = 0; }
#endif

/* Background saves on (the kernel's word), hooked on first use */
static int bg_ready(void)
{
	if (!BG_ENABLED())
		return 0;
	if (!bg()->hooked)
		bg_hook();
	return bg()->hooked;
}

/* A capture's pages, past the ones below 2 MB (DBAT0: nothing to protect those
 * with, so copied now): waiting, protected, the first few copies started */
static void bg_start(Engine *e, u32 n)
{
	volatile Bg *b = bg();
	u16 *work = t16(T_WORK), *page = t16(T_BG_PAGE), *store = t16(T_BG_STORE);
	b->engine = e;
	u8 *pend = t8(T_BG_PENDING), *hot = t8(T_BG_HOT);
	u32 i, k = 0, m = 0, base = e->free_stores + n - 1;
	/* now: the pages below 2 MB, and those the game wrote before their copy came in an earlier
	 * capture (they'd only fault again, at ~50 us each, against 5.5 to copy now) */
	for (i = 0; i < n; i++) {
		u32 p = work[i];
		if (!(i & 7))
			video_through(e);	/* (a retrace waiting goes through, as in a capture's copies) */
		if (i < e->low || (hot[p >> 3] & (1 << (p & 7))))
			dma_page(k++, store_phys(t16(T_FREE)[base - i]), p << 12);
	}
	if (k)
		dma_wait();	/* (the game may write them as soon as this returns) */
	for (i = e->low; i < n; i++) {
		u32 p = work[i];
		if (hot[p >> 3] & (1 << (p & 7)))
			continue;
		page[m] = (u16)p;
		store[m] = t16(T_FREE)[base - i];
		pend[p >> 3] |= (u8)(1 << (p & 7));
		m++;
	}
	b->n = m;
	b->issued = b->unprot = b->ops = 0;
	b->active = m != 0;
	for (i = 0; i < m; i++)
		bg_set_pte(page[i], 1);
	tlb_kill(page, m);
	bg_issue(b);
	if (b->active)
		bg_dec_on(b);
	bg_mark(6, m);
}

#ifdef RB_TRACE
static void tr_off(void);
#endif

static void install(Engine *e, u32 wrapper)
{
	if (!e->installed && take_branch(EXI_BRANCH, wrapper, &e->orig_exi)) {
		e->installed = 1;
		/* The MEM2 scratch is the engine's only when the kernel says so: next to a big
		 * memory card it is the disc cache's or the card's own (ISO.c). And it outlives a
		 * reboot: whatever the last session, the Homebrew Channel or the loader left there
		 * is junk. Bg's left "active" fed copies of stale pages into a locked cache that
		 * was off from the online CSS on: a console frozen as Direct opened, every boot
		 * (hand test 2026-11-08, runs 20261108-212722 and -213057). So: ask (through the
		 * wrapper just installed: rb_exi feeds nothing yet), then reset what is read
		 * before a game's first frame. At boot nothing is hooked: the OS's code is fresh. */
		e->scratch_ok = tell_kernel(3);
		if (e->scratch_ok) {
			volatile Bg *b = bg();
			b->active = b->n = b->issued = b->unprot = b->ops = 0;
			b->hooked = b->dec_hooked = 0;
#ifdef RB_TRACE
			tr_off();	/* (the record's `on` too: the marker would go on every menu frame) */
#endif
		}
	} else if (take_branch(EXI_BRANCH, wrapper, &e->orig_exi)) {
		e->installed = 1;
	}
#ifndef RB_HOST
	/* the pad's way in first: nothing may stash a pad before it can be used */
	if (take_branch(INPUT_BRANCH, pad_shim_at(), &e->orig_input) && rd32(EARLY_HOOK) == EARLY_HOOK_INSN)
		write_code(EARLY_HOOK, branch_to(EARLY_HOOK, early_shim_at()));
	take_branch(FN_LOAD_SAVESTATE, load_shim_at(), &e->orig_load);
#endif
}

#ifndef RB_HOST
/* The inputs on time. Slippi sends a frame's input to the opponent from the
 * VI retrace's input renewal, and puts that renewal off while a rollback runs
 * (PreventPadAlarmDuringRollback.asm), until the main loop is back from
 * rendering. A rollback takes a PC a millisecond or two; on a Wii it reaches
 * past the next retrace (a 2-frame one: a load, then 3 frames and captures, 17-20
 * ms), so that frame's input left 2-8 ms late, and the PC player rolled back on it.
 *
 * So at the retrace put off (early_shim), this reads the pad, cleans it as
 * TriggerSendInput will (its sections 2 and 3), sends it to the kernel, which
 * sends it on, and keeps it: the renewal, when it comes, uses this same pad
 * (pad_shim), and the game goes on as ever. Its frame: the renewal's next one,
 * one past the rollback's end. Not in the first 2 s: the inputs are cleared until
 * the game's start, and the kernel may still make the game wait by time sync. */
#define SDA_BASE 0x804DB6A0u	/* r13 */
#define EARLY_FROM 120

static void clamp_stick(u8 *stick)
{
	if ((s8)stick[0] >= -2 && (s8)stick[0] <= 2 && (s8)stick[1] >= -2 && (s8)stick[1] <= 2)
		stick[0] = stick[1] = 0;
}

void rb_early_pad(Engine *e)
{
	u8 raw[TXB_SIZE + 32];
	u8 *buf = (u8 *)(((__UINTPTR_TYPE__)raw + 31) & ~(__UINTPTR_TYPE__)31), *pad;
	u32 odb = rd32(SDA_BASE + OFST_R13_ODB_ADDR);
	s32 frame = (s32)rd32(odb + ODB_ROLLBACK_END_FRAME) + 1;
	int i;
	if (e->early_valid || frame <= EARLY_FROM)
		return;	/* (a second retrace in one rollback waits for its renewal) */
	PADRead(e->early_pad);
	pad = e->early_pad + rd8(odb + ODB_INPUT_SOURCE_INDEX) * PAD_REPORT_SIZE;
	clamp_stick(pad + 2);
	clamp_stick(pad + 4);
	if ((s8)pad[0xA] == -3)	/* no new data: last frame's, as the opponent must get */
		for (i = 0; i < PAD_REPORT_SIZE; i++)
			pad[i] = rd8(odb + ODB_LAST_LOCAL_INPUTS + i);
	buf[0] = CMD_EARLY_PAD;
	for (i = 0; i < 4; i++)
		buf[TXB_FRAME + i] = (u8)((u32)frame >> (24 - 8 * i));
	buf[TXB_DELAY] = rd8(odb + ODB_DELAY_FRAMES);
	for (i = 0; i < PAD_REPORT_SIZE; i++)
		buf[TXB_PAD + i] = pad[i];
	FN_EXITransferBuffer(buf, TXB_SIZE, 1);
	e->early_valid = 1;
	e->early_frame = frame;
}

/* A wait for a late input. With the input here, a poll needs no prediction, so no
 * rollback: an input a moment late, on a good connection (or the opponent's frame a
 * moment behind), is waited for and nobody sees it, as with delay based netcode;
 * predicted, it costs a rollback the next frame. The game has the time: a frame's
 * work is 6-7 ms of the 16.7, and the third frame buffer takes what is left over.
 *
 * A frame that rolls back needs all its time. So only when this poll has no rollback due: no
 * input that came since the last poll differs from the one before it, which is what the game
 * predicted. (Without the check, at a 70 ms ping: a frame shown twice, then a jump, 34 times in
 * 80 s, each just after a wait. Without the waits: four times the rollbacks.)
 *
 * Only the game waits: the player's pad goes to the opponent first (0xE0, as
 * rb_early_pad), so what the opponent sees does not change.
 *
 * Only with the game idle: its last frame done, and no poll it has yet to start on. The
 * poll is in the retrace interrupt, and the game's thread waits with it. Taken from a
 * frame still running, the wait makes it later, so that the next poll lands in it too.
 * With a poll still queued the game may never get going (seen: 12 refreshes without a
 * frame, polls overflowing HSD's queue of 5, every input a frame off: a desync).
 *
 * Here and by the timebase, not in the kernel: a kernel sleep below IOS's network
 * module can last 10 ms more. A poll past the next retrace runs a second poll before
 * the game has taken the first: inputs a frame off for good, a desync (seen). From
 * frame 120: until then the kernel's time sync may hold the game back.
 *
 * With the inputs frames late, by the connection, waiting cannot help. So after
 * INPUT_WAIT_MISSES waits in a row for nothing, only one poll in 64 tries. Nor for an input
 * more than this frame's away: the frame before it has not come either, and at a high ping
 * every such wait is in vain (seen: 271 a game, none got it, each a frame's 5 ms gone).
 *
 * The poll runs in the retrace's interrupt, so with interrupts off, and the sound engine
 * runs from the AI's, every 5 ms: a wait of 5 ms on top of the poll leaves it a whole block
 * late, and the sound breaks up (seen: gaps of 10-17 ms on most frames at a 30 ms ping). So
 * the devices' interrupts are let through while it waits. They nest as the OS allows (each
 * handler has its own context, and the scheduler is off in them), and none comes back here.
 * The alarms are held off, as an alarm may renew the pad, here in the middle of renewing it. */
#define INPUT_WAIT_FROM 120
/* With the pad read before the retrace, the rest of the poll after a wait (the 0xB0, Slippi's own
 * work) runs with interrupts off, about 0.7 ms and up to 2: a wait that ends nearer the retrace than
 * that holds it past the vertical blank, and the frame goes up a refresh late (seen: 28 of 41 late
 * retraces at 30 ms ping, after waits of 4-5 ms). A frame predicted instead costs nothing seen. */
#define INPUT_WAIT_BEFORE_RETRACE_US 2500
#define INPUT_WAIT_MISSES 8
#define PAD_QUEUE_COUNT 0x804C1F7Bu	/* HSD_PadLibData.qcount (HSD_PadGetRawQueueCount) */
#define MSR_EE 0x8000u

static void input_wait(Engine *e, const u8 *b0)
{
	u32 most = *(volatile u32 *)(0xD0000000u | (RB_SCRATCH + T_INPUT_WAIT));
	const volatile u32 *head = (const volatile u32 *)(0xD0000000u | (RB_SCRATCH + T_REMOTE_HEAD));
	s32 frame = (s32)be32(b0 + TXB_FRAME);
	u8 raw[TXB_SIZE + 32];
	u8 *buf = (u8 *)(((__UINTPTR_TYPE__)raw + 31) & ~(__UINTPTR_TYPE__)31);
	u32 t0, ticks, msr, mask;
	s32 seen = e->head_seen;
	int i;
	e->head_seen = (s32)*head;
	if ((s32)*SCRATCH_WORD(T_REMOTE_CHANGED) > seen)
		return;
	/* A poll from the game's own thread, one put off by a rollback: the pad's line interrupt could
	 * come in the wait below, and run a second poll of this frame inside it (seen: frame 135 polled
	 * 3 times in 11 ms, and a desync two frames on). */
	if (e->poll_cb && !e->in_poll)
		return;
	if (!most || e->busy || rd8(PAD_QUEUE_COUNT) || frame <= INPUT_WAIT_FROM || frame == e->early_frame || (s32)*head >= frame ||
	    (s32)*head < frame - 1 || (e->wait_misses >= INPUT_WAIT_MISSES && (frame & 63)))
		return;
	for (i = 0; i < TXB_SIZE; i++)
		buf[i] = b0[i];
	buf[0] = CMD_EARLY_PAD;
	((void (*)(u8 *, int, int))e->orig_exi)(buf, TXB_SIZE, 1);
	t0 = (u32)timebase();
	if (e->poll_cb && e->in_poll && e->last_retrace_tb) {
		u32 since = (t0 - e->last_retrace_tb) / 61, left;
		left = since < FIELD_US ? FIELD_US - since : 0;
		left = left > INPUT_WAIT_BEFORE_RETRACE_US ? left - INPUT_WAIT_BEFORE_RETRACE_US : 0;
		if (most > left)
			most = left;
	}
	most *= 61;	/* 60.75 ticks a microsecond */
	/* The sound's interrupts go on through this. The retrace's may not: its poll, nested
	 * in this one, is a whole frame's input with this frame's number, and when this one
	 * goes on, the same number again. The game's frames then run ahead of its polls, every
	 * input after that is sent for the wrong frame: a desync, or a savestate asked for that
	 * was never taken. (A poll here need not be near its retrace: one put off by a rollback
	 * comes from the game's own thread, at any time.) */
	msr = irq_off();
	set_dec(0x7FFFFFFF);
	/* (With the pad read before the retrace, poll_early_on, the retrace has no poll in it and may
	 * come through. It must: handled past the vertical blank, the frame it puts up shows a refresh
	 * late. The pad's own line interrupt does not come round again in a wait.) */
	mask = e->poll_cb ? OS_INTERRUPTMASK_PI_VI : OS_MASK_INTERRUPTS(OS_INTERRUPTMASK_PI_VI);
	irq_restore(msr | MSR_EE);
	do
		ticks = (u32)timebase() - t0;
	while ((s32)*head < frame && ticks < most);
	irq_restore(msr);
	if (!(mask & OS_INTERRUPTMASK_PI_VI))
		OS_UNMASK_INTERRUPTS(OS_INTERRUPTMASK_PI_VI);
	set_dec(0x100);	/* soon: the OS looks at the alarm queue's head again, sets its timer */
	e->input_waits++;
	e->head_seen = (s32)*head;	/* (an input that came for this poll is not predicted) */
	if ((s32)*head >= frame) {
		e->input_arrived++;
		e->wait_misses = 0;
	} else {
		e->wait_misses++;
	}
	e->input_wait_ticks += ticks;
	if (ticks > 4000 * 61)
		TR(8, (s32)*head >= frame, ticks / 61);
}
#endif

#ifndef RB_HOST
/* A trip over the EXI is a wait for the kernel's main loop: about 1 ms a trip, and half of a
 * frame's 5 ms of game work (`rb exi`). A rollback's frame must be ready for its refresh too,
 * so what needs no such wait does not wait.
 *
 * Replay data needs no answer. It goes in a ring in MEM2, for the kernel's main loop to take
 * when it comes round (so_exi_replay_next), in the same order. */

static void replay_queue(Engine *e, const u8 *buf, u32 len)
{
	volatile u32 *to = SCRATCH_WORD(T_REPLAY);
	u32 head = e->replay_head, at = head % REPLAY_SIZE, need = 4 + ((len + 3) & ~3u), i;
	u32 skip = at + need > REPLAY_SIZE ? REPLAY_SIZE - at : 0;
	while (REPLAY_SIZE - (head - *SCRATCH_WORD(T_REPLAY_TAIL)) < skip + need)
		;
	if (skip) {
		to[at / 4] = REPLAY_WRAP;
		head += skip;
		at = 0;
	}
	to[at / 4] = len;
	for (i = 0; i < (len + 3) / 4; i++)
		to[at / 4 + 1 + i] = rd32((u32)(__UINTPTR_TYPE__)buf + i * 4);
	e->replay_head = head + need;
	__asm__ volatile("sync");
	*SCRATCH_WORD(T_REPLAY_HEAD) = e->replay_head;
}

/* The 0xB0's answer is ready by the time the kernel acknowledges the 0xB0, so a second trip
 * only to fetch it is a wait for nothing: the kernel leaves it in MEM2 too (publish_answer).
 * 1: it did for this poll, and it is in buf. */
static int answer_ready(Engine *e, u8 *buf, int len)
{
	const volatile u32 *from = SCRATCH_WORD(T_ANSWER);
	s32 frame = e->poll_frame;
	int i;
	e->poll_frame = 0;
	if (!frame || (s32)from[0] != frame)
		return 0;
	for (i = 0; i < len / 4; i++)
		*(u32 *)(buf + i * 4) = from[8 + i];
	if (len & 3) {
		u32 last = from[8 + i];
		for (i = len & ~3; i < len; i++)
			buf[i] = (u8)(last >> (24 - 8 * (i & 3)));
	}
	return 1;
}

static void poll_early_track(Engine *e);
#endif

/* Slippi's visual buffer fix (Binary/LagReduction/PD+VB.bin): when a frame had to wait for a
 * free frame buffer, this flag (r2-0x3EB0) makes the next engine loop skip its render. On a PC
 * Normal Lag Reduction jumps over the code that sets it; here it's live, and it means "behind".
 * With the pad read before the retrace a frame that's just fast waits too, and each such frame
 * cost a frame shown twice, then a jump. So the skip stays only when the game really is behind:
 * another poll already waiting when the wait is over. */
#define VB_FLAG 0x804DBB30u

#ifndef RB_HOST
static void vb_keep_only_if_behind(Engine *e)
{
	if (e->poll_cb && (s32)rd32(VB_FLAG) == -1 && !rd8(PAD_QUEUE_COUNT)) {
		*(volatile u32 *)VB_FLAG = 0;
		e->vb_cleared++;
	}
}
#endif

/* (From here to direct_after_load the PC's test compiles too: tools/rollback/test, test_direct.) */

/* Frames on the screen a refresh sooner.
 *
 * A frame polled a little before a retrace is drawn after it, copied to the free frame buffer and
 * shown at the next retrace: a refresh and the poll's lead from the pad to the screen. One frame
 * drawn too late for that (a deep rollback, a load) is shown a refresh late, and so is every frame
 * after it: Melee has two frame buffers, and with one on the screen and one waiting its turn, each
 * frame's copy waits for the retrace, and its work starts when the copy before it is done. The
 * game stays there, a refresh behind (measured with tools/latency_probe: 105 of 106 presses of a
 * game at a 30 ms one-way delay, 121 of 121 at 15 ms; 74.7 ms from the pad to the screen with 2
 * delay frames, where 58 was meant). Being behind is smooth: a frame has up to two refreshes
 * before another is shown twice. It costs the player 16.7 ms.
 *
 * One frame not drawn brings the game level again: the next one starts at its poll. Slippi's
 * visual buffer fix does that when its flag is set (the next frame outside a rollback: a
 * rollback's loops go round its check), so this sets the flag, again after each frame drawn and
 * each savestate loaded (rb_exi: the flag is in them) until it is taken (count_frame). On the
 * screen that is one frame left out, once.
 *
 * When: in the countdown or not at all. A skip changes how soon the game answers its player, by a
 * whole frame: in the frozen countdown that is nothing, in play it is the timing a player's hands
 * have just settled on. So a skip is asked only before DIRECT_LAST (players move from tick 84), and
 * after a frame late while level (the one before it shown twice) the game stays a refresh behind to
 * its end: the lag a game starts with is the lag it keeps, but for that one step. (T_DIRECT 2, for
 * tests: level whatever comes, a skip whenever a frame is found behind.)
 *
 * And only when the game's frames will fit. Level, a frame has its poll's lead and a refresh, less
 * the GPU's part and the poll's own: about 19.5 ms of work. One frame run again fits (11-16 ms); two
 * take 13-21, by the ping and by the game: at a 100 ms ping (2026-11-10, level whatever came, the
 * saves not in the background) 3 games of 8 had them at 13.5 ms and no frame late, the other 5 at
 * 15-17 ms and 1-17 frames late (by the characters: Peach, Luigi, Kirby, Mario, the Ice Climbers
 * the dearer); at 83 ms 7 of 8 none late; with 1 delay frame at 70 ms, 12 frames shown twice in 2.5
 * minutes. A ping does not say which, so the engine counts each game's passes over DIRECT_FIT_US,
 * level or behind (behind, a pass over a refresh is all work: the frame buffer it waits for is free
 * by its end; not the tick the pad's read moves at, DIRECT_FROM, whose pass is two refreshes long
 * by the move), and keeps a number for the opponent from game to game (direct_end): half of what
 * it was, two for each such pass of the game just played, DIRECT_FIT_ENDED more if a run level
 * ended in its play, and as much again if more than one in DIRECT_FIT_DEEP of its passes had 3
 * loops or more. No skip with that over DIRECT_FIT_MOST: one such pass in a game leaves the next
 * one level, two do not, and a game that lost its level run, or was one of deep rollbacks, is
 * followed by two behind, more if those are no better. (It was the passes of the last 512-1024
 * ticks: 2026-11-10, a game with 12 of them, its last seconds calm, was followed by one that asked
 * and lost the run in its first second of play; and the pass of tick 40, counted, was all that
 * count allowed. The deep rollbacks: at a 100 ms ping every other frame is a rollback of two, and
 * a level run was lost in 2 games of 4 that had no pass too long before it: a pass of 21.6 ms in
 * time, one of 18 ms three ticks on late. Passes of 3 loops or more, a game: 0-2% of them at
 * 70 ms, 0.1-3.9% at 83, where 6 games of 6 kept their runs; 4-26% at 100, and 12-31% with 1
 * delay frame at 70.) Nor a skip
 * in a game with more than DIRECT_FIT_GAME of them by then (the second, at DIRECT_SETTLED). The
 * kernel's word says the rest (so_exi.c, direct_wanted): 1, under its ping limit: the skip unless
 * the number is against it; 3, over it or the ping unknown: the skip only if this opponent's last
 * game was counted and fits; bit 9, the first game against this opponent: the number starts
 * again, and with 3 there is no skip.
 *
 * While the engine has the game level, its savestates are not copied in the background (capture):
 * that is how level has been run from 80 ms up, where the kernel has those copies on (12 games at
 * 83 ms, 2026-11-10, none with a frame late). (For an hour that day the two together were kept
 * apart altogether, and input_wait out of games with the copies: a Wii had left the network in the
 * middle of a game four times, the first three at an 83 ms ping. It was a cable in an Ethernet
 * splitter.) */
#define DIRECT_FIT_US 19000
#define DIRECT_FIT_MOST 2	/* of the opponent's number */
#define DIRECT_FIT_ENDED 8
#define DIRECT_FIT_DEEP 16	/* a game of deep rollbacks: more than one pass in this many of 3 loops or more */
#define DIRECT_FIT_LEAST 600	/* passes: a shorter game says nothing */
#define DIRECT_FIT_GAME 1	/* of this game's own passes */
#define DIRECT_FROM 40		/* (POLL_EARLY_FROM) */
#define RETRACE_COUNT 0x804D7420u
#define DIRECT_START 8		/* frames drawn behind, from the early poll's start (frame 40): the skip in the frozen
				 * countdown, where a frame left out is not seen (players can move from 84) */
#define DIRECT_SETTLED 125	/* the kernel's first time sync may hold the game back until frame 120: a run that
				 * ends before this is tried again here, once */
#define DIRECT_LAST 160		/* no skip is asked from here on: the game is being played */

#ifdef RB_DIRECT_LOG
/* Tests (build_online_boot.py --latency): what the first runs level ended on, for the report
 * (tools/latency/analyze.py reads it). T_DIRECT_LOG: how many, then 4 words each: what (1 a run
 * level ended, 2 a skip asked, 5 a pass over DIRECT_FIT_US; 3 and 4 were a test's)
 * << 28 | loops << 20 | frame, the pass's busy time and the time
 * since its poll (timebase ticks), the run << 16 | retraces since the poll << 8 | polls queued.
 * In T_PROF's place (a trace build's; cleared at each game's start), through the cache as all
 * of it: not beside T_GAME_FRAMES' words, which the kernel shares and nothing caches (a cached
 * line written back late would put an old T_POLL_HW over a new one). */
#define T_DIRECT_LOG (T_PROF + 0x700)
#define DIRECT_LOGS 15	/* (to the end of T_PROF's 2 KB) */
static void direct_log(Engine *e, u32 what)
{
	u32 *log = t32(T_DIRECT_LOG), now = (u32)timebase();
	if (log[0] < DIRECT_LOGS) {
		u32 *r = log + 1 + log[0]++ * 4;
		r[0] = what << 28 | e->pass_loops << 20 | e->game_frames;
		r[1] = now - e->pass_start;
		r[2] = now - e->pass_poll_tb;
		r[3] = (u32)e->direct_run << 16 | ((rd32(RETRACE_COUNT) - e->pass_poll_rc) & 0xFF) << 8 | rd8(PAD_QUEUE_COUNT);
	}
}
#else
#define direct_log(e, what) ((void)0)
#endif

static void direct_track(Engine *e)
{
	u32 mode = e->poll_cb ? *SCRATCH_WORD(T_DIRECT) & 0xFF : 0;	/* (poll_cb: in a game the engine runs) */
	/* (in a game the engine runs, after the tick its pad's read moved at) */
	if (e->poll_cb && e->game_frames > DIRECT_FROM && ((u32)timebase() - e->pass_start) / 61 > DIRECT_FIT_US) {
		if (e->deep_game < 0xFFFF)
			e->deep_game++;
		direct_log(e, 5);
	}
	/* (the last frame drawn: level, or a refresh behind) */
	e->direct_is = (u8)((e->direct_is & 9) | (e->poll_cb && (u16)(rd32(RETRACE_COUNT) - e->pass_poll_rc) < 2 ? 4 : 0));
	if (!mode) {
		/* (the kernel's word gone to 0 with a skip asked and not taken: the ping, measured in the
		 * countdown, is too far over its limit. Not asked any more.) */
		if (e->direct_skip) {
			*(volatile u32 *)VB_FLAG = 0;
			e->direct_is &= 0xE;
		}
		e->direct_skip = 0;
		return;
	}
	if (e->direct_skip) {	/* (asked: not taken yet) */
		/* Level by itself meanwhile: rollbacks came first (their loops go round the check), one ran over
		 * 25 ms and left two polls waiting, and Melee's loop ran both frames and drew one. Nothing is
		 * left to leave out: a second frame not drawn would only be a frame not drawn. */
		if (e->direct_is & 4) {
			e->direct_skip = 0;
			e->direct_run = 0;
			*(volatile u32 *)VB_FLAG = 0;
			return;
		}
		*(volatile u32 *)VB_FLAG = 0xFFFFFFFFu;
		return;
	}
	if (e->direct_run < 0xFFFF)
		e->direct_run++;
	if ((u16)(rd32(RETRACE_COUNT) - e->pass_poll_rc) < 2)
		return;	/* level: copied before the second retrace after its poll */
	if (e->direct_behind < 0xFFFF)
		e->direct_behind++;
	if (e->direct_hold) {
		e->direct_hold--;
		return;
	}
	if ((e->direct_is & 1) && mode != 2) {	/* a run level has ended */
		direct_log(e, 1);
		if (e->direct_overruns < 0xFF)
			e->direct_overruns++;
		e->direct_is &= 0xE;
		if (e->game_frames < DIRECT_SETTLED) {	/* (the start's time sync: once more, there) */
			e->direct_hold = (u16)(DIRECT_SETTLED - e->game_frames);
			return;
		}
		e->direct_is |= 8;	/* behind to the game's end */
		return;
	}
	/* (the word as it is now: a new connection's ping is first measured as the game starts, and the
	 * kernel's 3 may be 1 a few frames on) */
	if (mode != 2 && ((e->direct_is & 8) || e->game_frames >= DIRECT_LAST || e->deep_carry > DIRECT_FIT_MOST || e->deep_game > DIRECT_FIT_GAME ||
	                  (mode == 3 && (*SCRATCH_WORD(T_DIRECT) & 0x200))))
		return;	/* (behind, this game, or not yet) */
	direct_log(e, 2);
	e->direct_skip = 1;
	e->direct_is |= 1;
	e->direct_run = 0;
	if (e->direct_skips < 0xFF)
		e->direct_skips++;
	*(volatile u32 *)VB_FLAG = 0xFFFFFFFFu;
}

/* (Two waiting polls run one a loop while the game is meant to be a refresh behind, so that Melee's
 * loop does not catch up by running both and drawing one: a test, the kernel's test_one_a_loop,
 * 2026-11-10, taken out again. Its case, two polls a refresh apart, never came, at 1 delay frame and
 * 70 ms or at 2 and 100: a poll that Slippi puts off during a rollback is taken at the rollback's
 * end, a millisecond before the next one, and by their times those two are the kernel's own advance
 * of a frame, which must run both.) */

/* A game's start (poll_early_on, tick 40). By the kernel's word the opponent's number starts again:
 * another opponent's games say nothing about this one's. */
static void direct_begin(Engine *e)
{
	if (*SCRATCH_WORD(T_DIRECT) & 0x200)
		e->deep_carry = 0;
	e->direct_skip = e->direct_is = 0;
	e->direct_hold = DIRECT_START;
	e->deep_game = e->direct_run = e->direct_skips = e->direct_overruns = e->direct_behind = 0;
}

/* A game's end (rb_frame, its report made and its passes still counted): the opponent's number with
 * this game in it (direct_track's comment). A game too short to say leaves it as it was: an opponent
 * who left in the countdown has not shown that the frames fit. */
static void direct_end(Engine *e)
{
	u32 all = 0, deep = e->pass_n[2] + e->pass_n[3] + e->pass_n[4], carry, i;
	for (i = 0; i < 5; i++)
		all += e->pass_n[i];
	if (all >= DIRECT_FIT_LEAST) {
		carry = (e->deep_carry >> 1) + 2 * (u32)e->deep_game + ((e->direct_is & 8) ? DIRECT_FIT_ENDED : 0) +
		        (deep * DIRECT_FIT_DEEP > all ? DIRECT_FIT_ENDED : 0);
		e->deep_carry = carry > 0xFFFF ? 0xFFFF : (u16)carry;
	}
	e->deep_game = 0;
	e->direct_is = 0;
}

/* A savestate is loaded (rb_exi). direct_track's flag is game memory, so in the savestates, and is now
 * what it was at that frame: a skip asked since then was gone (and count_frame read that as taken: 10
 * of 23 skips at a 70 ms ping, 7 of 9 with 1 delay frame, each a run "level" that never was); one
 * asked before it and taken since would be taken again. */
static void direct_after_load(Engine *e)
{
	if (e->poll_cb && rd32(VB_FLAG) != (e->direct_skip ? 0xFFFFFFFFu : 0))
		*(volatile u32 *)VB_FLAG = e->direct_skip ? 0xFFFFFFFFu : 0;
}

#ifndef RB_HOST
/* The frame's render is built (frame_end.s). */
void rb_render_built(Engine *e)
{
#ifdef RB_WALK
#ifdef RB_DIRECT_LOG
	walk_check(e, 3, 0);	/* (beside the probe, for 4 bytes of room: the kernel's record has the frames) */
#else
	walk_check(e, 3, e->game_frames);
#endif
#endif
	if (!e->direct_skip)
		vb_keep_only_if_behind(e);
	direct_track(e);
	if (e->poll_cb)
		poll_early_track(e);
	e->built_poll_tb = e->pass_poll_tb;
	e->busy = 0;
}
#endif

/* The two lines the pad is read at, `us` before each of a frame's two retraces. `second`: the line the
 * frame's second field starts at, where its first retrace comes (DI0's: 263 in 480i, 526 in 480p); the
 * other retrace is at line 1, after the frame's last line. */
static void poll_line_pair(u32 second, u32 us, u32 *first_read, u32 *second_read)
{
	u32 lines;
	if (second < 300) {	/* 480i: a field 262.5 lines */
		lines = us * second / FIELD_US;
		*second_read = second * 2 - 1 - lines;
	} else {		/* 480p: a field 525 lines, half as long each */
		lines = us * (second - 1) / FIELD_US;
		*second_read = (second - 1) * 2 + 1 - lines;
	}
	*first_read = second - lines;
}

#ifndef RB_HOST
/* The pad read a little before the retrace.
 *
 * Melee reads the pad at the retrace (the polling drift fix's post-retrace callback), so a
 * frame has one refresh until it is on screen. Most take 8-15 ms, but a rollback's take up to
 * about 21 ms on a Wii, and a frame not drawn by its refresh is shown a refresh late, the frame
 * before it twice, then the next one skipped. The poll need not be at a retrace: nothing ties
 * them but this callback. So the poll goes to a line interrupt of its own (VI's third and
 * fourth, DI2 and DI3, which the OS's handler passes over), and a frame has one refresh and
 * this much. The player's lag grows by this much, not by a refresh, as with another frame
 * buffer (tried: every frame 33 ms from poll to screen, 22 with this, and as smooth).
 *
 * How much: T_POLL_EARLY while there are rollbacks (poll_early_track). A load runs with
 * interrupts off from just after the poll, and if the retrace falls in it, the frame the retrace
 * puts up is shown a refresh late, past the vertical blank. With no rollbacks, as on a LAN, it
 * starts at and comes back down to what the frames need, and the player has that much less lag.
 * A change takes no frame shown twice or skipped: the time between two polls is a little longer
 * or shorter.
 *
 * A fast frame can be drawn before the coming retrace. Shown at it, it would be a refresh sooner
 * than a slow frame next to it, and the picture would go back and forth between the two (seen:
 * hundreds of frames shown twice, then a jump, in a game). So a frame whose poll came after the
 * last retrace waits for the next one (vi_retrace). (This went by whether an older frame was still
 * to be shown at the poll. Often so of a frame that then finished in time, and the next frame through
 * was shown early, a repeat a minute or so at a 70 ms ping.)
 *
 * VI counts lines over a frame of two fields, each field a refresh with its own retrace: 1-525 in
 * 480i (retraces at lines 263 and 1), 1-1050 in 480p, where a field is a whole picture of 525 lines
 * (retraces at 526 and 1). So a read before every retrace is two line interrupts in both. (Until
 * 1.5.0 480p was taken for one field a count, with one read: a player's 480p games ran at half
 * speed, a frame every other refresh, 2026-11-11.)
 *
 * The controller answers the SI when the SI polls it, not when the game reads. Melee's setting
 * polls twice a field, at lines 131 and 262, so a read before the retrace gets an answer about
 * 8 ms old. Moving the SI's lines doesn't last: every frame flip sets them again from the sampling
 * rate (__VIRetraceHandler, SIRefreshSamplingRate). So that's what changes: the SDK's 1 ms setting,
 * 18 polls a field, an answer never more than a millisecond old whenever the game reads.
 *
 * From frame 40, in the frozen countdown, as the change of timing costs a repeat (at frame 120, one in every game; players can move from 84). The first frames, the kernel's first time sync, are as ever. Neither the
 * handler's table nor HSD_VIData is in the savestates. */
#define VI_POST_CB 0x804C1F5Cu		/* HSD_VIData.post_cb */
#define VI_DI0 ((volatile u16 *)0xCC002030u)
#define VI_DI2 ((volatile u16 *)0xCC002038u)
#define VI_DI3 ((volatile u16 *)0xCC00203Cu)
#define SI_SAMPLING_RATE 0x804D740Cu	/* SamplingRate (in the savestates' excludes: pad .sbss) */
#define SI_SAMPLING_FRESH 1		/* ms */
#define VI_INT 0x8000u
#define VI_INT_ENABLE 0x1000u
#define OS_INTERRUPT_PI_VI 24
#define OS_INTERRUPT_PI_PE_FINISH 19
#define VI_NUDGING 0x8000u	/* in video_through: as the kernel last asked */
#define VI_HELD 0x4000u		/* and: this retrace passes over a frame from its own poll */
#define VI_OLDER 0x1000u	/* and, two bits by buffer: holding an older frame at this frame's start */
#define OS_DISABLE_SCHEDULER ((s32 (*)(void))0x8034AD48u)
#define OS_ENABLE_SCHEDULER ((s32 (*)(void))0x8034AD88u)
#define OS_CURRENT_CONTEXT 0x800000D4u
#define PI_CAUSE ((volatile u32 *)0xCC003000u)
#define PI_MASK ((volatile u32 *)0xCC003004u)
#define PI_VI 0x100u
#if 0
/* (diagnostics) the VI's state now, as an event: which 0xF5 load in, 0xF6 load out, 0xF7 capture
 * in. b: its line << 16, then DI0..DI3 pending (bits 0-3), the PI's VI cause and mask (4, 5), and
 * interrupts on (6) */
static void tr_vi_state(u32 which)
{
	u32 msr;
	__asm__ volatile("mfmsr %0" : "=r"(msr));
	TR(0, which, (u32)(*(volatile u16 *)0xCC00202Cu & 0x7FF) << 16 | !!(VI_DI0[0] & VI_INT) | !!(VI_DI0[2] & VI_INT) << 1 |
	   !!(VI_DI2[0] & VI_INT) << 2 | !!(VI_DI3[0] & VI_INT) << 3 | !!(*PI_CAUSE & PI_VI) << 4 | !!(*PI_MASK & PI_VI) << 5 |
	   !!(msr & 0x8000) << 6);
}
#endif
#define PI_PE_FINISH 0x400u
#define OS_SET_INTERRUPT_HANDLER ((u32 (*)(s32, u32))0x803473B0u)
#define OS_CLEAR_CONTEXT ((void (*)(void *))0x80345254u)
#define OS_SET_CURRENT_CONTEXT ((void (*)(void *))0x8034508Cu)
#define VI_DATA 0x804C1D80u
#define VI_XFB(i) (VI_DATA + 0x58 + (i) * 0x60)	/* buffer, status, vi_all (0x58 bytes) */
#define VI_XFB_WAITDONE 4
#define VI_XFB_DRAWDONE 5
#define VI_XFB_NEXT 6
#define POLL_EARLY_FROM 40
typedef char direct_from_is_the_polls[DIRECT_FROM == POLL_EARLY_FROM ? 1 : -1];
#define POLL_EARLY_LEAST 2000	/* us, with no rollbacks */
#define POLL_RENDER_TO_SCREEN 3500	/* the GPU's part, after the CPU has built the render */
#define POLL_SPARE 500
#define POLL_DOWN_STEP 1000
#define POLL_CALM_FRAMES 120

static void poll_lines(Engine *e)
{
	u32 first_read, second_read;
	poll_line_pair(VI_DI0[0] & 0x7FF, e->poll_us, &first_read, &second_read);
	VI_DI2[1] = 1;
	VI_DI2[0] = (u16)(VI_INT_ENABLE | first_read);
	VI_DI3[1] = 1;
	VI_DI3[0] = (u16)(VI_INT_ENABLE | second_read);
}

static void poll_early_on(Engine *e)
{
	u32 us = *SCRATCH_WORD(T_POLL_EARLY), msr;
	if (!us || e->poll_cb || !rd32(VI_POST_CB))
		return;
	msr = irq_off();
	if (!e->orig_vi)
		e->orig_vi = OS_SET_INTERRUPT_HANDLER(OS_INTERRUPT_PI_VI, vi_shim_at());
	if (!e->orig_pe) {
		e->orig_pe = OS_SET_INTERRUPT_HANDLER(OS_INTERRUPT_PI_PE_FINISH, 0);
		OS_SET_INTERRUPT_HANDLER(OS_INTERRUPT_PI_PE_FINISH, e->orig_pe);
	}
	e->poll_cb = rd32(VI_POST_CB);
	*(volatile u32 *)VI_POST_CB = 0;
	e->si_rate = rd32(SI_SAMPLING_RATE);
	*(volatile u32 *)SI_SAMPLING_RATE = SI_SAMPLING_FRESH;
	e->poll_us = e->poll_next = POLL_EARLY_LEAST;
	e->sync_lead = e->vi_burst = 0;
	e->poll_need = e->poll_calm = e->poll_loaded = e->vb_cleared = 0;
	direct_begin(e);
	e->pass_poll_tb = e->built_poll_tb = e->last_retrace_tb = 0;
	e->video_through = 0;
	poll_lines(e);
	irq_restore(msr);
}

static void vi_nudge_end(Engine *e);

static void poll_early_off(Engine *e)
{
	u32 msr = irq_off();
	if (e->poll_cb) {
		VI_DI2[0] = 0;
		VI_DI3[0] = 0;
		vi_nudge_end(e);
		*(volatile u32 *)SI_SAMPLING_RATE = e->si_rate;
		*(volatile u32 *)VI_POST_CB = e->poll_cb;
		e->poll_cb = 0;
	}
	irq_restore(msr);
}

/* A frame's render is built: what it needed, from its poll until it is on screen */
static void poll_early_track(Engine *e)
{
	u32 need = ((u32)timebase() - e->poll_tb) / 61 + POLL_RENDER_TO_SCREEN + POLL_SPARE, floor, want;
	want = need > FIELD_US ? need - FIELD_US : 0;
	/* (no further for a frame that needs more: tried, up to 12 ms, and at a 90 ms ping far more frames
	 * were shown twice, not fewer: the fast frames among them are done too soon) */
	if (e->poll_loaded || want > (*SCRATCH_WORD(T_POLL_EARLY) & 0xFFFF))
		want = *SCRATCH_WORD(T_POLL_EARLY) & 0xFFFF;
	/* Rollbacks of 2 frames or more take a little over a refresh each, so a few in a row eat into
	 * this; more time, for the player's lag only while they last (at 90 ms ping: 30 repeats a
	 * minute at 5.5 ms, 5 at 8; a read 10 ms early, far too many fast frames waiting). */
	if (e->poll_loaded > 1)
		want += *SCRATCH_WORD(T_POLL_EARLY) >> 16;
	if (want > e->poll_need)
		e->poll_need = want;
	e->poll_loaded = 0;
	if (want > e->poll_next) {
		e->poll_next = want;
		e->poll_calm = 0;
		return;
	}
	if (++e->poll_calm < POLL_CALM_FRAMES)
		return;
	floor = POLL_EARLY_LEAST;
	want = e->poll_need > floor ? e->poll_need : floor;
	if (want < e->poll_next)
		e->poll_next = e->poll_next - want > POLL_DOWN_STEP ? e->poll_next - POLL_DOWN_STEP : want;
	e->poll_need = e->poll_calm = 0;
}

/* The Wii running a little fast, for a while, to stay level with its opponent. Slippi Dolphin
 * stays level by running up to 1% faster when behind; a Wii runs a frame a refresh, and advancing
 * a whole frame costs far more than the frame: the opponent, now behind, takes 2 s to catch up, and
 * all that time every input reaches the Wii a frame later (seen: every burst of 2-frame rollbacks
 * at a 70 ms ping starts at one of these). So a frame ends a line early: the blanking after the
 * picture (PSB, in half lines) a line shorter for one frame. No line changes length and the picture
 * doesn't move against the vertical sync, so a display has nothing to lose its lock on (tried: lines
 * a tick shorter, and a capture card lost the picture each time). At most one a frame: a line of 480i
 * is 63 us, 2 ms a second. In 480i it's the second field, so the frame's first retrace (at its middle
 * line) comes as ever; the other is at line 1, wherever the frame ended.
 *
 * Never in 480p: there a field is a whole picture, and one of 524 lines is a signal a digital display
 * may answer with a black screen while it finds the picture again (a player's 480p games "blacked
 * out", 2026-11-11, with a field short at nearly every refresh; nobody here can see a 480p picture to
 * say what a display forgives). In 480p the Wii stays level by the pad's earlier read alone, and by
 * the kernel's time sync when that is not enough. */
#define VI_PSB_ODD ((volatile u16 *)0xCC00200Cu)
#define VI_PSB_EVEN ((volatile u16 *)0xCC002010u)
#define VI_LINE ((volatile u16 *)0xCC00202Cu)
#define VI_PSB_LINE 2

static void vi_nudge_end(Engine *e)
{
	if (e->vi_saved) {
		*VI_PSB_ODD = (u16)(e->vi_saved >> 16);
		*VI_PSB_EVEN = (u16)e->vi_saved;
		e->vi_saved = 0;
	}
}

/* Or first (the kernel's 3), the pad read earlier, a line a frame, as long as that's no more than
 * POLL_SYNC_MOST before the retrace. The opponent and the kernel's time sync go by when the pad is
 * sent, not by the picture, so that's as good as a frame ending early for staying level, and the
 * picture's timing never changes: a capture card showed a frame twice and then skipped one some
 * 10-25 frames after nearly every run of early-ending frames (2026-11-07). The cost is the
 * player's: their pad that much older when the frame is on screen. The read stays at least that
 * early for the rest of the game (sync_lead), so poll_early_track's coming back down, as the
 * rollbacks stop, does not put the Wii behind again. */
#define POLL_SYNC_MOST 8000	/* us, unless the kernel says (a read 10 ms early: rb LOST, once) */
#define POLL_SYNC_STEP 64	/* us: a line of 480i */

static u32 poll_target(Engine *e)
{
	return e->poll_next > e->sync_lead ? e->poll_next : e->sync_lead;
}

static void vi_nudge(Engine *e)
{
	u32 middle = VI_DI0[0] & 0x7FF, line = *VI_LINE & 0x7FF, want = *SCRATCH_WORD(T_VI_NUDGE), lead;
	vi_nudge_end(e);
	if (!(want & 1) != !(e->video_through & VI_NUDGING)) {
		e->video_through ^= VI_NUDGING;
		TR(0, e->video_through & VI_NUDGING ? 0xFE : 0xFD, e->poll_frame);
		if (!(e->video_through & VI_NUDGING)) {
			TR(0, 0xF3, (u32)e->vi_burst << 16 | e->sync_lead);
			e->vi_burst = 0;
		}
	}
	if (!(want & 1))
		return;
	lead = poll_target(e);
	if ((want & 2) && lead + POLL_SYNC_STEP <= (want >> 16 ? want >> 16 : POLL_SYNC_MOST)) {
		e->sync_lead = (u16)(lead + POLL_SYNC_STEP);
		return;
	}
	if ((want & 4) || middle >= 300)
		return;	/* (tests: the pad read earlier only; and 480p, always) */
	if (line < middle || line > middle + 128)
		return;
	if (*VI_PSB_ODD < VI_PSB_LINE || *VI_PSB_EVEN < VI_PSB_LINE)
		return;
	e->vi_saved = (u32)*VI_PSB_ODD << 16 | *VI_PSB_EVEN;
	*VI_PSB_EVEN = (u16)(*VI_PSB_EVEN - VI_PSB_LINE);
	if ((e->video_through & 0xFFF) != 0xFFF)
		e->video_through++;
	if (e->vi_burst != 0xFFFF)
		e->vi_burst++;
}

/* The retrace, in the OS's handler */
static void vi_retrace(Engine *e, s32 exception, void *context)
{
	s32 held = -1, i;
	/* The frame waiting: an older one (there at the start of this frame's work), or this frame's,
	 * drawn before its work is over and marked built. By age, as the timebase's low word wraps in
	 * 70 s: a stamp from long ago is never newer. Never two running: a frame held at one retrace
	 * goes at the next, whatever its stamp says. */
	if (e->poll_cb && !(e->video_through & VI_HELD))
		for (i = 0; i < 2; i++)
			if (rd32(VI_XFB(i) + 4) == VI_XFB_NEXT) {
				u32 stamp = e->busy && !(e->video_through & (VI_OLDER << i)) ? e->pass_poll_tb : e->built_poll_tb;
				if ((u32)timebase() - stamp < (u32)timebase() - e->last_retrace_tb)
					held = i;
			}
	e->video_through = (e->video_through & ~VI_HELD) | (held >= 0 ? VI_HELD : 0);
	if (held >= 0) {
		TS('H');
		*(volatile u32 *)(VI_XFB(held) + 4) = VI_XFB_DRAWDONE;
	}
	TS('W');
	((void (*)(s32, void *))e->orig_vi)(exception, context);
	TS('w');
	if (held >= 0)
		*(volatile u32 *)(VI_XFB(held) + 4) = VI_XFB_NEXT;
	e->last_retrace_tb = (u32)timebase();
	if (e->poll_cb)
		vi_nudge(e);
	/* (just past a retrace, a new line for the read can come neither twice in a field nor never) */
	if (e->poll_cb && poll_target(e) != e->poll_us) {
		if (e->poll_next >= e->sync_lead)
			TR(0, 0xF4, e->poll_next);	/* (not each line of a sync lead's: 0xF3 has it) */
		e->poll_us = poll_target(e);
		poll_lines(e);
	}
}

/* The OS's handler for the VI's interrupts, with this one's before it (glue.s, vi_shim) */
void rb_vi_interrupt(Engine *e, s32 exception, void *context)
{
	TS('V');
	bg_mark(8, *(volatile u16 *)0xCC00202Cu);
	bg_feed();
#ifdef RB_TRACE
	if (bg()->hooked && !bg()->dec_hooked)
		bg_dec_on(bg());	/* (the profiler's ticks, rb_dec) */
#endif
	if ((VI_DI2[0] | VI_DI3[0]) & VI_INT) {
		u32 own[0x2D0 / 4] __attribute__((aligned(8)));
		VI_DI2[0] = VI_DI2[0] & ~VI_INT;
		VI_DI3[0] = VI_DI3[0] & ~VI_INT;
		if (e->poll_cb) {
			/* when this frame's pad goes to the kernel, in its own clock: it then wakes in
			 * time for the next one, as the VI's line interrupts keep to the refresh */
			*SCRATCH_WORD(T_POLL_HW) = *(volatile u32 *)0xCD800010u;	/* HW_TIMER */
			OS_CLEAR_CONTEXT(own);
			OS_SET_CURRENT_CONTEXT(own);
			/* Slippi puts off a renewal that comes during a rollback, until the main loop is
			 * back from rendering (PreventPadAlarmDuringRollback.asm). It knows the retrace's by
			 * the caller's address, which this is not: it would run in the middle of the
			 * rollback, so this does what Slippi's check does, and the pad goes out at once. */
			e->in_poll = 1;
			if (rd8(rd32(SDA_BASE + OFST_R13_ODB_ADDR) + ODB_ROLLBACK_IS_ACTIVE)) {
				*(volatile u8 *)(rd32(SDA_BASE + OFST_R13_ODB_ADDR) + ODB_SHOULD_FORCE_PAD_RENEW) = 1;
				rb_early_pad(e);
			} else {
				((void (*)(u32))e->poll_cb)(0);
			}
			e->in_poll = 0;
			OS_CLEAR_CONTEXT(own);
			OS_SET_CURRENT_CONTEXT(context);
		}
		if (!((VI_DI0[0] | VI_DI0[2]) & VI_INT))
			return;
	}
	vi_retrace(e, exception, context);
}

/* A load or capture has interrupts off for 2-4 ms, and starts when the frame before it is done, so
 * after a slow frame one can reach across the retrace. The frame just drawn then misses it twice
 * over: its finished draw isn't noted, and the retrace isn't handled in the vertical blank. Shown
 * a refresh late, the frame before it twice, and the next frame finds no free buffer. So between
 * pages these two go through, called as the OS would call them, and nothing else does: no alarm,
 * no pad, no thread switch.
 *
 * It can't be seen mid-restore: what they touch (HSD_VIData, VI's and GX's own) isn't in the
 * savestates, or a load would already put it back, at whatever moment it comes. A read's line
 * interrupt waiting too means the poll is due, which can't run in here: then nothing goes through,
 * as the OS's retrace handler would clear that interrupt. */
static void video_through(Engine *e)
{
	u32 cause = *PI_CAUSE & *PI_MASK & (PI_VI | PI_PE_FINISH);
	void *context;
	if (!cause || !e->poll_cb || !e->orig_pe)
		return;
	if ((cause & PI_VI) && ((VI_DI2[0] | VI_DI3[0]) & VI_INT))
		cause &= ~PI_VI;
	if (!cause)
		return;
	context = *(void **)OS_CURRENT_CONTEXT;
	OS_DISABLE_SCHEDULER();
	/* (the draw first: this retrace can then put it up) */
	if (cause & PI_PE_FINISH)
		((void (*)(s32, void *))e->orig_pe)(OS_INTERRUPT_PI_PE_FINISH, context);
	if ((cause & PI_VI) && ((VI_DI0[0] | VI_DI0[2]) & VI_INT)) {
		vi_retrace(e, OS_INTERRUPT_PI_VI, context);
	}
	OS_ENABLE_SCHEDULER();
	if (e->video_through < 0xFFFF0000u)
		e->video_through += 0x10000;
	e->vt_count++;
}

/* A scene's heaps are about to go (heaps_reset.s): the game is over */
void rb_heaps_reset(Engine *e)
{
	poll_early_off(e);
}
#endif

#ifdef RB_TRACE
/* ---- RB_TRACE: what the player sees and hears ------------------------------------
 *
 * Counters of the game's work say little about the screen. This records, at each
 * refresh, which game frame the VI is given (tagged at its copy to the frame buffer), and
 * what may disturb the player: a retrace handled late (the VI's registers are written
 * while it draws: a flip that takes a refresh more, or tears), sound frames late (the
 * sound engine runs from the AI's interrupt, every 5 ms: late by a block, a block
 * repeats), long stretches with interrupts off. All by refresh number, as a list of
 * events, sent at the game's end; and the player's marks: a D-pad press (which does
 * nothing in a game) at the moment something felt wrong. tools/rollback/trace_view.py
 * reads it. The engine need not be active: a delay based game gets the same record.
 * Test builds only (build_online_boot.py --rollback-trace). */
#define TR_REPEAT 1	/* a refresh with no new frame: b the frame on screen */
#define TR_JUMP 2	/* a new frame, not the next: a how far, b the frame */
#define TR_LAG 3	/* refreshes from a frame's poll to its showing changed: a to what, b the frame */
#define TR_LOAD 4	/* a rollback's load: a its depth, b us with interrupts off */
#define TR_PASS 5	/* a long pass of the main loop: a engine loops in it, b us */
#define TR_LATE 6	/* a retrace long after the last: a the VI's line then (at most 255), b us since the last */
#define TR_AUDIO 7	/* the AI's interrupt late: a the DSP's state, b us since the last */
#define TR_WAIT 8	/* a long wait for a late input: a got it, b us */
#define TR_POLL 9	/* a long poll: b us on the EXI */
#define TR_MARK 10	/* the player's mark: a the D-pad's bits, b the frame polled */
#define TR_CAPTURE 11	/* a long capture: a pages, b us with interrupts off */
#define TR_FRAME 12	/* a frame long from poll to drawn: a poll to copy, in 0.25 ms; b us in all */
#define TR_SECOND 13	/* every 60 refreshes, of the frames drawn: a the longest poll to copy, in 0.25 ms;
			 * b the longest poll to drawn << 16 | their average, in us */
#define TR_FRAME_FROM_US 13500
#define TR_STAMP 15	/* (shared with TR_DISC, whose a is 0) a moment before a repeat: a which (tr_stamp), b us before its retrace */
#define TR_SHIFT 14	/* an idle poll's frame no longer the game's plus what it was: a the new difference, b the frame */
#define TR_DISC 15	/* every 60 refreshes, if any: b the polls in them that found the disc busy */
#define TR_POLLS 0	/* a frame polled twice: the last polls, a flags, b frame << 16 | the game's; then a 0xFF, b saved << 16 | end */
#define TR_STATES 6
#define TR_EVENTS 6900
#define TR_STAMPS 32
#define TR_STAMP_DUMPS 24
#define TR_LOGIC_FRAMES 0x80479D58u
#define TR_AX_DSP_READY 0x804D753Cu	/* __AXOutDspReady: 2 at an AI interrupt: the DSP is a frame behind */
#define TR_VI_LINE 0xCC00202Cu		/* VCT: the retrace interrupt comes at the field's last line */
#define TR_XFB_NEXT VI_XFB_NEXT
#define TR_XFB_WAITDONE VI_XFB_WAITDONE

typedef struct {
	u32 on, refresh, last_retrace, last_ai, shown, have_shown;
	u32 pending[4], pending_in, pending_out, skip_done;	/* frames copied, in the GPU's order */
	u32 tag[3];
	s32 off, last_poll;	/* a logic frame's poll: its number + off */
	u32 have_off, last_lag, buttons;
	u32 moved[5];		/* frames shown, by how far on: repeats, 1, 2, 3, other */
	u32 lag[8];		/* frames shown, by refreshes since their poll */
	u32 line[9];		/* retraces, by the VI's line then: 0, 1, 2-3, 4-7, ... 128-255, 256 on */
	u32 audio[7];		/* AI interrupts, by the gap: < 5.5, 6, 7, 8, 10, 15 ms, more */
	u32 audio_behind;	/* with the DSP a frame behind */
	u32 slack[8];		/* frames drawn, by whole ms left to the next refresh; none, 1, ... */
	u32 poll_t, pending_t[4], pending_cpu[4];	/* when a frame's poll came; for those copied: that, and on to the copy */
	u32 cost[20];		/* frames drawn, by whole ms from their poll: 0..18, more */
	u32 sec_cpu, sec_max, sec_sum, sec_n, sec_disc;
	u32 st[TR_STATES][3], st_n;	/* the last polls: frame << 16 | the game's frame; saved << 16 | end; flags */
	u32 st_lr[TR_STATES], awaiting_read;	/* who called; a 0xB0 written, its answer not read yet */
	u32 n, lost;
	u32 poll_at[256];
	u32 poll_tb[256];	/* when each poll came */
	u32 screen[20];	/* frames by ms from their poll to the refresh that showed them, 14..33 */
	u32 stamp_n, stamp_kind[TR_STAMPS], stamp_tb[TR_STAMPS], dumps, dumped_at;	/* (before ev: tr_start clears up to it) the last moments, for a repeat */
	u32 ev[TR_EVENTS][2];
} Trace;
typedef char trace_fits[sizeof(Trace) <= 0x10000 ? 1 : -1];	/* T_TRACE */

static volatile Trace *tr(void)
{
	return (volatile Trace *)(0xD0000000u | (RB_SCRATCH + T_TRACE));
}

static void tr_off(void)
{
	tr()->on = 0;
}

static void tr_event(u32 kind, u32 a, u32 b)
{
	volatile Trace *t = tr();
	u32 msr = irq_off();
	if (t->on) {
		if (t->n < TR_EVENTS) {
			t->ev[t->n][0] = (t->refresh << 12) | (kind << 8) | (a & 0xFF);
			t->ev[t->n][1] = b;
			t->n++;
		} else {
			t->lost++;
		}
	}
	irq_restore(msr);
}

static u32 tr_log2(u32 v)
{
	u32 k = 0;
	while (v && k < 8) {
		v >>= 1;
		k++;
	}
	return k;
}

static void tr_start(void)
{
	volatile Trace *t = tr();
	u32 i, *p = (u32 *)t;	/* (uncached: whole words) */
	u32 inflight = 0;
	t->on = 0;
	for (i = 0; i < (__builtin_offsetof(Trace, ev)) / 4; i++)
		((volatile u32 *)p)[i] = 0;
	for (i = 0; i < 3; i++)
		inflight += rd32(VI_XFB(i) + 4) == TR_XFB_WAITDONE;
	t->skip_done = inflight;
	t->last_poll = -1000;
	t->last_retrace = t->last_ai = (u32)timebase();
	t->on = 1;
}

/* 'R'etrace, 'I' the pad's interrupt, 'P'oll, 'S'tart of a pass, 'E'nd, 'L'oad, 'l' done, 'C'apture, 'c' done,
 * 'X' the copy, 'D' drawn, 'H' a retrace holding a frame */
static void tr_stamp(u32 kind)
{
	volatile Trace *t = tr();
	u32 i = t->stamp_n++ % TR_STAMPS;
	t->stamp_kind[i] = kind;
	t->stamp_tb[i] = (u32)timebase();
}

static void tr_retrace(Engine *e)
{
	volatile Trace *t = tr();
	u32 now, gap, line, i;
	if (!t->on)
		return;
	now = (u32)timebase();
	gap = (now - t->last_retrace) / 61;
	t->last_retrace = now;
	t->refresh++;
	tr_stamp('R');
	line = *(volatile u16 *)TR_VI_LINE & 0x7FF;
	{	/* lines past the retrace's own: two a frame, at DI0's line and after the last (480i; 480p's are at 526 and 1) */
		u32 at = VI_DI0[0] & 0x7FF;
		if (at < 300 && line >= at * 2 - 1)
			line -= at * 2 - 1;
		else if (line >= at)
			line -= at;
	}
	t->line[tr_log2(line)]++;
	if (gap > 18500 || line >= 16)	/* (16 lines: past the vertical blank in 480i) */
		tr_event(TR_LATE, line > 255 ? 255 : line, gap);
	if (line >= 20) {
		if (t->dumps < TR_STAMP_DUMPS) {
			u32 n = t->stamp_n < 20 ? t->stamp_n : 20, k;
			t->dumps++;
			for (k = 0; k < n; k++) {
				u32 at = (t->stamp_n - n + k) % TR_STAMPS;
				tr_event(TR_STAMP, t->stamp_kind[at], (now - t->stamp_tb[at]) / 61);
			}
		}
	}
	if (t->refresh % 60 == 0) {
		tr_event(TR_SECOND, t->sec_cpu > 255 ? 255 : t->sec_cpu,
			 (t->sec_max > 0xFFFF ? 0xFFFF : t->sec_max) << 16 | (t->sec_n ? t->sec_sum / t->sec_n : 0));
		if (t->sec_disc)
			tr_event(TR_DISC, 0, t->sec_disc);
		t->sec_cpu = t->sec_max = t->sec_sum = t->sec_n = t->sec_disc = 0;
	}
	for (i = 0; i < 3; i++)
		if (rd32(VI_XFB(i) + 4) == TR_XFB_NEXT)
			break;
	if (i == 3) {
		t->moved[0]++;
		if (t->have_shown) {	/* a: why, the buffers' states and what the game was doing */
			tr_event(TR_REPEAT, rd32(VI_XFB(0) + 4) << 4 | rd32(VI_XFB(1) + 4),
				t->shown | ((e->video_through & VI_HELD) != 0) << 16 | (e->busy != 0) << 17 |
				(u32)rd8(PAD_QUEUE_COUNT) << 19);
			if (t->dumps < TR_STAMP_DUMPS && t->refresh - t->dumped_at > 60) {
				u32 n = t->stamp_n < TR_STAMPS ? t->stamp_n : TR_STAMPS, k;
				t->dumps++;
				t->dumped_at = t->refresh;
				for (k = 0; k < n; k++) {
					u32 at = (t->stamp_n - n + k) % TR_STAMPS;
					tr_event(TR_STAMP, t->stamp_kind[at], (now - t->stamp_tb[at]) / 61);
				}
			}
		}
		return;
	}
	{
		u32 f = t->tag[i];
		s32 d = (s32)(f - t->shown);
		if (t->have_shown) {
			t->moved[d >= 1 && d <= 3 ? d : 4]++;
			if (d != 1)
				tr_event(TR_JUMP, (u32)d, f);
		}
		t->shown = f;
		t->have_shown = 1;
		if (t->have_off) {
			u32 ms = (now - t->poll_tb[(f + (u32)t->off) & 255]) / 61 / 1000;
			u32 lag = t->refresh - t->poll_at[(f + (u32)t->off) & 255];
			t->screen[ms < 14 ? 0 : ms > 33 ? 19 : ms - 14]++;
			t->lag[lag > 7 ? 7 : lag]++;
			if (lag != t->last_lag) {
				tr_event(TR_LAG, lag, f);
				t->last_lag = lag;
			}
		}
	}
}

static void tr_copy(void)
{
	volatile Trace *t = tr();
	if (t->on) {
		u32 i = t->pending_in++ & 3;
		t->pending[i] = rd32(TR_LOGIC_FRAMES);
		t->pending_t[i] = t->poll_t;
		t->pending_cpu[i] = ((u32)timebase() - t->poll_t) / 61;
		tr_stamp('X');
	}
}

/* What's on screen, from the outside: the frame's number in blocks at the top of its frame
 * buffer, which a capture of the video output can read (tools/sweep/decode_marker.py). */
#define TR_MARK_TOP 32
#define TR_MARK_LINES 16
#define TR_MARK_LEFT 24	/* in 2-pixel words */
#define TR_MARK_BLOCK 14
#define TR_WHITE 0xEB80EB80u
#define TR_BLACK 0x10801080u

static void tr_marker(u32 idx, u32 frame)
{
	u32 low = frame & 255, gray = low ^ (low >> 1), y, b, x;
	u32 base = 0xC0000000u | (rd32(VI_XFB(idx)) & 0x3FFFFFFFu);
	for (y = 0; y < TR_MARK_LINES; y++) {
		volatile u32 *line = (volatile u32 *)(base + (TR_MARK_TOP + y) * 1280 + TR_MARK_LEFT * 4);
		for (b = 0; b < 10; b++) {
			u32 v = b == 0 ? TR_WHITE : b == 1 ? TR_BLACK : gray >> (9 - b) & 1 ? TR_WHITE : TR_BLACK;
			for (x = 0; x < TR_MARK_BLOCK; x++)
				*line++ = v;
		}
	}
}

static void tr_drawn(u32 idx)
{
	volatile Trace *t = tr();
	u32 used;
	if (!t->on || idx > 2)
		return;
	tr_stamp('D');
	if (t->skip_done) {
		t->skip_done--;
		return;
	}
	{
		u32 i = t->pending_out++ & 3;
		u32 total = ((u32)timebase() - t->pending_t[i]) / 61;
		u32 cpu = t->pending_cpu[i] / 250;
		t->tag[idx] = t->pending[i];
		tr_marker(idx, t->pending[i]);
		t->cost[total / 1000 > 19 ? 19 : total / 1000]++;
		if (cpu > t->sec_cpu)
			t->sec_cpu = cpu;
		if (total > t->sec_max)
			t->sec_max = total;
		t->sec_sum += total;
		t->sec_n++;
		if (total > TR_FRAME_FROM_US)
			tr_event(TR_FRAME, cpu > 255 ? 255 : cpu, total);
	}
	used = ((u32)timebase() - t->last_retrace) / 61;
	used = used >= 16683 ? 0 : (16683 - used) / 1000 + 1;
	t->slack[used > 7 ? 7 : used]++;
}

static void tr_ai(void)
{
	volatile Trace *t = tr();
	u32 now, gap, ready;
	if (!t->on)
		return;
	now = (u32)timebase();
	gap = (now - t->last_ai) / 61;
	t->last_ai = now;
	tr_stamp('A');
	ready = rd32(TR_AX_DSP_READY);
	t->audio[gap < 5500 ? 0 : gap < 6000 ? 1 : gap < 7000 ? 2 : gap < 8000 ? 3 : gap < 10000 ? 4 : gap < 15000 ? 5 : 6]++;
	t->audio_behind += ready == 2;
	if (gap >= 10000 || ready == 2)	/* (10 ms: a whole block late) */
		tr_event(TR_AUDIO, ready, gap);
}

static void tr_poll(Engine *e, const u8 *b0)
{
	volatile Trace *t = tr();
	s32 frame = (s32)be32(b0 + TXB_FRAME);
	u32 dpad = b0[TXB_PAD + 1] & 0x0F;
	int repeated;
	if (!t->on)
		return;
	repeated = frame == t->last_poll;
	if (frame > t->last_poll) {
		t->last_poll = frame;
		t->poll_at[(u32)frame & 255] = t->refresh;
		t->poll_tb[(u32)frame & 255] = (u32)timebase();
		t->poll_t = (u32)timebase();
	}
	if (DVDGetDriveStatus() != 0)
		t->sec_disc++;
	if (frame > 120) {
		u32 odb = rd32(SDA_BASE + OFST_R13_ODB_ADDR), k = t->st_n++ % TR_STATES, i;
		t->st[k][0] = (u32)frame << 16 | (rd32(0x80479D60u) & 0xFFFF);
		t->st[k][1] = rd32(odb + ODB_SAVESTATE_FRAME) << 16 | (rd32(odb + ODB_ROLLBACK_END_FRAME) & 0xFFFF);
		t->st[k][2] = rd8(odb + ODB_ROLLBACK_IS_ACTIVE) | rd8(odb + ODB_ROLLBACK_SHOULD_LOAD_STATE) << 1 |
			(e->busy ? 4 : 0) | rd8(PAD_QUEUE_COUNT) << 4;
		{
			u32 m = irq_off();
			irq_restore(m);
			if (m & MSR_EE)
				t->st[k][2] |= 8;
		}
		if (t->awaiting_read)
			t->st[k][2] |= 0x80;
		t->st_lr[k] = (u32)__builtin_return_address(0);
		if (repeated) {	/* a frame polled twice: the polls up to this one */
			for (i = 0; i < TR_STATES; i++) {
				u32 j = (t->st_n + i) % TR_STATES;
				tr_event(TR_POLLS, t->st[j][2], t->st[j][0]);
				tr_event(TR_POLLS, 0xFF, t->st[j][1]);
			}
		}
	}
	if (frame > 120 && !e->busy && !rd8(PAD_QUEUE_COUNT)) {
		s32 off = frame - (s32)rd32(TR_LOGIC_FRAMES) - 1;
		if (t->have_off && off != t->off)
			tr_event(TR_SHIFT, (u32)off, (u32)frame);
		t->off = off;
		t->have_off = 1;
	}
	if (dpad && !t->buttons)
		tr_event(TR_MARK, dpad, (u32)frame);
	t->buttons = dpad;
}

/* Called from trace_hook.s, at engine + 0x308: 0 the retrace's start, 1 a copy to a frame
 * buffer (HSD_VICopyXFBAsync), 2 one drawn (HSD_VIDrawDoneXFB), 3 the AI's interrupt, 4 and 5 an audio
 * frame's work, its start and nearly its end */
void rb_trace_hook(Engine *e, u32 kind, u32 arg)
{
	(void)e;
	if (kind == 0)
		tr_retrace(e);
	else if (kind == 1)
		tr_copy();
	else if (kind == 2)
		tr_drawn(arg);
	else
		tr_ai();
}

static void tr_send(const char *a, const char *b)
{
	u64 t0 = timebase();
	send(a, b);
	while (timebase() - t0 < 12 * 60750)	/* the kernel's log goes out a line or so at a time */
		;
}

static void tr_list(u32 name, const volatile u32 *v, u32 n)
{
	char a[48], b[64];
	Out o;
	u32 i;
	o.p = a;
	o.end = a + 47;
	put4(&o, C4('r', 'b', ' ', 'T'));
	put4(&o, name);
	*o.p = 0;
	o.p = b;
	o.end = b + 63;
	for (i = 0; i < n; i++) {
		if (i)
			put_c(&o, ' ');
		put_u(&o, v[i]);
	}
	*o.p = 0;
	tr_send(a, b);
}

/* The game is over: the record goes to the log */
static void tr_report(void)
{
	volatile Trace *t = tr();
	u32 i = 0, n = t->n;
	u32 head[3];
	t->on = 0;
	head[0] = t->refresh;
	head[1] = n;
	head[2] = t->lost;
	tr_list(C4('h', 'e', 'a', 'd'), head, 3);
	tr_list(C4('m', 'o', 'v', 'e'), t->moved, 5);
	tr_list(C4('l', 'a', 'g', ' '), t->lag, 8);
	tr_list(C4('l', 'i', 'n', 'e'), t->line, 9);
	tr_list(C4('a', 'u', 'd', 'i'), t->audio, 7);
	head[0] = t->audio_behind;
	tr_list(C4('b', 'e', 'h', 'i'), head, 1);
	tr_list(C4('s', 'l', 'a', 'c'), t->slack, 8);
	tr_list(C4('c', 'o', 's', 't'), t->cost, 20);
	tr_list(C4('s', 'c', 'r', 'n'), t->screen, 20);
	while (i < n) {
		char a[48], b[64];
		Out o;
		u32 k;
		o.p = a;
		o.end = a + 47;
		put4(&o, C4('r', 'b', ' ', 'E'));
		for (k = 0; k < 2 && i < n; k++, i++) {
			put_c(&o, ' ');
			put_x(&o, t->ev[i][0]);
			put_x(&o, t->ev[i][1]);
		}
		*o.p = 0;
		o.p = b;
		o.end = b + 63;
		for (k = 0; k < 3 && i < n; k++, i++) {
			put_c(&o, ' ');
			put_x(&o, t->ev[i][0]);
			put_x(&o, t->ev[i][1]);
		}
		*o.p = 0;
		tr_send(a, b);
	}
}
#else
void rb_trace_hook(Engine *e, u32 kind, u32 arg)
{
	(void)e;
	(void)kind;
	(void)arg;
}
#endif

/* 1: handled here (0xB1 capture, 0xB2 load, replay data, the 0xB0's answer); 0: on to Slippi's function */
int rb_exi(Engine *e, u8 *buf, int len, int mode)
{
#ifndef RB_HOST
	/* Most of the engine's start-of-frame runs with interrupts off (StartEngineLoop.asm), in EXI
	 * calls and between them. (Not from a poll's interrupt: the handler is what's running.) */
	if (!e->in_poll) {
		u32 m = irq_off();
		if (!(m & MSR_EE))
			video_through(e);
		if (e->scratch_ok)
			bg_feed();
		irq_restore(m);
	}
#endif
#ifdef RB_TRACE
	if (mode == 1 && buf[0] == 0xB0) {
		tr_poll(e, buf);
		tr()->awaiting_read = 1;
	} else if (mode == 0) {
		tr()->awaiting_read = 0;
	}
	if (!e->active && mode == 1 && buf[0] == 0xB0) {	/* (delay based: its polls timed as well) */
		u64 t0 = timebase();
		((void (*)(u8 *, int, int))e->orig_exi)(buf, len, mode);
		if (timebase() - t0 > 2000 * 61)
			TR(9, 0, (u32)(timebase() - t0) / 61);
		return 1;
	}
#endif
	if (mode != 1 || len != 33 || (buf[0] != 0xB1 && buf[0] != 0xB2) || !e->active) {
#ifndef RB_HOST
		if (e->active) {
			u64 t0;
			int k = mode != 1 ? 1 : buf[0] == 0xB0 || buf[0] == 0xE0 ? 0 : 2;
			if (mode == 1 && buf[0] >= 0x35 && buf[0] <= 0x3F && len <= 0x4000) {
				replay_queue(e, buf, (u32)len);
				return 1;
			}
			if (mode == 1 && buf[0] == 0xB0) {
				TS('P');
				e->poll_prev_tb = e->poll_tb;
				e->poll_tb = (u32)timebase();
				e->poll_prev_rc = e->poll_rc;
				e->poll_rc = (u16)rd32(RETRACE_COUNT);
				input_wait(e, buf);
				e->busy = 1;	/* until this poll's frame is done */
			}
			t0 = timebase();
			if (mode == 1 || !answer_ready(e, buf, len))
				((void (*)(u8 *, int, int))e->orig_exi)(buf, len, mode);
			if (mode == 1 && buf[0] == 0xB0)
				e->poll_frame = (s32)be32(buf + TXB_FRAME);
			e->exi_n[k]++;
			e->exi_us[k] += (u32)(timebase() - t0) / 61;
			if (k == 0 && timebase() - t0 > 2000 * 61)
				TR(9, 0, (u32)(timebase() - t0) / 61);
			return 1;
		}
#endif
		return 0;
	}
	if (DVDGetDriveStatus() != 0)
		e->disc_busy++;	/* (fine: the I/O drivers are not rewound) */
	note_event(buf[0] == 0xB1 ? EV_CAPTURE : EV_LOAD, (s32)be32(buf + 1));
	if (buf[0] == 0xB1) {
		TS('C');
		tr_vi_state(0xF7);
#if defined(RB_WALK) && !defined(RB_HOST) && !defined(RB_DIRECT_LOG)	/* (beside the probe: after a load and at a frame's end) */
		walk_check(e, 2, be32(buf + 1));
#endif
		capture(e, (s32)be32(buf + 1));
		TS('c');
#ifndef RB_HOST
		{
			/* the frame a rollback just asked for, and it had not run yet: the race
			 * of so_gct_fix_capture_check (so_files.h), each time counted */
			u32 odb = rd32(SDA_BASE + OFST_R13_ODB_ADDR);
			s32 frame = (s32)be32(buf + 1);
			if (rd8(odb + ODB_ROLLBACK_SHOULD_LOAD_STATE) && !rd8(odb + ODB_STABLE_ROLLBACK_IS_ACTIVE) &&
			    frame == (s32)rd32(odb + ODB_SAVESTATE_FRAME) && frame == (s32)rd32(odb + ODB_STABLE_FINALIZED_FRAME)) {
				e->saved_late++;
				note_event(EV_SAVED_LATE, frame);
			}
		}
#endif
	} else {
		u32 caps_before = e->caps;
		TS('L');
		tr_vi_state(0xF5);
		load(e, (s32)be32(buf + 1), buf + 5);
#if defined(RB_WALK) && !defined(RB_HOST)
		walk_check(e, 1, be32(buf + 1));
#endif
		TS('l');
		tr_vi_state(0xF6);
#ifndef RB_HOST
		e->poll_loaded = caps_before - e->caps >= 1 ? 2 : 1;	/* (depth: captures dropped) */
		direct_after_load(e);
#else
		(void)caps_before;
#endif
	}
	return 1;
}

#ifndef RB_HOST
/* Slippi's FN_LoadSavestate asserts (b 0: a frozen console) when it has no
 * savestate of the frame asked for. Here it is checked first (load_shim). A
 * frame lost can only end the game: this says so to the kernel, with what the log
 * needs, and the kernel disconnects; meanwhile the game gets the nearest
 * savestate, to run on until then. args: FN_LoadSavestate's r3, r4, r5. */
u32 rb_load_check(Engine *e, u32 *args)
{
	u32 sscb = args[2], odb = rd32(SDA_BASE + OFST_R13_ODB_ADDR), i;
	s32 frame = (s32)args[1], near = -1, best = -1;
	char a[48], b[64];
	Out o;
	for (i = 0; i < ROLLBACK_MAX_FRAME_COUNT; i++) {
		s32 f = (s32)rd32(sscb + SSCB_SSDB_START + i * SSDB_SIZE + SSDB_FRAME);
		if (f == frame)
			return e->orig_load;
		if (f > best)
			best = f;
		if (f < frame && f > near)
			near = f;
	}
	if (near < 0)
		near = best;
	note_event(EV_LOST, frame);
	e->lost++;
	o.p = a;
	o.end = a + 47;
	put4(&o, C4('r', 'b', ' ', 'L'));
	put4(&o, C4('O', 'S', 'T', ' '));
	put_u(&o, (u32)frame);
	put_c(&o, ' ');
	put_u(&o, rd32(0x80479D60u));
	put_c(&o, ' ');
	put_u(&o, rd32(odb + ODB_FRAME));
	put_c(&o, ' ');
	put_u(&o, rd32(odb + ODB_FINALIZED_FRAME));
	put_c(&o, ' ');
	put_u(&o, rd32(odb + ODB_STABLE_FINALIZED_FRAME));
	*o.p = 0;
	o.p = b;
	o.end = b + 63;
	put_u(&o, rd8(sscb + SSCB_WRITE_INDEX));
	for (i = 0; i < ROLLBACK_MAX_FRAME_COUNT; i++) {
		put_c(&o, ' ');
		put_u(&o, rd32(sscb + SSCB_SSDB_START + i * SSDB_SIZE + SSDB_FRAME));
	}
	*o.p = 0;
	send(a, b);
	o.p = a;
	o.end = a + 47;
	put4(&o, C4('r', 'b', ' ', 'o'));
	put4(&o, C4('d', 'b', ' ', ' '));
	put_u(&o, rd32(odb + ODB_SAVESTATE_FRAME));
	put_c(&o, ' ');
	put_u(&o, rd32(odb + ODB_STABLE_SAVESTATE_FRAME));
	put_c(&o, ' ');
	put_u(&o, rd32(odb + ODB_PLAYER_SAVESTATE_FRAME));
	put_c(&o, ' ');
	put_u(&o, rd32(odb + ODB_STABLE_ROLLBACK_END_FRAME));
	put_c(&o, ' ');
	put_u(&o, rd32(odb + ODB_ROLLBACK_END_FRAME));
	*o.p = 0;
	o.p = b;
	o.end = b + 63;
	put_u(&o, rd8(odb + ODB_SAVESTATE_IS_PREDICTING));
	put_u(&o, rd8(odb + ODB_PLAYER_SAVESTATE_IS_PREDICTING));
	put_u(&o, rd8(odb + ODB_ROLLBACK_IS_ACTIVE));
	put_u(&o, rd8(odb + ODB_ROLLBACK_SHOULD_LOAD_STATE));
	put_u(&o, rd8(odb + ODB_STABLE_ROLLBACK_IS_ACTIVE));
	put_u(&o, rd8(odb + ODB_IS_FRAME_ADVANCE));
	*o.p = 0;
	send(a, b);
	args[1] = (u32)near;
	return near < 0 ? 0 : e->orig_load;
}
#endif

/* ---- each frame ------------------------------------------------------------------ */

static void reset(Engine *e)
{
	e->n = e->cpu_sum = e->cpu_max = e->draw_sum = e->draw_max = 0;
	{
		int i;
		for (i = 0; i < 8; i++)
			e->hist[i] = 0;
	}
	e->captures = e->cap_pages = e->cap_pages_max = e->cap_ticks = e->cap_ticks_max = 0;
	e->loads = e->load_pages = e->load_pages_max = e->load_ticks = e->load_ticks_max = e->load_depth_max = 0;
	e->misses = e->errors = e->saved_late = 0;
	e->pass_loops = 0;
	{
		int i;
		for (i = 0; i < 5; i++)
			e->pass_n[i] = e->pass_us[i] = e->pass_over[i] = 0;
		for (i = 0; i < 3; i++)
			e->exi_n[i] = e->exi_us[i] = 0;
		e->input_waits = e->input_arrived = e->input_wait_ticks = e->wait_misses = e->busy = e->head_seen = 0;
		e->early_frame = 0;
	}
	e->disc_busy = e->load_waits = e->load_wait_max = 0;
	e->ld_scan = e->ld_whole = e->ld_part = e->ld_parts = e->cap_scan = 0;
	e->early_valid = 0;
	e->unmarked = e->unmarked_told = 0;
	{
		int i;
		for (i = 0; i < 8; i++)
			e->unmarked_lo[i] = e->unmarked_hi[i] = 0;
	}
	{
		int i;
		for (i = 0; e->scratch_ok && i < PAGES / 16; i++)
			t32(T_HIST)[i] = 0;
	}
}

/* The last frame's busy time, from its start (here) to its end (HSD_PerfSetTotalTime,
 * after the render is built; frame_end.s), in microseconds; and the time between
 * two frames' starts. With the savestates' work. */
static void count_frame(Engine *e)
{
	u32 now = (u32)timebase();
	/* (direct_track's skip, taken: the visual buffer fix's check has cleared its flag, and the frame
	 * before this one was not drawn. Its loop is no part of this frame's pass.) */
	if (e->direct_skip && (s32)rd32(VB_FLAG) != -1) {
		e->direct_skip = 0;
		e->pass_loops = 0;
	}
	if (e->ends != e->last_ends && e->pass_loops) {
		u32 busy = (e->end_tb - e->pass_start) / 61, k = e->pass_loops > 5 ? 4 : e->pass_loops - 1;
		e->pass_n[k]++;
		e->pass_us[k] += busy;
		e->pass_over[k] += busy > 16683;
		if (busy > 12000 || e->pass_loops > 1)
			TR(5, e->pass_loops, busy);
		e->pass_loops = 0;
		TS('E');
	}
	if (!e->pass_loops) {
		e->pass_start = now;
		TS('S');
#ifndef RB_HOST
		/* (with a poll waiting behind this frame's, the older one is this frame's) */
		e->pass_poll_tb = rd8(PAD_QUEUE_COUNT) > 1 ? e->poll_prev_tb : e->poll_tb;
		e->pass_poll_rc = rd8(PAD_QUEUE_COUNT) > 1 ? e->poll_prev_rc : e->poll_rc;
		{
			u32 i, msr = irq_off();
			e->video_through &= ~(3 * VI_OLDER);
			for (i = 0; i < 2; i++) {
				u32 status = rd32(VI_XFB(i) + 4);
				if (status == VI_XFB_WAITDONE || status == VI_XFB_NEXT)
					e->video_through |= VI_OLDER << i;
			}
			irq_restore(msr);
		}
#endif
	}
	e->busy = 1;
	e->pass_loops++;
	if (e->start_tb && e->ends != e->last_ends) {
		u32 busy = (e->end_tb - e->start_tb) / 61;	/* 60.75 ticks a microsecond */
		u32 period = (now - e->start_tb) / 61;
		e->n++;
		e->hist[busy < 8000 ? 0 : busy < 10000 ? 1 : busy < 12000 ? 2 : busy < 14000 ? 3 : busy < 15000 ? 4 :
			busy < 16000 ? 5 : busy < 16683 ? 6 : 7]++;
		e->cpu_sum += busy;
		e->draw_sum += period;
		if (busy > e->cpu_max)
			e->cpu_max = busy;
		if (period > e->draw_max)
			e->draw_max = period;
	}
	e->last_ends = e->ends;
	e->start_tb = now;
}


/* Each game, and every 1200 frames:
 *   rb n=<frames> cpu=<avg>/<max> drw=<avg>/<max> base=<ticks>
 *   | cap=<n> pg=<avg>/<max> t=<avg>/<max> | ld=<n> pg= t= dep=<max> miss= err=
 */
static void report(Engine *e)
{
	char a[48], b[64];
	Out o;
#if defined(RB_WALK) && !defined(RB_HOST) && !defined(RB_DIRECT_LOG)
	walk_say('K', 0, 0, e->verify_reported, e->verify_have, e->verify_bad, 0);
	e->verify_bad = e->verify_reported = e->verify_have = 0;
#endif
	o.p = a;
	o.end = a + 47;
	put4(&o, C4('r', 'b', ' ', 'n'));
	put_c(&o, '=');
	put_u(&o, e->n);
	put_stat(&o, C4(' ', 'b', 'u', 's'), e->cpu_sum, e->n, e->cpu_max);
	put_stat(&o, C4(' ', 'p', 'e', 'r'), e->draw_sum, e->n, e->draw_max);
	*o.p = 0;
	o.p = b;
	o.end = b + 63;
	put4(&o, C4('b', 'a', 's', 'e'));
	put_c(&o, '=');
	put_u(&o, e->base_ticks);
	put4(&o, C4(' ', 'c', 'a', 'p'));
	put_c(&o, '=');
	put_u(&o, e->captures);
	put_stat(&o, C4(' ', 'p', 'g', 0), e->cap_pages, e->captures, e->cap_pages_max);
	put_stat(&o, C4(' ', 't', 0, 0), e->cap_ticks, e->captures, e->cap_ticks_max);
	*o.p = 0;
	send(a, b);
	o.p = a;
	o.end = a + 47;
	put4(&o, C4('r', 'b', ' ', 'l'));
	put4(&o, C4('d', '=', 0, 0));
	put_u(&o, e->loads);
	put_stat(&o, C4(' ', 'p', 'g', 0), e->load_pages, e->loads, e->load_pages_max);
	put_stat(&o, C4(' ', 't', 0, 0), e->load_ticks, e->loads, e->load_ticks_max);
	*o.p = 0;
	o.p = b;
	o.end = b + 63;
	put4(&o, C4('d', 'e', 'p', '='));
	put_u(&o, e->load_depth_max);
	put4(&o, C4(' ', 'm', 'i', 's'));
	put4(&o, C4('s', '=', 0, 0));
	put_u(&o, e->misses);
	put4(&o, C4(' ', 'e', 'r', 'r'));
	put_c(&o, '=');
	put_u(&o, e->errors + e->failed);
	put4(&o, C4(' ', 't', 'r', 'k'));
	put_c(&o, '=');
	put_u(&o, e->tracked);
	put4(&o, C4(' ', 'o', 'n', '='));
	put_u(&o, e->active);
	*o.p = 0;
	send(a, b);
	o.p = a;
	o.end = a + 47;
	put4(&o, C4('r', 'b', ' ', 'h'));	/* frames by busy time (count_frame) */
	put_c(&o, '=');
	{
		int i;
		for (i = 0; i < 8; i++) {
			if (i)
				put_c(&o, ',');
			put_u(&o, e->hist[i]);
		}
	}
	*o.p = 0;
	o.p = b;
	o.end = b + 63;
	put4(&o, C4('l', 'd', '=', 0));	/* a load's ticks: scan / whole pages / parts, and parts */
	put_u(&o, e->loads ? e->ld_scan / e->loads : 0);
	put_c(&o, '/');
	put_u(&o, e->loads ? e->ld_whole / e->loads : 0);
	put_c(&o, '/');
	put_u(&o, e->loads ? e->ld_part / e->loads : 0);
	put_c(&o, '/');
	put_u(&o, e->loads ? e->ld_parts / e->loads : 0);
	put4(&o, C4(' ', 's', 'v', '='));	/* savestates saved late (so_gct_fix_capture_check) */
	put_u(&o, e->saved_late);
	put4(&o, C4(' ', 'c', 's', '='));	/* a capture's scan ticks */
	put_u(&o, e->captures ? e->cap_scan / e->captures : 0);
	put4(&o, C4(' ', 'd', 'v', 'd'));	/* savestates with a disc read out */
	put_c(&o, '=');
	put_u(&o, e->disc_busy);
	put4(&o, C4(' ', 'l', 'w', '='));	/* file loads waited for (sync_loads.s), the longest in us */
	put_u(&o, e->load_waits);
	put_c(&o, '/');
	put_u(&o, e->load_wait_max / 61);
	*o.p = 0;
	send(a, b);
	{
		int i;
		o.p = a;
		o.end = a + 47;
		put4(&o, C4('r', 'b', ' ', 'p'));	/* a refresh's work by its loops: count/us/over a refresh */
		put_c(&o, ' ');
		for (i = 0; i < 2; i++) {
			put_u(&o, e->pass_n[i]);
			put_c(&o, '/');
			put_u(&o, e->pass_n[i] ? e->pass_us[i] / e->pass_n[i] : 0);
			put_c(&o, '/');
			put_u(&o, e->pass_over[i]);
			put_c(&o, ' ');
		}
		*o.p = 0;
		o.p = b;
		o.end = b + 63;
		for (i = 2; i < 5; i++) {
			put_u(&o, e->pass_n[i]);
			put_c(&o, '/');
			put_u(&o, e->pass_n[i] ? e->pass_us[i] / e->pass_n[i] : 0);
			put_c(&o, '/');
			put_u(&o, e->pass_over[i]);
			put_c(&o, ' ');
		}
		*o.p = 0;
		send(a, b);
#ifndef RB_DIRECT_LOG	/* (no room for these two lines in the test build's codes) */
		o.p = a;
		o.end = a + 47;
		put4(&o, C4('r', 'b', ' ', 'e'));	/* the EXI's wait: 0xB0, its answer, the rest: count/us */
		put4(&o, C4('x', 'i', ' ', 0));
		for (i = 0; i < 3; i++) {
			put_u(&o, e->exi_n[i]);
			put_c(&o, '/');
			put_u(&o, e->exi_n[i] ? e->exi_us[i] / e->exi_n[i] : 0);
			put_c(&o, ' ');
		}
		*o.p = 0;
		b[0] = 0;
		send(a, b);
		{	/* background saves: faults/us each/most us, spurious, finishes/us each, ticks */
			volatile Bg *g = bg();
			o.p = a;
			o.end = a + 47;
			put4(&o, C4('r', 'b', ' ', 'g'));
			put_c(&o, ' ');
			put_u(&o, g->faults);
			put_c(&o, '/');
			put_u(&o, g->faults ? g->fault_ticks / 61 / g->faults : 0);
			put_c(&o, '/');
			put_u(&o, g->fault_max / 61);
			put_c(&o, ' ');
			put_u(&o, g->spurious);
			*o.p = 0;
			o.p = b;
			o.end = b + 63;
			put_u(&o, g->finishes);
			put_c(&o, '/');
			put_u(&o, g->finishes ? g->finish_ticks / 61 / g->finishes : 0);
			put_c(&o, ' ');
			put_u(&o, g->ticks);
			*o.p = 0;
			send(a, b);
		}
#endif
#ifdef RB_TRACE
		{	/* the profiler (rb_dec): the 24 busiest 256-byte pieces of code in rollbacks, 6 a line */
			u32 *t = t32(T_PROF), line, k2, best, bi;
			for (line = 0; line < 4; line++) {
				o.p = a;
				o.end = a + 47;
				put4(&o, C4('r', 'b', ' ', 'f'));
				*o.p = 0;
				o.p = b;
				o.end = b + 63;
				for (k2 = 0; k2 < 6; k2++) {
					u32 h;
					best = 0;
					bi = 0;
					for (h = 0; h < 256; h++)
						if (t[h * 2 + 1] > best) {
							best = t[h * 2 + 1];
							bi = h;
						}
					if (!best)
						break;
					put_x(&o, t[bi * 2] << 8);
					put_c(&o, ':');
					put_u(&o, best);
					put_c(&o, ' ');
					t[bi * 2 + 1] = 0;
				}
				*o.p = 0;
				send(a, b);
			}
		}
#endif
		o.p = a;
		o.end = a + 47;
		put4(&o, C4('r', 'b', ' ', 'w'));	/* polls that waited for a late input/got it/us each */
		put_c(&o, ' ');
		put_u(&o, e->input_waits);
		put_c(&o, '/');
		put_u(&o, e->input_arrived);
		put_c(&o, '/');
		put_u(&o, e->input_waits ? e->input_wait_ticks / 61 / e->input_waits : 0);
		*o.p = 0;
		o.p = b;
		o.end = b + 63;
		put4(&o, C4('e', 'a', 'r', 'l'));	/* the pad read this long before the retrace, us; times it changed */
		put_c(&o, ' ');
		put_u(&o, e->poll_us);
		put4(&o, C4(' ', 'v', 'b', ' '));
		put_u(&o, e->vb_cleared);
		put4(&o, C4(' ', 't', 'h', 'r'));	/* video_through */
		*o.p++ = ' ';
		put_u(&o, e->video_through >> 16);
		put4(&o, C4(' ', 'n', 'd', 'g'));
		*o.p++ = ' ';
		put_u(&o, e->video_through & 0xFFF);
		/* direct_track: skips/runs level ended/frames drawn behind/passes over DIRECT_FIT_US/the opponent's
		 * number the game began with */
		put4(&o, C4(' ', 'd', 'i', 'r'));
		*o.p++ = ' ';
		put_u(&o, e->direct_skips);
		put_c(&o, '/');
		put_u(&o, e->direct_overruns);
		put_c(&o, '/');
		put_u(&o, e->direct_behind);
		put_c(&o, '/');
		put_u(&o, e->deep_game);
		put_c(&o, '/');
		put_u(&o, e->deep_carry);
		*o.p = 0;
		send(a, b);
#ifdef RB_DIRECT_LOG
		{
			u32 *log = t32(T_DIRECT_LOG), i, k;
			a[5] = 0;	/* "rb dx": direct_log's records, as they are */
			a[3] = 'd';
			a[4] = 'x';
			for (i = 0; i < log[0]; i++) {
				o.p = b;
				for (k = 0; k < 4; k++) {
					put_x(&o, log[1 + i * 4 + k]);
					put_c(&o, ' ');
				}
				*o.p = 0;
				send(a, b);
			}
		}
#endif
	}
#ifndef RB_DIRECT_LOG	/* (no room for both in the test build's codes) */
	if (e->captures) {	/* where the captured pages are: the 7 busiest 64 KB, pages a capture x 10 */
		u32 *hist = t32(T_HIST), done[7], k, i;
		o.p = a;
		o.end = a + 47;
		put4(&o, C4('r', 'b', ' ', 'w'));
		put4(&o, C4('r', 'o', 't', 'e'));
		for (k = 0; k < 7; k++) {
			u32 best = 0;
			done[k] = PAGES;
			for (i = 0; i < PAGES / 16; i++) {
				u32 j;
				for (j = 0; j < k && done[j] != i; j++)
					;
				if (j == k && hist[i] > best) {
					best = hist[i];
					done[k] = i;
				}
			}
			if (!best)
				break;
			if (k == 3) {
				*o.p = 0;
				o.p = b;
				o.end = b + 63;
			}
			put_c(&o, ' ');
			put_x(&o, 0x80000000u + (done[k] << 16));
			put_c(&o, ':');
			put_u(&o, best * 10 / e->captures);
		}
		*o.p = 0;
		if (k <= 3)
			b[0] = 0;
		send(a, b);
	}
#endif
#ifdef RB_VERIFY
	if (e->unmarked) {	/* (verify_load): one line a run */
		u32 i;
		for (i = 0; i < 8 && e->unmarked_hi[i]; i++) {
			o.p = a;
			o.end = a + 47;
			put4(&o, C4('r', 'b', ' ', 'u'));
			put4(&o, C4('n', 'm', 'a', 'r'));
			put4(&o, C4('k', 'e', 'd', ' '));
			put_x(&o, 0x80000000u + ((u32)e->unmarked_lo[i] << 12));
			put_c(&o, '-');
			put_x(&o, 0x80000000u + ((u32)e->unmarked_hi[i] << 12));
			*o.p = 0;
			o.p = b;
			o.end = b + 63;
			put4(&o, C4('o', 'f', ' ', 0));
			put_u(&o, e->unmarked);
			*o.p = 0;
			send(a, b);
		}
	}
#endif
}

void rb_frame(Engine *e, u32 wrapper, Data *d, u32 *regs)
{
	(void)regs;	/* (the host's tests have no game loop) */
	u8 major = rd8(0x80479D30), minor = rd8(0x80479D33);
	int in_game = major == 8 && minor == 2;

	if (major == 8)
		install(e, wrapper);
	if (!in_game) {
		if (e->was_in_game && e->scratch_ok) {
			{
				u32 msr = irq_off();
				bg_finish();
				bg_unhook();
				lc_off_now();
				irq_restore(msr);
			}
			report(e);
#ifdef RB_TRACE
			tr_report();
#endif
			if (e->mmu)
				mmu_off(e);
			direct_end(e);
			reset(e);
		}
		e->was_in_game = 0;
		return;
	}
	if (!e->scratch_ok) {
		/* (not the engine's memory: Slippi's own delay-based play; nothing here touches it) */
		e->was_in_game = 1;
		return;
	}
	if (!e->was_in_game) {
		/* the game's first frame: ask the kernel, then set up */
		e->was_in_game = 1;
		t32(T_LC_ON)[0] = 0;	/* (off: the last game turned it off; at boot never on) */
		{
			volatile Bg *b = bg();
			u8 *pend = t8(T_BG_PENDING);
			u32 k;
			b->hooked = 1;
			bg_unhook();	/* (a game that never ended may have left them) */
			b->active = b->faults = b->spurious = b->finishes = b->ticks = 0;
			for (k = 0; k < 10; k++)
				*BG_DIAG(k) = 0;
			b->fault_ticks = b->finish_ticks = b->fault_max = 0;
			/* (and a load's marks: it clears the ones it set, so they must start clear. They were
			 * whatever MEM2 held, and a bit set there was a page no load took from a later capture:
			 * left as the frames rolled back made it. 2026-11-10: every game desynced at its first
			 * rollback over the countdown's end, some crashed.) */
			for (k = 0; k < PAGES / 8; k++)
				pend[k] = t8(T_BG_HOT)[k] = t8(T_MARK)[k] = 0;
			for (k = 0; k < 512; k++)
				t32(T_PROF)[k] = 0;
		}
#ifdef RB_TRACE
		tr_start();
#endif
		e->failed = 0;
		e->start_tb = 0;
		reset(e);
		e->game_frames = 0;
#ifndef RB_HOST
		*(volatile u32 *)(0xD0000000u | (RB_SCRATCH + T_GAME_FRAMES)) = 0;
		/* The kernel takes its end of the ring from here on this tell_kernel. The count runs
		 * on from game to game: never back, or a kernel still at the old count would take all
		 * that lies between for replay data. */
		e->poll_frame = 0;
		*SCRATCH_WORD(T_REPLAY_HEAD) = e->replay_head;
#endif
		if (e->installed && tell_kernel(0)) {
			mmu_on(e, d);
#ifdef RB_VERIFY
			report_bats();
#endif
			if (e->mmu && make_base(e)) {
				e->active = 1;
				tell_kernel(1);
#if defined(RB_HWTEST) && !defined(RB_HOST)
				hw_test();
#endif
			} else {
				if (e->mmu)
					mmu_off(e);
				tell_kernel(2);
			}
		}
	}
	count_frame(e);
#ifndef RB_HOST
	*(volatile u32 *)(0xD0000000u | (RB_SCRATCH + T_GAME_FRAMES)) = ++e->game_frames;
	if (e->game_frames == POLL_EARLY_FROM && e->active)
		poll_early_on(e);
	{
		/* tests: interrupts are on here, just before Slippi's StartEngineLoop: polls
		 * land between queued inputs, as they do when a Wii falls behind. (In a game
		 * only; the kernel has cleared this by then: so_exi.c, clear_engine_words.) */
		u32 slow = *(volatile u32 *)(0xD0000000u | (RB_SCRATCH + T_SLOW_FRAME));
		if (slow) {
			u64 t0 = timebase();
			while (timebase() - t0 < (u64)slow * 60750)
				;
		}
	}
#endif
#ifdef RB_VERIFY
	/* (a dozen trips over the EXI: a late frame, so not in a game played. It ends with one.) */
	if (e->n && e->n % 1200 == 0 && e->n != e->reported) {
		e->reported = e->n;
		report(e);
	}
#endif
}
