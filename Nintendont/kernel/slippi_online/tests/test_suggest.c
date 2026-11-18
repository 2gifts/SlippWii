/* Input suggestions, the receiving side (so_suggest.c): slippi-rust-extensions' own tests of remote.rs
 * (input-suggestions, 934f373), one for one. */
#include "test.h"
#include "so_suggest.h"
#include <string.h>

static void pad(u8 *p, u8 first)
{
	memset(p, 0, SO_SUGGEST_PAD);
	p[0] = first;
}

static void suggest(SoSuggestRemote *r, s32 target, u32 seq, u8 first)
{
	u8 p[SO_SUGGEST_PAD];
	pad(p, first);
	so_suggest_on_suggestion(r, target, seq, p);
}

static int real(SoSuggestRemote *r, s32 frame, u8 first)
{
	u8 p[SO_SUGGEST_PAD];
	pad(p, first);
	return so_suggest_on_real(r, frame, p);
}

/* the 7 slots' first bytes, the latest input (first byte `latest`) put there first, as the caller does */
static void fill(SoSuggestRemote *r, s32 latest_frame, u8 latest, u8 *firsts)
{
	u8 out[SO_SUGGEST_FRAMES * 12];
	int k;
	for (k = 0; k < SO_SUGGEST_FRAMES; k++) {
		memset(out + k * 12, 0, 12);
		out[k * 12] = latest;
	}
	so_suggest_fill(r, latest_frame, out, 12);
	for (k = 0; k < SO_SUGGEST_FRAMES; k++)
		firsts[k] = out[k * 12];
}

/* suggests a press that never happens on each of the frames, returning the first frame that started
 * ignoring (0: none) */
static s32 wrong_presses(SoSuggestRemote *r, s32 from, s32 to)
{
	s32 f, first = 0;
	for (f = from; f <= to; f++) {
		suggest(r, f, (u32)f, 1);
		if (real(r, f, 0) && !first)
			first = f;
	}
	return first;
}

