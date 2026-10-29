/* See loader_stubs.h. main() runs SlippWii_Prepare once and prints what
 * it left: the config's networking bit and the account line. */
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

#include "loader_stubs.h"
#include "slippwii.h"

static NIN_CFG cfg;
NIN_CFG *ncfg = &cfg;
FATFS *devices[2];
static FATFS present;
u32 Shutdown;
static const char *answers;
static int last_y = -1;

static void real_path(char *out, size_t size, const char *path)
{
	const char *root = NULL;
	if (strncmp(path, "sd:", 3) == 0) {
		root = getenv("SW_SD");
		path += 3;
	} else if (strncmp(path, "usb:", 4) == 0) {
		root = getenv("SW_USB");
		path += 4;
	}
	snprintf(out, size, "%s%s", root ? root : "", path);
}

FRESULT f_open_char(FIL *fp, const char *path, BYTE mode)
{
	char p[1024];
	real_path(p, sizeof(p), path);
	fp->fp = fopen(p, (mode & FA_WRITE) ? "wb" : "rb");
	if (!fp->fp)
		return FR_NO_FILE;
	fseek(fp->fp, 0, SEEK_END);
	fp->size = (u64)ftell(fp->fp);
	fseek(fp->fp, 0, SEEK_SET);
	return FR_OK;
}

FRESULT f_read(FIL *fp, void *buf, UINT n, UINT *got)
{
	*got = (UINT)fread(buf, 1, n, fp->fp);
	return FR_OK;
}

FRESULT f_write(FIL *fp, const void *buf, UINT n, UINT *wrote)
{
	*wrote = (UINT)fwrite(buf, 1, n, fp->fp);
	return *wrote == n ? FR_OK : FR_DISK_ERR;
}

FRESULT f_lseek(FIL *fp, u64 offset)
{
	return fseek(fp->fp, (long)offset, SEEK_SET) == 0 ? FR_OK : FR_DISK_ERR;
}

FRESULT f_close(FIL *fp)
{
	fclose(fp->fp);
	return FR_OK;
}

FRESULT f_mkdir_char(const char *path)
{
	char p[1024];
	real_path(p, sizeof(p), path);
#ifdef _WIN32
	_mkdir(p);
#else
	mkdir(p, 0777);
#endif
	return FR_OK;
}

const char *GetRootDevice(void)
{
	return getenv("SW_GAME_DEV");
}

int IsSupportedFileExt(const char *path)
{
	size_t n = strlen(path);
	return (n > 4 && (!strcmp(path + n - 4, ".iso") || !strcmp(path + n - 4, ".gcm"))) ||
	       (n > 5 && !strcmp(path + n - 5, ".ciso"));
}

void ReadRealDisc(u8 *buf, u64 offset, u32 len, u32 cmd)
{
	FILE *f = fopen(getenv("SW_DRIVE"), "rb");
	(void)cmd;
	memset(buf, 0, len);
	if (f) {
		fseek(f, (long)offset, SEEK_SET);
		if (fread(buf, 1, len, f) != len)
			;
		fclose(f);
	}
}

void PrintFormat(u8 size, u32 color, int x, int y, const char *fmt, ...)
{
	va_list ap;
	(void)size;
	(void)x;
	if (y <= last_y)
		printf("--\n");
	last_y = y;
	printf("SCREEN%s ", color == MAROON ? "!" : color == GREEN ? "+" : " ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
}

void PrintBuildInfo(void) {}
void PrintButtonActions(const char *home, const char *a, const char *b, const char *x)
{
	(void)home;
	(void)x;
	if (a || b)
		printf("BUTTONS A=%s B=%s\n", a ? a : "-", b ? b : "-");
}
void GRRLIB_Render(void) { last_y = -1; }
void ClearScreen(void) {}
void VIDEO_WaitVSync(void) {}
void FPAD_Update(void) {}

/* The next scripted answer: 'A' or 'B'; none left is a test error. */
static int frames;
static int pressed(char want)
{
	if (++frames < 40)
		return 0;
	if (!answers || !*answers) {
		printf("ERROR: a question with no answer left\n");
		exit(4);
	}
	if (*answers != want)
		return 0;
	printf("PRESS %c\n", want);
	answers++;
	frames = 0;
	return 1;
}
int FPAD_OK(int lock) { (void)lock; return pressed('A'); }
int FPAD_Cancel(int lock) { (void)lock; return pressed('B'); }
void LoaderShutdown(void) { exit(5); }
void ExitToLoader(int ret)
{
	printf("EXIT %d\n", ret);
	exit(3);
}
void FlushDevices(void) {}

int main(int argc, char **argv)
{
	unsigned cmd = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 0) : 0;
	strncpy(cfg.GamePath, argc > 1 ? argv[1] : "", sizeof(cfg.GamePath) - 1);
	memcpy(&cfg.GameID, "GALE", 4);
	cfg.Config = NIN_CFG_NETWORK;
	answers = getenv("SW_ANSWERS");
	if (getenv("SW_SD"))
		devices[DEV_SD] = &present;
	if (getenv("SW_USB"))
		devices[DEV_USB] = &present;
	SlippWii_Prepare(cmd, 0);
	printf("RESULT network=%d account=%s\n", (cfg.Config & NIN_CFG_NETWORK) ? 1 : 0, SlippWii_AccountLine);
	return 0;
}
