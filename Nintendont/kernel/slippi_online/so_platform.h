/* slippi_online/so_platform.h
 * Platform services for Slippi Online in the Nintendont kernel (Starlet/IOS):
 * time, sleeping, randomness, mutexes, wake-up events, threads and logging.
 *
 * Everything above this layer (sockets, ENet, matchmaking, netplay) uses only
 * these functions, so the kernel's IOS syscalls stay in one place.
 */
#ifndef SO_PLATFORM_H
#define SO_PLATFORM_H

#include "global.h"

/* ---- time ---------------------------------------------------------------- */

/* Monotonic time since boot. The Starlet timer is 32-bit at ~1.9 MHz and
 * wraps every ~37 minutes; these extend it to 64 bits. Thread-safe. */
u64 so_time_us(void);
u32 so_time_ms(void);
/* Blocks the calling thread (an IOS timer, not a busy wait); 0 returns at
 * once. */
void so_sleep_us(u32 us);

/* xorshift32, seeded from the timer and the MAC address. */
u32 so_random(void);

/* ---- synchronisation ----------------------------------------------------- */

/* A mutex built on a one-slot IOS message queue (receive = lock, send =
 * unlock). Not recursive. */
typedef struct {
	s32 queue;
	u32 slot[8] __attribute__((aligned(32)));
} SoMutex;

int so_mutex_init(SoMutex *m);
void so_mutex_lock(SoMutex *m);
void so_mutex_unlock(SoMutex *m);

/* A wake-up event: any thread may signal it, one thread waits on it with a
 * timeout. Signals coalesce (a waiter wakes once for several signals), so
 * the queue never fills and signalling never blocks. */
typedef struct {
	s32 queue;
	volatile u32 pending;	/* a signal is queued */
	u32 slots[16] __attribute__((aligned(32)));
} SoEvent;

int so_event_init(SoEvent *e);
void so_event_signal(SoEvent *e);
/* Returns 1 if signalled, 0 on timeout. A timeout of 0 waits for a signal
 * only (no timer), as does a wait that gets no IOS timer (counted in
 * so_timer_failures). */
int so_event_wait(SoEvent *e, u32 timeout_us);
/* How many waits could not get an IOS timer (diagnostics). */
extern volatile u32 so_timer_failures;

/* ---- threads ------------------------------------------------------------- */

/* Starts a kernel thread and returns once it is running. `stack_top` and
 * `stack_size` come from kernel.ld. Returns the IOS thread id, or < 0. */
s32 so_thread_start(u32 (*entry)(void *), void *arg, void *stack_top, u32 stack_size, u32 priority);

/* ---- logging ------------------------------------------------------------- */

/* Lines go to a ring buffer that so_log_take() drains (the client forwards
 * them to the debug host when one is configured). Thread-safe; never blocks
 * on I/O. */
void so_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Copies the oldest pending line into `out` (NUL-terminated); returns its
 * length, or 0 when the ring is empty. */
int so_log_take(char *out, int size);

/* Short status shown in Slippi Nintendont's LAN broadcast (the nickname
 * field), so a PC on the network can see where the client is. */
void so_set_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
const char *so_status(void);

/* ---- checkpoints --------------------------------------------------------- */

/* Each thread records the last source line it passed; the watchdog
 * (so_diag.h) reports them, so a hang on hardware shows where every thread
 * is. SO_TRACE needs SO_FILE_ID (a short name) defined in the .c file. */
enum { SO_THREAD_NET, SO_THREAD_RX, SO_THREAD_DONE, SO_THREAD_TEST, SO_THREAD_COUNT };

typedef struct {
	const char *file;
	u32 line;
	u32 ticks;	/* so_ticks() when it got there */
	u32 count;	/* checkpoints passed */
} SoTrace;

/* The raw timer, without locks (so_time_* take one). */
u32 so_ticks(void);
u32 so_ticks_to_ms(u32 ticks);

extern volatile SoTrace so_traces[SO_THREAD_COUNT];
#define SO_TRACE(thread) do { \
	volatile SoTrace *t_ = &so_traces[thread]; \
	t_->file = SO_FILE_ID; \
	t_->line = __LINE__; \
	t_->ticks = so_ticks(); \
	t_->count++; \
} while (0)

