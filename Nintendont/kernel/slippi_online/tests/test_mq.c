/* Host unit tests for so_mq.c: Slippi's match quality rule. The rule is
 * written again here as Slippi's own arithmetic has it, in doubles, and the
 * kernel's whole-number version must agree with it pass for pass. */
#include <stdlib.h>
#include "so_mq.h"
#include "test.h"

#define FRAME_US 16683ull

typedef struct {
	int delay, verdict, debt, have_start, last_interval_frame, have_stall;
	unsigned long long start, last_stall, depth_sum, depth_samples;
	unsigned cur, events, passes;
} Ref;

static int ref_tier(double value, const double *t, double mod)
{
	if (value >= t[2] * mod)
		return 15;
	if (value >= t[1] * mod)
		return 8;
	if (value >= t[0] * mod)
		return 4;
	return -1;
}

static int ref_frame(Ref *r, int frame, unsigned long long t, int stall, int latest)
{
	static const double overrun[3] = { 0.1, 0.5, 0.75 }, depths[3] = { 2.0, 3.0, 4.75 };
	int shortfall = 2 - r->delay > 0 ? 2 - r->delay : 0, depth = frame - latest - shortfall;
	depth = depth < 0 ? 0 : depth > 8 ? 8 : depth;
	r->depth_sum += (unsigned)depth;
	r->depth_samples++;
	if (stall) {
		if (frame <= 120)
			return 0;
		r->passes++;
		if (!r->have_stall || (t > r->last_stall ? t - r->last_stall : 0) > 500000)
			r->cur = 0;
		r->have_stall = 1;
		r->last_stall = t;
		if (++r->cur == 5)
			r->events++;
		return 0;
	}
	if (frame < 120 || (frame - 120) % 150 || frame == r->last_interval_frame)
		return 0;
	r->last_interval_frame = frame;
	{
		unsigned long long sum = r->depth_sum, n = r->depth_samples, start = r->start;
		unsigned events = r->events, passes = r->passes;
		double expected = (double)(FRAME_US * 150), ratio, avg, mod = frame > 3600 ? 1.15 : 1.0;
		int speed, dep, st, change;
		r->depth_sum = r->depth_samples = 0;
		r->events = r->passes = 0;
		r->start = t;
		if (!r->have_start) {
			r->have_start = 1;
			return 0;
		}
		ratio = (double)(t > start ? t - start : 0) / expected;
		avg = (double)sum / (double)(n ? n : 1);
		speed = ref_tier(ratio - 1.0, overrun, mod);
		dep = ref_tier(avg, depths, mod);
		st = events ? 9 : passes ? 3 : -1;
		change = speed > dep ? speed : dep;
		change = st > change ? st : change;
		r->debt = r->debt + change > 0 ? r->debt + change : 0;
		if (r->debt >= 30 && !r->verdict) {
			r->verdict = 1;
			return 2;
		}
		return 1;
	}
}

/* Plays frames `from`..`to` at `us` a frame, each `depth` behind; the time is kept in *t. */
static int play(SoMq *q, int from, int to, unsigned long long us, int depth, unsigned long long *t)
{
	int f, last = 0, res;
	for (f = from; f <= to; f++) {
		*t += us;
		res = so_mq_frame(q, f, *t, 0, f - depth);
		if (res)
			last = res;
	}
	return last;
}

