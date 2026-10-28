/* slippi_online/so_files.h
 * The files Slippi Dolphin serves to the game over EXI: its menu and m-ex
 * files (0xD1/0xD2, from Sys/GameFiles) and the online codeset (0xD3/0xD4,
 * for Slippi's bootloader). tools/slippi_online_build.py makes them from the
 * player's disc:
 *
 *   /slippwii/netplay.gct     the codeset, loaded into memory at start-up
 *   /slippwii/files/<name>    replacement game files, read on demand
 *
 * The game asks for a file's length (0xD1) on every file load and expects
 * the answer at once, so the names and sizes are listed at start-up; a
 * file's data is read from the drive when the game loads it (0xD2).
 */
#ifndef SO_FILES_H
#define SO_FILES_H

#include "global.h"

#define SO_FILES_DIR "/slippwii/files"
#define SO_GCT_PATH "/slippwii/netplay.gct"

/* Lists the replacement files and loads the codeset (onto the online heap).
 * Returns the number of files; the codeset is optional (see so_files_gct). */
int so_files_init(void);

/* The size of the replacement for game file `name`, or 0 when the disc's
 * file stands. */
u32 so_files_size(const char *name);
/* Reads the replacement for `name` into `out` (up to `len` bytes); returns
 * the bytes read. Blocking (see so_read_file). */
u32 so_files_read(const char *name, u8 *out, u32 len);

/* The codeset as a GCT (00D0C0DE 00D0C0DE, codes, FF000000 00000000), or 0
 * bytes when there is none. */
u32 so_files_gct(const u8 **gct);

/* Keeps the most used menu files in RAM (so_platform_file_cache), for the
 * online mode only: the disc cache gives that memory up then (ISO.c). */
void so_files_preload(void);

/* Fills in, for a codeset loaded at `load_address`, the branch back at the
 * end of every C2 hook, exactly as Slippi's bootloader will write it
 * (Bootloader/main.asm, Callback_ProcessGeckoCode). The bootloader stores
 * that word through the data cache but only flushes the hooked site, so on
 * a console the CPU can fetch the placeholder (0) from memory instead and
 * crash; Dolphin hides this. With the word already in memory, both agree.
 * Returns the number of C2 codes linked, or < 0 for a malformed codeset. */
int so_gct_link(u8 *gct, u32 len, u32 load_address);

/* Removes the codes (04 and C2) that patch any of `addresses` from a GCT in
 * place; returns its new length. */
u32 so_gct_drop(u8 *gct, u32 len, const u32 *addresses, int count);

#endif
