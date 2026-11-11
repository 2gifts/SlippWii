/* slippi_online/so_platform.c
 * IOS-backed platform services (see so_platform.h).
 */
#include "so_platform.h"
#include "so_logfile.h"
#include "so_alloc.h"
#include "so_util.h"
#include "common.h"
#include "ff_utf8.h"
#include "ISO.h"
#include "Config.h"
#include "syscalls.h"
#include "string.h"

extern u8 wifi_mac_address[6];

/* ---- time ---------------------------------------------------------------- */

/* The Starlet timer ticks every 0.5267 us (HW_TIMER, 1.898 MHz). */
#define TICKS_PER_SECOND 1898614ULL

static SoMutex time_lock;
/* The timer's wraps, x 2, and its top bit when last seen. Read without the lock:
 * the sender and receive threads, below IOS's network module, read the clock all
 * the time, and with a lock around it, one preempted inside it held up the network
 * thread and the game's polls behind it for milliseconds. Written only as the
 * top bit changes, twice in the timer's 37 minutes. */
static volatile u32 time_state;
static int platform_ready;
int so_ppc_scratch_free;

u64 so_time_us(void)
{
	u32 now, state;
	if (!platform_ready)
		return (u64)read32(HW_TIMER) * 1000000ULL / TICKS_PER_SECOND;
	state = time_state;
	now = read32(HW_TIMER);
	if ((state & 1) != now >> 31) {
		so_mutex_lock(&time_lock);
		state = time_state;
		now = read32(HW_TIMER);
		if ((state & 1) != now >> 31) {
			state++;	/* 0 -> 1, or 1 -> 0 and a wrap: state >> 1 counts the wraps */
			time_state = state;
		}
		so_mutex_unlock(&time_lock);
	}
	return (((u64)(state >> 1) << 32) + now) * 1000000ULL / TICKS_PER_SECOND;
}

u32 so_time_ms(void)
{
	return (u32)(so_time_us() / 1000);
}

u32 so_ppc_word(u32 addr)
{
	sync_before_read((void *)addr, 4);
	return read32(addr);
}

void so_set_ppc_word(u32 addr, u32 value)
{
	write32(addr, value);
	sync_after_write((void *)addr, 4);
}

void so_ppc_will_read(u32 addr, u32 len)
{
	sync_before_read((void *)addr, len);
}

void so_ppc_set_block(u32 addr, const void *data, u32 len)
{
	memcpy((void *)addr, data, len);
	sync_after_write((void *)addr, len);
}

/* ES_GetViewCount, ES_GetViews, ES_LaunchTitle: resetstub/main.c's
 * es_launchtitle, from the kernel's side of IPC. */
