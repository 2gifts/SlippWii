/* slippi_online/so_exi.c
 * Slippi Dolphin's online EXI device (see so_exi.h). Function names in
 * comments refer to CEXISlippi in Slippi Dolphin's EXI_DeviceSlippi.cpp,
 * which this follows; byte layouts are in docs/EXI_DEVICE_SPEC.md (repository root).
 */
#define SO_FILE_ID "exi"
#include "so_exi.h"
#include "so_diag.h"
#include "so_alloc.h"
#include "so_client.h"
#include "so_files.h"
#include "so_logfile.h"
#include "so_mq.h"
#include "so_netplay.h"
#include "so_prefetch.h"
#include "so_socket.h"
#include "so_platform.h"
#include "so_selftest.h"	/* so_encode_connect_code */
#include "so_text.h"
#include "so_tls.h"
#include "so_util.h"
#include "string.h"

enum {
	CMD_ONLINE_INPUTS = 0xB0,
	CMD_CAPTURE_SAVESTATE = 0xB1,
	CMD_LOAD_SAVESTATE = 0xB2,
	CMD_GET_MATCH_STATE = 0xB3,
	CMD_FIND_OPPONENT = 0xB4,
	CMD_SET_MATCH_SELECTIONS = 0xB5,
	CMD_OPEN_LOGIN = 0xB6,
	CMD_LOGOUT = 0xB7,
	CMD_UPDATE = 0xB8,
	CMD_GET_ONLINE_STATUS = 0xB9,
	CMD_CLEANUP_CONNECTION = 0xBA,
	CMD_SEND_CHAT_MESSAGE = 0xBB,
	CMD_GET_NEW_SEED = 0xBC,
	CMD_REPORT_GAME = 0xBD,
	CMD_FETCH_CODE_SUGGESTION = 0xBE,
	CMD_OVERWRITE_SELECTIONS = 0xBF,
	CMD_GP_COMPLETE_STEP = 0xC0,
	CMD_GP_FETCH_STEP = 0xC1,
	CMD_REPORT_SET_COMPLETE = 0xC2,
	CMD_GET_PLAYER_SETTINGS = 0xC3,
	CMD_REPORT_MATCH_STATUS_UPDATE = 0xC4,
	CMD_LOG_MESSAGE = 0xD0,
	CMD_FILE_LENGTH = 0xD1,
	CMD_FILE_LOAD = 0xD2,
	CMD_GCT_LENGTH = 0xD3,
	CMD_GCT_LOAD = 0xD4,
	CMD_GET_DELAY = 0xD5,
	CMD_PLAY_MUSIC = 0xD6,
	CMD_STOP_MUSIC = 0xD7,
	CMD_CHANGE_MUSIC_VOLUME = 0xD8,
	CMD_PREMADE_TEXT_LENGTH = 0xE1,
	CMD_PREMADE_TEXT_LOAD = 0xE2,
	CMD_GET_RANK = 0xE3,
	CMD_FETCH_RANK = 0xE4,
	CMD_GET_RANK_VISIBILITY = 0xE5,
	/* Ours: Melee's crash report (tools/ppc/crash_report.s). */
	CMD_CRASH_REPORT = 0xDF,
	CMD_HEARTBEAT = 0xDE,	/* diagnostics (tools/ppc/heartbeat.s) */
	CMD_PROBE = 0xDD,	/* diagnostics (tools/ppc/probe_*.s) */
	/* Wii only: [0 off, 1 on, 0xFF read] -> PING_ANSWER_SIZE bytes: [the setting,
	 * opponents, then each one's ping in ms, u16 (PING_NONE: none yet)]. The online
	 * CSS's switch sets it; the display in a game reads it (tools/ping_display/). */
	CMD_PING_DISPLAY = 0xEF,
	CMD_TEXT_REPORT = 0xDA,	/* Melee's panics and OSReport (tools/ppc/text_report.s) */
	CMD_SET_DELAY = 0xDC,	/* Wii only: [step] -> [delay], the online CSS's delay buttons (tools/css_delay/) */
	CMD_SET_RUMBLE = 0xDB,	/* Wii only: [0 off, 1 on, 0xFF read] -> [rumble], the online CSS's rumble switch */
	/* Wii only: [engine version, 0 ask / 1 ready / 2 failed] -> [roll back
	 * this game]: the console savestate engine (tools/rollback) at each online
	 * game's first frame. Only a game whose engine said ready predicts the
	 * opponent's inputs; the rest wait for them (lockstep). */
	CMD_ROLLBACK = 0xD9,
	/* Wii only: as 0xB0's request, no answer: this pad, for the frame the next
	 * 0xB0 will be, goes to the opponent now. The engine sends it when Slippi
	 * puts that 0xB0 off for a rollback, which takes a Wii far longer than a PC
	 * (tools/rollback/rollback.c, rb_early_pad). */
	CMD_EARLY_PAD = 0xE0,
};

/* ONLINE_INPUTS results (the ASM's RESP_*). */
enum { RESP_NORMAL = 1, RESP_SKIP = 2, RESP_DISCONNECTED = 3, RESP_ADVANCE = 4 };

/* SlippiMatchmaking::ProcessState, as the game reads it. */
enum { MM_IDLE = 0, MM_INITIALIZING = 1, MM_MATCHMAKING = 2, MM_OPPONENT_CONNECTING = 3, MM_CONNECTION_SUCCESS = 4,
	MM_ERROR = 5 };

/* The 0xB0's request and answer and the 0xB3's answer, as Slippi 3.6.4's game code has them and as
 * 3.6.5's does, under Slippi's own names where the two differ (Online.s; docs/NEXT_SLIPPI.md).
 * Which one this build speaks is a setting (so_config_next), as the codeset on the card must be the
 * same version's. tools/check_exi_layout.py, run with every build, holds these numbers to Slippi's
 * source: one byte off in a field that is 0 in every test goes unseen otherwise. */
#define TXB_DELAY 13
#define TXB_PAD 14			/* 12 bytes */
#define TXB_INPUT_SOURCE_INDEX 26	/* (3.6.5) the port the controller is in */
#define TXB_IS_LEAVING 27		/* (3.6.5) the player chose to leave the game */
#define TXB_SIZE_364 26
#define TXB_SIZE_NEXT 28
#define RXB_SIZE_364 297
#define RXB_SIZE_NEXT 559
#define MSRB_SIZE_364 962
#define MSRB_SIZE_NEXT 1030
#define RXB_SIZE (next_mode() ? RXB_SIZE_NEXT : RXB_SIZE_364)
#define MSRB_SIZE (next_mode() ? MSRB_SIZE_NEXT : MSRB_SIZE_364)
/* in the 3.6.5 answer, after the opponents' pads (RXB_DESPAWN_FRAMES, RXB_OPNT_PREDICTED_INPUTS,
 * RXB_NEXT_PAD_RNG_BITS) and in its match state (MSRB_IS_BOT) */
#define RXB_DESPAWN 0x126
#define RXB_PREDICTED 0x132
#define RXB_NEXT_RNG 0x22E
#define MSRB_IS_BOT 0x402

static int next_mode(void)
{
	return so_config_next(so_client_config());
}
#define OSB_SIZE 42
#define PLAYER_SETTINGS_SIZE 3264
#define ROLLBACK_MAX_FRAMES 7
#define PAD_FULL_SIZE 12
#define MATCH_BLOCK_SIZE 0x138
#define STALL_LIMIT_US 7000000	/* Dolphin gives up after 420 stalled frames */
#define TIME_SYNC_INTERVAL 30	/* SLIPPI_ONLINE_LOCKSTEP_INTERVAL */
#define FRAME_US 16683

/* Payload sizes (bytes after the command byte): Dolphin's payload_sizes,
 * EXI_DeviceSlippi.h. 0 marks a command so_exi does not handle. */
static u16 payload_size(u8 cmd)
{
	switch (cmd) {
	case CMD_ONLINE_INPUTS: return (next_mode() ? TXB_SIZE_NEXT : TXB_SIZE_364) - 1;	/* (after the command's byte) */
	case CMD_CAPTURE_SAVESTATE:
	case CMD_LOAD_SAVESTATE: return 32;
	case CMD_FIND_OPPONENT: return 19;
	case CMD_SET_MATCH_SELECTIONS: return 9;
	case CMD_SEND_CHAT_MESSAGE: return 2;
	case CMD_REPORT_GAME: return 368;
	case CMD_FETCH_CODE_SUGGESTION: return 31;
	case CMD_OVERWRITE_SELECTIONS: return 14;
	case CMD_GP_COMPLETE_STEP: return 5;
	case CMD_GP_FETCH_STEP:
	case CMD_REPORT_SET_COMPLETE:
	case CMD_REPORT_MATCH_STATUS_UPDATE:
	case CMD_CHANGE_MUSIC_VOLUME: return 1;
	case CMD_LOG_MESSAGE: return 127;	/* the logf buffer: 128 bytes */
	case CMD_FILE_LENGTH:
	case CMD_FILE_LOAD: return 0x40;
	case CMD_GCT_LOAD: return 4;
	case CMD_PLAY_MUSIC: return 8;
	case CMD_PREMADE_TEXT_LENGTH:
	case CMD_PREMADE_TEXT_LOAD: return 2;
	case CMD_CRASH_REPORT: return 0xBF;
	case CMD_PROBE: return 0x3F;
	case CMD_HEARTBEAT: return 0x5F;
	case CMD_PING_DISPLAY: return 1;
	case CMD_SET_DELAY: return 1;
	case CMD_SET_RUMBLE: return 1;
	case CMD_ROLLBACK: return 2;
	case CMD_EARLY_PAD: return (next_mode() ? TXB_SIZE_NEXT : TXB_SIZE_364) - 1;	/* (as CMD_ONLINE_INPUTS) */
	case CMD_TEXT_REPORT: return 0x7F;
	default: return 0;
	}
}

int so_exi_online_mode(void)
{
	return so_client_online_game();
}

int so_exi_is_online_command(u8 cmd)
{
	return (cmd >= CMD_ONLINE_INPUTS && cmd <= CMD_REPORT_MATCH_STATUS_UPDATE) ||
		(cmd >= CMD_LOG_MESSAGE && cmd <= CMD_CHANGE_MUSIC_VOLUME) || cmd == CMD_PING_DISPLAY ||
		(cmd >= CMD_PREMADE_TEXT_LENGTH && cmd <= CMD_GET_RANK_VISIBILITY) || cmd == CMD_CRASH_REPORT || cmd == CMD_HEARTBEAT ||
		cmd == CMD_PROBE || cmd == CMD_TEXT_REPORT || cmd == CMD_SET_DELAY ||
		cmd == CMD_SET_RUMBLE || cmd == CMD_ROLLBACK || cmd == CMD_EARLY_PAD;
}

u32 so_exi_request_size(u8 cmd)
{
	return 1 + payload_size(cmd);
}

/* Dolphin's starting match block: a VS match, P1 red Falco vs P2 red Bowser
 * (P3/P4 Young Link, not playing) on Battlefield. It is patched, never rebuilt:
 * the game reads the last stage played from it after a game. */
static const u8 default_match_block[MATCH_BLOCK_SIZE] = {
	0x32, 0x01, 0x86, 0x4C, 0xC3, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0xFF, 0xFF, 0x6E, 0x00,
	0x1F, 0x00, 0x00, 0x01, 0xE0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x3F,
	0x80, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x09,
	0x00, 0x78, 0x00, 0xC0, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x05, 0x00, 0x04,
	0x01, 0x00, 0x01, 0x00, 0x00, 0x09, 0x00, 0x78, 0x00, 0xC0, 0x00, 0x04, 0x01, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F,
	0x80, 0x00, 0x00, 0x15, 0x03, 0x04, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x09, 0x00, 0x78, 0x00,
	0xC0, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00,
	0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x15, 0x03, 0x04, 0x00, 0x00, 0xFF,
	0x00, 0x00, 0x09, 0x00, 0x78, 0x00, 0xC0, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00,
	0x21, 0x03, 0x04, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x09, 0x00, 0x78, 0x00, 0x40, 0x00, 0x04,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80,
	0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x21, 0x03, 0x04, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x09,
	0x00, 0x78, 0x00, 0x40, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00,
};

/* Fallback stages: FoD, Pokemon Stadium, Yoshi's Story, Dream Land,
 * Battlefield, Final Destination. */
static const u16 default_stages[] = { 0x2, 0x3, 0x8, 0x1C, 0x1F, 0x20 };

#define MAX_STAGES 32

static struct {
	/* the answer for the next read (Dolphin's m_read_queue): small ones in
	 * `small`, the rest in `big`, from the online heap (none without the
	 * online client) */
	u8 small[48];
	u8 *big;
	const u8 *resp;
	u32 resp_len;
	int has_resp;
	/* 0xD2: the answer is this file, read from the drive by the read */
	int file_pending;
	char file[0x41];
	/* A main-thread command's answer not read yet, set aside while a command from an
	 * interrupt (the pad's renewal: 0xB0, 0xE0) is answered (see so_exi_write) */
	struct {
		int valid, file_pending;
		u32 len;
		u8 *buf;	/* (a copy: the interrupt's answer may reuse `big`) */
		char file[0x41];
	} stash;
	u32 stashes, unstashed;
	int answer_read_main;	/* the main thread's last command's answer has been read */

	SoSelections local_sel;	/* local_selections */
	u8 mode;	/* last_search.mode */
	u8 forced_error;	/* the lobby ended over the local player's character (Dolphin's forcedError) */
	u8 match_block[MATCH_BLOCK_SIZE];
	u8 local_idx, remote_idx;	/* m_local_player_idx / m_remote_player_idx */
	int have_client;	/* Dolphin holds slippi_netplay */
	u8 said_outdated;	/* (the log has said the game was told "update required") */
	u16 allowed[MAX_STAGES];
	int allowed_count;
	u16 pool[MAX_STAGES];
	int pool_count;
	u8 sent_chat;	/* remote_sent_chat_message_id */

	s32 last_frame;	/* of the previous ONLINE_INPUTS */
	int rb_ready;	/* the savestate engine is ready for the next game (CMD_ROLLBACK) */
	int replay_on;	/* its replay ring (so_exi_replay_next) */
	u32 replay_tail, replay_told;
	int dummies_sent;	/* the delay frames' empty pads, this game */
	u64 stall_since_us;
	/* What the last 0xB0 sends once the game has read its answer. */
	struct {
		int valid, has_pad, delay_pads;
		u8 delay;
		s32 delay_from;	/* the first of the delay frames' pads not sent yet */
		SoLocalPad pad;
	} pend;
	int answer_read;	/* the game read the last answer */

	/* This game's netcode, logged when it ends: frames played, polls
	 * answered SKIP (each one a whole frame the game waited), frames that
	 * waited at all, the longest wait for one frame, and when it started. */
	struct {
		u32 frames, skips, held, longest, run;
		u64 start_us;
		u32 max_send_us, replay_writes;
		/* The first frames that waited for the opponent: frame, wait in ms. */
		u32 wait_frame[8], wait_ms[8];
		u32 waits_logged;
		u64 wait_since_us;
		/* The game's own frame pacing: the time between 0xB0 polls (one
		 * per frame: over 20 ms means it missed a refresh), and how long
		 * the device took to answer (with the client lock's wait). */
		u64 last_poll_us;
		u32 slow_polls, max_gap_us;
		u32 gap_logs;	/* "poll gap" lines so far */
		/* the polls over 20 ms after the one before, in play: frame << 8 | ms (the report's) */
		u32 slow_at[24], slow_n;
		/* In rollback games: how far each 0xB0 poll came from a 60th of a
		 * second after the last (Slippi sends the inputs from the controller
		 * alarm, every 60th): within 0.5, 1, 2, 4, 8 ms, more */
		u32 spacing[6];
		u32 max_answer_us, slow_answers, max_lock_us;
		/* How far the opponent's inputs were behind at each frame's first
		 * poll: the frames a rollback client would have predicted (0-7, 8
		 * = 8 or more; docs/research/ROLLBACK.md). */
		u32 depth[9];
		s32 depth_frame;
		SoMq mq;	/* Slippi's match quality rule, for the log (so_mq.h) */
		u8 mq_logs;
		/* Rollback (the engine said ready): Dolphin's time sync, the
		 * frames answered with predictions, the stalls at the rollback
		 * limit, the time sync's skipped and advanced frames. */
		int rb;
		s32 seen[3];	/* the opponents' latest frame the game was given */
		u64 short_since_us[3];	/* more than one opponent: since when each one's inputs have held the game */
		u64 spike_until_us;	/* tests: test_spike_ms */
		s32 spike_frame, spike_head[3];
		int to_skip, skipping, to_advance, advancing;
		int catching, frozen_advanced;
		u64 early_us;	/* when the early pad came: its poll's real time */
		u32 frozen_advances;
		u32 predicted, limit_stalls, sync_skips, advances;
		/* Time the game has fallen behind the clock (catch_up), the frames
		 * made up so far, and whether the next poll is an advance's extra one */
		s32 behind_us;
		u32 caught_up;
		s32 caught_up_frame;
		int extra_poll;
		/* The offset from the opponent (time_sync_advance), summed every 30
		 * frames for the fine sync and the report, and the frames the fine
		 * sync advanced */
		s32 fine_sum, fine_n, offset_sum, offset_n;
		u32 fine_advances;
		u32 nudging, nudges, nudges_far;
		u32 frozen_assumed;	/* frames run on the opponent's frozen pads, not yet here */
		int reported;	/* its end logged (CMD_REPORT_GAME); its last frames still run */	/* fine_sync: asked (2: frames ending early too), how often */
		/* Pads sent ahead of their 0xB0 (CMD_EARLY_PAD): the frame and the pad
		 * still to come in a 0xB0, how many, how many its 0xB0 did not match;
		 * and the last 0xB0's finalized frame and checksum, which they carry */
		s32 early_frame;
		u8 early_pad[SO_PAD_SIZE];
		u32 early_pads, early_wrong;
		s32 last_finalized;
		u32 last_checksum;
		/* Desync detection (check_sums) */
		s32 last_checked;
		u32 checks, desyncs;
		s32 first_desync;
		u32 hard, near, soft;	/* the desyncs as Slippi's ASM sees them: positions off (ends the game), off by 1, action states only */
		u32 first_ours, first_theirs;
		u8 setup_said;	/* say_setup's line is in the log */
		u8 gone_said;	/* a bit an opponent: fill_gone's line is */
		u8 left_said;	/* every opponent gone in the middle of the game: kept for a report */
	} game;
	int initialised;
	u32 setup_ms;	/* a game was set up then (setup_on), and has had no poll yet (so_exi_start_watch) */
	u8 setup_on;	/* 1: waiting for that poll; 2: said that it has not come (once a game) */
	int drive_runs;	/* the drive's runs since the last game still to log, a line a poll */
	u8 poll_delay;	/* the last 0xB0's delay (diagnostics: a change is logged) */
	u8 rng_bits[16], rng_left;	/* (3.6.5) random bytes for RXB_NEXT_RNG, one an answer */
	/* so_prefetch may use the engine's scratch again: once this game is over, and
	 * the engine (when it was asked to run) has reported, so is done with it */
	int pf_engine, pf_game_over;

	SoExiStats stats;
} x;

static void init_once(void)
{
	if (x.initialised)
		return;
	memcpy(x.match_block, default_match_block, sizeof(x.match_block));
	memcpy(x.allowed, default_stages, sizeof(default_stages));
	x.allowed_count = sizeof(default_stages) / sizeof(default_stages[0]);
	x.remote_idx = 1;
	x.initialised = 1;
}

/* A zeroed answer of `len` bytes, or NULL (and no answer) when a big one
 * has no buffer. */
static u8 *answer(u32 len)
{
	u8 *b = x.small;
	x.file_pending = 0;
	if (len > sizeof(x.small)) {
		if (!x.big)
			x.big = (u8 *)so_malloc(PLAYER_SETTINGS_SIZE);
		b = len <= PLAYER_SETTINGS_SIZE ? x.big : NULL;
	}
	x.has_resp = b != NULL;
	if (!b)
		return NULL;
	memset(b, 0, len);
	x.resp = b;
	x.resp_len = len;
	return b;
}

/* SlippiPlayerSelections::Reset (keeps the alt stage mode). */
static void reset_selections(SoSelections *s)
{
	u8 alt = s->alt_stage_mode;
	memset(s, 0, sizeof(*s));
	s->alt_stage_mode = alt;
}

/* SlippiPlayerSelections::Merge */
static void merge_selections(SoSelections *into, const SoSelections *s)
{
	into->rng_offset = s->rng_offset;
	if (s->is_stage_selected) {
		into->stage_id = s->stage_id;
		into->is_stage_selected = 1;
		into->alt_stage_mode = s->alt_stage_mode;
	}
	if (s->is_character_selected) {
		into->character_id = s->character_id;
		into->character_color = s->character_color;
		into->team_id = s->team_id;
		into->is_character_selected = 1;
	}
}

