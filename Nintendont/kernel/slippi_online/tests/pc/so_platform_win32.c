/* PC test build: so_platform.h on Win32 (threads, events, time, files), so
 * the Wii client code can run against the local test tools. */
#include <winsock2.h>
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "so_platform.h"
#include "so_util.h"
#include "string.h"
#include "pc.h"

#define MAX_OBJECTS 64

static LARGE_INTEGER freq, origin;
static u32 random_state = 0x2545F491;
static CRITICAL_SECTION mutexes[MAX_OBJECTS];
static HANDLE events[MAX_OBJECTS];
static int mutex_count, event_count;
static CRITICAL_SECTION log_lock;
static char data_dir[260] = ".";
static char status[32];

void so_pc_set_data_dir(const char *dir)
{
	strlcpy(data_dir, dir, sizeof(data_dir));
}

volatile SoTrace so_traces[SO_THREAD_COUNT];
volatile u32 so_timer_failures;

u32 so_ticks(void)
{
	return (u32)so_time_us();
}

u32 so_ticks_to_ms(u32 ticks)
{
	return ticks / 1000;
}

u64 so_time_us(void)
{
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	return (u64)((now.QuadPart - origin.QuadPart) * 1000000 / freq.QuadPart);
}

u32 so_time_ms(void)
{
	return (u32)(so_time_us() / 1000);
}

void so_sleep_us(u32 us)
{
	Sleep(us >= 1000 ? us / 1000 : (us ? 1 : 0));
}

u32 so_random(void)
{
	u32 x = random_state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return random_state = x;
}

int so_mutex_init(SoMutex *m)
{
	if (mutex_count >= MAX_OBJECTS)
		return -1;
	InitializeCriticalSection(&mutexes[mutex_count]);
	m->queue = mutex_count++;
	return 0;
}

void so_mutex_lock(SoMutex *m)
{
	EnterCriticalSection(&mutexes[m->queue]);
}

void so_mutex_unlock(SoMutex *m)
{
	LeaveCriticalSection(&mutexes[m->queue]);
}

int so_event_init(SoEvent *e)
{
	if (event_count >= MAX_OBJECTS)
		return -1;
	events[event_count] = CreateEvent(NULL, FALSE, FALSE, NULL);
	e->queue = event_count++;
	return 0;
}

void so_event_signal(SoEvent *e)
{
	SetEvent(events[e->queue]);
}

int so_event_wait(SoEvent *e, u32 timeout_us)
{
	DWORD ms = timeout_us ? (timeout_us + 999) / 1000 : INFINITE;
	return WaitForSingleObject(events[e->queue], ms) == WAIT_OBJECT_0;
}

typedef struct {
	u32 (*entry)(void *);
	void *arg;
} Launch;

static DWORD WINAPI trampoline(LPVOID p)
{
	Launch l = *(Launch *)p;
	free(p);
	return l.entry(l.arg);
}

s32 so_thread_start(u32 (*entry)(void *), void *arg, void *stack_top, u32 stack_size, u32 priority)
{
	Launch *l = malloc(sizeof(*l));
	HANDLE h;
	(void)stack_top;
	(void)stack_size;
	(void)priority;
	l->entry = entry;
	l->arg = arg;
	h = CreateThread(NULL, 0, trampoline, l, 0, NULL);
	if (!h)
		return -1;
	CloseHandle(h);
	return 1;
}

void so_log(const char *fmt, ...)
{
	char line[256];
	va_list ap;
	u32 ms = so_time_ms();
	int n = so_snprintf(line, sizeof(line), "%u.%03u ", (unsigned)(ms / 1000), (unsigned)(ms % 1000));
	va_start(ap, fmt);
	so_vsnprintf(line + n, sizeof(line) - n, fmt, ap);
	va_end(ap);
	EnterCriticalSection(&log_lock);
	printf("wii-pc | %s\n", line);
	fflush(stdout);
	LeaveCriticalSection(&log_lock);
}

