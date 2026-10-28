/* slippi_online/so_alloc.c
 * First-fit arena allocator (see so_alloc.h).
 *
 * The arena is a sequence of blocks in address order. Each block starts with
 * a 32-byte header, so payloads stay 32-byte aligned. Freeing merges the
 * block with free neighbours. The client makes a few dozen allocations at a
 * time (ENet peers and packets, receive buffers, JSON), so a linear scan is
 * fast enough and keeps the allocator simple to verify.
 */
#include "so_alloc.h"
#include "so_platform.h"
#include "string.h"

#define ALIGN 32
#define MAGIC_USED 0x534F5531	/* "SOU1" */
#define MAGIC_FREE 0x534F4631	/* "SOF1" */

typedef struct Block {
	u32 size;	/* whole block, header included; multiple of ALIGN */
	u32 magic;
	u32 pad[6];
} Block;

static u8 *arena;
static u32 arena_size;
static SoMutex lock;
static int lock_ready;
static SoAllocStats stats;
static int fill_free;
static SoAllocIntegrity integrity;
static u32 bad_headers;	/* walks stopped by a damaged header */

#define PATTERN(addr) ((u32)(size_t)(addr) ^ 0x5A5AA5A5u)

/* A header is sane when its size keeps the block inside the arena. */
static int header_ok(const Block *b)
{
	u32 offset = (u32)((const u8 *)b - arena);
	return (b->magic == MAGIC_USED || b->magic == MAGIC_FREE) && b->size >= sizeof(Block) &&
		(b->size & (ALIGN - 1)) == 0 && b->size <= arena_size - offset;
}

static void fill_range(void *start, void *end)
{
	u32 *w;
	for (w = (u32 *)start; w < (u32 *)end; w++)
		*w = PATTERN(w);
}

static void fill_block(Block *b)
{
	fill_range(b + 1, (u8 *)b + b->size);
}

/* The next block, or NULL at the end or at a damaged header (counted, so a
 * corrupted arena fails allocations instead of sending the walk into wild
 * memory). */
static Block *next_block(Block *b)
{
	Block *n;
	if (!header_ok(b)) {
		bad_headers++;
		return NULL;
	}
	n = (Block *)((u8 *)b + b->size);
	if ((u8 *)n >= arena + arena_size)
		return NULL;
	if (!header_ok(n)) {
		bad_headers++;
		return NULL;
	}
	return n;
}

int so_alloc_init(void *base, u32 size)
{
	Block *first;
	if (!lock_ready) {
		if (so_mutex_init(&lock) < 0)
			return -1;
		lock_ready = 1;
	}
	/* Only the low bits matter for alignment (also on a 64-bit test host). */
	arena = (u8 *)base + ((ALIGN - ((u32)(size_t)base & (ALIGN - 1))) & (ALIGN - 1));
	arena_size = (size - (u32)(arena - (u8 *)base)) & ~(ALIGN - 1);
	first = (Block *)arena;
	first->size = arena_size;
	first->magic = MAGIC_FREE;
	memset(&stats, 0, sizeof(stats));
	return 0;
}

void *so_malloc(u32 size)
{
	u32 need = (size + sizeof(Block) + ALIGN - 1) & ~(ALIGN - 1);
	Block *b;
	void *result = NULL;
	if (!arena || size == 0)
		return NULL;
	so_mutex_lock(&lock);
	for (b = (Block *)arena; b; b = next_block(b)) {
		if (b->magic != MAGIC_FREE || b->size < need)
			continue;
		if (b->size - need >= sizeof(Block) + ALIGN) {
			/* Split: the remainder stays free. */
			Block *rest = (Block *)((u8 *)b + need);
			rest->size = b->size - need;
			rest->magic = MAGIC_FREE;
			b->size = need;
		}
		b->magic = MAGIC_USED;
		stats.used += b->size;
		stats.blocks++;
		if (stats.used > stats.peak)
			stats.peak = stats.used;
		result = b + 1;
		break;
	}
	if (!result)
		stats.failures++;
	so_mutex_unlock(&lock);
	return result;
}

void so_free(void *ptr)
{
	Block *b, *n;
	if (!ptr)
		return;
	b = (Block *)ptr - 1;
	so_mutex_lock(&lock);
	if (b->magic != MAGIC_USED) {
		so_mutex_unlock(&lock);
		so_log("alloc: bad free %p", ptr);
		return;
	}
	b->magic = MAGIC_FREE;
	stats.used -= b->size;
	stats.blocks--;
	if (fill_free)
		fill_block(b);	/* only what changed: this block and absorbed headers */
	/* Merge every run of adjacent free blocks. */
	for (b = (Block *)arena; b; b = next_block(b)) {
		if (b->magic != MAGIC_FREE)
			continue;
		while ((n = next_block(b)) && n->magic == MAGIC_FREE) {
			b->size += n->size;
			if (fill_free)
				fill_range(n, n + 1);
			else
				n->magic = 0;
		}
	}
	so_mutex_unlock(&lock);
}

void so_alloc_fill_free(void)
{
	Block *b;
	so_mutex_lock(&lock);
	fill_free = 1;
	for (b = (Block *)arena; b; b = next_block(b))
		if (b->magic == MAGIC_FREE)
			fill_block(b);
	so_mutex_unlock(&lock);
}

int so_alloc_check(void)
{
	Block *b;
	u32 count = 0, first_addr = 0, first_value = 0, kind = 0, walked = 0, before;
	so_mutex_lock(&lock);
	before = bad_headers;
	for (b = (Block *)arena; b; b = next_block(b)) {
		walked += b->size;
		if (fill_free && b->magic == MAGIC_FREE) {
			u32 *w = (u32 *)(b + 1), *end = (u32 *)((u8 *)b + b->size);
			for (; w < end; w++) {
				if (*w == PATTERN(w))
					continue;
				if (!count++) {
					first_addr = (u32)(size_t)w;
					first_value = *w;
					kind = 2;
				}
			}
		}
	}
	if (bad_headers != before || walked != arena_size) {
		/* The walk stopped early: report where. */
		kind = 1;
		first_addr = (u32)(size_t)(arena + walked);
		first_value = walked < arena_size ? *(u32 *)(arena + walked + 4) : 0;	/* its magic */
		count++;
	}
	integrity.checks++;
	if (count) {
		if (!integrity.bad_kind)
			integrity.first_bad_check = integrity.checks;
		integrity.bad_kind = kind;
		integrity.bad_addr = first_addr;
		integrity.bad_value = first_value;
	}
	integrity.bad_count = count;
	so_mutex_unlock(&lock);
	return count ? -1 : 0;
}

void so_alloc_integrity(SoAllocIntegrity *out)
{
	*out = integrity;
}

void so_alloc_stats(SoAllocStats *out)
{
	so_mutex_lock(&lock);
	*out = stats;
	so_mutex_unlock(&lock);
}
