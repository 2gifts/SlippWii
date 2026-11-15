/* slippi_online/so_mq.h
 * Slippi's match quality rule: what Slippi Dolphin ends a Ranked match by
 * (the match-quality tracker in its development source, 2026-11), kept here
 * for every game so that the Wii's log says how the game would have been
 * judged. Nothing is ended by it: SlippWii does not play Ranked.
 *
 * Every 150 frames from frame 120 on, the worst of three signals moves a
 * debt:
 *   time    the 150 frames took 10%, 50% or 75% longer than at 59.94 fps
 *   depth   how far the game ran ahead of the opponent's newest input, on
 *           average over the passes: 2, 3 or 4.75 frames (judged as if the
 *           local delay were 2 at least)
 *   stalls  passes halted at the rollback limit: an event (5 passes or
 *           more, less than 0.5 s apart) or shorter ones
 * Mild +4, bad +8, severe +15, a stall event +9, shorter stalls +3, a
 * healthy interval -1; the debt never goes under 0. After frame 3600 the
 * time and depth thresholds are 15% higher. At 30 a Ranked match is ended.
 * Ping is not judged.
 */
#ifndef SO_MQ_H
#define SO_MQ_H

#include "global.h"

#define SO_MQ_TERMINATE_DEBT 30

enum { SO_MQ_TIME, SO_MQ_DEPTH, SO_MQ_STALLS, SO_MQ_SIGNALS };

typedef struct {
	u8 delay;	/* the local player's delay frames */
	u8 clock_started;	/* the first boundary has passed */
	u8 have_stall;
	u8 verdict;	/* the debt has reached the limit (said once a game) */
	s32 debt;
	s32 last_interval_frame;
	u64 interval_start_us;
	u64 last_stall_us;
	u32 depth_sum, depth_samples;
	u32 current_stall_passes;	/* in the stall event under way, or the last one */
	u32 interval_stall_events, interval_stalled_passes;

	/* the game so far, for its log */
	u32 stall_events;
	s32 max_debt, verdict_frame;
	u16 intervals;
	/* intervals by each signal's debt: healthy, mild, bad, severe (stalls:
	 * none, short ones, an event) */
	u16 tiers[SO_MQ_SIGNALS][4];
	u32 game_depth_sum, game_depth_samples;

	/* the interval last judged */
	s8 change, signal[SO_MQ_SIGNALS];
	u16 time_pct;	/* its real time, percent of 150 frames' */
	u16 depth_x100;	/* its average depth, hundredths of a frame */
	u16 last_stall_events, last_stalled_passes;
} SoMq;

/* A new game. */
void so_mq_start(SoMq *q, u8 delay);

/* One pass through the online frame handler, halted or not (a frame held
 * back is passed again each time). `stalled`: halted because the opponent's
 * inputs are too far behind. Returns 0, 1 when this pass ended an interval
 * (its numbers are in `q`), or 2 when that interval also brought the debt
 * to the limit for the first time this game. */
int so_mq_frame(SoMq *q, s32 frame, u64 time_us, int stalled, s32 latest_remote_frame);

#endif