/* getRandomStage: draws from the pool without replacement. */
static u16 random_stage(void)
{
	int i;
	u16 stage;
	if (x.pool_count == 0) {
		for (i = 0; i < x.allowed_count; i++)
			x.pool[i] = x.allowed[i];
		x.pool_count = x.allowed_count;
	}
	if (x.pool_count == 0)
		return 0x1F;
	i = (int)(so_random() % (u32)x.pool_count);
	stage = x.pool[i];
	x.pool[i] = x.pool[--x.pool_count];
	return stage;
}

/* The match players are known once matchmaking has paired us. */
static const SoMatchResult *current_match(void)
{
	SoClientState st = so_client_state();
	if (st == SO_CLIENT_CONNECTING || st == SO_CLIENT_CONNECTED || st == SO_CLIENT_DISCONNECTED)
		return so_client_match();
	return NULL;
}

/* False without an account or a network: the client (and its lock) may
 * not exist, the game's online modes stay locked, and nothing here touches
 * the client. */
static int client_up(void)
{
	return so_client_state() != SO_CLIENT_OFF;
}

/* handleConnectionCleanup */
static void cleanup(void)
{
	if (client_up())
		so_client_cancel();
	reset_selections(&x.local_sel);
	x.pool_count = 0;
	x.have_client = 0;
	x.forced_error = 0;
	x.sent_chat = 0;
	x.last_frame = 0;	/* the next frame 1 starts a game */
	x.setup_on = 0;
}

/* ---- 0xEF PING_DISPLAY ----------------------------------------------------- */

#define PING_ANSWER_SIZE (2 + 2 * SO_REMOTE_MAX)
#define PING_NONE 0xFFFF
#define PING_MAX_MS 9999

/* Slippi Dolphin's ping display: the last pad's round trip, per opponent. No
 * client lock: a game's frame must not wait for the network thread, and these
 * are whole words, each written in one store. */
static void answer_ping_display(u8 request)
{
	u8 *r = answer(PING_ANSWER_SIZE);
	int i, remotes = so_np_remote_count();
	if (!r)
		return;
	r[0] = (u8)so_client_set_ping_display(request);
	r[1] = (u8)remotes;
	for (i = 0; i < SO_REMOTE_MAX; i++) {
		u32 us = i < remotes ? so_np_ping_us(i) : 0;
		u32 ms = us / 1000;
		so_put_u16(r + 2 + i * 2, !us ? PING_NONE : ms > PING_MAX_MS ? PING_MAX_MS : (u16)ms);
	}
}

/* ---- 0xB0 ONLINE_INPUTS (handleOnlineInputs, lockstep) -------------------- */

/* The opponent's answer block: frame numbers, checksums and up to 7 pads per
 * remote, newest first (prepareOpponentInputs). The client lock is held. */
/* An opponent of several that has left (prepareOpponentInputs, Slippi Dolphin 3.6.4): the game is
 * told it has inputs as far as the newest of those still here, so nobody waits for it; its checksum
 * is that one's; its pads are the last it sent, as Dolphin's queue holds them (from the finalized
 * frame on, the newest always, 7 at most), read from where the newest opponent's frame is against
 * the game's: past them, no input; and the game takes its character out (the byte after the pads)
 * from the first frame that is a multiple of 30 and 16 frames (twice the rollback limit and 2) past
 * its last input, the same frame on every console that had the same last input from it. */
#define GAME_FIRST_FRAME (-123)
static void fill_gone(u8 *r, int i, s32 frame, s32 finalized, s32 head, s32 newest, s32 cf, u32 cs, s32 *num)
{
	s32 lo = finalized < head ? finalized : head, top = head < lo + ROLLBACK_MAX_FRAMES - 1 ? head : lo + ROLLBACK_MAX_FRAMES - 1;
	s32 despawn = (top + 2 * ROLLBACK_MAX_FRAMES + 2 + 29) / 30 * 30, off = newest > frame ? newest - frame : 0;
	u8 *pads = r + 0x2A + i * ROLLBACK_MAX_FRAMES * PAD_FULL_SIZE;
	int k;
	so_put_u32(r + 0x02 + i * 8, (u32)cf);
	so_put_u32(r + 0x06 + i * 8, cs);
	*num = newest < frame ? newest : frame;
	if (!(x.game.gone_said & (1 << i))) {
		x.game.gone_said |= (u8)(1 << i);
		so_log("exi: player %u has left at frame %d: its inputs end at frame %d, it is out of the game from frame %d",
			(unsigned)so_np_remote_player_idx(i) + 1, (int)frame, (int)head, (int)despawn);
	}
	for (k = 0; k < ROLLBACK_MAX_FRAMES && off + k <= top - lo; k++)
		so_np_remote_pad(i, top - off - k, pads + k * PAD_FULL_SIZE);
	if (next_mode())
		so_put_u32(r + RXB_DESPAWN + i * 4, (u32)despawn);	/* (3.6.5: the frame itself; the game compares) */
	else
		r[RXB_DESPAWN + i] = frame >= despawn;
}

static void fill_remote_inputs(u8 *r, s32 frame, s32 finalized, const s32 *heads, int remotes)
{
	s32 nums[3], smallest, newest = GAME_FIRST_FRAME - 1, newest_cf = 0;
	u32 newest_cs = 0;
	u8 gone[3] = { 0, 0, 0 };
	int i, k;
	r[1] = (u8)remotes;
	for (i = 0; remotes > 1 && i < remotes; i++) {
		s32 head = heads[i];
		gone[i] = !so_np_player_active(so_np_remote_player_idx(i));
		if (x.game.rb && head > finalized + ROLLBACK_MAX_FRAMES - 1)
			head = finalized + ROLLBACK_MAX_FRAMES - 1;
		if (!gone[i] && head > newest) {
			newest = head;
			so_np_remote_checksum(i, &newest_cf, &newest_cs);
		}
	}
	for (i = 0; i < 3; i++) {
		nums[i] = frame;
		if (i < remotes && gone[i] && !next_mode()) {
			fill_gone(r, i, frame, finalized, heads[i], newest, newest_cf, newest_cs, &nums[i]);
			so_put_u32(r + 0x1A + i * 4, (u32)nums[i]);
			continue;
		}
		if (i < remotes) {
			s32 head = heads[i], cf;
			u32 cs;
			so_np_remote_checksum(i, &cf, &cs);
			if (gone[i]) {
				/* (3.6.5) One that has left: its checksums stopped, so the game is given the newest
				 * of the others'. Its inputs are in its queue as anyone's: the last one, repeated
				 * (so_np_fill_dropped), and the frame its character leaves on follows below. */
				cf = newest_cf;
				cs = newest_cs;
			}
			so_put_u32(r + 0x02 + i * 8, (u32)cf);
			so_put_u32(r + 0x06 + i * 8, cs);
			/* Dolphin sends the oldest 7 frames it holds, from the
			 * finalized frame on (GetSlippiRemotePad after
			 * DropOldRemoteInputs), newest first */
			if (x.game.rb && head > finalized + ROLLBACK_MAX_FRAMES - 1)
				head = finalized + ROLLBACK_MAX_FRAMES - 1;
			nums[i] = head < frame ? head : frame;
		}
		so_put_u32(r + 0x1A + i * 4, (u32)nums[i]);
	}
	smallest = nums[0];
	for (i = 1; i < 3; i++)
		if (nums[i] < smallest)
			smallest = nums[i];
	so_put_u32(r + 0x26, (u32)smallest);
	for (i = 0; i < remotes; i++) {
		u8 *pads = r + 0x2A + i * ROLLBACK_MAX_FRAMES * PAD_FULL_SIZE;
		if (gone[i] && !next_mode())
			continue;	/* (fill_gone) */
		if (next_mode())
			so_put_u32(r + RXB_DESPAWN + i * 4, (u32)so_np_despawn_frame(i));
		for (k = 0; k < ROLLBACK_MAX_FRAMES; k++)
			if (!x.game.rb || k == 0 || nums[i] - k >= finalized)	/* Dolphin's holds no older ones */
				so_np_remote_pad(i, nums[i] - k, pads + k * PAD_FULL_SIZE);	/* 9 bytes; the rest stays 0 */
	}
	if (next_mode()) {
		/* (3.6.5) What the game predicts with for each of the 7 frames after an opponent's newest
		 * input: that input, the first of its block (Slippi Dolphin puts an "input suggestion" there
		 * when it has one; SlippWii takes none). And the random bits the game sends with its next pad:
		 * not so_random's, whose generator an opponent could read back from them. */
		for (i = 0; i < 3; i++) {
			const u8 *latest = r + 0x2A + i * ROLLBACK_MAX_FRAMES * PAD_FULL_SIZE;
			u8 *predicted = r + RXB_PREDICTED + i * ROLLBACK_MAX_FRAMES * PAD_FULL_SIZE;
			for (k = 0; k < ROLLBACK_MAX_FRAMES; k++)
				memcpy(predicted + k * PAD_FULL_SIZE, latest, PAD_FULL_SIZE);
		}
		if (x.rng_left == 0) {
			so_tls_random(x.rng_bits, sizeof(x.rng_bits));
			x.rng_left = sizeof(x.rng_bits);
		}
		r[RXB_NEXT_RNG] = x.rng_bits[--x.rng_left];
	}
}

/* The game as it was set up, in a line: what a desync's report is read by first (two consoles with
 * another team, colour or stage here are in different games from the first frame). Each port:
 * character/colour/team/shade, - for none, * for this console's. (Not with dots: four numbers with
 * dots are an address to the card's file, and lose two: so_logfile_scrub.) */
static void say_setup(void)
{
	const u8 *b = x.match_block;
	char ports[64];
	int i, len = 0;
	for (i = 0; i < 4; i++) {
		const u8 *q = b + 0x60 + i * 0x24;
		if (q[1] != 0)
			len += so_snprintf(ports + len, sizeof(ports) - len, " -");
		else
			len += so_snprintf(ports + len, sizeof(ports) - len, " %u/%u/%u/%u%s", (unsigned)q[0], (unsigned)q[3],
				(unsigned)q[9], (unsigned)q[7], i == x.local_idx ? "*" : "");
	}
	so_log("exi: the game: mode %u, stage %u, teams %u, delay %u, %u opponents, rollback %u; ports:%s", (unsigned)x.mode,
		(unsigned)so_get_u16(b + 0x0E), (unsigned)b[0x08], (unsigned)x.game.mq.delay, (unsigned)so_np_remote_count(),
		(unsigned)x.game.rb, ports);
}

/* A desync's report. check_sums keeps, at the first frame whose checksum is not an opponent's, the
 * inputs this console had from each opponent for the frames up to it; so_exi_desync_service (the
 * network thread: not the game's poll) says them, with the game's setup, and starts the file. With
 * a replay of the same game from a PC in it, that is: the same inputs and another result (the game's
 * state: its setup, a player taken for gone), or other inputs (the netcode). On the online heap, as
 * the checksums: the kernel's data is full. */
#define DESYNC_PADS 12
static struct {
	/* 1: a checksum differs; 2: an opponent was given up, and the game goes on without it; 3: every
	 * opponent left in the middle of a game of four (what they gave this console up for is in the
	 * lines before) */
	u8 pending;
	u8 remotes;
	u8 over;	/* the game ended before the network thread's turn: the file ends when it is started */
	u8 player[SO_REMOTE_MAX];
	s32 frame;
	u8 have[SO_REMOTE_MAX][DESYNC_PADS];
	u8 pads[SO_REMOTE_MAX][DESYNC_PADS][SO_PAD_SIZE];
} *desync;

/* (the game's poll, the client lock held) */
static void desync_keep(int kind, s32 frame, int remotes)
{
	int i, k;
	if (!desync && !(desync = so_malloc(sizeof(*desync))))
		return;
	memset(desync, 0, sizeof(*desync));
	desync->frame = frame;
	desync->remotes = (u8)(kind == 1 ? remotes : 0);
	for (i = 0; i < desync->remotes && i < SO_REMOTE_MAX; i++) {
		desync->player[i] = so_np_remote_player_idx(i);
		for (k = 0; k < DESYNC_PADS; k++)
			desync->have[i][k] = so_np_remote_pad(i, frame - (DESYNC_PADS - 1) + k, desync->pads[i][k]) == 0;
	}
	desync->pending = (u8)kind;
}

void so_exi_desync_service(void)
{
	static const char hex[] = "0123456789abcdef";
	int kind, i, k, b;
	if (!desync || !desync->pending)
		return;
	kind = desync->pending;
	desync->pending = 0;
	so_log("desync: %s (frame %d); what this console had:", kind == 1 ? "a frame's checksum is not an opponent's" :
		kind == 2 ? "an opponent's inputs stopped, and the game goes on without it" :
		"every opponent left in the middle of the game", (int)desync->frame);
	say_setup();
	for (i = 0; i < desync->remotes; i++) {
		for (k = 0; k < DESYNC_PADS; k += 4) {
			char line[4 * (2 * SO_PAD_SIZE + 1) + 1];
			int len = 0, j;
			for (j = k; j < k + 4; j++) {
				for (b = 0; b < so_pad_wire; b++) {
					u8 v = desync->pads[i][j][b];
					line[len++] = desync->have[i][j] ? hex[v >> 4] : '-';
					line[len++] = desync->have[i][j] ? hex[v & 15] : '-';
				}
				line[len++] = ' ';
			}
			line[len] = 0;
			so_log("desync: player %u's inputs, frames %d to %d: %s", (unsigned)desync->player[i] + 1,
				(int)(desync->frame - (DESYNC_PADS - 1) + k), (int)(desync->frame - (DESYNC_PADS - 1) + k + 3), line);
		}
	}
	so_logfile_desync();
	if (desync->over) {
		desync->over = 0;
		so_logfile_report_end();
	}
}

/* A game that was set up and never started. Once (run 20261114-220434, a Teams lobby, 1 of about 130
 * that day and night) all four had picked, the Wii went to the versus screen and stayed on it, having
 * loaded that screen's files and none of the game's, and its three PCs stood at READY for good. From
 * the game's setup to its first poll is 3 to 5 s here; a PC gives a player up after 7. So 20 s without
 * a poll, on the versus screen or past it: said with the game's setup and the kernel's side of a disc
 * read, and written as a desync's report is: nothing else would be left of it. */
#define START_LIMIT_MS 20000
void so_exi_start_watch(void)
{
	u32 limit = so_client_config()->test_start_limit_ms, waited, scene;
	if (x.setup_on != 1)
		return;
	waited = so_time_ms() - x.setup_ms;
	if (waited < (limit ? limit : START_LIMIT_MS) || waited > 0x7FFFFFFFu)	/* (or set this very moment) */
		return;
	/* (only on the versus screen or in the game's own scene: all have picked while one may still be
	 * choosing the stage, for as long as it likes; looked at again in a second) */
	scene = so_ppc_word(0x00479D30) & 0xFF0000FF;
	if (scene != 0x08000004 && scene != 0x08000002) {
		x.setup_ms += 1000;
		return;
	}
	x.setup_on = 2;
	so_log("desync: the game was set up %u s ago and has not started; what this console had:", (unsigned)(waited / 1000));
	say_setup();
	so_diag_where();
	so_logfile_desync();
	so_logfile_report_end();	/* (this, and no more: a game that starts after all is another matter) */
}

/* Desync detection: this console's checksum of each finalized frame
 * (Slippi's game state hash, sent with the pads), the last 32, on the online
 * heap (the kernel's data is full), checked against the opponents' latest
 * for the same frame. Cleared with the game's numbers (reset_game). */
#define SUMS 32
static struct {
	s32 frame;
	u32 sum;
} *sums;

/* More than one opponent: the game waits for every input (rollback_wanted), so the inputs have to be
 * here before their frame, and the delay is what gives them the time: an input for frame N leaves its
 * player delay frames before N and is a one-way trip on the way. Slippi's 2 frames cover 33 ms one way
 * less what the Wii takes to have it in hand; past that the game waits a refresh at a time and stays
 * behind (at 60 ms of ping: 23 frames waited a game at 2, none at 4, 2026-11-14). So: the player's own
 * setting, or more by the lobby's slowest ping: the one-way time and multi_delay_room_ms, in frames,
 * up. The room is for the line's jitter, IOS's receive and the frame the level-keeping below may run
 * early: 20 ms. At 63 ms of ping with a room of 10 (3 frames) the game waited 1 to 6 frames a game
 * and ran early 5 times in three games, with 16 none and twice; at 95 ms 4 frames waited 6 in a game,
 * 5 frames 0 to 2. So: under 27 ms of ping 2 frames, under 60 ms 3, under 93 ms 4, under 127 ms 5,
 * else 6. Only the Wii's player feels it: its opponents see its inputs no later (Slippi's time sync
 * goes by the frames' numbers, so they run that much ahead), and play at their own delay. One
 * opponent: the setting, as ever. */
#define MULTI_DELAY_MOST 6
static u8 lobby_delay(int remotes)
{
	u8 delay = so_client_config()->delay;
	u32 want;
	if (remotes <= 1 || so_client_config()->test_multi_rollback)
		return delay;
	want = (so_np_slowest_ping_ms() / 2 + so_client_config()->multi_delay_room_ms) * 1000 / FRAME_US + 1;
	if (want > MULTI_DELAY_MOST)
		want = MULTI_DELAY_MOST;
	return want > delay ? (u8)want : delay;
}

/* More than one opponent, waiting for inputs: what keeps the Wii level with them. Dolphin runs faster
 * as soon as it reads itself behind, but slower only from some 8 ms ahead, and it takes half its ping
 * to the Wii for the one-way time, a ping that holds the Wii's ack, up to a frame after the pad
 * (HELD_ACK_US). Left alone it sits 25 to 30 ms ahead of the Wii for the whole game, and the Wii's
 * inputs are that late for everyone: at 60 ms of ping each PC ran 2.0 frames ahead of the Wii's inputs
 * on average and 0.1 to 0.5 ahead of another PC's (lobby.py --net 30,3, 2026-11-14). With one opponent
 * the rollback engine keeps the Wii level (fine_sync); here the game runs a frame early (two in a
 * refresh) when it reads itself behind by MULTI_BEHIND_US on average over MULTI_SAMPLES readings, and
 * the next frame's inputs are all here: it is then about a third of a frame ahead, where Dolphin
 * follows it. */
#define MULTI_BEHIND_US 11000
#define MULTI_SAMPLES 4
static int multi_level(s32 frame, const s32 *heads, int remotes)
{
	int i;
	if (frame <= 120 || frame % TIME_SYNC_INTERVAL != 0)
		return 0;
	x.game.fine_sum += so_np_time_offset_us();
	if (++x.game.fine_n < MULTI_SAMPLES)
		return 0;
	i = x.game.fine_sum / MULTI_SAMPLES < -MULTI_BEHIND_US;
	x.game.fine_sum = x.game.fine_n = 0;
	if (!i)
		return 0;
	for (i = 0; i < remotes; i++)
		if (heads[i] < frame + 1)
			return 0;	/* (it would only wait for them) */
	x.game.fine_advances++;
	return 1;
}

/* One opponent: the game rolls back. More (Teams, Party): it waits for inputs, as it did for
 * everyone before 1.4.0, and its opponents roll back as against anyone. Four characters do not
 * fit: their frame is 11 ms of the 16.7 where two characters' is 6 to 8, a rollback of one frame
 * makes it 20 (a load of 360 pages alone is 5 ms), and nearly every frame has one, some opponent's
 * input changing. Measured 2026-11-14 on a LAN with every input shown a frame late
 * (lobby.py --lag 1): the replays the same as Dolphin's, but 6 refreshes in 10 with no new frame
 * and the game at 86% of its speed, its opponents waiting for it. Waiting, the same games ran
 * with no refresh lost. test_multi_rollback: that measurement again. */
static int rollback_wanted(void)
{
	if (so_np_remote_count() > 1 && !so_client_config()->test_multi_rollback)
		return 0;
	return so_client_config()->rollback && x.have_client && so_ppc_scratch_free;
}

static void reset_game(void)
{
	memset(&x.game, 0, sizeof(x.game));
	if (sums)
		memset(sums, 0, sizeof(*sums) * SUMS);
}

/* This game's numbers, when it ends: its frames and waits, its pacing, the
 * checksums compared, and with rollback its predictions and time sync. */
/* keep: the game's frames go on (its end reported, GAME! still running): it goes on
 * rolling back to the end as the opponent does, and is reset at the next game. In
 * lockstep for those 100-odd frames, each side waited on the other, and the PC
 * halted at its rollback limit. */
