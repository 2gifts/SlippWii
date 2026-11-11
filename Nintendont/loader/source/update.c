/* SlippWii: "Check for Updates" (update.h).
 *
 * Asks GitHub what the latest release is and, if it is newer than this one, puts it on the card:
 * the same zip a person would extract there from a PC. The work is in update/upd_core.c and
 * upd_http.c (which tools/test_update.py tests on a PC); this file is the Wii's part: the network
 * (libogc's, through IOS: the wired LAN adapter as Wii Settings has it), the card (FatFs) and the
 * screen.
 *
 * Nothing leaves the console but the requests for the two files, to GitHub, and only when the
 * person asks. The account, the settings and the logs are never touched (upd_name_allowed).
 */
#include <gccore.h>
#include <network.h>
#include <ogc/lwp_watchdog.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "global.h"
#include "font.h"
#include "FPad.h"
#include "menu.h"
#include "ff_utf8.h"
#include "diskio.h"
#include "update.h"
#include "update/upd_core.h"
#include "update/upd_key.h"
/* Empty in every real build. A test build (tools/update_wii_test.py) has a key made for the run
 * here, and with it the launch argument that names the test's server and folder. */
#include "update_test.inc"

#define MANIFEST_URL "https://github.com/2gifts/SlippWii/releases/latest/download/update.txt"
#define AGENT "SlippWii/" SLIPPWII_VERSION
/* (tests can say which version this is, to be offered a release that is not newer than the build) */
#ifdef UPD_TEST_VERSION
#define THIS_VERSION UPD_TEST_VERSION
#else
#define THIS_VERSION SLIPPWII_VERSION
#endif

#define NET_START_MS 20000
#define CONNECT_MS 10000
#define IDLE_MS 20000	/* a connection that says nothing for this long is given up */

extern FATFS *devices[2];
extern char launch_dir[];	/* main.c: where this app was started from ("sd:/apps/SlippWii/") */
extern u32 Shutdown;		/* menu.c */

const char *SlippWiiUpdateTest;	/* (update.h) */

/* ---- the platform (upd_core.h) ---- */

static u32 ms_since(s64 t)
{
	return (u32)ticks_to_millisecs(diff_ticks(t, gettime()));
}

int upd_plat_connect(const char *host, uint16_t port)
{
	struct sockaddr_in sa;
	struct in_addr address;
	s64 t = gettime();
	s32 s, res;

	if (!inet_aton(host, &address)) {	/* (tests name their server by its numbers) */
		struct hostent *hp = net_gethostbyname(host);
		if (!hp || hp->h_addrtype != PF_INET || !hp->h_addr_list || !hp->h_addr_list[0])
			return -1;	/* no such name: no DNS, or no internet */
		memcpy(&address, hp->h_addr_list[0], sizeof(address));
	}
	s = net_socket(PF_INET, SOCK_STREAM, 0);
	if (s < 0)
		return -2;
	res = net_fcntl(s, F_GETFL, 0);
	if (res < 0 || net_fcntl(s, F_SETFL, res | 4) < 0) {	/* (4: non-blocking, as IOS has it) */
		net_close(s);
		return -3;
	}
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = PF_INET;
	sa.sin_len = sizeof(sa);
	sa.sin_port = htons(port);
	sa.sin_addr = address;
	for (;;) {
		res = net_connect(s, (struct sockaddr *)&sa, sizeof(sa));
		if (res >= 0 || res == -EISCONN)
			return s;
		if ((res != -EINPROGRESS && res != -EALREADY) || ms_since(t) > CONNECT_MS) {
			net_close(s);
			return -4;
		}
		usleep(20 * 1000);
	}
}

int upd_plat_recv(int s, uint8_t *buf, int len)
{
	s64 t = gettime();
	for (;;) {
		s32 n = net_recv(s, buf, len, 0);
		if (n != -EAGAIN)
			return n;
		if (ms_since(t) > IDLE_MS)
			return -ETIMEDOUT;
		usleep(1000);
	}
}

