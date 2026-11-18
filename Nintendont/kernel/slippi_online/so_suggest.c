/* slippi_online/so_suggest.c: input suggestions, the receiving side (so_suggest.h). As remote.rs and
 * pad.rs of slippi-rust-extensions' input-suggestions crate (934f373), whose numbers these are. */
#include "so_suggest.h"
#include <string.h>

/* A player whose suggestions don't match their real inputs: each suggestion is checked once, against
 * the real input for the frame it targets, and a mistake the player's previous input didn't make adds
 * its cost; the bucket drains a point every SO_DRAIN frames, so occasional misses never add up. Over
 * SO_IGNORE_AFTER points, the player's suggestions are ignored for the rest of the game. */
#define SO_IGNORE_AFTER 15u
#define SO_DRAIN 200u
#define SO_WRONG_BUTTON_COST 3u
#define SO_FARTHER_ANALOG_COST 1u
#define SO_TRIGGER_DEADZONE 42	/* triggers at or below this are released, as the game's comparison has it */

static u16 wrong_buttons(const u8 *p, const u8 *r)
{
	return (u16)(((p[0] ^ r[0]) & 0x1F) << 8 | ((p[1] ^ r[1]) & 0x7F));
}

static u32 axis(const u8 *p, const u8 *r, int i)
{
	s32 d = (s32)(s8)p[i] - (s32)(s8)r[i];
	return (u32)(d < 0 ? -d : d);
}

static u32 trigger(const u8 *p, const u8 *r, int i)
{
	if (p[i] <= SO_TRIGGER_DEADZONE && r[i] <= SO_TRIGGER_DEADZONE)
		return 0;
	return p[i] > r[i] ? (u32)(p[i] - r[i]) : (u32)(r[i] - p[i]);
}

static u32 umax(u32 a, u32 b)
{
	return a > b ? a : b;
}

static u32 analog_error(const u8 *p, const u8 *r)
{
	return umax(axis(p, r, 2), axis(p, r, 3)) + umax(axis(p, r, 4), axis(p, r, 5)) +
	       umax(trigger(p, r, 6), trigger(p, r, 7));
}

u32 so_suggest_worse_cost(const u8 *previous, const u8 *suggested, const u8 *real)
{
	u16 pw = wrong_buttons(previous, real), sw = wrong_buttons(suggested, real);
	if (sw & (u16)~pw)
		return SO_WRONG_BUTTON_COST;
	if (sw == pw && analog_error(suggested, real) > analog_error(previous, real))
		return SO_FARTHER_ANALOG_COST;
	return 0;
}

void so_suggest_reset(SoSuggestRemote *r)
{
	memset(r, 0, sizeof(*r));
}

void so_suggest_on_suggestion(SoSuggestRemote *r, s32 target, u32 seq, const u8 *pad)
{
	s32 latest = r->have_latest ? r->latest_frame : 0;
	r->received++;
	if (r->ignored || target <= latest || target > latest + SO_SUGGEST_LEAD)
		return;
	if (r->count == SO_SUGGEST_KEEP) {	/* (the oldest goes) */
		u32 i;
		for (i = 0; i + 1 < SO_SUGGEST_KEEP; i++)	/* (the kernel has no memmove) */
			r->s[i] = r->s[i + 1];
		r->count--;
	}
	r->s[r->count].target = target;
	r->s[r->count].seq = seq;
	memcpy(r->s[r->count].pad, pad, SO_SUGGEST_PAD);
	r->count++;
	r->taken++;
}

/* The newest (by seq) suggestion that applies to `frame` when the latest real input is for `latest`:
 * one for frame T describes the input from T on, so it applies when T <= frame, and it is only newer
 * than the real inputs when T > latest. */
static const SoSuggestion *best(const SoSuggestRemote *r, s32 latest, s32 frame)
{
	const SoSuggestion *b = 0;
	u32 i;
	for (i = 0; i < r->count; i++) {
		const SoSuggestion *s = &r->s[i];
		if (s->target > latest && s->target <= frame && (!b || s->seq > b->seq))
			b = s;
	}
	return b;
}