static void log_game(int keep)
{
	u64 us;
	u32 expected;
	if (x.game.reported) {	/* (logged at its report already) */
		reset_game();
		return;
	}
	if (x.game.frames < 300) {	/* the results screen's polls, not a game */
		reset_game();
		return;
	}
	us = so_time_us() - x.game.start_us;
	expected = (u32)(us / SO_FRAME_US);
	so_client_lock();
	so_drive_log("game drive", 1);
	so_log("game over: %u frames in %u ms (%u at 59.94 fps); waited %u frames (%u frames waited, longest %u), ping %u us",
		(unsigned)x.game.frames, (unsigned)(us / 1000), (unsigned)expected, (unsigned)x.game.skips,
		(unsigned)x.game.held, (unsigned)x.game.longest, (unsigned)so_np_take_avg_ping_us());
	so_log("game pacing: %u polls over 20 ms apart (longest %u us); answers: longest %u us, %u over 1 ms, lock wait up to %u us",
		(unsigned)x.game.slow_polls, (unsigned)x.game.max_gap_us, (unsigned)x.game.max_answer_us,
		(unsigned)x.game.slow_answers, (unsigned)x.game.max_lock_us);
	if (x.game.slow_n) {
		char text[200];
		u32 k, n = 0;
		for (k = 0; k < x.game.slow_n && n < sizeof(text) - 12; k++)
			n += so_snprintf(text + n, sizeof(text) - n, " %u:%u", (unsigned)(x.game.slow_at[k] >> 8),
				(unsigned)(x.game.slow_at[k] & 0xFF));
		so_log("game slow polls (frame:ms):%s", text);
	}
	so_client_log_holds();
	{
		u32 send_avg, send_max;
		so_client_take_pad_send_us(&send_avg, &send_max);
		so_log("game traffic: pads waited up to %u us for the network thread, sending one took %u us (at most %u); "
			"%u replay transfers", (unsigned)so_client_take_pad_latency_us(), (unsigned)send_avg,
			(unsigned)send_max, (unsigned)x.game.replay_writes);
	}
	{
		SoSocketStats st;
		so_socket_stats(&st);
		so_log("game sockets: in %u datagrams, waited %u us (at most %u) to be taken; out %u, waited %u us (at most %u) "
			"for the sender, sendto took %u us (at most %u)", (unsigned)st.rx_waits,
			(unsigned)(st.rx_waits ? st.rx_wait_sum / st.rx_waits : 0), (unsigned)st.rx_wait_max, (unsigned)st.sends,
			(unsigned)(st.sends ? st.tx_wait_sum / st.sends : 0), (unsigned)st.tx_wait_max,
			(unsigned)(st.sends ? st.sends_us / st.sends : 0), (unsigned)st.send_us_max);
		{
			int k;
			for (k = 0; k < 2; k++) {
				u32 *b = st.by_size[k], n = b[0] ? b[0] : 1;
				so_log("game sends %s: %u, %u bytes, waited %u us, sendto %u us", k ? "big" : "small",
					(unsigned)b[0], (unsigned)(b[1] / n), (unsigned)(b[2] / n), (unsigned)(b[3] / n));
			}
		}
		so_log("game sendto, under 0.5/1/2/3/5 ms/more: %u %u %u %u %u %u", (unsigned)st.send_hist[0],
			(unsigned)st.send_hist[1], (unsigned)st.send_hist[2], (unsigned)st.send_hist[3], (unsigned)st.send_hist[4],
			(unsigned)st.send_hist[5]);
	}
	if (x.game.rb)
		so_log("game input timing: 0xB0 polls a 60th apart, within 0.5/1/2/4/8 ms/more: %u %u %u %u %u %u; %u pads sent ahead",
			(unsigned)x.game.spacing[0], (unsigned)x.game.spacing[1], (unsigned)x.game.spacing[2],
			(unsigned)x.game.spacing[3], (unsigned)x.game.spacing[4], (unsigned)x.game.spacing[5],
			(unsigned)x.game.early_pads);
	so_log("game checksums: %u frames compared with the opponent's, %u differ%s (first at frame %d: %08x/%08x); "
		"as Slippi sees them: %u hard (positions), %u off by 1, %u action states only",
		(unsigned)x.game.checks, (unsigned)x.game.desyncs, x.game.hard ? " - DESYNC" : x.game.desyncs ? " - soft" : "",
		(int)x.game.first_desync, (unsigned)x.game.first_ours, (unsigned)x.game.first_theirs,
		(unsigned)x.game.hard, (unsigned)x.game.near, (unsigned)x.game.soft);
	if (x.game.desyncs)
		so_exi_report_last();	/* (now, not in the game: see check_sums) */
	if (x.game.rb)
		so_log("game rollback: %u frames predicted, %u stalls at the 7-frame limit; time sync skipped %u, "
			"advanced %u (%u to stay level, %u after loading); %u refreshes missed made up; offset from the opponent %d us on average",
			(unsigned)x.game.predicted, (unsigned)x.game.limit_stalls, (unsigned)x.game.sync_skips,
			(unsigned)x.game.advances, (unsigned)x.game.fine_advances, (unsigned)x.game.frozen_advances,
			(unsigned)x.game.caught_up, x.game.offset_n ? (int)(x.game.offset_sum / x.game.offset_n) : 0);
	if (x.game.rb)
		so_log("game staying level: %u times behind, %u of them far enough for frames ending early",
			(unsigned)x.game.nudges, (unsigned)x.game.nudges_far);
	{
		char line[150];
		int len = so_snprintf(line, sizeof(line), "game rollback depth needed (frames 0..8+):"), i;
		for (i = 0; i < 9; i++)
			len += so_snprintf(line + len, sizeof(line) - len, " %u", (unsigned)x.game.depth[i]);
		so_log("%s", line);
	}
	if (x.game.mq.intervals) {	/* (two lines: a log line holds 150 characters) */
		const SoMq *q = &x.game.mq;
		u32 n = q->game_depth_samples ? q->game_depth_samples : 1, d = (u32)((u64)q->game_depth_sum * 100 / n);
		so_log("game quality (Slippi's rule): %u intervals, delay %u, debt at most %d of %d, %d at the end, depth %u.%02u; Ranked: %s (frame %d)",
			(unsigned)q->intervals, (unsigned)q->delay, (int)q->max_debt, SO_MQ_TERMINATE_DEBT, (int)q->debt,
			(unsigned)(d / 100), (unsigned)(d % 100), q->verdict ? "ended" : "not ended", (int)q->verdict_frame);
		so_log("game quality, unhealthy intervals: time %u/%u/%u, depth %u/%u/%u (mild/bad/severe), stalls %u short, %u with an event (%u events)",
			(unsigned)q->tiers[SO_MQ_TIME][1], (unsigned)q->tiers[SO_MQ_TIME][2], (unsigned)q->tiers[SO_MQ_TIME][3],
			(unsigned)q->tiers[SO_MQ_DEPTH][1], (unsigned)q->tiers[SO_MQ_DEPTH][2], (unsigned)q->tiers[SO_MQ_DEPTH][3],
			(unsigned)q->tiers[SO_MQ_STALLS][1], (unsigned)q->tiers[SO_MQ_STALLS][2], (unsigned)q->stall_events);
	}
	if (x.game.waits_logged) {
		char line[150];
		int len = so_snprintf(line, sizeof(line), "game waits (frame:ms):"), i;
		for (i = 0; i < (int)x.game.waits_logged; i++)
			len += so_snprintf(line + len, sizeof(line) - len, " %u:%u", (unsigned)x.game.wait_frame[i],
				(unsigned)x.game.wait_ms[i]);
		so_log("%s", line);
	}
	so_client_unlock();
	/* (a desync's file: this game's lines, and no write in the next) */
	if (desync && desync->pending)
		desync->over = 1;
	else
		so_logfile_report_end();
	if (keep)
		x.game.reported = 1;
	else
		reset_game();
}

static void check_sums(s32 finalized, u32 checksum, int remotes)
{
	int i, slot;
	if (!sums && !(sums = so_malloc(sizeof(*sums) * SUMS)))
		return;
	if (finalized > 0 && checksum) {
		slot = finalized % SUMS;
		sums[slot].frame = finalized;
		sums[slot].sum = checksum;
	}
	for (i = 0; i < remotes; i++) {
		s32 cf;
		u32 cs;
		so_np_remote_checksum(i, &cf, &cs);
		if (cf <= x.game.last_checked || !cs || !so_np_player_active(so_np_remote_player_idx(i)))
			continue;	/* (a disconnected opponent's last checksum is not the game's) */
		slot = cf % SUMS;
		if (sums[slot].frame != cf)
			continue;
		if (so_client_config()->test_desync_frame && cf >= (s32)so_client_config()->test_desync_frame && !x.game.desyncs)
			cs ^= 0x10;	/* tests: the desync file, of a game in step */
		x.game.last_checked = cf;
		x.game.checks++;
		if (sums[slot].sum != cs) {
			/* Slippi's ASM (StartEngineLoop.asm): the low 16 bits are the fighters' positions
			 * summed, off by more than 1 a hard desync that ends the game; the high 16 bits
			 * action states, a warning only. Logged once, in one line: a log line is slow
			 * on a test build, and this is the game's poll (the report itself waits for the
			 * game's end: its 27 lines froze a beta game for 321 ms, 2026-11-08). */
			u32 ours = sums[slot].sum;
			s32 d = (s32)(s16)(u16)(ours & 0xFFFF) - (s32)(s16)(u16)(cs & 0xFFFF);
			if (d > 1 || d < -1)
				x.game.hard++;
			else if (d)
				x.game.near++;
			else
				x.game.soft++;
			if (!x.game.desyncs++) {
				x.game.first_desync = cf;
				x.game.first_ours = ours;
				x.game.first_theirs = cs;
				so_log("exi: checksum differs at frame %d: ours %08x, player %u's %08x", (int)cf,
					(unsigned)ours, (unsigned)so_np_remote_player_idx(i) + 1, (unsigned)cs);
				desync_keep(1, cf, remotes);
			}
		}
	}
}

/* Rollback's time sync, as Dolphin's (shouldSkipOnlineFrame,
 * shouldAdvanceOnlineFrame; the speed adjustment aside): in the first 2 s,
 * wait out up to 5 frames when ahead by over 10 ms; after that, when behind
 * by over a frame and 10 ms, run up to 3 frames twice (one every 5 frames).
 * The client lock is held. */
#define ENGINE_VI_NUDGE (0x1159F500 + 24)	/* (T_VI_NUDGE) */
#define ENGINE_BG_SAVES (0x1159F500 + 56)	/* (T_BG_SAVES) */
#define ENGINE_DIRECT (0x1159F500 + 28)	/* (T_DIRECT) */
#define SKIP_LEAVE_US 3000
static int time_sync_skip(s32 frame)
{
	if (frame % TIME_SYNC_INTERVAL == 0 && !x.game.skipping && frame <= 120) {
		s32 offset = so_np_time_offset_us();
		/* (not as Dolphin's, over 10 ms: a frame waited out from 10-16 ms ahead left the Wii up
		 * to 6 ms behind, and the pad read moving earlier from frame 40 on reads as up to 6 ms
		 * ahead; behind by 3 ms takes frames ending early, which a capture card shows (seen at
		 * frame 120). So only whole frames that leave it ahead: Dolphin, behind, catches up.) */
		if (offset > FRAME_US + SKIP_LEAVE_US) {
			x.game.skipping = 1;
			x.game.to_skip = (offset - SKIP_LEAVE_US) / FRAME_US;
			if (x.game.to_skip > 5)
				x.game.to_skip = 5;
		}
	}
	if (x.game.to_skip > 0) {
		x.game.to_skip--;
		return 1;
	}
	x.game.skipping = 0;
	return 0;
}

/* Staying level. Slippi Dolphin nudges its emulation speed up (to 1%) when
 * behind by over 0.25 ms, so two Dolphins pull each other level; it does not
 * slow down when ahead by up to 8 ms (its own choice, for the faster PC). So
 * left alone a Wii with the slower clock settles behind by up to 8 ms, and its
 * inputs reach the opponent that much later every frame (more rollbacks for them
 * than against a PC; measured: 5.5 ms, 2.7 times the rollbacks at 30 ms).
 *
 * The Wii nudges too: while it is behind its frames end a line early
 * (vi_nudge in rollback.c), bringing it back at about 2 ms a second. Advancing a
 * frame stays for a real loss of time, over FINE_SYNC_FAR_US on average over 2 s:
 * a frame the player does not see, and then 2 s of the opponent catching up. */
/* (behind by 1 ms on, 0.3 off: Dolphin reads itself at about the opposite, 0.3 ahead, its noise
 * puts it behind often, and it runs faster whenever it is, proportionally (100.012% on average,
 * some 70 ppm): the Wii followed it all game, and once the pad read was at its earliest, frames
 * ended early. 2.5 and 1.5: Dolphin less often behind, 30 ms one way, 12 games: capture odd
 * steps 10 -> 1, the Wii's inputs at the PC no later (2.6 -> 1.8 ms); 45 ms: 2 -> 0.) */
#define VI_NUDGE_ON_US 2500
#define VI_NUDGE_OFF_US 1500
/* First (rollback_nudge_poll) by the pad read earlier only, which the picture doesn't show; frames
 * end early only when that is not enough, behind by this much */
#define VI_NUDGE_FAR_US 4500
#define FINE_SYNC_FAR_US 8000
#define FINE_SYNC_SAMPLES 4
/* Each advance leaves Dolphin behind by over 10 ms for about 2 s; 25 s of that in a
 * game and it warns its player of poor match performance. */
#define FINE_SYNC_MOST 12

/* Background saves (rollback_bg_saves): on, off, or by the ping, looked at with each
 * fine sync sample (the engine reads the word at each capture) */
static u32 bg_saves_wanted(void)
{
	const SoConfig *c = so_client_config();
	if (c->rollback_bg_saves < 2)
		return c->rollback_bg_saves;
	return so_np_ping_us(0) >= (u32)c->rollback_bg_ping_ms * 1000;
}

/* Frames on the screen a refresh sooner (rollback_direct; rollback.c, direct_track). The engine decides
 * by the frames' work, which it measures; this says how it starts a game (its word, T_DIRECT): 0 never;
 * 1 with the skip in the countdown, unless the last game's frames did not fit; 3 the same, and only against an opponent whose
 * last game the engine has counted; 2 level whatever comes (tests). rollback_direct 1, the ping's
 * part: 1 under rollback_direct_ping_ms less 33 ms for each delay frame under 2 (a frame less of delay
 * is a frame more of each rollback, as 33 ms more ping), once the ping is measured (a new connection
 * reads 0), and once there until 5 ms over it; 0 over it (so_pads.h, so_direct_mode); 3 with the ping
 * not measured yet, and in a band of rollback_direct_try_ms over the limit, 0 as set: the first game
 * against someone is played a refresh behind and counted, and the next starts level if it fits.
 * That band was 15 ms for a morning (2026-11-10): at 100 ms, before it, 1 game in 3 that started
 * level lost the run in play, a frame shown twice; at 83 ms, in 36 games, 13 were level throughout,
 * 10 lost the run (9 of them in the first second of play: the first rollbacks of two frames come
 * before the pad's read is as early as they need, and the one more skip at DIRECT_SETTLED is then
 * past its last tick, the ticks counting a rollback's loops), and most of the rest were a refresh
 * behind by the opponent's number: no frames to speak of shown twice (1 in 23 minutes), but a player's lag
 * 58 ms in one game and 75 in the next. Over the limit a game is what it is with the setting off,
 * until level play holds there. Never a skip in play: it would change the game's timing under the
 * player's hands. rollback_direct 2: 1 at any ping; 3: level whatever comes. Looked at with each
 * fine sync sample; the engine reads it at each frame. */
static u32 direct_wanted(void)
{
	const SoConfig *c = so_client_config();
	static u8 was;
	u32 ping;
	/* (more than one opponent, which only a test rolls back against: never. It goes by one
	 * opponent's ping and inputs.) */
	if (so_np_remote_count() > 1)
		return 0;
	if (c->rollback_direct != 1)
		return c->rollback_direct > 1 ? c->rollback_direct - 1 : 0;
	ping = so_np_ping_us(0);
	if (!ping)
		return 3;
	was = (u8)so_direct_mode(ping, (s32)c->rollback_direct_ping_ms + ((s32)x.poll_delay - 2) * 33,
		(s32)c->rollback_direct_try_ms, was);
	return was;
}

/* (the engine's word: how; in bit 9 the first game against this opponent, whose frames the engine has
 * not counted yet) */
static u32 direct_word(void)
{
	return direct_wanted() | (so_np_first_game() ? 0x200 : 0);
}

static void fine_sync(s32 offset, int countdown)
{
	so_set_ppc_word(ENGINE_BG_SAVES, bg_saves_wanted());
	so_set_ppc_word(ENGINE_DIRECT, direct_word());
	const SoConfig *cfg = so_client_config();
	s32 on = cfg->rollback_nudge_on_us ? cfg->rollback_nudge_on_us : VI_NUDGE_ON_US;
	s32 off = cfg->rollback_nudge_off_us ? cfg->rollback_nudge_off_us : VI_NUDGE_OFF_US;
	s32 far = on + VI_NUDGE_FAR_US - VI_NUDGE_ON_US;
	if (offset < -on) {
		u32 mode = so_client_config()->rollback_nudge_poll, was = x.game.nudging;
		if (!x.game.nudging) {
			x.game.nudging = 1;
			x.game.nudges++;
		}
		if (offset < -far && x.game.nudging < 2 && !countdown) {
			x.game.nudging = 2;
			x.game.nudges_far++;
		}
		if (x.game.nudging != was)
			so_set_ppc_word(ENGINE_VI_NUDGE, (!mode ? (countdown ? 0 : 1) : mode == 2 || x.game.nudging < 2 ? 7 : 3) |
				(u32)so_client_config()->rollback_sync_most_us << 16);
	} else if (offset > -off) {
		x.game.nudging = 0;
		so_set_ppc_word(ENGINE_VI_NUDGE, 0);
	}
	if (countdown)
		return;
	x.game.offset_sum += offset;
	x.game.offset_n++;
	x.game.fine_sum += offset;
	if (++x.game.fine_n < FINE_SYNC_SAMPLES)
		return;
	if (x.game.fine_sum / FINE_SYNC_SAMPLES < -FINE_SYNC_FAR_US && !x.game.advancing &&
	    x.game.fine_advances < FINE_SYNC_MOST) {
		x.game.advancing = 1;
		x.game.to_advance = 1;
		x.game.fine_advances++;
	}
	x.game.fine_sum = x.game.fine_n = 0;
}

static int time_sync_advance(s32 frame)
{
	if (frame % TIME_SYNC_INTERVAL == 0) {
		s32 offset = so_np_time_offset_us();
		if (offset < -(FRAME_US + 10000) && !x.game.advancing) {
			int most = frame > 120 ? 3 : 0;
			x.game.advancing = 1;
			x.game.to_advance = (-offset - 10000) / FRAME_US + 1;
			if (x.game.to_advance > most)
				x.game.to_advance = most;
		}
		/* (from the engine's early pad read, frame 40, by the read alone until 120: then it's
		 * 2 ms before the retrace, and up to 6 ms of a start's offset go with no frame ending
		 * early; from the first rollbacks on it's up to 8 ms early already, and a start 3 ms
		 * behind took 8-11 frames ending early at frame ~200 in 4 games of 12) */
		if (frame >= 60)
			fine_sync(offset, frame <= 120);
	}
	if (x.game.to_advance > 0) {
		if (frame % 5 != 0)
			return 0;
		x.game.to_advance--;
		return 1;
	}
	x.game.advancing = 0;
	return 0;
}

/* A frame the game took more than a refresh for leaves it a frame behind for
 * good: its loop runs a frame a refresh. Slippi Dolphin's frame limiter makes
 * up such a frame at once, as it paces by the clock; here the game is told to
 * advance (two frames in one refresh), one every 3 frames at most, so that the
 * opponent does not end up waiting at its rollback limit. Dolphin's network time
 * sync (time_sync_advance) still corrects the drift that is left.
 *
 * Behind by the clock, not by one long gap between polls: Slippi puts a retrace's
 * poll off while a rollback runs, and a late poll is followed by a short gap,
 * with no refresh lost. So every poll owes a refresh's time, and an advance's
 * extra poll, in the refresh it was told to share, owes none. Rollback games
 * after the first 2 s; at most 4 frames owed (a longer stop, the opponent has
 * waited out at its limit anyway). */
#define BEHIND_ENOUGH_US (FRAME_US * 3 / 4)
#define BEHIND_MOST_US (FRAME_US * 4)
#define BEHIND_LEAST_US (-FRAME_US / 2)
#define CATCH_UP_EVERY 3

static void catch_up_note(s32 frame, u32 gap)
{
	int extra = x.game.extra_poll;
	x.game.extra_poll = 0;
	if (!x.game.rb || frame <= 120)
		return;
	x.game.behind_us += (s32)gap - (extra ? 0 : FRAME_US);
	if (x.game.behind_us > BEHIND_MOST_US)
		x.game.behind_us = BEHIND_MOST_US;
	if (x.game.behind_us < BEHIND_LEAST_US)
		x.game.behind_us = BEHIND_LEAST_US;
}

