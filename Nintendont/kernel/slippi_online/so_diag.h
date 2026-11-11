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

/* Tests (test_control=1): a word the PPC reads (0xD30034E0), "ARMD" from the
 * start, "EXIT" once the PC asks: the boot codes' test exit
 * (tools/ppc/test_exit.s) and crash reporter then go back to the Homebrew
 * Channel, as Nintendont's own exit combo does. */
#define SO_TEST_EXIT_WORD 0x130034E0	/* its own cache line: the kernel re-reads 0x13003420's constantly */
#define SO_TEST_ARMED 0x41524D44	/* "ARMD" */
#define SO_TEST_EXIT 0x45584954	/* "EXIT" */
#define SO_TEST_HANG 0x48414E47	/* "HANG": tests of what follows a hung PowerPC */
#define SO_TEST_RETRACES (SO_TEST_EXIT_WORD + 4)	/* counted by test_exit.s, logged */

/* The exit's steps on the PPC, after the kernel's own: 1 let go, 2 into the
 * reload stub (tools/ppc/exit_to_loader.inc), 3 to 7 the stub's
 * (resetstub/main.c): 5 into the Homebrew Channel's stub, 6 and 7 IOS. */
#define SO_EXIT_STEP (SO_TEST_EXIT_WORD + 8)

/* The kernel's exit is done (main.c): logs the PPC's steps for 10 s. */
void so_diag_exit_watch(void);

/* Tests (test_control): the PPC did not reload IOS (the kernel still runs
 * after the watch): the kernel launches the Homebrew Channel itself. */
void so_diag_launch_loader(void);

/* The next beacon text (NUL-terminated, at most 31 characters). Lock-free:
 * called from the broadcast thread. */
const char *so_diag_beacon(void);

#endif
