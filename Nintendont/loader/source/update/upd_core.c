/* loader/source/update/upd_core.c
 * The updater's manifest, and a release's zip on its way to the card (see upd_core.h).
 */
#include "upd_core.h"
#include "puff.h"
#include "bearssl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void upd_sha256(const uint8_t *data, uint32_t len, uint8_t out[32])
{
	br_sha256_context c;
	br_sha256_init(&c);
	br_sha256_update(&c, data, len);
	br_sha256_out(&c, out);
}

uint32_t upd_crc32_more(uint32_t crc, const uint8_t *data, uint32_t len)
{
	uint32_t i;
	int k;
	crc = ~crc;
	for (i = 0; i < len; i++) {
		crc ^= data[i];
		for (k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
	}
	return ~crc;
}

uint32_t upd_crc32(const uint8_t *data, uint32_t len)
{
	return upd_crc32_more(0, data, len);
}

/* ---- the manifest ----
 *
 *	SlippWii update 1
 *	version=1.5.0
 *	zip=https://github.com/2gifts/SlippWii/releases/download/v1.5.0/SlippWii-v1.5.0.zip
 *	size=1947989
 *	sha256=<64 hex digits>
 *	note=<a line for the screen>		(up to UPD_NOTES of them)
 *	sig=<hex of an ECDSA P-256 signature, DER, over every byte before this line>
 *
 * Lines end with "\n" alone. A key this version does not know is skipped, so a later release can
 * say more. tools/sign_update.py writes it.
 */

static const char HEAD[] = "SlippWii update 1\n";

static int hex_digit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* `chars` hex digits into bytes; how many, or -1. */
static int hex_bytes(const char *s, uint32_t chars, uint8_t *out, uint32_t room)
{
	uint32_t i;
	if (chars % 2 || chars / 2 > room)
		return -1;
	for (i = 0; i < chars; i += 2) {
		int hi = hex_digit(s[i]), lo = hex_digit(s[i + 1]);
		if (hi < 0 || lo < 0)
			return -1;
		out[i / 2] = (uint8_t)(hi << 4 | lo);
	}
	return (int)(chars / 2);
}

/* A value into `out` if it fits and is plain text. */
static int text_value(const char *v, uint32_t n, char *out, uint32_t size)
{
	uint32_t i;
	if (n >= size)
		return -1;
	for (i = 0; i < n; i++)
		if (v[i] < 0x20 || v[i] > 0x7E)
			return -1;
	memcpy(out, v, n);
	out[n] = 0;
	return 0;
}

int upd_manifest_read(const uint8_t *text, uint32_t len, const uint8_t *pubkey, int flags, UpdManifest *m)
{
	const char *t = (const char *)text;
	uint8_t sig[80], hash[32];
	br_ec_public_key pk;
	uint32_t end = len, signed_len, line;
	int sig_len, notes = 0, have_size = 0, have_sha = 0;

	memset(m, 0, sizeof(*m));
	if (len < sizeof(HEAD) - 1 || len > UPD_MANIFEST_MAX || memcmp(t, HEAD, sizeof(HEAD) - 1) != 0)
		return UPD_E_FORMAT;
	if (t[end - 1] == '\n')
		end--;
	for (signed_len = end; signed_len > 0 && t[signed_len - 1] != '\n'; signed_len--)
		;
	if (end - signed_len < 4 || memcmp(t + signed_len, "sig=", 4) != 0)
		return UPD_E_FORMAT;
	sig_len = hex_bytes(t + signed_len + 4, end - signed_len - 4, sig, sizeof(sig));
	if (sig_len < 8)
		return UPD_E_FORMAT;

	/* Signed by SlippWii's release key, or nothing of it is read. */
	upd_sha256(text, signed_len, hash);
	pk.curve = BR_EC_secp256r1;
	pk.q = (unsigned char *)pubkey;
	pk.qlen = 65;
	if (br_ecdsa_i31_vrfy_asn1(&br_ec_p256_m31, hash, sizeof(hash), &pk, sig, (size_t)sig_len) != 1)
		return UPD_E_SIGNATURE;

	for (line = sizeof(HEAD) - 1; line < signed_len; ) {
		uint32_t eol = line, eq = line, n;
		const char *v;
		while (eol < signed_len && t[eol] != '\n')
			eol++;
		while (eq < eol && t[eq] != '=')
			eq++;
		if (eq == eol)
			return UPD_E_FORMAT;
		v = t + eq + 1;
		n = eol - eq - 1;
#define KEY(name) (eq - line == sizeof(name) - 1 && memcmp(t + line, name, sizeof(name) - 1) == 0)
		if (KEY("version")) {
			if (text_value(v, n, m->version, sizeof(m->version)) < 0)
				return UPD_E_FORMAT;
		} else if (KEY("zip")) {
			if (text_value(v, n, m->zip_url, sizeof(m->zip_url)) < 0)
				return UPD_E_FORMAT;
		} else if (KEY("size")) {
			uint32_t i, size = 0;
			if (n < 1 || n > 9)
				return UPD_E_FORMAT;
			for (i = 0; i < n; i++) {
				if (v[i] < '0' || v[i] > '9')
					return UPD_E_FORMAT;
				size = size * 10 + (uint32_t)(v[i] - '0');
			}
			m->zip_size = size;
			have_size = 1;
		} else if (KEY("sha256")) {
			if (n != 64 || hex_bytes(v, n, m->zip_sha256, 32) != 32)
				return UPD_E_FORMAT;
			have_sha = 1;
		} else if (KEY("note")) {
			if (notes < UPD_NOTES && text_value(v, n, m->notes[notes], sizeof(m->notes[0])) == 0)
				notes++;
		}
#undef KEY
		line = eol + 1;
	}
	if (!m->version[0] || !m->zip_url[0] || !have_size || !have_sha)
		return UPD_E_FORMAT;
	if (m->version[0] < '0' || m->version[0] > '9' || m->zip_size == 0 || m->zip_size > UPD_ZIP_MAX)
		return UPD_E_FORMAT;
	if (flags & UPD_TEST_URLS) {
		if (strncmp(m->zip_url, "https://", 8) != 0 && strncmp(m->zip_url, "http://", 7) != 0)
			return UPD_E_URL;
	} else if (strncmp(m->zip_url, UPD_ZIP_PREFIX, sizeof(UPD_ZIP_PREFIX) - 1) != 0) {
		return UPD_E_URL;
	}
	return UPD_OK;
}

/* "1.5.0" or "1.5.0-beta.2": the three numbers, 1 for a release and 0 for a beta, the beta's number. */
static void version_parts(const char *s, long v[5])
{
	char *e;
	int i;
	v[0] = v[1] = v[2] = v[4] = 0;
	v[3] = 1;
	for (i = 0; i < 3; i++) {
		v[i] = strtol(s, &e, 10);
		s = e;
		if (*s != '.')
			break;
		s++;
	}
	if (*s == '-') {
		v[3] = 0;
		while (*s && (*s < '0' || *s > '9'))
			s++;
		v[4] = strtol(s, NULL, 10);
	}
}

int upd_version_cmp(const char *a, const char *b)
{
	long x[5], y[5];
	int i;
	version_parts(a, x);
	version_parts(b, y);
	for (i = 0; i < 5; i++)
		if (x[i] != y[i])
			return x[i] < y[i] ? -1 : 1;
	return 0;
}

int upd_check(const char *url, const uint8_t *pubkey, int flags, const char *agent, UpdManifest *m,
	char *err, int err_size)
{
	static uint8_t text[UPD_MANIFEST_MAX];
	uint32_t got = 0;
	int res = upd_http_get(url, text, sizeof(text), &got, flags, agent, NULL, NULL, err, err_size);
	if (res != 200)
		return res;
	res = upd_manifest_read(text, got, pubkey, flags, m);
	if (res == UPD_E_SIGNATURE)
		snprintf(err, (size_t)err_size, "the update information is not signed by SlippWii");
	else if (res < 0)
		snprintf(err, (size_t)err_size, "the update information cannot be read (%d)", res);
	return res;
}

int upd_download(const UpdManifest *m, int flags, const char *agent, uint8_t **zip, UpdProgress progress,
	void *arg, char *err, int err_size)
{
	uint8_t hash[32], *buf;
	uint32_t got = 0;
	int res;
	*zip = NULL;
	buf = (uint8_t *)upd_plat_alloc(m->zip_size);
	if (!buf) {
		snprintf(err, (size_t)err_size, "out of memory");
		return UPD_E_MEMORY;
	}
	res = upd_http_get(m->zip_url, buf, m->zip_size, &got, flags, agent, progress, arg, err, err_size);
	if (res != 200) {
		upd_plat_free(buf);
		return res < 0 ? res : UPD_E_HTTP;
	}
	upd_sha256(buf, got, hash);
	if (got != m->zip_size || memcmp(hash, m->zip_sha256, 32) != 0) {
		upd_plat_free(buf);
		snprintf(err, (size_t)err_size, "the download is not the file this release names");
		return UPD_E_ZIP;
	}
	*zip = buf;
	return UPD_OK;
}

/* ---- a release's zip ----
 *
 * As Python's zipfile writes it (tools/make_release.py): files stored or deflated, their sizes and
 * CRC-32s in the central directory. No folders' own entries are needed, no encryption, no zip64.
 */

typedef struct {
	char name[128];
	const uint8_t *data;
	uint32_t packed, size, crc;
	int method;
} Entry;

static uint32_t le16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | le16(p + 2) << 16; }