static int catch_up(s32 frame)
{
	if (x.game.behind_us < BEHIND_ENOUGH_US || frame - x.game.caught_up_frame < CATCH_UP_EVERY)
		return 0;
	x.game.behind_us -= FRAME_US;
	x.game.caught_up++;
	x.game.caught_up_frame = frame;
	return 1;
}

/* The opponent's pads have run ahead of a game still loading (so_netplay.c,
 * send_frozen_pads). It runs the frames it missed two to a refresh, until it reaches
 * the last sent for it, then until within half a frame by the time sync's reading,
 * which now goes by its own pads: past that an advance only overshoots.
 *
 * Not if the game is behind its polls, as in its first frames, which can take many
 * refreshes: HSD's pad queue holds 5, and merges the oldest away when one more comes.
 * The frame numbers then run one ahead of the game's for good: every input a frame
 * early, a desync. (Seen at two a refresh after a stall of 3.) The engine says how
 * many frames the game has started (rb_frame). */
#define ENGINE_INPUT_WAIT (ENGINE_GAME_FRAMES + 4)	/* (T_INPUT_WAIT) */
#define ENGINE_REMOTE_CHANGED (ENGINE_GAME_FRAMES + 16)	/* (T_REMOTE_CHANGED) */
#define ENGINE_POLL_EARLY (ENGINE_GAME_FRAMES + 20)	/* (T_POLL_EARLY) */
#define ENGINE_REMOTE_HEAD (ENGINE_GAME_FRAMES + 8)	/* (T_REMOTE_HEAD) */
#define ENGINE_GAME_FRAMES 0x1159F500	/* (T_GAME_FRAMES) */
#define CATCHING_LAST 240	/* (a game this late is left to the time sync) */
static int catch_up_after_load(s32 frame, u8 delay, s32 frozen_head)
{
	if (x.game.frozen_advanced) {
		x.game.frozen_advanced = 0;
		return 0;
	}
	if (frame + delay >= frozen_head && (so_np_newest_offset_us() > -FRAME_US / 2 || frame > CATCHING_LAST)) {
		x.game.catching = 0;
		return 0;
	}
	if (frame - 1 - (s32)so_ppc_word(ENGINE_GAME_FRAMES) > 1)
		return 0;
	x.game.frozen_advanced = 1;
	x.game.frozen_advances++;
	return 1;
}

/* What the game may see of an opponent at `frame`: with test_lag_frames
 * (tests), only the inputs that many frames old; during a test lag spike, none
 * newer; never less than it has seen (it does not predict a frame again once it
 * has its input: it has no savestate for it). */
static s32 visible_head(int i, s32 frame, u64 now)
{
	s32 lag = x.game.rb ? so_client_config()->test_lag_frames : 0;
	s32 head = so_np_remote_head_frame(i);
	if (lag && head > frame - lag)
		head = frame - lag;
	if (now < x.game.spike_until_us && head > x.game.spike_head[i])
		head = x.game.spike_head[i];
	return head < x.game.seen[i] ? x.game.seen[i] : head;
}

/* Dolphin's test (shouldSkipOnlineFrame), rolling back: the latest frame of the
 * 7 it would send from the finalized one on (fill_remote_inputs), at most 7
 * behind */
/* More than one opponent (tests only: rollback_wanted): the game waits from this many frames
 * ahead of an opponent's inputs. A capture of four characters' game is 310 to 390 pages against
 * two characters' 225, and the engine's copies (rollback.c, STORES) then hold 5 or 6 frames, not
 * 8: at Slippi's 7 it asked for a frame the engine had let go ("rb MISS", 2026-11-14). One
 * opponent: Slippi's 7, as ever. */
#define MULTI_ROLLBACK_FRAMES 5
static int within_rollback_limit(s32 frame, s32 finalized, s32 head)
{
	s32 latest = head < finalized + ROLLBACK_MAX_FRAMES - 1 ? head : finalized + ROLLBACK_MAX_FRAMES - 1;
	return latest >= frame - (so_np_remote_count() > 1 ? MULTI_ROLLBACK_FRAMES : ROLLBACK_MAX_FRAMES);
}

/* 0xE0: the frame's pad, to the opponent now, if its 0xB0 will not have to wait
 * (or else its pad would have been sent and the game not gone on). The 0xB0
 * then does not wait, nor send it again. */
static void on_early_pad(const u8 *p)
{
	s32 frame = (s32)so_get_u32(p + 1);
	u64 now = so_time_us();
	SoLocalPad pad;
	int remotes, i, ok;
	if (!client_up() || !x.game.rb || frame <= 120 || x.game.early_frame)
		return;
	so_client_lock();
	ok = so_np_status() == SO_NP_CONNECTED;
	remotes = so_np_remote_count();
	if (remotes > 3)
		remotes = 3;
	for (i = 0; ok && i < remotes; i++)
		ok = within_rollback_limit(frame, x.game.last_finalized, visible_head(i, frame, now));
	so_client_unlock();
	if (!ok)
		return;
	pad.frame = frame + p[13];
	pad.checksum_frame = x.game.last_finalized;
	pad.checksum = x.game.last_checksum;
	memcpy(pad.pad, p + 14, SO_PAD_SIZE);
	memcpy(x.game.early_pad, p + 14, SO_PAD_SIZE);
	x.game.early_frame = frame;
	x.game.early_us = now;
	x.game.early_pads++;
	x.stats.early_pads++;
	so_client_queue_pad(&pad);
}

/* The last polls, for a report of what went wrong (so_exi_report_last). */
#define LAST_POLLS 32
typedef struct {
	s32 frame, finalized, head;
	s16 more[2];	/* the second and third opponents' newest inputs, from the first's */
	u32 ms;
	u8 answer, remotes;
} LastPoll;
static LastPoll last_polls[LAST_POLLS];
static u32 last_poll_n;

/* The engine's (rollback.c): T_EVENTS */
#ifndef ENGINE_EVENTS
#define ENGINE_EVENTS 0x1159F100
#define ENGINE_EVENTS_N 96
#endif
#define ENGINE_SLOW_FRAME (ENGINE_EVENTS + 4 + ENGINE_EVENTS_N * 4)	/* (T_SLOW_FRAME) */

static int scratch_junked;
static u32 marks_found, marks_first[8];	/* (so_exi_scratch_boot) */

/* MEM2 outlives a reboot, and after a power cut holds anything: what the engine reads
 * from the kernel starts at 0, and the diagnostics this session reports are its own.
 * ISO.c, once it has given the scratch to the engine: before the game runs at all. */
/* tests (test_logfile_dump): the newest file in the log's folder on the card, as newest.txt names it, read
 * back: its size as far as 24 KB, its first lines, and of the rest those a desync's report is read by, to
 * the PC's log only. A file this boot is writing is left alone (it is open). */
static void logfile_dump(void)
{
	enum { MOST = 24576, FIRST = 8, LINES = 96 };
	char *buf = (char *)so_malloc(MOST + 1), path[64], *name, *end, *line;
	int n, lines = 0;
	if (!buf)
		return;
	n = so_read_file("/slippwii/logs/newest.txt", buf, 200);
	buf[n > 0 ? n : 0] = 0;
	name = n > 0 ? strstr(buf, " is ") : NULL;
	end = name ? strchr(name + 4, ' ') : NULL;
	if (!end) {
		so_log_private("card> no newest.txt in the log's folder (%d)", n);
		so_free(buf);
		return;
	}
	*end = 0;
	so_snprintf(path, sizeof(path), "/slippwii/logs/%s", name + 4);
	if (so_logfile_path() && !strcmp(so_logfile_path(), path)) {
		so_log_private("card> %s is this boot's own, and open", path);
		so_free(buf);
		return;
	}
	n = so_read_file(path, buf, MOST);
	so_log_private("card> %s: %d bytes read (of at most %d)", path, n, (int)MOST);
	buf[n > 0 ? n : 0] = 0;
	for (line = buf; n > 0 && *line && lines < LINES;) {
		char *nl = strchr(line, 10);	/* (a newline) */
		if (nl)
			*nl = 0;
		if (lines < FIRST || strstr(line, "desync") || strstr(line, "the game:") || strstr(line, "checksum") ||
		    strstr(line, "selections") || strstr(line, "has left") || strstr(line, "game over") ||
		    strstr(line, " np: ") || strstr(line, "exi: no input") ||
		    strstr(line, " di: ") || strstr(line, "fs lock") || strstr(line, "prefetch: state")) {
			so_log_private("card> %.140s", line);
			if (!(++lines & 7))
				so_client_flush_logs();
		}
		if (!nl)
			break;
		line = nl + 1;
	}
	if (so_client_config()->test_logfile_dump > 1 && n == MOST) {
		/* (2) what a long report says after its first part: the rest of the file, a part at a time, whole
		 * lines, without the stall profiler's (a test build writes thousands of those in a stall) */
		int err = 0;
		void *f = so_file_open(path, &err);
		u8 *bounce = (u8 *)so_malloc(4096);
		u32 at = MOST;
		lines = 0;
		while (f && bounce && lines < 400) {
			int last;
			n = so_file_read_at(f, at, (u8 *)buf, MOST, bounce, 4096);
			if (n <= 0)
				break;
			buf[n] = 0;
			for (last = n; n == MOST && last > 0 && buf[last - 1] != 10; last--)
				;	/* (a line cut by the part's end comes with the next) */
			if (!last)
				break;
			buf[last - (n == MOST)] = 0;
			at += (u32)last;
			for (line = buf; *line;) {
				char *nl = strchr(line, 10);
				if (nl)
					*nl = 0;
				if (!strstr(line, "STALL") && !strstr(line, "SHOWN")) {
					so_log_private("card+ %.150s", line);
					if (!(++lines & 7))
						so_client_flush_logs();
				}
				if (!nl)
					break;
				line = nl + 1;
			}
		}
		so_log_private("card+ (%d lines of the rest, read to byte %u; open %d)", lines, (unsigned)at, err);
		if (bounce)
			so_free(bounce);
	}
	so_free(buf);
}

void so_exi_scratch_boot(void)
{
	static const u32 zero_words[] = {
		ENGINE_SLOW_FRAME, ENGINE_INPUT_WAIT, ENGINE_POLL_EARLY, ENGINE_BG_SAVES, ENGINE_VI_NUDGE, ENGINE_DIRECT,
		ENGINE_REMOTE_HEAD, ENGINE_REMOTE_CHANGED, ENGINE_EVENTS,
		0x1159EA00u,	/* T_BG_RING's count */
	};
	u32 a;
	if (!so_ppc_scratch_free)
		return;
	/* The engine's page marks (rollback.c T_MARK, a bit a MEM1 page: on a load's list) as this boot
	 * found them, for the log (CMD_ROLLBACK's line: this early the log's lines are dropped). A load
	 * sets and clears its own, and the engine clears them all at a game's start; until 2026-11-10 it
	 * did not, and a bit set here was a page no load took from a later capture: not put back. */
	marks_found = 0;
	for (a = 0; a < 0x300; a += 4) {
		u32 w = so_ppc_word(0x11570000u + 0x14C00u + a);
		if (w && marks_found++ < 4) {
			marks_first[2 * (marks_found - 1)] = 0x80000u + a * 8;
			marks_first[2 * (marks_found - 1) + 1] = w;
		}
	}
	if (so_client_config()->test_mark_page) {
		/* tests: one page's mark set, the rest as found: the engine clears them at a game's start;
		 * one that did not (before 24f5ab6) never took this page from a later capture */
		u32 p = so_client_config()->test_mark_page, at = 0x11570000u + 0x14C00u + ((p >> 3) & ~3u);
		so_set_ppc_word(at, so_ppc_word(at) | 1u << ((p & 7) + 8 * (3 - ((p >> 3) & 3))));
	}
	if (so_client_config()->test_scratch_junk) {
		/* tests: the engine's tables and rings (below the prefetch's 0x60000) hold junk, so
		 * anything trusted before this boot set it shows (the freeze fixed in 8bdfbeb) */
		static u32 junk[64] __attribute__((aligned(32)));
		for (a = 0; a < 64; a++)
			junk[a] = 0xA5A5A5A5u;
		for (a = 0x11570000u; a < 0x11570000u + 0x60000u; a += sizeof(junk))
			so_ppc_set_block(a, junk, sizeof(junk));
		scratch_junked = 1;
	}
	for (a = 0; a < sizeof(zero_words) / sizeof(zero_words[0]); a++)
		so_set_ppc_word(zero_words[a], 0);
	for (a = 64; a <= 100; a += 4)	/* T_BG_DIAG and the DMA wait's HID2 */
		so_set_ppc_word(ENGINE_GAME_FRAMES + a, 0);
}

/* rollback.c: replay_queue, answer_ready */
/* (the answer of Slippi 3.6.5's 0xB0 is 559 bytes, where there was room for 448 before the words
 * at 0x1159F500: the ring's two words moved up, and the answer down into their place) */
#define ENGINE_REPLAY_HEAD 0x1159FE00
#define ENGINE_REPLAY_TAIL 0x1159FE20
#define ENGINE_ANSWER 0x1159F2A0
#define ENGINE_REPLAY 0x115A0000
#define ENGINE_REPLAY_SIZE 0x10000u
#define ENGINE_REPLAY_WRAP 0xFFFFFFFFu

/* The next of the engine's replay data, or NULL: the last is done with by the next call.
 * The kernel's main loop, between EXI commands. */
const u8 *so_exi_replay_next(u32 *len)
{
	u32 at;
	if (!x.replay_on)
		return NULL;
	if (x.replay_tail != x.replay_told)
		so_set_ppc_word(ENGINE_REPLAY_TAIL, x.replay_told = x.replay_tail);
	if (x.replay_tail == so_ppc_word(ENGINE_REPLAY_HEAD))
		return NULL;
	at = x.replay_tail % ENGINE_REPLAY_SIZE;
	*len = so_ppc_word(ENGINE_REPLAY + at);
	if (*len == ENGINE_REPLAY_WRAP) {
		x.replay_tail += ENGINE_REPLAY_SIZE - at;
		at = 0;
		*len = so_ppc_word(ENGINE_REPLAY);
	}
	x.replay_tail += 4 + ((*len + 3) & ~3u);
	x.game.replay_writes++;
	so_ppc_will_read(ENGINE_REPLAY + at + 4, *len);
	return (const u8 *)(__UINTPTR_TYPE__)(ENGINE_REPLAY + at + 4);
}

/* The 0xB0's answer, where the engine takes it from without a second trip here
 * (rollback.c, answer_ready): so it counts as read, and its pad can go. */
static void publish_answer(s32 frame, const u8 *r)
{
	so_ppc_set_block(ENGINE_ANSWER + 0x20, r, RXB_SIZE);
	so_set_ppc_word(ENGINE_ANSWER, (u32)frame);
	x.answer_read = 1;
}

void so_exi_report_last(void)
{
	char line[140];
	int len = 0;
	u32 i, n = so_ppc_word(ENGINE_EVENTS);
	/* few lines, each full: the log's ring is short, and what came just before this
	 * (the report that asked for it) is what matters most */
	so_log("rb bg: hid2 when a DMA wait went on %08x", (unsigned)so_ppc_word(ENGINE_GAME_FRAMES + 100));
	{
		u32 n = so_ppc_word(0x1159EA00), k;	/* (rollback.c T_BG_RING) */
		for (k = n > 16 ? n - 16 : 0; k < n; k++) {
			u32 at = 0x1159EA04 + (k & 15) * 8;
			so_log("rb bg ring %u: %08x at %08x", (unsigned)k, (unsigned)so_ppc_word(at), (unsigned)so_ppc_word(at + 4));
		}
	}
	so_log("rb bg: faults %u dar %08x srr0 %08x pte %08x same %u spurious %u ticks %u state %08x flushed %u",
		(unsigned)so_ppc_word(ENGINE_GAME_FRAMES + 64), (unsigned)so_ppc_word(ENGINE_GAME_FRAMES + 68),
		(unsigned)so_ppc_word(ENGINE_GAME_FRAMES + 72), (unsigned)so_ppc_word(ENGINE_GAME_FRAMES + 76),
		(unsigned)so_ppc_word(ENGINE_GAME_FRAMES + 80), (unsigned)so_ppc_word(ENGINE_GAME_FRAMES + 84),
		(unsigned)so_ppc_word(ENGINE_GAME_FRAMES + 88), (unsigned)so_ppc_word(ENGINE_GAME_FRAMES + 92),
		(unsigned)so_ppc_word(ENGINE_GAME_FRAMES + 96));
	so_log("last polls, frame/finalized/head/answer@ms:");
	for (i = last_poll_n > LAST_POLLS ? last_poll_n - LAST_POLLS : 0; i < last_poll_n; i++) {
		const LastPoll *l = &last_polls[i % LAST_POLLS];
		if (l->remotes > 1)	/* (the other opponents' heads after the first's, as differences from it) */
			len += so_snprintf(line + len, sizeof(line) - len, "%d/%d/%d,%d,%d/%x@%u ", (int)l->frame, (int)l->finalized,
				(int)l->head, (int)l->more[0], (int)l->more[1], (unsigned)l->answer, (unsigned)(l->ms % 100000));
		else
			len += so_snprintf(line + len, sizeof(line) - len, "%d/%d/%d/%x@%u ", (int)l->frame, (int)l->finalized,
				(int)l->head, (unsigned)l->answer, (unsigned)(l->ms % 100000));
		if (len > (int)sizeof(line) - 32) {
			so_log("%s", line);
			len = 0;
		}
	}
	if (len)
		so_log("%s", line);
	so_log("last engine, Capture/Load/Saved late/lost, frame@ms (its own clock):");
	len = 0;
	for (i = n > 48 ? n - 48 : 0; i < n; i++) {
		u32 v = so_ppc_word(ENGINE_EVENTS + 4 + (i % ENGINE_EVENTS_N) * 4);
		len += so_snprintf(line + len, sizeof(line) - len, "%c%d@%u ", "CLSX"[v >> 30], (int)(v << 12) >> 12,
			(unsigned)(v >> 20 & 0x3FF));
		if (len > (int)sizeof(line) - 16) {
			so_log("%s", line);
			len = 0;
		}
	}
	if (len)
		so_log("%s", line);
	so_client_flush_logs();
}

/* ---- tests: a crash's memory against the engine's copies ---------------------------------
 *
 * The crashes after a lag spike's rollback (2026-11-10) each found, in the first frame run again, an
 * object that was not what the frame loaded had: a free piece with a live object's bytes, an effect
 * whose GObj had no data. The crash report has flushed the caches (crash_report.s), so memory is what
 * the game saw. For each cache line a register of the faulting thread points into (and the line after
 * it), in a page the engine saves:
 *   post: r<n> line <address>: class, the page's C bit, where it is in the engine's last list of pages
 *         (a load's or a capture's: 65535, not in it), its newest copy's record and the base's copy
 *   post:   now    the line in memory
 *   post:   copy   in the page's newest copy: what the last load put back, if the page was in it
 *   post:   older / dropped <record>   in every other copy the tables still name: kept ones, and the
 *                  ones a load has dropped since the last capture (the frames rolled back)
 *   post:   base   in the base's copy
 * A line now that is a dropped copy's and not the newest's was not put back, or came back after;
 * one that is the newest copy's while the registers say otherwise was right in memory and wrong in
 * the cache; a newest copy that is itself wrong was made wrong. (rollback.c's tables: T_*.) */
#define EG 0x11570000u
#define EG_PTE_OF 0x10000u
#define EG_CLASS 0x13000u
#define EG_BASE 0x15000u
#define EG_HEAD 0x18000u
#define EG_WORK 0x1E000u
#define EG_REC_STORE 0x21000u
#define EG_REC_PAGE 0x23000u
#define EG_REC_PREV 0x25000u
#define EG_STORES 0x50000u
#define EG_NONE 0xFFFFu

static u32 eg16(u32 table, u32 i)
{
	u32 w = so_ppc_word(EG + table + ((i * 2) & ~3u));
	return (i & 1) ? (w & 0xFFFF) : (w >> 16);
}

static void post_line(const char *what, u32 n, u32 phys)
{
	so_log("post:   %s %x: %08x %08x %08x %08x %08x %08x %08x %08x", what, (unsigned)n, (unsigned)so_ppc_word(phys),
		(unsigned)so_ppc_word(phys + 4), (unsigned)so_ppc_word(phys + 8), (unsigned)so_ppc_word(phys + 12),
		(unsigned)so_ppc_word(phys + 16), (unsigned)so_ppc_word(phys + 20), (unsigned)so_ppc_word(phys + 24),
		(unsigned)so_ppc_word(phys + 28));
}

