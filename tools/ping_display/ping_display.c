/* The ping in an online game's top left corner (see glue.s), as Slippi Dolphin
 * shows it with "Show NetPlay Ping" on: "Ping: N" in cyan with a black shadow,
 * updated once a second (SLIPPI_PING_DISPLAY_INTERVAL), N being the last pad's
 * round trip in ms. Against several opponents: "Ping: A | B | C".
 *
 * It draws on the HUD canvas Slippi's online code made for its "Delay: Nf".
 * The kernel keeps the setting (the online CSS's switch, tools/css_delay/) and
 * measures the pings (EXI 0xEF). With the setting off nothing is made at all,
 * so a game costs what it did without this.
 *
 * Built position independent (tools/pic_code.py checks): no globals, no
 * constant data; floats, colours and strings come from struct Ui. Addresses
 * are Melee NTSC 1.02 and Slippi's online code (slippi-ssbm-asm):
 *   0x80479D30 scene controller (major << 24 ... minor)
 *   0x804D6CBC Slippi: the online data buffer (r13 - 0x49E4)
 *   +0xACE     its HUD canvas's id (ODB_HUD_CANVAS, unaligned)
 *   +0xAD2     its HUD text (ODB_HUD_TEXT_STRUCT): there once Slippi made the HUD
 */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

#define SCENE_MAJOR_ONLINE 8
#define SCENE_MINOR_IN_GAME 2
#define ODB_POINTER 0x804D6CBCu
#define ODB_HUD_CANVAS 0xACE
#define ODB_HUD_TEXT_STRUCT 0xAD2
#define CMD_PING_DISPLAY 0xEF
#define PING_READ 0xFF
#define PING_NONE 0xFFFF
#define PING_ANSWER_SIZE 8
#define REMOTE_MAX 3
#define HUD_FONT 2		/* Slippi's HUD text uses this font set */
#define INTERVAL_FRAMES 60	/* SLIPPI_PING_DISPLAY_INTERVAL */

/* HSD_Text */
#define TEXT_Z 0x08
#define TEXT_SCALE_X 0x24
#define TEXT_SCALE_Y 0x28
#define TEXT_KERNING 0x49
#define TEXT_ALIGNMENT 0x4A
#define TEXT_HIDDEN 0x4D

typedef struct {
	void *text;		/* 0x00 */
	u16 tick;		/* 0x04 frames since the last read */
	u8 shadow_sub;		/* 0x06 */
	u8 front_sub;		/* 0x07 */
	float x;		/* 0x08 */
	float y;		/* 0x0C */
	float shadow;		/* 0x10 the shadow's offset, both ways */
	float size;		/* 0x14 */
	float scale;		/* 0x18 Slippi's HUD text scale */
	u32 cyan;		/* 0x1C */
	u32 black;		/* 0x20 */
	float team_size;	/* 0x24 several pings: small enough to stay clear of the timer */
	char fmt[3][24];	/* 0x28 by the number of opponents */
} Ui;

/* SisLib text (Slippi's names) and the rest */
#define Text_CreateStruct ((void *(*)(int, int))0x803A6754)
#define Text_InitializeSubtext ((int (*)(void *, float, float, const char *, ...))0x803A6B98)
#define Text_UpdateSubtextContents ((int (*)(void *, int, const char *, ...))0x803A70A0)
#define Text_ChangeTextColor ((void (*)(void *, int, u32 *))0x803A74F0)
#define Text_UpdateSubtextSize ((void (*)(void *, int, float, float))0x803A7548)
#define GObj_Create ((void *(*)(int, int, int))0x803901F0)
#define GObj_AddProc ((void *(*)(void *, void *, int))0x8038FD54)
#define FN_EXITransferBuffer ((void (*)(void *, int, int))0x800055F0)

static u32 rd32(u32 a) { return *(volatile u32 *)a; }
static u8 rd8(u32 a) { return *(volatile u8 *)a; }

static int valid_ptr(u32 p)
{
	return p >= 0x80000000u && p < 0x81800000u;
}

