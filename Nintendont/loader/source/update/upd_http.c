/* loader/source/update/upd_http.c
 * The updater's downloads: an HTTP GET over TLS 1.2 (BearSSL), redirects followed (see upd_core.h).
 *
 * GitHub answers a release's file with two redirects, the second to its file store, whose URL is
 * long and signed; each hop is a new connection ("Connection: close" keeps this simple, and three
 * handshakes are a few seconds on a Wii).
 */
#include "upd_core.h"
#include "bearssl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Certificates are read for the server's key and not judged (upd_core.h says why): BearSSL's
 * X.509 engine replaced by one that takes the first certificate's public key as it comes. */
typedef struct {
	const br_x509_class *vtable;
	br_x509_decoder_context first;
	int index;
} AnyCert;

static void any_start_chain(const br_x509_class **ctx, const char *server_name)
{
	AnyCert *a = (AnyCert *)(void *)ctx;
	(void)server_name;
	a->index = 0;
}

static void any_start_cert(const br_x509_class **ctx, uint32_t length)
{
	AnyCert *a = (AnyCert *)(void *)ctx;
	(void)length;
	if (a->index == 0)
		br_x509_decoder_init(&a->first, 0, 0);
}

static void any_append(const br_x509_class **ctx, const unsigned char *buf, size_t len)
{
	AnyCert *a = (AnyCert *)(void *)ctx;
	if (a->index == 0)
		br_x509_decoder_push(&a->first, buf, len);
}

static void any_end_cert(const br_x509_class **ctx)
{
	AnyCert *a = (AnyCert *)(void *)ctx;
	a->index++;
}

static unsigned any_end_chain(const br_x509_class **ctx)
{
	AnyCert *a = (AnyCert *)(void *)ctx;
	int err;
	if (a->index == 0)
		return BR_ERR_X509_EMPTY_CHAIN;
	err = br_x509_decoder_last_error(&a->first);
	if (err)
		return (unsigned)err;
	return br_x509_decoder_get_pkey(&a->first) ? 0 : BR_ERR_X509_BAD_SERVER_NAME;
}

static const br_x509_pkey *any_get_pkey(const br_x509_class *const *ctx, unsigned *usages)
{
	AnyCert *a = (AnyCert *)(void *)ctx;
	if (usages)
		*usages = BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN;
	return br_x509_decoder_get_pkey(&a->first);
}

static const br_x509_class any_vtable = {
	sizeof(AnyCert), any_start_chain, any_start_cert, any_append, any_end_cert, any_end_chain, any_get_pkey,
};

/* The one connection (static: 40 KB that a stack should not hold). */
static struct {
	int sock, tls;
	br_ssl_client_context sc;
	br_x509_minimal_context unused;	/* (what br_ssl_client_init_full sets up before AnyCert replaces it) */
	AnyCert any;
	br_sslio_context io;
	unsigned char buf[BR_SSL_BUFSIZE_BIDI];
} c = { -1 };

static int low_read(void *ctx, unsigned char *buf, size_t len)
{
	int n = upd_plat_recv(c.sock, buf, (int)(len > 0x4000 ? 0x4000 : len));
	(void)ctx;
	return n > 0 ? n : -1;
}

static int low_write(void *ctx, const unsigned char *buf, size_t len)
{
	int n = upd_plat_send(c.sock, buf, (int)(len > 0x4000 ? 0x4000 : len));
	(void)ctx;
	return n > 0 ? n : -1;
}

static void conn_close(void)
{
	if (c.sock >= 0)
		upd_plat_close(c.sock);
	c.sock = -1;
}

