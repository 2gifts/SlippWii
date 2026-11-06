/* The online CSS's settings (see glue.s): "Delay Frames" and "－ N ＋", then
 * "Rumble" and "Show Ping", each "On"/"Off", under Slippi's User and Connect
 * Code lines, in its text style. Pointing the hand at a button lights it; A
 * changes the delay (1-9; EXI command 0xDC [step] -> [delay]), switches rumble
 * (0xDB [on] -> [on]) or the ping display (0xEF [on] -> [on, ...], see
 * tools/ping_display/); the kernel saves them all on the SD card. All of them
 * work while searching or connected, as Slippi Dolphin's settings windows do:
 * Slippi's code reads the delay at the start of each game, so a change is for
 * the next game. Only while a connect code is being typed do they not change
 * (the delay shows gray). Rumble off silences the controller in online play
 * (`motor` in glue.s, installed here at PADControlMotor).
 *
 * Built position independent (tools/pic_code.py checks): no globals, no
 * constant data; floats, colours and strings come from struct Ui. Addresses
 * are Melee NTSC 1.02 and Slippi's online code (slippi-ssbm-asm):
 *   0x80479D30 scene controller (major << 24 ... minor)
 *   0x804D6640 Slippi: the online mode (r13 - 0x5060; 2 = Direct)
 *   0x804D6643 Slippi: name entry mode (r13 - 0x505D; 1 = connect code)
 *   0x804D6CF0 the port in control of the CSS
 *   0x804A0BC0 CSS cursors (pointer per port; x at +0x0C, y at +0x10)
 *   0x804C20BC HSD_PadCopyStatus[4], 0x44 each; trigger at +0x08
 *   0x8034DEBC PADControlMotor
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
	float rumble_gap;	/* 0x74 */
	float switch_x;		/* 0x78 */
	u8 rumble;		/* 0x7C 1 on, 0 off: read by glue.s's motor */
	u8 rumble_sub;		/* 0x7D */
	u8 rumble_look;		/* 0x7E */
	u8 pad;
	char rumble_label[8];	/* 0x80 */
	char on[4];		/* 0x88 */
	char off[4];		/* 0x8C */
	u8 ping;		/* 0x90 the ping display: 1 on, 0 off */
	u8 ping_sub;		/* 0x91 */
	u8 ping_look;		/* 0x92 its colour set: 1 + hovered */
	u8 pad2;
	char ping_label[12];	/* 0x94 */
	float ping_x;		/* 0xA0 its column, from x */
} Ui;

#define A_BUTTON 0x100
#define CMD_SET_DELAY 0xDC
#define CMD_SET_RUMBLE 0xDB
#define CMD_PING_DISPLAY 0xEF
#define PING_ANSWER_SIZE 8
#define PAD_CONTROL_MOTOR 0x8034DEBCu	/* its first instruction: mflr r0 */
#define MFLR_R0 0x7C0802A6u

void flush_code(u32 address, u32 bytes);	/* glue.s */

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

/* 0xDB [on]: the rumble switch after it (0xFF: as it is); on if unknown */
static int set_rumble(Ui *u, int on)
{
#ifdef CSS_DELAY_FAKE
	return on == 0xFF ? u->rumble : on;
#else
	u8 raw[64];
	u8 *buf = (u8 *)(((u32)raw + 31) & ~31u);
	buf[0] = CMD_SET_RUMBLE;
	buf[1] = (u8)on;
	FN_EXITransferBuffer(buf, 2, 1);
	buf[0] = 1;
	FN_EXITransferBuffer(buf, 1, 0);
	(void)u;
	return buf[0] == 0 ? 0 : 1;
#endif
}

/* 0xEF [on]: the ping display's setting after it (0xFF: as it is); on if unknown */
static int set_ping(Ui *u, int on)
{
#ifdef CSS_DELAY_FAKE
	return on == 0xFF ? u->ping : on;
#else
	u8 raw[64];
	u8 *buf = (u8 *)(((u32)raw + 31) & ~31u);
	buf[0] = CMD_PING_DISPLAY;
	buf[1] = (u8)on;
	FN_EXITransferBuffer(buf, 2, 1);
	buf[0] = 1;
	FN_EXITransferBuffer(buf, PING_ANSWER_SIZE, 0);
	(void)u;
	return buf[0] == 0 ? 0 : 1;
#endif
}

static void show_delay(Ui *u, int d)
{
	u->delay = d;
	Text_UpdateSubtextContents(u->text, u->sub[1], u->fmt, d);
}

static void show_rumble(Ui *u, int on)
{
	u->rumble = (u8)on;
	Text_UpdateSubtextContents(u->text, u->rumble_sub, on ? u->on : u->off);
}

static void show_ping(Ui *u, int on)
{
	u->ping = (u8)on;
	Text_UpdateSubtextContents(u->text, u->ping_sub, on ? u->on : u->off);
}

/* PADControlMotor's first instruction -> `motor` (once; it stays). */
static void install_motor(u32 motor)
{
	u32 at = PAD_CONTROL_MOTOR, offset = motor - at;
	u32 branch = 0x48000000u | (offset & 0x03FFFFFCu);
	u32 now = rd32(at);
	if (now == branch)
		return;
	if (now != MFLR_R0 || offset + 0x02000000u >= 0x04000000u)
		return;	/* not Melee 1.02's, or out of a branch's reach: leave it */
	*(volatile u32 *)at = branch;
	flush_code(at, 4);
}

