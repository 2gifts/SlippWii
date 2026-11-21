/* The kernel's log on the SD card, for a problem report.
 *
 * The files, in /slippwii/logs/ on the game's drive:
 *
 *   crash-N.txt  When the game crashes (its crash report, or a panic, has come over the EXI), or a
 *                match stops with no pad poll for 5 s (a hang sends no report): this boot's last
 *                lines, the report and what follows. Always: the game has stopped, so a write
 *                costs nothing.
 *   desync-N.txt When a game goes out of step with an opponent's (a frame's checksum differs, the game
 *                gives an opponent up, or all three leave a game of four): this boot's last lines, what the game was (so_exi.c,
 *                so_exi_desync_service) and what follows to that game's end. Always: nothing else would be
 *                left of it. Written once the game is over, not in it: see so_logfile_service.
 *   log-N.txt    With the loader's Diagnostic Log switch on: every line of this boot, a crash's
 *                among them. Written between games (a second apart at most), never in one: it
 *                costs a game nothing (until 1.8.1 it was written every 5 s in a game, on the network
 *                thread: a hitch now and then).
 *
 * N goes round 1 to 8 for each kind (number.txt has the count, newest.txt the newest file's name),
 * and a file ends at 1 MB. Other problems (so_logfile_event: a freeze, a lost or failed connection, the
 * card's failed reads) are reports as a desync's is, named by their kind. reports.txt: a line for each
 * problem, with its file.
 *
 * Nothing is written to the card while a match runs, or while the disc is busy (so_logfile_quiet), but a
 * crash's file: the writes and syncs hold the FatFS lock the game's disc reads wait for, and once held
 * the network thread too. They are made by the kernel's lowest thread (so_diag.c), between games.
 *
 * Every line is kept in a ring of 32 KB as it is logged (so_logfile_line, from so_log). Without a
 * file the ring is only the last lines, for a crash to start from; with one, what the file has not
 * had yet. What a person would not post is taken out on the way in (so_logfile_scrub): the last
 * two numbers of an IPv4 address, a connect code but for its first character. Lines with a name
 * in them are not given to this at all (so_log_private), and the play key is in no line. */
#include "so_logfile.h"
#include "so_platform.h"
#include "so_alloc.h"
#include "so_util.h"
#include "so_config.h"
#include "so_client.h"
#include "string.h"

#define HISTORY 0x8000u	/* (a whole game's lines: none are written in it) */
#define CHUNK 2048u		/* a write (from the heap: 32-byte aligned, as the SD driver wants) */
#define FILE_MOST 0x100000u
#define SLOTS 8
#define IDLE_PERIOD_MS 1000
#define DIR "/slippwii/logs"

static struct {
	SoMutex lock;
	char *ring, *chunk;
	u32 head;	/* bytes ever logged */
	u32 written;	/* of them, those the file has (or that are gone) */
	u32 file_bytes, last_ms;
	u32 writes, write_us_max, write_us_all;	/* the card's writes this boot, the longest, all (so_logfile_stats) */
	u8 ready, diag, crash, now, opened, failed, lost, align;
	u8 desync;	/* the file wanted (crash) is a desync's: named so, and ended with its game */
	u8 ending;	/* its game is over: what is pending, and the file is done */
	u8 keep;	/* a crash has come: the file goes on whatever it started as */
	u8 index_n;
	char path[48];
	/* (on the heap: the kernel's data region is full) */
	struct {
		char name[16];	/* the next file's kind (so_logfile_event), or "" */
		char verdict[100];	/* its first line */
		char index[4][100];	/* lines for reports.txt, not written yet */
	} *r;
	u32 number;	/* the open file's */
	SoMutex wlock;	/* (one writer at a time: the watcher, the shutdown's last write) */
} lf;

int (*so_logfile_quiet)(void);

static int digit(char c) { return c >= '0' && c <= '9'; }
static int code_char(char c) { return digit(c) || (c >= 'A' && c <= 'Z'); }