int upd_plat_send(int s, const uint8_t *buf, int len)
{
	s64 t = gettime();
	for (;;) {
		s32 n = net_send(s, buf, len, 0);
		if (n != -EAGAIN && n != 0)
			return n;
		if (ms_since(t) > IDLE_MS)
			return -ETIMEDOUT;
		usleep(1000);
	}
}

void upd_plat_close(int s)
{
	net_close(s);
}

/* (the SD and USB drivers take a buffer as it is only when it is aligned to 32 bytes) */
void *upd_plat_alloc(uint32_t size)
{
	return memalign(32, (size + 32) & ~31u);
}

void upd_plat_free(void *p)
{
	free(p);
}

/* A Wii has no source of random numbers to ask. What varies from one run to the next: the clocks,
 * where the beam is, what the heap and this stack held. It keeps two downloads' keys apart; nothing
 * that is installed is trusted because of it. */
void upd_plat_entropy(uint8_t out[32])
{
	static u32 calls;
	struct {
		s64 ticks, more;
		time_t now;
		u32 line, field, calls, ip;
		void *heap, *stack;
		u32 junk[8];
	} seed;
	seed.ticks = gettime();
	seed.now = time(NULL);
	seed.line = VIDEO_GetCurrentLine();
	seed.field = VIDEO_GetNextField();
	seed.calls = ++calls;
	seed.ip = net_gethostip();
	seed.heap = malloc(16);
	free(seed.heap);
	seed.stack = &seed;
	seed.more = gettime();
	upd_sha256((const uint8_t *)&seed, sizeof(seed), out);
}

/* ---- the card ---- */

static const char *app_dev, *data_dev;	/* "sd" or "usb": where the app is, where /slippwii is */
static char root[64];			/* tests: a folder to install into, in place of the card's root */

static void card_path(const char *rel, char *out, size_t size)
{
	snprintf(out, size, "%s:%s/%s", strncmp(rel, "apps/", 5) == 0 ? app_dev : data_dev, root, rel);
}

static void wide(const char *s, WCHAR *out, size_t count)
{
	size_t i;
	for (i = 0; i + 1 < count && s[i]; i++)
		out[i] = (unsigned char)s[i];
	out[i] = 0;
}

/* What these return when they fail is shown with the file's name: -(100 * the step) - FatFs's own
 * result (ff.h, FRESULT), steps 1 open, 2 write or read, 3 close; -1 for memory not aligned, -2 for
 * a short write, -3 when the cache's pages did not reach the drive. */
#define CARD_PIECE 0x10000

static int card_write(void *ctx, const char *rel, const uint8_t *data, uint32_t len)
{
	char path[200], *slash;
	FRESULT res;
	uint32_t at;
	FIL f;

	if ((u32)data & 31)
		return -1;	/* (upd_plat_alloc's memory is aligned) */
	card_path(rel, path, sizeof(path));
	for (slash = strchr(strchr(path, ':') + 2, '/'); slash; slash = strchr(slash + 1, '/')) {
		*slash = 0;
		f_mkdir_char(path);	/* (there already, most times) */
		*slash = '/';
	}
	res = f_open_char(&f, path, FA_WRITE | FA_CREATE_ALWAYS);
	if (res != FR_OK)
		return -100 - (int)res;
	for (at = 0; at < len; at += CARD_PIECE) {
		UINT n = len - at < CARD_PIECE ? len - at : CARD_PIECE, wrote = 0;
		res = f_write(&f, data + at, n, &wrote);
		if (res != FR_OK || wrote != n) {
			f_close(&f);
			return res != FR_OK ? -200 - (int)res : -2;
		}
	}
	res = f_close(&f);
	return res == FR_OK ? 0 : -300 - (int)res;
}

