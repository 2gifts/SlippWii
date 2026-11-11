/* SlippWii diagnostics (see glue.s): input latency, measured inside the console.
 *
 * For a button press, from the controller's reading to the screen:
 *
 *   R1    the first PADRead that returns the button down (pad_hook.s, at PADRead's end:
 *         every read, whoever asked, used or not)
 *   copy  the first frame drawn whose game logic had the button (the game's own pad
 *         status, after Slippi's delay frames online), when its picture has been copied
 *         to a frame buffer (this code's entry, in HSD_VICopyXFBAsync)
 *   show  the retrace that put that frame buffer on the screen: the first retrace after
 *         which the video hardware's own register points at it (retrace_hook.s, at
 *         HSD_VIPreRetraceCB's entry: the register then holds what the retrace before
 *         wrote, so `show` is that retrace's time)
 *
 * and how long before R1 that port was last read with the button up (`gap`): the press came
 * somewhere in between, so a finger's press at any moment adds half of it on average
 * (tools/latency/analyze.py). A controller that tells the time (tools/latency/tadpole)
 * gives the press itself: its six analog values at R1 are sent on as they came.
 *
 * It also counts what the screen did in a game, from the same register: retraces that put up
 * no new frame (a frame shown twice) and frames shown two or more game frames after the one
 * before (a jump), from a game's frame 125 on: past its loading, its countdown (players can move
 * from frame 84) and its first time sync, which as on a PC may wait out frames until frame 120.
 *
 * Each press is reported over EXI 0xDA kind 5, from the game's thread at the next frame
 * drawn (the kernel logs "LAT ..." and "LATx ..."). On a CRT the light follows `show`
 * by where the line is on the screen: the top line about 1.3 ms after the retrace, the
 * middle 9 ms.
 *
 * In any scene: online games (the local player's port in the game, after the delay
 * frames), offline games and the menus (the controller's own port). Integer code only:
 * two of its three entries run in interrupts.
 *
 * Position independent (tools/pic_code.py): no globals or constant data.
 */
typedef unsigned char u8;
typedef signed char s8;
typedef unsigned short u16;
typedef unsigned int u32;

typedef struct {
	u32 state;		/* 0x00 */
	u32 seq;		/* 0x04 presses reported */
	u32 quiet;		/* 0x08 PADReads since the last press was done with */
	u32 timeouts;		/* 0x0C presses that never reached the screen (let go too soon) */
	u32 last_ok[4];		/* 0x10 each port's last PADRead with new data: the timebase */
	u16 prev[4];		/* 0x20 and its buttons */
	u32 retrace_tb;		/* 0x28 the last retrace (HSD_VIPreRetraceCB's entry) */
	u32 retrace_period;	/* 0x2C and how long after the one before */
	u16 retrace_vct;	/* 0x30 the VI's line then */
	/* the press being followed */
	u16 bit;		/* 0x32 */
	u32 t_r1, gap, t_copy, t_show;
	u32 scene, sipoll, sampling;
	u32 logic_r1, logic_copy, retrace_r1, retrace_show;
	u16 vct_r1, vct_show, reads, copies;
	u8 port, use_port, idx, delay, online, inband[6];
	/* what the screen did this scene (lat_retrace) */
	u32 tfbl, scene_key, scene_retraces;
	u32 retraces, repeats, jumps;	/* counted, with no new frame, frames shown 2 or more on */
	u32 frame_of[3];	/* the game frame in each frame buffer (lat_copy) */
	u32 shown_frame;
} Lat;

enum { IDLE, ARMED, COPIED, SHOWN };
typedef char lat_fits[sizeof(Lat) <= 0xC0 ? 1 : -1];	/* glue.s: lat */

#define FN_EXITransferBuffer ((void (*)(void *, int, int))0x800055F0)
#define SCENE 0x80479D30u		/* major << 24 ... minor */
#define LOGIC_FRAMES 0x80479D58u
#define SCENE_FRAME 0x80479D60u		/* the frame of the scene being made (Slippi's loadGlobalFrame) */
#define RETRACE_COUNT 0x804D7420u
#define PAD_COPY_STATUS 0x804C20BCu	/* HSD_PadCopyStatus[4], 0x44 each: the buttons held at + 0 */
#define VI_XFB 0x804C1D80u		/* HSD_VIData.xfb[3], 0x60 each: the buffer at + 0x58 */
#define XFB_BYTES 0x96000u		/* 640 x 480, 2 bytes a pixel */
#define VI_TFBL ((volatile u32 *)0xCC00201Cu)	/* the top field's base, as the video hardware has it */
#define VI_VCT ((volatile u16 *)0xCC00202Cu)	/* the line being drawn */
#define SI_POLL ((volatile u32 *)0xCD006430u)	/* (a Wii's SI) the lines between polls, polls a field */
#define SI_SAMPLING_RATE 0x804D740Cu	/* the SDK's setting, in ms (0: the game's own lines) */
#define SDA_BASE 0x804DB6A0u		/* r13 */
#define BUTTONS 0x1F7Fu
#define QUIET 40			/* PADReads: the presses followed are this far apart (each costs a report) */
#define TIMEOUT 90			/* retraces */

