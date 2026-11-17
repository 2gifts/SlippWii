/* SlippWii: resolving the "slp:" links of Slippi's game files from the disc (hsd_links.c). */
#ifndef HSD_LINKS_H
#define HSD_LINKS_H

#ifdef HSD_STANDALONE	/* (tools/test_hsd_links.py builds it by itself for a PC) */
#include <stdint.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define hsd_alloc(n) malloc(n)
#else
#include <gccore.h>
#include <malloc.h>
#define hsd_alloc(n) memalign(32, ((n) + 31) & ~31u)
#endif

/* The disc: load gives a file's bytes (to be handed back to unload), or NULL when the disc has none
 * of that name. The bytes are changed while they are held. */
typedef struct {
	void *ctx;
	u8 *(*load)(void *ctx, const char *name, u32 *size);
	void (*unload)(void *ctx, u8 *data);
} HsdDisc;

/* The archive in `raw` (which is changed) with the asset of each of its "slp:" links copied in from
 * the disc: a new buffer (hsd_alloc: free() frees it) and its size, or NULL with *error a short
 * text. The result is what Slippi Dolphin gives the game for the same file and disc; the caller
 * checks its size and CRC against what an unmodified disc gives. */
u8 *hsd_resolve(u8 *raw, u32 size, const HsdDisc *disc, u32 *out_size, const char **error);

#endif