void test_suggest(void)
{
	SoSuggestRemote r;
	u8 firsts[SO_SUGGEST_FRAMES];

	/* newest applicable suggestion fills each slot */
	so_suggest_reset(&r);
	real(&r, 100, 0);
	suggest(&r, 102, 1, 1);
	suggest(&r, 104, 2, 2);
	suggest(&r, 102, 0, 9);	/* (arrives out of order but is older) */
	fill(&r, 100, 0, firsts);
	{
		static const u8 want[SO_SUGGEST_FRAMES] = { 0, 1, 1, 2, 2, 2, 2 };	/* frames 101..107 */
		CHECK(!memcmp(firsts, want, sizeof(want)));
	}

	/* late and stale suggestions are rejected */
	so_suggest_reset(&r);
	real(&r, 100, 0);
	suggest(&r, 100, 1, 1);
	suggest(&r, 5000, 2, 1);
	CHECK_INT(r.count, 0);

	/* the cost counts only what the suggestion made worse */
	{
		static const u8 real_a[8] = { 0x01, 0, 10, 0, 0, 0, 0, 0 }, nothing[8] = { 0x00, 0, 10, 0, 0, 0, 0, 0 };
		static const u8 a_far[8] = { 0x01, 0, 40, 0, 0, 0, 0, 0 }, b[8] = { 0x02, 0, 10, 0, 0, 0, 0, 0 };
		static const u8 a30[8] = { 0x01, 0, 30, 0, 0, 0, 0, 0 }, a12[8] = { 0x01, 0, 12, 0, 0, 0, 0, 0 };
		CHECK_INT(so_suggest_worse_cost(nothing, a_far, real_a), 0);	/* fixing a button outweighs a farther stick */
		CHECK_INT(so_suggest_worse_cost(nothing, b, real_a), 3);	/* B instead of A is worse than nothing */
		CHECK_INT(so_suggest_worse_cost(real_a, a30, real_a), 1);	/* same buttons, farther stick */
		CHECK_INT(so_suggest_worse_cost(a30, a12, real_a), 0);	/* closer */
	}

	/* judged only at their target frame */
	so_suggest_reset(&r);
	real(&r, 100, 0);
	suggest(&r, 101, 1, 1);
	fill(&r, 100, 0, firsts);
	{
		int k, all = 1;
		for (k = 0; k < SO_SUGGEST_FRAMES; k++)
			all &= firsts[k] == 1;
		CHECK(all);
	}
	real(&r, 101, 1);
	real(&r, 102, 1);
	real(&r, 103, 0);
	real(&r, 104, 0);
	real(&r, 105, 0);
	suggest(&r, 106, 2, 1);	/* only the newest for a frame counts: pressed and released before the poll */
	suggest(&r, 106, 3, 0);
	real(&r, 106, 0);
	CHECK_INT(r.bucket, 0);

	/* wrong suggestions are ignored past the limit: the sixth wrong press passes 15 */
	so_suggest_reset(&r);
	real(&r, 100, 0);
	CHECK_INT(wrong_presses(&r, 101, 110), 106);
	suggest(&r, 112, 1000, 1);	/* later ones are dropped, predictions the latest input */
	CHECK_INT(r.count, 0);
	fill(&r, 110, 0, firsts);
	{
		int k, none = 1;
		for (k = 0; k < SO_SUGGEST_FRAMES; k++)
			none &= firsts[k] == 0;
		CHECK(none);
	}

	/* occasional misses never add up in a long game: twice the worst rate an honest game showed, for
	 * about half an hour of frames */
	{
		u32 peak = 0;
		s32 f;
		int ignored = 0;
		so_suggest_reset(&r);
		real(&r, 100, 0);
		for (f = 101; f <= 100100; f++) {
			u8 s[SO_SUGGEST_PAD], z[SO_SUGGEST_PAD];
			pad(s, 0);
			pad(z, 0);
			if (f % 400 == 0)
				s[2] = 5;
			if (f % 2000 == 1000)
				s[0] = 1;
			so_suggest_on_suggestion(&r, f, (u32)f, s);
			ignored |= so_suggest_on_real(&r, f, z);
			if (r.bucket > peak)
				peak = r.bucket;
		}
		CHECK(!ignored && peak < 5 * 200);
	}

	/* a clean stretch does not bank allowance */
	so_suggest_reset(&r);
	real(&r, 100, 0);
	{
		s32 f;
		for (f = 101; f <= 20100; f++) {
			suggest(&r, f, (u32)f, 0);
			real(&r, f, 0);
		}
	}
	CHECK_INT(wrong_presses(&r, 20101, 20110), 20106);

	/* (SlippWii's own) more suggestions than are kept: the newest stay */
	so_suggest_reset(&r);
	real(&r, 100, 0);
	{
		u32 i;
		for (i = 0; i < SO_SUGGEST_KEEP + 10; i++)
			suggest(&r, 101 + (s32)(i % 7), i + 1, (u8)(i + 1));
	}
	CHECK_INT(r.count, SO_SUGGEST_KEEP);
	fill(&r, 100, 0, firsts);
	CHECK_INT(firsts[6], SO_SUGGEST_KEEP + 10);
}

/* ---- the sending side: slippi-rust-extensions' pad.rs tests (934f373), with SI answers ---- */

/* an SI answer: buttons as PADStatus has them (0x80, always set by a controller, added), sticks and
 * triggers raw */
static void si(u16 buttons, u8 sx, u8 sy, u8 cx, u8 cy, u8 l, u8 r, u32 *hi, u32 *lo)
{
	*hi = (u32)(buttons | 0x0080) << 16 | (u32)sx << 8 | sy;
	*lo = (u32)cx << 24 | (u32)cy << 16 | (u32)l << 8 | r;
}

static int conv(u16 buttons, u8 sx, u8 sy, u8 cx, u8 cy, u8 l, u8 r, const u8 *origin, u8 *pad)
{
	u32 hi, lo;
	si(buttons, sx, sy, cx, cy, l, r, &hi, &lo);
	return so_suggest_pad_from_si(hi, lo, origin, 0x300, pad);
}

