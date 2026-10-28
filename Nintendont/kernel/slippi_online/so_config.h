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
	 * At 0x50 its polling loop starves IOS's network module: round trips
	 * took 39 ms and sends up to 11 ms; at 0x30, 2.8 ms and 0.9 ms. */
	u8 online_main_priority;
	/* The main loop's sleep between passes while online, in microseconds
	 * (20 otherwise). With the loop above the network threads, Melee's EXI
	 * requests wait at most about this long, and the network threads and
	 * IOS have the CPU in between. */
	u16 online_loop_us;
	u8 chat;	/* quick chat in Direct (Slippi's default: on) */
	u8 test_drop_pct;	/* tests: drop this share of outgoing pad packets */
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