u32 tb(void);
u32 irq_off(void);
void irq_restore(u32 msr);

static u32 rd32(u32 a) { return *(volatile u32 *)a; }
static u8 rd8(u32 a) { return *(volatile u8 *)a; }

/* The port whose pad the game plays this controller's inputs on, and the delay frames
 * they wait; 0 when it can't be told. Online it is the local player's (Slippi's
 * TriggerSendInput puts the delayed inputs there), anywhere else the controller's. */
static int game_port(Lat *s, u32 port)
{
	u32 scene = rd32(SCENE), odb;
	s->scene = scene;
	s->online = (scene >> 24) == 8 && (scene & 0xFF) == 2;
	s->delay = 0;
	s->use_port = (u8)port;
	if (!s->online)
		return 1;
	odb = rd32(SDA_BASE + OFST_R13_ODB_ADDR);
	if (odb < 0x80000000u || odb >= 0x81800000u)
		return 0;
	s->use_port = rd8(odb + ODB_LOCAL_PLAYER_INDEX) & 3;
	s->delay = rd8(odb + ODB_DELAY_FRAMES);
	return 1;
}

/* PADRead's end (any thread, or an interrupt): status is its PADStatus[4] */
void lat_pad(Lat *s, const u8 *status)
{
	u32 msr = irq_off(), now = tb(), port, i;
	if (s->state == ARMED || s->state == COPIED)
		s->reads++;
	for (port = 0; port < 4; port++) {
		const u8 *p = status + port * 12;
		u32 btn = (u32)p[0] << 8 | p[1], newly = btn & ~(u32)s->prev[port] & BUTTONS;
		if ((s8)p[10] != 0)
			continue;	/* no controller, or no new answer since the last read (all zeros) */
		if (s->state == IDLE && s->quiet >= QUIET && newly && game_port(s, port) &&
		    !(rd32(PAD_COPY_STATUS + s->use_port * 0x44) & newly & (0 - newly))) {
			s->bit = (u16)(newly & (0 - newly));
			s->port = (u8)port;
			s->t_r1 = now;
			s->gap = now - s->last_ok[port];
			s->vct_r1 = *VI_VCT;
			s->sipoll = *SI_POLL;
			s->sampling = rd32(SI_SAMPLING_RATE);
			s->logic_r1 = rd32(LOGIC_FRAMES);
			s->retrace_r1 = rd32(RETRACE_COUNT);
			s->reads = s->copies = 0;
			for (i = 0; i < 6; i++)
				s->inband[i] = p[2 + i];
			s->state = ARMED;
		}
		s->prev[port] = (u16)btn;
		s->last_ok[port] = now;
	}
	if (s->state == IDLE && s->quiet < 0xFFFF)
		s->quiet++;
	irq_restore(msr);
}

static void put(u8 *buf, u32 at, u32 v)
{
	buf[at] = (u8)(v >> 24);
	buf[at + 1] = (u8)(v >> 16);
	buf[at + 2] = (u8)(v >> 8);
	buf[at + 3] = (u8)v;
}