static int sent(const u8 *prev, const u8 *next)
{
	return so_suggest_meaningful(prev, next);
}

static void stick(u8 *pad, int c, s8 x, s8 y)
{
	memset(pad, 0, SO_SUGGEST_PAD);
	pad[c ? 4 : 2] = (u8)x;
	pad[c ? 5 : 3] = (u8)y;
}

void test_suggest_send(void)
{
	static const u8 centered[12] = { 0 };
	/* (the SDK's Origin: sticks centered as s8, triggers raw) */
	static const u8 origin[12] = { 0, 0, 2, (u8)-2, 0, 0, 30, 0 };
	u8 pad[SO_SUGGEST_PAD], a[SO_SUGGEST_PAD], b[SO_SUGGEST_PAD];
	u32 hi, lo;
	SoSuggestSend s;
	SoSuggestion out;
	int i, n;

	/* neutral reads as zero */
	CHECK_INT(conv(0, 128, 128, 128, 128, 0, 0, centered, pad), 0);
	memset(a, 0, sizeof(a));
	CHECK(!memcmp(pad, a, 8));
	/* buttons: A and Start set, d-pad up and L */
	conv(0x0100 | 0x1000 | 0x0008 | 0x0040, 128, 128, 128, 128, 0, 0, centered, pad);
	CHECK_INT(pad[0], 0x11);
	CHECK_INT(pad[1], 0x48);
	/* sticks centered and rest-clamped */
	conv(0, 200, 56, 129, 127, 0, 0, centered, pad);
	CHECK_INT((s8)pad[2], 72);
	CHECK_INT((s8)pad[3], -72);
	CHECK_INT(pad[4], 0);
	CHECK_INT(pad[5], 0);
	conv(0, 131, 128, 128, 128, 0, 0, centered, pad);
	CHECK_INT((s8)pad[2], 3);
	/* origin subtracted and clamped */
	conv(0, 140, 120, 128, 128, 100, 20, origin, pad);
	CHECK_INT((s8)pad[2], 10);
	CHECK_INT((s8)pad[3], -6);
	CHECK_INT(pad[6], 70);
	CHECK_INT(pad[7], 20);
	conv(0, 128, 128, 128, 128, 20, 50, origin, pad);
	CHECK_INT(pad[6], 0);
	CHECK_INT(pad[7], 50);
	conv(0, 0, 128, 128, 128, 0, 0, origin, pad);
	CHECK_INT((s8)pad[2], -128);
	/* not converted: another analog mode, an error, an origin request */
	si(0, 128, 128, 128, 128, 0, 0, &hi, &lo);
	CHECK_INT(so_suggest_pad_from_si(hi, lo, centered, 0x000, pad), -1);
	CHECK_INT(so_suggest_pad_from_si(hi | 0x80000000u, lo, centered, 0x300, pad), -1);
	CHECK_INT(so_suggest_pad_from_si(hi | 0x20000000u, lo, centered, 0x300, pad), -1);

	/* only action triggers are worth suggesting */
	stick(a, 0, 0, 0); stick(b, 0, 22, 0); CHECK(!sent(a, b));
	stick(b, 0, 0, -23); CHECK(sent(a, b));
	stick(a, 0, 23, 0); stick(b, 0, 0, 0); CHECK(!sent(a, b));
	stick(a, 0, 30, 0); stick(b, 0, 63, 0); CHECK(!sent(a, b));
	stick(b, 0, 66, 0); CHECK(sent(a, b));
	stick(a, 0, -30, 0); stick(b, 0, -64, 0); CHECK(sent(a, b));
	stick(a, 0, 70, 0); stick(b, 0, 40, 0); CHECK(!sent(a, b));
	stick(a, 0, 30, 50); stick(b, 0, 66, 50); CHECK(!sent(a, b));	/* (the unit circle: about 63.8) */
	stick(a, 0, 0, 30); stick(b, 0, 0, 53); CHECK(sent(a, b));
	stick(a, 0, 0, -30); stick(b, 0, 0, -55); CHECK(!sent(a, b));
	stick(b, 0, 0, -56); CHECK(sent(a, b));
	stick(a, 1, 30, 0); stick(b, 1, -64, 0); CHECK(sent(a, b));
	stick(a, 1, 0, -30); stick(b, 1, 0, -53); CHECK(sent(a, b));
	stick(a, 1, 0, 30); stick(b, 1, 0, 52); CHECK(!sent(a, b));
	memset(a, 0, 8); memset(b, 0, 8);
	a[6] = 42; b[6] = 43; CHECK(sent(a, b)); CHECK(sent(b, a));
	a[6] = 50; b[6] = 120; CHECK(!sent(a, b));
	memset(a, 0, 8); memset(b, 0, 8);
	b[0] = 0x01; CHECK(sent(a, b)); CHECK(sent(b, a));
	b[0] = 0x02; CHECK(sent(b, a));
	b[0] = 0; b[1] = 0x40; CHECK(sent(a, b));
	b[1] = 0x10; CHECK(sent(b, a));
	b[1] = 0x08; CHECK(!sent(a, b));	/* never the d-pad */

	/* (SlippWii's own) the sender: nothing until SO_SUGGEST_AGREE_RUN polls in a row agreed */
	so_suggest_send_reset(&s);
	memset(a, 0, 8);
	memset(b, 0, 8);
	b[0] = 0x01;	/* A */
	n = 0;
	for (i = 1; i < SO_SUGGEST_AGREE_RUN; i++)
		n += so_suggest_send_on_poll(&s, 100 + i, 0, a, 1, &out);
	n += so_suggest_send_on_si(&s, b, &out);
	CHECK_INT(n, 0);
	so_suggest_send_on_poll(&s, 100 + SO_SUGGEST_AGREE_RUN, 0, a, 1, &out);
	CHECK_INT(so_suggest_send_on_si(&s, b, &out), 1);
	CHECK_INT(out.target, 101 + SO_SUGGEST_AGREE_RUN);	/* the next poll's frame */
	CHECK_INT(out.seq, 1);
	CHECK_INT(out.pad[0], 0x01);
	/* the same again is no news; a release is */
	CHECK_INT(so_suggest_send_on_si(&s, b, &out), 0);
	CHECK_INT(so_suggest_send_on_si(&s, a, &out), 1);
	CHECK_INT(out.seq, 2);
	/* a skipped frame: polled again, its input unsent: news if it differs; an advance: two frames on */
	CHECK_INT(so_suggest_send_on_poll(&s, 200, 1, b, 1, &out), 1);
	CHECK_INT(out.target, 200);
	so_suggest_send_on_poll(&s, 200, 2, b, 1, &out);
	CHECK_INT(s.next_target, 202);
	/* a poll whose answer was not sampled in time: counted apart, the run kept */
	so_suggest_send_on_poll(&s, 202, 0, b, -1, &out);
	CHECK_INT(s.unknown, 1);
	CHECK_INT(s.agree_run >= SO_SUGGEST_AGREE_RUN, 1);
	CHECK_INT(s.next_target, 203);
	/* one poll that did not agree (another controller, the test bot, a USB pad): none for a while */
	so_suggest_send_on_poll(&s, 202, 0, a, 0, &out);
	CHECK_INT(so_suggest_send_on_si(&s, b, &out), 0);
	CHECK_INT(s.polls, SO_SUGGEST_AGREE_RUN + 4);
	/* before the game's inputs count (frame 78 on): none */
	so_suggest_send_reset(&s);
	for (i = 0; i < SO_SUGGEST_AGREE_RUN; i++)
		so_suggest_send_on_poll(&s, 10 + i, 0, a, 1, &out);
	CHECK_INT(s.next_target, 10 + SO_SUGGEST_AGREE_RUN);
	CHECK_INT(so_suggest_send_on_si(&s, b, &out), 0);
}
