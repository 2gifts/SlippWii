/* slippi_online/so_mq.c
 * Slippi's match quality rule (so_mq.h), in whole numbers: the kernel has
 * no floating point. Each comparison is the exact one (a value of a/b
 * against a threshold in 400ths), which is what the rule's own arithmetic
 * in doubles comes to for every value a game can produce.
 */
#include "so_mq.h"
#include "string.h"

#define ROLLBACK_MAX_FRAMES 7
#define FRAME_TIME_US 16683
#define WARMUP_FRAMES 120
#define INTERVAL_FRAMES 150
#define LATE_GAME_FRAME 3600
#define MIN_JUDGED_DELAY 2
#define STALL_MERGE_GAP_US 500000
#define MIN_STALL_PASSES 5

#define MILD_DEBT 4
#define BAD_DEBT 8
#define SEVERE_DEBT 15
#define HEALTHY_DEBT (-1)
#define STALL_DEBT 9
#define SHORT_STALL_DEBT 3

/* thresholds in 400ths: 0.1, 0.5, 0.75 over in time; 2, 3, 4.75 frames deep */
static const u16 overrun_tiers[3] = { 40, 200, 300 };
static const u16 depth_tiers[3] = { 800, 1200, 1900 };

/* The tier (0 healthy .. 3 severe) of num/den against the thresholds, which
 * are 15% higher late in a game: num/den >= t/400 * 23/20. */
static int tier_of(u64 num, u64 den, const u16 *tiers, int late)
{
	u64 value = num * 8000, scale = den * (late ? 23 : 20);
	int i;
	for (i = 2; i >= 0; i--)
		if (value >= tiers[i] * scale)
			return i + 1;
	return 0;
}

static int tier_debt(int tier)
{
	static const s8 debt[4] = { HEALTHY_DEBT, MILD_DEBT, BAD_DEBT, SEVERE_DEBT };
	return debt[tier];
}

void so_mq_start(SoMq *q, u8 delay)
{
	memset(q, 0, sizeof(*q));
	q->delay = delay;
}

static void on_stall(SoMq *q, s32 frame, u64 time_us)
{
	int new_event;
	if (frame <= WARMUP_FRAMES)
		return;	/* loading and the time sync halt both sides at a game's start */
	q->interval_stalled_passes++;
	new_event = !q->have_stall || (time_us > q->last_stall_us && time_us - q->last_stall_us > STALL_MERGE_GAP_US);
	q->have_stall = 1;
	q->last_stall_us = time_us;
	if (new_event)
		q->current_stall_passes = 0;
	/* an event counts once, on the pass that makes it long enough */
	if (++q->current_stall_passes == MIN_STALL_PASSES) {
		q->interval_stall_events++;
		q->stall_events++;
	}
}

static int on_interval(SoMq *q, s32 frame, u64 time_us)
{
	const u64 expected = (u64)FRAME_TIME_US * INTERVAL_FRAMES;
	u32 depth_sum = q->depth_sum, samples = q->depth_samples;
	u32 events = q->interval_stall_events, stalled = q->interval_stalled_passes;
	u64 start = q->interval_start_us, elapsed;
	int late = frame > LATE_GAME_FRAME, t[SO_MQ_SIGNALS], change, i;

	q->depth_sum = q->depth_samples = 0;
	q->interval_stall_events = q->interval_stalled_passes = 0;
	q->interval_start_us = time_us;
	if (!q->clock_started) {	/* the first boundary only starts the clock */
		q->clock_started = 1;
		return 0;
	}
	elapsed = time_us > start ? time_us - start : 0;
	if (!samples)
		samples = 1;

	t[SO_MQ_TIME] = tier_of(elapsed > expected ? elapsed - expected : 0, expected, overrun_tiers, late);
	t[SO_MQ_DEPTH] = tier_of(depth_sum, samples, depth_tiers, late);
	t[SO_MQ_STALLS] = events ? 2 : stalled ? 1 : 0;
	q->signal[SO_MQ_TIME] = (s8)tier_debt(t[SO_MQ_TIME]);
	q->signal[SO_MQ_DEPTH] = (s8)tier_debt(t[SO_MQ_DEPTH]);
	q->signal[SO_MQ_STALLS] = (s8)(events ? STALL_DEBT : stalled ? SHORT_STALL_DEBT : HEALTHY_DEBT);
	/* the worst of the three, not their sum: one bad connection shows in all of them */
	change = q->signal[0];
	for (i = 1; i < SO_MQ_SIGNALS; i++)
		if (q->signal[i] > change)
			change = q->signal[i];
	q->debt += change;
	if (q->debt < 0)
		q->debt = 0;

	q->change = (s8)change;
	q->time_pct = (u16)(elapsed * 100 / expected > 0xFFFF ? 0xFFFF : elapsed * 100 / expected);
	q->depth_x100 = (u16)((u64)depth_sum * 100 / samples);
	q->last_stall_events = (u16)events;
	q->last_stalled_passes = (u16)stalled;
	q->intervals++;
	for (i = 0; i < SO_MQ_SIGNALS; i++)
		q->tiers[i][t[i]]++;
	q->game_depth_sum += depth_sum;
	q->game_depth_samples += samples;
	if (q->debt > q->max_debt)
		q->max_debt = q->debt;
	if (q->debt >= SO_MQ_TERMINATE_DEBT && !q->verdict) {
		q->verdict = 1;
		q->verdict_frame = frame;
		return 2;
	}
	return 1;
}

int so_mq_frame(SoMq *q, s32 frame, u64 time_us, int stalled, s32 latest_remote_frame)
{
	s32 shortfall = MIN_JUDGED_DELAY - (s32)q->delay, depth;
	if (shortfall < 0)
		shortfall = 0;
	/* (a halted pass is past the limit by definition: the cap covers a
	 * queue that is empty and reads as frame 0) */
	depth = frame - latest_remote_frame - shortfall;
	if (depth < 0)
		depth = 0;
	if (depth > ROLLBACK_MAX_FRAMES + 1)
		depth = ROLLBACK_MAX_FRAMES + 1;
	q->depth_sum += (u32)depth;
	q->depth_samples++;

	if (stalled) {
		on_stall(q, frame, time_us);
		return 0;
	}
	/* a frame held back for the time sync comes more than once and ends one interval */
	if (frame < WARMUP_FRAMES || (frame - WARMUP_FRAMES) % INTERVAL_FRAMES || frame == q->last_interval_frame)
		return 0;
	q->last_interval_frame = frame;
	return on_interval(q, frame, time_us);
}
