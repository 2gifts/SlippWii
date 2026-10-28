/* slippi_online/so_platform.c
 * IOS-backed platform services (see so_platform.h).
 */
#include "so_platform.h"
#include "so_alloc.h"
#include "so_util.h"
#include "common.h"
#include "ff_utf8.h"
#include "ISO.h"
#include "syscalls.h"
#include "string.h"

extern u8 wifi_mac_address[6];

/* ---- time ---------------------------------------------------------------- */

/* The Starlet timer ticks every 0.5267 us (HW_TIMER, 1.898 MHz). */
#define TICKS_PER_SECOND 1898614ULL

static SoMutex time_lock;
static u32 time_last_ticks;
static u64 time_high;	/* ticks lost to wraps, in ticks */
static int platform_ready;

u64 so_time_us(void)
{
	u64 ticks;
	if (!platform_ready)
		return (u64)read32(HW_TIMER) * 1000000ULL / TICKS_PER_SECOND;
	so_mutex_lock(&time_lock);
	{
		u32 now = read32(HW_TIMER);
		if (now < time_last_ticks)
			time_high += 1ULL << 32;
		time_last_ticks = now;
		ticks = time_high + now;
	}
	so_mutex_unlock(&time_lock);
	return ticks * 1000000ULL / TICKS_PER_SECOND;
}

u32 so_time_ms(void)
{
	return (u32)(so_time_us() / 1000);
}

void so_sleep_us(u32 us)
{
	/* An IOS timer of 0 us never fires: udelay(0) would block for ever. */
	if (us)
		udelay((int)us);
}

static u32 random_state;

u32 so_random(void)
{
	u32 x = random_state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	random_state = x;
	return x;
}

/* ---- mutex --------------------------------------------------------------- */

int so_mutex_init(SoMutex *m)
{
	m->queue = mqueue_create(m->slot, 1);
	if (m->queue < 0)
		return m->queue;
	/* One token in the queue: the mutex is free. */
	return mqueue_send(m->queue, (struct ipcmessage *)1, 0);
}

void so_mutex_lock(SoMutex *m)
{
	struct ipcmessage *token;
	mqueue_recv(m->queue, &token, 0);
}

void so_mutex_unlock(SoMutex *m)
{
	mqueue_send(m->queue, (struct ipcmessage *)1, 0);
}

/* ---- event --------------------------------------------------------------- */

#define EVENT_SIGNAL  ((struct ipcmessage *)1)
#define EVENT_TIMEOUT ((struct ipcmessage *)2)

int so_event_init(SoEvent *e)
{
	e->pending = 0;
	e->queue = mqueue_create(e->slots, sizeof(e->slots) / sizeof(e->slots[0]));
	return e->queue < 0 ? e->queue : 0;
}

void so_event_signal(SoEvent *e)
{
	/* A blocking send, as the mutexes use; with at most a few messages
	 * queued (signals coalesce, one timer per wait) it never waits. */
	if (e->pending)
		return;
	e->pending = 1;
	mqueue_send(e->queue, EVENT_SIGNAL, 0);
}

volatile u32 so_timer_failures;

int so_event_wait(SoEvent *e, u32 timeout_us)
{
	struct ipcmessage *msg = NULL;
	s32 timer = -1;
	int signalled;
	if (timeout_us) {
		timer = TimerCreate((int)timeout_us, 0, e->queue, (int)EVENT_TIMEOUT);
		/* Without a timer, wait for a signal only: returning at once would
		 * let the caller's loop spin and starve the kernel's main thread. */
		if (timer < 0)
			so_timer_failures++;
	}
	mqueue_recv(e->queue, &msg, 0);
	e->pending = 0;	/* a later signal queues a new message */
	signalled = msg == EVENT_SIGNAL;
	if (timer >= 0)
		TimerDestroy(timer);
	/* Drop extra signals and a timeout that raced with the signal. */
	while (mqueue_recv(e->queue, &msg, 1) >= 0)
		signalled |= msg == EVENT_SIGNAL;
	return signalled;
}

/* ---- threads ------------------------------------------------------------- */

/* do_thread_create (global.h) passes the entry point through one shared word
 * that the new thread reads when it first runs. A creator with the same or a
 * higher priority than the new thread would overwrite that word before the
 * thread runs, so so_thread_start waits for each thread to check in. */
static struct {
	u32 (*entry)(void *);
	void *arg;
	volatile int started;
} launch;
static SoMutex launch_lock;

static u32 thread_trampoline(void *unused)
{
	u32 (*entry)(void *) = launch.entry;
	void *arg = launch.arg;
	(void)unused;
	launch.started = 1;
	return entry(arg);
}

