/* slippi_online/so_selftest.c
 * Hardware self-test (see so_selftest.h). The frame loop is a lockstep peer
 * in miniature: each frame it sends our pad for frame + delay, then waits
 * until the opponent's pad for the frame is here, as the online EXI device
 * will for the game.
 */
#define SO_FILE_ID "selftest"
#include "so_selftest.h"
#include "so_alloc.h"
#include "so_client.h"
#include "so_platform.h"
#include "so_socket.h"
#include "so_util.h"
#include "string.h"

extern char __slippi_online_test_stack_addr, __slippi_online_test_stack_size;

#define TEST_THREAD_PRIORITY 0x70	/* below the network thread */
#define CONNECT_TIMEOUT_MS 120000
#define SELECTIONS_TIMEOUT_MS 30000
#define STALL_LIMIT_US 7000000	/* Dolphin gives up on a peer after ~7 s */
#define RESEND_US SO_FRAME_US
#define TEST_CHARACTER 2	/* Fox */
#define SYNC_TOLERANCE_US 10000
#define PROBES 20
#define PROBE_TIMEOUT_US 500000
/* Between stages, so a hang shows which one (selftest_stage_pause_s). */
#define STAGE_PAUSE_US ((u32)test_config.selftest_stage_pause_s * 1000000)

static SoConfig test_config;
static volatile int test_result = -1;

int so_selftest_result(void)
{
	return test_result;
}

void so_test_pad(int player_idx, s32 frame, u8 out[8])
{
	u32 x = (u32)frame * 2654435761u ^ (u32)(player_idx + 1) * 0x9E3779B9u;
	out[0] = (u8)((x >> 3) & 0x1F);	/* buttons: ---SYXBA */
	out[1] = (u8)((x >> 9) & 0x7F);	/* buttons: -LRZUDRL */
	out[2] = (u8)(s8)((int)(frame % 160) - 80);	/* stick X */
	out[3] = (u8)(s8)(player_idx ? 60 : -60);	/* stick Y */
	out[4] = (u8)(x >> 17);
	out[5] = (u8)(x >> 25);
	out[6] = (u8)frame;	/* L analog: frame, low byte */
	out[7] = (u8)(frame >> 8);	/* R analog: frame, high byte */
}

u32 so_test_checksum(s32 frame)
{
	return (u32)frame * 0x01000193u ^ 0xC0FFEEu;
}

int so_encode_connect_code(const char *code, u8 *out, int size)
{
	int n = 0;
	for (; *code; code++) {
		u16 sj;
		char ch = *code;
		if (ch >= 'a' && ch <= 'z')
			ch = (char)(ch - 'a' + 'A');
		if (ch >= 'A' && ch <= 'Z')
			sj = (u16)(0x8260 + (ch - 'A'));
		else if (ch >= '0' && ch <= '9')
			sj = (u16)(0x824F + (ch - '0'));
		else if (ch == '#')
			sj = 0x8194;
		else
			return -1;
		if (n + 2 > size)
			return -1;
		out[n++] = (u8)(sj >> 8);
		out[n++] = (u8)sj;
	}
	return n;
}

static int wait_for_state(SoClientState want, u32 timeout_ms)
{
	u32 start = so_time_ms();
	SoClientState last = so_client_state();
	while (so_time_ms() - start < timeout_ms) {
		SoClientState s = so_client_state();
		if (s == want)
			return 0;
		if (s == SO_CLIENT_ERROR || s == SO_CLIENT_IDLE || s == SO_CLIENT_OFF)
			return -1;
		if (s != last)
			so_log("selftest: %s", so_client_state_name(s));
		last = s;
		SO_TRACE(SO_THREAD_TEST);
		so_sleep_us(50000);
	}
	return -2;
}

typedef struct {
	u32 frames, waits, longest_wait_us, mismatches, skipped_frames;
	u32 ping_max_us;
	u64 ping_sum_us;
	u32 ping_samples;
} Results;

