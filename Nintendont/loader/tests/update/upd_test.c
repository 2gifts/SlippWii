/* loader/tests/update/upd_test.c
 * The updater's core as a PC program, one step a command; tools/test_update.py drives it.
 *
 *	upd_test version A B			-1, 0 or 1
 *	upd_test allowed NAME			0 or 1
 *	upd_test manifest FILE KEYHEX FLAGS	the manifest's fields, or its error
 *	upd_test get URL OUT MAX FLAGS		the HTTP status and the bytes, the body into OUT
 *	upd_test install ZIP ROOT		the result, the files replaced
 *	upd_test verify ZIP ROOT		how many of the zip's files the card has, read back
 *	upd_test remove ZIP ROOT		takes them off again, then the same count
 *	upd_test update URL KEYHEX ROOT HAVE FLAGS	the whole of it, as update.c does on a Wii
 */
#include "upd_core.h"
#include "upd_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AGENT "SlippWii-test"

static int key_of(const char *hex, uint8_t key[65])
{
	unsigned v;
	int i;
	if (strlen(hex) != 130)
		return -1;
	for (i = 0; i < 65; i++) {
		if (sscanf(hex + 2 * i, "%2x", &v) != 1)
			return -1;
		key[i] = (uint8_t)v;
	}
	return 0;
}

static uint8_t *file(const char *path, uint32_t *len)
{
	FILE *f = fopen(path, "rb");
	uint8_t *buf;
	long size;
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = (uint8_t *)malloc(size > 0 ? (size_t)size : 1);
	if (fread(buf, 1, (size_t)size, f) != (size_t)size)
		size = 0;
	fclose(f);
	*len = (uint32_t)size;
	return buf;
}

static void show(const UpdManifest *m)
{
	int i;
	printf("version=%s zip=%s size=%lu sha256=", m->version, m->zip_url, (unsigned long)m->zip_size);
	for (i = 0; i < 32; i++)
		printf("%02x", m->zip_sha256[i]);
	for (i = 0; i < UPD_NOTES; i++)
		if (m->notes[i][0])
			printf(" note=[%s]", m->notes[i]);
	printf("\n");
}

int main(int argc, char **argv)
{
	char err[160] = "";
	uint8_t key[65], *data;
	uint32_t len = 0;
	UpdManifest m;
	UpdFs fs;
	int res, files = 0;

	if (argc == 4 && strcmp(argv[1], "version") == 0) {
		printf("%d\n", upd_version_cmp(argv[2], argv[3]));
	} else if (argc == 3 && strcmp(argv[1], "allowed") == 0) {
		printf("%d\n", upd_name_allowed(argv[2]));
	} else if (argc == 5 && strcmp(argv[1], "manifest") == 0) {
		data = file(argv[2], &len);
		if (!data || key_of(argv[3], key) < 0)
			return 2;
		res = upd_manifest_read(data, len, key, atoi(argv[4]), &m);
		if (res < 0)
			printf("error %d\n", res);
		else
			show(&m);
	} else if (argc == 6 && strcmp(argv[1], "get") == 0) {
		uint32_t max = (uint32_t)strtoul(argv[4], NULL, 10), got = 0;
		FILE *f;
		data = (uint8_t *)malloc(max ? max : 1);
		res = upd_http_get(argv[2], data, max, &got, atoi(argv[5]), AGENT, NULL, NULL, err, sizeof(err));
		printf("status %d bytes %lu [%s]\n", res, (unsigned long)got, err);
		f = fopen(argv[3], "wb");
		if (f) {
			fwrite(data, 1, got, f);
			fclose(f);
		}
	} else if (argc == 4 && strcmp(argv[1], "install") == 0) {
		data = file(argv[2], &len);
		if (!data)
			return 2;
		upd_host_fs(&fs, argv[3]);
		res = upd_install(data, len, &fs, NULL, NULL, &files, err, sizeof(err));
		printf("result %d files %d [%s]\n", res, files, err);
	} else if (argc == 4 && (strcmp(argv[1], "verify") == 0 || strcmp(argv[1], "remove") == 0)) {
		data = file(argv[2], &len);
		if (!data)
			return 2;
		upd_host_fs(&fs, argv[3]);
		if (argv[1][0] == 'r')
			upd_remove(data, len, &fs);
		printf("verified %d\n", upd_verify(data, len, &fs));
	} else if (argc == 7 && strcmp(argv[1], "update") == 0) {
		int flags = atoi(argv[6]);
		if (key_of(argv[3], key) < 0)
			return 2;
		res = upd_check(argv[2], key, flags, AGENT, &m, err, sizeof(err));
		if (res != UPD_OK) {
			printf("check %d [%s]\n", res, err);
			return 0;
		}
		show(&m);
		if (upd_version_cmp(m.version, argv[5]) <= 0) {
			printf("up to date (%s, the latest is %s)\n", argv[5], m.version);
			return 0;
		}
		res = upd_download(&m, flags, AGENT, &data, NULL, NULL, err, sizeof(err));
		if (res != UPD_OK) {
			printf("download %d [%s]\n", res, err);
			return 0;
		}
		upd_host_fs(&fs, argv[4]);
		res = upd_install(data, m.zip_size, &fs, NULL, NULL, &files, err, sizeof(err));
		printf("installed %d files %d [%s]\n", res, files, err);
		upd_plat_free(data);
	} else {
		fprintf(stderr, "see upd_test.c for the commands\n");
		return 2;
	}
	return 0;
}
