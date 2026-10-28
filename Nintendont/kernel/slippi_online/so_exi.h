/* slippi_online/so_exi.h
 * Slippi Dolphin's online EXI device (CEXISlippi's online commands) for the
 * Nintendont kernel, so Melee running Slippi's online codeset drives the
 * client. The command layouts are in docs/EXI_DEVICE_SPEC.md (repository root).
 *
 * The game issues each online command as its own DMA write, usually followed
 * at once by a read, with interrupts off: so_exi_write prepares the answer
 * and so_exi_read hands it over, both without waiting for the network.
 *
 * The Wii plays in lockstep: 0xB0 answers SKIP until the opponent's input
 * for the frame has arrived, so the game never predicts, and never asks for
 * the savestates (0xB1/0xB2) a rollback would need.
 *
 * Called from the kernel's EXI handler only (one thread).
 */
#ifndef SO_EXI_H
#define SO_EXI_H

#include "global.h"

/* True when Melee runs Slippi's online codeset (so_client_online_game),
 * whose slot-B traffic needs no EXI interrupts. */
int so_exi_online_mode(void);

/* True for the commands so_exi handles (the rest is replay data). */
int so_exi_is_online_command(u8 cmd);
/* Bytes of a command the device reads from memory, command byte included:
 * at least the DMA length, and more for 0xC2 and the file commands (the
 * caller makes that much memory readable). */
u32 so_exi_request_size(u8 cmd);

/* Handles one command; `request` starts with the command byte. */
void so_exi_write(const u8 *request, u32 dma_len);
/* Copies the prepared answer into `out` (zero-padded to `len`) and returns
 * `len`. Returns 0, leaving `out` alone as Dolphin does, when no command
 * prepared one. The answer stays: a repeated read returns the same bytes. */
u32 so_exi_read(u8 *out, u32 len);
/* After the kernel acknowledged a transfer (the game runs on): sends what
 * the last 0xB0 queued, once the game has read its answer. The 0xB0
 * handshake itself never waits for the network: the game waits for it with
 * interrupts off, every frame. */
void so_exi_after_ack(void);

/* The direct-code history (connect-code autocomplete) is kept on the SD card,
 * as Slippi Dolphin keeps direct-codes.json: when a search added a code, this
 * writes it out (from the network thread, never in the game's EXI path). */
void so_exi_save_history(void);

/* The slot's other traffic (replay data) was written: reads are not ours
 * again until the next online command, so nothing else on the slot (the
 * memory card library's probes) ever gets an answer meant for the game. */
void so_exi_other_write(void);

/* Counters for diagnostics. */
typedef struct {
	u32 commands, reads;
	u32 b0_polls, b0_skips, b0_frames;
	u32 savestate_requests;	/* 0xB1/0xB2: never expected in lockstep */
	u32 unknown;
	u32 file_loads;
	u32 disc_files;	/* 0xD1 answered 0: the disc's file stands */
	u8 last_command;
	/* Melee's crash report (0xDF, from tools/ppc/crash_report.s): the
	 * faulting instruction and data address; 0 until a crash. */
	u32 crash_srr0, crash_dar;
	/* Diagnostics heartbeats (0xDE) and the last one's position. */
	u32 heartbeats, heartbeat_pc;
	u32 panics;	/* Melee's HSD_Panic/OSPanic reports (0xDA) */
} SoExiStats;
void so_exi_stats(SoExiStats *out);

#endif
