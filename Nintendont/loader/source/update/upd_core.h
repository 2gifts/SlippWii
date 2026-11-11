/* loader/source/update/upd_core.h
 * SlippWii's updater: the work that is the same on a Wii and on a PC (where its tests run).
 *
 *   - the manifest: a small signed text that says what the latest version is (upd_manifest_read);
 *   - an HTTPS download, redirects followed (upd_http_get; TLS by BearSSL, the Wii's own is too old
 *     for GitHub);
 *   - the release's zip, checked against the manifest's SHA-256 and put on the card (upd_install).
 *
 * What is installed is trusted because of the signature and nothing else: the manifest is signed
 * with SlippWii's release key (ECDSA P-256; the public half is in upd_key.h), and it names the
 * zip's SHA-256. The connection's certificates are not checked: a Wii's clock is often wrong, and
 * no certificate could make a file ours anyway.
 *
 * The platform gives sockets, random bytes and files (upd_plat_*, UpdFs): update.c on a Wii,
 * tests/update/upd_host.c on a PC.
 */
#ifndef UPD_CORE_H
#define UPD_CORE_H

#include <stddef.h>
#include <stdint.h>

#define UPD_MANIFEST_MAX 4096		/* bytes of manifest text */
#define UPD_ZIP_MAX (12u << 20)		/* a release's zip is 2 MB */
#define UPD_URL_MAX 2048		/* GitHub's redirects to its file store are long */
#define UPD_NOTES 3

/* Where a release's zip may come from (a signed manifest naming another place is refused). */
#define UPD_ZIP_PREFIX "https://github.com/2gifts/SlippWii/releases/download/"

enum {
	UPD_OK = 0,
	UPD_E_FORMAT = -1,	/* not a manifest, or a line of it is not what it should be */
	UPD_E_SIGNATURE = -2,	/* not signed by SlippWii's key */
	UPD_E_URL = -3,		/* the zip is not on SlippWii's releases page */
	UPD_E_NET = -10,	/* could not connect, or the connection broke */
	UPD_E_TLS = -11,
	UPD_E_HTTP = -12,	/* an answer that is not HTTP, or too many redirects */
	UPD_E_SIZE = -13,	/* more than the caller has room for */
	UPD_E_ZIP = -20,	/* not a zip, or a file of it is damaged */
	UPD_E_NAME = -21,	/* a file that does not belong in a SlippWii release */
	UPD_E_WRITE = -22,	/* the card did not take a file (nothing was changed) */
	UPD_E_SWAP = -23,	/* the card failed while the new files replaced the old ones */
	UPD_E_MEMORY = -24
};

typedef struct {
	char version[24];
	char zip_url[256];
	uint32_t zip_size;
	uint8_t zip_sha256[32];
	char notes[UPD_NOTES][64];	/* what is new, for the screen; unused ones are "" */
} UpdManifest;

/* flags */
#define UPD_TEST_URLS 1		/* tests: the zip may be anywhere, over http too */

/* Reads a manifest. Only a signed one is looked at: the signature is checked first.
 * `pubkey` is the release key's public point, 65 bytes (04, X, Y). */
int upd_manifest_read(const uint8_t *text, uint32_t len, const uint8_t *pubkey, int flags, UpdManifest *m);

/* < 0, 0, > 0: version a is older than, the same as, newer than b ("1.4.0", "1.5.0-beta.2";
 * a beta is older than its release). */
int upd_version_cmp(const char *a, const char *b);

void upd_sha256(const uint8_t *data, uint32_t len, uint8_t out[32]);
uint32_t upd_crc32(const uint8_t *data, uint32_t len);
uint32_t upd_crc32_more(uint32_t crc, const uint8_t *data, uint32_t len);	/* (a file in pieces: start from 0) */

/* ---- the platform's part ---- */

int upd_plat_connect(const char *host, uint16_t port);	/* a handle >= 0, or < 0 */
int upd_plat_recv(int s, uint8_t *buf, int len);	/* > 0: bytes; 0: closed; < 0: failed or timed out */
int upd_plat_send(int s, const uint8_t *buf, int len);	/* > 0: bytes sent; < 0: failed */
void upd_plat_close(int s);
void upd_plat_entropy(uint8_t out[32]);			/* what randomness the machine has */
void *upd_plat_alloc(uint32_t size);			/* memory for a file (a Wii's drivers want it aligned) */
void upd_plat_free(void *p);

typedef void (*UpdProgress)(void *arg, uint32_t done, uint32_t total);	/* total 0: not known */

/* GET `url` into `out`. Follows redirects; https only unless `flags` has UPD_TEST_URLS.
 * Returns the HTTP status (200 for a file that is in `out`, `*got` bytes of it), or UPD_E_*; `err`
 * gets a line a person can read. */
int upd_http_get(const char *url, uint8_t *out, uint32_t max, uint32_t *got, int flags,
	const char *agent, UpdProgress progress, void *arg, char *err, int err_size);

/* The latest release's manifest, fetched from `url` and read (upd_manifest_read). Returns UPD_OK,
 * UPD_E_*, or the HTTP status when the server had no such file (404: a release without one). */
int upd_check(const char *url, const uint8_t *pubkey, int flags, const char *agent, UpdManifest *m,
	char *err, int err_size);

/* The zip a manifest names, whole and with the SHA-256 the manifest says: `*zip`, which the caller
 * frees, m->zip_size bytes. UPD_OK or UPD_E_*. */
int upd_download(const UpdManifest *m, int flags, const char *agent, uint8_t **zip, UpdProgress progress,
	void *arg, char *err, int err_size);

/* The card's files, by the path a release's zip has them under ("apps/SlippWii/boot.dol"). */
typedef struct {
	/* (what these return below 0 is shown with the file's name when one fails: the platform's own code) */
	int (*write)(void *ctx, const char *path, const uint8_t *data, uint32_t len);	/* makes its folders; 0 or < 0 */
	int (*crc)(void *ctx, const char *path, uint32_t *crc, uint32_t *len);		/* as the card now has it; 0 or < 0 */
	int (*rename)(void *ctx, const char *from, const char *to);			/* (`to` does not exist) 0 or < 0 */
	int (*remove)(void *ctx, const char *path);					/* 0 also if there was none */
	void *ctx;
} UpdFs;

/* 1 if a release's zip may hold a file of this name. */
int upd_name_allowed(const char *name);

/* Puts a release's zip on the card: every file is unpacked and checked first, then written under
 * a temporary name and read back, and only then do the new files replace the old ones (boot.dol
 * last). UPD_E_WRITE and everything before it leave the card as it was. `*files`: how many. */
int upd_install(const uint8_t *zip, uint32_t len, const UpdFs *fs, UpdProgress progress, void *arg,
	int *files, char *err, int err_size);

/* How many of the zip's files the card now has, read back whole with the zip's CRC-32s: all of
 * them after an install that worked. */
int upd_verify(const uint8_t *zip, uint32_t len, const UpdFs *fs);

/* tests: takes the zip's files off the card again. */
void upd_remove(const uint8_t *zip, uint32_t len, const UpdFs *fs);

#endif
