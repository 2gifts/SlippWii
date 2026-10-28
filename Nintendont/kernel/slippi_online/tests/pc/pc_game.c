/* PC "fake Melee": drives the EXI device (so_exi) with the commands Slippi's
 * online codeset sends, at the console's 59.94 Hz, against the local test
 * tools: it logs in (0xB9), locks in and searches Slippi Direct (0xB5, 0xB4),
 * waits on the CSS (0xB3 every frame) until both players are ready, then
 * plays a lockstep game through 0xB0 and checks every opponent pad, then
 * leaves (0xBA). The opponent code is selftest_code in DIR's config.ini.
 *
 *   so_pc_game DIR FRAMES [TIMEOUT_S] [--any-pads]
 *
 * The fake Dolphin peer sends the self-test's pad pattern (so_test_pad), and
 * this sends it too, so both sides can check what they receive. --any-pads
 * skips that check (against so_pc_opponent, which plays scripted inputs).
 * SO_PC_VI_US in the environment sets the frame time (a real Wii runs Melee
 * at about 16667 us, a hair faster than 59.94 Hz).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "so_client.h"
#include "so_exi.h"
#include "so_platform.h"
#include "so_selftest.h"
#include "so_text.h"
#include "so_util.h"
#include "pc.h"

char __slippi_online_stack_addr, __slippi_online_stack_size;
char __slippi_online_rx_stack_addr, __slippi_online_rx_stack_size;
char __slippi_online_done_stack_addr, __slippi_online_done_stack_size;
char __slippi_online_test_stack_addr, __slippi_online_test_stack_size;
char __slippi_online_diag_stack_addr, __slippi_online_diag_stack_size;

#define MSRB_SIZE 962
#define RXB_SIZE 297
#define BLOCK 0x256	/* the game info block in the MSRB */
#define FOX 0x02

static u8 msrb[MSRB_SIZE], rxb[RXB_SIZE];
static int failures;
static u64 next_vi_us;
static u32 vi_us = SO_FRAME_US;
static const char *data_dir;
static int check_pads = 1;

#define CHECK(cond, ...)                                  \
	do {                                              \
		if (!(cond)) {                            \
			printf("game: FAIL: " __VA_ARGS__); \
			printf("\n");                     \
			failures++;                       \
		}                                         \
	} while (0)

/* Waits for the next vertical interrupt (the game polls once per frame). */
static void wait_vi(void)
{
	u64 now = so_time_us();
	if (!next_vi_us || now > next_vi_us + 100000)
		next_vi_us = now;
	if (next_vi_us > now)
		so_sleep_us((u32)(next_vi_us - now));
	next_vi_us += vi_us;
}

/* One write and its read, acknowledged as EXI.c does it. */
static void command(const u8 *req, u32 len, u8 *out, u32 out_len)
{
	so_exi_write(req, len);
	so_exi_after_ack();
	if (out) {
		so_exi_read(out, out_len);
		so_exi_after_ack();
	}
}

static void simple(u8 cmd, u8 *out, u32 out_len)
{
	command(&cmd, 1, out, out_len);
}

static void match_state(void)
{
	simple(0xB3, msrb, MSRB_SIZE);
}

/* A game string as ASCII (full-width punctuation shown as '?'). */
static void game_text(const u8 *s, int n, char *out)
{
	int i = 0, o = 0;
	while (i < 2 * n + 1 && s[i]) {
		if (s[i] >= 0x80) {
			out[o++] = '?';
			i += 2;
		} else {
			out[o++] = (char)s[i++];
		}
	}
	out[o] = '\0';
}

static const char *state_name(u8 s)
{
	static const char *const names[] = { "idle", "initializing", "matchmaking", "opponent connecting",
		"connected", "error" };
	return s < 6 ? names[s] : "?";
}

