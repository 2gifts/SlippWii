/* slippi_online/so_tls.h
 * A byte stream to a server: TLS 1.2 over so_tcp, or plain TCP (a test
 * server's ws://). One stream at a time, used from the matchmaking worker
 * thread only (so_tcp.h); every call returns within its timeout.
 *
 * TLS is BearSSL (MIT licence, as in the loader's updater), with the
 * smallest profile a current HTTPS host accepts: ECDHE with RSA or ECDSA
 * certificates, AES-GCM or ChaCha20-Poly1305. Unlike the updater, this
 * checks the server: its chain must end at one of the roots compiled in
 * (so_tls_roots.inc: the authorities Slippi's hosts use and the likeliest
 * to follow) and its certificate must name the host. A certificate's dates
 * are not checked (tools/build_bearssl.py says why).
 */
#ifndef SO_TLS_H
#define SO_TLS_H

#include "global.h"

/* Connects, and with `secure` runs the TLS handshake as `host`. Returns 0,
 * or < 0: so_tcp's errors, SO_TLS_FAILED (so_tls_error says why), or
 * SO_TLS_NO_MEMORY. */
int so_tls_open(const char *host, u32 addr, u16 port, int secure, u32 timeout_ms, volatile int *cancel);
/* Sends all of `data` (and flushes). Returns len, or < 0. */
int so_tls_write(const u8 *data, u32 len);
/* > 0 the bytes read, 0 none within the timeout, < 0 closed or failed. */
int so_tls_read(u8 *buf, u32 size, u32 timeout_ms);
void so_tls_close(void);

#define SO_TLS_FAILED -2100
#define SO_TLS_NO_MEMORY -2101

/* BearSSL's error for the stream (0: none; 62: the chain does not end at a
 * root we know; 56: the certificate is for another name). */
int so_tls_error(void);

/* Tests: one more root, a P-256 key (65 bytes: 04, X, Y) whose certificate
 * is named "CN=SlippWii Test Root". Never set against Slippi's own hosts
 * (so_mm_ws.c). NULL takes it away. */
void so_tls_set_test_root(const u8 *point);

/* Stirs timing and other unpredictable bytes into the seed of the next
 * handshake's random numbers. */
void so_tls_add_entropy(const void *data, u32 len);

/* Random bytes that cannot be worked out from the ones given before: for the bits a game sends
 * with each pad from Slippi 3.6.5 on, which the opponent sees (so_random's generator could be
 * read back from them). One caller's thread at a time. */
void so_tls_random(u8 *out, u32 len);

typedef struct {
	u32 connect_ms;	/* the TCP connection */
	u32 handshake_ms;	/* from there to the end of the handshake */
	u16 suite;	/* the cipher suite agreed (IANA's number) */
	u16 version;	/* 0x0303: TLS 1.2 */
	u32 received, sent;	/* bytes on the wire */
} SoTlsStats;
void so_tls_stats(SoTlsStats *out);

#endif
