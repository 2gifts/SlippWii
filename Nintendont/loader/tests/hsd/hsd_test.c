/* PC test of the loader's link resolver: hsd_test ARCHIVE DISC_FOLDER OUT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hsd_links.h"

static u8 *slurp(const char *path, u32 *size)
{
	FILE *f = fopen(path, "rb");
	u8 *buf;
	long n;
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = (u8 *)malloc((size_t)n + 1);
	if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
		free(buf);
		buf = NULL;
	}
	fclose(f);
	*size = (u32)n;
	return buf;
}

static u8 *disc_load(void *ctx, const char *name, u32 *size)
{
	char path[600];
	snprintf(path, sizeof(path), "%s/%s", (const char *)ctx, name);
	return slurp(path, size);
}

static void disc_unload(void *ctx, u8 *data)
{
	(void)ctx;
	free(data);
}

int main(int argc, char **argv)
{
	HsdDisc disc = { NULL, disc_load, disc_unload };
	u32 size, out_size;
	const char *error;
	u8 *raw, *out;
	FILE *f;
	if (argc != 4)
		return 2;
	disc.ctx = argv[2];
	raw = slurp(argv[1], &size);
	if (!raw)
		return 2;
	out = hsd_resolve(raw, size, &disc, &out_size, &error);
	if (!out) {
		printf("error: %s\n", error);
		return 1;
	}
	f = fopen(argv[3], "wb");
	fwrite(out, 1, out_size, f);
	fclose(f);
	printf("%u bytes\n", (unsigned)out_size);
	free(out);
	free(raw);
	return 0;
}