int so_log_take(char *out, int size)
{
	(void)out;
	(void)size;
	return 0;	/* printed directly */
}

void so_set_status(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	so_vsnprintf(status, sizeof(status), fmt, ap);
	va_end(ap);
}

const char *so_status(void)
{
	return status;
}

/* "/slippwii/x" -> <data dir>/x (and other paths by their file name) */
static void map_path(const char *path, char *full, int size)
{
	static const char prefix[] = "/slippwii/";
	const char *name = strrchr(path, '/');
	if (strncmp(path, prefix, sizeof(prefix) - 1) == 0)
		name = path + sizeof(prefix) - 2;
	so_snprintf(full, size, "%s/%s", data_dir, name ? name + 1 : path);
}

int so_list_dir(const char *path, void (*fn)(const char *name, u32 size, void *ctx), void *ctx)
{
	char full[400];
	WIN32_FIND_DATAA found;
	HANDLE h;
	int n = 0;
	map_path(path, full, sizeof(full) - 2);
	strlcpy(full + strlen(full), "/*", 3);
	h = FindFirstFileA(full, &found);
	if (h == INVALID_HANDLE_VALUE)
		return -1;
	do {
		if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;
		fn(found.cFileName, (u32)found.nFileSizeLow, ctx);
		n++;
	} while (FindNextFileA(h, &found));
	FindClose(h);
	return n;
}

void so_exi_debug(u32 out[9])
{
	memset(out, 0, 9 * sizeof(u32));
}

void *so_platform_file_cache(u32 *size)
{
	static void *cache;
	*size = SO_FILE_CACHE_SIZE;
	if (!cache)
		cache = malloc(SO_FILE_CACHE_SIZE);
	return cache;
}

int so_disc_read(u32 offset, void *buf, u32 len)
{
	(void)offset;
	(void)buf;
	(void)len;
	return -1;	/* no disc on the PC */
}

void so_fs_lock(void) {}
void so_fs_unlock(void) {}

void *so_file_open(const char *path, int *error)
{
	char full[400];
	FILE *f;
	map_path(path, full, sizeof(full));
	f = fopen(full, "rb");
	*error = f ? 0 : 4;	/* FR_NO_FILE */
	return f;
}

int so_file_read_at(void *file, u32 offset, u8 *out, u32 size, u8 *bounce, u32 bounce_size)
{
	(void)bounce;
	(void)bounce_size;
	if (fseek((FILE *)file, (long)offset, SEEK_SET) != 0)
		return -1001;
	return (int)fread(out, 1, size, (FILE *)file);
}

u32 so_game_word(u32 address)
{
	(void)address;
	return 0;
}

int so_read_file_bounced(const char *path, u8 *out, u32 size, u8 *bounce, u32 bounce_size)
{
	(void)bounce;
	(void)bounce_size;
	return so_read_file(path, out, size);
}

int so_write_file(const char *path, const void *buf, u32 size)
{
	char full[400];
	FILE *f;
	int ok;
	map_path(path, full, sizeof(full));
	f = fopen(full, "wb");
	if (!f)
		return -1;
	ok = fwrite(buf, 1, size, f) == size;
	fclose(f);
	return ok ? 0 : -2;
}

int so_read_file(const char *path, void *buf, u32 size)
{
	char full[400];
	FILE *f;
	int n;
	map_path(path, full, sizeof(full));
	f = fopen(full, "rb");
	if (!f)
		return -1;
	n = (int)fread(buf, 1, size, f);
	fclose(f);
	return n;
}

void so_capture_main_thread(void)
{
}

int so_set_main_thread_priority(int priority)
{
	(void)priority;
	return 0;
}

void *so_platform_arena(u32 *size)
{
	*size = 512 * 1024;
	return malloc(*size);
}

int so_platform_init(void)
{
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&origin);
	InitializeCriticalSection(&log_lock);
	timeBeginPeriod(1);
	random_state ^= (u32)GetTickCount() ^ (u32)GetCurrentProcessId() << 16;
	return 0;
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
