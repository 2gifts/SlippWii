/* slippi_online/so_files.c
 * Replacement game files and the codeset (see so_files.h).
 */
#define SO_FILE_ID "files"
#include "so_files.h"
#include "so_alloc.h"
#include "so_platform.h"
#include "so_util.h"
#include "string.h"

#define MAX_FILES 32
#define MAX_NAME 32	/* Melee's file names are short ("GameSetup_gui.dat") */
#define MAX_GCT 0x10000	/* the bootloader reads it in one DMA */
#define BOUNCE_SIZE 0x8000

#define ATTEMPTS 3	/* the SD card occasionally fails a request (seen on hardware) */

static struct {
	char name[MAX_NAME];
	u32 size;
	void *handle;	/* open from start-up: no directory lookups while the game runs */
	const u8 *data;	/* kept in RAM (see preload), or NULL: read from the drive */
} files[MAX_FILES];
static int file_count;
static u8 *gct_data;
static u32 gct_len;
/* File reads land here first, then are copied to the game: Nintendont
 * never lets the drivers write into the game's memory (its disc reads copy
 * from a cache too). */
static u8 *bounce;

static void add_file(const char *name, u32 size, void *ctx)
{
	(void)ctx;
	if (file_count >= MAX_FILES || strlen(name) >= MAX_NAME || size == 0) {
		so_log("files: %s skipped", name);
		return;
	}
	strlcpy(files[file_count].name, name, MAX_NAME);
	files[file_count].size = size;
	files[file_count].handle = NULL;
	files[file_count].data = NULL;
	file_count++;
}

/* A GCT: the header, whole 8-byte lines, a terminator line at the end. */
static int gct_ok(const u8 *d, u32 len)
{
	return len >= 16 && len % 8 == 0 && so_get_u32(d) == 0x00D0C0DE && so_get_u32(d + 4) == 0x00D0C0DE &&
		(d[len - 8] & 0xF0) == 0xF0 && so_get_u32(d + len - 4) == 0;
}

static int find(const char *name);

/* Pokemon Stadium's transformations come from the disc and stay in RAM, as
 * Slippi Dolphin serves them (SlippiGameFileLoader): Slippi's
 * StadiumFileLoad loads them over EXI in the middle of a game. Answered
 * "from the disc", the game reads them from the disc synchronously
 * instead: about 150 ms on a console (9 polls). Melee's pad queue holds 5,
 * so inputs Slippi had already sent were dropped, and the opponent played
 * other ones: a desync at every transformation (seen on hardware). */
static const char *const disc_files[] = { "GrPs1.dat", "GrPs2.dat", "GrPs3.dat", "GrPs4.dat" };

/* Finds the disc_files in the disc's file table and reads them into the
 * cache; returns the bytes used. */
static u32 preload_disc_files(u8 *cache, u32 size)
{
	u8 header[0x40] __attribute__((aligned(32)));
	u8 *fst;
	u32 fst_offset, fst_size, count, i, used = 0;
	int kept = 0;
	if (so_disc_read(0x400, header, sizeof(header)) < 0)
		return 0;
	fst_offset = so_get_u32(header + 0x24);
	fst_size = so_get_u32(header + 0x28);
	if (fst_size < 12 || fst_size > 0x40000 || !(fst = (u8 *)so_malloc(fst_size))) {
		so_log("files: no disc file table (%u bytes)", (unsigned)fst_size);
		return 0;
	}
	if (so_disc_read(fst_offset, fst, fst_size) == 0 && (count = so_get_u32(fst + 8)) * 12 < fst_size) {
		const char *names = (const char *)fst + count * 12;
		for (i = 1; i < count; i++) {
			const u8 *e = fst + i * 12;
			u32 name = so_get_u32(e) & 0xFFFFFF, offset = so_get_u32(e + 4), len = so_get_u32(e + 8), n;
			if (e[0] != 0 || count * 12 + name >= fst_size)
				continue;	/* a directory */
			for (n = 0; n < sizeof(disc_files) / sizeof(disc_files[0]); n++) {
				if (strcmp(names + name, disc_files[n]) != 0)
					continue;
				if (file_count >= MAX_FILES || used + len > size || find(disc_files[n]) >= 0 ||
				    so_disc_read(offset, cache + used, len) < 0) {
					so_log("files: %s (%u bytes) not kept in RAM", disc_files[n], (unsigned)len);
					break;
				}
				strlcpy(files[file_count].name, disc_files[n], MAX_NAME);
				files[file_count].size = len;
				files[file_count].handle = NULL;
				files[file_count].data = cache + used;
				file_count++;
				used = (used + len + 31) & ~31u;
				kept++;
				so_log("files: %s from the disc kept in RAM (%u bytes)", disc_files[n], (unsigned)len);
			}
		}
	}
	so_free(fst);
	if (kept != 4)
		so_log("files: only %d of Stadium's 4 transformations kept in RAM", kept);
	return used;
}

