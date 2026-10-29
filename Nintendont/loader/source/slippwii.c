/* SlippWii: getting Slippi Online ready before Melee starts (slippwii.h).
 *
 * The release is files to put on the SD card, nothing to build on a PC:
 *
 *   /slippwii/netplay.gct          Slippi's online code (as Slippi Dolphin runs it)
 *   /slippwii/files/<name>         Slippi's own menu files, shipped whole
 *   /slippwii/patches/<name>.diff  Slippi's patches for Melee's menu files
 *   /slippwii/patches/manifest.txt what the files must be: kind, path, size, CRC-32
 *   /slippwii/user.json            the player's Slippi account (they copy it)
 *
 * Melee's own menu files can't be shipped, so on the first start this makes
 * them here, from the disc being started: each patch applied to its disc
 * file (vcdiff.c), checked against the manifest, and written to
 * /slippwii/files/ (then /slippwii/ready.txt, so later starts skip
 * it). The kernel reads the result from the game's drive (SD or USB): when
 * that isn't the drive SlippWii started from, the files and the account are
 * put there too. A missing account or a disc the patches don't fit is
 * explained on screen, with the choice to start Melee without online play.
 */
#include <gccore.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "font.h"
#include "FPad.h"
#include "menu.h"
#include "dip.h"
#include "ff_utf8.h"
#include "diskio.h"
#include "vcdiff.h"
#include "slippwii.h"

extern FATFS *devices[2];
extern u32 Shutdown;	/* menu.c */

char SlippWii_AccountLine[64];

static const char *src_dev, *dst_dev;	/* "sd"/"usb": SlippWii's own drive, the game's */

/* ---- the screen ---------------------------------------------------------- */

#define MAX_LINES 20

static struct {
	char text[64];
	u32 color;
} lines[MAX_LINES];
static int line_count;

static void screen_clear(void)
{
	line_count = 0;
}

static void say(u32 color, const char *fmt, ...)
{
	va_list ap;
	if (line_count == MAX_LINES)
		return;
	va_start(ap, fmt);
	vsnprintf(lines[line_count].text, sizeof(lines[line_count].text), fmt, ap);
	va_end(ap);
	lines[line_count].color = color;
	line_count++;
}

static void screen_draw(const char *btn_a, const char *btn_b)
{
	int i;
	PrintBuildInfo();
	PrintButtonActions(NULL, btn_a, btn_b, NULL);
	for (i = 0; i < line_count; i++)
		PrintFormat(DEFAULT_SIZE, lines[i].color, MENU_POS_X, MENU_POS_Y + 20 * (4 + i), "%s", lines[i].text);
	GRRLIB_Render();
	ClearScreen();
}

/* Shows the screen until A (1) or B (0) is pressed. */
static int screen_ask(const char *btn_a, const char *btn_b)
{
	int frames = 0;
	while (1) {
		VIDEO_WaitVSync();
		FPAD_Update();
		if (Shutdown)
			LoaderShutdown();
		if (frames++ % 30 == 0)
			screen_draw(btn_a, btn_b);
		if (frames > 30 && FPAD_OK(0))
			return 1;
		if (frames > 30 && btn_b && FPAD_Cancel(0))
			return 0;
	}
}

/* A: start Melee without online play (networking off for this start only),
 * B: back to the Homebrew Channel. */
static void ask_offline_or_exit(void)
{
	say(BLACK, "");
	say(BLACK, "A: start Melee without online play");
	say(BLACK, "B: go back (to fix it, then start SlippWii again)");
	if (screen_ask("Start offline", "Back")) {
		ncfg->Config &= ~NIN_CFG_NETWORK;
		SlippWii_AccountLine[0] = '\0';
		return;
	}
	screen_clear();
	say(BLACK, "Going back...");
	screen_draw(NULL, NULL);
	ExitToLoader(0);
}

/* ---- files ----------------------------------------------------------------- */

static void path_of(char *out, size_t size, const char *dev, const char *rel)
{
	snprintf(out, size, "%s:/slippwii/%s", dev, rel);
}

/* The file (malloc'd), or NULL. */
static u8 *load(const char *path, u32 *size)
{
	FIL f;
	UINT got = 0;
	u8 *buf;
	if (f_open_char(&f, path, FA_READ | FA_OPEN_EXISTING) != FR_OK)
		return NULL;
	*size = (u32)f_size(&f);
	buf = memalign(32, *size + 1);
	if (buf && (f_read(&f, buf, *size, &got) != FR_OK || got != *size)) {
		free(buf);
		buf = NULL;
	}
	f_close(&f);
	if (buf)
		buf[*size] = '\0';
	return buf;
}