u32 so_logfile_scrub(char *s)
{
	char *in = s, *out = s;
	while (*in) {
		if (digit(*in) && (in == s || (!digit(in[-1]) && in[-1] != '.'))) {
			/* four numbers of 1-3 digits with dots between: the first two stay */
			const char *p = in, *third = NULL;
			int part, ok = 1;
			for (part = 0; part < 4 && ok; part++) {
				int n = 0;
				while (digit(*p) && n < 3) {
					p++;
					n++;
				}
				if (!n || digit(*p))
					ok = 0;
				else if (part < 3) {
					if (*p != '.')
						ok = 0;
					else if (++p, part == 1)
						third = p;
				}
			}
			if (ok && *p != '.') {
				while (in < third)
					*out++ = *in++;
				*out++ = 'x';
				*out++ = '.';
				*out++ = 'x';
				in = (char *)p;
				continue;
			}
		}
		if (*in == '#' && in > s && code_char(in[-1]) && digit(in[1])) {
			/* a connect code (ABCD#123): its first character stays */
			char *q = out;
			while (q > s && code_char(q[-1]))
				q--;
			for (q++; q < out; q++)
				*q = '*';
			*out++ = *in++;
			while (digit(*in)) {
				*out++ = '*';
				in++;
			}
			continue;
		}
		*out++ = *in++;
	}
	*out = 0;
	return (u32)(out - s);
}

void so_logfile_init(int diag)
{
	if (lf.ready)
		return;
	lf.ring = (char *)so_malloc(HISTORY);
	lf.chunk = (char *)so_malloc(CHUNK);
	lf.r = so_malloc(sizeof(*lf.r));
	if (lf.r)
		memset(lf.r, 0, sizeof(*lf.r));
	if (!lf.ring || !lf.chunk || !lf.r || so_mutex_init(&lf.lock) < 0 || so_mutex_init(&lf.wlock) < 0)
		return;	/* (no room: no file, and the game is as ever) */
	lf.diag = diag != 0;
	lf.ready = 1;
}

int so_logfile_diag(void)
{
	return lf.ready && lf.diag && !lf.failed;
}

void so_logfile_line(const char *line)
{
	char copy[224];
	u32 n, i;
	if (!lf.ready)
		return;
	strlcpy(copy, line, sizeof(copy) - 1);
	n = so_logfile_scrub(copy);
	copy[n++] = '\n';
	so_mutex_lock(&lf.lock);
	for (i = 0; i < n; i++)
		lf.ring[(lf.head + i) % HISTORY] = copy[i];
	lf.head += n;
	if (lf.head - lf.written > HISTORY) {	/* the oldest bytes are gone: on from a whole line */
		if (lf.diag || lf.crash)
			lf.lost = 1;
		lf.written = lf.head - HISTORY;
		lf.align = 1;
	}
	so_mutex_unlock(&lf.lock);
}

void so_logfile_crash(void)
{
	if (!lf.ready)
		return;
	so_mutex_lock(&lf.lock);
	if (!lf.diag)
		lf.crash = 1;	/* (from the ring's oldest line: `written` is there) */
	if (!lf.opened) {
		lf.desync = 0;	/* (a crash's name; one that is a desync's already keeps it) */
		lf.r->name[0] = 0;
	}
	lf.ending = 0;
	lf.keep = 1;	/* (and goes on) */
	lf.now = 1;
	so_mutex_unlock(&lf.lock);
}

void so_logfile_desync(void)
{
	if (!lf.ready)
		return;
	so_mutex_lock(&lf.lock);
	if (!lf.diag && !lf.crash) {
		lf.crash = 1;
		lf.desync = 1;
	}
	lf.now = 1;
	so_mutex_unlock(&lf.lock);
}

void so_logfile_report_end(void)
{
	if (!lf.ready)
		return;
	so_mutex_lock(&lf.lock);
	if (lf.crash && lf.desync && !lf.keep) {
		lf.ending = 1;
		lf.now = 1;
	}
	so_mutex_unlock(&lf.lock);
}

void so_logfile_event(const char *kind, const char *text, int ended)
{
	char line[sizeof(lf.r->verdict)];
	if (!lf.ready)
		return;
	so_log("REPORT %s: %s", kind, text);
	so_snprintf(line, sizeof(line), "%s: %s", kind, text);
	so_logfile_scrub(line);
	so_mutex_lock(&lf.lock);
	if (!lf.opened && !lf.diag && !lf.r->verdict[0]) {
		strlcpy(lf.r->verdict, line, sizeof(lf.r->verdict));
		if (!lf.crash)	/* (a crash's file keeps its name) */
			strlcpy(lf.r->name, kind, sizeof(lf.r->name));
	}
	if (lf.index_n < 4)
		strlcpy(lf.r->index[lf.index_n++], line, sizeof(lf.r->index[0]));
	if (!lf.diag && !lf.crash) {
		lf.crash = 1;
		lf.desync = 1;
	}
	if (ended && lf.crash && lf.desync && !lf.keep)
		lf.ending = 1;
	lf.now = 1;
	so_mutex_unlock(&lf.lock);
}