/* 0xEF: the answer, in the cache-aligned part of raw */
static u8 *read_ping(u8 *raw)
{
	u8 *buf = (u8 *)(((u32)raw + 31) & ~31u);
#ifdef PING_DISPLAY_FAKE
	/* tests in Dolphin, whose Slippi device has no 0xEF: every width the text
	 * can take, from the timebase (40.5 MHz) */
	u32 tb, s;
	__asm__ volatile("mftb %0" : "=r"(tb));
	s = tb / 40500000u;
	buf[0] = 1;
	buf[1] = (u8)((s / 6) % 3 + 1);
	buf[2] = 0;
	buf[3] = (u8)(3 + (s % 5) * 37);
	buf[4] = buf[5] = buf[6] = buf[7] = 0;
	buf[5] = 88;
	buf[7] = 123;
#else
	buf[0] = CMD_PING_DISPLAY;
	buf[1] = PING_READ;
	FN_EXITransferBuffer(buf, 2, 1);
	FN_EXITransferBuffer(buf, PING_ANSWER_SIZE, 0);
#endif
	return buf;
}

void ping_load(Ui *u, void *think)
{
	u8 raw[64];
	u32 odb;
	void *t, *gobj;
	u->text = 0;
	if (rd8(0x80479D30) != SCENE_MAJOR_ONLINE || rd8(0x80479D33) != SCENE_MINOR_IN_GAME)
		return;
	odb = rd32(ODB_POINTER);
	if (!valid_ptr(odb) || !valid_ptr(rd32(odb + ODB_HUD_TEXT_STRUCT)))
		return;
	if (!read_ping(raw)[0])
		return;
	t = Text_CreateStruct(HUD_FONT, rd32(odb + ODB_HUD_CANVAS));
	if (!t)
		return;
	*((u8 *)t + TEXT_KERNING) = 1;		/* close, as Slippi's */
	*((u8 *)t + TEXT_ALIGNMENT) = 0;	/* left */
	*(u32 *)((u8 *)t + TEXT_Z) = 0;		/* 0.0f */
	*(float *)((u8 *)t + TEXT_SCALE_X) = u->scale;
	*(float *)((u8 *)t + TEXT_SCALE_Y) = u->scale;
	*((u8 *)t + TEXT_HIDDEN) = 1;		/* until the first ping */
	u->text = t;
	u->tick = 0;

	/* the widest it can be, so a change never needs the text heap */
	u->shadow_sub = (u8)Text_InitializeSubtext(t, u->x + u->shadow, u->y + u->shadow, u->fmt[REMOTE_MAX - 1], 9999, 9999, 9999);
	u->front_sub = (u8)Text_InitializeSubtext(t, u->x, u->y, u->fmt[REMOTE_MAX - 1], 9999, 9999, 9999);
	Text_UpdateSubtextSize(t, u->shadow_sub, u->size, u->size);
	Text_UpdateSubtextSize(t, u->front_sub, u->size, u->size);
	Text_ChangeTextColor(t, u->shadow_sub, &u->black);
	Text_ChangeTextColor(t, u->front_sub, &u->cyan);

	gobj = GObj_Create(14, 15, 0);	/* as Slippi's other HUD objects */
	if (gobj)
		GObj_AddProc(gobj, think, 0);
}

void ping_think(void *gobj, Ui *u)
{
	u8 raw[64], *ping;
	int remotes, shown[REMOTE_MAX], i;
	(void)gobj;
	if (!u->text || ++u->tick < INTERVAL_FRAMES)
		return;
	u->tick = 0;
	ping = read_ping(raw);
	remotes = ping[1];
	if (remotes > REMOTE_MAX)
		remotes = REMOTE_MAX;
	for (i = 0; i < REMOTE_MAX; i++)
		shown[i] = (ping[2 + i * 2] << 8) | ping[3 + i * 2];
	if (!remotes || shown[0] == PING_NONE)
		return;	/* no pad acked yet: hidden, or as it was */
	for (i = 0; i < remotes; i++)
		if (shown[i] == PING_NONE)
			shown[i] = 0;
	Text_UpdateSubtextContents(u->text, u->shadow_sub, u->fmt[remotes - 1], shown[0], shown[1], shown[2]);
	Text_UpdateSubtextContents(u->text, u->front_sub, u->fmt[remotes - 1], shown[0], shown[1], shown[2]);
	if (remotes > 1) {
		Text_UpdateSubtextSize(u->text, u->shadow_sub, u->team_size, u->team_size);
		Text_UpdateSubtextSize(u->text, u->front_sub, u->team_size, u->team_size);
	}
	*((u8 *)u->text + TEXT_HIDDEN) = 0;
}
