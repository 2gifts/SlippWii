/* slippi_online/so_exi.c
 * Slippi Dolphin's online EXI device (see so_exi.h). Function names in
 * comments refer to CEXISlippi in Slippi Dolphin's EXI_DeviceSlippi.cpp,
 * which this follows; byte layouts are in docs/EXI_DEVICE_SPEC.md (repository root).
 */
#define SO_FILE_ID "exi"
#include "so_exi.h"
#include "so_alloc.h"
#include "so_client.h"
#include "so_files.h"
#include "so_netplay.h"
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
	CMD_TEXT_REPORT = 0xDA,	/* Melee's panics and OSReport (tools/ppc/text_report.s) */
	CMD_SET_DELAY = 0xDC,	/* Wii only: [step] -> [delay], the online CSS's delay buttons (tools/css_delay/) */
};

/* ONLINE_INPUTS results (the ASM's RESP_*). */
enum { RESP_NORMAL = 1, RESP_SKIP = 2, RESP_DISCONNECTED = 3 };

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
	case CMD_CRASH_REPORT:
	case CMD_PROBE: return 0x3F;
	case CMD_HEARTBEAT: return 0x5F;
	case CMD_SET_DELAY: return 1;
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
		(cmd >= CMD_LOG_MESSAGE && cmd <= CMD_CHANGE_MUSIC_VOLUME) ||
		(cmd >= CMD_PREMADE_TEXT_LENGTH && cmd <= CMD_GET_RANK_VISIBILITY) || cmd == CMD_CRASH_REPORT || cmd == CMD_HEARTBEAT ||
		cmd == CMD_PROBE || cmd == CMD_TEXT_REPORT || cmd == CMD_SET_DELAY;
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
	int dummies_sent;	/* the delay frames' empty pads, this game */
	u64 stall_since_us;
	/* This game's netcode, logged when it ends: frames played, polls
	 * answered SKIP (each one a whole frame the game waited), frames that
	 * waited at all, the longest wait for one frame, and when it started. */
	/* What the last 0xB0 sends once the game has read its answer. */
	struct {
		int valid, has_pad, delay_pads;
		u8 delay;
		SoLocalPad pad;
	} pend;
	int answer_read;	/* the game read the last answer */

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
		u32 max_answer_us, slow_answers, max_lock_us;
	} game;
	int initialised;

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

/* ---- 0xB0 ONLINE_INPUTS (handleOnlineInputs, lockstep) -------------------- */

/* The opponent's answer block: frame numbers, checksums and up to 7 pads per
 * remote, newest first (prepareOpponentInputs). The client lock is held. */