int so_suggest_on_real(SoSuggestRemote *r, s32 frame, const u8 *pad)
{
	int had = r->have_latest;
	s32 previous_frame = r->latest_frame;
	u8 previous_pad[SO_SUGGEST_PAD];
	const SoSuggestion *suggested;
	u32 i, kept = 0;
	if (had && frame <= r->latest_frame)
		return 0;
	memcpy(previous_pad, r->latest_pad, SO_SUGGEST_PAD);
	r->have_latest = 1;
	r->latest_frame = frame;
	memcpy(r->latest_pad, pad, SO_SUGGEST_PAD);
	/* the suggestion in effect for this frame, against what the player did */
	if (r->bucket)
		r->bucket--;
	suggested = best(r, frame - 1, frame);
	if (suggested && had && previous_frame == frame - 1)
		r->bucket += so_suggest_worse_cost(previous_pad, suggested->pad, pad) * SO_DRAIN;
	/* nothing at or before the latest real input is needed for predictions any more */
	for (i = 0; i < r->count; i++)
		if (r->s[i].target > frame)
			r->s[kept++] = r->s[i];
	r->count = (u8)kept;
	if (r->ignored || r->bucket <= SO_IGNORE_AFTER * SO_DRAIN)
		return 0;
	r->ignored = 1;
	r->count = 0;
	return 1;
}

u32 so_suggest_fill(SoSuggestRemote *r, s32 latest_frame, u8 *out, u32 stride)
{
	u32 k, filled = 0;
	if (r->ignored || !r->count)
		return 0;
	for (k = 0; k < SO_SUGGEST_FRAMES; k++) {
		const SoSuggestion *s = best(r, latest_frame, latest_frame + 1 + (s32)k);
		if (!s)
			continue;
		memset(out + k * stride, 0, stride);
		memcpy(out + k * stride, s->pad, SO_SUGGEST_PAD);
		filled++;
	}
	r->used += filled ? 1 : 0;
	return filled;
}

/* ---- the sending side ---------------------------------------------------------------------------------- */

static s8 clamp_s8(s8 v, s8 org)
{
	if (org > 0) {
		s8 min = (s8)(-128 + org);
		if (v < min)
			v = min;
	} else if (org < 0) {
		s8 max = (s8)(127 + org);
		if (v > max)
			v = max;
	}
	return (s8)(v - org);
}

static u8 clamp_u8(u8 v, u8 org)
{
	return (u8)((v < org ? org : v) - org);
}

static void rest_clamp(s8 *x, s8 *y)
{
	if (*x >= -2 && *x <= 2 && *y >= -2 && *y <= 2)
		*x = *y = 0;
}

int so_suggest_pad_from_si(u32 hi, u32 lo, const u8 *origin, u32 analog_mode, u8 *pad)
{
	u16 button = (u16)(hi >> 16);
	s8 sx, sy, cx, cy;
	if ((hi & 0x80000000u) || (button & 0x2000) || (analog_mode & 0x700) != 0x300)
		return -1;
	button &= 0x1F7F;	/* (PAD_ALL, and PADRead's &= ~0x80) */
	sx = clamp_s8((s8)((u8)(hi >> 8) - 128), (s8)origin[2]);
	sy = clamp_s8((s8)((u8)hi - 128), (s8)origin[3]);
	cx = clamp_s8((s8)((u8)(lo >> 24) - 128), (s8)origin[4]);
	cy = clamp_s8((s8)((u8)(lo >> 16) - 128), (s8)origin[5]);
	rest_clamp(&sx, &sy);
	rest_clamp(&cx, &cy);
	pad[0] = (u8)(button >> 8);
	pad[1] = (u8)button;
	pad[2] = (u8)sx;
	pad[3] = (u8)sy;
	pad[4] = (u8)cx;
	pad[5] = (u8)cy;
	pad[6] = clamp_u8((u8)(lo >> 8), origin[6]);
	pad[7] = clamp_u8((u8)lo, origin[7]);
	return 0;
}

