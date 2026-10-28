/* The online CSS's delay setting (see glue.s): "Delay Frames" and "－ N ＋"
 * under Slippi's User and Connect Code lines, in its text style. Pointing
 * the hand at － or ＋ lights it; A changes the delay (1-9) through the
 * kernel (EXI command 0xDC [step] -> [delay], saved on the SD card for the
 * next game). Locked (gray) while searching or connected, and while a
 * connect code is being typed.
 *
 * Built position independent (tools/pic_code.py checks): no globals, no
 * constant data; floats, colours and strings come from struct Ui. Addresses
 * are Melee NTSC 1.02 and Slippi's online code (slippi-ssbm-asm):
 *   0x80479D30 scene controller (major << 24 ... minor)
 *   0x804D6640 Slippi: the online mode (r13 - 0x5060; 2 = Direct)
 *   0x804D6643 Slippi: name entry mode (r13 - 0x505D; 1 = connect code)
 *   0x80005614 Slippi: CSS data -> its match state buffer -> connection
 *              state (0 = idle)
 *   0x804D6CF0 the port in control of the CSS
 *   0x804A0BC0 CSS cursors (pointer per port; x at +0x0C, y at +0x10)
 *   0x804C20BC HSD_PadCopyStatus[4], 0x44 each; trigger at +0x08
 */
typedef unsigned char u8;
typedef signed char s8;
typedef unsigned int u32;

typedef struct {
	void *text;		/* 0x00 */
	int delay;		/* 0x04 */
	u8 sub[3];		/* 0x08 minus, value, plus */
	u8 look;		/* 0x0B the colours set: 1 + hovered button + 3 * locked */
	float x;		/* 0x0C */
	float y_direct;		/* 0x10 */
	float y_other;		/* 0x14 */
	float line;		/* 0x18 */
	float gap;		/* 0x1C */
	float label_size;	/* 0x20 */
	float value_size;	/* 0x24 */
	float scale;		/* 0x28 */
	float to_world_x;	/* 0x2C */
	float to_world_y;	/* 0x30 */
	float centre_x;		/* 0x34 */
	float centre_y;		/* 0x38 */
	float half_w;		/* 0x3C */
	float half_h;		/* 0x40 */
	float tip_x;		/* 0x44 */
	float tip_y;		/* 0x48 */
	u32 gray;		/* 0x4C */
	u32 white;		/* 0x50 */
	u32 yellow;		/* 0x54 */
	char label[16];		/* 0x58 */
	char minus[4];		/* 0x68 */
	char plus[4];		/* 0x6C */
	char fmt[4];		/* 0x70 */
} Ui;

#define A_BUTTON 0x100
#define CMD_SET_DELAY 0xDC

/* SisLib text (Slippi's names) and the rest */
#define Text_CreateStruct ((void *(*)(int, int))0x803A6754)
#define Text_InitializeSubtext ((int (*)(void *, float, float, const char *, ...))0x803A6B98)
#define Text_UpdateSubtextContents ((int (*)(void *, int, const char *, ...))0x803A70A0)
#define Text_ChangeTextColor ((void (*)(void *, int, u32 *))0x803A74F0)
#define Text_UpdateSubtextSize ((void (*)(void *, int, float, float))0x803A7548)
#define GObj_Create ((void *(*)(int, int, int))0x803901F0)
#define GObj_AddProc ((void *(*)(void *, void *, int))0x8038FD54)
#define FN_EXITransferBuffer ((void (*)(void *, int, int))0x800055F0)
#define SFX_Menu_CommonSound ((void (*)(int))0x80024030)

static u32 rd32(u32 a) { return *(volatile u32 *)a; }
static u8 rd8(u32 a) { return *(volatile u8 *)a; }
static float rdf(u32 a) { return *(volatile float *)a; }

static int valid_ptr(u32 p)
{
	return p >= 0x80000000u && p < 0x81800000u;
}

static int online_css(void)
{
	return rd8(0x80479D30) == 8 && rd8(0x80479D33) == 0;
}

/* Not searching or connected, and not typing a connect code. */
static int idle(void)
{
	u32 cssdt = rd32(0x80005614), msrb;
	if (rd8(0x804D6643) != 0 || !valid_ptr(cssdt))
		return 0;
	msrb = rd32(cssdt);
	return valid_ptr(msrb) && rd8(msrb) == 0;
}