s32 so_thread_start(u32 (*entry)(void *), void *arg, void *stack_top, u32 stack_size, u32 priority)
{
	s32 id;
	int waited;
	so_mutex_lock(&launch_lock);
	launch.entry = entry;
	launch.arg = arg;
	launch.started = 0;
	id = (s32)do_thread_create(thread_trampoline, (u32 *)stack_top, stack_size, priority);
	if (id >= 0) {
		thread_continue(id);
		for (waited = 0; !launch.started && waited < 1000; waited++)
			mdelay(1);
		if (!launch.started)
			id = -1;
	}
	so_mutex_unlock(&launch_lock);
	return id;
}

/* ---- logging ------------------------------------------------------------- */

#define LOG_LINES 64
#define LOG_LINE_SIZE 160

static struct {
	char lines[LOG_LINES][LOG_LINE_SIZE];
	u32 head, tail;	/* head: next write; tail: next read */
	u32 dropped;
} log_ring;
static SoMutex log_lock;

void so_log(const char *fmt, ...)
{
	char line[LOG_LINE_SIZE];
	va_list ap;
	u32 ms;
	int n;
	if (!platform_ready)
		return;
	ms = so_time_ms();
	n = so_snprintf(line, sizeof(line), "%u.%03u ", (unsigned)(ms / 1000), (unsigned)(ms % 1000));
	va_start(ap, fmt);
	so_vsnprintf(line + n, sizeof(line) - n, fmt, ap);
	va_end(ap);
	so_mutex_lock(&log_lock);
	if (log_ring.head - log_ring.tail >= LOG_LINES) {
		log_ring.tail++;	/* overwrite the oldest line */
		log_ring.dropped++;
	}
	strlcpy(log_ring.lines[log_ring.head % LOG_LINES], line, LOG_LINE_SIZE);
	log_ring.head++;
	so_mutex_unlock(&log_lock);
}

int so_log_take(char *out, int size)
{
	int n = 0;
	so_mutex_lock(&log_lock);
	if (log_ring.dropped) {
		n = so_snprintf(out, size, "(%u log lines dropped)", (unsigned)log_ring.dropped);
		log_ring.dropped = 0;
	} else if (log_ring.tail != log_ring.head) {
		strlcpy(out, log_ring.lines[log_ring.tail % LOG_LINES], size);
		n = strlen(out);
		log_ring.tail++;
	}
	so_mutex_unlock(&log_lock);
	return n;
}

volatile SoTrace so_traces[SO_THREAD_COUNT];

u32 so_ticks(void)
{
	return read32(HW_TIMER);
}

u32 so_ticks_to_ms(u32 ticks)
{
	return (u32)((u64)ticks * 1000 / TICKS_PER_SECOND);
}

/* Read by SlippiNetworkBroadcast.c into the SLIP_READY nickname field. */
char slippi_online_status[32] = "";

void so_set_status(const char *fmt, ...)
{
	char text[64];
	va_list ap;
	va_start(ap, fmt);
	so_vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	memcpy(slippi_online_status, text, sizeof(slippi_online_status) - 1);
	slippi_online_status[sizeof(slippi_online_status) - 1] = '\0';
}

const char *so_status(void)
{
	return slippi_online_status;
}

int so_read_file(const char *path, void *buf, u32 size)
{
	FIL file;
	UINT got = 0;
	FRESULT res;
	so_fs_lock();
	if (f_open_main_drive(&file, path, FA_OPEN_EXISTING | FA_READ) != FR_OK) {
		so_fs_unlock();
		return -1;
	}
	res = f_read(&file, buf, size, &got);
	f_close(&file);
	so_fs_unlock();
	return res == FR_OK ? (int)got : -2;
}