static int online_status(void)
{
	u8 osb[42], want[31], step[2], got;
	int delay = so_client_config()->delay;
	simple(0xB9, osb, sizeof(osb));
	so_game_string(so_client_user()->display_name, 15, want);
	CHECK(osb[0] == 1, "0xB9: not logged in (%u)", osb[0]);
	CHECK(memcmp(osb + 1, want, 31) == 0, "0xB9: display name");
	/* The CSS's delay buttons (tools/css_delay/, 0xDC): read, +, - */
	step[0] = 0xDC;
	step[1] = 0;
	command(step, 2, &got, 1);
	CHECK(got == delay, "0xDC 0: delay %u, not %d", got, delay);
	step[1] = 1;
	command(step, 2, &got, 1);
	CHECK(got == (delay < 9 ? delay + 1 : 9), "0xDC +1: delay %u", got);
	step[1] = 0xFF;	/* -1 */
	command(step, 2, &got, 1);
	CHECK(so_client_config()->delay == delay && got == delay, "0xDC -1: the delay did not come back (%u)", got);
	return osb[0] == 1 ? 0 : -1;
}

/* The menus' side commands, as a CSS session issues them. */
static void side_commands(void)
{
	u8 req[0x41], out[400];
	u32 len;
	int n;

	simple(0xBC, out, 4);	/* new seed: any value */
	simple(0xD5, out, 2);
	CHECK(out[0] == 1, "0xD5: delay answer %u", out[0]);
	simple(0xE3, out, 16);
	CHECK(out[1] == 1, "0xE3: rank not 'fetched'");

	/* Premade text: the logout dialog's "Are You Sure?". */
	req[0] = 0xE1;
	req[1] = SO_TEXT_LOGOUT;
	req[2] = 0;
	command(req, 3, out, 4);
	len = so_get_u32(out);
	CHECK(len == 35, "0xE1: logout text is %u bytes, not 35", (unsigned)len);
	req[0] = 0xE2;
	command(req, 3, out, len);
	n = so_premade_text(SO_TEXT_LOGOUT, 0, "", req, sizeof(req));
	CHECK(n == (int)len && memcmp(out, req, n) == 0, "0xE2: logout text");

	/* A file the device does not replace: length 0. */
	memset(req, 0, sizeof(req));
	req[0] = 0xD1;
	strcpy((char *)req + 1, "MnSlChr.usd");
	command(req, 0x41, out, 4);
	CHECK(so_get_u32(out) == 0, "0xD1: unexpected replacement file");
}

/* Reads DIR/rel whole; returns its length, or 0. */
static u32 load(const char *rel, u8 **data)
{
	char path[400];
	FILE *f;
	long n;
	snprintf(path, sizeof(path), "%s/%s", data_dir, rel);
	*data = NULL;
	if (!(f = fopen(path, "rb")))
		return 0;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	*data = malloc(n + 32);
	n = (long)fread(*data, 1, n, f);
	fclose(f);
	return (u32)n;
}

/* The codeset as served for `load`: the file's bytes, except that each C2
 * hook's last word is now a branch back to the instruction after the hook
 * (decoded here independently of so_gct_link). */
static void check_codeset(const u8 *got, const u8 *file, u32 len, u32 load)
{
	u32 off = 8, hooks = 0, i;
	u8 *expect = malloc(len);
	memcpy(expect, file, len);
	while (off + 8 <= len) {
		u32 word = so_get_u32(file + off), n = so_get_u32(file + off + 4), type = (word >> 24) & 0xFE;
		if ((word & 0xF0FFFFFF) == 0xF0000000 && n == 0)
			break;
		if (type == 0xC2) {
			u32 site = load + off + n * 8 + 4, insn = so_get_u32(got + off + n * 8 + 4);
			s32 disp = (s32)((insn & 0x03FFFFFC) << 6) >> 6;
			CHECK((insn & 0xFC000003) == 0x48000000 && site + (u32)disp == (0x80000000u | (word & 0x01FFFFFF)) + 4,
				"0xD4: hook at %08x does not branch back (%08x)", (unsigned)(0x80000000u | (word & 0x01FFFFFF)),
				(unsigned)insn);
			memcpy(expect + off + n * 8 + 4, got + off + n * 8 + 4, 4);
			hooks++;
			off += 8 + n * 8;
		} else {
			off += type == 0xC0 ? 8 + n * 8 : type == 0x06 ? 8 + ((n + 7) & ~7u) : type == 0x08 ? 16 : 8;
		}
	}
	for (i = 0; i < len && got[i] == expect[i]; i++)
		;
	CHECK(i == len, "0xD4: codeset byte %u differs", (unsigned)i);
	printf("game: codeset: %u hooks branch back correctly for a load at %08x\n", (unsigned)hooks, (unsigned)load);
	free(expect);
}