/* 0xDC [step]: the delay after the step (0: as it is) */
static int step_delay(Ui *u, int step)
{
#ifdef CSS_DELAY_FAKE
	/* tests in Dolphin, whose Slippi device has no 0xDC: step it here */
	int d = u->delay + step;
	return d < 1 ? 1 : d > 9 ? 9 : d;
#else
	u8 raw[64];
	u8 *buf = (u8 *)(((u32)raw + 31) & ~31u);
	buf[0] = CMD_SET_DELAY;
	buf[1] = (u8)step;
	FN_EXITransferBuffer(buf, 2, 1);
	buf[0] = 0;
	FN_EXITransferBuffer(buf, 1, 0);
	(void)u;
	return buf[0] >= 1 && buf[0] <= 9 ? buf[0] : 0;
#endif
}

static void show_delay(Ui *u, int d)
{
	u->delay = d;
	Text_UpdateSubtextContents(u->text, u->sub[1], u->fmt, d);
}

void css_delay_load(Ui *u, void *think)
{
	void *t, *gobj;
	float y, x;
	int i;
	u->text = 0;
	u->look = 0;
	if (!online_css())
		return;
	t = Text_CreateStruct(0, 0);
	if (!t)
		return;
	/* as Slippi's: close kerning, left aligned, Z 0, scaled down */
	*((u8 *)t + 0x49) = 1;
	*((u8 *)t + 0x4A) = 0;
	*(u32 *)((u8 *)t + 0x08) = 0;	/* 0.0f */
	*(float *)((u8 *)t + 0x24) = u->scale;
	*(float *)((u8 *)t + 0x28) = u->scale;
	u->text = t;

	y = rd8(0x804D6640) == 2 ? u->y_direct : u->y_other;
	i = Text_InitializeSubtext(t, u->x, y, u->label);
	Text_UpdateSubtextSize(t, i, u->label_size, u->label_size);
	Text_ChangeTextColor(t, i, &u->gray);
	y += u->line;
	x = u->x;
	u->sub[0] = (u8)Text_InitializeSubtext(t, x, y, u->minus);
	x += u->gap;
	u->sub[1] = (u8)Text_InitializeSubtext(t, x, y, u->fmt, 0);
	x += u->gap;
	u->sub[2] = (u8)Text_InitializeSubtext(t, x, y, u->plus);
	for (i = 0; i < 3; i++) {
		Text_UpdateSubtextSize(t, u->sub[i], u->value_size, u->value_size);
		Text_ChangeTextColor(t, u->sub[i], &u->white);
	}
	show_delay(u, step_delay(u, 0));

	gobj = GObj_Create(4, 5, 0x80);
	if (gobj)
		GObj_AddProc(gobj, think, 4);
}

/* Which button (-1, +1) the hand points at, or 0. The cursor's position
 * is left of the fingertip the hand is drawn pointing with (the token's
 * spot); small buttons need the fingertip. */
static int hovered(Ui *u, u32 cursor)
{
	float cx = rdf(cursor + 0x0C) + u->tip_x, cy = rdf(cursor + 0x10) + u->tip_y;
	float y = (rd8(0x804D6640) == 2 ? u->y_direct : u->y_other) + u->line + u->centre_y;
	float dy = (cy - y * u->to_world_y);
	float hw = u->half_w * u->to_world_x, hh = u->half_h * u->to_world_x;
	int i;
	if (dy < -hh || dy > hh)
		return 0;
	for (i = 0; i < 2; i++) {
		float bx = u->x + u->centre_x, dx;
		if (i) {
			bx += u->gap;	/* not gap + gap: that is 2.0f * gap, a constant */
			bx += u->gap;
		}
		dx = cx - bx * u->to_world_x;
		if (dx >= -hw && dx <= hw)
			return i ? 1 : -1;
	}
	return 0;
}

void css_delay_think(void *gobj, Ui *u)
{
	u32 port, cursor;
	int hover = 0, locked, look;
	(void)gobj;
	if (!u->text)
		return;
	port = rd8(0x804D6CF0);
	if (port > 3)
		return;
	cursor = rd32(0x804A0BC0 + port * 4);
	if (valid_ptr(cursor))
		hover = hovered(u, cursor);
	locked = !idle();
	look = 1 + (locked ? 3 : hover + 1);
	if (look != u->look) {
		u->look = (u8)look;
		Text_ChangeTextColor(u->text, u->sub[0], locked ? &u->gray : hover < 0 ? &u->yellow : &u->white);
		Text_ChangeTextColor(u->text, u->sub[2], locked ? &u->gray : hover > 0 ? &u->yellow : &u->white);
	}
	if (locked || !hover || !(rd32(0x804C20BC + port * 0x44 + 0x08) & A_BUTTON))
		return;
	{
		int d = step_delay(u, hover);
		if (d && d != u->delay) {
			show_delay(u, d);
			SFX_Menu_CommonSound(2);
		}
	}
}