/* Runs the synthetic match; returns 0 on success. */
static int run_frames(int frames, Results *r)
{
	const u8 delay = test_config.delay;
	const u8 remote_player = so_np_remote_player_idx(0);
	u64 next_us, start_us, stall_us = 0;
	s32 frame;
	u8 local_player, pad[8], want[8];
	static const u8 zero[8];

	so_client_lock();
	local_player = so_np_local_player_idx();
	so_np_start_game();
	so_client_unlock();

	start_us = next_us = so_time_us();
	for (frame = 1; frame <= frames; frame++) {
		SoLocalPad lp;
		u64 waited_from, resent_at;
		s32 f;
		u64 now = so_time_us();
		if (next_us > now + 500)
			so_sleep_us((u32)(next_us - now));
		next_us += SO_FRAME_US;

		/* Our pad for frame + delay (and zero pads for the delay frames at
		 * the start). The checksum describes the last frame both played. */
		so_client_lock();
		for (f = frame == 1 ? 1 : frame + delay; f <= frame + delay; f++) {
			memset(&lp, 0, sizeof(lp));
			lp.frame = f;
			lp.checksum_frame = frame - 1;
			lp.checksum = so_test_checksum(frame - 1);
			if (f > delay)
				so_test_pad(local_player, f, lp.pad);
			so_np_send_pad(&lp);
		}
		so_client_unlock();

		/* Lockstep: wait for the opponent's pad for this frame, resending
		 * what is not acked once per frame of waiting (each IOS send costs
		 * ~10 ms, so resending faster only floods the send queue). */
		waited_from = so_time_us();
		resent_at = waited_from;
		for (;;) {
			int got;
			u64 now_us = so_time_us();
			so_client_lock();
			got = so_np_remote_pad(0, frame, pad) == 0;
			if (!got && now_us - resent_at >= RESEND_US) {
				so_np_send_pad(NULL);
				resent_at = now_us;
			}
			so_client_unlock();
			if (got)
				break;
			if (so_client_state() != SO_CLIENT_CONNECTED) {
				so_log("selftest: connection lost at frame %d", (int)frame);
				return -1;
			}
			if (so_time_us() - waited_from > STALL_LIMIT_US) {
				so_log("selftest: no pad from the opponent for frame %d in 7 s", (int)frame);
				return -2;
			}
			so_sleep_us(1000);
		}
		stall_us = so_time_us() - waited_from;
		if (stall_us > 2000) {
			r->waits++;
			/* Frames we waited for do not count against the schedule. */
			next_us += stall_us;
		}
		if (stall_us > r->longest_wait_us)
			r->longest_wait_us = (u32)stall_us;

		so_test_pad(remote_player, frame, want);
		if (memcmp(pad, want, 8) != 0 && !(frame <= 15 && memcmp(pad, zero, 8) == 0)) {
			if (r->mismatches++ < 5)
				so_log("selftest: frame %d: pad %02x%02x%02x%02x%02x%02x%02x%02x is wrong", (int)frame,
					pad[0], pad[1], pad[2], pad[3], pad[4], pad[5], pad[6], pad[7]);
		}
		r->frames++;

		/* Time sync, as Dolphin does every 30 frames: when we run more than
		 * 10 ms ahead, hold back for the difference (up to 5 frames). */
		if (frame % 30 == 0) {
			s32 offset;
			u32 ping;
			so_client_lock();
			offset = so_np_time_offset_us();
			ping = so_np_ping_us(0);
			so_client_unlock();
			if (ping > r->ping_max_us)
				r->ping_max_us = ping;
			r->ping_sum_us += ping;
			r->ping_samples++;
			if (offset > SYNC_TOLERANCE_US) {
				u32 skip = (u32)(offset - SYNC_TOLERANCE_US) / SO_FRAME_US + 1;
				if (skip > 5)
					skip = 5;
				r->skipped_frames += skip;
				next_us += (u64)skip * SO_FRAME_US;
			}
		}
		if (frame % 600 == 0)
			so_log("selftest: frame %d, %u waits, ping %u us", (int)frame, (unsigned)r->waits,
				(unsigned)so_np_ping_us(0));
	}
	{
		u32 ms = (u32)((so_time_us() - start_us) / 1000);
		so_log("selftest: %u frames in %u.%03u s (%u waits, longest %u us, %u frames held for time sync)",
			(unsigned)r->frames, (unsigned)(ms / 1000), (unsigned)(ms % 1000), (unsigned)r->waits,
			(unsigned)r->longest_wait_us, (unsigned)r->skipped_frames);
	}
	return 0;
}