/* The step the client is about to take (a string literal), for the
 * diagnostics beacon (so_diag.h). */
extern const char *volatile so_phase;
#define SO_PHASE(text) (so_phase = (text))

/* In diagnostics runs (diag_step_pause_ms) a step is announced as the phase
 * and held before it runs, so the beacon shows which step a hang is in;
 * otherwise it does nothing. */
extern volatile u32 so_step_pause_us;
#define SO_STEP(text) do { \
	if (so_step_pause_us) { \
		SO_PHASE(text); \
		so_sleep_us(so_step_pause_us); \
	} \
} while (0)

/* ---- the kernel's main thread (experiments) -------------------------------- */

/* Remembers the calling thread (so_client_init runs on the kernel's main
 * thread, priority 0x50) and sets its priority; returns the IOS result. */
void so_capture_main_thread(void);
int so_set_main_thread_priority(int priority);
#define SO_MAIN_THREAD_PRIORITY 0x50

/* ---- files --------------------------------------------------------------- */

/* Reads up to `size` bytes of a file on the boot drive (SD or USB). Returns
 * the length read, or < 0 when the file cannot be opened. Blocking. FatFS is
 * not reentrant: on the Wii, call the file functions from the kernel main
 * thread only (where disc reads run too), as initialisation and the EXI
 * device do. */
int so_read_file(const char *path, void *buf, u32 size);
/* Writes a whole file on the boot drive (replacing it). Returns 0, or < 0.
 * Takes the FatFS lock: fine from any kernel thread. */
int so_write_file(const char *path, const void *buf, u32 size);
/* Reads up to `size` bytes of a file into `out` through `bounce` (32-byte
 * aligned, `bounce_size` bytes), a piece at a time: for destinations the SD
 * and USB drivers must not write to directly, such as the game's memory.
 * Returns the length read, or < 0. */
int so_read_file_bounced(const char *path, u8 *out, u32 size, u8 *bounce, u32 bounce_size);
/* FatFS is not reentrant, and the kernel uses it from two threads at once
 * while a game runs: DIReadThread reads the game image (DI.c) and the EXI
 * device reads Slippi's files. Both hold this lock around their FatFS calls
 * (an overlap failed an open on hardware). No-ops until so_platform_init. */
void so_fs_lock(void);
void so_fs_unlock(void);

/* A file of the boot drive kept open (for reads while the game runs, with
 * no directory lookups then). so_file_open returns NULL, or a handle on the
 * online heap; `error` receives FatFS's result. so_file_read_at reads up to
 * `size` bytes from `offset` through `bounce` (as so_read_file_bounced) and
 * returns the length read, or < 0 (-1000 - FatFS's result). */
void *so_file_open(const char *path, int *error);
int so_file_read_at(void *file, u32 offset, u8 *out, u32 size, u8 *bounce, u32 bounce_size);

/* Calls `fn` for each file (not folder) in a folder of the boot drive.
 * Returns the number of files, or < 0 when the folder cannot be opened. */
int so_list_dir(const char *path, void (*fn)(const char *name, u32 size, void *ctx), void *ctx);

/* A word of the game's memory (diagnostics; 0 off the Wii). */
u32 so_game_word(u32 address);
/* Nintendont's EXI mailbox (command, data, length), the requests for an
 * unhandled device (count, last command word, device), and the select/imm/
 * dma request counts (diagnostics; zeros off the Wii). */
void so_exi_debug(u32 out[9]);

/* Memory for keeping files in RAM (so_files): Pokemon Stadium's
 * transformations and Slippi's menu files. On the Wii the top of the disc
 * cache's MEM2 region, which ISO.c leaves out in online mode. */
#define SO_FILE_CACHE_SIZE 0x600000	/* 5.45 MB used: 1.1 MB of Stadium transformations, 4.4 MB of menus */

/* Reads bytes of the game disc (the image Nintendont runs); 0, or < 0 with
 * no disc (off the Wii). */
int so_disc_read(u32 offset, void *buf, u32 len);
void *so_platform_file_cache(u32 *size);

/* The memory for so_alloc (a fixed MEM2 region on the Wii). */
void *so_platform_arena(u32 *size);

int so_platform_init(void);

#endif
