/* Autoplay for the automated Slippi Online sweep (see glue.s): plays one
 * controller port through Slippi's online menus and games, the same way on
 * a Wii and in Dolphin, following a plan of rounds (character, stage).
 *
 * Built position independent (build_autoplay.py checks): no globals, no
 * constant data, no jump tables; floats come from the Bot struct. Addresses
 * are Melee NTSC 1.02 and Slippi's online code (slippi-ssbm-asm):
 *   0x80479D30 scene controller (major << 24 ... minor)
 *   0x804A04F0 menu flow: current menu (u8), hovered option (u16 at +2)
 *   0x804D6CF0 the port in control of the CSS (r13 - 0x49B0)
 *   0x804A0BC0 CSS cursors (pointer per port; x at +0x0C, y at +0x10)
 *   0x803F0B24 CSS icons (0x1C each; bounds l, r, u, d at +0x0C..+0x18)
 *   0x804D6CAE SSS: the hovered stage icon
 *   0x804D6643 Slippi: name entry mode (r13 - 0x505D; 1 = connect code)
 */
typedef unsigned char u8;
typedef signed char s8;
typedef unsigned short u16;
typedef unsigned int u32;

typedef struct {
	u16 button;
	s8 sx, sy, cx, cy;
	u8 l, r, aa, ab;
	s8 err;
	u8 pad;
} Pad;

#define MAGIC 0x4155544Fu	/* 'AUTO' */
#define PLAN_MAX 64

typedef struct {
	/* filled in by build_autoplay.py */
	u32 magic;		/* 0x00 */
	u8 port;		/* 0x04 the controller port to play */
	u8 plan_len;		/* 0x05 */
	u8 lras_s;		/* 0x06 seconds before quitting a game with L+R+A+Start (0: never) */
	u8 seed;		/* 0x07 */
	float one;		/* 0x08 1.0 */
	float half;		/* 0x0C 0.5 */
	u8 plan[PLAN_MAX][2];	/* 0x10 per round: CSS icon, SSS icon (0xFF: any) */
	/* state, 0x90 on */
	u32 polls;		/* 0x90 */
	u32 scene;		/* 0x94 */
	u32 scene_polls;	/* 0x98 */
	u32 rng;		/* 0x9C */
	u8 round;		/* 0xA0 games started */
	u8 step;		/* 0xA1 */
	u8 hold;		/* 0xA2 */
	u8 installed;		/* 0xA3 */
	u16 buttons;		/* 0xA4 the current in-game action */
	s8 sx, sy, cx, cy;	/* 0xA6 */
	u8 tl, tr;		/* 0xAA */
	u16 scan_t;		/* 0xAC */
	u16 pad1;		/* 0xAE */
	u32 last;		/* 0xB0 the last action (diagnostics) */
	u32 cursor;		/* 0xB4 the stage select cursor's JObj (0 until found) */
} Bot;

#define B_LEFT 0x0001
#define B_RIGHT 0x0002
#define B_DOWN 0x0004
#define B_UP 0x0008
#define B_Z 0x0010
#define B_R 0x0020
#define B_L 0x0040
#define B_A 0x0100
#define B_B 0x0200
#define B_X 0x0400
#define B_Y 0x0800
#define B_START 0x1000

#define PADREAD 0x8034DA00u

void flush_code(u32 address, u32 bytes);

static u32 rd32(u32 a) { return *(volatile u32 *)a; }
static u8 rd8(u32 a) { return *(volatile u8 *)a; }
static float rdf(u32 a) { return *(volatile float *)a; }

static int valid_ptr(u32 p)
{
	return p >= 0x80000000u && p < 0x81800000u;
}

void autoplay_install(Bot *b, u32 wrapper, u32 tramp)
{
	volatile u32 *pad = (volatile u32 *)PADREAD;
	u32 want = 0x48000000u | ((wrapper - PADREAD) & 0x03FFFFFCu);
	u32 *t = (u32 *)tramp;
	if (b->magic != MAGIC || *pad == want)
		return;
	t[0] = *pad;	/* PADRead's first instruction: position independent in both */
	t[1] = 0x48000000u | ((PADREAD + 4 - (tramp + 4)) & 0x03FFFFFCu);
	flush_code(tramp, 8);
	*pad = want;
	flush_code(PADREAD, 4);
	b->installed++;
}

static u32 next_rand(Bot *b)
{
	u32 x = b->rng ? b->rng : 0x9E3779B9u ^ b->seed;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	b->rng = x;
	return x;
}

/* A button press `period` polls apart, held 2 polls (menus react to the edge). */
static int pulse(Bot *b, u32 period, u32 phase)
{
	return (b->scene_polls + period - phase) % period < 2;
}