static void report(const Results *r, int ok)
{
	SoNpStats np;
	SoSocketStats sock;
	SoAllocStats mem;
	so_client_lock();
	so_np_stats(&np);
	so_client_unlock();
	so_socket_stats(&sock);
	so_alloc_stats(&mem);
	so_log("selftest: ping avg %u us, max %u us; pad mismatches %u",
		(unsigned)(r->ping_samples ? r->ping_sum_us / r->ping_samples : 0), (unsigned)r->ping_max_us,
		(unsigned)r->mismatches);
	so_log("selftest: pads tx %u rx %u (stale %u, bad %u), acks tx %u rx %u, resends %u, test-dropped %u",
		(unsigned)np.pad_packets_sent, (unsigned)np.pad_packets_received, (unsigned)np.pad_packets_stale,
		(unsigned)np.pad_packets_bad, (unsigned)np.acks_sent, (unsigned)np.acks_received,
		(unsigned)np.resends, (unsigned)np.test_dropped);
	so_log("selftest: socket sent %u (dropped %u, errors %u), received %u (dropped %u, errors %u)",
		(unsigned)sock.sent, (unsigned)sock.send_dropped, (unsigned)sock.send_errors, (unsigned)sock.received,
		(unsigned)sock.receive_dropped, (unsigned)sock.receive_errors);
	so_log("selftest: IOS sendto avg %u us max %u us, send queue max %u; arena peak %u bytes",
		(unsigned)(sock.sent ? sock.send_us_sum / sock.sent : 0), (unsigned)sock.send_us_max,
		(unsigned)sock.send_queue_max, (unsigned)mem.peak);
	so_log("selftest: %s", ok ? "PASS" : "FAIL");
	so_set_status("SO selftest %s", ok ? "PASS" : "FAIL");
	SO_PHASE(ok ? "PASS" : "FAIL");
	test_result = ok;
}

/* Asks the PC tool for the test plan: config.ini lines that override the
 * settings for this run, so a test changes without touching the SD card. */
static void fetch_plan(void)
{
	static char reply[1024];
	u64 sent = so_time_us();
	SO_PHASE("plan");
	so_socket_send(test_config.log_host, test_config.log_port, "SLPWII PLAN?", 12);
	while (so_time_us() - sent < 2000000) {
		u32 addr;
		u16 from;
		int got = so_socket_receive(&addr, &from, reply, sizeof(reply) - 1, NULL);
		if (got <= 0) {
			so_sleep_us(5000);
			continue;
		}
		if (got >= 12 && memcmp(reply, "SLPWII PLAN\n", 12) == 0) {
			so_client_apply_settings(reply + 12, (u32)(got - 12));
			test_config = *so_client_config();
			so_log("selftest: plan from the PC applied (%d bytes)", got - 12);
			return;
		}
	}
	so_log("selftest: no plan from the PC; using config.ini");
}

/* Fills the free heap in 16 KB blocks, announcing each address first, then
 * verifies and frees them: a hang names the address that kills it. */
static int memory_walk(void)
{
	static char where[24];
	enum { CHUNK = 16 * 1024, MAX_CHUNKS = 40 };
	void *chunks[MAX_CHUNKS];
	int n = 0, i, bad = 0;
	while (n < MAX_CHUNKS) {
		u32 *w, *end;
		void *p = so_malloc(CHUNK);
		if (!p)
			break;
		so_snprintf(where, sizeof(where), "mem %08x", (unsigned)(size_t)p);
		SO_PHASE(where);
		so_sleep_us(test_config.diag_step_pause_ms * 1000);
		end = (u32 *)((u8 *)p + CHUNK);
		for (w = (u32 *)p; w < end; w++)
			*w = (u32)(size_t)w ^ 0x0F1E2D3Cu;
		chunks[n++] = p;
	}
	SO_PHASE("mem verify");
	for (i = 0; i < n; i++) {
		u32 *w, *end = (u32 *)((u8 *)chunks[i] + CHUNK);
		for (w = (u32 *)chunks[i]; w < end; w++)
			if (*w != ((u32)(size_t)w ^ 0x0F1E2D3Cu))
				bad++;
		so_free(chunks[i]);
	}
	so_log("selftest: memory walk: %d blocks of 16 KB written and read back, %d bad words", n, bad);
	SO_PHASE(bad ? "mem BAD" : "mem ok");
	return bad ? -1 : 0;
}

/* Exercises the socket layer alone before any ENet traffic: open the
 * session's port, then PROBES round trips with the PC tool, which answers
 * each "SLPWII PROBE n" with "SLPWII PROBE-ACK n". */
