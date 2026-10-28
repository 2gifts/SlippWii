/* slippi_online/so_alloc.h
 * Heap for Slippi Online: a fixed MEM2 arena (512 KB freed from the replay
 * ring buffer, see SlippiMemory.c), separate from the IOS heap the rest of
 * the kernel shares. First fit with coalescing; every block is 32-byte
 * aligned, as IOS buffers must be. Thread-safe.
 *
 * The arena lies in the Triforce DIMM region that DIinit() zeroes at boot, so
 * so_alloc_init must run after DI_INIT (main.c starts the client after
 * SlippiMemoryInit for that reason). Walks validate every block header: a
 * damaged one fails the allocation rather than looping or reading wild
 * memory.
 */
#ifndef SO_ALLOC_H
#define SO_ALLOC_H

#include "global.h"

#define SO_ARENA_BASE 0x12E00000
#define SO_ARENA_SIZE 0x00080000

int so_alloc_init(void *base, u32 size);
void *so_malloc(u32 size);
void so_free(void *ptr);

typedef struct {
	u32 used;	/* bytes in live blocks, headers included */
	u32 peak;
	u32 blocks;
	u32 failures;
} SoAllocStats;

void so_alloc_stats(SoAllocStats *out);

/* ---- integrity (diagnostics) ---------------------------------------------- */

typedef struct {
	u32 checks;	/* so_alloc_check calls */
	u32 bad_kind;	/* 0 ok, 1 block header, 2 free-space pattern */
	u32 bad_addr;	/* first bad word */
	u32 bad_value;
	u32 bad_count;	/* bad words found by the last check */
	u32 first_bad_check;	/* the check that first found damage */
} SoAllocIntegrity;

/* From now on keep free space filled with a pattern (address-derived), so
 * so_alloc_check can tell whether anything else writes into the arena. */
void so_alloc_fill_free(void);
/* Walks the blocks and the free-space pattern; returns 0 when intact. */
int so_alloc_check(void);
void so_alloc_integrity(SoAllocIntegrity *out);

#endif