/* The central directory: where it starts and how many entries; 0 or < 0. */
static int zip_open(const uint8_t *z, uint32_t len, uint32_t *at, uint32_t *count)
{
	uint32_t i, size;
	if (len < 22)
		return -1;
	for (i = len - 22; le32(z + i) != 0x06054B50; i--)
		if (i == 0 || len - i >= 22 + 65535)
			return -1;
	*count = le16(z + i + 10);
	size = le32(z + i + 12);
	*at = le32(z + i + 16);
	if (*at > len || size > len - *at)
		return -1;
	return 0;
}

/* The entry at `*at`, which moves on to the next; 0 or < 0. */
static int zip_entry(const uint8_t *z, uint32_t len, uint32_t *at, Entry *e)
{
	const uint8_t *c = z + *at;
	uint32_t n, extra, comment, local, data;
	if (*at > len || len - *at < 46 || le32(c) != 0x02014B50)
		return -1;
	n = le16(c + 28);
	extra = le16(c + 30);
	comment = le16(c + 32);
	if (len - *at - 46 < n + extra + comment || n == 0 || n >= sizeof(e->name))
		return -1;
	memcpy(e->name, c + 46, n);
	e->name[n] = 0;
	e->method = (int)le16(c + 10);
	e->crc = le32(c + 16);
	e->packed = le32(c + 20);
	e->size = le32(c + 24);
	local = le32(c + 42);
	*at += 46 + n + extra + comment;
	if (le16(c + 8) & 1)	/* encrypted */
		return -1;
	if (local > len || len - local < 30 || le32(z + local) != 0x04034B50)
		return -1;
	data = local + 30 + le16(z + local + 26) + le16(z + local + 28);
	if (data > len || e->packed > len - data || e->size > UPD_ZIP_MAX)
		return -1;
	if (e->method != 0 && e->method != 8)
		return -1;
	if (e->method == 0 && e->packed != e->size)
		return -1;
	e->data = z + data;
	return 0;
}

