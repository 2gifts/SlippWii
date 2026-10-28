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

/* The next beacon text (NUL-terminated, at most 31 characters). Lock-free:
 * called from the broadcast thread. */
const char *so_diag_beacon(void);

#endif
