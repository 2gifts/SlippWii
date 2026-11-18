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

/* Which Slippi this build is for: 0, Slippi 3.6.4; 1, the version after it (docs/NEXT_SLIPPI.md),
 * to be set when Slippi releases it. The one switch: the tools read this line
 * (tools/slippi_version.py) for the boot codes (the rollback engine is built for one version's
 * codes) and for Slippi's files and codes in a release (tools/make_release.py), and the defaults
 * below follow it. At run time the kernel goes by the version it reports (so_config_next) and by
 * mm_url, so a test build of either kind can speak either version. */
#ifndef SO_SLIPPI_NEXT
#define SO_SLIPPI_NEXT 1
#endif

/* The Slippi Dolphin version this build speaks (its netplay and its
 * matchmaking), which it reports to the matchmaking server. Slippi's server
 * turns away versions it no longer supports, and that is how a SlippWii
 * that Slippi has moved on from stops queueing: so against Slippi's server
 * this is the version, whatever config.ini says (so_config_finish), and it
 * changes only with a release checked against that Slippi version. */
#if SO_SLIPPI_NEXT
#define SO_APP_VERSION_DEFAULT "3.7.0"	/* (Slippi 3.7.0, released 2026-11-17: the development source this was built from, 7333463b, and three commits with nothing a console speaks) */
#else
#define SO_APP_VERSION_DEFAULT "3.6.4"
#endif
/* Slippi's matchmaking: up to 3.6.4 an ENet service (mm_host, mm_port; this is ""); after it a
 * WebSocket over TLS at this address (so_mm_ws.c). Against Slippi's server this too is the
 * build's, not a setting (so_config_finish). */
#if SO_SLIPPI_NEXT
#define SO_MM_URL_DEFAULT "wss://matchmaking.slippi.gg/v1/queue"
#else
#define SO_MM_URL_DEFAULT ""
#endif
/* The file the newest version Slippi's server has named is kept in
 * (so_client.c): with one over SO_APP_VERSION_DEFAULT the game's online
 * modes are locked ("update required"), as Slippi Dolphin locks its own. */
#define SO_LATEST_VERSION_PATH "/slippwii/slippi-version.txt"
/* How SlippWii names itself to matchmaking. Slippi's protocol has no field
 * for a client's name (3.6.4's create-ticket; the development source's
 * "join", 2026-11): where one goes is the Slippi team's to say, so none is
 * sent (0) until they do. 1: a "client" key in create-ticket
 * ("SlippWii/1.5.1"); 2: after the version, as semver build metadata
 * ("3.6.4+slippwii.1.5.1"). */
#define SO_CLIENT_NAME_MODE 0
#define SO_MM_HOST_DEFAULT "mm.slippi.gg"
#define SO_MM_PORT_DEFAULT 43113
/* The servers Slippi Dolphin asks for the netplay socket's public address
 * before it joins the WebSocket service's queue (SlippiMatchmaking.h, after
 * 3.6.4), in its order; the first two that answer are used. */