static void test_mq_rule(void)
{
	SoMq q;
	unsigned long long t = 1000000;
	int f, i;

	/* a healthy game: the debt stays at 0 and no interval is unhealthy */
	so_mq_start(&q, 2);
	CHECK_INT(play(&q, 1, 120, FRAME_US, 1, &t), 0);	/* the first boundary starts the clock */
	CHECK_INT(q.intervals, 0);
	CHECK_INT(play(&q, 121, 270, FRAME_US, 1, &t), 1);
	CHECK_INT(q.intervals, 1);
	CHECK_INT(q.change, -1);
	CHECK_INT(q.debt, 0);
	CHECK_INT(q.time_pct, 100);
	CHECK_INT(q.depth_x100, 100);
	play(&q, 271, 3000, FRAME_US, 1, &t);
	CHECK_INT(q.debt, 0);
	CHECK_INT(q.max_debt, 0);
	CHECK_INT(q.verdict, 0);
	CHECK_INT(q.tiers[SO_MQ_DEPTH][0], q.intervals);

	/* an average depth of 2 frames is mild: +4 an interval, the limit at the 8th */
	so_mq_start(&q, 2);
	t = 0;
	play(&q, 1, 120, FRAME_US, 2, &t);
	for (i = 1; i <= 7; i++) {
		CHECK_INT(play(&q, 120 + (i - 1) * 150 + 1, 120 + i * 150, FRAME_US, 2, &t), 1);
		CHECK_INT(q.debt, 4 * i);
	}
	CHECK_INT(play(&q, 1171, 1320, FRAME_US, 2, &t), 2);
	CHECK_INT(q.debt, 32);
	CHECK_INT(q.verdict_frame, 1320);
	CHECK_INT(play(&q, 1321, 1470, FRAME_US, 2, &t), 1);	/* said once; the game is still tracked */
	CHECK_INT(q.debt, 36);
	/* just under: 149 passes at 2 and one at 1 */
	so_mq_start(&q, 2);
	t = 0;
	play(&q, 1, 120, FRAME_US, 2, &t);
	play(&q, 121, 269, FRAME_US, 2, &t);
	CHECK_INT(play(&q, 270, 270, FRAME_US, 1, &t), 1);
	CHECK_INT(q.change, -1);
	/* bad and severe; a delay under 2 is judged as 2 */
	so_mq_start(&q, 2);
	t = 0;
	play(&q, 1, 270, FRAME_US, 3, &t);
	CHECK_INT(q.change, 8);
	play(&q, 271, 420, FRAME_US, 5, &t);
	CHECK_INT(q.change, 15);
	CHECK_INT(q.debt, 23);
	so_mq_start(&q, 1);
	t = 0;
	play(&q, 1, 270, FRAME_US, 3, &t);	/* 3 behind at 1 delay frame: 2 at 2 */
	CHECK_INT(q.change, 4);
	CHECK_INT(q.depth_x100, 200);
	so_mq_start(&q, 3);
	t = 0;
	play(&q, 1, 270, FRAME_US, 2, &t);	/* a higher delay is not judged more kindly */
	CHECK_INT(q.change, 4);

	/* time: 10%, 50% and 75% over */
	so_mq_start(&q, 2);
	t = 0;
	play(&q, 1, 120, FRAME_US, 0, &t);
	play(&q, 121, 269, FRAME_US, 0, &t);
	t += FRAME_US * 15;	/* 150 frames in 165 frames' time, to the microsecond */
	CHECK_INT(play(&q, 270, 270, FRAME_US, 0, &t), 1);
	CHECK_INT(q.signal[SO_MQ_TIME], 4);
	CHECK_INT(q.time_pct, 110);
	play(&q, 271, 419, FRAME_US, 0, &t);
	t += FRAME_US * 15 - 1;	/* a microsecond less */
	play(&q, 420, 420, FRAME_US, 0, &t);
	CHECK_INT(q.signal[SO_MQ_TIME], -1);
	play(&q, 421, 569, FRAME_US, 0, &t);
	t += FRAME_US * 75;
	play(&q, 570, 570, FRAME_US, 0, &t);
	CHECK_INT(q.signal[SO_MQ_TIME], 8);
	play(&q, 571, 719, FRAME_US, 0, &t);
	t += FRAME_US * 113;
	play(&q, 720, 720, FRAME_US, 0, &t);
	CHECK_INT(q.signal[SO_MQ_TIME], 15);
	CHECK_INT(q.debt, 4 + 0 + 8 + 15 - 1);

	/* stalls: 4 halted passes are short ones (+3), the 5th makes an event (+9), once;
	 * halts 0.5 s apart or less are one event; none count up to frame 120 */
	so_mq_start(&q, 2);
	t = 0;
	play(&q, 1, 100, FRAME_US, 1, &t);
	for (i = 0; i < 30; i++)
		CHECK_INT(so_mq_frame(&q, 101, t += FRAME_US, 1, 93), 0);
	play(&q, 101, 200, FRAME_US, 1, &t);
	for (i = 0; i < 4; i++)
		so_mq_frame(&q, 201, t += FRAME_US, 1, 193);
	play(&q, 201, 270, FRAME_US, 1, &t);
	CHECK_INT(q.signal[SO_MQ_STALLS], 3);
	CHECK_INT(q.last_stalled_passes, 4);
	CHECK_INT(q.stall_events, 0);
	CHECK_INT(q.debt, 3);
	play(&q, 271, 300, FRAME_US, 1, &t);
	for (i = 0; i < 3; i++)
		so_mq_frame(&q, 301, t += FRAME_US, 1, 293);
	play(&q, 301, 320, FRAME_US, 1, &t);	/* 20 frames: 0.33 s, the same event */
	for (i = 0; i < 9; i++)
		so_mq_frame(&q, 321, t += FRAME_US, 1, 313);
	play(&q, 321, 420, FRAME_US, 1, &t);
	CHECK_INT(q.signal[SO_MQ_STALLS], 9);
	CHECK_INT(q.last_stall_events, 1);
	CHECK_INT(q.stall_events, 1);
	CHECK_INT(q.debt, 12);
	play(&q, 421, 500, FRAME_US, 1, &t);	/* over 0.5 s later: a new event, too short to count */
	for (i = 0; i < 4; i++)
		so_mq_frame(&q, 501, t += FRAME_US, 1, 493);
	play(&q, 501, 570, FRAME_US, 1, &t);
	CHECK_INT(q.signal[SO_MQ_STALLS], 3);
	CHECK_INT(q.stall_events, 1);

	/* a frame held back for the time sync is passed twice and ends one interval */
	so_mq_start(&q, 2);
	t = 0;
	play(&q, 1, 270, FRAME_US, 1, &t);
	CHECK_INT(q.intervals, 1);
	CHECK_INT(so_mq_frame(&q, 270, t += FRAME_US, 0, 269), 0);
	CHECK_INT(q.intervals, 1);

	/* after frame 3600 the thresholds are 15% higher: 2.3 frames is mild, less is healthy */
	so_mq_start(&q, 2);
	t = 0;
	play(&q, 1, 3720, FRAME_US, 1, &t);
	for (f = 3721; f <= 3870; f++)
		so_mq_frame(&q, f, t += FRAME_US, 0, f - (f <= 3721 + 44 ? 3 : 2));	/* 45 at 3, 105 at 2: 2.3 */
	CHECK_INT(q.depth_x100, 230);
	CHECK_INT(q.signal[SO_MQ_DEPTH], 4);
	for (f = 3871; f <= 4020; f++)
		so_mq_frame(&q, f, t += FRAME_US, 0, f - (f <= 3871 + 43 ? 3 : 2));	/* 44 at 3 */
	CHECK_INT(q.signal[SO_MQ_DEPTH], -1);
}