static void post_mortem(const u8 *report)
{
	static const u8 regs[] = { 3, 4, 5, 6, 24, 25, 26, 27, 28, 29, 30, 31 };
	u32 seen[5], n_seen = 0, i, j, k;
	if (!x.game.rb)
		return;
	for (i = 0; i < sizeof(regs) && n_seen < 5; i++) {
		u32 a = so_get_u32(report + 0x40 + regs[i] * 4) & ~31u, page, cls, head, base, off, lines, in_work = 0xFFFF, shown = 0;
		if (a < 0x80200000u || a >= 0x817FFFE0u)
			continue;
		for (j = 0; j < n_seen && seen[j] != a; j++)
			;
		page = (a - 0x80000000u) >> 12;
		cls = (so_ppc_word(EG + EG_CLASS + (page & ~3u)) >> (24 - 8 * (page & 3))) & 0xFF;
		if (j < n_seen || !cls)
			continue;
		seen[n_seen++] = a;
		head = eg16(EG_HEAD, page);
		base = eg16(EG_BASE, page);
		off = a & 0xFFF;
		lines = off + 32 < 4096 ? 2 : 1;
		for (k = 0; k < 1024; k++)
			if (eg16(EG_WORK, k) == page) {
				in_work = k;
				break;
			}
		so_log("post: r%u line %08x: class %u, C %u, in the last pages at %u, newest record %04x, base copy %04x",
			(unsigned)regs[i], (unsigned)a, (unsigned)cls,
			(unsigned)((so_ppc_word(EG + eg16(EG_PTE_OF, page) * 8 + 4) >> 7) & 1), (unsigned)in_work, (unsigned)head,
			(unsigned)base);
		for (k = 0; k < lines; k++)
			post_line("now   ", k, (a & 0x7FFFFFFFu) + k * 32);
		for (k = 0; k < lines; k++)
			post_line("copy  ", k, EG + EG_STORES + ((head != EG_NONE ? eg16(EG_REC_STORE, head) : base) << 12) + off + k * 32);
		for (k = 0; k < 4096 && shown < 7; k++) {
			u32 r, kept = 0, steps;
			if (k == head || eg16(EG_REC_PAGE, k) != page)
				continue;
			for (r = head, steps = 0; r != EG_NONE && r < 4096 && steps < 12; r = eg16(EG_REC_PREV, r), steps++)
				if (r == k)
					kept = 1;
			for (j = 0; j < lines; j++)
				post_line(kept ? "older " : "dropped", k << 4 | j, EG + EG_STORES + (eg16(EG_REC_STORE, k) << 12) + off + j * 32);
			shown++;
		}
		for (k = 0; k < lines; k++)
			post_line("base  ", k, EG + EG_STORES + (base << 12) + off + k * 32);
		so_client_flush_logs();
	}
}

/* When the game's next poll is due, the main loop does not sleep its usual time (main.c): a
 * poll waited about 1.1 ms for it, a sixth of a rollback's spare time every frame, and it holds
 * up the pad on its way to the opponent. From a little before the poll until it comes, or the
 * game has plainly missed it. A pass above IOS's network module holds up its sends
 * (online_loop_us), but a pad is sent just after the poll, when this has ended. */
#define POLL_PERIOD_US 16683
#define POLL_EARLY_US 700
#define POLL_LATE_US 2000
static u64 last_poll_us;

/* The engine's pad reads come at a VI line each refresh: it leaves Hollywood's timer
 * there at each (rollback.c, T_POLL_HW), so the loop is quick from just before the next
 * to a millisecond after (a poll was noticed 400 us late on average, the PPC waiting
 * with interrupts off; a quick loop all the time cost the network twice that). */
#define ENGINE_POLL_HW (0x1159F500 + 104)
#define HW_TICKS_PER_FIELD 31675	/* 16683 us at 526.7 ns */
#define HW_POLL_EARLY 380		/* 200 us */
#define HW_POLL_LATE 1900		/* 1 ms */

/* A file the game loads in the middle of a game (a Pokemon Stadium transformation):
 * it asks for the size twice, then the data, each a write and a read that the PPC
 * waits for, and the loop took about a millisecond to come round to each (its sleep
 * lets IOS's network module run): some 5 ms of a frame for a 1 ms copy. When a
 * rollback runs the load again, that frame was shown late (a hitch at most
 * transformations). So from the first size query to the data, the loop goes on
 * without sleeping: at most FILE_BUSY_US, and in a game only. */
#define FILE_BUSY_US 15000
static u64 file_busy_until_us;

/* Tests' log, in a game the rollback engine has: which reads, when, where to. (rollback.c, "Write
 * tracking": what the disc writes goes unseen by the engine; a crash after a lag spike's rollback of 7
 * frames, 2026-11-10, with model data where a free list's pointer had been, asks whether that is so
 * harmless.) The first 60 of a game. */
void so_exi_disc_read(u32 dest, u32 length, u32 offset)
{
	static u32 logged;
	if (!x.pf_engine) {
		logged = 0;
		return;
	}
	/* (not the music's: 32 bytes and 16 KB at a time to HSD DevCom's relay buffers, every 2 s, which
	 * no savestate has: excludes.py) */
	if (dest >= 0x004C28E0 && dest < 0x004CE380)
		return;
	if (logged++ < 60)
		so_log("exi: disc read in the game, frame %d: %u bytes to %08x (disc %08x)", (int)x.last_frame,
			(unsigned)length, (unsigned)(dest | 0x80000000u), (unsigned)offset);
}

int so_exi_file_busy(void)
{
	if (!file_busy_until_us)
		return 0;
	if (so_time_us() < file_busy_until_us)
		return 1;
	file_busy_until_us = 0;
	return 0;
}

u32 so_exi_poll_count(void)
{
	return x.stats.b0_polls;
}

u32 so_exi_poll_age_ms(void)
{
	u64 last = last_poll_us;
	return last ? (u32)((so_time_us() - last) / 1000) : 0xFFFFFFFFu;
}

int so_exi_in_match(void)
{
	return (so_ppc_word(0x00479D30) & 0xFF0000FFu) == 0x08000002u;	/* (the scene: major 8, minor 2; so_diag.c) */
}

int so_exi_poll_due(void)
{
	u64 now, due;
	u32 hw = so_ppc_word(ENGINE_POLL_HW);
#ifdef HW_TIMER
	if (hw && x.game.rb) {	/* (only this: the window by the last poll as well kept IOS's network
				 * module waiting some 3 ms a frame, and its sends with it) */
		u32 phase = (read32(HW_TIMER) - hw) % HW_TICKS_PER_FIELD;
		return phase >= HW_TICKS_PER_FIELD - HW_POLL_EARLY || phase <= HW_POLL_LATE;
	}
#else
	if (hw && x.game.rb)	/* (PC tests: no Hollywood timer) */
		return 1;
#endif
	if (!last_poll_us)
		return 0;
	now = so_time_us();
	due = last_poll_us + POLL_PERIOD_US;
	return now + POLL_EARLY_US >= due && now < due + POLL_LATE_US;
}

/* The polls after a file loaded in a game (a Stadium transformation): each one's
 * gap from the last and its frame, logged as one line. A repeated frame on screen
 * shows as a gap of 33 ms or more (or the frame not moving on). */
#define AFTER_LOAD_POLLS 16
#ifndef ENGINE_EVENTS
#define ENGINE_EVENTS 0x1159F100
#define ENGINE_EVENTS_N 96
#endif
static struct {
	int n, want;
	u32 ev_from;
	u16 gap_ms10[AFTER_LOAD_POLLS];
	s32 frame[AFTER_LOAD_POLLS];
} after_load;

/* Every EXI write and read for 150 ms from a mid-game file's first size query: what
 * the game did around the load (one line). */
#define CT_MAX 48
static struct {
	int n, on;
	u64 t0;
	u8 cmd[CT_MAX];
	u16 at_100us[CT_MAX];
} ct;

static void ct_note(u8 cmd)
{
	u64 now;
	if (!ct.on)
		return;
	now = so_time_us();
	if (ct.n < CT_MAX && now - ct.t0 < 150000) {
		ct.cmd[ct.n] = cmd;
		ct.at_100us[ct.n] = (u16)((now - ct.t0) / 100);
		ct.n++;
		return;
	}
	{
		char line[CT_MAX * 10 + 8];
		int i, o = 0;
		for (i = 0; i < ct.n && o < (int)sizeof(line) - 12; i++) {
			o += so_snprintf(line + o, sizeof(line) - o, " %02x@%u.%u", (unsigned)ct.cmd[i],
				(unsigned)(ct.at_100us[i] / 10), (unsigned)(ct.at_100us[i] % 10));
			if (o > 90) {
				so_log("exi: around the load (cmd@ms, reads 00):%s", line);
				o = 0;
			}
		}
		if (o)
			so_log("exi: around the load (cmd@ms, reads 00):%s", line);
	}
	ct.on = 0;
}

static void after_load_note(s32 frame, u32 gap)
{
	char line[AFTER_LOAD_POLLS * 14 + 8];
	int i, o = 0;
	if (!after_load.want)
		return;
	after_load.gap_ms10[after_load.n] = (u16)(gap / 100 > 65535 ? 65535 : gap / 100);
	after_load.frame[after_load.n] = frame;
	if (++after_load.n < after_load.want)
		return;
	for (i = 0; i < after_load.n && o < (int)sizeof(line) - 16; i++)
		o += so_snprintf(line + o, sizeof(line) - o, " %d:%u.%u", (int)after_load.frame[i],
			(unsigned)(after_load.gap_ms10[i] / 10), (unsigned)(after_load.gap_ms10[i] % 10));
	so_log("exi: polls after the load (frame:gap ms):%s", line);
	after_load.want = 0;
	{	/* the engine's captures (C) and loads (L) since: frame@ms (1.08 ms ticks) */
		u32 to = so_ppc_word(ENGINE_EVENTS), k, t0 = 0;
		o = 0;
		if (to - after_load.ev_from > 60)
			after_load.ev_from = to - 60;
		for (k = after_load.ev_from; k < to && o < (int)sizeof(line) - 16; k++) {
			u32 v = so_ppc_word(ENGINE_EVENTS + 4 + (k % ENGINE_EVENTS_N) * 4), t = (v >> 20) & 0x3FF;
			if (k == after_load.ev_from)
				t0 = t;
			if ((v >> 30) > 1)
				continue;
			o += so_snprintf(line + o, sizeof(line) - o, " %c%d@%u", (v >> 30) ? 'L' : 'C',
				(int)(v & 0xFFFFF), (unsigned)((t - t0) & 0x3FF));
			if (o > 90) {
				so_log("exi: engine after the load:%s", line);
				o = 0;
			}
		}
		if (o)
			so_log("exi: engine after the load:%s", line);
	}
}

/* Slippi's match quality rule (so_mq.h) said of an interval: the log gets the
 * unhealthy ones (a game's first 10) and the one that reaches the limit. */
static void mq_note(s32 frame, int judged)
{
	const SoMq *q = &x.game.mq;
	if (judged == 2)
		so_log("quality: frame %d: Slippi's rule would end a Ranked match here", (int)frame);
	else if (judged != 1 || q->change <= 0 || x.game.mq_logs >= 10)
		return;
	x.game.mq_logs++;
	so_log("quality: frame %d: debt +%d to %d (time %u%%: %d, depth %u.%02u: %d, stalls %u in %u events: %d)", (int)frame,
		(int)q->change, (int)q->debt, (unsigned)q->time_pct, (int)q->signal[SO_MQ_TIME], (unsigned)(q->depth_x100 / 100),
		(unsigned)(q->depth_x100 % 100), (int)q->signal[SO_MQ_DEPTH], (unsigned)q->last_stalled_passes,
		(unsigned)q->last_stall_events, (int)q->signal[SO_MQ_STALLS]);
}

static void on_online_inputs(const u8 *p)
{
	s32 frame = (s32)so_get_u32(p + 1), finalized = (s32)so_get_u32(p + 5);
	u32 checksum = so_get_u32(p + 9);
	u8 delay = p[13];
	if (delay != x.poll_delay) {
		if (x.poll_delay)
			so_log("exi: frame %d: the game's delay %u (was %u)", (int)so_get_u32(p + 1), (unsigned)delay,
				(unsigned)x.poll_delay);
		x.poll_delay = delay;
	}
	u8 *r = answer(RXB_SIZE);
	int remotes, i, ready = 1, skip, sent_early, sent_frozen;
	s32 heads[3] = { 0, 0, 0 }, frozen_head;

	u64 t_in = so_time_us(), t_poll = t_in, t_lock;
	last_poll_us = t_in;
	x.stats.b0_polls++;
	/* (a poll with its pad sent early comes later, for a rollback or a late input: its
	 * retrace was at the pad. By this one it looks late, then the next one early, and
	 * catch_up takes a game on time for one behind: advances it does not need, more
	 * polls than the game can take, and HSD's pad queue overflows: a desync.) */
	if (frame == x.game.early_frame)
		t_poll = x.game.early_us;
	if (x.game.last_poll_us && frame > 1) {
		u32 gap = (u32)(t_poll - x.game.last_poll_us);
		if (gap > 20000) {
			x.game.slow_polls++;
			if (frame > 130 && x.game.slow_n < 24)
				x.game.slow_at[x.game.slow_n++] = (u32)frame << 8 | (gap / 1000 > 255 ? 255 : gap / 1000);
		}
		if (gap > x.game.max_gap_us)
			x.game.max_gap_us = gap;
		/* A game that stops for 100 ms or more, in play: said at once, with what the network did, as
		 * the log may not live to the game's end (run 20261109-210054: the Wii's pads stopped for
		 * 150 ms, 150 ms and 1.7 s, 36 frames apart, and then the Wii was silent and off the network). */
		if (gap > 100000 && frame > 130 && x.game.gap_logs < 12) {
			SoSocketStats st;
			so_socket_stats_peek(&st);
			x.game.gap_logs++;
			so_log("poll gap: %u ms before frame %d; the opponent's newest input: frame %d; datagrams in %u (%u dropped, %u errors), "
				"out %u (%u dropped, %u errors, %u stuck), longest sendto %u us", (unsigned)(gap / 1000), (int)frame,
				(int)so_np_remote_head_frame(0), (unsigned)st.received, (unsigned)st.receive_dropped,
				(unsigned)st.receive_errors, (unsigned)st.sent, (unsigned)st.send_dropped, (unsigned)st.send_errors,
				(unsigned)st.sends_stuck, (unsigned)st.send_us_max);
			so_logfile_now();	/* (with the Diagnostic Log on: to the card before anything else happens) */
		}
		catch_up_note(frame, gap);
		after_load_note(frame, gap);
		if (x.game.rb && frame > 120) {
			u32 off = gap > 16667 ? gap - 16667 : 16667 - gap;
			x.game.spacing[off < 500 ? 0 : off < 1000 ? 1 : off < 2000 ? 2 : off < 4000 ? 3 : off < 8000 ? 4 : 5]++;
		}
	}
	x.game.last_poll_us = t_poll;
	x.setup_on = 0;	/* (the game has started: so_exi_start_watch) */
	if (!r)
		return;
	if (!client_up()) {
		r[0] = RESP_DISCONNECTED;
		return;
	}
	/* Game start, once per game: lockstep may poll frame 1 several times. */
	if (frame == 1 && x.last_frame != 1) {
		log_game(0);	/* a game that ended without a report (or after one) */
		so_client_lock();
		so_np_start_game();
		so_client_unlock();
		reset_selections(&x.local_sel);
		x.dummies_sent = 0;
		x.stall_since_us = 0;
		x.game.start_us = so_time_us();
		so_mq_start(&x.game.mq, delay);
	}
	x.last_frame = frame;
	if (x.drive_runs && frame > 2 && !so_drive_runs_line(x.drive_runs - 1, "exi: drive runs"))
		x.drive_runs = 0;
	else if (x.drive_runs && frame > 2)
		x.drive_runs++;
	/* Rollback from the first poll after the engine's "ready" (CMD_ROLLBACK),
	 * which comes about frames 1 to 3 (a slow stage load moves it): until
	 * then the inputs are waited for. The engine's savestates start at the
	 * game's first frame, so any frame will do. */
	if (frame >= 2 && !x.game.rb && x.rb_ready) {
		x.game.rb = 1;
		x.rb_ready = 0;
		/* the drive since the last game ended: the menus, then this game's loading */
		so_drive_log("exi: drive since the last game", 0);
		x.drive_runs = 1;
		so_log("exi: this game rolls back from frame %d (test lag %u frames, spikes %u ms)", (int)frame,
			(unsigned)so_client_config()->test_lag_frames, (unsigned)so_client_config()->test_spike_ms);
		say_setup();
		x.game.setup_said = 1;
		so_set_ppc_word(ENGINE_SLOW_FRAME, so_client_config()->test_slow_frame_ms);
		so_set_ppc_word(ENGINE_POLL_EARLY, so_client_config()->rollback_poll_early_us |
			(u32)so_client_config()->rollback_poll_deep_us << 16);
		so_set_ppc_word(ENGINE_BG_SAVES, bg_saves_wanted());
		so_set_ppc_word(ENGINE_DIRECT, direct_word());
		so_set_ppc_word(ENGINE_POLL_HW, 0);
		so_set_ppc_word(ENGINE_VI_NUDGE, 0);
		/* (one opponent: the word is its newest input; tests' lag hides inputs it would see) */
		so_client_lock();
		so_set_ppc_word(ENGINE_REMOTE_HEAD, (u32)so_np_remote_head_frame(0));
		so_set_ppc_word(ENGINE_REMOTE_CHANGED, (u32)so_np_remote_head_frame(0));
		so_np_publish_head(so_np_remote_count() == 1 ? ENGINE_REMOTE_HEAD : 0, ENGINE_REMOTE_CHANGED);
		so_set_ppc_word(ENGINE_INPUT_WAIT, so_np_remote_count() == 1 && !so_client_config()->test_lag_frames ?
			so_client_config()->rollback_wait_ms * 1000u : 0);
		so_client_unlock();
	} else if (frame == 2 && x.game.depth_frame != 2 && !rollback_wanted()) {
		so_log("exi: this game waits for inputs (rollback %u, scratch %u, %u opponents)",
			(unsigned)so_client_config()->rollback, (unsigned)so_ppc_scratch_free, (unsigned)so_np_remote_count());
		if (!x.game.setup_said)
			say_setup();
		x.game.setup_said = 1;
	}
	/* (a new connection's ping is first measured from this game's own acks, often after the write
	 * above: the engine's word follows it through the countdown, where the engine reads it for its
	 * skip; from frame 60 fine_sync keeps it as well) */
	if (x.game.rb && frame < 160)
		so_set_ppc_word(ENGINE_DIRECT, direct_word());

	so_client_lock();
	t_lock = so_time_us() - t_in;
	if (t_lock > x.game.max_lock_us)
		x.game.max_lock_us = (u32)t_lock;
	/* (3.6.5) The player chose to leave (Party's L+R+A+Start): the game ends for this console as when
	 * the connection is lost, and the others play on without it. */
	if (next_mode() && p[TXB_IS_LEAVING] && so_np_status() == SO_NP_CONNECTED) {
		so_log("exi: the player leaves the game at frame %d", (int)frame);
		so_np_force_disconnect(0);
		x.game.left_said = 1;	/* (its own choice: nothing went wrong, nothing to report) */
	}
	if (so_np_status() != SO_NP_CONNECTED) {
		/* A game of four ends for one console alone when the three others have given it up (one
		 * who leaves is played on without): kept as a desync is, for the lines before this one. */
		if (so_np_remote_count() > 1 && !x.game.reported && !x.game.left_said && !x.game.desyncs) {
			x.game.left_said = 1;
			desync_keep(3, frame, 0);
		}
		so_client_unlock();
		r[0] = RESP_DISCONNECTED;
		if (x.game.rb)
			publish_answer(frame, r);
		return;
	}
	remotes = so_np_remote_count();
	if (remotes > 3)
		remotes = 3;
	check_sums(finalized, checksum, remotes);
	if (next_mode())
		so_np_fill_dropped(frame);
	/* tests: a lag spike (test_spike_ms) every 300 frames (test_spike_every), the first at
	 * frame 80, so its rollbacks cross the end of the countdown (GO),
	 * timed, as the game's frames stop while it waits */
	if (x.game.rb && so_client_config()->test_spike_ms && so_client_config()->test_spike_every &&
	    frame % so_client_config()->test_spike_every == 80 % so_client_config()->test_spike_every &&
	    frame != x.game.spike_frame) {
		x.game.spike_frame = frame;
		x.game.spike_until_us = t_in + so_client_config()->test_spike_ms * 1000u;
		for (i = 0; i < remotes; i++)
			x.game.spike_head[i] = x.game.seen[i];
	}
	x.game.last_finalized = finalized;
	x.game.last_checksum = checksum;
	if (so_client_config()->test_stuck_frame && frame == so_client_config()->test_stuck_frame)
		so_socket_test_stuck = 1;
	if (so_client_config()->test_crash_frame && frame == so_client_config()->test_crash_frame) {
		/* tests: the crash file, without a crash (the game goes on, and so does the file) */
		so_log("CRASH: none, a test's (test_crash_frame %u)", (unsigned)so_client_config()->test_crash_frame);
		so_exi_report_last();
		so_logfile_crash();
	}
	if (so_client_config()->test_hang_frame && frame == so_client_config()->test_hang_frame)
		so_set_ppc_word(SO_TEST_EXIT_WORD, SO_TEST_HANG);
	for (i = 0; i < remotes; i++) {
		/* Lockstep needs this frame's input; rollback predicts up to 7 frames
		 * (Dolphin's ROLLBACK_MAX_FRAMES; it then halts a frame). */
		heads[i] = visible_head(i, frame, t_in);
		/* A game loaded before the opponent's: its frozen frames' pads are known (0), so it
		 * does not wait for them in lockstep, a round trip's half behind the opponent from
		 * then on; it runs on as a PC does, and the time sync levels it (time_sync_skip). */
		if (!x.game.rb && heads[i] < frame && frame < SO_NP_FROZEN_PADS_END && remotes == 1 &&
		    rollback_wanted() && !so_client_config()->test_no_frozen_assume) {
			if (heads[i] == x.game.seen[i] && !x.game.frozen_assumed++)
				so_log("exi: the opponent's frozen pads assumed from frame %d", (int)frame);
			heads[i] = frame;
		}
		x.game.seen[i] = heads[i];
		if (remotes > 1) {
			/* As Dolphin (shouldSkipOnlineFrame): an opponent that has left is not waited for,
			 * the game goes on with the others (fill_remote_inputs); one whose inputs hold the
			 * game for 7 s is given up, alone. */
			int late = x.game.rb ? !within_rollback_limit(frame, finalized, heads[i]) : heads[i] < frame;
			int gone = !so_np_player_active(so_np_remote_player_idx(i));
			/* (3.6.5: one that has left is waited for like anyone until its last input is settled:
			 * until then the others' reports may still add to its inputs) */
			if (next_mode() ? (gone && so_np_despawn_frame(i)) || !late : gone || !late) {
				x.game.short_since_us[i] = 0;
				continue;
			}
			ready = 0;
			if (!x.game.short_since_us[i]) {
				x.game.short_since_us[i] = t_in;
			} else if (t_in - x.game.short_since_us[i] > STALL_LIMIT_US) {
				so_log("exi: no input from player %u for 7 s: the game goes on without it",
					(unsigned)so_np_remote_player_idx(i) + 1);
				so_np_drop_player(i);
				x.game.short_since_us[i] = 0;
				if (!x.game.desyncs)
					desync_keep(2, frame, remotes);
			}
			continue;
		}
		if (x.game.rb ? !within_rollback_limit(frame, finalized, heads[i]) : heads[i] < frame)
			ready = 0;
	}
	/* Its pad is with the opponent already (on_early_pad, send_frozen_pads): it runs. */
	frozen_head = so_np_frozen_pads_sent(frame + delay);
	if (frame == 1 && frozen_head)
		x.game.catching = 1;
	sent_frozen = frame + delay <= frozen_head;
	sent_early = sent_frozen || (x.game.early_frame && frame == x.game.early_frame);
	if (sent_early)
		ready = 1;
	skip = !ready;
	if (ready && x.game.rb && !sent_early && time_sync_skip(frame)) {
		skip = 1;
		x.game.sync_skips++;
	}
	/* (as Dolphin: every pass, halted or not, by the first opponent's newest input of the 7 it
	 * would hand the game from the finalized frame on: GetSlippiRemotePad's latest_frame) */
	if (remotes) {
		s32 cap = finalized + ROLLBACK_MAX_FRAMES - 1;
		mq_note(frame, so_mq_frame(&x.game.mq, frame, t_in, !ready, heads[0] < cap ? heads[0] : cap));
	}
	if (frame > 1 && frame != x.game.depth_frame) {
		s32 behind = 0, b;
		for (i = 0; i < remotes; i++) {
			b = frame - heads[i];
			if (b > behind)
				behind = b;
		}
		x.game.depth[behind > 8 ? 8 : behind]++;
		x.game.depth_frame = frame;
	}

	if (frame == 1 && !x.dummies_sent) {
		/* sent with this frame's (after the answer), so the opponent need not wait */
		x.pend.delay_pads = 1;
		x.pend.delay_from = frozen_head + 1;
		x.pend.delay = delay;
		x.pend.valid = 1;
		x.dummies_sent = 1;
	}

	if (skip) {
		/* Only resend what is not acked: the game polls this frame again. */
		if (!ready && x.game.rb)
			x.game.limit_stalls++;
		u64 now = so_time_us();
		x.pend.valid = 1;	/* a resend, after the answer */
		r[0] = RESP_SKIP;
		x.stats.b0_skips++;
		x.game.skips++;
		if (x.game.run++ == 0) {
			x.game.held++;
			x.game.wait_since_us = now;
		}
		if (x.game.run > x.game.longest)
			x.game.longest = x.game.run;
		if (!x.stall_since_us) {
			x.stall_since_us = now;
		} else if (now - x.stall_since_us > STALL_LIMIT_US && remotes <= 1) {	/* (more: each by itself, above) */
			so_log("exi: no input from the opponent for 7 s; disconnecting");
			so_np_force_disconnect(0);
			r[0] = RESP_DISCONNECTED;
		}
	} else {
		SoLocalPad pad;
		pad.frame = frame + delay;
		pad.checksum_frame = finalized;
		pad.checksum = checksum;
		memcpy(pad.pad, p + 14, SO_PAD_SIZE);
		x.pend.pad = pad;	/* sent after the answer */
		x.pend.has_pad = 1;
		if (sent_frozen && (so_get_u32(p + 14) || so_get_u32(p + 18))) {
			x.stats.early_wrong++;
			if (!x.game.early_wrong++)
				so_log("exi: FROZEN PAD SENT AHEAD IS WRONG: this is frame %d's", (int)frame);
		}
		/* (A pad sent ahead has gone, moments ago: another datagram now would only queue behind it
		 * in IOS, 1.7 ms each, and hold up the acks. What's not acked goes with the next pad.) */
		x.pend.valid = !sent_early;
		r[0] = RESP_NORMAL;
		if (x.game.early_frame && frame >= x.game.early_frame) {
			/* the pad the opponent has must be this frame's and this pad */
			if (!sent_early || memcmp(x.game.early_pad, p + 14, so_pad_wire)) {
				x.stats.early_wrong++;
				if (!x.game.early_wrong++)
					so_log("exi: PAD SENT AHEAD IS WRONG: for frame %d, this is frame %d", (int)x.game.early_frame,
						(int)frame);
			}
			x.game.early_frame = 0;
		}
		if (x.game.rb) {
			for (i = 0; i < remotes; i++)
				if (heads[i] < frame) {
					x.game.predicted++;
					break;
				}
			if (x.game.catching && catch_up_after_load(frame, delay, frozen_head)) {
				r[0] = RESP_ADVANCE;
			} else if (x.game.catching) {
				/* (the second of two in a refresh, or a game too far behind its polls) */
			} else if (time_sync_advance(frame)) {
				r[0] = RESP_ADVANCE;
				x.game.advances++;
				if (x.game.behind_us > 0)	/* the same lost time, seen by both */
					x.game.behind_us -= x.game.behind_us < FRAME_US ? x.game.behind_us : FRAME_US;
			} else if (catch_up(frame)) {
				r[0] = RESP_ADVANCE;
			}
			x.game.extra_poll = r[0] == RESP_ADVANCE;
		} else if (remotes > 1 && multi_level(frame, heads, remotes)) {
			r[0] = RESP_ADVANCE;	/* (more than one opponent, waiting for inputs: level with them) */
		}
		x.stats.b0_frames++;
		x.game.frames++;
		if (x.game.run && x.game.waits_logged < 8) {
			x.game.wait_frame[x.game.waits_logged] = (u32)frame;
			x.game.wait_ms[x.game.waits_logged] = (u32)((so_time_us() - x.game.wait_since_us) / 1000);
			x.game.waits_logged++;
		}
		x.game.run = 0;
		x.stall_since_us = 0;
	}
	fill_remote_inputs(r, frame, finalized, heads, remotes);
	if (x.game.rb)
		publish_answer(frame, r);
	{
		LastPoll *l = &last_polls[last_poll_n++ % LAST_POLLS];
		l->frame = frame;
		l->finalized = finalized;
		l->head = heads[0];
		l->more[0] = remotes > 1 ? (s16)(heads[1] - heads[0]) : 0;
		l->more[1] = remotes > 2 ? (s16)(heads[2] - heads[0]) : 0;
		l->remotes = (u8)remotes;
		l->answer = r[0];
		l->ms = (u32)(t_in / 1000);
	}
	so_client_unlock();
	{
		u32 took = (u32)(so_time_us() - t_in);
		if (took > x.game.max_answer_us)
			x.game.max_answer_us = took;
		if (took > 1000)
			x.game.slow_answers++;
	}
}