/* Whether a stick's axis `v` (the other `w`) as Melee sees it, pulled back inside the unit circle of 80, is
 * at or past `t` (> 0): v * min(1, 80 / |(v, w)|) >= t, in integers (Melee's own rounding left out, as in
 * Slippi's: it matters only at a range's very edge). */
static int beyond(s32 v, s32 w, s32 t)
{
	s32 r2 = v * v + w * w;
	if (v < t)
		return 0;
	return r2 <= 6400 || v * v * 6400 >= t * t * r2;
}

/* The ranges of one stick that start an action, as a bit each */
static u32 stick_ranges(s8 x, s8 y, int c_stick)
{
	u32 m = 0;
	if (beyond(x, y, 23) || beyond(-x, y, 23))
		m |= 1;
	if (beyond(y, x, 23) || beyond(-y, x, 23))
		m |= 2;
	if (beyond(x, y, 64))
		m |= 4;		/* dash right, side smash: 0.8 */
	if (beyond(-x, y, 64))
		m |= 8;
	if (beyond(y, x, 53))
		m |= 16;	/* tap jump, up smash: 0.6625 */
	if (beyond(-y, x, c_stick ? 53 : 56))
		m |= 32;	/* crouch -0.7, down smash -0.6625 */
	return m;
}

int so_suggest_meaningful(const u8 *prev, const u8 *next)
{
	int i;
	if (((prev[0] ^ next[0]) & 0x1F) || ((prev[1] ^ next[1]) & 0x70))
		return 1;
	for (i = 6; i < 8; i++)
		if ((prev[i] > SO_TRIGGER_DEADZONE) != (next[i] > SO_TRIGGER_DEADZONE))
			return 1;
	/* (entering a range is sent; leaving one is not: the real input brings that) */
	if (stick_ranges((s8)next[2], (s8)next[3], 0) & ~stick_ranges((s8)prev[2], (s8)prev[3], 0))
		return 1;
	return (stick_ranges((s8)next[4], (s8)next[5], 1) & ~stick_ranges((s8)prev[4], (s8)prev[5], 1)) != 0;
}

void so_suggest_send_reset(SoSuggestSend *s)
{
	memset(s, 0, sizeof(*s));
}

static int suggest(SoSuggestSend *s, const u8 *pad, s32 target, SoSuggestion *out)
{
	if (s->agree_run < SO_SUGGEST_AGREE_RUN || !s->have_last || target < SO_SUGGEST_START_SYNC_FRAME ||
	    !so_suggest_meaningful(s->last, pad))
		return 0;
	s->seq++;
	memcpy(s->last, pad, SO_SUGGEST_PAD);
	out->target = target;
	out->seq = s->seq;
	memcpy(out->pad, pad, SO_SUGGEST_PAD);
	s->sent++;
	return 1;
}

int so_suggest_send_on_poll(SoSuggestSend *s, s32 frame, int how, const u8 *pad, int agree, SoSuggestion *out)
{
	s->polls++;
	if (agree < 0) {
		s->unknown++;
	} else if (agree) {
		s->agreed++;
		if (s->agree_run < 0xFFFF)
			s->agree_run++;
	} else {
		s->agree_run = 0;
	}
	if (how == 1) {
		/* a skipped frame is polled again; its input never went, so it is news if it differs */
		s->next_target = frame;
		return suggest(s, pad, frame, out);
	}
	s->next_target = frame + (how == 2 ? 2 : 1);
	memcpy(s->last, pad, SO_SUGGEST_PAD);
	s->have_last = 1;
	return 0;
}

int so_suggest_send_on_si(SoSuggestSend *s, const u8 *pad, SoSuggestion *out)
{
	return suggest(s, pad, s->next_target, out);
}
