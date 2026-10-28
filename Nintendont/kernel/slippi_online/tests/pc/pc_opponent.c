/* PC opponent for playing the Wii by hand on a local test setup: it searches
 * Slippi Direct for selftest_code (in DIR's config.ini) through the fake
 * matchmaking server, locks in Fox (and picks Final Destination when it is
 * the decider), and plays with simple scripted inputs, paced as Slippi
 * Dolphin paces itself (time sync, rollback limit): it walks back and forth
 * and jumps now and then. Its pads carry no checksum (frame
 * 0), so the game never compares them with its own: this opponent does not
 * simulate the game. After each game it locks in again, for the next one.
 *
 *   so_pc_opponent DIR [GAMES] [TIMEOUT_S]
 *
 * Not a Slippi Dolphin stand-in for netcode tests (that is the 3DS fork's
 * slippi_fake_peer); this exists so a person can see the Wii play online.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "so_client.h"
#include "so_platform.h"
#include "so_selftest.h"
#include "so_util.h"
#include "pc.h"

char __slippi_online_stack_addr, __slippi_online_stack_size;
char __slippi_online_rx_stack_addr, __slippi_online_rx_stack_size;
char __slippi_online_done_stack_addr, __slippi_online_done_stack_size;
char __slippi_online_test_stack_addr, __slippi_online_test_stack_size;
char __slippi_online_diag_stack_addr, __slippi_online_diag_stack_size;

#define FOX 0x02
#define FINAL_DESTINATION 0x20
#define GAME_OVER_US 3000000	/* no pad from the Wii this long: its game ended */
#define FIRST_FRAME_US 60000000
#define ROLLBACK_MAX 7	/* Slippi's ROLLBACK_MAX_FRAMES */

static s64 min_s64(s64 a, s64 b)
{
	return a < b ? a : b;
}

/* Walk right for 2 s, then left for 2 s; jump (X) once a second. */
static void scripted_pad(s32 frame, u8 out[8])
{
	memset(out, 0, 8);
	out[2] = (u8)(s8)((frame / 120) % 2 ? -100 : 100);	/* stick X */
	if (frame % 60 == 30)
		out[0] = 0x04;	/* X */
}

static int lock_in(int first_game)
{
	SoSelections sel;
	memset(&sel, 0, sizeof(sel));
	sel.character_id = FOX;
	sel.is_character_selected = 1;
	sel.rng_offset = so_random() % 0xFFFF;
	so_client_lock();
	if (first_game && so_np_is_decider()) {
		sel.stage_id = FINAL_DESTINATION;
		sel.is_stage_selected = 1;
	}
	so_np_set_selections(&sel);
	so_client_unlock();
	printf("opponent: locked in Fox\n");
	return 0;
}

/* Waits until the Wii has locked in too. */
static int wait_remote_ready(u32 timeout_ms)
{
	u32 start = so_time_ms();
	for (;;) {
		int ready;
		so_client_lock();
		ready = so_np_remote_selections(0)->is_character_selected;
		so_client_unlock();
		if (ready)
			return 0;
		if (so_client_state() != SO_CLIENT_CONNECTED || so_time_ms() - start > timeout_ms)
			return -1;
		so_sleep_us(20000);
	}
}