/* The file itself, checked against its CRC-32; the caller frees it (upd_plat_free). */
static int unpack(const Entry *e, uint8_t **out)
{
	uint8_t *buf = (uint8_t *)upd_plat_alloc(e->size ? e->size : 1);
	if (!buf)
		return UPD_E_MEMORY;
	if (e->method == 0) {
		memcpy(buf, e->data, e->size);
	} else {
		unsigned long got = e->size, used = e->packed;
		if (puff(buf, &got, e->data, &used) != 0 || got != e->size) {
			upd_plat_free(buf);
			return UPD_E_ZIP;
		}
	}
	if (upd_crc32(buf, e->size) != e->crc) {
		upd_plat_free(buf);
		return UPD_E_ZIP;
	}
	*out = buf;
	return 0;
}

static int ends_with(const char *s, const char *tail)
{
	size_t n = strlen(s), k = strlen(tail);
	return n > k && strcmp(s + n - k, tail) == 0;
}

/* What a release puts on the card, and nothing else: the app, Slippi's files, the notes beside
 * them. Never the account, the settings or anything the console writes itself. A release with a
 * file outside this is refused whole by this version (tools/make_release.py checks the same). */
int upd_name_allowed(const char *name)
{
	static const char *const slippwii_files[] = {
		"netplay.gct", "NOTICE.txt", "LICENSE.txt", "PUT user.json HERE.txt", "copy-account.ps1",
	};
	size_t n = strlen(name), i;
	if (n == 0 || n > 100 || name[0] == '/' || name[n - 1] == '/' || strstr(name, "..") || strstr(name, "//"))
		return 0;
	for (i = 0; i < n; i++)
		if (name[i] < 0x20 || name[i] > 0x7E || strchr("\\:*?\"<>|", name[i]))
			return 0;
	if (strncmp(name, "apps/SlippWii/", 14) == 0)
		return !strchr(name + 14, '/');
	if (strncmp(name, "slippwii/files/", 15) == 0)
		return !strchr(name + 15, '/');
	if (strncmp(name, "slippwii/patches/", 17) == 0)
		return !strchr(name + 17, '/');
	if (strncmp(name, "slippwii/", 9) == 0) {
		for (i = 0; i < sizeof(slippwii_files) / sizeof(slippwii_files[0]); i++)
			if (strcmp(name + 9, slippwii_files[i]) == 0)
				return 1;
		return 0;
	}
	if (strchr(name, '/'))
		return 0;
	return ends_with(name, ".txt") || ends_with(name, ".bat");
}

#define BOOT_DOL "apps/SlippWii/boot.dol"

static void say(char *err, int size, const char *what, const char *name)
{
	if (err && size > 0)
		snprintf(err, (size_t)size, "%s%s%s", what, name ? ": " : "", name ? name : "");
}