static int card_crc(void *ctx, const char *rel, uint32_t *crc, uint32_t *len)
{
	static u8 piece[CARD_PIECE] ATTRIBUTE_ALIGN(32);
	char path[200];
	FRESULT res;
	uint32_t sum = 0, size = 0;
	UINT got;
	FIL f;

	card_path(rel, path, sizeof(path));
	/* What the drive itself has: the loader's disk cache (diskio.c) writes its pages out and forgets
	 * them first, or the last of a file would be read back from memory. */
	if (disk_forget(path[0] == 'u' ? DEV_USB : DEV_SD) != RES_OK)
		return -3;
	res = f_open_char(&f, path, FA_READ | FA_OPEN_EXISTING);
	if (res != FR_OK)
		return -100 - (int)res;
	do {
		got = 0;
		res = f_read(&f, piece, sizeof(piece), &got);
		if (res != FR_OK) {
			f_close(&f);
			return -200 - (int)res;
		}
		sum = upd_crc32_more(sum, piece, got);
		size += got;
	} while (got == sizeof(piece));
	f_close(&f);
	*crc = sum;
	*len = size;
	return 0;
}

static int card_rename(void *ctx, const char *from, const char *to)
{
	char a[200], b[200];
	WCHAR wa[200], wb[200];
	card_path(from, a, sizeof(a));
	card_path(to, b, sizeof(b));
	wide(a, wa, 200);
	wide(b, wb, 200);
	return f_rename(wa, wb) == FR_OK ? 0 : -1;
}

static int card_remove(void *ctx, const char *rel)
{
	char path[200];
	WCHAR w[200];
	card_path(rel, path, sizeof(path));
	wide(path, w, 200);
	f_unlink(w);
	return 0;
}

static const UpdFs card = { card_write, card_crc, card_rename, card_remove, NULL };

static void pick_devices(void)
{
	data_dev = devices[DEV_SD] ? "sd" : "usb";
	app_dev = data_dev;
	if (strncmp(launch_dir, "usb:", 4) == 0 && devices[DEV_USB])
		app_dev = "usb";
	else if (strncmp(launch_dir, "sd:", 3) == 0 && devices[DEV_SD])
		app_dev = "sd";
}

/* ---- the screen ---- */

#define MAX_LINES 16

static struct {
	char text[64];
	u32 color;
} lines[MAX_LINES];
static int line_count;
static int unattended;	/* tests: nobody is there to press a button */

static void screen_clear(void)
{
	line_count = 0;
}

static void say(u32 color, const char *fmt, ...)
{
	va_list ap;
	if (line_count == MAX_LINES)
		return;
	va_start(ap, fmt);
	vsnprintf(lines[line_count].text, sizeof(lines[line_count].text), fmt, ap);
	va_end(ap);
	lines[line_count].color = color;
	line_count++;
}

static void screen_draw(const char *btn_a, const char *btn_b)
{
	int i;
	PrintBuildInfo();
	PrintButtonActions(NULL, btn_a, btn_b, NULL);
	PrintFormat(DEFAULT_SIZE, DARK_BLUE, MENU_POS_X, MENU_POS_Y + 20 * 3 + 10, "Check for Updates");
	for (i = 0; i < line_count; i++)
		PrintFormat(DEFAULT_SIZE, lines[i].color, MENU_POS_X, MENU_POS_Y + 20 * (5 + i), "%s", lines[i].text);
	GRRLIB_Render();
	ClearScreen();
}

/* Shows the screen until A (1) or B (0) is pressed. (tests: 4 s of it, then A.) */
static int screen_ask(const char *btn_a, const char *btn_b)
{
	int frames = 0;
	while (1) {
		VIDEO_WaitVSync();
		FPAD_Update();
		if (Shutdown)
			LoaderShutdown();
		if (frames++ % 30 == 0)
			screen_draw(btn_a, btn_b);
		if (unattended && frames > 240)
			return 1;
		if (unattended)
			continue;
		if (frames > 20 && FPAD_OK(0))
			return 1;
		if (frames > 20 && btn_b && FPAD_Cancel(0))
			return 0;
	}
}

/* A line that changes while something runs (the last line said). */
static void progress_line(void *arg, uint32_t done, uint32_t total)
{
	static s64 drawn;
	const char *what = (const char *)arg;
	if (done != total && ms_since(drawn) < 200)
		return;
	drawn = gettime();
	if (total)
		snprintf(lines[line_count - 1].text, sizeof(lines[0].text), "%s %u%%", what, (unsigned)(done * 100ull / total));
	else
		snprintf(lines[line_count - 1].text, sizeof(lines[0].text), "%s %u KB", what, (unsigned)(done / 1024));
	screen_draw(NULL, NULL);
	if (Shutdown)
		LoaderShutdown();
}