/* PROBES round trips with the PC tool: "SLPWII PROBE n" -> "...-ACK n". */
static int probe_round(const char *name)
{
	char msg[32], want[40], reply[64];
	u32 rtt_sum = 0, rtt_max = 0;
	int i, acks = 0;
	SoSocketStats st;
	so_socket_stats(&st);	/* resets the maxima */
	for (i = 1; i <= PROBES; i++) {
		u64 sent = so_time_us();
		int n = so_snprintf(msg, sizeof(msg), "SLPWII PROBE %d", i), res;
		so_snprintf(want, sizeof(want), "SLPWII PROBE-ACK %d", i);
		SO_TRACE(SO_THREAD_TEST);
		res = so_socket_send(test_config.log_host, test_config.log_port, msg, n);
		if (res < 0)
			so_log("selftest: probe %d: send returned %d", i, res);
		while (so_time_us() - sent < PROBE_TIMEOUT_US) {
			u32 addr;
			u16 from;
			int got = so_socket_receive(&addr, &from, reply, sizeof(reply) - 1, NULL);
			if (got <= 0) {
				so_sleep_us(1000);
				continue;
			}
			reply[got] = '\0';
			if (strcmp(reply, want) == 0) {
				u32 rtt = (u32)(so_time_us() - sent);
				acks++;
				rtt_sum += rtt;
				if (rtt > rtt_max)
					rtt_max = rtt;
				break;
			}
		}
		so_sleep_us(20000);
	}
	so_socket_stats(&st);
	so_log("selftest: probes (%s): %d/%d answered, rtt avg %u us max %u us; IOS sendto max %u us",
		name, acks, PROBES, (unsigned)(acks ? rtt_sum / acks : 0), (unsigned)rtt_max, (unsigned)st.send_us_max);
	return acks;
}

/* Opens the session socket, fetches the plan, then measures round trips:
 * as configured, and (diag_main_priority) with the kernel's main thread at
 * another priority, to see whether IOS's network module waits for it. */
static int probe_network(void)
{
	u16 port = 0;
	int res, acks;

	so_log("selftest: stage 1: open the socket");
	SO_PHASE("st1 open");
	SO_TRACE(SO_THREAD_TEST);
	res = so_client_open_socket(&port);
	SO_TRACE(SO_THREAD_TEST);
	so_log("selftest: stage 1 done: socket on port %u: %d", (unsigned)port, res);
	if (res < 0)
		return -1;
	fetch_plan();
	so_sleep_us(STAGE_PAUSE_US);

	SO_PHASE("st2 probes");
	acks = probe_round("main thread 0x50");
	if (test_config.diag_main_priority) {
		SO_PHASE("st3 probes prio");
		res = so_set_main_thread_priority(test_config.diag_main_priority);
		so_log("selftest: main thread priority -> 0x%02x (%d)", test_config.diag_main_priority, res);
		if (res >= 0) {
			char name[24];
			so_snprintf(name, sizeof(name), "main thread 0x%02x", test_config.diag_main_priority);
			probe_round(name);
			res = so_set_main_thread_priority(SO_MAIN_THREAD_PRIORITY);
			so_log("selftest: main thread priority -> 0x50 (%d)", res);
		}
	}
	SO_PHASE(acks ? "st3 done" : "st3 no replies");
	so_socket_flush();
	return acks ? 0 : -2;
}

/* Replays builds 1-4's asynchronous sends after the test (see
 * so_socket_experiment_async_send); the beacon shows how far it got. */
static void async_send_experiment(void)
{
	static const char *const phases[2][2] = {
		{ "x1 async, no reply", "x1 survived" },
		{ "x2 async + reply", "x2 survived" },
	};
	int round, i;
	for (round = 0; round < 2; round++) {
		int ok = 0;
		SO_PHASE(phases[round][0]);
		so_log("selftest: experiment %d: 20 asynchronous sends%s", round + 1,
			round ? " answered with IOS_ResourceReply" : "");
		so_sleep_us(2000000);
		for (i = 0; i < 20; i++) {
			char msg[40];
			int n = so_snprintf(msg, sizeof(msg), "SLPWII ASYNC %d %d", round + 1, i);
			if (so_socket_experiment_async_send(test_config.log_host, test_config.log_port, msg, n, round) >= 0)
				ok++;
			so_sleep_us(50000);
		}
		so_log("selftest: experiment %d: %d/20 sent; waiting 10 s", round + 1, ok);
		so_sleep_us(10000000);
		SO_PHASE(phases[round][1]);
		so_log("selftest: experiment %d: still running", round + 1);
		so_sleep_us(3000000);
	}
	SO_PHASE("x done");
}

/* How long short sleeps and event waits really take: lockstep pacing at
 * 59.94 Hz depends on them. */
