/* slippi_online/so_diag.c
 * Hardware diagnostics through Slippi's LAN broadcast (see so_diag.h).
 */
#include "so_diag.h"
#include "so_alloc.h"
#include "so_exi.h"
#include "so_platform.h"
#include "so_socket.h"
#include "so_util.h"
#include "string.h"

extern char __slippi_online_diag_stack_addr, __slippi_online_diag_stack_size;

#define WATCH_PRIORITY 0x40	/* below the kernel's main loop: never in its way */
#define WATCH_PERIOD_US 1000000

volatile u32 so_kernel_loops;
const char *volatile so_phase = "boot";
volatile u32 so_step_pause_us;

static u32 start_ticks;
static u32 beacons;
static char text[32];

/* Checks the heap's free-space pattern once a second (so_alloc.h). */
static u32 WatchThread(void *unused)
{
	(void)unused;
	for (;;) {
		so_sleep_us(WATCH_PERIOD_US);
		so_alloc_check();
	}
	return 0;
}

void so_diag_start(const SoConfig *config)
{
	start_ticks = so_ticks();
	if (!config->status_beacon)
		return;
	so_alloc_fill_free();
	so_alloc_check();
	if (so_thread_start(WatchThread, NULL, &__slippi_online_diag_stack_addr,
			(u32)(size_t)&__slippi_online_diag_stack_size, WATCH_PRIORITY) < 0)
		so_log("diag: could not start the heap watcher");
}

/* "cl212": the first two letters of the checkpoint's file, and its line. */
static int checkpoint(char *out, int size, int thread)
{
	volatile SoTrace *t = &so_traces[thread];
	const char *file = t->file;
	if (!t->count || !file)
		return so_snprintf(out, size, " -");
	return so_snprintf(out, size, " %c%c%u", file[0], file[1] ? file[1] : '_', (unsigned)t->line);
}

/* "G <scene> <pc> <lr>": Melee's scene controller (current major, pending
 * major, previous major, current minor), and where the OS's current thread
 * context last stood (after a crash: the faulting instruction). */
static void game_line(void)
{
	/* Melee 1.02's main thread (DefaultThread): its context's srr0 and lr. */
	u32 scene = so_game_word(0x80479D30), pc = so_game_word(0x804A83C8 + 0x198), lr = so_game_word(0x804A83C8 + 0x84);
	so_snprintf(text, sizeof(text), "G %08x %08x %08x", (unsigned)scene, (unsigned)pc, (unsigned)lr);
}

/* Every other text is the phase. Between them: the game every other time,
 * then the EXI device, and checkpoints, counters and heap in turn. */
const char *so_diag_beacon(void)
{
	u32 count = beacons++, slot;
	int n, i;
	if (count % 16 == 6) {
		/* The interrupt flight recorder (tools/ppc/flight.s), oldest first:
		 * D disable, E enable, R restore (r: one that leaves interrupts
		 * off). The CPU flushes each entry, so these reads are current
		 * (the kernel's other reads of game memory miss the CPU's cache). */
		u32 ring = 0x80197000, next = so_game_word(ring), k, m[9];
		char line[150];
		int len = so_snprintf(line, sizeof(line), "irq %u:", (unsigned)next);
		for (k = next > 12 ? next - 12 : 0; k < next && len < (int)sizeof(line) - 12; k++) {
			u32 e = so_game_word(ring + 0x20 + (k % 64) * 8);
			static const char tags[] = "RDEr";
			len += so_snprintf(line + len, sizeof(line) - len, " %c%08x", tags[e & 3], (unsigned)(e & ~3u));
		}
		if (next)
			so_log("%s", line);
		so_exi_debug(m);
		if (m[0] || m[3])
			so_log("exi mailbox %08x %08x %08x unhandled %u (%08x dev %u)", (unsigned)m[0], (unsigned)m[1],
				(unsigned)m[2], (unsigned)m[3], (unsigned)m[4], (unsigned)m[5]);
	}
	if (count % 2 == 0) {
		slot = 0;
	} else if (count % 4 == 1) {
		game_line();
		return text;
	} else if (count % 8 == 3) {
		SoExiStats e;
		so_exi_stats(&e);
		if (e.panics)	/* Melee panicked: see the log's PANIC line */
			so_snprintf(text, sizeof(text), "P %u n%u", (unsigned)e.panics, (unsigned)e.commands);
		else if (e.crash_srr0)	/* Melee crashed: where, and on what address */
			so_snprintf(text, sizeof(text), "X %08x %08x", (unsigned)e.crash_srr0, (unsigned)e.crash_dar);
		else if (e.heartbeats)	/* Melee's own heartbeats: count, last position */
			so_snprintf(text, sizeof(text), "H %u %08x n%u", (unsigned)e.heartbeats, (unsigned)e.heartbeat_pc,
				(unsigned)e.commands);
		else
			so_snprintf(text, sizeof(text), "E n%u l%02x r%u f%u", (unsigned)e.commands, (unsigned)e.last_command,
				(unsigned)e.reads, (unsigned)e.file_loads);
		return text;
	} else {
		slot = 1 + 2 * ((count / 8) % 3);	/* L, C, A in turn */
	}
	if (slot == 0) {
		so_snprintf(text, sizeof(text), "%us k%u %s", (unsigned)(so_ticks_to_ms(so_ticks() - start_ticks) / 1000),
			(unsigned)so_kernel_loops, so_phase);
	} else if (slot == 1) {
		n = so_snprintf(text, sizeof(text), "L");
		for (i = 0; i < SO_THREAD_COUNT; i++)
			n += checkpoint(text + n, sizeof(text) - n, i);
	} else if (slot == 3) {
		/* Racy reads on purpose: the broadcast thread must never wait. */
		SoSocketStats st;
		so_socket_stats_peek(&st);
		so_snprintf(text, sizeof(text), "C r%u s%u e%u/%u d%u tf%u", (unsigned)st.received, (unsigned)st.sent,
			(unsigned)st.send_errors, (unsigned)st.receive_errors, (unsigned)st.send_dropped,
			(unsigned)so_timer_failures);
	} else {
		SoAllocIntegrity in;
		so_alloc_integrity(&in);
		if (!in.bad_kind)
			so_snprintf(text, sizeof(text), "A ok c%u", (unsigned)in.checks);
		else
			so_snprintf(text, sizeof(text), "A %c%08x %08x c%u/%u", in.bad_kind == 1 ? 'h' : 'p',
				(unsigned)in.bad_addr, (unsigned)in.bad_value, (unsigned)in.first_bad_check,
				(unsigned)in.checks);
	}
	return text;
}
