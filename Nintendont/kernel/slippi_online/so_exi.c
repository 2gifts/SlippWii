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
#include "so_netplay.h"
#include "so_prefetch.h"
#include "so_socket.h"
#include "so_platform.h"
#include "so_selftest.h"	/* so_encode_connect_code */
#include "so_text.h"
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

#define RXB_SIZE 297
#define MSRB_SIZE 962
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
	case CMD_ONLINE_INPUTS: return 25;
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
	case CMD_EARLY_PAD: return 25;
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
	u8 match_block[MATCH_BLOCK_SIZE];
	u8 local_idx, remote_idx;	/* m_local_player_idx / m_remote_player_idx */
	int have_client;	/* Dolphin holds slippi_netplay */
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
		/* Rollback (the engine said ready): Dolphin's time sync, the
		 * frames answered with predictions, the stalls at the rollback
		 * limit, the time sync's skipped and advanced frames. */
		int rb;
		s32 seen[3];	/* the opponents' latest frame the game was given */
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
	} game;
	int initialised;
	int drive_runs;	/* the drive's runs since the last game still to log, a line a poll */
	u8 poll_delay;	/* the last 0xB0's delay (diagnostics: a change is logged) */
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
	x.sent_chat = 0;
	x.last_frame = 0;	/* the next frame 1 starts a game */
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
static void fill_remote_inputs(u8 *r, s32 frame, s32 finalized, const s32 *heads, int remotes)
{
	s32 nums[3], smallest;
	int i, k;
	r[1] = (u8)remotes;
	for (i = 0; i < 3; i++) {
		nums[i] = frame;
		if (i < remotes) {
			s32 head = heads[i], cf;
			u32 cs;
			so_np_remote_checksum(i, &cf, &cs);
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
		for (k = 0; k < ROLLBACK_MAX_FRAMES; k++)
			if (!x.game.rb || k == 0 || nums[i] - k >= finalized)	/* Dolphin's holds no older ones */
				so_np_remote_pad(i, nums[i] - k, pads + k * PAD_FULL_SIZE);	/* 8 bytes; the rest stays 0 */
	}
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

static int rollback_wanted(void)
{
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
	if (x.game.waits_logged) {
		char line[150];
		int len = so_snprintf(line, sizeof(line), "game waits (frame:ms):"), i;
		for (i = 0; i < (int)x.game.waits_logged; i++)
			len += so_snprintf(line + len, sizeof(line) - len, " %u:%u", (unsigned)x.game.wait_frame[i],
				(unsigned)x.game.wait_ms[i]);
		so_log("%s", line);
	}
	so_client_unlock();
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
				so_log("exi: checksum differs at frame %d: ours %08x, the opponent's %08x", (int)cf,
					(unsigned)ours, (unsigned)cs);
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

static void fine_sync(s32 offset, int countdown)
{
	so_set_ppc_word(ENGINE_BG_SAVES, bg_saves_wanted());
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
static int within_rollback_limit(s32 frame, s32 finalized, s32 head)
{
	s32 latest = head < finalized + ROLLBACK_MAX_FRAMES - 1 ? head : finalized + ROLLBACK_MAX_FRAMES - 1;
	return latest >= frame - ROLLBACK_MAX_FRAMES;
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
	u32 ms;
	u8 answer;
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

/* MEM2 outlives a reboot, and after a power cut holds anything: what the engine reads
 * from the kernel starts at 0, and the diagnostics this session reports are its own.
 * ISO.c, once it has given the scratch to the engine: before the game runs at all. */
void so_exi_scratch_boot(void)
{
	static const u32 zero_words[] = {
		ENGINE_SLOW_FRAME, ENGINE_INPUT_WAIT, ENGINE_POLL_EARLY, ENGINE_BG_SAVES, ENGINE_VI_NUDGE,
		ENGINE_REMOTE_HEAD, ENGINE_REMOTE_CHANGED, ENGINE_EVENTS,
		0x1159EA00u,	/* T_BG_RING's count */
	};
	u32 a;
	if (!so_ppc_scratch_free)
		return;
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
#define ENGINE_REPLAY_HEAD 0x1159F300
#define ENGINE_REPLAY_TAIL 0x1159F320
#define ENGINE_ANSWER 0x1159F340
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

int so_exi_file_busy(void)
{
	if (!file_busy_until_us)
		return 0;
	if (so_time_us() < file_busy_until_us)
		return 1;
	file_busy_until_us = 0;
	return 0;
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
		if (gap > 20000)
			x.game.slow_polls++;
		if (gap > x.game.max_gap_us)
			x.game.max_gap_us = gap;
		catch_up_note(frame, gap);
		after_load_note(frame, gap);
		if (x.game.rb && frame > 120) {
			u32 off = gap > 16667 ? gap - 16667 : 16667 - gap;
			x.game.spacing[off < 500 ? 0 : off < 1000 ? 1 : off < 2000 ? 2 : off < 4000 ? 3 : off < 8000 ? 4 : 5]++;
		}
	}
	x.game.last_poll_us = t_poll;
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
		so_set_ppc_word(ENGINE_SLOW_FRAME, so_client_config()->test_slow_frame_ms);
		so_set_ppc_word(ENGINE_POLL_EARLY, so_client_config()->rollback_poll_early_us |
			(u32)so_client_config()->rollback_poll_deep_us << 16);
		so_set_ppc_word(ENGINE_BG_SAVES, bg_saves_wanted());
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
		so_log("exi: this game waits for inputs (rollback %u, scratch %u)",
			(unsigned)so_client_config()->rollback, (unsigned)so_ppc_scratch_free);
	}

	so_client_lock();
	t_lock = so_time_us() - t_in;
	if (t_lock > x.game.max_lock_us)
		x.game.max_lock_us = (u32)t_lock;
	if (so_np_status() != SO_NP_CONNECTED) {
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
	/* tests: a lag spike (test_spike_ms) every 300 frames, the first at
	 * frame 80, so its rollbacks cross the end of the countdown (GO),
	 * timed, as the game's frames stop while it waits */
	if (x.game.rb && so_client_config()->test_spike_ms && frame % 300 == 80 && frame != x.game.spike_frame) {
		x.game.spike_frame = frame;
		x.game.spike_until_us = t_in + so_client_config()->test_spike_ms * 1000u;
		for (i = 0; i < remotes; i++)
			x.game.spike_head[i] = x.game.seen[i];
	}
	x.game.last_finalized = finalized;
	x.game.last_checksum = checksum;
	if (so_client_config()->test_stuck_frame && frame == so_client_config()->test_stuck_frame)
		so_socket_test_stuck = 1;
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
		} else if (now - x.stall_since_us > STALL_LIMIT_US) {
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
			if (!sent_early || memcmp(x.game.early_pad, p + 14, SO_PAD_SIZE)) {
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
	switch (so_client_state()) {
	case SO_CLIENT_SEARCHING: return MM_MATCHMAKING;
	case SO_CLIENT_CONNECTING: return MM_OPPONENT_CONNECTING;
	case SO_CLIENT_CONNECTED:
	case SO_CLIENT_DISCONNECTED: return MM_CONNECTION_SUCCESS;
	case SO_CLIENT_ERROR: return MM_ERROR;
	default: return MM_IDLE;
	}
}

static u16 shade_key(const u8 *b, int slot)
{
	u8 c = b[0x60 + slot * 0x24];
	return (u16)(((c == 0x13 ? 0x12 : c) << 8) | b[0x63 + slot * 0x24]);
}

/* Writes the per-game fields into the match block once both players have
 * picked. The client lock is held. */
static u32 build_game(int remotes, u8 *alt_out)
{
	const SoSelections *by_port[SO_REMOTE_MAX + 1] = { NULL };
	const SoSelections *lps = so_np_local_selections();
	u16 stage = 0x1F;
	u8 alt = 0, *b = x.match_block;
	u32 rng;
	int i, j;

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

	for (i = 0; i <= remotes; i++) {
		const SoSelections *s = by_port[i];
		if (!s || !s->is_character_selected)
			continue;
		b[0x60 + i * 0x24] = s->character_id;
		b[0x63 + i * 0x24] = s->character_color;
		b[0x67 + i * 0x24] = 0;
		b[0x69 + i * 0x24] = 0;	/* team: Teams only */
	}
	/* Shades: each human slot gets the number of earlier slots with the same
	 * character (Sheik counting as Zelda) and colour. */
	for (i = 0; i < 4; i++) {
		u8 shade = 0;
		if (b[0x61 + i * 0x24] != 0)
			continue;
		for (j = 0; j < i; j++)
			if (b[0x61 + j * 0x24] == 0 && shade_key(b, j) == shade_key(b, i))
				shade++;
		b[0x67 + i * 0x24] = shade;
	}
	b[0x08] = 0;	/* not Teams */
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
			if (chat_id && chat_id != SO_CHAT_DISABLED && !so_client_config()->chat) {
				so_np_send_chat(SO_CHAT_DISABLED);
				chat_id = 0;
			}
		}
		if (remotes == 1)
			chat_player = sent ? x.local_idx : x.remote_idx;
		if (local_ready && remote_ready)
			rng = build_game(remotes, &alt);
		so_client_unlock();
		break;
	}

	r[0x000] = state;
	r[0x001] = local_ready;
	r[0x002] = remote_ready;
	r[0x003] = x.local_idx;
	r[0x004] = x.remote_idx;

	/* Always: mode, timer and items. */
	x.match_block[0x00] = 0x32;	/* stock mode, timer counting down */
	x.match_block[0x03] = 0x4C;	/* score UI hidden */
	so_put_u32(x.match_block + 0x10, 8 * 60);
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
	r[0x009] = so_client_config()->delay;
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
			for (i = 0; i < 4 && i < m->player_count; i++)
				if (i != x.local_idx && x.match_block[0x61 + i * 0x24] == 0 && m->players[i].display_name[0])
					names[n++] = m->players[i].display_name;
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
	if (client_up())
		so_client_error(error, sizeof(error));
	so_game_string(error, 120, r + 0x165);
	memcpy(r + 0x256, x.match_block, MATCH_BLOCK_SIZE);
	r[0x3C1] = alt;
}

/* ---- the other commands --------------------------------------------------- */

static void remember_code(const u8 *code, int len);
static void load_history(void);
static volatile int history_loaded;

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
	if (search.mode == 2) {	/* ONLINE_MODE_DIRECT */
		if (!history_loaded)
			load_history();
		remember_code(search.connect_code, len);
	}
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
	r[0] = 1;
	so_game_string(user->display_name, 15, r + 0x01);
	so_game_connect_code(user->connect_code, r + 0x20);
}

static void on_send_chat(const u8 *p)
{
	if (!x.have_client || !so_client_config()->chat)
		return;
	so_client_lock();
	if (so_np_status() == SO_NP_CONNECTED) {
		x.sent_chat = p[1];
		so_np_send_chat(p[1]);
	}
	so_client_unlock();
}

/* The direct-code history behind connect-code autocomplete (0xBE), most
 * recent first: the codes searched in Direct this session (raw Shift-JIS, 2
 * bytes a character, as 0xB4 sends them), then selftest_code. Slippi Dolphin
 * keeps it in direct-codes.json; this one lasts until the console is off. */
#define HISTORY_MAX 8
static struct {
	u8 code[16];
	u8 len;	/* bytes */
} history[HISTORY_MAX + 1];
static int history_count;
static volatile int history_dirty, history_loaded;

/* SO_DIRECT_CODES_PATH: "SLDC", a count, then per code its length in bytes
 * and 16 bytes of Shift-JIS, most recent first. */
#define SO_DIRECT_CODES_PATH "/slippwii/direct-codes.bin"
#define HISTORY_FILE_SIZE (5 + HISTORY_MAX * 17)

static void load_history(void)
{
	u8 buf[HISTORY_FILE_SIZE];
	int n, i;
	const SoConfig *c = so_client_config();
	history_loaded = 1;
	if (c && c->test_control)
		return;	/* tests keep it to the session: not the player's codes */
	n = so_read_file(SO_DIRECT_CODES_PATH, buf, sizeof(buf));
	if (n < 5 || memcmp(buf, "SLDC", 4) != 0)
		return;
	for (i = 0; i < buf[4] && i < HISTORY_MAX && 5 + (i + 1) * 17 <= n; i++) {
		const u8 *e = buf + 5 + i * 17;
		if (e[0] == 0 || e[0] > 16 || e[0] % 2)
			break;
		history[i].len = e[0];
		memcpy(history[i].code, e + 1, 16);
		history_count = i + 1;
	}
	so_log("exi: %d direct codes remembered", history_count);
}

void so_exi_save_history(void)
{
	u8 buf[HISTORY_FILE_SIZE];
	int i, count;
	const SoConfig *c = so_client_config();
	if (!history_dirty)
		return;
	history_dirty = 0;
	if (c && c->test_control)
		return;
	count = history_count;
	memset(buf, 0, sizeof(buf));
	memcpy(buf, "SLDC", 4);
	buf[4] = (u8)count;
	for (i = 0; i < count; i++) {
		buf[5 + i * 17] = history[i].len;
		memcpy(buf + 6 + i * 17, history[i].code, 16);
	}
	if (so_write_file(SO_DIRECT_CODES_PATH, buf, 5 + count * 17) < 0)
		so_log("exi: could not save the direct codes");
}

static void remember_code(const u8 *code, int len)
{
	int i, j;
	if (len <= 0 || len > 16 || len % 2)
		return;
	for (i = 0; i < history_count; i++)
		if (history[i].len == len && memcmp(history[i].code, code, len) == 0)
			break;
	if (i == history_count && history_count < HISTORY_MAX)
		history_count++;
	if (i == history_count)
		i = history_count - 1;	/* full: drop the oldest */
	for (j = i; j > 0; j--)
		history[j] = history[j - 1];
	memset(history[0].code, 0, sizeof(history[0].code));
	memcpy(history[0].code, code, len);
	history[0].len = (u8)len;
	history_dirty = 1;
}

/* The history with selftest_code last (unless already in it); returns the count. */
static int code_list(void)
{
	if (!history_loaded)
		load_history();
	const SoConfig *c = so_client_config();
	u8 code[16];
	int n = c && c->selftest_code[0] ? so_encode_connect_code(c->selftest_code, code, sizeof(code)) : -1, i;
	for (i = 0; n > 0 && i < history_count; i++)
		if (history[i].len == n && memcmp(history[i].code, code, n) == 0)
			n = -1;
	if (n > 0) {
		memcpy(history[history_count].code, code, n);
		history[history_count].len = (u8)n;
		return history_count + 1;
	}
	return history_count;
}

/* Does this code start with the typed characters? */
static int code_matches(int index, const u8 *p, u8 len)
{
	int k, ok = history[index].len / 2 >= len;
	for (k = 0; ok && k < len; k++)
		ok = history[index].code[2 * k] == p[1 + 3 * k] && history[index].code[2 * k + 1] == p[2 + 3 * k];
	return ok;
}

/* handleNameEntryLoad. `dir` from the shown code (index): none (0), a step
 * older (1), newer (2, never past the newest) or back to the newest
 * (3), the first code that starts with the typed characters, walking the
 * same way. None found: the shown one stays if it still matches (else
 * scrolling past the end would clear it), or the typed text is echoed. */
static void on_code_suggestion(const u8 *p)
{
	u8 *r = answer(30);
	u8 len = p[0x19], dir = p[0x1E];
	u32 shown = so_get_u32(p + 0x1A), index = shown;
	int count = code_list(), found = -1, i;
	if (len > 8)
		len = 8;
	if (dir == 1)
		index++;
	else if (dir == 2 && index > 0)
		index--;
	else if (dir == 3)
		index = 0;
	while (found < 0 && index < (u32)count) {
		if (code_matches((int)index, p, len))
			found = (int)index;
		else
			index += dir == 2 ? (u32)-1 : 1;	/* unsigned: below 0 ends the walk */
	}
	if (found < 0 && shown < (u32)count && code_matches((int)shown, p, len))
		found = (int)shown;
	if (found < 0) {
		/* not found: the typed text, echoed */
		memcpy(r + 1, p + 1, 3 * len);
		r[0x19] = len;
		memcpy(r + 0x1A, p + 0x1A, 4);
		return;
	}
	r[0] = 1;
	for (i = 0; i < history[found].len / 2 && i < 8; i++) {
		r[1 + 3 * i] = history[found].code[2 * i];
		r[2 + 3 * i] = history[found].code[2 * i + 1];
	}
	r[0x19] = (u8)(history[found].len / 2);
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
		break;
	}
	case CMD_TEXT_REPORT: {
		static u32 reports;
		const u8 *p = request;
		char a[49], b[65];
		u8 kind = p[1];
		int i;
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
		} else if (kind == 4) {
			so_log("PERF %s | %s", a, b);	/* tools/perf_probe (test builds) */
			if (!memcmp(a, "rb n=", 5) && x.game.reported)	/* (the engine has left the game) */
				reset_game();
			if (x.pf_game_over && !memcmp(a, "rb n=", 5)) {	/* (the engine's last word on a game) */
				x.pf_engine = 0;
				so_pf_open();
			}
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
			so_log("STALL   at %08x lr %08x < %08x %08x %08x %08x %08x %08x %08x %08x", (unsigned)so_get_u32(p + 0x08),
				(unsigned)so_get_u32(p + 0x0C), (unsigned)so_get_u32(p + 0x40), (unsigned)so_get_u32(p + 0x44),
				(unsigned)so_get_u32(p + 0x48), (unsigned)so_get_u32(p + 0x4C), (unsigned)so_get_u32(p + 0x50),
				(unsigned)so_get_u32(p + 0x54), (unsigned)so_get_u32(p + 0x58), (unsigned)so_get_u32(p + 0x5C));
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
	case CMD_OPEN_LOGIN:
	case CMD_LOGOUT:
	case CMD_UPDATE:
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