static void report(Lat *s)
{
	u8 raw[0x80 + 32];
	u8 *buf = (u8 *)(((u32)raw + 31) & ~31u);
	int i;
	for (i = 0; i < 0x80; i++)
		buf[i] = 0;
	buf[0] = 0xDA;
	buf[1] = 5;
	put(buf, 0x04, ++s->seq);
	put(buf, 0x08, (u32)s->port << 24 | (u32)s->use_port << 16 | s->bit);
	put(buf, 0x0C, s->scene);
	put(buf, 0x10, s->gap);
	put(buf, 0x14, s->t_copy - s->t_r1);
	put(buf, 0x18, s->t_show - s->t_r1);
	put(buf, 0x1C, (u32)s->vct_r1 << 16 | s->vct_show);
	put(buf, 0x20, (u32)s->reads << 16 | s->copies);
	put(buf, 0x24, (u32)s->delay << 24 | (u32)s->online << 16 | (u32)s->idx << 8 |
		((s->retrace_show - s->retrace_r1) & 0xFF));
	put(buf, 0x28, s->sipoll);
	put(buf, 0x2C, s->sampling);
	put(buf, 0x30, s->logic_r1);
	put(buf, 0x34, s->logic_copy);
	for (i = 0; i < 6; i++)
		buf[0x38 + i] = s->inband[i];
	put(buf, 0x40, s->timeouts);
	put(buf, 0x44, s->retrace_period);
	put(buf, 0x48, s->retraces);
	put(buf, 0x4C, s->repeats);
	put(buf, 0x50, s->jumps);
	FN_EXITransferBuffer(buf, 0x80, 1);
}

/* A frame's picture has been copied to frame buffer idx (the game's thread) */
void lat_copy(Lat *s, u32 idx)
{
	u32 msr, now = tb();
	if (idx < 3)	/* the frame this picture is of, for the jumps (lat_retrace): the scene's own count, which
			 * a rollback takes back with the rest (Slippi's, online, moves at the polls: interrupts) */
		s->frame_of[idx] = rd32(SCENE_FRAME);
	if (s->state == SHOWN) {
		if (rd32(0x800055F0) >> 26 != 0)	/* (Slippi's code is in) */
			report(s);
		s->quiet = 0;
		s->state = IDLE;
		return;
	}
	msr = irq_off();
	if (s->state == ARMED) {
		if (rd32(PAD_COPY_STATUS + s->use_port * 0x44) & s->bit) {
			s->idx = (u8)idx;
			s->t_copy = now;
			s->logic_copy = rd32(LOGIC_FRAMES);
			s->state = COPIED;
		} else {
			s->copies++;
		}
	}
	if ((s->state == ARMED || s->state == COPIED) && rd32(RETRACE_COUNT) - s->retrace_r1 > TIMEOUT) {
		s->timeouts++;
		s->quiet = 0;
		s->state = IDLE;
	}
	irq_restore(msr);
}

/* A retrace, before the game's handler has chosen the next frame buffer (an interrupt) */
void lat_retrace(Lat *s)
{
	u32 now = tb(), reg = *VI_TFBL, at = reg & 0x10000000 ? (reg & 0x00FFFFFF) << 5 : reg & 0x00FFFFFF;
	u32 scene = rd32(SCENE), key = (scene >> 24) << 8 | (scene & 0xFF), i;
	if (key != s->scene_key) {
		s->scene_key = key;
		s->scene_retraces = s->retraces = s->repeats = s->jumps = 0;
		s->shown_frame = 0;
	}
	/* what the last retrace put up: nothing new, or a frame (and which) */
	/* (scene_retraces: 1 once this scene's own count is there, below 125: the last scene's stays through the
	 * loading; 2 in play) */
	if ((key & 0xFF) == 2 && rd32(SCENE_FRAME) < 125)
		s->scene_retraces = 1;
	else if (s->scene_retraces)
		s->scene_retraces = 2;
	if (s->scene_retraces == 2) {
		s->retraces++;
		if (reg == s->tfbl)
			s->repeats++;
	}
	if (reg != s->tfbl)
		for (i = 0; i < 3; i++)
			if (at - (rd32(VI_XFB + i * 0x60 + 0x58) & 0x3FFFFFFF) < XFB_BYTES) {
				if (s->scene_retraces == 2 && s->frame_of[i] - s->shown_frame >= 2 &&
				    s->frame_of[i] - s->shown_frame < 1000)
					s->jumps++;
				s->shown_frame = s->frame_of[i];
				break;
			}
	s->tfbl = reg;
	if (s->state == COPIED && s->idx < 3) {
		u32 buffer = rd32(VI_XFB + s->idx * 0x60 + 0x58) & 0x3FFFFFFF;
		if (at - buffer < XFB_BYTES) {	/* on the screen since the last retrace */
			s->t_show = s->retrace_tb;
			s->vct_show = s->retrace_vct;
			s->retrace_show = rd32(RETRACE_COUNT) - 1;	/* (this retrace is counted already) */
			s->state = SHOWN;
		}
	}
	s->retrace_period = now - s->retrace_tb;
	s->retrace_tb = now;
	s->retrace_vct = *VI_VCT;
}
