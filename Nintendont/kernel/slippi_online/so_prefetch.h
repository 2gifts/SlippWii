/* slippi_online/so_prefetch.h
 * Disc prefetch between games (Wii only).
 *
 * A match's load reads about 4-8 MB from the SD card (characters, the stage,
 * sound banks), about 1.1 s of it after both players have picked, while the
 * PC opponent, done loading sooner, waits. Between games the rollback
 * engine's 16 MB of MEM2 scratch (SO_PPC_SCRATCH_SIZE) is idle: a low
 * priority thread fills it with the files the next load will read (the
 * opponent's character once picked, the last game's characters, the legal
 * stages and their sound banks, the files every match loads), and the
 * game's disc reads (ISO.c) are served from there. When the engine asks to
 * start a game (EXI 0xD9 state 0), the kernel closes it before answering;
 * it opens again once the engine is done with the scratch (its report at the
 * game's end), or at the game's end when the engine did not run.
 */
#ifndef SO_PREFETCH_H
#define SO_PREFETCH_H

#include "global.h"

/* Starts the thread (online games). It waits for the disc cache's setup to
 * say the scratch is free (so_ppc_scratch_free), then opens. */
void so_pf_init(void);
/* The engine may use the scratch from now on: drops everything, waits out a
 * read in progress. */
void so_pf_close(void);
/* The engine is done with the scratch: filling may start again. */
void so_pf_open(void);
/* A player's pick (who: 0 this console, 1 the opponent) and a stage */
void so_pf_want_char(int who, u8 character, u8 color);
void so_pf_want_stage(u16 stage);
/* ISO.c, with the FatFS lock held: the bytes at disc offset `offset` when
 * they are all here, else NULL (a miss also holds the thread back a bit,
 * so the game's own reads go first). */
const u8 *so_pf_lookup(u32 offset, u32 length);
/* Logs what it served since the last call */
void so_pf_log(void);

#endif