#define SO_STUN_HOSTS_DEFAULT "stun.l.google.com:19302,stun1.l.google.com:19302,stun.cloudflare.com:3478,global.stun.twilio.com:3478"

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
	/* Slippi's WebSocket matchmaking service (Slippi Dolphin after 3.6.4):
	 * "wss://host[:port]/path". "": the ENet service at mm_host, as 3.6.4. */
	char mm_url[96];
	char stun_hosts[112];	/* "name:port,..." (SO_STUN_HOSTS_DEFAULT); tests name their own */
	u32 test_mm_addr;	/* tests: connect here instead of looking mm_url's host up (the certificate is still checked for the name) */
	u8 test_tls_root[65];	/* tests: one more trusted root's P-256 key (so_tls.h), never used against Slippi's hosts */
	u8 test_tls_root_set;
	/* tests: a folder of the card looked in first for the codeset (netplay.gct) and for Slippi's
	 * files (files/NAME), ahead of /slippwii: another Slippi version's, put there by the updater's
	 * test mode, beside the player's own installed files */
	char test_files_root[40];
	u16 local_port;	/* 0: random in 41000-50999, as Dolphin picks */
	char app_version[24];
	u8 client_name_mode;	/* SO_CLIENT_NAME_MODE; tests set it (test_client_name) */
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
	u8 rollback_direct;	/* frames on the screen a refresh sooner (so_exi.c, direct_wanted): 0 never; 1 while the game's frames fit, with the skip in the countdown under rollback_direct_ping_ms; 2 the same, the skip in the countdown at any ping; 3 always */
	u16 rollback_direct_ping_ms;
	u16 rollback_direct_try_ms;
	u16 rollback_direct_early_ms;	/* and up to this much over it, the game begun with the pad read an answer earlier (8.3 ms more: rollback.c, direct_track's step 1); 0: none */	/* and up to this much over it, only against an opponent whose last game was counted and fits (so_pads.h, so_direct_mode); 0: no such band */
	u8 rollback_bg_saves;
	u16 rollback_bg_ping_ms;
	/* Rollback: staying level with the opponent (fine_sync) by reading the pad earlier first, up to
	 * 8 ms before the retrace, and only then by frames ending a line early (rollback.c, vi_nudge).
	 * 1 (default) so, 0 frames ending early only, 2 (tests) the pad read earlier only. */
	u8 rollback_nudge_poll;
	/* Tests: the pad read 15 ms before the retrace, after the SI's first answer of the field, which it
	 * then plays (rollback.c, POLL_ANSWER_BEFORE): 8.3 ms more lag, 8 ms more for each frame's work. */
	u8 rollback_answer_early;
	u16 rollback_sync_most_us;
	/* staying level: behind by this much (us, by the Wii's reckoning) it starts, ahead of
	 * minus this it stops (0: 2500 and 1500); frames end early 2 ms past the start */
	u16 rollback_nudge_on_us, rollback_nudge_off_us;	/* the earliest it reads the pad for that (0: the engine's 8000) */
	u8 input_suggestions;	/* (3.7.0) predict an opponent's inputs with their input suggestions, as Slippi Dolphin: 1 (default), 0 never */
	u8 chat;	/* quick chat: 1 on (Slippi's default), 2 in Direct only, 0 off (so_exi.c, chat_on) */
	u8 rumble;	/* controller rumble in online play (default on; rumble.txt, set on the
		 * online CSS, wins over config.ini) */
	u8 ping_display;	/* the ping in a game's top left corner (default on; ping.txt, set on
		 * the online CSS, wins over config.ini) */
	u8 test_drop_pct;	/* tests: drop this share of outgoing pad packets */
	/* Rollback: predict the opponent's inputs as Slippi Dolphin does, with
	 * the console savestate engine (tools/rollback) in the boot codes.
	 * Default on since 1.4.0. 0: lockstep, the game waits for late inputs
	 * (as 1.3 did, and as a game does next to a big memory card: ISO.c). */
	u8 rollback;
	/* tests: the game sees the opponent's inputs only this many frames
	 * late, so rollback predicts (and rolls back) every frame */
	u8 test_lag_frames;
	u8 multi_delay_room_ms;	/* more than one opponent: the delay's room past the one-way time (so_exi.c, lobby_delay) */
	u8 multi_stream;	/* more than one opponent: the game's replay data goes to a mirroring PC a send a frame (1),
				 * one send in this many ms (2 to 250), or not while the game runs (0)
				 * (SlippiNetwork.c, streamHolds) */
	u8 test_multi_rollback;	/* tests: roll back against more than one opponent too (so_exi.c, rollback_wanted) */
	/* tests: every 5 s, the opponent's inputs held back this long, as in a
	 * lag spike: the game predicts to the rollback limit, waits there, then
	 * rolls back the most it can */
	u16 test_spike_ms;
	u16 test_spike_every;	/* tests: a spike every this many frames (300: 5 s) */
	/* tests: at this frame the sender thread sticks, as a sendto that never
	 * returns has been seen to; the spare must carry the game on */
	u16 test_stuck_frame;
	u16 test_hang_frame;	/* tests: at this frame the PowerPC hangs (exit.s) */
	u8 test_logfile_dump;	/* tests: at the online CSS, the newest file of the log's folder on the card is read back and its
				 * first lines go to the PC's log: what a boot before wrote is really there (so_exi.c, logfile_dump).
				 * 2: the rest of the file as well, without the stall profiler's lines (400 lines at most) */
	u16 test_crash_frame;	/* tests: at this frame the kernel does what it does with a crash report, the game
				 * going on: the crash file on the card (so_logfile.c) */
	u16 test_start_limit_ms;	/* tests: a game set up this long ago with no poll yet counts as one that never
				 * started (20 s without it), so a game that starts leaves that report (so_exi.c) */
	u16 test_desync_frame;	/* tests: from this frame an opponent's checksum is taken for another: the desync
				 * file on the card, of a game that is in step (so_exi.c, check_sums) */
	/* tests: each frame run also takes this long, so the game falls behind and runs
	 * queued inputs, as it does now and then at a high ping; with the capture check
	 * as Slippi has it (so_gct_fix_capture_check), that loses savestates */
	u8 test_slow_frame_ms;
	u8 test_old_capture_check;
	u8 test_lag_reduction;	/* tests: Slippi's codeset with another lag reduction code: 1 none, 2 the polling drift fix alone (offline games only) */
	u8 test_one_a_loop;	/* (read and not used: the engine's test of two waiting polls run one a loop is out again, rollback.c) */
	u8 test_file_sleep;	/* tests: the loop sleeps as ever while a file loads in a game (so_exi_file_busy) */
	u8 test_frozen_pads;	/* tests: the opponent does not wait for the Wii's load (send_frozen_pads) */
	u8 test_scratch_junk;	/* tests: the engine's part of the PPC scratch filled with junk at boot, as MEM2
				 * may hold after a power cut or another app (so_exi_scratch_boot) */
	u8 test_no_scratch;	/* tests: the disc cache keeps the scratch, as next to a big memory card */
	u16 test_mark_page;	/* tests: this MEM1 page's mark is set at boot, as a session before may have left it
				 * (so_exi_scratch_boot; 1017, 0x803F9: the HUD's table, written at the countdown's end) */
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
	u8 test_wifi;	/* tests: the console counts as on Wi-Fi (so_client.c, on_wifi) */
	/* tests: the version a test server named in an earlier run is read at
	 * start-up, as a console reads Slippi's (otherwise a test's leftover would
	 * lock every later test out of online) */
	u8 test_latest_keep;
} SoConfig;

void so_config_defaults(SoConfig *config);
/* 1 when the version spoken (app_version) is past 3.6.4: Slippi's netplay
 * and its game code changed with 3.6.5 (9-byte pads, larger 0xB0 and 0xB3
 * answers, drop reports: docs/NEXT_SLIPPI.md), and the two cannot be mixed. */
int so_config_next(const SoConfig *config);
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

/* After the settings are read: what no file may change against Slippi's
 * server (the version reported and the client's name) is as compiled.
 * Returns 1 when a setting was put back. */
int so_config_finish(SoConfig *config);

#endif