void so_logfile_now(void)
{
	if (lf.ready)
		lf.now = 1;
}

/* A new file: its number, its name, its first lines */
static int open_file(void)
{
	char text[16];
	const SoConfig *config = so_client_config();
	const char *kind = lf.diag ? "log" : lf.r->name[0] ? lf.r->name : lf.desync ? "desync" : "crash";
	const char *what = lf.diag ? "diagnostic log" : "report";
	u32 number = 0, i;
	int n, got;
	so_make_dir(DIR);
	got = so_read_file(DIR "/number.txt", text, sizeof(text) - 1);
	if (got > 0) {
		text[got] = 0;
		for (i = 0; digit(text[i]); i++)
			number = number * 10 + (u32)(text[i] - '0');
	}
	number++;
	n = so_snprintf(text, sizeof(text), "%u\n", (unsigned)number);
	so_write_file(DIR "/number.txt", text, (u32)n);
	so_snprintf(lf.path, sizeof(lf.path), DIR "/%s-%u.txt", kind, (unsigned)((number - 1) % SLOTS + 1));
	n = so_snprintf(lf.chunk, CHUNK, "SlippWii %s%s%s, number %u on this card\n", lf.diag ? "" : kind, lf.diag ? "" : " ", what,
		(unsigned)number);
	if (lf.r->verdict[0])
		n += so_snprintf(lf.chunk + n, CHUNK - n, "VERDICT %s\n", lf.r->verdict);
	n += so_platform_info(lf.chunk + n, (int)(CHUNK - n));
	if (config)
		n += so_snprintf(lf.chunk + n, CHUNK - n, "online: delay %u, rollback %u, direct %u (under %u ms), background saves %u, "
			"input wait %u ms\n", (unsigned)config->delay, (unsigned)config->rollback, (unsigned)config->rollback_direct,
			(unsigned)config->rollback_direct_ping_ms, (unsigned)config->rollback_bg_saves, (unsigned)config->rollback_wait_ms);
	{
		u32 failed, slow;
		SoAllocStats mem;
		so_disc_counters(&failed, &slow);
		so_alloc_stats(&mem);
		n += so_snprintf(lf.chunk + n, CHUNK - n, "this boot: %u s; the card's writes %u (the longest %u ms), its failed reads %u, "
			"disc commands over half a second %u; kernel heap peak %u KB, %u allocations failed\n",
			(unsigned)(so_time_ms() / 1000), (unsigned)lf.writes, (unsigned)(lf.write_us_max / 1000), (unsigned)failed,
			(unsigned)slow, (unsigned)(mem.peak / 1024), (unsigned)mem.failures);
	}
	n += so_snprintf(lf.chunk + n, CHUNK - n, "Addresses and connect codes are masked; names and the play key are never written.\n"
		"Each line starts with the seconds since the game was started.\n\n");
	if (so_append_file(lf.path, lf.chunk, (u32)n, 1) < 0)
		return 0;
	lf.file_bytes = (u32)n;
	lf.opened = 1;
	lf.number = number;
	/* (said in the log, so in the file too: which file this is) */
	so_log("the %s: on the card as %s (number %u)", what, lf.path, (unsigned)number);
	n = so_snprintf(lf.chunk, CHUNK, "The newest file in this folder is %s (number %u).\r\n"
		"To report a problem, attach this whole folder.\r\n", lf.path + sizeof(DIR), (unsigned)number);
	so_write_file(DIR "/newest.txt", lf.chunk, (u32)n);
	return 1;
}

/* reports.txt: the problems said since the last write, each with its file */
static void write_index(void)
{
	char line[sizeof(lf.r->verdict) + 40];
	for (;;) {
		int n, k;
		so_mutex_lock(&lf.lock);
		if (!lf.index_n) {
			so_mutex_unlock(&lf.lock);
			return;
		}
		n = so_snprintf(line, sizeof(line), "%u %s %s\n", (unsigned)lf.number, lf.path + sizeof(DIR), lf.r->index[0]);
		lf.index_n--;
		for (k = 0; k < lf.index_n; k++)
			memcpy(lf.r->index[k], lf.r->index[k + 1], sizeof(lf.r->index[0]));
		so_mutex_unlock(&lf.lock);
		so_append_file(DIR "/reports.txt", line, (u32)n, 0);
	}
}

