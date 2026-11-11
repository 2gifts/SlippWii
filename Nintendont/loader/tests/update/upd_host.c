/* loader/tests/update/upd_host.c
 * The updater's platform on a PC, for its tests (tools/test_update.py): sockets, and the card as a
 * folder. UPD_FAIL_WRITE=N and UPD_FAIL_RENAME=N in the environment make the Nth write or the Nth
 * rename of a ".new" file fail, as a card might.
 */
#include "upd_core.h"
#include "upd_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <netdb.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0777)
#define closesocket close
#endif

int upd_plat_connect(const char *host, uint16_t port)
{
	struct addrinfo hints, *list = NULL, *a;
	char service[8];
	int s = -1;
#ifdef _WIN32
	static int started;
	WSADATA wsa;
	DWORD timeout = 20000;
	if (!started++)
		WSAStartup(MAKEWORD(2, 2), &wsa);
#else
	struct timeval timeout = { 20, 0 };
#endif
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	snprintf(service, sizeof(service), "%u", (unsigned)port);
	if (getaddrinfo(host, service, &hints, &list) != 0)
		return -2;
	for (a = list; a && s < 0; a = a->ai_next) {
		s = (int)socket(a->ai_family, a->ai_socktype, a->ai_protocol);
		if (s >= 0 && connect(s, a->ai_addr, (int)a->ai_addrlen) != 0) {
			closesocket(s);
			s = -1;
		}
	}
	freeaddrinfo(list);
	if (s < 0)
		return -3;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
	return s;
}

int upd_plat_recv(int s, uint8_t *buf, int len) { return (int)recv(s, (char *)buf, len, 0); }
int upd_plat_send(int s, const uint8_t *buf, int len) { return (int)send(s, (const char *)buf, len, 0); }
void upd_plat_close(int s) { closesocket(s); }

void *upd_plat_alloc(uint32_t size) { return malloc(size ? size : 1); }
void upd_plat_free(void *p) { free(p); }

void upd_plat_entropy(uint8_t out[32])
{
	int i;
	srand((unsigned)time(NULL) ^ (unsigned)clock());
	for (i = 0; i < 32; i++)
		out[i] = (uint8_t)rand();
}

/* ---- the card: a folder ---- */

static int fail_at(const char *name, int *count)
{
	const char *v = getenv(name);
	return v && ++*count == atoi(v);
}

static void full(const char *root, const char *path, char *out, size_t size)
{
	snprintf(out, size, "%s/%s", root, path);
}

static int h_write(void *ctx, const char *path, const uint8_t *data, uint32_t len)
{
	static int writes;
	char p[512], *slash;
	FILE *f;
	size_t n;
	if (fail_at("UPD_FAIL_WRITE", &writes))
		return -1;
	full((const char *)ctx, path, p, sizeof(p));
	for (slash = strchr(p + strlen((const char *)ctx) + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
		*slash = 0;
		MKDIR(p);
		*slash = '/';
	}
	f = fopen(p, "wb");
	if (!f)
		return -1;
	n = fwrite(data, 1, len, f);
	return fclose(f) == 0 && n == len ? 0 : -1;
}

static int h_crc(void *ctx, const char *path, uint32_t *crc, uint32_t *len)
{
	char p[512];
	uint8_t *buf;
	long size;
	FILE *f;
	full((const char *)ctx, path, p, sizeof(p));
	f = fopen(p, "rb");
	if (!f)
		return -1;
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = (uint8_t *)malloc(size > 0 ? (size_t)size : 1);
	if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) {
		fclose(f);
		free(buf);
		return -1;
	}
	fclose(f);
	*crc = upd_crc32(buf, (uint32_t)size);
	*len = (uint32_t)size;
	free(buf);
	return 0;
}

static int h_rename(void *ctx, const char *from, const char *to)
{
	static int renames;
	char a[512], b[512];
	size_t n = strlen(from);
	if (n > 4 && strcmp(from + n - 4, ".new") == 0 && fail_at("UPD_FAIL_RENAME", &renames))
		return -1;
	full((const char *)ctx, from, a, sizeof(a));
	full((const char *)ctx, to, b, sizeof(b));
	return rename(a, b) == 0 ? 0 : -1;
}

static int h_remove(void *ctx, const char *path)
{
	char p[512];
	full((const char *)ctx, path, p, sizeof(p));
	remove(p);
	return 0;
}

void upd_host_fs(UpdFs *fs, const char *root)
{
	fs->write = h_write;
	fs->crc = h_crc;
	fs->rename = h_rename;
	fs->remove = h_remove;
	fs->ctx = (void *)root;
}
