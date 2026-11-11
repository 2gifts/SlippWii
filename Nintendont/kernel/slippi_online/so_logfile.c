/* The kernel's log on the SD card, for a problem report.
 *
 * Two files, in /slippwii/logs/ on the game's drive:
 *
 *   crash-N.txt  When the game crashes (its crash report, or a panic, has come over the EXI), or a
 *                match stops with no pad poll for 5 s (a hang sends no report): this boot's last
 *                lines, the report and what follows. Always: the game has stopped, so a write
 *                costs nothing.
 *   log-N.txt    With the loader's Diagnostic Log switch on: every line of this boot, a crash's
 *                among them. Written between games, every 5 s in one, and at once when something is
 *                wrong (so_logfile_now). A write is the SD card's business for some milliseconds,
 *                on the network thread, where a pad may wait for it: a hitch now and then, which is
 *                why it is a switch.
 *
 * N goes round 1 to 8 for each kind (number.txt has the count, newest.txt the newest file's name),
 * and a file ends at 1 MB.
 *
 * Every line is kept in a ring of 16 KB as it is logged (so_logfile_line, from so_log). Without a
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

#define HISTORY 0x4000u
#define CHUNK 2048u		/* a write (from the heap: 32-byte aligned, as the SD driver wants) */
#define FILE_MOST 0x100000u
#define SLOTS 8
#define GAME_PERIOD_MS 5000
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
	char path[48];
} lf;

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
	if (!lf.ring || !lf.chunk || so_mutex_init(&lf.lock) < 0)
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
	so_snprintf(lf.path, sizeof(lf.path), DIR "/%s-%u.txt", lf.diag ? "log" : "crash", (unsigned)((number - 1) % SLOTS + 1));
	n = so_snprintf(lf.chunk, CHUNK, "SlippWii %s, number %u on this card\n", lf.diag ? "diagnostic log" : "crash report",
		(unsigned)number);
	n += so_platform_info(lf.chunk + n, (int)(CHUNK - n));
	if (config)
		n += so_snprintf(lf.chunk + n, CHUNK - n, "online: delay %u, rollback %u, direct %u (under %u ms), background saves %u, "
			"input wait %u ms\n", (unsigned)config->delay, (unsigned)config->rollback, (unsigned)config->rollback_direct,
			(unsigned)config->rollback_direct_ping_ms, (unsigned)config->rollback_bg_saves, (unsigned)config->rollback_wait_ms);
	n += so_snprintf(lf.chunk + n, CHUNK - n, "Addresses and connect codes are masked; names and the play key are never written.\n"
		"Each line starts with the seconds since the game was started.\n\n");
	if (so_append_file(lf.path, lf.chunk, (u32)n, 1) < 0)
		return 0;
	lf.file_bytes = (u32)n;
	lf.opened = 1;
	/* (said in the log, so in the file too: which file this is) */
	so_log("%s: on the card as %s (number %u)", lf.diag ? "the diagnostic log" : "the crash report", lf.path, (unsigned)number);
	n = so_snprintf(lf.chunk, CHUNK, "The newest file in this folder is %s (number %u).\r\n"
		"To report a problem, attach this whole folder.\r\n", lf.path + sizeof(DIR), (unsigned)number);
	so_write_file(DIR "/newest.txt", lf.chunk, (u32)n);
	return 1;
}

int so_logfile_service(int in_game, int now_anyway)
{
	u32 pending, now;
	int wrote = 0;
	if (!lf.ready || lf.failed || (!lf.diag && !lf.crash))
		return 0;
	so_mutex_lock(&lf.lock);
	pending = lf.head - lf.written;
	if (lf.now)
		now_anyway = 1;
	lf.now = 0;
	so_mutex_unlock(&lf.lock);
	now = so_time_ms();
	if (lf.opened && !pending)
		return 0;
	if (lf.opened && !now_anyway && pending < HISTORY / 2 &&
	    now - lf.last_ms < (u32)(in_game ? GAME_PERIOD_MS : IDLE_PERIOD_MS))
		return 0;
	if (!lf.opened && !open_file()) {
		lf.failed = 1;	/* (no card, or it is full or locked: nothing more is tried) */
		return 0;
	}
	for (;;) {
		u32 n = 0;
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
