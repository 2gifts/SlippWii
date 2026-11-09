/* slippi_online/so_config.h
 * Settings and the Slippi account, read once at start-up from the boot
 * drive:
 *
 *   /slippwii/user.json    Slippi Launcher's user.json, copied as is
 *   /slippwii/config.ini   optional key=value settings (README.md)
 *
 * The play key is a credential: it is only ever sent inside a create-ticket
 * to the configured matchmaking server, and never logged.
 */
#ifndef SO_CONFIG_H
#define SO_CONFIG_H

#include "global.h"

#define SO_USER_PATH   "/slippwii/user.json"
#define SO_TEST_USER_PATH "/slippwii/test-user.json"	/* the dummy test account */
#define SO_CONFIG_PATH "/slippwii/config.ini"

/* The netplay version Slippi Dolphin (Ishiiruka) reports; the server only
 * pairs compatible versions. */
#define SO_APP_VERSION_DEFAULT "3.6.4"
#define SO_MM_HOST_DEFAULT "mm.slippi.gg"
#define SO_MM_PORT_DEFAULT 43113

typedef struct {
	char uid[64];
	char play_key[96];
	char connect_code[16];	/* "ABCD#123" */
	char display_name[64];	/* UTF-8 */
	char latest_version[24];
} SoUser;