static void main_menu(Bot *b, Pad *o)
{
	if (rd8(0x804A04F0) == 8) {	/* Slippi's Online Play submenu */
		if (b->scene_polls % 30 == 0)
			*(volatile u16 *)0x804A04F2 = 2;	/* Direct */
		if (pulse(b, 30, 15))
			o->button = B_A;
	} else if (pulse(b, 40, 20)) {
		o->button = B_A;
	}
}

static void name_entry(Bot *b, Pad *o)
{
	/* R: fetch a suggestion from the direct-code history; Z: take it (the
	 * selector moves to Confirm); A: confirm. */
	u32 t = b->scene_polls % 120;
	if (t >= 30 && t < 32)
		o->button = B_R;
	else if (t >= 60 && t < 62)
		o->button = B_Z;
	else if (t >= 90 && t < 92)
		o->button = B_A;
}

static void css(Bot *b, Pad *o)
{
	if (b->seed == 0xEE) {
		/* test mode (tools/css_delay/): A once a second on the CSS,
		 * nothing else (the test puts the hand on the delay buttons) */
		if (pulse(b, 60, 30))
			o->button = B_A;
		return;
	}
	u8 icon = b->plan_len ? b->plan[b->round % b->plan_len][0] : 0;
	u32 port = rd8(0x804D6CF0) & 3;
	u32 cursor = rd32(0x804A0BC0 + port * 4);
	if (b->scene_polls < 40)
		return;
	if (b->step == 0 && valid_ptr(cursor)) {
		u32 e = 0x803F0B24u + icon * 0x1C;
		float x = (rdf(e + 0x0C) + rdf(e + 0x10)) * b->half;
		float y = (rdf(e + 0x14) + rdf(e + 0x18)) * b->half;
		*(volatile float *)(cursor + 0x0C) = x;
		*(volatile float *)(cursor + 0x10) = y;
		b->step = 1;
		b->hold = 0;
		return;
	}
	if (b->step == 1) {
		/* A character already picked (r13 - 0x49A9) must be put down (B)
		 * before another can be (A); B with none picked would leave. */
		u8 picked = rd8(0x803F0DFC + port * 0x24 + 0x0E);
		if (b->hold == 0 && rd8(0x804D6CF7) && picked != icon)
			b->hold = 100;
		if (b->hold >= 100 && b->hold < 102)
			o->button = B_B;
		else if (b->hold == 112 && valid_ptr(cursor))
			b->step = 0;	/* place the cursor again, then A */
		else if (b->hold < 2)
			o->button = B_A;
		if (++b->hold > 30 && b->hold < 100)
			b->step = 2;
		return;
	}
	if (b->step == 2) {
		/* Lock in only once the planned character is the one picked
		 * (door sel_icon at 0x803F0DFC + 0x24 * port + 0x0E); else again. */
		u8 picked = rd8(0x803F0DFC + port * 0x24 + 0x0E);
		b->last = (u32)picked << 24 | (u32)icon << 16 | port << 8 | b->round;
		if (picked != icon) {
			b->step = 0;
			b->scene_polls = 20;	/* back to placing the cursor soon */
			return;
		}
		if (pulse(b, 45, 0))
			o->button = B_START;
	}
}

/* The stage select cursor's JObj: the owner of the process running its
 * think function (fn_8025A310), found in Melee's process lists
 * (HSD_GObj_GObjProcHead at 0x804D7840; proc: next +4, gobj +0x10,
 * function +0x14; gobj: JObj +0x28). */
static u32 find_sss_cursor(void)
{
	u32 heads = rd32(0x804D7840), i;
	if (!valid_ptr(heads))
		return 0;
	for (i = 0; i < 32; i++) {
		u32 p = rd32(heads + i * 4), n;
		for (n = 0; valid_ptr(p) && n < 256; n++) {
			if (rd32(p + 0x14) == 0x8025A310u) {
				u32 g = rd32(p + 0x10);
				return valid_ptr(g) ? rd32(g + 0x28) : 0;
			}
			p = rd32(p + 4);
		}
	}
	return 0;
}

static void sss(Bot *b, Pad *o)
{
	/* stage icons: 0x803F06D0, 0x1C each, JObj first; 30 icons */
	u8 target = b->plan_len ? b->plan[b->round % b->plan_len][1] : 24;
	u8 hovered = rd8(0x804D6CAE);
	u32 icon;
	if (target >= 30)
		target = 24;	/* Battlefield */
	if (b->scene_polls < 90)
		return;	/* the icons slide in first: placing the cursor during it misses */
	if (hovered == target) {
		/* settled on it (several polls running): A */
		if (++b->hold > 8 && b->hold < 11)
			o->button = B_A;
		if (b->hold > 40)
			b->hold = 0;
		return;
	}
	b->hold = 0;
	if (!valid_ptr(b->cursor))
		b->cursor = find_sss_cursor();
	icon = rd32(0x803F06D0 + target * 0x1C);
	if (valid_ptr(b->cursor) && valid_ptr(icon) && b->scene_polls % 10 == 0) {
		/* the cursor's translation, to the icon's world position (JObj
		 * matrix translation at +0x50, +0x60); Melee's hit test does the rest */
		*(volatile float *)(b->cursor + 0x38) = rdf(icon + 0x50);
		*(volatile float *)(b->cursor + 0x3C) = rdf(icon + 0x60);
	}
}