/* Appends `src` cut to `chars` code points. */
static void utf8_append(char *dst, int size, const char *src, int chars)
{
	int len = (int)strlen(dst);
	const u8 *p = (const u8 *)src;
	while (*p && chars > 0) {
		int step = 1;
		if ((*p & 0xE0) == 0xC0)
			step = 2;
		else if ((*p & 0xF0) == 0xE0)
			step = 3;
		else if ((*p & 0xF8) == 0xF0)
			step = 4;
		if (len + step >= size)
			break;
		while (step-- && *p)
			dst[len++] = (char)*p++;
		chars--;
	}
	dst[len] = '\0';
}

/* ---- 0xB3 GET_MATCH_STATE (prepareOnlineMatchState) ------------------------ */

static u8 mm_state(void)
{
	if (x.forced_error)
		return MM_ERROR;
	switch (so_client_state()) {
	case SO_CLIENT_SEARCHING: return MM_MATCHMAKING;
	case SO_CLIENT_CONNECTING: return MM_OPPONENT_CONNECTING;
	case SO_CLIENT_CONNECTED:
	case SO_CLIENT_DISCONNECTED: return MM_CONNECTION_SUCCESS;
	case SO_CLIENT_ERROR: return MM_ERROR;
	default: return MM_IDLE;
	}
}

/* (Teams: by team, the colour being the team's) */
static u16 shade_key(const u8 *b, int slot, int teams)
{
	u8 c = b[0x60 + slot * 0x24];
	return (u16)(((c == 0x13 ? 0x12 : c) << 8) | b[(teams ? 0x69 : 0x63) + slot * 0x24]);
}

/* Fixed-rules modes (Unranked, Ranked, Party) and Teams take the 26 characters
 * only, and the fixed-rules ones only the stages their match allows: as Dolphin,
 * someone who chose otherwise ends the lobby (prepareOnlineMatchState). 0: fine;
 * 1: end the lobby; 2: and say why (the local player's character). The client
 * lock is held. */
static int rules_broken(int remotes)
{
	const SoSelections *lps = so_np_local_selections();
	int fixed = x.mode == SO_MODE_UNRANKED || x.mode == SO_MODE_RANKED || x.mode == SO_MODE_PARTY;
	u16 stage = 0x1F;
	int i, k, found = 0;
	if (!fixed && x.mode != SO_MODE_TEAMS)
		return 0;
	if (lps->character_id >= 26)
		return 2;
	for (i = 0; i < remotes; i++)
		if (so_np_remote_selections(i)->character_id >= 26)
			return 1;
	if (!fixed)
		return 0;
	/* the stage the game would get: the first chosen one, in port order */
	for (k = 0; k <= remotes && !found; k++) {
		const SoSelections *s = lps->player_idx == k ? lps : NULL;
		for (i = 0; i < remotes && !s; i++)
			if (so_np_remote_selections(i)->player_idx == k)
				s = so_np_remote_selections(i);
		if (s && s->is_stage_selected) {
			stage = s->stage_id;
			found = 1;
		}
	}
	for (i = 0; i < x.allowed_count; i++)
		if (x.allowed[i] == stage)
			return 0;
	return 1;
}

/* isSlippiChatEnabled: the setting on, or "in Direct only" and this is Direct */
static int chat_on(void)
{
	u8 chat = so_client_config()->chat;
	return chat == 1 || (chat == 2 && x.mode == SO_MODE_DIRECT);
}

/* Writes the per-game fields into the match block once both players have
 * picked. The client lock is held. */
static u32 build_game(int remotes, u8 *alt_out)
{
	const SoSelections *by_port[SO_REMOTE_MAX + 1] = { NULL };
	const SoSelections *lps = so_np_local_selections();
	/* Dolphin's draws for a Teams lobby whose players all chose one team (by the
	 * random seed: the same on every side) */
	static const u8 team_draws[6][4] = {
		{ 0, 0, 1, 1 }, { 1, 1, 0, 0 }, { 0, 1, 1, 0 }, { 1, 0, 0, 1 }, { 0, 1, 0, 1 }, { 1, 0, 1, 0 },
	};
	u16 stage = 0x1F;
	u8 alt = 0, *b = x.match_block;
	u32 rng;
	int i, j, teams = x.mode == SO_MODE_TEAMS, one_team = 1;

	if (lps->player_idx <= SO_REMOTE_MAX)
		by_port[lps->player_idx] = lps;
	for (i = 0; i < remotes; i++) {
		const SoSelections *s = so_np_remote_selections(i);
		if (s->player_idx <= SO_REMOTE_MAX)
			by_port[s->player_idx] = s;
	}
	for (i = 0; i <= remotes; i++) {
		if (by_port[i] && by_port[i]->is_stage_selected) {
			stage = by_port[i]->stage_id;
			alt = by_port[i]->alt_stage_mode;
			break;
		}
	}
	rng = so_np_is_decider() ? lps->rng_offset : so_np_remote_selections(0)->rng_offset;

	for (i = 0; i <= remotes; i++)
		if (by_port[i] && by_port[0] && by_port[i]->team_id != by_port[0]->team_id)
			one_team = 0;
	for (i = 0; i <= remotes; i++) {
		const SoSelections *s = by_port[i];
		if (!s || !s->is_character_selected)
			continue;
		b[0x60 + i * 0x24] = s->character_id;
		b[0x63 + i * 0x24] = s->character_color;	/* (Teams: the game puts the team's colour here) */
		b[0x67 + i * 0x24] = 0;
		b[0x69 + i * 0x24] = !teams ? 0 : one_team ? team_draws[rng % 6][i & 3] : s->team_id;
	}
	/* Shades: each human slot gets the number of earlier slots with the same
	 * character (Sheik counting as Zelda) and colour. */
	for (i = 0; i < 4; i++) {
		u8 shade = 0;
		if (b[0x61 + i * 0x24] != 0)
			continue;
		for (j = 0; j < i; j++)
			if (b[0x61 + j * 0x24] == 0 && shade_key(b, j, teams) == shade_key(b, i, teams))
				shade++;
		b[0x67 + i * 0x24] = shade;
	}
	b[0x08] = (u8)teams;
	b[0x61 + 2 * 0x24] = remotes >= 2 ? 0 : 3;
	b[0x61 + 3 * 0x24] = remotes >= 3 ? 0 : 3;
	so_put_u16(b + 0x0E, stage);
	if (x.mode == SO_MODE_DIRECT)
		b[0x02] &= 0xF7;	/* pause allowed */
	else
		b[0x02] |= 0x08;
	if (x.mode != SO_MODE_DIRECT && x.mode != SO_MODE_TEAMS)
		alt = 0;
	for (i = 0; i < 4; i++) {
		b[0x62 + i * 0x24] = 4;	/* stocks */
		so_put_u16(b + 0x70 + i * 0x24, 0);	/* percent */
	}
	*alt_out = alt;
	return rng;
}

static void on_get_match_state(void)
{
	const SoUser *user = so_client_user();
	const SoMatchResult *m;
	u8 *r, state, local_ready, remote_ready = 0, chat_id = 0, chat_player = 0, sent = 0, alt = 0;
	u8 game_delay = so_client_config()->delay;
	u32 rng = 0;
	int remotes = 0, i;
	char error[128];

	for (;;) {
		r = answer(MSRB_SIZE);
		if (!r)
			return;
		state = mm_state();
		local_ready = x.local_sel.is_character_selected;
		remote_ready = 0;
		if (state != MM_CONNECTION_SUCCESS) {
			x.have_client = 0;
			break;
		}
		m = so_client_match();
		x.local_idx = m->local_player_index;
		if (!x.have_client) {
			/* First answer since connecting: the stages this match allows,
			 * and our selections (made before the connection) go out. */
			x.have_client = 1;
			x.allowed_count = 0;
			for (i = 0; i < m->stage_count && i < MAX_STAGES; i++)
				x.allowed[x.allowed_count++] = m->stages[i];
			if (!x.allowed_count) {
				memcpy(x.allowed, default_stages, sizeof(default_stages));
				x.allowed_count = sizeof(default_stages) / sizeof(default_stages[0]);
			}
			x.pool_count = 0;
			x.local_sel.stage_id = random_stage();
			so_client_lock();
			so_np_set_selections(&x.local_sel);
			so_client_unlock();
		}
		so_client_lock();
		remotes = so_np_remote_count();
		{
			int connected = so_np_status() == SO_NP_CONNECTED, active = 0;
			for (i = 0; i < remotes; i++)
				active += so_np_player_active(so_np_remote_player_idx(i));
			if (!connected || active != remotes) {
				so_client_unlock();
				cleanup();	/* the opponent left: end the lobby */
				continue;	/* and answer for the new state */
			}
		}
		remote_ready = 1;
		for (i = 0; i < remotes; i++)
			if (!so_np_remote_selections(i)->is_character_selected)
				remote_ready = 0;
		if (remotes == 1) {
			x.local_idx = so_np_is_decider() ? 0 : 1;
			x.remote_idx = so_np_is_decider() ? 1 : 0;
		}
		/* Chat: the message we sent, else one received (one-shot each). */
		sent = x.sent_chat;
		x.sent_chat = 0;
		if (!sent) {
			u8 from;
			chat_id = (u8)so_np_take_chat(&from);
			if (chat_id && chat_id != SO_CHAT_DISABLED && !chat_on()) {
				so_np_send_chat(SO_CHAT_DISABLED);
				chat_id = 0;
			}
			/* (more than one opponent: by its sender's port, and "chat disabled" is not shown) */
			chat_player = chat_id ? from : 0;
			if (chat_id == SO_CHAT_DISABLED && remotes != 1)
				chat_id = chat_player = 0;
		} else {
			chat_player = x.local_idx;
		}
		if (remotes == 1)
			chat_player = sent ? x.local_idx : x.remote_idx;
		if (local_ready && remote_ready) {
			int broken = rules_broken(remotes);
			if (broken) {
				so_client_unlock();
				so_log("exi: a selection this mode does not allow: the lobby ends");
				cleanup();
				x.forced_error = (u8)(broken == 2);
				continue;
			}
			rng = build_game(remotes, &alt);
			if (!x.setup_on) {	/* (the game asks twice on its way in: from the first) */
				x.setup_ms = so_time_ms();
				x.setup_on = 1;
			}
		}
		game_delay = lobby_delay(remotes);	/* (with the lock: it reads the connections) */
		so_client_unlock();
		break;
	}

	r[0x000] = state;
	r[0x001] = local_ready;
	r[0x002] = remote_ready;
	r[0x003] = x.local_idx;
	r[0x004] = x.remote_idx;

	/* Always: mode, timer and items. */
	if (x.mode == SO_MODE_PARTY && !next_mode()) {
		/* (as Slippi Dolphin 3.6.4's games of it: a replay of four of them, 2026-11-14; from 3.6.5
		 * Party is stock mode with 8 minutes, as every other mode) */
		x.match_block[0x00] = 0x12;	/* time mode */
		x.match_block[0x03] = 0xCC;	/* score UI shown */
		so_put_u32(x.match_block + 0x10, 5 * 60);
	} else {
		x.match_block[0x00] = 0x32;	/* stock mode, timer counting down */
		x.match_block[0x03] = 0x4C;	/* score UI hidden */
		so_put_u32(x.match_block + 0x10, 8 * 60);
	}
	m = current_match();
	x.match_block[0x0B] = 0xFF;	/* items off */
	{
		u32 items = m ? m->items : 0;
		u32 hi = 0xF8000000u, lo = 0x0F000000u;
		if (items) {
			/* items << 28 across the 64-bit field */
			hi |= items >> 4;
			lo |= items << 28;
			x.match_block[0x0B] = 3;
		}
		so_put_u32(x.match_block + 0x23, hi);
		so_put_u32(x.match_block + 0x27, lo);
	}

	so_put_u32(r + 0x005, rng);
	r[0x009] = game_delay;
	r[0x00A] = sent;
	r[0x00B] = chat_id;
	r[0x00C] = chat_player;
	/* 0x00D, 0x00E: ranks (Ranked only) */
	if (x.have_client)
		so_game_string(user->display_name, 15, r + 0x00F);
	if (m) {
		char opponents[64] = "";
		for (i = 0; i < 4 && i < m->player_count; i++) {
			so_game_string(m->players[i].display_name, 15, r + 0x02E + i * 31);
			so_game_connect_code(m->players[i].connect_code, r + 0x0C9 + i * 10);
			strlcpy((char *)r + 0x0F1 + i * 29, m->players[i].uid, 29);
		}
		/* The opponents, as the game names them: the other human players'
		 * names, each cut to share 15 characters, joined with '/'. */
		{
			const char *names[4];
			int n = 0, each;
			for (i = 0; i < 4 && i < m->player_count; i++) {
				if (i == x.local_idx || x.match_block[0x61 + i * 0x24] != 0 || !m->players[i].display_name[0])
					continue;
				/* (Teams: not the teammate) */
				if (x.mode == SO_MODE_TEAMS && x.match_block[0x69 + i * 0x24] == x.match_block[0x69 + x.local_idx * 0x24])
					continue;
				names[n++] = m->players[i].display_name;
			}
			each = n ? (15 - (n - 1)) / n : 0;
			for (i = 0; i < n; i++) {
				if (i)
					strlcpy(opponents + strlen(opponents), "/", sizeof(opponents) - strlen(opponents));
				utf8_append(opponents, sizeof(opponents), names[i], each);
			}
		}
		so_game_string(opponents, 15, r + 0x0AA);
		strlcpy((char *)r + 0x38E, m->match_id, 51);
	}
	error[0] = '\0';
	if (x.forced_error)
		strlcpy(error, "The character you selected is not allowed in this mode", sizeof(error));
	else if (client_up())
		so_client_error(error, sizeof(error));
	so_game_string(error, 120, r + 0x165);
	memcpy(r + 0x256, x.match_block, MATCH_BLOCK_SIZE);
	r[0x3C1] = alt;
	if (next_mode() && client_up() && (m = so_client_match()) != NULL) {
		/* (3.6.5: for the replay's Game Start. The rank information before it stays zero: SlippWii
		 * has no ranks) */
		for (i = 0; i < m->player_count; i++)
			if (m->players[i].port >= 1 && m->players[i].port <= 4)
				r[MSRB_IS_BOT + m->players[i].port - 1] = m->players[i].is_bot;
	}
}