/* The rows' y: the delay label, then (+ rumble_gap) the switches' labels, Rumble
 * and Show Ping side by side: a third row would be out of the hand's reach (it
 * stops at world y -22) and in a CRT's overscan. */
static float rows_y(Ui *u)
{
	return rd8(0x804D6640) == 2 ? u->y_direct : u->y_other;
}

static float switches_y(Ui *u)
{
	return rows_y(u) + u->rumble_gap;
}

/* A switch's label and its value; the value's subtext. */
static int make_switch(Ui *u, float x, const char *label)
{
	void *t = u->text;
	float y = switches_y(u);
	int i = Text_InitializeSubtext(t, x, y, label);
	Text_UpdateSubtextSize(t, i, u->label_size, u->label_size);
	Text_ChangeTextColor(t, i, &u->gray);
	y += u->line;
	i = Text_InitializeSubtext(t, x, y, u->on);
	Text_UpdateSubtextSize(t, i, u->value_size, u->value_size);
	Text_ChangeTextColor(t, i, &u->white);
	return i;
}

void css_delay_load(Ui *u, void *think, void *motor)
{
	void *t, *gobj;
	float y, x;
	int i;
	u->text = 0;
	u->look = 0;
	u->rumble_look = 0;
	u->ping_look = 0;
	if (!online_css())
		return;
	install_motor((u32)motor);
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

	y = rows_y(u);
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

	u->rumble_sub = (u8)make_switch(u, u->x, u->rumble_label);
	show_rumble(u, set_rumble(u, 0xFF));
	u->ping_sub = (u8)make_switch(u, u->x + u->ping_x, u->ping_label);
	show_ping(u, set_ping(u, 0xFF));

	gobj = GObj_Create(4, 5, 0x80);
	if (gobj)
		GObj_AddProc(gobj, think, 4);
}

/* Whether the fingertip is within a button centred at text (bx, by). The
 * cursor's position is left of the fingertip the hand is drawn pointing
 * with (the token's spot); small buttons need the fingertip. */
static int on_button(Ui *u, u32 cursor, float bx, float by)
{
	float cx = rdf(cursor + 0x0C) + u->tip_x, cy = rdf(cursor + 0x10) + u->tip_y;
	float dx = cx - bx * u->to_world_x, dy = cy - by * u->to_world_y;
	float hw = u->half_w * u->to_world_x, hh = u->half_h * u->to_world_x;
	return dx >= -hw && dx <= hw && dy >= -hh && dy <= hh;
}

/* Which delay button (-1, +1) the hand points at, or 0. */
static int hovered(Ui *u, u32 cursor)
{
	float y = rows_y(u) + u->line + u->centre_y;
	float plus = u->x + u->centre_x;
	plus += u->gap;	/* not gap + gap: that is 2.0f * gap, a constant */
	plus += u->gap;
	if (on_button(u, cursor, u->x + u->centre_x, y))
		return -1;
	if (on_button(u, cursor, plus, y))
		return 1;
	return 0;
}

/* The hand points at the switch whose label is at x. */
static int on_switch(Ui *u, u32 cursor, float x)
{
	float y = switches_y(u);
	y += u->line;
	y += u->centre_y;
	return on_button(u, cursor, x + u->switch_x, y);
}

void css_delay_think(void *gobj, Ui *u)
{
	u32 port, cursor;
	int hover = 0, rumble_hover = 0, ping_hover = 0, locked, look, a;
	(void)gobj;
	if (!u->text)
		return;
	port = rd8(0x804D6CF0);
	if (port > 3)
		return;
	cursor = rd32(0x804A0BC0 + port * 4);
	if (valid_ptr(cursor)) {
		hover = hovered(u, cursor);
		rumble_hover = on_switch(u, cursor, u->x);
		ping_hover = on_switch(u, cursor, u->x + u->ping_x);
	}
	locked = rd8(0x804D6643) != 0;	/* typing a connect code */
	a = (rd32(0x804C20BC + port * 0x44 + 0x08) & A_BUTTON) != 0;

	look = 1 + (locked ? 3 : hover + 1);
	if (look != u->look) {
		u->look = (u8)look;
		Text_ChangeTextColor(u->text, u->sub[0], locked ? &u->gray : hover < 0 ? &u->yellow : &u->white);
		Text_ChangeTextColor(u->text, u->sub[2], locked ? &u->gray : hover > 0 ? &u->yellow : &u->white);
	}
	if (rumble_hover + 1 != u->rumble_look) {
		u->rumble_look = (u8)(rumble_hover + 1);
		Text_ChangeTextColor(u->text, u->rumble_sub, rumble_hover ? &u->yellow : &u->white);
	}
	if (ping_hover + 1 != u->ping_look) {
		u->ping_look = (u8)(ping_hover + 1);
		Text_ChangeTextColor(u->text, u->ping_sub, ping_hover ? &u->yellow : &u->white);
	}

	if (locked || !a)
		return;
	if (rumble_hover) {
		int on = set_rumble(u, !u->rumble);
		if (on != u->rumble) {
			show_rumble(u, on);
			SFX_Menu_CommonSound(2);
		}
		return;
	}
	if (ping_hover) {
		int on = set_ping(u, !u->ping);
		if (on != u->ping) {
			show_ping(u, on);
			SFX_Menu_CommonSound(2);
		}
		return;
	}
	if (hover) {
		int d = step_delay(u, hover);
		if (d && d != u->delay) {
			show_delay(u, d);
			SFX_Menu_CommonSound(2);
		}
	}
}