static void timer_check(void)
{
	static SoEvent ev;
	static const u32 asks[] = { 1000, 2000, 5000, 16683 };
	unsigned i, k;
	if (so_event_init(&ev) < 0)
		return;
	for (i = 0; i < sizeof(asks) / sizeof(asks[0]); i++) {
		u32 sleep_sum = 0, sleep_max = 0, wait_sum = 0, wait_max = 0;
		for (k = 0; k < 10; k++) {
			u64 t0 = so_time_us();
			u32 took;
			so_sleep_us(asks[i]);
			took = (u32)(so_time_us() - t0);
			sleep_sum += took;
			if (took > sleep_max)
				sleep_max = took;
			t0 = so_time_us();
			so_event_wait(&ev, asks[i]);
			took = (u32)(so_time_us() - t0);
			wait_sum += took;
			if (took > wait_max)
				wait_max = took;
		}
		so_log("selftest: timers: ask %u us: sleep avg %u max %u, event wait avg %u max %u", (unsigned)asks[i],
			(unsigned)(sleep_sum / 10), (unsigned)sleep_max, (unsigned)(wait_sum / 10), (unsigned)wait_max);
	}
}

static u32 SelftestThread(void *unused)
{
	SoSearch search;
	SoSelections sel;
	Results r;
	int n, ok = 0;
	u32 start;
	(void)unused;
	memset(&r, 0, sizeof(r));

	SO_TRACE(SO_THREAD_TEST);
	SO_PHASE("st0 wait");
	while (!so_client_traffic_allowed())
		so_sleep_us(500000);
	SO_TRACE(SO_THREAD_TEST);
	SO_PHASE("st0 timers");
	timer_check();
	SO_PHASE("st0 go");

	if (so_config_is_real_mm(&test_config) && !test_config.selftest_allow_real_mm) {
		so_log("selftest: refusing Slippi's real server (set selftest_allow_real_mm=1)");
		report(&r, 0);
		return 0;
	}
	memset(&search, 0, sizeof(search));
	search.mode = SO_MODE_DIRECT;
	n = so_encode_connect_code(test_config.selftest_code, search.connect_code, sizeof(search.connect_code));
	if (n <= 0) {
		so_log("selftest: selftest_code '%s' is not a connect code", test_config.selftest_code);
		report(&r, 0);
		return 0;
	}
	search.connect_code_len = (u8)n;
	if (test_config.log_host && probe_network() < 0) {
		so_log("selftest: the socket probe failed; not starting matchmaking");
		report(&r, 0);
		return 0;
	}
	so_sleep_us(STAGE_PAUSE_US);
	if (test_config.selftest_memory_walk) {
		so_log("selftest: stage 3b: memory walk");
		memory_walk();
		so_sleep_us(STAGE_PAUSE_US);
	}
	so_log("selftest: stage 4: matchmaking; searching for %s", test_config.selftest_code);
	SO_PHASE("st4 mm");
	SO_TRACE(SO_THREAD_TEST);
	so_client_find_match(&search);
	SO_TRACE(SO_THREAD_TEST);
	if (wait_for_state(SO_CLIENT_CONNECTED, CONNECT_TIMEOUT_MS) < 0) {
		char error[128];
		so_client_error(error, sizeof(error));
		so_log("selftest: no match (%s; %s)", so_client_state_name(so_client_state()), error);
		report(&r, 0);
		return 0;
	}

	/* Character select: pick, then wait for the opponent's pick. The
	 * decider also picks the stage, as the game would. */
	memset(&sel, 0, sizeof(sel));
	sel.character_id = TEST_CHARACTER;
	sel.is_character_selected = 1;
	sel.rng_offset = so_random();
	so_client_lock();
	if (so_np_is_decider()) {
		sel.stage_id = 0x1F;	/* Battlefield */
		sel.is_stage_selected = 1;
	}
	so_np_set_selections(&sel);
	so_client_unlock();
	start = so_time_ms();
	for (;;) {
		int picked;
		so_client_lock();
		picked = so_np_remote_selections(0)->is_character_selected;
		so_client_unlock();
		if (picked)
			break;
		if (so_time_ms() - start > SELECTIONS_TIMEOUT_MS || so_client_state() != SO_CLIENT_CONNECTED) {
			so_log("selftest: the opponent did not pick a character");
			report(&r, 0);
			so_client_cancel();
			return 0;
		}
		so_sleep_us(20000);
	}

	SO_PHASE("st5 frames");
	if (run_frames(test_config.selftest_frames, &r) == 0)
		ok = r.mismatches == 0;
so_sleep_us(2000000);	/* let the last acks arrive */
	report(&r, ok);
	so_client_cancel();
	if (test_config.selftest_async_probe && test_config.log_host) {
		so_sleep_us(3000000);
		async_send_experiment();
	}
	return 0;
}

void so_selftest_start(const SoConfig *config)
{
	test_config = *config;
	if (so_thread_start(SelftestThread, NULL, &__slippi_online_test_stack_addr,
			(u32)(size_t)&__slippi_online_test_stack_size, TEST_THREAD_PRIORITY) < 0)
		so_log("selftest: could not start");
}