static void fill_remote_inputs(u8 *r, s32 frame, int remotes)
{
	s32 nums[3], smallest;
	int i, k;
	r[1] = (u8)remotes;
	for (i = 0; i < 3; i++) {
		nums[i] = frame;
		if (i < remotes) {
			s32 head = so_np_remote_head_frame(i), cf;
			u32 cs;
			so_np_remote_checksum(i, &cf, &cs);
			so_put_u32(r + 0x02 + i * 8, (u32)cf);
			so_put_u32(r + 0x06 + i * 8, cs);
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
			so_np_remote_pad(i, nums[i] - k, pads + k * PAD_FULL_SIZE);	/* 8 bytes; the rest stays 0 */
	}
}

/* One line per game: how much the lockstep waited for the opponent. */
static void log_game(void)
{
	u64 us;
	u32 expected;
	if (x.game.frames < 300) {	/* the results screen's polls, not a game */
		memset(&x.game, 0, sizeof(x.game));
		return;
	}
	us = so_time_us() - x.game.start_us;
	expected = (u32)(us / SO_FRAME_US);
	so_client_lock();
	so_log("game over: %u frames in %u ms (%u at 59.94 fps); waited %u frames (%u frames waited, longest %u), ping %u us",
		(unsigned)x.game.frames, (unsigned)(us / 1000), (unsigned)expected, (unsigned)x.game.skips,
		(unsigned)x.game.held, (unsigned)x.game.longest, (unsigned)so_np_take_avg_ping_us());
	so_log("game pacing: %u polls over 20 ms apart (longest %u us); answers: longest %u us, %u over 1 ms, lock wait up to %u us",
		(unsigned)x.game.slow_polls, (unsigned)x.game.max_gap_us, (unsigned)x.game.max_answer_us,
		(unsigned)x.game.slow_answers, (unsigned)x.game.max_lock_us);
	so_log("game traffic: pads waited up to %u us for the network thread; %u replay transfers",
		(unsigned)so_client_take_pad_latency_us(), (unsigned)x.game.replay_writes);
	if (x.game.waits_logged) {
		char line[150];
		int len = so_snprintf(line, sizeof(line), "game waits (frame:ms):"), i;
		for (i = 0; i < (int)x.game.waits_logged; i++)
			len += so_snprintf(line + len, sizeof(line) - len, " %u:%u", (unsigned)x.game.wait_frame[i],
				(unsigned)x.game.wait_ms[i]);
		so_log("%s", line);
	}
	so_client_unlock();
	memset(&x.game, 0, sizeof(x.game));
}

static void on_online_inputs(const u8 *p)
{
	s32 frame = (s32)so_get_u32(p + 1), finalized = (s32)so_get_u32(p + 5);
	u32 checksum = so_get_u32(p + 9);
	u8 delay = p[13];
	u8 *r = answer(RXB_SIZE);
	int remotes, i, ready = 1;

	u64 t_in = so_time_us(), t_lock;
	x.stats.b0_polls++;
	if (x.game.last_poll_us && frame > 1) {
		u32 gap = (u32)(t_in - x.game.last_poll_us);
		if (gap > 20000)
			x.game.slow_polls++;
		if (gap > x.game.max_gap_us)
			x.game.max_gap_us = gap;
	}
	x.game.last_poll_us = t_in;
	if (!r)
		return;
	if (!client_up()) {
		r[0] = RESP_DISCONNECTED;
		return;
	}
	/* Game start, once per game: lockstep may poll frame 1 several times. */
	if (frame == 1 && x.last_frame != 1) {
		log_game();	/* a game that ended without a report */
		so_client_lock();
		so_np_start_game();
		so_client_unlock();
		reset_selections(&x.local_sel);
		x.dummies_sent = 0;
		x.stall_since_us = 0;
		x.game.start_us = so_time_us();
	}
	x.last_frame = frame;

	so_client_lock();
	t_lock = so_time_us() - t_in;
	if (t_lock > x.game.max_lock_us)
		x.game.max_lock_us = (u32)t_lock;
	if (so_np_status() != SO_NP_CONNECTED) {
		so_client_unlock();
		r[0] = RESP_DISCONNECTED;
		return;
	}
	remotes = so_np_remote_count();
	for (i = 0; i < remotes; i++)
		if (so_np_remote_head_frame(i) < frame)
			ready = 0;

	if (frame == 1 && !x.dummies_sent) {
		/* sent with this frame's (after the answer), so the opponent need not wait */
		x.pend.delay_pads = 1;
		x.pend.delay = delay;
		x.pend.valid = 1;
		x.dummies_sent = 1;
	}

	if (!ready) {
		/* Only resend what is not acked: the game polls this frame again. */
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
		x.pend.valid = 1;
		r[0] = RESP_NORMAL;
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
	fill_remote_inputs(r, frame, remotes);
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
	history_loaded = 1;
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
	if (!history_dirty)
		return;
	history_dirty = 0;
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

/* handleNameEntryLoad: the first code from `index` on (or back, scrolling
 * newer) that starts with the typed characters. */
static void on_code_suggestion(const u8 *p)
{
	u8 *r = answer(30);
	u8 len = p[0x19], dir = p[0x1E];
	u32 index = so_get_u32(p + 0x1A), start;
	int count = code_list(), found = -1, i, k;
	if (len > 8)
		len = 8;
	if (dir == 1)
		index++;
	else if (dir == 2)
		index--;
	else if (dir == 3)
		index = 0;
	for (start = index; found < 0 && start < (u32)count;) {
		int ok = history[start].len / 2 >= len;
		for (k = 0; ok && k < len; k++)
			ok = history[start].code[2 * k] == p[1 + 3 * k] && history[start].code[2 * k + 1] == p[2 + 3 * k];
		if (ok)
			found = (int)start;
		else if (dir == 2)
			start--;	/* unsigned: below 0 ends the walk */
		else
			start++;
	}
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
		for (f = 1; f <= x.pend.delay; f++) {
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

void so_exi_write(const u8 *request, u32 dma_len)
{
	static u8 seen[32];
	u8 cmd = request[0];
	init_once();
	if (x.pend.valid)
		send_pending();	/* the game wrote again without reading the answer */
	x.answer_read = 0;
	x.stats.commands++;
	x.stats.last_command = cmd;
	if (!(seen[cmd >> 3] & (1 << (cmd & 7)))) {
		seen[cmd >> 3] |= (u8)(1 << (cmd & 7));
		so_log("exi: first 0x%02X (%u bytes)", cmd, (unsigned)dma_len);
	}
	/* A command without an answer leaves none behind (Dolphin's read queue
	 * keeps the old one, but nothing reads it). */
	x.has_resp = 0;
	x.file_pending = 0;
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
		log_game();	/* the game is over */
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
	x.stats.reads++;
	x.answer_read = 1;
	if (!x.has_resp)
		return 0;	/* Dolphin writes nothing then */
	if (x.file_pending) {
		/* Into the game's buffer, a piece at a time: these files reach 2 MB. */
		u64 start = so_time_us();
		n = so_files_read(x.file, out, len);
		memset(out + n, 0, len - n);
		/* An archive starts with its own length: a cheap check of the read. */
		x.stats.file_loads++;
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
