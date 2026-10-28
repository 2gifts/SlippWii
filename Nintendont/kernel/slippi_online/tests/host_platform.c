/* Host implementation of so_platform.h for the unit tests: single-threaded,
 * with a clock the tests set by hand. */
#include <stdarg.h>
#include <stdio.h>
#include "so_platform.h"
#include "so_util.h"
#include "string.h"
#include "host_platform.h"

static u64 now_us;
static u32 random_state = 0x2545F491;
static int log_echo;

void test_set_time_us(u64 us) { now_us = us; }
void test_advance_us(u64 us) { now_us += us; }
void test_log_echo(int on) { log_echo = on; }

u64 so_time_us(void) { return now_us; }
u32 so_time_ms(void) { return (u32)(now_us / 1000); }
void so_sleep_us(u32 us) { now_us += us; }

u32 so_random(void)
{
	u32 x = random_state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return random_state = x;
}

int so_mutex_init(SoMutex *m) { m->queue = 1; return 0; }
void so_mutex_lock(SoMutex *m) { (void)m; }
void so_mutex_unlock(SoMutex *m) { (void)m; }

int so_event_init(SoEvent *e) { e->queue = 0; return 0; }
void so_event_signal(SoEvent *e) { e->queue = 1; }
int so_event_wait(SoEvent *e, u32 timeout_us)
{
	int signalled = e->queue;
	if (!signalled)
		now_us += timeout_us;
	e->queue = 0;
	return signalled;
}

s32 so_thread_start(u32 (*entry)(void *), void *arg, void *stack_top, u32 stack_size, u32 priority)
{
	(void)entry; (void)arg; (void)stack_top; (void)stack_size; (void)priority;
	return -1;
}

static char last_log[256];

void so_log(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	so_vsnprintf(last_log, sizeof(last_log), fmt, ap);
	va_end(ap);
	if (log_echo)
		printf("    log: %s\n", last_log);
}

const char *test_last_log(void) { return last_log; }

int so_log_take(char *out, int size) { (void)out; (void)size; return 0; }

static char status[32];

void so_set_status(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	so_vsnprintf(status, sizeof(status), fmt, ap);
	va_end(ap);
}

const char *so_status(void) { return status; }
int so_platform_init(void) { return 0; }
int so_list_dir(const char *path, void (*fn)(const char *name, u32 size, void *ctx), void *ctx)
{
	(void)path; (void)fn; (void)ctx;
	return -1;
}
void *so_platform_file_cache(u32 *size)
{
	static u8 cache[0x10000];
	*size = sizeof(cache);
	return cache;
}

static const u8 *disc;
static u32 disc_len;

void test_set_disc(const u8 *d, u32 len)
{
	disc = d;
	disc_len = len;
}

int so_disc_read(u32 offset, void *buf, u32 len)
{
	if (!disc || offset > disc_len || len > disc_len - offset)
		return -1;
	memcpy(buf, disc + offset, len);
	return 0;
}
void so_fs_lock(void) {}
void so_fs_unlock(void) {}
void *so_file_open(const char *path, int *error) { (void)path; *error = 4; return NULL; }
int so_file_read_at(void *file, u32 offset, u8 *out, u32 size, u8 *bounce, u32 bounce_size)
{
	(void)file; (void)offset; (void)out; (void)size; (void)bounce; (void)bounce_size;
	return -1;
}
int so_read_file_bounced(const char *path, u8 *out, u32 size, u8 *bounce, u32 bounce_size)
{
	(void)path; (void)out; (void)size; (void)bounce; (void)bounce_size;
	return -1;
}

int so_write_file(const char *path, const void *buf, u32 size)
{
	FILE *f = fopen(path, "wb");
	int ok;
	if (!f)
		return -1;
	ok = fwrite(buf, 1, size, f) == size;
	fclose(f);
	return ok ? 0 : -2;
}

int so_read_file(const char *path, void *buf, u32 size)
{
	FILE *f = fopen(path, "rb");
	int n;
	if (!f)
		return -1;
	n = (int)fread(buf, 1, size, f);
	fclose(f);
	return n;
}

size_t so_test_strlcpy(char *dest, const char *src, size_t size)
{
	size_t n = strlen(src);
	if (size) {
		size_t copy = n < size - 1 ? n : size - 1;
		memcpy(dest, src, copy);
		dest[copy] = '\0';
	}
	return n;
}