/* Slippi's bootloader (0xD3/0xD4) and a menu file load (0xD1/0xD2), when the
 * builder's files are in DIR. */
static void boot_and_files(void)
{
	u8 *gct, *file, *buf, req[0x41];
	u32 gct_len = load("netplay.gct", &gct), file_len = load("files/MnMaAll.usd", &file), len, padded;
	if (!gct_len || !file_len) {
		printf("game: no netplay.gct / files in %s; codeset and file loads not tested\n", data_dir);
		free(gct);
		free(file);
		return;
	}
	/* The bootloader: the length, then the codeset into a 32-byte-rounded buffer. */
	req[0] = 0xD3;
	command(req, 1, (u8 *)&len, 4);
	len = so_get_u32((u8 *)&len);
	CHECK(len == gct_len, "0xD3: codeset length %u, want %u", (unsigned)len, (unsigned)gct_len);
	padded = (len + 31) & ~31u;
	buf = malloc(padded);
	req[0] = 0xD4;
	so_put_u32(req + 1, 0x80400000);
	command(req, 5, buf, padded);
	check_codeset(buf, gct, gct_len, 0x80400000);
	free(buf);

	/* A menu file: its length, then the file into the game's buffer. */
	memset(req, 0, sizeof(req));
	req[0] = 0xD1;
	strcpy((char *)req + 1, "MnMaAll.usd");
	command(req, 13, (u8 *)&len, 4);
	len = so_get_u32((u8 *)&len);
	CHECK(len == file_len, "0xD1: MnMaAll.usd is %u bytes, want %u", (unsigned)len, (unsigned)file_len);
	buf = malloc(file_len);
	req[0] = 0xD2;
	command(req, 13, buf, file_len);
	CHECK(memcmp(buf, file, file_len) == 0, "0xD2: MnMaAll.usd bytes differ");
	printf("game: codeset %u bytes and MnMaAll.usd %u bytes served intact\n", (unsigned)gct_len,
		(unsigned)file_len);
	free(buf);
	free(gct);
	free(file);
}

/* CSS lock-in and search (FN_LOCK_IN_AND_SEARCH: 0xB5 then 0xB4). */
static int lock_in_and_search(const char *code)
{
	u8 pstb[10] = { 0xB5, 0, FOX, 0, 1, 0, 0, 3 /* SB_RAND */, 2 /* Direct */, 0 };
	u8 fmtb[20] = { 0xB4, 2 };
	int n = so_encode_connect_code(code, fmtb + 2, 18);
	if (n <= 0) {
		printf("game: selftest_code '%s' is not a connect code\n", code);
		return -1;
	}
	command(pstb, sizeof(pstb), NULL, 0);
	command(fmtb, sizeof(fmtb), NULL, 0);
	match_state();
	CHECK(msrb[0] >= 1, "0xB3 right after 0xB4: state %u, want >= 1", msrb[0]);
	return 0;
}

/* The CSS: 0xB3 every frame until both players are ready. */
static int wait_ready(u32 timeout_ms)
{
	u32 start = so_time_ms();
	u8 last = 0xFF;
	int chatted = 0;
	for (;;) {
		wait_vi();
		match_state();
		if (msrb[0] != last) {
			printf("game: CSS state %s\n", state_name(msrb[0]));
			last = msrb[0];
		}
		if (msrb[0] == 5) {
			char error[256];
			game_text(msrb + 0x165, 120, error);
			printf("game: matchmaking error: %s\n", error);
			return -1;
		}
		if (msrb[0] == 4 && !chatted) {
			/* Quick chat: "ggs" shows once, as ours. */
			u8 cmtb[2] = { 0xBB, 0x88 };
			command(cmtb, 2, NULL, 0);
			match_state();
			CHECK(msrb[0x0A] == 0x88, "0xB3 after 0xBB: sent chat id 0x%02x", msrb[0x0A]);
			CHECK(msrb[0x0C] == msrb[0x03], "0xB3 after 0xBB: chat player %u, local %u", msrb[0x0C], msrb[0x03]);
			match_state();
			CHECK(msrb[0x0A] == 0, "0xB3: the sent chat id did not clear");
			chatted = 1;
		}
		if (msrb[0] == 4 && msrb[1] && msrb[2])
			return 0;
		if (so_time_ms() - start > timeout_ms) {
			printf("game: not ready after %u s (state %s, ready %u/%u)\n", (unsigned)(timeout_ms / 1000),
				state_name(msrb[0]), msrb[1], msrb[2]);
			return -1;
		}
	}
}