static void step(const char *fmt, ...)
{
	va_list ap;
	char text[64];
	va_start(ap, fmt);
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	say(BLACK, "%s", text);
	screen_draw(NULL, NULL);
}

/* ---- the network ---- */

static int net_up;

static int network_start(void)
{
	s64 t = gettime();
	s32 res;
	if (net_up)
		return 0;
	net_init_async(NULL, NULL);
	while ((res = net_get_status()) == -EBUSY && ms_since(t) < NET_START_MS)
		usleep(50 * 1000);
	if (res < 0 || net_gethostip() == 0)
		return res < 0 ? res : -1;
	net_up = 1;
	return 0;
}

/* ---- the update ---- */

static uint8_t *kept_zip;	/* tests: the zip that was installed */
static uint32_t kept_len;

typedef struct {
	int result;		/* UPD_OK: installed and read back; 1: nothing to do; else the step's error */
	char step[12];		/* what failed: "network", "check", "download", "install", "verify" */
	char latest[24];
	int files, verified;
	u32 net_ms, check_ms, download_ms, install_ms;
	char err[100];
} Outcome;

/* The whole of it. `key` and `flags` are the release's (upd_key.h, 0) outside tests. Returns when
 * the person is done with it; an installed update ends at the Homebrew Channel instead (the app in
 * memory is the old one), unless `stay`. */