static int save(const char *path, const void *buf, u32 size)
{
	FIL f;
	UINT wrote = 0;
	FRESULT res;
	if (f_open_char(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
		return -1;
	res = f_write(&f, buf, size, &wrote);
	f_close(&f);
	return res == FR_OK && wrote == size ? 0 : -1;
}

static long size_of(const char *path)
{
	FIL f;
	long size;
	if (f_open_char(&f, path, FA_READ | FA_OPEN_EXISTING) != FR_OK)
		return -1;
	size = (long)f_size(&f);
	f_close(&f);
	return size;
}

/* Copies SlippWii's file to the game's drive when that is another drive and
 * its copy is missing or different. 0, or -1. */
static int copy_to_game_drive(const char *rel)
{
	char from[128], to[128];
	u8 *a, *b;
	u32 an, bn;
	int same, res = 0;
	if (strcmp(src_dev, dst_dev) == 0)
		return 0;
	path_of(from, sizeof(from), src_dev, rel);
	path_of(to, sizeof(to), dst_dev, rel);
	a = load(from, &an);
	if (!a)
		return 0;	/* nothing to copy */
	b = load(to, &bn);
	same = b && an == bn && memcmp(a, b, an) == 0;
	if (!same)
		res = save(to, a, an);
	free(a);
	free(b);
	return res;
}

static void make_dirs(const char *dev)
{
	char path[64];
	snprintf(path, sizeof(path), "%s:/slippwii", dev);
	f_mkdir_char(path);
	snprintf(path, sizeof(path), "%s:/slippwii/files", dev);
	f_mkdir_char(path);
}

/* ---- the disc ---------------------------------------------------------------- */

enum { DISC_FILE, DISC_CISO, DISC_FST, DISC_DRIVE };
#define CISO_HEADER 0x8000

typedef struct {
	int kind;
	FIL f;
	u64 base;		/* multi-game discs: the game's offset */
	unsigned cmd;		/* DISC_DRIVE: the DI command */
	u32 block_size;		/* DISC_CISO */
	u32 *blocks;		/* DISC_CISO: stored index of each block, or ~0 */
	u32 block_count;
	u8 *fst;		/* the file table */
	u32 fst_size;
	char path[300];		/* the image, or the extracted game's folder */
} Disc;

static int disc_read(Disc *d, u64 offset, void *buf, u32 len)
{
	u8 *out = buf;
	UINT got;
	if (d->kind == DISC_DRIVE) {
		while (len) {
			u32 n = len < 0x100000 ? len : 0x100000;
			ReadRealDisc(out, d->base + offset, n, d->cmd);
			out += n;
			offset += n;
			len -= n;
		}
		return 0;
	}
	if (d->kind == DISC_FILE) {
		if (f_lseek(&d->f, d->base + offset) != FR_OK || f_read(&d->f, out, len, &got) != FR_OK || got != len)
			return -1;
		return 0;
	}
	/* CISO */
	while (len) {
		u32 index = (u32)(offset / d->block_size), within = (u32)(offset % d->block_size);
		u32 n = d->block_size - within;
		if (n > len)
			n = len;
		if (index >= d->block_count || d->blocks[index] == ~0u) {
			memset(out, 0, n);
		} else {
			u64 at = CISO_HEADER + (u64)d->blocks[index] * d->block_size + within;
			if (f_lseek(&d->f, at) != FR_OK || f_read(&d->f, out, n, &got) != FR_OK || got != n)
				return -1;
		}
		out += n;
		offset += n;
		len -= n;
	}
	return 0;
}

static void disc_close(Disc *d)
{
	if (d->kind == DISC_FILE || d->kind == DISC_CISO)
		f_close(&d->f);
	free(d->blocks);
	free(d->fst);
	d->blocks = NULL;
	d->fst = NULL;
}

/* Opens the chosen game; *id gets its 8-byte header. 0, or -1. */
static int disc_open(Disc *d, unsigned CurDICMD, u32 ISOShift, u8 id[8])
{
	u8 head[0x20] ALIGNED(32);
	memset(d, 0, sizeof(*d));
	d->base = (u64)ISOShift << 2;
	if (CurDICMD) {
		d->kind = DISC_DRIVE;
		d->cmd = CurDICMD;
	} else if (!IsSupportedFileExt(ncfg->GamePath)) {
		u32 n;
		u8 *boot;
		d->kind = DISC_FST;
		snprintf(d->path, sizeof(d->path), "%s:%s", GetRootDevice(), ncfg->GamePath);
		{
			char p[340];
			snprintf(p, sizeof(p), "%ssys/boot.bin", d->path);
			boot = load(p, &n);
		}
		if (!boot || n < 8) {
			free(boot);
			return -1;
		}
		memcpy(id, boot, 8);
		free(boot);
		return 0;
	} else {
		snprintf(d->path, sizeof(d->path), "%s:%s", GetRootDevice(), ncfg->GamePath);
		if (f_open_char(&d->f, d->path, FA_READ | FA_OPEN_EXISTING) != FR_OK)
			return -1;
		d->kind = DISC_FILE;
		{
			UINT got;
			if (f_read(&d->f, head, 8, &got) != FR_OK || got != 8)
				return -1;
		}
		if (memcmp(head, "CISO", 4) == 0) {
			u8 *map = malloc(CISO_HEADER);
			u32 i, used = 0;
			UINT got;
			if (!map || f_lseek(&d->f, 0) != FR_OK || f_read(&d->f, map, CISO_HEADER, &got) != FR_OK ||
			    got != CISO_HEADER) {
				free(map);
				return -1;
			}
			d->kind = DISC_CISO;
			d->base = 0;
			d->block_size = map[4] | (map[5] << 8) | (map[6] << 16) | ((u32)map[7] << 24);
			d->block_count = CISO_HEADER - 8;
			d->blocks = malloc(d->block_count * sizeof(u32));
			if (!d->blocks || d->block_size == 0) {
				free(map);
				return -1;
			}
			for (i = 0; i < d->block_count; i++)
				d->blocks[i] = map[8 + i] == 1 ? used++ : ~0u;
			free(map);
		}
	}
	if (disc_read(d, 0, head, 0x20) < 0)
		return -1;
	memcpy(id, head, 8);
	return 0;
}

static u32 be32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/* A file of the disc (malloc'd), or NULL. */
static u8 *disc_file(Disc *d, const char *name, u32 *size)
{
	u32 count, i;
	char *names;
	if (d->kind == DISC_FST) {
		char p[400];
		snprintf(p, sizeof(p), "%sfiles/%s", d->path, name);
		return load(p, size);
	}
	if (!d->fst) {
		u8 hdr[0x20] ALIGNED(32);
		u32 offset;
		if (disc_read(d, 0x420, hdr, 0x20) < 0)
			return NULL;
		offset = be32(hdr + 4);
		d->fst_size = be32(hdr + 8);
		if (d->fst_size < 12 || d->fst_size > 0x200000)
			return NULL;
		d->fst = memalign(32, d->fst_size + 1);
		if (!d->fst || disc_read(d, offset, d->fst, d->fst_size) < 0) {
			free(d->fst);
			d->fst = NULL;
			return NULL;
		}
		d->fst[d->fst_size] = '\0';
	}
	count = be32(d->fst + 8);
	if (count == 0 || count * 12 > d->fst_size)
		return NULL;
	names = (char *)d->fst + count * 12;
	for (i = 1; i < count; i++) {
		const u8 *e = d->fst + i * 12;
		u32 name_at = be32(e) & 0xFFFFFF, at = be32(e + 4), len = be32(e + 8);
		u8 *buf;
		if (e[0])
			continue;	/* a folder */
		if (count * 12 + name_at >= d->fst_size || strcmp(names + name_at, name) != 0)
			continue;
		*size = len;
		buf = memalign(32, ALIGN_FORWARD(len, 32) + 32);
		if (buf && disc_read(d, at, buf, len) < 0) {
			free(buf);
			buf = NULL;
		}
		return buf;
	}
	return NULL;
}

/* ---- the account ------------------------------------------------------------------ */

/* A JSON string field's value, printable ASCII only (others shown as '?'). */
static int json_string(const char *json, const char *key, char *out, size_t size)
{
	char pattern[40];
	const char *p;
	size_t n = 0;
	snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	p = strstr(json, pattern);
	if (!p)
		return 0;
	p += strlen(pattern);
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':')
		p++;
	if (*p++ != '"')
		return 0;
	while (*p && *p != '"' && n + 1 < size) {
		unsigned char c = (unsigned char)*p++;
		if (c == '\\' && *p)
			c = (unsigned char)*p++;
		if (c >= 0x80) {
			while ((*p & 0xC0) == 0x80)
				p++;	/* the rest of a UTF-8 character */
			c = '?';
		} else if (c < 0x20) {
			c = '?';
		}
		out[n++] = (char)c;
	}
	out[n] = '\0';
	return n > 0;
}

/* 1: a Slippi account (name, code filled in), 0: no file, -1: not one. */
static int read_account(const char *dev, char *name, char *code)
{
	char path[64], key[8];
	u32 n;
	char *json;
	int ok;
	path_of(path, sizeof(path), dev, "user.json");
	json = (char *)load(path, &n);
	if (!json)
		return 0;
	ok = json_string(json, "uid", key, sizeof(key)) && json_string(json, "playKey", key, sizeof(key)) &&
	     json_string(json, "connectCode", code, 16);
	if (ok && !json_string(json, "displayName", name, 24))
		strcpy(name, "?");
	free(json);
	return ok ? 1 : -1;
}

static void account_help(void)
{
	say(BLACK, "Online Play needs your Slippi login: the file user.json.");
	say(BLACK, "To add it: put this SD card in your computer, open it,");
	say(BLACK, "and double-click  Copy my Slippi account to SD.bat");
	say(BLACK, "(that computer must be logged in to Slippi Launcher).");
}

static void check_account(void)
{
	char name[24], code[16];
	int state = read_account(dst_dev, name, code);
	if (state != 1 && strcmp(src_dev, dst_dev) != 0) {
		state = read_account(src_dev, name, code);
		if (state == 1)
			copy_to_game_drive("user.json");
	} else if (state == 1) {
		copy_to_game_drive("user.json");	/* a newer one on SlippWii's drive */
		read_account(dst_dev, name, code);
	}
	if (state == 1) {
		snprintf(SlippWii_AccountLine, sizeof(SlippWii_AccountLine), "Slippi account: %s (%s)", name, code);
		return;
	}
	screen_clear();
	if (state == 0) {
		say(MAROON, "SlippWii: no Slippi account on the SD card");
	} else {
		say(MAROON, "SlippWii: slippwii/user.json is not a Slippi account");
		say(BLACK, "(it may be damaged: copy it again).");
	}
	say(BLACK, "");
	account_help();
	ask_offline_or_exit();
}

/* ---- the menu files ---------------------------------------------------------------- */

typedef struct {
	int patch;		/* made from the disc, or copied */
	char path[48];		/* in /slippwii/ */
	u32 size;
	u32 crc;
} Entry;

#define MAX_ENTRIES 40

static int parse_manifest(char *text, Entry *e, int max)
{
	int n = 0;
	char *line = strtok(text, "\r\n");
	for (; line; line = strtok(NULL, "\r\n")) {
		char kind[8], path[48];
		unsigned long size, crc;
		if (line[0] == '#' || line[0] == '\0')
			continue;
		if (n == max || sscanf(line, "%7s %47s %lu %lx", kind, path, &size, &crc) != 4)
			return -1;
		e[n].patch = strcmp(kind, "patch") == 0;
		strcpy(e[n].path, path);
		e[n].size = (u32)size;
		e[n].crc = (u32)crc;
		n++;
	}
	return n;
}

static const char *base_name(const char *path)
{
	const char *slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

/* The game's drive already has every file at its size, made for this manifest. */
static int ready(const Entry *e, int n, u32 stamp)
{
	char path[128];
	u32 size;
	char *text;
	int i, ok;
	path_of(path, sizeof(path), dst_dev, "ready.txt");
	text = (char *)load(path, &size);
	ok = text && strtoul(text, NULL, 16) == stamp;
	free(text);
	for (i = 0; ok && i < n; i++) {
		path_of(path, sizeof(path), dst_dev, e[i].path);
		ok = size_of(path) == (long)e[i].size;
	}
	return ok;
}

static void files_failed(const char *file, const char *why)
{
	screen_clear();
	say(MAROON, "SlippWii could not make Slippi's online menus:");
	say(MAROON, "  %s: %.40s", file, why);
	say(BLACK, "");
	say(BLACK, "SlippWii needs an unmodified NTSC 1.02 Melee disc image");
	say(BLACK, "(GALE01, revision 2): the same one Slippi uses. Also put");
	say(BLACK, "all of SlippWii's files on the SD card again (from its zip).");
	ask_offline_or_exit();
}

static void prepare_files(unsigned int CurDICMD, u32 ISOShift)
{
	char path[128];
	u32 manifest_size, i;
	char *manifest;
	Entry e[MAX_ENTRIES];
	int n;
	u32 stamp;
	Disc disc;
	u8 id[8];
	int opened = 0;

	path_of(path, sizeof(path), src_dev, "patches/manifest.txt");
	manifest = (char *)load(path, &manifest_size);
	if (!manifest)
		return;	/* not the release layout (files made on a PC): use them as they are */
	stamp = (u32)vcdiff_crc32(0, (u8 *)manifest, manifest_size);
	n = parse_manifest(manifest, e, MAX_ENTRIES);
	free(manifest);
	if (n <= 0) {
		files_failed("manifest.txt", "damaged");
		return;
	}
	if (ready(e, n, stamp))
		return;

	make_dirs(dst_dev);
	for (i = 0; i < (u32)n; i++) {
		u8 *out = NULL;
		u32 out_size = 0;
		const char *name = base_name(e[i].path);
		const char *error = NULL;

		screen_clear();
		say(BLACK, "SlippWii: getting Slippi's online menus ready");
		say(BLACK, "from your disc (only the first time)...");
		say(BLACK, "");
		say(BLACK, "%2u/%d  %s", i + 1, n, name);
		screen_draw(NULL, NULL);

		if (e[i].patch) {
			u8 *delta, *source;
			u32 delta_size, source_size;
			long target;
			char rel[64];
			if (!opened) {
				if (disc_open(&disc, CurDICMD, ISOShift, id) < 0) {
					files_failed("the disc", "could not be read");
					return;
				}
				opened = 1;
			}
			snprintf(rel, sizeof(rel), "patches/%s.diff", name);
			path_of(path, sizeof(path), src_dev, rel);
			delta = load(path, &delta_size);
			if (!delta) {
				disc_close(&disc);
				files_failed(rel, "missing");
				return;
			}
			source = disc_file(&disc, name, &source_size);
			target = source ? vcdiff_target_size(delta, delta_size, &error) : -1;
			if (!source)
				error = "not on the disc";
			else if (target != (long)e[i].size)
				error = target < 0 ? error : "the patch does not match the manifest";
			else if (!(out = memalign(32, (u32)target + 32)))
				error = "out of memory";
			else if (vcdiff_decode(source, source_size, delta, delta_size, out, (u32)target, &error) == 0)
				out_size = (u32)target;
			free(delta);
			free(source);
		} else {
			path_of(path, sizeof(path), src_dev, e[i].path);
			out = load(path, &out_size);
			if (!out)
				error = "missing (put SlippWii's files on the SD card again)";
		}
		if (!error && (out_size != e[i].size || vcdiff_crc32(0, out, out_size) != e[i].crc))
			error = e[i].patch ? "the result is wrong: not an unmodified NTSC 1.02 disc"
					   : "damaged (put SlippWii's files on the SD card again)";
		if (!error) {
			path_of(path, sizeof(path), dst_dev, e[i].path);
			if (!e[i].patch && strcmp(src_dev, dst_dev) == 0)
				;	/* already there */
			else if (save(path, out, out_size) < 0)
				error = "could not be written to the SD card (is it full or locked?)";
		}
		free(out);
		if (error) {
			if (opened)
				disc_close(&disc);
			files_failed(name, error);
			return;
		}
	}
	if (opened)
		disc_close(&disc);
	{
		char text[16];
		snprintf(text, sizeof(text), "%08lx\n", (unsigned long)stamp);
		path_of(path, sizeof(path), dst_dev, "ready.txt");
		save(path, text, strlen(text));
	}
	FlushDevices();
}

/* ---- the entry ------------------------------------------------------------------------ */

void SlippWii_Prepare(unsigned int CurDICMD, u32 ISOShift)
{
	u8 id[8];
	Disc disc;
	SlippWii_AccountLine[0] = '\0';
	src_dev = devices[DEV_SD] ? "sd" : "usb";
	dst_dev = GetRootDevice();

	if (disc_open(&disc, CurDICMD, ISOShift, id) < 0)
		return;	/* the loader reports unreadable discs itself */
	disc_close(&disc);
	if (memcmp(id, "GALE01", 6) != 0)
		return;	/* not NTSC Melee */
	if (id[7] != 2) {
		screen_clear();
		say(MAROON, "SlippWii: this Melee is revision 1.0%d, not 1.02", id[7]);
		say(BLACK, "");
		say(BLACK, "Slippi Online needs NTSC 1.02 Melee (GALE01, revision 2),");
		say(BLACK, "the same disc image Slippi uses on a PC.");
		ask_offline_or_exit();
		return;
	}
	if (!(ncfg->Config & NIN_CFG_NETWORK))
		return;

	/* The game's drive gets SlippWii's online code and settings. */
	if (strcmp(src_dev, dst_dev) != 0) {
		make_dirs(dst_dev);
		copy_to_game_drive("netplay.gct");
		copy_to_game_drive("config.ini");
	}
	check_account();	/* first: without one there's no online play to get ready */
	if (ncfg->Config & NIN_CFG_NETWORK)
		prepare_files(CurDICMD, ISOShift);
	FlushDevices();
}