static int conn_open(const char *host, uint16_t port, int tls, char *err, int err_size)
{
	c.tls = tls;
	c.sock = upd_plat_connect(host, port);
	if (c.sock < 0) {
		snprintf(err, (size_t)err_size, "could not connect to %s (%d)", host, c.sock);
		c.sock = -1;
		return UPD_E_NET;
	}
	if (tls) {
		uint8_t seed[32];
		br_ssl_client_init_full(&c.sc, &c.unused, NULL, 0);
		br_ssl_engine_set_versions(&c.sc.eng, BR_TLS12, BR_TLS12);
		c.any.vtable = &any_vtable;
		br_ssl_engine_set_x509(&c.sc.eng, &c.any.vtable);
		br_ssl_engine_set_buffer(&c.sc.eng, c.buf, sizeof(c.buf), 1);
		upd_plat_entropy(seed);
		br_ssl_engine_inject_entropy(&c.sc.eng, seed, sizeof(seed));
		if (!br_ssl_client_reset(&c.sc, host, 0)) {
			snprintf(err, (size_t)err_size, "TLS could not start (%d)", br_ssl_engine_last_error(&c.sc.eng));
			conn_close();
			return UPD_E_TLS;
		}
		br_sslio_init(&c.io, &c.sc.eng, low_read, NULL, low_write, NULL);
	}
	return 0;
}

static int conn_write(const char *data, int len)
{
	if (c.tls)
		return br_sslio_write_all(&c.io, data, (size_t)len) == 0 && br_sslio_flush(&c.io) == 0 ? 0 : -1;
	while (len > 0) {
		int n = upd_plat_send(c.sock, (const uint8_t *)data, len);
		if (n <= 0)
			return -1;
		data += n;
		len -= n;
	}
	return 0;
}

/* > 0: bytes; <= 0: the end, or an error (which, the caller tells by what it still expected). */
static int conn_read(uint8_t *buf, int len)
{
	if (c.tls)
		return br_sslio_read(&c.io, buf, (size_t)len);
	return upd_plat_recv(c.sock, buf, len);
}

static void conn_error(char *err, int err_size, const char *what, const char *host)
{
	int tls = c.tls ? br_ssl_engine_last_error(&c.sc.eng) : 0;
	if (tls)
		snprintf(err, (size_t)err_size, "%s %s (TLS error %d)", what, host, tls);
	else
		snprintf(err, (size_t)err_size, "%s %s", what, host);
}

static int lower(int ch) { return ch >= 'A' && ch <= 'Z' ? ch + 32 : ch; }

/* The value of a header in a response's head ("\r\n" before each line, 0 at the end), or NULL. */
static const char *header(const char *head, const char *name)
{
	size_t n = strlen(name), i;
	const char *p;
	for (p = strstr(head, "\r\n"); p; p = strstr(p + 2, "\r\n")) {
		for (i = 0; i < n && lower(p[2 + i]) == lower(name[i]); i++)
			;
		if (i == n && p[2 + n] == ':') {
			p += 3 + n;
			while (*p == ' ' || *p == '\t')
				p++;
			return p;
		}
	}
	return NULL;
}

/* "https://host[:port]/path" in pieces; 0 or < 0. `authority` is "host[:port]" as written. */
static int url_split(const char *url, int *tls, char *authority, char *host, size_t size, uint16_t *port,
	const char **path)
{
	const char *p, *slash, *colon;
	size_t n;
	if (strncmp(url, "https://", 8) == 0) {
		*tls = 1;
		p = url + 8;
	} else if (strncmp(url, "http://", 7) == 0) {
		*tls = 0;
		p = url + 7;
	} else {
		return -1;
	}
	slash = strchr(p, '/');
	n = slash ? (size_t)(slash - p) : strlen(p);
	if (n == 0 || n >= size)
		return -1;
	memcpy(authority, p, n);
	authority[n] = 0;
	memcpy(host, p, n);
	host[n] = 0;
	*port = (uint16_t)(*tls ? 443 : 80);
	colon = strchr(host, ':');
	if (colon) {
		long v = strtol(colon + 1, NULL, 10);
		if (v < 1 || v > 65535)
			return -1;
		*port = (uint16_t)v;
		host[colon - host] = 0;
	}
	*path = slash ? slash : "/";
	return 0;
}