/* The game loads a file over EXI in one transfer with its interrupts off,
 * so the audio loops its last buffer for as long as the transfer takes
 * (a buzz entering and leaving the online menus: 200-340 ms from the SD
 * card for MnExtAll.usd and MnMaAll.usd). The menu files are kept in RAM
 * instead, read once at start-up, most used first, while they fit; the
 * rest are read from the drive when loaded. */
void so_files_preload(void)
{
	static const char *const order[] = {
		"MnMaAll.usd", "MnExtAll.usd", "MnSlMap.usd", "slpCSS.dat", "SdMenu.usd", "SlippiCSS.dat",
		"SdSlChr.usd", "MxScn.dat", "GameSetup.dat",
	};
	u32 size, used = 0, n;
	u8 *cache = (u8 *)so_platform_file_cache(&size);
	int count = 0;
	if (!cache || !bounce)
		return;
	used = preload_disc_files(cache, size);	/* first: they matter to the game itself */
	for (n = 0; n < sizeof(order) / sizeof(order[0]); n++) {
		int i = find(order[n]);
		u32 len;
		if (i < 0)
			continue;
		len = files[i].size;
		if (used + len > size)
			continue;
		if (so_file_read_at(files[i].handle, 0, cache + used, len, bounce, BOUNCE_SIZE) != (int)len) {
			so_log("files: %s could not be kept in RAM", files[i].name);
			continue;
		}
		files[i].data = cache + used;
		used = (used + len + 31) & ~31u;
		count++;
	}
	so_log("files: %d kept in RAM (%u KB)", count, (unsigned)(used / 1024));
}

int so_files_init(void)
{
	int res;
	file_count = 0;
	so_list_dir(SO_FILES_DIR, add_file, NULL);
	if (!bounce)
		bounce = (u8 *)so_malloc(BOUNCE_SIZE);
	/* Open every file now. One that will not open is left out, so the game
	 * loads the disc's version rather than nothing. */
	{
		int i, kept = 0;
		for (i = 0; i < file_count; i++) {
			char path[sizeof(SO_FILES_DIR) + MAX_NAME + 1];
			int attempt, error = 0;
			so_snprintf(path, sizeof(path), "%s/%s", SO_FILES_DIR, files[i].name);
			for (attempt = 0; attempt < ATTEMPTS && !files[i].handle; attempt++)
				files[i].handle = so_file_open(path, &error);
			if (!files[i].handle) {
				so_log("files: %s will not open (FatFS %d); the disc's file stands", path, error);
				continue;
			}
			files[kept++] = files[i];
		}
		file_count = kept;
	}
	gct_data = (u8 *)so_malloc(MAX_GCT);
	res = gct_data ? so_read_file(SO_GCT_PATH, gct_data, MAX_GCT) : -1;
	if (res > 0 && (u32)res < MAX_GCT && gct_ok(gct_data, (u32)res)) {
		/* "Normal Lag Reduction" is Dolphin-only: it copies frames to display
		 * buffer -1 (harmless where the copy never reaches RAM; on a console
		 * the GPU's write hangs the bus, which froze Melee's boot at its
		 * first frame). It changes no game logic, only frame pacing.
		 *
		 * "Polling Drift Fix + VB" stays: online play needs exactly one pad
		 * poll per engine frame. Without it a Stadium transformation once
		 * left the inputs the Wii sent 4 polls behind the ones it played
		 * (its replay against Dolphin's: a desync at frame 3761). */
		static const u32 dolphin_only[] = {
			0x803761EC, 0x80376238,	/* Normal Lag Reduction [Hannes Mann] */
		};
		u32 before = (u32)res;
		gct_len = so_gct_drop(gct_data, before, dolphin_only, sizeof(dolphin_only) / sizeof(dolphin_only[0]));
		if (gct_len != before)
			so_log("files: dropped Dolphin-only Normal Lag Reduction (%u bytes)", (unsigned)(before - gct_len));
	} else {
		if (res >= 0)
			so_log("files: %s is not a usable codeset (%d bytes)", SO_GCT_PATH, res);
		so_free(gct_data);
		gct_data = NULL;
		gct_len = 0;
	}
	so_log("files: %d replacement files, codeset %u bytes", file_count, (unsigned)gct_len);
	return file_count;
}