static void check_match(void)
{
	const u8 *b = msrb + BLOCK;
	char name[64], opp[64];
	u8 local = msrb[3], remote = msrb[4];
	u16 stage = so_get_u16(b + 0x0E);
	game_text(msrb + 0x0F, 15, name);
	game_text(msrb + 0x0AA, 15, opp);
	printf("game: ready: we are port %u (%s), opponent port %u (%s); stage 0x%02x, rng 0x%08x, delay %u\n",
		local + 1, name, remote + 1, opp, stage, (unsigned)so_get_u32(msrb + 5), msrb[9]);
	CHECK(local != remote && local < 2 && remote < 2, "ports %u/%u", local, remote);
	CHECK(b[0x60 + local * 0x24] == FOX, "our character in the block is 0x%02x", b[0x60 + local * 0x24]);
	CHECK(b[0x61 + 2 * 0x24] == 3 && b[0x61 + 3 * 0x24] == 3, "P3/P4 present in a 1v1");
	CHECK(stage != 0, "no stage");
	CHECK((b[0x02] & 0x08) == 0, "pause not allowed in Direct");
	CHECK(b[0x00] == 0x32 && b[0x03] == 0x4C && so_get_u32(b + 0x10) == 480, "mode/timer bytes");
	CHECK(msrb[9] >= 1 && msrb[9] <= 9, "delay %u", msrb[9]);
	CHECK(opp[0] != '\0', "no opponent name");
	CHECK(msrb[0x38E] != 0, "no match id");
}

typedef struct {
	u32 frames, skips, longest_skip_run, mismatches;
	u64 start_us, end_us;
} GameResult;

/* The game: one 0xB0 per frame; SKIP means the frame did not advance. */
static int play(int frames, GameResult *g)
{
	const u8 delay = msrb[9];
	u8 local_np, remote_np, txb[26], want[8];
	static const u8 zero[8];
	s32 frame = 1;
	u32 run = 0;

	so_client_lock();
	local_np = so_np_local_player_idx();
	remote_np = so_np_remote_player_idx(0);
	so_client_unlock();
	memset(g, 0, sizeof(*g));
	g->start_us = so_time_us();
	while (frame <= frames) {
		wait_vi();
		memset(txb, 0, sizeof(txb));
		txb[0] = 0xB0;
		so_put_u32(txb + 1, (u32)frame);
		so_put_u32(txb + 5, (u32)(frame - 1));
		so_put_u32(txb + 9, so_test_checksum(frame - 1));
		txb[13] = delay;
		/* The input for this frame, which the opponent uses at frame + delay. */
		so_test_pad(local_np, frame + delay, txb + 14);
		command(txb, sizeof(txb), rxb, RXB_SIZE);

		if (rxb[0] == 3) {
			printf("game: disconnected at frame %d\n", (int)frame);
			return -1;
		}
		CHECK(rxb[1] == 1, "0xB0: remote count %u", rxb[1]);
		if (rxb[0] == 2) {
			g->skips++;
			if (++run > g->longest_skip_run)
				g->longest_skip_run = run;
			continue;
		}
		run = 0;
		CHECK(rxb[0] == 1, "0xB0: result %u", rxb[0]);
		CHECK((s32)so_get_u32(rxb + 0x1A) == frame, "0xB0 frame %d: opponent frame %d", (int)frame,
			(int)so_get_u32(rxb + 0x1A));
		so_test_pad(remote_np, frame, want);
		if (check_pads && memcmp(rxb + 0x2A, want, 8) != 0 && !(frame <= 15 && memcmp(rxb + 0x2A, zero, 8) == 0)) {
			if (g->mismatches++ < 5)
				printf("game: frame %d: opponent pad is wrong\n", (int)frame);
		}
		/* The pad before it, one slot on. */
		if (check_pads && frame > 15) {
			so_test_pad(remote_np, frame - 1, want);
			CHECK(memcmp(rxb + 0x2A + 12, want, 8) == 0, "frame %d: previous pad", (int)frame);
		}
		g->frames++;
		frame++;
	}
	g->end_us = so_time_us();
	return 0;
}