int so_launch_title(u64 title_id)
{
	static const char es_path[] __attribute__((aligned(32))) = "/dev/es";
	u64 *id = so_malloc(32);
	u32 *count = so_malloc(32);
	u8 *views = so_malloc(0xD8 * 4);
	vector *v = so_malloc(3 * sizeof(vector) + 32);
	int fd, res = -1;
	if (!id || !count || !views || !v)
		goto out;
	fd = IOS_Open(es_path, 0);
	if (fd < 0) {
		res = fd;
		goto out;
	}
	*id = title_id;
	v[0].data = (u32)id;
	v[0].len = 8;
	v[1].data = (u32)count;
	v[1].len = 4;
	res = IOS_Ioctlv(fd, 0x12, 1, 1, v);	/* the ticket views' count */
	if (res >= 0 && (*count < 1 || *count > 4))
		res = -2;
	if (res >= 0) {
		v[2].data = (u32)views;
		v[2].len = 0xD8 * *count;
		res = IOS_Ioctlv(fd, 0x13, 2, 1, v);	/* the views */
	}
	if (res >= 0) {
		v[1].data = (u32)views;
		v[1].len = 0xD8;
		res = IOS_Ioctlv(fd, 0x08, 2, 0, v);	/* launch: reboots, if it works */
	}
	IOS_Close(fd);
out:
	so_free(id);
	so_free(count);
	so_free(views);
	so_free(v);
	return res;
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
	signalled = msg == EVENT_SIGNAL;
	if (timer >= 0)
		TimerDestroy(timer);
	/* Drop extra signals and a timeout that raced with the signal. */
	while (mqueue_recv(e->queue, &msg, 1) >= 0)
		signalled |= msg == EVENT_SIGNAL;
	/* Last: a later signal queues a new message. Cleared any sooner, a signal in between
	 * has its message dropped just above and leaves `pending` set with the queue empty, and
	 * no signal gets through again. For the sender thread, which waits with no timer, that
	 * was for good: the Wii sent nothing more, a game ended in a disconnect 7 s later (about
	 * 1 game in 18 at 25-30 ms). A signal that finds it still set here needs no message: its
	 * work was queued before it, and the caller looks for work after this returns. */
	e->pending = 0;
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

#define LOG_LINES 56
#define LOG_LINE_SIZE 160

static struct {
	char lines[LOG_LINES][LOG_LINE_SIZE];
	u32 head, tail;	/* head: next write; tail: next read */
	u32 dropped;
} log_ring;
static SoMutex log_lock;

static void log_line(const char *fmt, va_list ap, int to_file)
{
	char line[LOG_LINE_SIZE];
	u32 ms;
	int n;
	if (!platform_ready)
		return;
	ms = so_time_ms();
	n = so_snprintf(line, sizeof(line), "%u.%03u ", (unsigned)(ms / 1000), (unsigned)(ms % 1000));
	so_vsnprintf(line + n, sizeof(line) - n, fmt, ap);
	so_mutex_lock(&log_lock);
	if (log_ring.head - log_ring.tail >= LOG_LINES) {
		log_ring.tail++;	/* overwrite the oldest line */
		log_ring.dropped++;
	}
	strlcpy(log_ring.lines[log_ring.head % LOG_LINES], line, LOG_LINE_SIZE);
	log_ring.head++;
	so_mutex_unlock(&log_lock);
	if (to_file)
		so_logfile_line(line);	/* (the card's file, if there is to be one: so_logfile.c) */
}

void so_log(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	log_line(fmt, ap, 1);
	va_end(ap);
}

void so_log_private(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	log_line(fmt, ap, 0);
	va_end(ap);
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

/* The file added to stays open from one call to the next (the log's: so_logfile.c). Opening it and
 * finding its end again were most of a write's 88-120 ms (seen, an SD card); what a write still costs
 * is its sectors and f_sync's, after which the card has it whatever comes next. */
static FIL append_file;
static char append_path[48];

int so_append_file(const char *path, const void *buf, u32 size, int create)
{
	UINT done = 0;
	FRESULT res = FR_OK;
	so_fs_lock();
	if (create || strcmp(path, append_path)) {
		if (append_path[0]) {
			f_close(&append_file);
			append_path[0] = 0;
		}
		res = f_open_main_drive(&append_file, path, (create ? FA_CREATE_ALWAYS : FA_OPEN_ALWAYS) | FA_WRITE);
		if (res == FR_OK && !create)
			res = f_lseek(&append_file, f_size(&append_file));
		if (res != FR_OK) {
			so_fs_unlock();
			return -1;
		}
		strlcpy(append_path, path, sizeof(append_path));
	}
	res = f_write(&append_file, buf, size, &done);
	if (res == FR_OK)
		res = f_sync(&append_file);
	if (res != FR_OK) {
		f_close(&append_file);
		append_path[0] = 0;
	}
	so_fs_unlock();
	return res == FR_OK && done == size ? 0 : -2;
}

void so_make_dir(const char *path)
{
	so_fs_lock();
	f_mkdir_main_drive(path);	/* (there already: fine) */
	so_fs_unlock();
}

int so_platform_info(char *out, int size)
{
	/* (0x0D8005A0: the Wii U's mark in its Wii mode, as Nintendont's own check reads it) */
	return so_snprintf(out, size, "SlippWii %s on a %s, the game on %s; loader settings %08x, video %08x (TV format %u), "
		"memory card blocks %u, code options %u %u %u %u %u %u %u %u; %s\n",
		SLIPPWII_VERSION, (read32(0x0D8005A0) >> 16) == 0xCAFE ? "Wii U" : "Wii", ncfg->UseUSB ? "USB" : "SD",
		(unsigned)ncfg->Config, (unsigned)ncfg->VideoMode, (unsigned)so_ppc_word(0x000000CC),
		(unsigned)ncfg->MemCardBlocks, (unsigned)ncfg->MeleeCodeOptions[0], (unsigned)ncfg->MeleeCodeOptions[1],
		(unsigned)ncfg->MeleeCodeOptions[2], (unsigned)ncfg->MeleeCodeOptions[3], (unsigned)ncfg->MeleeCodeOptions[4],
		(unsigned)ncfg->MeleeCodeOptions[5], (unsigned)ncfg->MeleeCodeOptions[6], (unsigned)ncfg->MeleeCodeOptions[7],
		/* (what the picture is, for whoever reads the log: the engine's timing goes by it. Whether the
		 * engine has its memory is not known this early: "rollback engine ... at boot" says it later.) */
		(ncfg->Config & NIN_CFG_FORCE_PROG) || (ncfg->VideoMode & NIN_VID_PROG) ? "480p (progressive)" : "480i or 576i");
}

int so_diag_log_setting(void)
{
	return ConfigGetConfig(NIN_CFG_SW_DIAG_LOG);
}

char *getMainDrivePath(const char *path);	/* common.c */

u32 SlippWiiOptionalCodes(u8 *out, u32 room, int screen);	/* Patch.c */

u32 so_optional_codes(u8 *out, u32 room)
{
	return SlippWiiOptionalCodes(out, room, 1);
}

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

extern u32 iso_reads, iso_read_bytes, iso_read_ticks, iso_cache_hits;

extern u32 rs_cmds, rs_seeks, rs_bytes, rs_ticks;

void so_drive_log(const char *what, int reset)
{
	so_log("%s: %u reads, %u KB, %u ms reading; %u from the cache; paced %u commands (%u seeks), %u KB, %u ms",
		what, (unsigned)iso_reads, (unsigned)(iso_read_bytes >> 10), (unsigned)(iso_read_ticks / 1898),
		(unsigned)iso_cache_hits, (unsigned)rs_cmds, (unsigned)rs_seeks, (unsigned)(rs_bytes >> 10),
		(unsigned)(rs_ticks / 1898));
	if (reset) {
		rs_cmds = rs_seeks = rs_bytes = rs_ticks = 0;
		iso_reads = iso_read_bytes = iso_read_ticks = iso_cache_hits = 0;
		iso_ext_n = 0;
	}
}

/* Runs as "start ms before now +offset KB/reads ms": the start's age, the
 * disc offset (hex), its length, reads and read time */
int so_drive_runs_line(int n, const char *what)
{
	char line[150];
	u32 first = iso_ext_n > ISO_EXTENTS ? iso_ext_n - ISO_EXTENTS : 0, i, now = read32(HW_TIMER);
	int len;
	i = first + (u32)n * 4;
	if (i >= iso_ext_n)
		return 0;
	len = so_snprintf(line, sizeof(line), "%s %u:", what, (unsigned)i);
	for (; i < iso_ext_n && i < first + (u32)n * 4 + 4; i++) {
		IsoExtent *e = &iso_ext[i % ISO_EXTENTS];
		len += so_snprintf(line + len, sizeof(line) - len, " -%u@%x+%uK/%u/%u", (unsigned)((now - e->t_start) / 1898),
			(unsigned)e->offset, (unsigned)(e->len >> 10), (unsigned)e->reads, (unsigned)(e->ticks / 1898));
	}
	so_log("%s", line);
	return 1;
}