static int service(int now_anyway);

static int may_write(void)
{
	return lf.keep || !so_logfile_quiet || so_logfile_quiet();
}

int so_logfile_service(int quiet, int now_anyway)
{
	int wrote;
	if (!lf.ready || lf.failed || (!lf.diag && !lf.crash))
		return 0;
	if (!quiet && !lf.keep) {
		if (now_anyway)
			lf.now = 1;	/* (still wanted, when the card may be written) */
		return 0;
	}
	so_mutex_lock(&lf.wlock);
	wrote = service(now_anyway);
	so_mutex_unlock(&lf.wlock);
	return wrote;
}

static int service(int now_anyway)
{
	u32 pending, now;
	int wrote = 0, ending;
	so_mutex_lock(&lf.lock);
	pending = lf.head - lf.written;
	if (lf.now)
		now_anyway = 1;
	lf.now = 0;
	ending = lf.ending;
	so_mutex_unlock(&lf.lock);
	/* (Before 1.8.1 a desync's report and the Diagnostic Log were written in the game, on the network
	 * thread: no input sent or taken during the card's writes and syncs, and between them the game's disc
	 * reads held, the music's stream looping. On a slow card, seconds: a frozen game, a lost connection.) */
	now = so_time_ms();
	if (lf.opened && !pending && !ending) {
		write_index();
		return 0;
	}
	if (lf.opened && !now_anyway && pending < HISTORY / 2 && now - lf.last_ms < IDLE_PERIOD_MS)
		return 0;
	if (!lf.opened && !open_file()) {
		lf.failed = 1;	/* (no card, or it is full or locked: nothing more is tried) */
		return 0;
	}
	for (;;) {
		u32 n = 0;
		if (!may_write()) {
			lf.now = 1;	/* (a match or a load has begun: the rest after it) */
			return wrote;
		}
		so_mutex_lock(&lf.lock);
		if (lf.lost) {
			n = (u32)so_snprintf(lf.chunk, CHUNK, "(lines are missing here: more was logged than the buffer holds between two writes)\n");
			lf.lost = 0;
		}
		if (lf.align) {
			while (lf.written != lf.head && lf.ring[lf.written % HISTORY] != '\n')
				lf.written++;
			if (lf.written != lf.head)
				lf.written++;
			lf.align = 0;
		}
		while (n < CHUNK && lf.written != lf.head)
			lf.chunk[n++] = lf.ring[lf.written++ % HISTORY];
		so_mutex_unlock(&lf.lock);
		if (!n)
			break;
		{
			u64 t0 = so_time_us();
			int res = so_append_file(lf.path, lf.chunk, n, 0);
			u32 took = (u32)(so_time_us() - t0);
			lf.writes++;
			lf.write_us_all += took;
			if (took > lf.write_us_max)
				lf.write_us_max = took;
			if (res < 0) {
				lf.failed = 1;
				break;
			}
		}
		wrote += (int)n;
		lf.file_bytes += n;
		if (lf.file_bytes > FILE_MOST) {
			n = (u32)so_snprintf(lf.chunk, CHUNK, "(the file has its megabyte: nothing more is written this time)\n");
			so_append_file(lf.path, lf.chunk, n, 0);
			lf.failed = 1;
			break;
		}
	}
	lf.last_ms = so_time_ms();
	write_index();
	if (ending) {
		/* a desync's game is over and its lines are in the file: no writes in the games after it;
		 * the next report is a new file, from the lines after this one's */
		so_mutex_lock(&lf.lock);
		if (lf.ending && lf.written == lf.head) {
			lf.ending = lf.desync = lf.crash = lf.opened = 0;
			lf.r->name[0] = lf.r->verdict[0] = 0;
		}
		so_mutex_unlock(&lf.lock);
	}
	return wrote;
}

const char *so_logfile_path(void)
{
	return lf.opened ? lf.path : NULL;
}

void so_logfile_stats(u32 *writes, u32 *longest_us, u32 *all_us)
{
	*writes = lf.writes;
	*longest_us = lf.write_us_max;
	*all_us = lf.write_us_all;
}

#ifdef SO_LOGFILE_TEST
void so_logfile_test_reset(void)
{
	memset(&lf, 0, sizeof(lf));	/* (the test gives the heap anew before so_logfile_init) */
}
#endif
