/* slippi_online/so_suggest.h
 * Input suggestions (Slippi 3.7.0), the receiving side: what an opponent sends between frames as soon as
 * its input changes (0x87), and the inputs the game predicts with from them. A port of slippi-rust-
 * extensions' input-suggestions crate, remote.rs (934f373), without networking, for the unit tests.
 *
 * Suggestions only ever feed predictions: the game records every prediction and rolls back when the
 * real input differs, so every client ends up on the real inputs. A player whose suggestions keep
 * disagreeing with their real inputs only makes the game look worse: past a limit, theirs are not used
 * for the rest of the game.
 */
#ifndef SO_SUGGEST_H
#define SO_SUGGEST_H

#include "global.h"

#define SO_SUGGEST_PAD 8		/* a suggestion's inputs: a pad's first 8 bytes */
#define SO_SUGGEST_FRAMES 7		/* the frames after an opponent's newest input the game may predict */
#define SO_SUGGEST_KEEP 24		/* kept a player (Dolphin keeps 512; only those ahead of the newest real
					 * input are, and a player changes inputs a few times a frame at most) */
#define SO_SUGGEST_LEAD 120		/* frames past the newest real input: further is garbage (an old game's) */

typedef struct {
	s32 target;	/* the first frame the input applies to, in the sender's numbering (its delay included) */
	u32 seq;
	u8 pad[SO_SUGGEST_PAD];
} SoSuggestion;

typedef struct {
	SoSuggestion s[SO_SUGGEST_KEEP];
	u8 count;
	u8 have_latest;	/* a real input has come */
	u8 ignored;	/* too many wrong: not used for the rest of the game */
	s32 latest_frame;
	u8 latest_pad[SO_SUGGEST_PAD];
	u32 bucket;	/* the cost of wrong suggestions, in frames' worth of draining */
	/* (the game's report) */
	u32 received, taken, used;
} SoSuggestRemote;

void so_suggest_reset(SoSuggestRemote *r);
/* A suggestion arrived. */
void so_suggest_on_suggestion(SoSuggestRemote *r, s32 target, u32 seq, const u8 *pad);
/* A real input arrived, for `frame` (each frame once, in any order: older ones are passed over).
 * Returns 1 when it is the one that got this player's suggestions ignored. */
int so_suggest_on_real(SoSuggestRemote *r, s32 frame, const u8 *pad);
/* The inputs the game is to predict each of the SO_SUGGEST_FRAMES frames after `latest_frame` with:
 * where a suggestion applies, its 8 bytes and the rest of the slot zero (`stride` bytes a slot, as
 * Dolphin's: "suggestions only have the inputs"); other slots are left as they are (the latest input,
 * put there by the caller). Returns how many slots were filled from suggestions. */
u32 so_suggest_fill(SoSuggestRemote *r, s32 latest_frame, u8 *out, u32 stride);

/* (the costs, for the tests) */
u32 so_suggest_worse_cost(const u8 *previous, const u8 *suggested, const u8 *real);

/* ---- the sending side: a GameCube controller on the console's own SI -------------------------------- */
/* The console asks the controller twice a refresh, and the game reads one of the answers: the other comes
 * about 8.3 ms before the game sees it. When it says something the opponent does not know yet, it goes out
 * as a suggestion for the next poll's frame, as a PC's GameCube adapter thread sends them (Slippi 3.7.0).
 * The game runs the SDK's own PADRead on the real SI here (Slippi Nintendont), so an answer becomes the
 * game's pad as the SDK makes it: so_suggest_pad_from_si. */
#define SO_SUGGEST_START_SYNC_FRAME 78	/* Online.s: the game zeroes inputs polled before this, less the delay */
#define SO_SUGGEST_AGREE_RUN 60		/* polls in a row whose pad the SI's answer made exactly, before any is sent */

/* An SI answer (its two input words) as the 8 input bytes the game sends for it: the SDK's SPEC2_MakeStatus
 * in analog mode 3 with the controller's origin (`origin`: its 12-byte PADStatus, as the SDK keeps it), and
 * TriggerSendInput's snap of a resting stick to center. -1 for an answer the game would not use (an error,
 * an origin request, another analog mode). */
int so_suggest_pad_from_si(u32 hi, u32 lo, const u8 *origin, u32 analog_mode, u8 *pad);
/* Whether `next` is worth suggesting after `prev` was sent (slippi-rust-extensions' is_meaningful_change):
 * pressing or releasing A, B, X, Y, L, R, Z or Start; a trigger crossing light shield; a stick leaving the
 * deadzone, or reaching dash, tap jump, crouch or smash range. */
int so_suggest_meaningful(const u8 *prev, const u8 *next);

typedef struct {
	u8 last[SO_SUGGEST_PAD];	/* the newest input the opponents have from us, real or suggested */
	u8 have_last;
	u32 seq;
	s32 next_target;	/* the frame the next poll's pad goes as (its delay included) */
	u16 agree_run;		/* polls in a row whose pad the SI's answer made exactly */
	/* (the game's report) */
	u32 polls, agreed, sent, unknown, late_agreed;
} SoSuggestSend;

void so_suggest_send_reset(SoSuggestSend *s);
/* A poll: the pad the game sent, as frame `frame` (its delay included); `how` 0 as usual, 1 a time sync's
 * skip (the same frame polled again: its pad not sent), 2 an advance; `agree` whether the SI's answer the
 * game read made this pad (taken just after the read: a rollback's 0xB0 comes after the next answer),
 * -1 not known (no sample of it: counted apart, the run neither grows nor ends). 1 with `out` set: a
 * suggestion to send (a skipped frame's input). */
int so_suggest_send_on_poll(SoSuggestSend *s, s32 frame, int how, const u8 *pad, int agree, SoSuggestion *out);
/* The SI's newer answer, between polls, as a pad. 1 with `out` set: a suggestion to send. */
int so_suggest_send_on_si(SoSuggestSend *s, const u8 *pad, SoSuggestion *out);

#endif