static unsigned rnd_state;
static unsigned rnd(unsigned n)
{
	rnd_state = rnd_state * 1664525u + 1013904223u;
	return (rnd_state >> 8) % n;
}

/* Random games, the two versions side by side: every pass's answer and the debt. */
static void test_mq_against_doubles(void)
{
	int game, mismatches = 0, verdicts = 0, unhealthy = 0;
	unsigned intervals = 0;
	for (game = 0; game < 400; game++) {
		static const unsigned pct[4] = { 100, 110, 150, 175 };
		SoMq q;
		Ref r;
		unsigned long long t;
		int delay, behind, slow = 0, f, k, kind = game % 4;
		rnd_state = 12345u + (unsigned)game * 7919u;
		t = 5000 + rnd(1000000);
		delay = 1 + (int)rnd(3);
		behind = (int)rnd(kind == 0 ? 2 : 5);
		memset(&r, 0, sizeof(r));
		r.delay = delay;
		so_mq_start(&q, (u8)delay);
		for (f = 1; f <= 5400; f++) {
			int passes = 1, a, b, latest;
			/* a game in four clean, one near the depth thresholds, one with halts, one with all of it */
			if (rnd(40) == 0)
				behind = (int)rnd(kind == 0 ? 2 : kind == 3 ? 10 : 5);
			if (kind == 3 && rnd(300) == 0)
				slow = (int)rnd(4);	/* stretches at 100%, 110%, 150%, 175% of a frame's time */
			if (kind >= 2 && rnd(kind == 2 ? 1500 : 200) == 0) {	/* a halt of 1 to 12 passes */
				int n = 1 + (int)rnd(12);
				for (k = 0; k < n; k++) {
					t += 14000 + rnd(6000);
					a = so_mq_frame(&q, f, t, 1, f - 8);
					b = ref_frame(&r, f, t, 1, f - 8);
					mismatches += a != b;
				}
			}
			if (rnd(250) == 0)
				passes = 2;	/* held back once by the time sync */
			for (k = 0; k < passes; k++) {
				/* (two passes in three to the microsecond: the thresholds' exact values come up) */
				t += FRAME_US * pct[slow] / 100;
				if (rnd(3) == 0)
					t += rnd(400);
				latest = f - behind + (int)rnd(2);
				a = so_mq_frame(&q, f, t, 0, latest);
				b = ref_frame(&r, f, t, 0, latest);
				mismatches += a != b || q.debt != r.debt;
				if (a) {
					intervals++;
					unhealthy += q.change > 0;
				}
				verdicts += a == 2;
			}
		}
	}
	CHECK_INT(mismatches, 0);
	/* (the games must have been worth comparing) */
	CHECK(intervals > 10000);
	CHECK(unhealthy > 1000 && unhealthy < (int)intervals * 3 / 4);
	CHECK(verdicts > 50 && verdicts < 350);
	printf("  %u intervals, %d unhealthy, %d games past the limit" "\n", intervals, unhealthy, verdicts);
}

void test_mq(void)
{
	test_mq_rule();
	test_mq_against_doubles();
}