static void run(const char *url, const unsigned char *key, int flags, int stay, Outcome *o)
{
	UpdManifest m;
	uint8_t *zip = NULL;
	s64 t;
	int res, i;

	memset(o, 0, sizeof(*o));
	pick_devices();
	screen_clear();
	step("Connecting to the network...");
	t = gettime();
	res = network_start();
	o->net_ms = ms_since(t);
	if (res < 0) {
		o->result = res;
		strcpy(o->step, "network");
		screen_clear();
		say(MAROON, "No network connection (%d).", res);
		say(BLACK, "");
		say(BLACK, "SlippWii needs the wired LAN adapter, set up in");
		say(BLACK, "Wii Settings > Internet. Check its cable and try again.");
		screen_ask("Back", NULL);
		return;
	}

	step("Asking GitHub for the latest version...");
	t = gettime();
	res = upd_check(url, key, flags, AGENT, &m, o->err, sizeof(o->err));
	o->check_ms = ms_since(t);
	if (res != UPD_OK) {
		o->result = res;
		strcpy(o->step, "check");
		screen_clear();
		if (res == 404) {
			say(BLACK, "The latest release on GitHub has no update");
			say(BLACK, "information. To update, get its zip with a computer:");
		} else {
			say(MAROON, "Could not check for updates:");
			say(BLACK, "%.60s", o->err);
			say(BLACK, "");
			say(BLACK, "Try again later, or get the zip with a computer:");
		}
		say(BLACK, "github.com/2gifts/SlippWii/releases");
		screen_ask("Back", NULL);
		return;
	}
	strcpy(o->latest, m.version);
	if (upd_version_cmp(m.version, THIS_VERSION) <= 0) {
		o->result = 1;
		screen_clear();
		say(BLACK, "SlippWii %s is the latest version.", THIS_VERSION);
		if (strcmp(m.version, THIS_VERSION) != 0)
			say(BLACK, "(The latest release is %s.)", m.version);
		screen_ask("Back", NULL);
		return;
	}

	screen_clear();
	say(BLACK, "SlippWii %s is available. You have %s.", m.version, THIS_VERSION);
	say(BLACK, "");
	for (i = 0; i < UPD_NOTES; i++)
		if (m.notes[i][0])
			say(BLACK, "%.62s", m.notes[i]);
	say(BLACK, "");
	say(BLACK, "A: install it now (%u KB). Your account and your", (unsigned)(m.zip_size / 1024));
	say(BLACK, "   settings are kept.");
	say(BLACK, "B: not now");
	if (!screen_ask("Install", "Not now")) {
		o->result = 1;
		return;
	}

	screen_clear();
	say(BLACK, "Installing SlippWii %s.", m.version);
	say(MAROON, "Do not turn the console off or take the card out.");
	say(BLACK, "");
	say(BLACK, "Downloading...");
	screen_draw(NULL, NULL);
	t = gettime();
	res = upd_download(&m, flags, AGENT, &zip, progress_line, "Downloading...", o->err, sizeof(o->err));
	o->download_ms = ms_since(t);
	if (res != UPD_OK) {
		o->result = res;
		strcpy(o->step, "download");
		screen_clear();
		say(MAROON, "The download failed:");
		say(BLACK, "%.60s", o->err);
		say(BLACK, "");
		say(BLACK, "Nothing on the card was changed. Try again later.");
		screen_ask("Back", NULL);
		return;
	}

	say(BLACK, "Writing the files...");
	screen_draw(NULL, NULL);
	t = gettime();
	res = upd_install(zip, m.zip_size, &card, progress_line, "Writing the files...", &o->files, o->err, sizeof(o->err));
	if (res == UPD_OK) {
		FlushDevices();
		o->verified = upd_verify(zip, m.zip_size, &card);
		if (o->verified != o->files) {
			res = UPD_E_SWAP;
			snprintf(o->err, sizeof(o->err), "%d of %d files read back right", o->verified, o->files);
		}
	}
	o->install_ms = ms_since(t);
	FlushDevices();
	if (res != UPD_OK) {
		o->result = res;
		strcpy(o->step, o->verified != o->files && o->files ? "verify" : "install");
		screen_clear();
		say(MAROON, "The update could not be installed:");
		say(BLACK, "%.60s", o->err);
		say(BLACK, "");
		if (res == UPD_E_SWAP) {
			say(BLACK, "Some files on the card are new and some are old.");
			say(BLACK, "Run Check for Updates again. If SlippWii does not");
			say(BLACK, "start, extract the release's zip onto the card with");
			say(BLACK, "a computer: github.com/2gifts/SlippWii/releases");
		} else {
			say(BLACK, "Nothing on the card was changed. If the card is");
			say(BLACK, "full or locked, fix that and try again.");
		}
		upd_plat_free(zip);
		screen_ask("Back", NULL);
		return;
	}
	if (stay) {	/* (tests: the zip says which files to take off the card again) */
		kept_zip = zip;
		kept_len = m.zip_size;
	} else {
		upd_plat_free(zip);
	}
	o->result = UPD_OK;
	screen_clear();
	say(BLACK, "SlippWii %s is installed (%d files).", m.version, o->files);
	say(BLACK, "");
	say(BLACK, "A: back to the Homebrew Channel. Start SlippWii");
	say(BLACK, "   again there, and it is the new version.");
	say(BLACK, "");
	say(BLACK, "The first start takes a little longer, as after");
	say(BLACK, "every update.");
	screen_ask("Done", NULL);
	if (stay)
		return;
	screen_clear();
	say(BLACK, "Going back...");
	screen_draw(NULL, NULL);
	ExitToLoader(0);
}

void SlippWii_Update(void)
{
	Outcome o;
	root[0] = 0;
	unattended = 0;
	run(MANIFEST_URL, upd_release_key, 0, 0, &o);
	ClearScreen();
}

#ifdef UPD_TEST_KEY
/* Tests (tools/update_wii_test.py): the launch argument "slippwii-update-test=URL;FOLDER" runs the
 * update by itself, from the test's server and with the test's key, into FOLDER on the card in
 * place of its root; tells the server how it went (a request for /report/...); takes FOLDER's
 * files off the card again; and goes back to the Homebrew Channel. */
static const unsigned char test_key[65] = { UPD_TEST_KEY };

static void test_cleanup(void)
{
	static const char *const folders[] = { "apps/SlippWii", "apps", "slippwii/files", "slippwii/patches", "slippwii", "" };
	char path[200];
	WCHAR w[200];
	unsigned i, d;
	for (d = 0; d < 2; d++) {
		const char *dev = d ? "usb" : "sd";
		if (!devices[d ? DEV_USB : DEV_SD] || !root[0])
			continue;
		for (i = 0; i < sizeof(folders) / sizeof(folders[0]); i++) {
			snprintf(path, sizeof(path), "%s:%s%s%s", dev, root, folders[i][0] ? "/" : "", folders[i]);
			wide(path, w, 200);
			f_unlink(w);	/* (a folder goes only when it is empty) */
		}
	}
	FlushDevices();
}