int upd_install(const uint8_t *zip, uint32_t len, const UpdFs *fs, UpdProgress progress, void *arg,
	int *files, char *err, int err_size)
{
	char tmp[160], old[160];
	uint32_t first, count, at, i, step = 0;
	Entry e;
	uint8_t *data;
	int res, pass;

	if (files)
		*files = 0;
	if (zip_open(zip, len, &first, &count) < 0 || count == 0) {
		say(err, err_size, "the download is not a zip file", NULL);
		return UPD_E_ZIP;
	}

	/* 1. Every file unpacks, has its CRC-32 and belongs on the card: before anything is written. */
	for (at = first, i = 0; i < count; i++) {
		if (zip_entry(zip, len, &at, &e) < 0) {
			say(err, err_size, "the zip file is damaged", NULL);
			return UPD_E_ZIP;
		}
		if (!upd_name_allowed(e.name)) {
			say(err, err_size, "this release has a file this version cannot install", e.name);
			return UPD_E_NAME;
		}
		res = unpack(&e, &data);
		if (res < 0) {
			say(err, err_size, res == UPD_E_MEMORY ? "out of memory" : "a file in the zip is damaged", e.name);
			return res;
		}
		upd_plat_free(data);
		if (progress)
			progress(arg, ++step, count * 3);
	}

	/* 2. Each under a temporary name, and read back from the card. */
	res = 0;
	for (at = first, i = 0; i < count && res == 0; i++) {
		uint32_t crc = 0, size = 0;
		const char *what = NULL;
		int code;
		zip_entry(zip, len, &at, &e);
		snprintf(tmp, sizeof(tmp), "%s.new", e.name);
		res = unpack(&e, &data);
		if (res < 0)
			break;
		code = fs->write(fs->ctx, tmp, data, e.size);
		upd_plat_free(data);
		if (code < 0) {
			what = "could not be written";
		} else if ((code = fs->crc(fs->ctx, tmp, &crc, &size)) < 0) {
			what = "could not be read back";
		} else if (size != e.size || crc != e.crc) {
			what = "came back changed";
			code = 0;
		}
		if (what) {
			if (err && err_size > 0)
				snprintf(err, (size_t)err_size, "%.40s %s (%d)", e.name, what, code);
			res = UPD_E_WRITE;
		}
		if (progress && res == 0)
			progress(arg, ++step, count * 3);
	}
	if (res < 0) {
		if (res != UPD_E_WRITE)
			say(err, err_size, "out of memory", NULL);
		for (at = first, i = 0; i < count; i++) {
			zip_entry(zip, len, &at, &e);
			snprintf(tmp, sizeof(tmp), "%s.new", e.name);
			fs->remove(fs->ctx, tmp);
		}
		return res < 0 && res != UPD_E_MEMORY ? UPD_E_WRITE : res;
	}

	/* 3. The new files in the old ones' places, the app itself last: if the card fails on the way,
	 * the app that starts is still the old one, which can do this again. */
	for (pass = 0; pass < 2; pass++) {
		for (at = first, i = 0; i < count; i++) {
			zip_entry(zip, len, &at, &e);
			if ((strcmp(e.name, BOOT_DOL) == 0) != pass)
				continue;
			snprintf(tmp, sizeof(tmp), "%s.new", e.name);
			snprintf(old, sizeof(old), "%s.old", e.name);
			fs->remove(fs->ctx, old);
			fs->rename(fs->ctx, e.name, old);	/* (fails when there was none: a new file) */
			if (fs->rename(fs->ctx, tmp, e.name) < 0) {
				fs->rename(fs->ctx, old, e.name);
				say(err, err_size, "the card failed while the files were replaced", e.name);
				return UPD_E_SWAP;
			}
			fs->remove(fs->ctx, old);
			if (files)
				(*files)++;
			if (progress)
				progress(arg, ++step, count * 3);
		}
	}
	return UPD_OK;
}

int upd_verify(const uint8_t *zip, uint32_t len, const UpdFs *fs)
{
	uint32_t first, count, at, i, crc, size;
	Entry e;
	int good = 0;
	if (zip_open(zip, len, &first, &count) < 0)
		return 0;
	for (at = first, i = 0; i < count; i++) {
		if (zip_entry(zip, len, &at, &e) < 0)
			break;
		if (fs->crc(fs->ctx, e.name, &crc, &size) == 0 && size == e.size && crc == e.crc)
			good++;
	}
	return good;
}

void upd_remove(const uint8_t *zip, uint32_t len, const UpdFs *fs)
{
	uint32_t first, count, at, i;
	Entry e;
	if (zip_open(zip, len, &first, &count) < 0)
		return;
	for (at = first, i = 0; i < count; i++) {
		if (zip_entry(zip, len, &at, &e) < 0 || !upd_name_allowed(e.name))
			break;
		fs->remove(fs->ctx, e.name);
	}
}