/* A response's body as it comes in, whole or in chunks ("Transfer-Encoding: chunked"). */
typedef struct {
	uint8_t *out;
	uint32_t max, got;
	uint32_t length;	/* Content-Length, or 0xFFFFFFFF: to the connection's end */
	int chunked, state;	/* chunked: 0 a size line, 1 its data, 2 the line end after it, 3 the trailer */
	uint32_t left;
	int digits, rest;	/* in a size line: it has digits; the digits are over (an extension, "\r") */
	int done, too_big, bad;
} Body;

static int hex_digit_of(int ch)
{
	if (ch >= '0' && ch <= '9')
		return ch - '0';
	ch = ch >= 'A' && ch <= 'Z' ? ch + 32 : ch;
	return ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1;
}

static void body_put(Body *b, const uint8_t *p, uint32_t n)
{
	if (n > b->max - b->got) {
		b->too_big = 1;
		n = b->max - b->got;
	}
	memcpy(b->out + b->got, p, n);
	b->got += n;
}

static void body_feed(Body *b, const uint8_t *p, uint32_t n)
{
	if (!b->chunked) {
		if (b->length != 0xFFFFFFFFu && n > b->length - b->got)
			n = b->length - b->got;
		body_put(b, p, n);
		if (b->got == b->length)
			b->done = 1;
		return;
	}
	while (n > 0 && !b->done && !b->bad && !b->too_big) {
		if (b->state == 0) {	/* "<hex>[;extension]\r\n" */
			int d = hex_digit_of(*p);
			if (d >= 0 && !b->rest) {
				if (b->left >> 28)
					b->bad = 1;
				b->left = b->left << 4 | (uint32_t)d;
				b->digits = 1;
			} else if (*p == '\n') {
				if (!b->digits)
					b->bad = 1;
				b->state = b->left ? 1 : 3;
				b->digits = b->rest = 0;
			} else {
				b->rest = 1;
			}
			p++;
			n--;
		} else if (b->state == 1) {
			uint32_t k = n < b->left ? n : b->left;
			body_put(b, p, k);
			p += k;
			n -= k;
			b->left -= k;
			if (b->left == 0)
				b->state = 2;
		} else if (b->state == 2) {
			if (*p == '\n')
				b->state = 0;
			else if (*p != '\r')
				b->bad = 1;
			p++;
			n--;
		} else {	/* after the last chunk: nothing more is needed */
			b->done = 1;
		}
	}
	if (b->state == 3)
		b->done = 1;
}