int SlippWii_UpdateTest(void)
{
	static char url[200], report[400], text[160];
	static uint8_t answer[64];
	char *part, *next, *folder = NULL, *tell = NULL;
	uint32_t got;
	Outcome o;
	int left = 0, keep = 0;
	unsigned i;

	if (!SlippWiiUpdateTest)
		return 0;
	/* "URL;FOLDER[;keep][;report=BASE]". keep: the files stay on the card, for the next run to
	 * replace. report=: the server to tell (default: the manifest's, when that is a test's own, plain
	 * http; never GitHub). */
	snprintf(url, sizeof(url), "%s", SlippWiiUpdateTest);
	for (part = strchr(url, ';'); part; part = next) {
		*part++ = 0;
		next = strchr(part, ';');
		if (!folder)
			folder = part;
		else if (strncmp(part, "keep", 4) == 0)
			keep = 1;
		else if (strncmp(part, "report=", 7) == 0)
			tell = part + 7;
	}
	if (!folder || folder[0] != '/' || strlen(folder) < 4 || strlen(folder) >= sizeof(root) || strstr(folder, "..") || strchr(folder + 1, '/'))
		return 0;	/* (one folder of the card's root, by name) */
	strcpy(root, folder);	/* (never the card's root: the installed SlippWii is not a test's to replace) */
	unattended = 1;
	run(url, test_key, UPD_TEST_URLS, 1, &o);

	/* the files off the card again, when they went on; and what is left of them */
	if (kept_zip && keep) {
		left = upd_verify(kept_zip, kept_len, &card);
		upd_plat_free(kept_zip);
		kept_zip = NULL;
	} else if (kept_zip) {
		upd_remove(kept_zip, kept_len, &card);
		left = upd_verify(kept_zip, kept_len, &card);
		upd_plat_free(kept_zip);
		kept_zip = NULL;
	} else if (o.files) {
		left = -1;	/* (files went on and the install did not end well: they stay, under the test's folder) */
	}
	if (!keep)
		test_cleanup();

	for (i = 0; o.err[i]; i++)
		if (!((o.err[i] >= '0' && o.err[i] <= '9') || (o.err[i] >= 'a' && o.err[i] <= 'z') || (o.err[i] >= 'A' && o.err[i] <= 'Z')))
			o.err[i] = '_';
	if (!tell && strncmp(url, "http://", 7) == 0) {
		part = strchr(url + 7, '/');
		if (part)
			*part = 0;
		tell = url;
	}
	snprintf(report, sizeof(report), "%s/report/result=%d;step=%s;have=%s;latest=%s;files=%d;verified=%d;left=%d;"
		"net_ms=%u;check_ms=%u;download_ms=%u;install_ms=%u;app=%s;data=%s;err=%s", tell ? tell : "", o.result, o.step,
		THIS_VERSION, o.latest, o.files, o.verified, left, (unsigned)o.net_ms, (unsigned)o.check_ms,
		(unsigned)o.download_ms, (unsigned)o.install_ms, app_dev, data_dev, o.err);
	if (net_up && tell && strncmp(tell, "http://", 7) == 0)
		upd_http_get(report, answer, sizeof(answer), &got, UPD_TEST_URLS, AGENT, NULL, NULL, text, sizeof(text));
	screen_clear();
	say(BLACK, "test: result %d %s, %d files, %d read back, %d left", o.result, o.step, o.files, o.verified, left);
	say(BLACK, "%u ms network, %u check, %u download, %u install", (unsigned)o.net_ms, (unsigned)o.check_ms,
		(unsigned)o.download_ms, (unsigned)o.install_ms);
	screen_ask(NULL, NULL);
	return 1;
}
#else
int SlippWii_UpdateTest(void)
{
	return 0;
}
#endif