int main(int argc, char **argv)
{
	GameResult g;
	SoExiStats stats;
	int frames, res;
	u32 timeout_ms;
	double secs;

	if (argc < 3) {
		fprintf(stderr, "usage: so_pc_game DIR FRAMES [TIMEOUT_S]\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	if (!strcmp(argv[argc - 1], "--any-pads")) {
		check_pads = 0;
		argc--;
	}
	frames = atoi(argv[2]);
	timeout_ms = (u32)(argc > 3 ? atoi(argv[3]) : 120) * 1000;
	so_pc_set_data_dir(argv[1]);
	data_dir = argv[1];
	if (getenv("SO_PC_VI_US"))
		vi_us = (u32)atoi(getenv("SO_PC_VI_US"));
	res = so_client_init();
	if (res < 0) {
		printf("so_client_init failed: %d\n", res);
		return 1;
	}
	if (so_client_config()->selftest) {
		printf("selftest=1 is set in %s/config.ini; the game test needs it off\n", argv[1]);
		return 1;
	}

	/* Start-up: the bootloader fetches the codeset; the menus load files. */
	boot_and_files();

	/* The main menu, then the online submenu. */
	simple(0xBA, NULL, 0);	/* every main-menu load */
	if (online_status() < 0)
		return 1;
	side_commands();
	match_state();
	CHECK(msrb[0] == 0, "0xB3 before searching: state %u", msrb[0]);
	{
		/* SlippWii refuses Ranked (the menu hides it; this is the backstop) */
		u8 ranked[20] = { 0xB4, 0 /* Ranked */ };
		command(ranked, sizeof(ranked), NULL, 0);
		match_state();
		CHECK(msrb[0] == 5, "0xB4 Ranked: state %s, want error", state_name(msrb[0]));
		simple(0xBA, NULL, 0);
		match_state();
		CHECK(msrb[0] == 0, "0xBA after the Ranked refusal: state %s", state_name(msrb[0]));
	}

	if (lock_in_and_search(so_client_config()->selftest_code) < 0 || wait_ready(timeout_ms) < 0) {
		simple(0xBA, NULL, 0);
		return 1;
	}
	check_match();
	/* The scene change into the game asks twice in one frame. */
	match_state();
	match_state();

	res = play(frames, &g);
	so_exi_stats(&stats);
	secs = (double)((g.end_us ? g.end_us : so_time_us()) - g.start_us) / 1e6;
	printf("game: %u frames in %.2f s (%.2f fps), %u skipped polls (longest run %u), %u wrong pads\n",
		(unsigned)g.frames, secs, secs > 0 ? g.frames / secs : 0.0, (unsigned)g.skips,
		(unsigned)g.longest_skip_run, (unsigned)g.mismatches);
	printf("game: exi: %u commands, %u reads, %u B0 polls, %u savestate requests, %u unknown\n",
		(unsigned)stats.commands, (unsigned)stats.reads, (unsigned)stats.b0_polls,
		(unsigned)stats.savestate_requests, (unsigned)stats.unknown);
	CHECK(res == 0, "the game did not finish");
	CHECK(g.mismatches == 0, "%u wrong pads", (unsigned)g.mismatches);
	CHECK(stats.savestate_requests == 0 && stats.unknown == 0, "unexpected commands");

	/* Results screen, then back to the CSS: the opponent stays. */
	so_sleep_us(2000000);	/* let the last pads be acked */
	match_state();
	CHECK(msrb[0] == 4, "after the game: state %s", state_name(msrb[0]));
	CHECK(so_get_u16(msrb + BLOCK + 0x0E) != 0, "the last stage was not kept");
	simple(0xBA, NULL, 0);
	match_state();
	CHECK(msrb[0] == 0, "0xB3 right after 0xBA: state %s", state_name(msrb[0]));
	so_sleep_us(500000);

	printf("game: %s\n", failures ? "FAIL" : "PASS");
	return failures ? 1 : 0;
}