/* One game; returns the Wii's last frame, or < 0 on disconnect. */
static int play_game(void)
{
	const u8 delay = so_client_config()->delay;
	u64 next_us, waited_from, resent_at;
	s32 frame, speed_ppm = 0, min_offset = 0, max_offset = 0, last_head = 0;
	int advance = 0, advances = 0, stalls = 0;
	u8 pad[8];

	so_client_lock();
	so_np_start_game();
	so_client_unlock();
	next_us = so_time_us();
	for (frame = 1;; frame++) {
		SoLocalPad lp;
		s32 f;
		u64 now = so_time_us();
		/* Sleep to a millisecond short of the frame, then spin: Windows
		 * sleeps overshoot, and a late frame here is a held frame on the
		 * Wii (lockstep). */
		while ((now = so_time_us()) + 1500 < next_us)
			so_sleep_us(1000);
		while (so_time_us() < next_us)
			;
		now = so_time_us();
		if (now > next_us + 5000)
			printf("opponent: frame %d late by %u us\n", (int)frame, (unsigned)(now - next_us));
		/* Time sync as Slippi Dolphin does it (shouldAdvanceOnlineFrame,
		 * docs/EXI_DEVICE_SPEC.md): every 30 frames, from the measured time
		 * offset (positive: we are ahead), run up to 1% faster when behind
		 * by over 250 us, up to 0.5% slower when ahead by over 8 ms (both
		 * in full at 3 frames), and past frame 120 advance up to 3 extra
		 * frames (one every 5) when behind by over 26.7 ms. A console runs
		 * at about 60.0 fps, a hair above 59.94. */
		if (frame % 30 == 0) {
			s32 offset;
			so_client_lock();
			offset = so_np_time_offset_us();
			so_client_unlock();
			if (offset < -250)
				speed_ppm = (s32)min_s64((s64)-offset * 10000 / (3 * SO_FRAME_US), 10000);
			else if (offset > 8000)
				speed_ppm = -(s32)min_s64((s64)offset * 5000 / (3 * SO_FRAME_US), 5000);
			else
				speed_ppm = 0;
			if (offset < -26683 && !advance && frame > 120) {
				advance = (int)min_s64((-offset - 10000) / SO_FRAME_US + 1, 3);
				advances += advance;
			}
			if (offset < min_offset)
				min_offset = offset;
			if (offset > max_offset)
				max_offset = offset;
			if (frame % 600 == 0)
				printf("opponent: frame %d, ping %u us, offset %d us, speed %+d ppm\n", (int)frame,
					(unsigned)so_np_ping_us(0), (int)offset, (int)speed_ppm);
		}
		if (advance && frame % 5 == 0)
			advance--;	/* an extra frame: no wait */
		else
			next_us += SO_FRAME_US - (s64)SO_FRAME_US * speed_ppm / 1000000;

		so_client_lock();
		for (f = frame == 1 ? 1 : frame + delay; f <= frame + delay; f++) {
			memset(&lp, 0, sizeof(lp));
			lp.frame = f;	/* checksum frame 0: never compared */
			if (f > delay)
				scripted_pad(f, lp.pad);
			so_np_send_pad(&lp);
		}
		so_client_unlock();

		/* Dolphin rolls back instead of waiting: it only stops when the
		 * Wii's newest pad is more than 7 frames old (the rollback limit),
		 * resending what is not acked once per frame meanwhile. The first
		 * pad comes after the VS screen; a long stop mid-game means the
		 * game ended. */
		waited_from = resent_at = so_time_us();
		for (;;) {
			s32 head;
			u64 now_us = so_time_us();
			so_client_lock();
			head = so_np_remote_head_frame(0);
			if (head > last_head)
				last_head = head;
			if (head >= frame - (frame == 1 ? 0 : ROLLBACK_MAX))
				so_np_remote_pad(0, head < frame ? head : frame, pad);
			else if (now_us - resent_at >= SO_FRAME_US) {
				so_np_send_pad(NULL);
				resent_at = now_us;
			}
			so_client_unlock();
			if (head >= frame - (frame == 1 ? 0 : ROLLBACK_MAX))
				break;
			if (so_client_state() != SO_CLIENT_CONNECTED)
				return -1;
			if (now_us - waited_from > (frame == 1 ? FIRST_FRAME_US : GAME_OVER_US))
				goto over;
			so_sleep_us(1000);
		}
		if (frame == 1 || so_time_us() - waited_from > 2000) {
			if (frame > 1)
				stalls++;
			next_us = so_time_us() + SO_FRAME_US;	/* do not race to catch up */
		}
	}
over:
	printf("opponent: time offset %d..%d us, %d frames advanced, %d rollback-limit stalls\n",
		(int)min_offset, (int)max_offset, advances, stalls);
	/* (not the queue's head now: the Wii's next selections clear it) */
	return last_head;
}

int main(int argc, char **argv)
{
	SoSearch search;
	int games, game, n;
	u32 timeout_ms, start;

	if (argc < 2) {
		fprintf(stderr, "usage: so_pc_opponent DIR [GAMES] [TIMEOUT_S]\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	/* Behave like a PC running Slippi Dolphin well: on time every frame. */
	SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
	games = argc > 2 ? atoi(argv[2]) : 3;
	timeout_ms = (u32)(argc > 3 ? atoi(argv[3]) : 900) * 1000;
	so_pc_set_data_dir(argv[1]);
	if (so_client_init() < 0) {
		printf("opponent: no account in %s\n", argv[1]);
		return 1;
	}
	memset(&search, 0, sizeof(search));
	search.mode = SO_MODE_DIRECT;
	n = so_encode_connect_code(so_client_config()->selftest_code, search.connect_code, sizeof(search.connect_code));
	if (n <= 0) {
		printf("opponent: selftest_code is not a connect code\n");
		return 1;
	}
	search.connect_code_len = (u8)n;
	printf("opponent: waiting for %s in Slippi Direct\n", so_client_config()->selftest_code);
	so_client_find_match(&search);
	start = so_time_ms();
	while (so_client_state() != SO_CLIENT_CONNECTED) {
		SoClientState s = so_client_state();
		if (s == SO_CLIENT_ERROR || so_time_ms() - start > timeout_ms) {
			char error[128];
			so_client_error(error, sizeof(error));
			printf("opponent: no match (%s %s)\n", so_client_state_name(s), error);
			return 1;
		}
		/* A failed search starts again, as the Wii's player may take a while. */
		if (s == SO_CLIENT_IDLE)
			so_client_find_match(&search);
		so_sleep_us(100000);
	}
	printf("opponent: connected to %s\n", so_client_match()->players[so_client_match()->local_player_index ? 0 : 1].display_name);

	for (game = 0; game < games; game++) {
		lock_in(game == 0);
		if (wait_remote_ready(timeout_ms) < 0) {
			printf("opponent: the Wii left (%s)\n", so_client_state_name(so_client_state()));
			break;
		}
		printf("opponent: game %d starting\n", game + 1);
		n = play_game();
		if (n < 0) {
			printf("opponent: disconnected\n");
			break;
		}
		printf("opponent: game %d over after %d frames\n", game + 1, n);
	}
	so_client_cancel();
	so_sleep_us(500000);
	return 0;
}