int upd_http_get(const char *url0, uint8_t *out, uint32_t max, uint32_t *got, int flags,
	const char *agent, UpdProgress progress, void *arg, char *err, int err_size)
{
	static char url[UPD_URL_MAX], next[UPD_URL_MAX], head[16384];
	char authority[128], host[128];
	const char *path, *v;
	uint16_t port;
	int hop, tls, n, have, status;
	uint32_t body_at, told;
	char *end;
	Body b;

	*got = 0;
	if (err_size > 0)
		err[0] = 0;
	if (strlen(url0) >= sizeof(url)) {
		snprintf(err, (size_t)err_size, "the address is too long");
		return UPD_E_HTTP;
	}
	strcpy(url, url0);
	for (hop = 0; hop < 6; hop++) {
		if (url_split(url, &tls, authority, host, sizeof(host), &port, &path) < 0 ||
		    (!tls && !(flags & UPD_TEST_URLS))) {
			snprintf(err, (size_t)err_size, "not an https address: %.60s", url);
			return UPD_E_HTTP;
		}
		n = conn_open(host, port, tls, err, err_size);
		if (n < 0)
			return n;
		n = snprintf(head, sizeof(head), "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\nAccept: */*\r\n"
			"Connection: close\r\n\r\n", path, authority, agent);
		if (n >= (int)sizeof(head) || conn_write(head, n) < 0) {
			conn_error(err, err_size, "could not send the request to", host);
			conn_close();
			return c.tls ? UPD_E_TLS : UPD_E_NET;
		}

		/* the head, up to its empty line */
		have = 0;
		end = NULL;
		while (!end) {
			if (have >= (int)sizeof(head) - 1) {
				snprintf(err, (size_t)err_size, "%s answered with more headers than fit", host);
				conn_close();
				return UPD_E_HTTP;
			}
			n = conn_read((uint8_t *)head + have, (int)sizeof(head) - 1 - have);
			if (n <= 0) {
				conn_error(err, err_size, have ? "the connection broke in the answer of" : "no answer from", host);
				conn_close();
				return c.tls && br_ssl_engine_last_error(&c.sc.eng) ? UPD_E_TLS : UPD_E_NET;
			}
			have += n;
			head[have] = 0;
			end = strstr(head, "\r\n\r\n");
		}
		body_at = (uint32_t)(end + 4 - head);
		end[2] = 0;	/* (the head as a string, each line with its "\r\n") */
		if (strncmp(head, "HTTP/1.", 7) != 0 || strlen(head) < 12) {
			snprintf(err, (size_t)err_size, "%s did not answer in HTTP", host);
			conn_close();
			return UPD_E_HTTP;
		}
		status = (int)strtol(head + 9, NULL, 10);

		if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
			size_t k;
			v = header(head, "Location");
			k = v ? strcspn(v, "\r\n") : 0;
			conn_close();
			if (!v || k == 0 || k >= sizeof(next) - 160) {
				snprintf(err, (size_t)err_size, "%s sent a redirect this cannot follow", host);
				return UPD_E_HTTP;
			}
			if (v[0] == '/')
				snprintf(next, sizeof(next), "%s://%s%.*s", tls ? "https" : "http", authority, (int)k, v);
			else
				snprintf(next, sizeof(next), "%.*s", (int)k, v);
			strcpy(url, next);
			continue;
		}
		if (status != 200) {
			conn_close();
			snprintf(err, (size_t)err_size, "%s answered %d", host, status);
			return status > 0 ? status : UPD_E_HTTP;
		}

		memset(&b, 0, sizeof(b));
		b.out = out;
		b.max = max;
		b.length = 0xFFFFFFFFu;
		v = header(head, "Transfer-Encoding");
		if (v && lower(v[0]) == 'c')
			b.chunked = 1;
		v = header(head, "Content-Length");
		if (v && !b.chunked)
			b.length = (uint32_t)strtoul(v, NULL, 10);
		if (b.length != 0xFFFFFFFFu && b.length > max) {
			conn_close();
			snprintf(err, (size_t)err_size, "the file is larger than expected (%lu bytes)", (unsigned long)b.length);
			return UPD_E_SIZE;
		}
		if (b.length == 0)
			b.done = 1;
		told = 0;
		body_feed(&b, (const uint8_t *)head + body_at, (uint32_t)have - body_at);
		while (!b.done && !b.bad && !b.too_big) {
			n = conn_read((uint8_t *)head, (int)sizeof(head));
			if (n <= 0)
				break;
			body_feed(&b, (const uint8_t *)head, (uint32_t)n);
			if (progress && b.got - told >= 0x8000) {
				told = b.got;
				progress(arg, b.got, b.length == 0xFFFFFFFFu ? 0 : b.length);
			}
		}
		if (b.too_big) {
			conn_close();
			snprintf(err, (size_t)err_size, "the file is larger than expected");
			return UPD_E_SIZE;
		}
		/* (a body with no length given ends when the connection does) */
		if (b.bad || (!b.done && (b.chunked || b.length != 0xFFFFFFFFu))) {
			conn_error(err, err_size, "the download broke off from", host);
			conn_close();
			return UPD_E_NET;
		}
		conn_close();
		*got = b.got;
		if (progress)
			progress(arg, b.got, b.got);
		return 200;
	}
	snprintf(err, (size_t)err_size, "too many redirects");
	return UPD_E_HTTP;
}
