/* slippi_online/so_diag.h
 * Diagnostics for hardware runs (status_beacon=1 in config.ini).
 *
 * The client's state rides in the nickname field of Slippi Nintendont's own
 * SLIP_READY LAN broadcast (SlippiNetworkBroadcast.c), the one channel
 * proven to work on the console in every test. In diagnostics mode it goes
 * out every second; every other text is the phase, the rest rotate:
 *   "<s>s k<n> <phase>"     seconds since start-up, the kernel main loop's
 *                           pass counter, the step about to be taken
 *   "L <net> <rx> <done> <test>"  each thread's last checkpoint (SO_TRACE),
 *                           as file tag + line, e.g. "cl212"
 *   "C r<n> s<n> e<send>/<recv> d<n> tf<n>"  datagrams received and sent,
 *                           send and receive errors, sends dropped, waits
 *                           that got no IOS timer
 *   "A ok c<n>" or "A <h|p><addr> <value> c<first>/<n>"  the heap check: a
 *                           watcher re-reads the heap's free-space pattern
 *                           every second and reports the first damaged
 *                           header (h) or word (p)
 * When the console hangs, the last broadcast says where; when the kernel
 * main loop stops, its counter stops.
 */
#ifndef SO_DIAG_H
#define SO_DIAG_H

#include "global.h"
#include "so_config.h"

/* Counted by the kernel's main loop (main.c). */
extern volatile u32 so_kernel_loops;

void so_diag_start(const SoConfig *config);

/* A word the PPC reads at every retrace (0xD30034E0; tools/ppc/exit.s): once
 * it says "EXIT" the game stops and hands over to the kernel, which closes
 * down and starts the Homebrew Channel (main.c, so_diag_launch_loader). 0
 * from every start (main.c: this memory outlives a restart). Set by the Online Play menu's Exit and Update (so_exi.c) and, in
 * tests (test_control=1), when the PC asks; there it is "ARMD" from the
 * start, which also sends the crash reporter back after a crash. */
#define SO_TEST_EXIT_WORD 0x130034E0	/* its own cache line: the kernel re-reads 0x13003420's constantly */
#define SO_TEST_ARMED 0x41524D44	/* "ARMD" */
#define SO_TEST_EXIT 0x45584954	/* "EXIT" */
#define SO_TEST_HANG 0x48414E47	/* "HANG": tests of what follows a hung PowerPC */
#define SO_TEST_RETRACES (SO_TEST_EXIT_WORD + 4)	/* counted by exit.s in test builds, logged */

/* The exit's steps on the PPC, after the kernel's own: 1 stopped, waiting
 * for the kernel to start the Homebrew Channel (tools/ppc/exit_to_loader.inc).
 * An exit of Nintendont's own (a USB controller's buttons) reports 3 to 7,
 * its reload stub's (resetstub/main.c): 5 into the Homebrew Channel's stub,
 * 6 and 7 IOS. */
#define SO_EXIT_STEP (SO_TEST_EXIT_WORD + 8)

/* Sends the game back to the Homebrew Channel: it stops at its next retrace,
 * and the kernel's exit then starts the Channel. `who` is for the log. */
void so_diag_request_exit(const char *who);

/* The kernel's exit is done (main.c): logs the PPC's steps, until it has
 * stopped for the kernel (SlippWii's own exit) or for 10 s. */
void so_diag_exit_watch(void);

/* After an exit asked for by so_diag_request_exit, and in tests
 * (test_control) after any exit or a hung PowerPC: the kernel has IOS start
 * the Homebrew Channel (the Wii Menu if there is none), which restarts the
 * console. Nothing after another exit: Nintendont's own finishes by itself. */
void so_diag_launch_loader(void);
/* What the kernel holds that a game could be waiting for: the disc command in hand, the file
 * system's lock, the prefetch (three log lines) */
void so_diag_where(void);

/* The next beacon text (NUL-terminated, at most 31 characters). Lock-free:
 * called from the broadcast thread. */
const char *so_diag_beacon(void);

#endif