int so_write_file(const char *path, const void *buf, u32 size)
{
	FIL file;
	UINT done = 0;
	FRESULT res;
	so_fs_lock();
	if (f_open_main_drive(&file, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
		so_fs_unlock();
		return -1;
	}
	res = f_write(&file, buf, size, &done);
	f_close(&file);
	so_fs_unlock();
	return res == FR_OK && done == size ? 0 : -2;
}

char *getMainDrivePath(const char *path);	/* common.c */

static SoMutex fs_mutex;
static volatile int fs_lock_ready;

void so_fs_lock(void)
{
	if (fs_lock_ready)
		so_mutex_lock(&fs_mutex);
}

void so_fs_unlock(void)
{
	if (fs_lock_ready)
		so_mutex_unlock(&fs_mutex);
}

int so_disc_read(u32 offset, void *buf, u32 len)
{
	bool ok;
	so_fs_lock();	/* the disc image is on the same FatFS drive */
	ok = ISOReadInto(buf, len, offset);
	so_fs_unlock();
	return ok ? 0 : -1;
}

void *so_file_open(const char *path, int *error)
{
	FIL *file = (FIL *)so_malloc(sizeof(FIL));
	FRESULT res;
	if (!file) {
		*error = -1;
		return NULL;
	}
	so_fs_lock();
	res = f_open_main_drive(file, path, FA_OPEN_EXISTING | FA_READ);
	so_fs_unlock();
	*error = (int)res;
	if (res != FR_OK) {
		so_free(file);
		return NULL;
	}
	return file;
}

int so_file_read_at(void *handle, u32 offset, u8 *out, u32 size, u8 *bounce, u32 bounce_size)
{
	FIL *file = (FIL *)handle;
	FRESULT res;
	u32 done = 0;
	so_fs_lock();
	if ((res = f_lseek(file, offset)) != FR_OK) {
		so_fs_unlock();
		return -1000 - (int)res;
	}
	while (done < size) {
		UINT got = 0;
		u32 want = size - done < bounce_size ? size - done : bounce_size;
		if ((res = f_read(file, bounce, want, &got)) != FR_OK) {
			so_fs_unlock();
			return -1000 - (int)res;
		}
		memcpy(out + done, bounce, got);
		done += got;
		if (got < want)
			break;	/* end of file */
	}
	so_fs_unlock();
	return (int)done;
}

extern u32 EXIDebugUnhandled, EXIDebugUnhandledCommand, EXIDebugUnhandledDevice, EXIDebugCount[3];

void so_exi_debug(u32 out[9])
{
	sync_before_read((void *)0x13026800, 0x20);
	out[0] = read32(0x13026800);	/* EXI_CMD_0: non-zero while the game waits */
	out[1] = read32(0x13026804);
	out[2] = read32(0x13026808);
	out[3] = EXIDebugUnhandled;
	out[4] = EXIDebugUnhandledCommand;
	out[5] = EXIDebugUnhandledDevice;
	out[6] = EXIDebugCount[0];
	out[7] = EXIDebugCount[1];
	out[8] = EXIDebugCount[2];
}

u32 so_game_word(u32 address)
{
	u32 phys = address & 0x017FFFFC;	/* MEM1 */
	sync_before_read((void *)(phys & ~31u), 32);
	return read32(phys);
}

int so_read_file_bounced(const char *path, u8 *out, u32 size, u8 *bounce, u32 bounce_size)
{
	FIL file;
	u32 done = 0;
	so_fs_lock();
	if (f_open_main_drive(&file, path, FA_OPEN_EXISTING | FA_READ) != FR_OK) {
		so_fs_unlock();
		return -1;
	}
	while (done < size) {
		UINT got = 0;
		u32 want = size - done < bounce_size ? size - done : bounce_size;
		if (f_read(&file, bounce, want, &got) != FR_OK) {
			f_close(&file);
			so_fs_unlock();
			return -2;
		}
		memcpy(out + done, bounce, got);
		done += got;
		if (got < want)
			break;	/* end of file */
	}
	f_close(&file);
	so_fs_unlock();
	return (int)done;
}

int so_list_dir(const char *path, void (*fn)(const char *name, u32 size, void *ctx), void *ctx)
{
	DIR dir;
	FILINFO info;
	int n = 0;
	so_fs_lock();
	if (f_opendir_char(&dir, getMainDrivePath(path)) != FR_OK) {
		so_fs_unlock();
		return -1;
	}
	while (f_readdir(&dir, &info) == FR_OK && info.fname[0]) {
		if (info.fattrib & AM_DIR)
			continue;
		fn(wchar_to_char(info.fname), (u32)info.fsize, ctx);
		n++;
	}
	f_closedir(&dir);
	so_fs_unlock();
	return n;
}

static s32 main_thread = -1;

void so_capture_main_thread(void)
{
	main_thread = (s32)thread_get_id();
}

int so_set_main_thread_priority(int priority)
{
	return main_thread < 0 ? -1 : thread_set_priority(main_thread, priority);
}

void *so_platform_file_cache(u32 *size)
{
	/* ISO.c's cache ends at 0x12B70000 when no memory card is emulated
	 * (Slippi Nintendont); this is its top SO_FILE_CACHE_SIZE. */
	*size = SO_FILE_CACHE_SIZE;
	return (void *)(0x12B70000 - SO_FILE_CACHE_SIZE);
}

void *so_platform_arena(u32 *size)
{
	*size = SO_ARENA_SIZE;
	return (void *)SO_ARENA_BASE;
}

int so_platform_init(void)
{
	int i;
	if (platform_ready)
		return 0;
	if (so_mutex_init(&time_lock) < 0 || so_mutex_init(&launch_lock) < 0 || so_mutex_init(&log_lock) < 0 ||
	    so_mutex_init(&fs_mutex) < 0)
		return -1;
	fs_lock_ready = 1;
	random_state = read32(HW_TIMER) ^ 0x9E3779B9;
	for (i = 0; i < 6; i++)
		random_state = (random_state << 5) ^ (random_state >> 27) ^ wifi_mac_address[i];
	if (!random_state)
		random_state = 0x2545F491;
	platform_ready = 1;
	return 0;
}
