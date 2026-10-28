/* slippi_online/so_client.h
 * Slippi Online for the Nintendont kernel: the public entry points.
 *
 * so_client ties the pieces together: it loads the account, runs the
 * network thread that drives matchmaking and netplay, retries the way Slippi
 * Dolphin does, and forwards log lines to a PC when configured.
 *
 * Threads:
 *   network thread   waits on one wake event (a datagram arrived, a request
 *                    came in, or a timer), then services ENet under the lock
 *   callers          (the EXI device for the game, the self-test) take the
 *                    lock around netplay calls with so_client_lock(); pads
 *                    are sent from the caller's thread, so they leave at once
 *
 * No network traffic happens until the game asks for a match (or the
 * self-test starts), because traffic during Melee's boot crashed it.
 */
#ifndef SO_CLIENT_H
#define SO_CLIENT_H

#include "global.h"
#include "so_config.h"
#include "so_mm.h"
#include "so_netplay.h"

typedef enum {
	SO_CLIENT_OFF,	/* not initialised, or no usable account */
	SO_CLIENT_IDLE,
	SO_CLIENT_SEARCHING,	/* matchmaking */
	SO_CLIENT_CONNECTING,	/* reaching the opponents */
	SO_CLIENT_CONNECTED,
	SO_CLIENT_DISCONNECTED,	/* an opponent left, or we did */
	SO_CLIENT_ERROR,	/* see so_client_error() */
} SoClientState;

/* Once, after the kernel's network is up. Returns 0 when online play is
 * available (user.json loaded). */
int so_client_init(void);

/* True when Melee should run Slippi's online codeset (through Slippi's
 * bootloader, served by the EXI device) instead of the console codeset: an
 * account and /slippwii/netplay.gct are present, and the self-test is
 * off. Decided once, before the game is patched. */
int so_client_online_game(void);

const SoConfig *so_client_config(void);
/* Changes the online input delay by `step` frames (1-9) for the next game,
 * and saves it on the SD card (/slippwii/delay.txt, which overrides
 * config.ini's delay at start-up). Returns the delay now. */
int so_client_step_delay(int step);
const SoUser *so_client_user(void);

/* Starts matchmaking; any previous search or match ends first. */
void so_client_find_match(const SoSearch *search);
/* Leaves the queue or the match. */
void so_client_cancel(void);

SoClientState so_client_state(void);
const char *so_client_state_name(SoClientState state);
/* The message for SO_CLIENT_ERROR (empty otherwise). */
void so_client_error(char *out, int size);
/* The match found by matchmaking (valid from SO_CLIENT_CONNECTING on). */
const SoMatchResult *so_client_match(void);

/* False during Melee's boot: until the game asks for a match, or
 * boot_quiet_s seconds after start-up. Nothing is sent before then. */
int so_client_traffic_allowed(void);

/* The status for Slippi's LAN broadcast (32 bytes, as the nickname), or NULL
 * unless status_beacon=1. */
const char *so_client_beacon(void);
/* True with status_beacon=1: the broadcast then goes out every second. */
int so_client_diagnostics(void);

/* Applies config.ini-style lines on top of the current settings (the test
 * plan the PC sends to the self-test). */
void so_client_apply_settings(const char *text, u32 len);

/* Opens the session's UDP socket (matchmaking does this itself); `port`
 * receives its port. Returns 0 or < 0. */
int so_client_open_socket(u16 *port);

/* The kernel main loop's sleep per pass (main.c): 20 us, or online_loop_us
 * while online. */
extern volatile u32 so_main_loop_sleep_us;

/* Queues a pad for the network thread to send (pad NULL: resend what is not
 * acked), so the EXI device, on the kernel's main thread, never does
 * network I/O. Never blocks. */
void so_client_queue_pad(const SoLocalPad *pad);
/* The longest a queued pad waited to be sent since the last call, in us. */
u32 so_client_take_pad_latency_us(void);

/* Guards every so_np_* call made outside the network thread. */
void so_client_lock(void);
void so_client_unlock(void);

#endif