typedef struct {
	char user_path[64];	/* the account file (tests point it at a dummy) */
	char mm_host[64];
	u16 mm_port;
	u16 local_port;	/* 0: random in 41000-50999, as Dolphin picks */
	char app_version[24];
	u8 delay;	/* online input delay in frames, 1-9 (Slippi's default 2) */
	u32 lan_ip;	/* forced ipAddressLan; 0: this console's address */
	u32 log_host;	/* send log lines to this PC over UDP; 0: off */
	u16 log_port;
	u8 log_packets;	/* log every netplay packet (debugging) */
	/* Seconds after start-up with no traffic unless the game asks for a
	 * match (traffic during Melee's boot crashed it). */
	u8 boot_quiet_s;
	u8 status_beacon;	/* show the client state as the console nickname in
				 * Slippi's LAN broadcast (debugging) */
	/* 0: flush each pad ack at once, as Dolphin does. Otherwise hold it up
	 * to this long so it shares a datagram with our next pad (IOS sends are
	 * slow; see so_socket.h). */
	u16 ack_delay_us;
	/* 1: a pad packet is acked from the receive thread as it arrives (so_netplay.c, fast_ack_hook),
	 * not after the network thread and ENet have handled it */
	u8 fast_ack;
	/* Self-test (tests/README): Direct search and a synthetic match,
	 * started without the game. */
	u8 selftest;
	char selftest_code[16];
	u16 selftest_frames;
	u8 selftest_allow_real_mm;
	/* After the self-test: replay the asynchronous IOS sends that froze
	 * diagnostics builds 1-4 (without, then with the completion reply), to
	 * confirm the cause on hardware. */
	u8 selftest_async_probe;
	u8 selftest_stage_pause_s;	/* between the probe's stages (default 3) */
	u16 diag_step_pause_ms;	/* hold each announced start-up step (SO_STEP) */
	u8 selftest_memory_walk;	/* write and verify the free heap first */
	/* Experiment: a second probe round with the kernel's main thread at
	 * this priority instead of 0x50 (0: skip). */
	u8 diag_main_priority;
	/* The kernel main thread's priority while online (0: leave it at 0x50).
	 * It answers the game's polls, and the game waits for each with interrupts
	 * off. Below the network and disc threads (0x78) a third of them took over
	 * 2 ms, some 5, all of it gone from a frame's 16.7 ms: on a heavy stage, a
	 * frame shown twice, then one skipped. So above them all. The price is that
	 * a pass that wakes above IOS's network module (between 0x30 and 0x38)
	 * preempts its sends, which online_loop_us keeps down. */
	u8 online_main_priority;
	/* The main loop's sleep between passes while online, in microseconds
	 * (20 otherwise). A poll waits about this long at most, and is two passes
	 * but for so_main_loop_quick. Fountain of Dreams, no latency added:
	 *
	 *   priority, sleep    polls over 2 ms a game  ping      sendto
	 *   0x30, 200 us       556-756                 3.2 ms    1.1 ms
	 *   0x7C, 200 us       0                       4.7 ms    2.1 ms
	 *   0x7C, 500 us       4-9                     2.9-3.7   1.0 ms
	 *   0x7C, 1000 us      813-955                 4.3-5.2   1.0 ms */
	u16 online_loop_us;
	u8 online_net_priority;	/* the network thread's (ENet, pads to send); 0: 0x78 */
	u8 prefetch_off;	/* 1: no disc prefetch between games (so_prefetch) */
	u8 test_read_seek_ms;	/* tests: an online read's seek in ms (0: 2) */
	u8 test_read_pace;	/* tests: online reads at N times 15 MB/s (0: 4) */
	u8 test_no_frozen_assume;	/* tests: wait in lockstep for the opponent's frozen frames' pads */
	/* Rollback: savestate copies go on in the background while the game runs, its pages
	 * write-protected until copied (tools/rollback/rollback.c, bg_*). 0 off, 1 on, 2 (default)
	 * on while the ping is at least rollback_bg_ping_ms (80: a 30 ms connection reads 70 ms here, a 45 ms one 100): at 90 ms ping they halved the hitches
	 * a capture card saw, at 60 ms they cost about one in 12 minutes (2026-11-07). */
	u8 rollback_bg_saves;
	u16 rollback_bg_ping_ms;
	/* Rollback: staying level with the opponent (fine_sync) by reading the pad earlier first, up to
	 * 8 ms before the retrace, and only then by frames ending a line early (rollback.c, vi_nudge).
	 * 1 (default) so, 0 frames ending early only, 2 (tests) the pad read earlier only. */
	u8 rollback_nudge_poll;
	u16 rollback_sync_most_us;
	/* staying level: behind by this much (us, by the Wii's reckoning) it starts, ahead of
	 * minus this it stops (0: 2500 and 1500); frames end early 2 ms past the start */
	u16 rollback_nudge_on_us, rollback_nudge_off_us;	/* the earliest it reads the pad for that (0: the engine's 8000) */
	u8 chat;	/* quick chat in Direct (Slippi's default: on) */
	u8 rumble;	/* controller rumble in online play (default on; rumble.txt, set on the
		 * online CSS, wins over config.ini) */
	u8 ping_display;	/* the ping in a game's top left corner (default on; ping.txt, set on
		 * the online CSS, wins over config.ini) */
	u8 test_drop_pct;	/* tests: drop this share of outgoing pad packets */
	/* Rollback (in development): predict the opponent's inputs as Slippi
	 * Dolphin does, when the console savestate engine (tools/rollback) is in
	 * the boot codes. Default off: lockstep. */
	u8 rollback;
	/* tests: the game sees the opponent's inputs only this many frames
	 * late, so rollback predicts (and rolls back) every frame */
	u8 test_lag_frames;
	/* tests: every 5 s, the opponent's inputs held back this long, as in a
	 * lag spike: the game predicts to the rollback limit, waits there, then
	 * rolls back the most it can */
	u16 test_spike_ms;
	/* tests: at this frame the sender thread sticks, as a sendto that never
	 * returns has been seen to; the spare must carry the game on */
	u16 test_stuck_frame;
	u16 test_hang_frame;	/* tests: at this frame the PowerPC hangs (test_exit.s) */
	/* tests: each frame run also takes this long, so the game falls behind and runs
	 * queued inputs, as it does now and then at a high ping; with the capture check
	 * as Slippi has it (so_gct_fix_capture_check), that loses savestates */
	u8 test_slow_frame_ms;
	u8 test_old_capture_check;
	u8 test_file_sleep;	/* tests: the loop sleeps as ever while a file loads in a game (so_exi_file_busy) */
	u8 test_frozen_pads;	/* tests: the opponent does not wait for the Wii's load (send_frozen_pads) */
	u8 test_scratch_junk;	/* tests: the engine's part of the PPC scratch filled with junk at boot, as MEM2
				 * may hold after a power cut or another app (so_exi_scratch_boot) */
	u8 test_no_scratch;	/* tests: the disc cache keeps the scratch, as next to a big memory card */
	/* How long a poll waits for an input not here yet before the game predicts it:
	 * see input_wait in rollback.c. 0: predict at once, as Slippi Dolphin does. */
	u8 rollback_wait_ms;
	u16 rollback_poll_early_us;	/* the pad read this long before the retrace, to start with and at
					 * least while there are rollbacks (rollback.c, poll_early_on); 0: at it */
	u16 rollback_poll_deep_us;	/* and this much more while rollbacks are 2 frames or deeper (2500) */
	/* tests: the PC may send "SLPWII EXIT" (from log_host, to the session
	 * port) to send the console back to the Homebrew Channel, and a crash
	 * goes back there by itself (so_diag.h) */
	u8 test_control;
} SoConfig;

void so_config_defaults(SoConfig *config);
/* Applies config.ini text; unknown keys and bad values are logged and
 * skipped. Comment lines start with '#' or ';' (connect codes contain '#',
 * so only whole lines are comments). */
void so_config_parse(SoConfig *config, const char *text, u32 len);

/* Parses user.json; returns 0 when uid, playKey and connectCode are present. */
int so_user_parse(SoUser *user, const char *text, u32 len);

/* Reads both files from the boot drive. Returns 0 when user.json is usable;
 * the settings fall back to defaults either way. */
int so_config_load(SoConfig *config, SoUser *user);

/* True when the matchmaking host is Slippi's real server. */
int so_config_is_real_mm(const SoConfig *config);

#endif