/* ---- the other commands --------------------------------------------------- */

static void remember_code(int teams, const u8 *code, int len);

/* startFindMatch */
static void on_find_opponent(const u8 *p)
{
	SoSearch search;
	int len = 0;
	memset(&search, 0, sizeof(search));
	search.mode = p[1];
	while (len < 18 && p[2 + len])
		len++;	/* the code, cut at its first 0 */
	if (len > (int)sizeof(search.connect_code))
		len = sizeof(search.connect_code);
	memcpy(search.connect_code, p + 2, len);
	search.connect_code_len = (u8)len;
	if (search.mode == SO_MODE_DIRECT || search.mode == SO_MODE_TEAMS)
		remember_code(search.mode == SO_MODE_TEAMS, search.connect_code, len);
	x.mode = search.mode;
	x.have_client = 0;
	x.last_frame = 0;
	if (!client_up())
		return;
	so_np_allow_frozen_pads(so_client_config()->rollback && so_ppc_scratch_free &&
		so_client_config()->test_frozen_pads);
	so_log("exi: find opponent (mode %u)", (unsigned)search.mode);
	so_client_find_match(&search);
}

/* setMatchSelections */
static void on_set_selections(const u8 *p)
{
	SoSelections s;
	u8 option = p[7];
	memset(&s, 0, sizeof(s));
	s.team_id = p[1];
	s.character_id = p[2];
	s.character_color = p[3];
	s.is_character_selected = p[4] != 0;
	s.stage_id = so_get_u16(p + 5);
	s.is_stage_selected = option == 1 || option == 3;
	if (option == 3)
		s.stage_id = random_stage();
	s.alt_stage_mode = p[9];
	s.rng_offset = so_random() % 0xFFFF;
	if (s.is_character_selected || s.is_stage_selected)
		so_log("exi: our selections: char %u/%u%s, team %u, stage %u%s", (unsigned)s.character_id,
			(unsigned)s.character_color, s.is_character_selected ? " (picked)" : "", (unsigned)s.team_id,
			(unsigned)s.stage_id, s.is_stage_selected ? " (picked)" : "");
	merge_selections(&x.local_sel, &s);
	if (s.is_character_selected)
		so_pf_want_char(0, s.character_id, s.character_color);
	if (s.is_stage_selected)
		so_pf_want_stage(s.stage_id);
	if (x.have_client) {
		so_client_lock();
		if (so_np_status() == SO_NP_CONNECTED)
			so_np_set_selections(&x.local_sel);
		so_client_unlock();
	}
}

static void on_online_status(void)
{
	const SoUser *user = so_client_user();
	u8 *r = answer(OSB_SIZE);
	if (so_client_state() == SO_CLIENT_OFF)
		return;	/* not logged in: the online modes stay locked */
	/* 2: "update required", as Slippi Dolphin answers once Slippi's server has
	 * named a version over its own: the game locks the online modes */
	r[0] = so_client_outdated() ? 2 : 1;
	if (r[0] == 2 && !x.said_outdated) {
		x.said_outdated = 1;
		so_log("exi: the game is told an update is required: its online modes are locked");
	}
	so_game_string(user->display_name, 15, r + 0x01);
	so_game_connect_code(user->connect_code, r + 0x20);
}

static void on_send_chat(const u8 *p)
{
	if (!x.have_client || !chat_on())
		return;
	so_client_lock();
	if (so_np_status() == SO_NP_CONNECTED) {
		x.sent_chat = p[1];
		so_np_send_chat(p[1]);
	}
	so_client_unlock();
}

/* The code histories behind connect-code autocomplete (0xBE), most recent
 * first: the codes searched in Direct, and the lobby codes of Teams (raw
 * Shift-JIS, 2 bytes a character, as 0xB4 sends them), then selftest_code.
 * Slippi Dolphin keeps them in direct-codes.json and teams-codes.json. */
#define HISTORY_MAX 8
typedef struct {
	struct {
		u8 code[16];
		u8 len;	/* bytes */
	} e[HISTORY_MAX + 1];
	int count;
	u8 dirty, loaded;
} History;
static History histories[2];	/* Direct's, Teams' */

/* The files: "SLDC", a count, then per code its length in bytes and 16 bytes
 * of Shift-JIS, most recent first. */
static const char *const history_paths[2] = { "/slippwii/direct-codes.bin", "/slippwii/teams-codes.bin" };
#define HISTORY_FILE_SIZE (5 + HISTORY_MAX * 17)

static void load_history(History *h)
{
	u8 buf[HISTORY_FILE_SIZE];
	int n, i;
	const SoConfig *c = so_client_config();
	h->loaded = 1;
	if (c && c->test_control)
		return;	/* tests keep it to the session: not the player's codes */
	n = so_read_file(history_paths[h - histories], buf, sizeof(buf));
	if (n < 5 || memcmp(buf, "SLDC", 4) != 0)
		return;
	for (i = 0; i < buf[4] && i < HISTORY_MAX && 5 + (i + 1) * 17 <= n; i++) {
		const u8 *e = buf + 5 + i * 17;
		if (e[0] == 0 || e[0] > 16 || e[0] % 2)
			break;
		h->e[i].len = e[0];
		memcpy(h->e[i].code, e + 1, 16);
		h->count = i + 1;
	}
	so_log("exi: %d %s codes remembered", h->count, h == histories ? "direct" : "teams");
}

void so_exi_save_history(void)
{
	u8 buf[HISTORY_FILE_SIZE];
	int i, k, count;
	const SoConfig *c = so_client_config();
	for (k = 0; k < 2; k++) {
		History *h = &histories[k];
		if (!h->dirty)
			continue;
		h->dirty = 0;
		if (c && c->test_control)
			continue;
		count = h->count;
		memset(buf, 0, sizeof(buf));
		memcpy(buf, "SLDC", 4);
		buf[4] = (u8)count;
		for (i = 0; i < count; i++) {
			buf[5 + i * 17] = h->e[i].len;
			memcpy(buf + 6 + i * 17, h->e[i].code, 16);
		}
		if (so_write_file(history_paths[k], buf, 5 + count * 17) < 0)
			so_log("exi: could not save the %s codes", k ? "teams" : "direct");
	}
}

static void remember_code(int teams, const u8 *code, int len)
{
	History *h = &histories[teams ? 1 : 0];
	int i, j;
	if (len <= 0 || len > 16 || len % 2)
		return;
	if (!h->loaded)
		load_history(h);
	for (i = 0; i < h->count; i++)
		if (h->e[i].len == len && memcmp(h->e[i].code, code, len) == 0)
			break;
	if (i == h->count && h->count < HISTORY_MAX)
		h->count++;
	if (i == h->count)
		i = h->count - 1;	/* full: drop the oldest */
	for (j = i; j > 0; j--)
		h->e[j] = h->e[j - 1];
	memset(h->e[0].code, 0, sizeof(h->e[0].code));
	memcpy(h->e[0].code, code, len);
	h->e[0].len = (u8)len;
	h->dirty = 1;
}

/* The history with selftest_code last (unless already in it); returns the count. */
static int code_list(History *h)
{
	const SoConfig *c = so_client_config();
	u8 code[16];
	int n, i;
	if (!h->loaded)
		load_history(h);
	n = c && c->selftest_code[0] ? so_encode_connect_code(c->selftest_code, code, sizeof(code)) : -1;
	for (i = 0; n > 0 && i < h->count; i++)
		if (h->e[i].len == n && memcmp(h->e[i].code, code, n) == 0)
			n = -1;
	if (n > 0) {
		memcpy(h->e[h->count].code, code, n);
		h->e[h->count].len = (u8)n;
		return h->count + 1;
	}
	return h->count;
}

/* Does this code start with the typed characters? */
static int code_matches(const History *h, int index, const u8 *p, u8 len)
{
	int k, ok = h->e[index].len / 2 >= len;
	for (k = 0; ok && k < len; k++)
		ok = h->e[index].code[2 * k] == p[1 + 3 * k] && h->e[index].code[2 * k + 1] == p[2 + 3 * k];
	return ok;
}

/* handleNameEntryLoad. `dir` from the shown code (index): none (0), a step
 * older (1), newer (2, never past the newest) or back to the newest
 * (3), the first code that starts with the typed characters, walking the
 * same way. None found: the shown one stays if it still matches (else
 * scrolling past the end would clear it), or the typed text is echoed.
 * The mode's history: Teams has its own. */
static void on_code_suggestion(const u8 *p)
{
	u8 *r = answer(30);
	u8 len = p[0x19], dir = p[0x1E];
	u32 shown = so_get_u32(p + 0x1A), index = shown;
	History *h = &histories[p[0x1F] == SO_MODE_TEAMS ? 1 : 0];
	int count = code_list(h), found = -1, i;
	if (len > 8)
		len = 8;
	if (dir == 1)
		index++;
	else if (dir == 2 && index > 0)
		index--;
	else if (dir == 3)
		index = 0;
	while (found < 0 && index < (u32)count) {
		if (code_matches(h, (int)index, p, len))
			found = (int)index;
		else
			index += dir == 2 ? (u32)-1 : 1;	/* unsigned: below 0 ends the walk */
	}
	if (found < 0 && shown < (u32)count && code_matches(h, (int)shown, p, len))
		found = (int)shown;
	if (found < 0) {
		/* not found: the typed text, echoed */
		memcpy(r + 1, p + 1, 3 * len);
		r[0x19] = len;
		memcpy(r + 0x1A, p + 0x1A, 4);
		return;
	}
	r[0] = 1;
	for (i = 0; i < h->e[found].len / 2 && i < 8; i++) {
		r[1 + 3 * i] = h->e[found].code[2 * i];
		r[2 + 3 * i] = h->e[found].code[2 * i + 1];
	}
	r[0x19] = (u8)(h->e[found].len / 2);
	so_put_u32(r + 0x1A, (u32)found);
}

/* handleGetPlayerSettings: every player's 16 quick-chat messages. */
static void on_player_settings(void)
{
	static const char *const defaults[16] = {
		"ggs", "one more", "brb", "good luck", "well played", "that was fun", "thanks", "too good",
		"sorry", "my b", "lol", "wow", "gotta go", "one sec", "let's play again later", "bad connection",
	};
	const SoMatchResult *m = current_match();
	u8 *r = answer(PLAYER_SETTINGS_SIZE);
	int i, j;
	if (!r)
		return;
	for (i = 0; i < 4; i++) {
		const SoMmPlayer *p = NULL;
		int k;
		for (k = 0; m && k < m->player_count; k++)
			if (m->players[k].port == i + 1 && m->players[k].has_chat_messages)
				p = &m->players[k];
		for (j = 0; j < 16; j++)
			so_game_string(p ? p->chat_messages[j] : defaults[j], 25, r + i * 816 + j * 51);
	}
}

static void on_premade_text(const u8 *p, int load)
{
	u8 text[400];
	const SoMatchResult *m = current_match();
	const char *name = "";
	int id = p[1], n;
	if (id >= 1 && id <= 4 && m && id - 1 < m->player_count)
		name = m->players[id - 1].display_name;
	n = so_premade_text(id, p[2], name, text, sizeof(text));
	if (n < 0) {
		so_log("exi: premade text %d (param 0x%02x) unknown", id, p[2]);
		n = 0;
	}
	if (load) {
		u8 *r = answer((u32)n);
		if (r)
			memcpy(r, text, n);
	} else {
		so_put_u32(answer(4), (u32)n);
	}
}

/* Diagnostics: the last file the game asked for, and the last archives it
 * parsed (tools/ppc/probe_archive.s), shown when it panics. */
static char last_file[40];
static struct {
	char file[40];
	u32 data, length, caller, header[3];
} archives[4];
static u32 archive_count;

/* The PPC's timebase ticks (a quarter of the 243 MHz bus clock) as microseconds */
static u32 lat_us(u32 ticks)
{
	return ticks < 0x3FFFFFFFu ? ticks * 4 / 243 : 0xFFFFFFFFu;
}

static void log_archive(u32 i)
{
	so_log("ARCHIVE %u: after %s: data %08x length %u (header size %u data %u relocs %u) from %08x", (unsigned)i,
		archives[i % 4].file, (unsigned)archives[i % 4].data, (unsigned)archives[i % 4].length,
		(unsigned)archives[i % 4].header[0], (unsigned)archives[i % 4].header[1], (unsigned)archives[i % 4].header[2],
		(unsigned)archives[i % 4].caller);
}

static void on_file(const u8 *p, int load)
{
	char name[0x41];
	u32 len;
	memcpy(name, p + 1, 0x40);
	name[0x40] = '\0';
	len = so_files_size(name);
	strlcpy(last_file, name, sizeof(last_file));
	if (!load) {
		so_put_u32(answer(4), len);
		if (len)
			so_log("exi: serving %s (%u bytes)", name, (unsigned)len);
		else if (x.stats.disc_files++ < 64)
			so_log("exi: %s from the disc", name);
	} else if (len) {
		answer(0);
		strlcpy(x.file, name, sizeof(x.file));
		x.file_pending = 1;
		x.resp_len = len;
	} else {
		answer(0);
	}
}

static void on_gct(int load, u32 address)
{
	const u8 *gct;
	u32 len = so_files_gct(&gct);
	if (!load) {
		so_put_u32(answer(4), len);
	} else {
		/* The bootloader writes its buffer's address after the command. */
		int linked = len ? so_gct_link((u8 *)gct, len, address) : 0;
		answer(0);
		x.resp = gct;
		x.resp_len = len;
		so_log("exi: codeset loaded (%u bytes) for %08x, %d hooks linked", (unsigned)len, (unsigned)address, linked);
	}
}

/* Hands what the last 0xB0 queued to the network thread (so_exi.h,
 * so_exi_after_ack): the kernel's main thread does no network I/O. */
static void send_pending(void)
{
	if (!x.pend.valid)
		return;
	if (x.pend.delay_pads) {
		/* empty pads for frames 1..delay: the first input is for frame
		 * 1 + delay (handleSendInputs) */
		SoLocalPad pad;
		int f;
		for (f = x.pend.delay_from; f <= x.pend.delay; f++) {
			memset(&pad, 0, sizeof(pad));
			pad.frame = f;
			so_client_queue_pad(&pad);
		}
	}
	so_client_queue_pad(x.pend.has_pad ? &x.pend.pad : NULL);
	memset(&x.pend, 0, sizeof(x.pend));
}

void so_exi_after_ack(void)
{
	if (x.pend.valid && x.answer_read)
		send_pending();
}

void so_exi_other_write(void)
{
	x.has_resp = 0;
	x.file_pending = 0;
	x.game.replay_writes++;
}

static void stash_answer(void)
{
	if (!x.file_pending) {
		if (x.resp_len > PLAYER_SETTINGS_SIZE)
			return;
		if (!x.stash.buf && !(x.stash.buf = (u8 *)so_malloc(PLAYER_SETTINGS_SIZE)))
			return;
		memcpy(x.stash.buf, x.resp, x.resp_len);
	} else {
		strlcpy(x.stash.file, x.file, sizeof(x.stash.file));
	}
	x.stash.file_pending = x.file_pending;
	x.stash.len = x.resp_len;
	x.stash.valid = 1;
	x.stashes++;
}

/* The read is the set-aside answer's (its length, and not the current answer's) */
static int unstash_for(u32 len)
{
	if (!x.stash.valid || len != x.stash.len || (x.has_resp && len == x.resp_len))
		return 0;
	x.stash.valid = 0;
	x.file_pending = x.stash.file_pending;
	if (x.file_pending) {
		strlcpy(x.file, x.stash.file, sizeof(x.file));
	} else {
		x.resp = x.stash.buf;
	}
	x.resp_len = x.stash.len;
	x.has_resp = 1;
	x.unstashed++;
	if (x.unstashed <= 4)
		so_log("exi: a %u-byte answer kept across the pad's renewal (%s)", (unsigned)len,
			x.file_pending ? x.file : "an answer");
	return 1;
}