static void in_game(Bot *b, Pad *o)
{
	u32 r;
	if (b->lras_s && b->scene_polls > (u32)b->lras_s * 60) {
		/* Quit the game as a player does: pause (Start), then hold L+R+A
		 * and press Start again (Melee wants a fresh press, so it repeats). */
		u32 t = b->scene_polls - (u32)b->lras_s * 60;
		if (t < 2) {
			o->button = B_START;
		} else if (t >= 30) {
			o->button = B_L | B_R | B_A;
			o->l = o->r = 255;
			if (t >= 40 && (t - 40) % 60 < 50)
				o->button |= B_START;
		}
		return;
	}
	if (b->scene_polls < 60)
		return;
	if (b->hold) {
		b->hold--;
		if (b->hold == 0)
			return;	/* one neutral poll between actions: buttons press again */
		o->button = b->buttons;
		o->sx = b->sx;
		o->sy = b->sy;
		o->cx = b->cx;
		o->cy = b->cy;
		o->l = b->tl;
		o->r = b->tr;
		return;
	}
	r = next_rand(b);
	b->buttons = 0;
	b->sx = b->sy = b->cx = b->cy = 0;
	b->tl = b->tr = 0;
	if ((r & 3) != 0) {
		/* a stick direction: one of 8, full or partway */
		u32 dir = (r >> 2) & 7, mag = (r & 0x20) ? 127 : 80;
		s8 m = (s8)mag, d = (s8)(mag * 7 / 10);
		s8 xs[8], ys[8];
		xs[0] = m; ys[0] = 0;
		xs[1] = d; ys[1] = d;
		xs[2] = 0; ys[2] = m;
		xs[3] = (s8)-d; ys[3] = d;
		xs[4] = (s8)-m; ys[4] = 0;
		xs[5] = (s8)-d; ys[5] = (s8)-d;
		xs[6] = 0; ys[6] = (s8)-m;
		xs[7] = d; ys[7] = (s8)-d;
		b->sx = xs[dir];
		b->sy = ys[dir];
	}
	switch ((r >> 8) & 15) {
	case 5: case 6: b->buttons = B_A; break;
	case 7: case 8: b->buttons = B_B; break;
	case 9: b->buttons = B_X; break;
	case 10: b->buttons = B_Y; break;
	case 11: b->buttons = B_Z; break;
	case 12: b->buttons = B_L; b->tl = 255; break;
	case 13: b->buttons = B_R; b->tr = 255; break;
	case 14: b->tl = 80; break;	/* light shield */
	case 15: b->cx = (r & 0x10000) ? 127 : -127; break;	/* smash attack */
	default: break;	/* only the stick */
	}
	b->hold = (u8)(2 + ((r >> 20) % 14));
	b->last = r;
}

static void results(Bot *b, Pad *o)
{
	if (pulse(b, 30, 15))
		o->button = (b->scene_polls / 30) & 1 ? B_START : B_A;
}

void autoplay_poll(Pad *status, Bot *b)
{
	Pad o;
	u32 scene, major, minor;
	if (b->magic != MAGIC)
		return;
	b->polls++;
	scene = rd32(0x80479D30);
	major = scene >> 24;
	minor = scene & 0xFF;
	if (scene != b->scene) {
		b->scene = scene;
		b->scene_polls = 0;
		b->step = 0;
		b->hold = 0;
		b->scan_t = 0;
		b->cursor = 0;
		if (major == 8 && minor == 2)
			b->round++;
	} else {
		b->scene_polls++;
	}
	o.button = 0;
	o.sx = o.sy = o.cx = o.cy = 0;
	o.l = o.r = o.aa = o.ab = 0;
	o.err = 0;
	o.pad = 0;
	if (major == 1) {
		if (rd8(0x804D6643) == 1)
			name_entry(b, &o);
		else
			main_menu(b, &o);
	} else if (major == 8 && minor == 0) {
		if (rd8(0x804D6643) == 1 && rd8(0x804A04F0) != 0)
			name_entry(b, &o);
		else
			css(b, &o);
	} else if (major == 8 && minor == 1) {
		sss(b, &o);
	} else if (major == 8 && minor == 2) {
		in_game(b, &o);
	} else if (major == 8 && minor == 3) {
		results(b, &o);
	}
	if (o.button & B_A)
		o.aa = 255;
	status[b->port & 3] = o;
}