static int find(const char *name)
{
	int i;
	for (i = 0; i < file_count; i++)
		if (strcmp(files[i].name, name) == 0)
			return i;
	return -1;
}

u32 so_files_size(const char *name)
{
	int i = find(name);
	return i < 0 ? 0 : files[i].size;
}

u32 so_files_read(const char *name, u8 *out, u32 len)
{
	int i = find(name), got = -1, attempt;
	u32 want;
	if (i < 0 || !bounce)
		return 0;
	want = len < files[i].size ? len : files[i].size;
	if (files[i].data) {
		memcpy(out, files[i].data, want);
		return want;
	}
	for (attempt = 0; attempt < ATTEMPTS; attempt++) {
		got = so_file_read_at(files[i].handle, 0, out, want, bounce, BOUNCE_SIZE);
		if (got == (int)want)
			break;
		so_log("files: %s: read %d of %u bytes (attempt %d)", files[i].name, got, (unsigned)want, attempt + 1);
	}
	return got < 0 ? 0 : (u32)got;
}

/* Copies toward the start of a buffer (dst <= src), as so_gct_drop needs;
 * the kernel's C library has no memmove. */
static void move_down(u8 *dst, const u8 *src, u32 n)
{
	while (n--)
		*dst++ = *src++;
}

/* The size of the Gecko code at `gct + off` (as the bootloader steps). */
static u32 code_size(const u8 *gct, u32 off)
{
	u32 word = so_get_u32(gct + off), value = so_get_u32(gct + off + 4), type = (word >> 24) & 0xFE;
	if (type == 0xC0 || type == 0xC2)
		return 8 + value * 8;
	if (type == 0x06)
		return 8 + ((value + 7) & ~7u);
	if (type == 0x08)
		return 16;
	return 8;
}

u32 so_gct_drop(u8 *gct, u32 len, const u32 *addresses, int count)
{
	u32 in = 8, out = 8;
	while (in + 8 <= len) {
		u32 word = so_get_u32(gct + in), size = code_size(gct, in), type = (word >> 24) & 0xFE;
		int i, drop = 0;
		if (size > len - in)
			break;
		if ((word & 0xF0FFFFFF) == 0xF0000000 && so_get_u32(gct + in + 4) == 0) {
			move_down(gct + out, gct + in, 8);	/* the terminator */
			return out + 8;
		}
		if (type == 0x04 || type == 0xC2)
			for (i = 0; i < count; i++)
				if ((word & 0x01FFFFFF) == (addresses[i] & 0x01FFFFFF))
					drop = 1;
		if (!drop) {
			move_down(gct + out, gct + in, size);
			out += size;
		}
		in += size;
	}
	return len;	/* malformed: leave it as it was */
}

int so_gct_link(u8 *gct, u32 len, u32 load_address)
{
	u32 off = 8;	/* the bootloader processes the codes from buffer + 8 */
	int linked = 0;
	while (off + 8 <= len) {
		u32 word = so_get_u32(gct + off), value = so_get_u32(gct + off + 4), next = 8;
		u32 type = (word >> 24) & 0xFE;
		if (type == 0xC0 || type == 0xC2) {
			next = 8 + value * 8;
		} else if (type == 0x06) {
			next = 8 + ((value + 7) & ~7u);
		} else if (type == 0x08) {
			next = 16;
		} else if ((word & 0xF0FFFFFF) == 0xF0000000 && value == 0) {
			return linked;	/* the end of the list */
		}
		if (next > len - off)
			return -1;
		if (type == 0xC2 && value) {
			/* As the bootloader computes it: a branch from the last
			 * word of the code back to the instruction after the hook. */
			u32 code = load_address + off, target = 0x80000000u | (word & 0x01FFFFFF);
			u32 site = code + value * 8;
			u32 branch = (((target + 4 - site) & 0x03FFFFFC) | 0x48000000) - 4;
			so_put_u32(gct + off + value * 8 + 4, branch);
			linked++;
		}
		off += next;
	}
	return -1;	/* no terminator */
}

u32 so_files_gct(const u8 **gct)
{
	*gct = gct_data;
	return gct_len;
}