void so_exi_write(const u8 *request, u32 dma_len)
{
	static u8 seen[32];
	u8 cmd = request[0];
	init_once();
	so_main_loop_quick = 1;	/* its read comes next, with the game at a standstill */
	if (x.pend.valid)
		send_pending();	/* the game wrote again without reading the answer */
	x.answer_read = 0;
	x.stats.commands++;
	x.stats.last_command = cmd;
	if ((cmd == CMD_FILE_LENGTH || cmd == CMD_FILE_LOAD) && x.game.rb && !so_client_config()->test_file_sleep)
		file_busy_until_us = so_time_us() + FILE_BUSY_US;
	if (cmd == 0xD1 && x.game.rb && !ct.on && !after_load.want) {
		ct.on = 1;
		ct.n = 0;
		ct.t0 = so_time_us();
	}
	ct_note(cmd);
	if (!(seen[cmd >> 3] & (1 << (cmd & 7)))) {
		seen[cmd >> 3] |= (u8)(1 << (cmd & 7));
		so_log("exi: first 0x%02X (%u bytes)", cmd, (unsigned)dma_len);
	}
	/* A command without an answer leaves none behind (Dolphin's read queue
	 * keeps the old one, but nothing reads it). Not the diagnostics' report, though:
	 * tools/ppc/stall.s sends it from the retrace interrupt, maybe between another
	 * command's write and read. That lost 0xB3's answer once in about 1400 games: the game
	 * took a stale buffer for its match data and crashed in Slippi's nametag code.
	 *
	 * The pad's renewal comes from an interrupt too (0xB0, and the engine's 0xE0): with
	 * rollback, from a line interrupt in the middle of the game's frame, so between the
	 * main thread's write and its read. It cleared a Pokemon Stadium transformation's 0xD2:
	 * the game read nothing, parsed the stale buffer and panicked (HSD_ArchiveParse,
	 * lbarchive.c:73; run 20261109-034806). So a main-thread answer still unread is set
	 * aside for it, and a read of that answer's length gets it back (so_exi_read). */
	if (cmd == CMD_ONLINE_INPUTS || cmd == CMD_EARLY_PAD) {
		if ((x.has_resp || x.file_pending) && !x.answer_read_main && !x.stash.valid)
			stash_answer();
	} else if (cmd != CMD_HEARTBEAT) {
		x.stash.valid = 0;	/* (the main thread went on: what it left unread is not wanted) */
	}
	if (cmd != CMD_HEARTBEAT) {
		x.has_resp = 0;
		x.file_pending = 0;
	}
	if (cmd != CMD_ONLINE_INPUTS && cmd != CMD_EARLY_PAD && cmd != CMD_HEARTBEAT)
		x.answer_read_main = 0;
	switch (cmd) {
	case CMD_ONLINE_INPUTS:
		on_online_inputs(request);
		break;
	case CMD_CAPTURE_SAVESTATE:
	case CMD_LOAD_SAVESTATE:
		/* Lockstep never predicts, so these should never come. */
		if (x.stats.savestate_requests++ < 4)
			so_log("exi: unexpected savestate command 0x%02x (frame %d)", cmd, (int)so_get_u32(request + 1));
		break;
	case CMD_GET_MATCH_STATE:
		on_get_match_state();
		break;
	case CMD_FIND_OPPONENT:
		on_find_opponent(request);
		break;
	case CMD_SET_MATCH_SELECTIONS:
		on_set_selections(request);
		break;
	case CMD_GET_ONLINE_STATUS:
		on_online_status();
		break;
	case CMD_CLEANUP_CONNECTION:
		cleanup();
		break;
	case CMD_SEND_CHAT_MESSAGE:
		on_send_chat(request);
		break;
	case CMD_GET_NEW_SEED:
		so_put_u32(answer(4), so_random());
		break;
	case CMD_FETCH_CODE_SUGGESTION:
		on_code_suggestion(request);
		break;
	case CMD_GP_FETCH_STEP:
		answer(6);	/* not found (Ranked only) */
		break;
	case CMD_GET_PLAYER_SETTINGS:
		on_player_settings();
		break;
	case CMD_LOG_MESSAGE: {
		char text[126];
		memcpy(text, request + 3, sizeof(text) - 1);
		text[sizeof(text) - 1] = '\0';
		so_log("game: %s", text);
		break;
	}
	case CMD_FILE_LENGTH:
	case CMD_FILE_LOAD:
		on_file(request, cmd == CMD_FILE_LOAD);
		break;
	case CMD_GCT_LENGTH:
	case CMD_GCT_LOAD:
		on_gct(cmd == CMD_GCT_LOAD, cmd == CMD_GCT_LOAD ? so_get_u32(request + 1) : 0);
		break;
	case CMD_CRASH_REPORT: {
		const u8 *p = request;
		x.stats.crash_srr0 = so_get_u32(p + 0x08);
		x.stats.crash_dar = so_get_u32(p + 0x10);
		so_log("CRASH: exception %u at %08x (srr1 %08x), data address %08x, dsisr %08x, lr %08x, sp %08x",
			(unsigned)so_get_u32(p + 0x04), (unsigned)so_get_u32(p + 0x08), (unsigned)so_get_u32(p + 0x0C),
			(unsigned)so_get_u32(p + 0x10), (unsigned)so_get_u32(p + 0x14), (unsigned)so_get_u32(p + 0x18),
			(unsigned)so_get_u32(p + 0x1C));
		so_log("CRASH: stack %08x %08x %08x %08x %08x %08x %08x %08x", (unsigned)so_get_u32(p + 0x20),
			(unsigned)so_get_u32(p + 0x24), (unsigned)so_get_u32(p + 0x28), (unsigned)so_get_u32(p + 0x2C),
			(unsigned)so_get_u32(p + 0x30), (unsigned)so_get_u32(p + 0x34), (unsigned)so_get_u32(p + 0x38),
			(unsigned)so_get_u32(p + 0x3C));
		{
			int i;
			for (i = 0; i < 32; i += 8)
				so_log("CRASH: r%u-r%u %08x %08x %08x %08x %08x %08x %08x %08x", (unsigned)i, (unsigned)i + 7,
					(unsigned)so_get_u32(p + 0x40 + i * 4), (unsigned)so_get_u32(p + 0x44 + i * 4),
					(unsigned)so_get_u32(p + 0x48 + i * 4), (unsigned)so_get_u32(p + 0x4C + i * 4),
					(unsigned)so_get_u32(p + 0x50 + i * 4), (unsigned)so_get_u32(p + 0x54 + i * 4),
					(unsigned)so_get_u32(p + 0x58 + i * 4), (unsigned)so_get_u32(p + 0x5C + i * 4));
		}
		so_exi_report_last();
		post_mortem(p);
		so_logfile_crash();	/* (to the card: this boot's last lines and these; so_logfile.c) */
		so_client_flush_logs();
		break;
	}
	case CMD_TEXT_REPORT: {
		static u32 reports;
		const u8 *p = request;
		char a[49], b[65];
		u8 kind = p[1];
		int i;
		if (kind == 5) {
			/* tools/latency_probe (test builds): a button press, from the controller's
			 * reading (R1) to the screen. Times in us (the timebase: 60.75 ticks). */
			u32 who = so_get_u32(p + 0x08), vct = so_get_u32(p + 0x1C), n = so_get_u32(p + 0x20),
				how = so_get_u32(p + 0x24);
			so_log("LAT n=%u p=%u>%u b=%04x sc=%08x d=%u gap=%u copy=%u show=%u vct=%u/%u rf=%u rd=%u cp=%u",
				(unsigned)so_get_u32(p + 0x04), (unsigned)(who >> 24), (unsigned)(who >> 16 & 0xFF),
				(unsigned)(who & 0xFFFF), (unsigned)so_get_u32(p + 0x0C), (unsigned)(how >> 24),
				(unsigned)lat_us(so_get_u32(p + 0x10)), (unsigned)lat_us(so_get_u32(p + 0x14)),
				(unsigned)lat_us(so_get_u32(p + 0x18)), (unsigned)(vct >> 16), (unsigned)(vct & 0xFFFF),
				(unsigned)(how & 0xFF), (unsigned)(n >> 16), (unsigned)(n & 0xFFFF));
			so_log("LATx n=%u sr=%u poll=%08x logic=%u/%u ib=%02x%02x%02x%02x%02x%02x to=%u per=%u rp=%u/%u jp=%u",
				(unsigned)so_get_u32(p + 0x04), (unsigned)so_get_u32(p + 0x2C), (unsigned)so_get_u32(p + 0x28),
				(unsigned)so_get_u32(p + 0x30), (unsigned)so_get_u32(p + 0x34), p[0x38], p[0x39], p[0x3A],
				p[0x3B], p[0x3C], p[0x3D], (unsigned)so_get_u32(p + 0x40), (unsigned)lat_us(so_get_u32(p + 0x44)),
				(unsigned)so_get_u32(p + 0x4C), (unsigned)so_get_u32(p + 0x48), (unsigned)so_get_u32(p + 0x50));
			break;
		}
		memcpy(a, p + 0x10, 48);
		a[48] = '\0';
		memcpy(b, p + 0x40, 64);
		b[64] = '\0';
		for (i = 0; a[i]; i++)
			if (a[i] == '\n' || a[i] == '\r')
				a[i] = ' ';
		for (i = 0; b[i]; i++)
			if (b[i] == '\n' || b[i] == '\r')
				b[i] = ' ';
		if (kind == 1 || kind == 2) {
			u32 n;
			for (n = archive_count > 4 ? archive_count - 4 : 0; n < archive_count; n++)
				log_archive(n);
			x.stats.panics++;
			so_log("PANIC (%s) %s:%d from %08x: %s", kind == 1 ? "HSD_Panic" : "OSPanic", a,
				(int)so_get_u32(p + 0x04), (unsigned)so_get_u32(p + 0x08), b);
			so_logfile_crash();	/* (the game stops here too) */
		} else if (kind == 4) {
			so_log("PERF %s | %s", a, b);	/* tools/perf_probe (test builds) */
			if (!memcmp(a, "rb n=", 5) && x.game.reported)	/* (the engine has left the game) */
				reset_game();
			if (x.pf_game_over && !memcmp(a, "rb n=", 5)) {	/* (the engine's last word on a game) */
				x.pf_engine = 0;
				so_pf_open();
			}
			/* (tests, rollback.c RB_WALK: a free list of the game's is not what it should be; not the
			 * game's end line, "rb WALK 0") */
			if (!memcmp(a, "rb WALK ", 8) && a[8] != '0')
				so_exi_report_last();
			if (!memcmp(a, "rb MISS", 7))	/* (Slippi asked the engine for a frame it had not saved) */
				so_exi_report_last();
			if (!memcmp(a, "rb odb", 6)) {	/* (rb_load_check, its last line) */
				so_exi_report_last();
				so_np_force_disconnect(0);
			}
		} else if (reports++ < 60) {
			so_log("OSReport from %08x: %s | %s", (unsigned)so_get_u32(p + 0x08), a, b);
		}
		break;
	}
	case CMD_PROBE: {
		static u32 probes, last_us, bad;
		const u8 *p = request;
		if (p[1] == 2) {
			u32 i = archive_count++ % 4;
			strlcpy(archives[i].file, last_file, sizeof(archives[i].file));
			archives[i].data = so_get_u32(p + 0x08);
			archives[i].length = so_get_u32(p + 0x0C);
			archives[i].caller = so_get_u32(p + 0x10);
			archives[i].header[0] = so_get_u32(p + 0x18);
			archives[i].header[1] = so_get_u32(p + 0x1C);
			archives[i].header[2] = so_get_u32(p + 0x20);
			/* An archive starts with its own size: flag any that does not match. */
			if (archives[i].header[0] != archives[i].length)
				log_archive(archive_count - 1);
			break;
		}
		u32 us = so_get_u32(p + 0x14);
		s32 n_us = (s32)so_get_u32(p + 0x1C), n_jp = (s32)so_get_u32(p + 0x18);
		probes++;
		/* The first ones, every change of table, and the first bad ones. */
		if (probes <= 6 || us != last_us || ((n_us < 0 || n_jp < 0) && bad++ < 6))
			so_log("PROBE %u: toy(%d, %u) from %08x: jp %08x n%d [0]=%08x | us %08x n%d [0]=%08x [1]=%08x | r13 %08x",
				(unsigned)probes, (int)so_get_u32(p + 0x04), (unsigned)so_get_u32(p + 0x08),
				(unsigned)so_get_u32(p + 0x0C), (unsigned)so_get_u32(p + 0x10), (int)n_jp,
				(unsigned)so_get_u32(p + 0x28), (unsigned)us, (int)n_us, (unsigned)so_get_u32(p + 0x20),
				(unsigned)so_get_u32(p + 0x24), (unsigned)so_get_u32(p + 0x2C));
		last_us = us;
		break;
	}
	case CMD_HEARTBEAT: {
		/* tools/ppc/heartbeat.s: logged when the main thread moved, and
		 * every 16th. */
		static u32 last_main_pc;
		const u8 *p = request;
		u32 main_pc = so_get_u32(p + 0x20), tag = so_get_u32(p + 0x2C);
		if (tag >> 24 == 'D') {
			/* tools/ppc/stall.s: what the player sees */
			so_log("SHOWN logic %u: %u refreshes, %u with no new frame, %u frames jumped; so far %u predicted, %u fine, %u caught up",
				(unsigned)so_get_u32(p + 0x28), (unsigned)so_get_u32(p + 0x30), (unsigned)so_get_u32(p + 0x34),
				(unsigned)so_get_u32(p + 0x38), (unsigned)x.game.predicted, (unsigned)x.game.fine_advances, (unsigned)x.game.caught_up);
			break;
		}
		if (tag >> 24 == 'S') {
			/* tools/ppc/stall.s: the game loop overran a frame */
			so_log("STALL vi %u logic %u: %s %u retraces | cur %08x main st %u pc %08x lr %08x",
				(unsigned)so_get_u32(p + 0x04), (unsigned)so_get_u32(p + 0x28), (tag >> 16 & 0xFF) ? "ended after" : "for",
				(unsigned)(tag & 0xFFFF), (unsigned)so_get_u32(p + 0x18), (unsigned)(so_get_u32(p + 0x1C) >> 16),
				(unsigned)main_pc, (unsigned)so_get_u32(p + 0x24));
			so_log("STALL   at %08x lr %08x < %08x %08x %08x %08x %08x %08x %08x %08x; scene %08x", (unsigned)so_get_u32(p + 0x08),
				(unsigned)so_get_u32(p + 0x0C), (unsigned)so_get_u32(p + 0x40), (unsigned)so_get_u32(p + 0x44),
				(unsigned)so_get_u32(p + 0x48), (unsigned)so_get_u32(p + 0x4C), (unsigned)so_get_u32(p + 0x50),
				(unsigned)so_get_u32(p + 0x54), (unsigned)so_get_u32(p + 0x58), (unsigned)so_get_u32(p + 0x5C),
				(unsigned)so_get_u32(p + 0x14));
			break;
		}
		x.stats.heartbeats++;
		x.stats.heartbeat_pc = main_pc;
		if (main_pc != last_main_pc || x.stats.heartbeats % 16 == 1) {
			so_log("PPC vi %u scene %08x | main st %u pc %08x lr %08x < %08x %08x %08x %08x %08x %08x",
				(unsigned)so_get_u32(p + 0x04), (unsigned)so_get_u32(p + 0x14), (unsigned)(so_get_u32(p + 0x1C) >> 16),
				(unsigned)main_pc, (unsigned)so_get_u32(p + 0x24), (unsigned)so_get_u32(p + 0x40),
				(unsigned)so_get_u32(p + 0x44), (unsigned)so_get_u32(p + 0x48), (unsigned)so_get_u32(p + 0x4C),
				(unsigned)so_get_u32(p + 0x50), (unsigned)so_get_u32(p + 0x54));
			so_log("PPC   irq at %08x lr %08x < %08x %08x | current thread %08x", (unsigned)so_get_u32(p + 0x08),
				(unsigned)so_get_u32(p + 0x0C), (unsigned)so_get_u32(p + 0x30), (unsigned)so_get_u32(p + 0x34),
				(unsigned)so_get_u32(p + 0x18));
		}
		last_main_pc = main_pc;
		break;
	}
	case CMD_GET_DELAY:
		answer(2)[0] = 1;	/* a real answer: no extra delay offline */
		break;
	case CMD_PING_DISPLAY:	/* 0xFF: just the setting and the pings */
		answer_ping_display(request[1]);
		break;
	case CMD_SET_RUMBLE:	/* 0xFF: just the setting */
		answer(1)[0] = (u8)so_client_set_rumble(request[1]);
		break;
	case CMD_EARLY_PAD:
		on_early_pad(request);
		break;
	case CMD_ROLLBACK: {
		int want = rollback_wanted();
		if (request[2] == 3) {
			/* the engine, once a boot, at the online CSS: may it use the scratch? (ISO.c) */
			answer(1)[0] = (u8)(so_ppc_scratch_free != 0);
			so_log("exi: rollback engine v%u at boot: the scratch is %s%s", (unsigned)request[1],
				so_ppc_scratch_free ? "its own" : "not free (no rollback)",
				scratch_junked ? " (test: filled with junk first)" : "");
			/* (Slippi Dolphin's optional codes: what the codeset got, and the instructions at the four
			 * places they patch as the game has them now: 7c0802a6-like words are Melee's own, a
			 * branch 4xxxxxxx or 4e800020 a code's) */
			so_log("exi: optional codes: %u bytes in the codeset; in the game %08x (L-cancel flash), %08x (screen shake), "
				"%08x (2P HUD), %08x (Final Destination)", (unsigned)so_files_optional(), (unsigned)so_ppc_word(0x000C0148),
				(unsigned)so_ppc_word(0x00030E44), (unsigned)so_ppc_word(0x0016E9AC), (unsigned)so_ppc_word(0x0021AAE0));
			/* (the No Music switch: 38800000 and 4e800020 with it, Melee's own words without) */
			so_log("exi: music: in the game %08x (its volume), %08x (its stream)", (unsigned)so_ppc_word(0x00023FFC),
				(unsigned)so_ppc_word(0x0038ADD0));
			if (so_client_config()->test_logfile_dump)
				logfile_dump();
			/* (192 words, a bit a page; the first few: their first page, the word) */
			so_log("exi: the engine's page marks as this boot found them: %u words of 192 not 0 (%05x: %08x, %05x: %08x, %05x: %08x, %05x: %08x)",
				(unsigned)marks_found, (unsigned)marks_first[0], (unsigned)marks_first[1], (unsigned)marks_first[2],
				(unsigned)marks_first[3], (unsigned)marks_first[4], (unsigned)marks_first[5], (unsigned)marks_first[6],
				(unsigned)marks_first[7]);
			break;
		}
		answer(1)[0] = (u8)want;
		so_log("exi: rollback engine v%u says %u; rollback %s", (unsigned)request[1], (unsigned)request[2],
			want ? "on" : "off");
		/* The engine starts (0) at its game's first frame, then is ready (1)
		 * or not (2): before frame 2's poll (above), which takes it, but on
		 * either side of frame 1's (a slow stage load moves that one). */
		if (request[2] == 0) {
			so_pf_close();	/* (before the answer: the engine's scratch from now on) */
			x.pf_engine = 1;
			x.pf_game_over = 0;
			x.rb_ready = 0;
			/* (the engine has just written its end of the replay ring) */
			x.replay_tail = x.replay_told = so_ppc_word(ENGINE_REPLAY_HEAD);
			x.replay_on = 1;
			so_set_ppc_word(ENGINE_REPLAY_TAIL, x.replay_tail);
			so_set_ppc_word(ENGINE_ANSWER, 0);
		} else if (request[2] == 1 && want) {
			x.rb_ready = 1;
		} else if (request[2] == 2) {
			x.pf_engine = 0;	/* (it gave the scratch up before saying so) */
			x.rb_ready = 0;
			so_log("exi: the rollback engine (v%u) could not start; this game waits for inputs", (unsigned)request[1]);
		}
		break;
	}
	case CMD_SET_DELAY:	/* step 0: just the delay */
		answer(1)[0] = (u8)so_client_step_delay((s8)request[1]);
		break;
	case CMD_PREMADE_TEXT_LENGTH:
	case CMD_PREMADE_TEXT_LOAD:
		on_premade_text(request, cmd == CMD_PREMADE_TEXT_LOAD);
		break;
	case CMD_GET_RANK:
		answer(16)[1] = 1;	/* hidden, fetched, unranked */
		break;
	case CMD_REPORT_GAME:
		if (!x.game.reported)
			log_game(1);	/* the game is over */
		x.pf_game_over = 1;
		if (!x.pf_engine)
			so_pf_open();
		break;
	case CMD_LOGOUT:
	case CMD_UPDATE:
		/* The Online Play menu's Exit: Slippi's "Log-out" option under
		 * SlippWii's name (tools/ppc/online_menu.s, tools/menu_edits.py),
		 * sent once the player has said yes in Slippi's own box. And
		 * Slippi's "Update", the one option of a version Slippi has moved on
		 * from: a console is updated from SlippWii's start menu. Both leave
		 * for the Homebrew Channel (tools/ppc/exit.s). */
		so_diag_request_exit(cmd == CMD_UPDATE ? "the menu's Update" : "the menu's Exit");
		break;
	case CMD_OPEN_LOGIN:
	case CMD_OVERWRITE_SELECTIONS:
	case CMD_GP_COMPLETE_STEP:
	case CMD_REPORT_SET_COMPLETE:
	case CMD_REPORT_MATCH_STATUS_UPDATE:
	case CMD_PLAY_MUSIC:
	case CMD_STOP_MUSIC:
	case CMD_CHANGE_MUSIC_VOLUME:
	case CMD_FETCH_RANK:
	case CMD_GET_RANK_VISIBILITY:
		break;	/* nothing to do on a console (see the spec) */
	default:
		x.stats.unknown++;
		break;
	}
}

u32 so_exi_read(u8 *out, u32 len)
{
	u32 n;
	ct_note(0);
	x.stats.reads++;
	x.answer_read = 1;
	if (unstash_for(len))
		x.answer_read_main = 1;
	else if (x.stats.last_command != CMD_ONLINE_INPUTS && x.stats.last_command != CMD_EARLY_PAD)
		x.answer_read_main = 1;
	if (!x.has_resp)
		return 0;	/* Dolphin writes nothing then */
	if (x.file_pending) {
		/* Into the game's buffer, a piece at a time: these files reach 2 MB. */
		u64 start = so_time_us();
		n = so_files_read(x.file, out, len);
		memset(out + n, 0, len - n);
		/* An archive starts with its own length: a cheap check of the read. */
		x.stats.file_loads++;
		file_busy_until_us = 0;	/* (the data is the last of a load) */
		if (x.game.rb && !after_load.want) {
			after_load.want = AFTER_LOAD_POLLS;
			after_load.n = 0;
			after_load.ev_from = so_ppc_word(ENGINE_EVENTS);
		}
		so_log("exi: loaded %s at %p: %u of %u bytes (read %u) in %u ms, header %08x", x.file, out, (unsigned)n,
			(unsigned)x.resp_len, (unsigned)len, (unsigned)((so_time_us() - start) / 1000),
			(unsigned)(n >= 4 ? so_get_u32(out) : 0));
		return len;
	}
	n = x.resp_len < len ? x.resp_len : len;
	memcpy(out, x.resp, n);
	memset(out + n, 0, len - n);
	if (x.stats.last_command == CMD_GCT_LOAD)
		so_log("exi: codeset at %p (read %u)", out, (unsigned)len);
	return len;
}

void so_exi_stats(SoExiStats *out)
{
	*out = x.stats;
}
