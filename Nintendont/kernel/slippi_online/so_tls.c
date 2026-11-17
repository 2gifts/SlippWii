/* slippi_online/so_tls.c
 * TLS 1.2 client over so_tcp (see so_tls.h): BearSSL's engine, fed and
 * drained here a record at a time.
 */
#define SO_FILE_ID "tls"
#include "so_tls.h"
#include "so_alloc.h"
#include "so_platform.h"
#include "so_tcp.h"
#include "string.h"
#include "bearssl.h"

#include "so_tls_roots.inc"

#define ROOTS (sizeof(so_tls_roots) / sizeof(so_tls_roots[0]))
#define IO_STEP_MS 50

typedef struct {
	br_ssl_client_context cc;
	br_x509_minimal_context xc;
	br_x509_trust_anchor anchors[ROOTS + 1];
	unsigned char iobuf[BR_SSL_BUFSIZE_MONO];
} Tls;

static Tls *tls;	/* on the online heap while a secure stream is open */
static int is_open, is_secure, last_error;
static volatile int *cancel_flag;
static SoTlsStats stats;
static u8 test_root[65];
static int test_root_set;
/* "CN=SlippWii Test Root" as a certificate holds it */
static const unsigned char test_root_dn[] = {
	0x30, 0x1D, 0x31, 0x1B, 0x30, 0x19, 0x06, 0x03, 0x55, 0x04, 0x03, 0x0C, 0x12,
	'S', 'l', 'i', 'p', 'p', 'W', 'i', 'i', ' ', 'T', 'e', 's', 't', ' ', 'R', 'o', 'o', 't',
};

/* The seed's pool: whatever unpredictable bytes come by are hashed in, and
 * each handshake takes the pool's hash with the time. */
static br_sha256_context pool;
static int pool_ready;

void so_tls_add_entropy(const void *data, u32 len)
{
	u64 now = so_time_us();
	u32 ticks = so_ticks();
	if (!pool_ready) {
		br_sha256_init(&pool);
		pool_ready = 1;
	}
	br_sha256_update(&pool, &now, sizeof(now));
	br_sha256_update(&pool, &ticks, sizeof(ticks));
	if (data && len)
		br_sha256_update(&pool, data, len);
}

void so_tls_random(u8 *out, u32 len)
{
	static br_sha256_context own;
	static int ready;
	static u32 calls;
	unsigned char digest[32];
	br_sha256_context copy;
	u32 mix[3];
	if (!ready) {
		so_tls_add_entropy(&calls, sizeof(calls));
		own = pool;
		ready = 1;
	}
	while (len) {
		u32 take = len < 16 ? len : 16;	/* (half of each hash is given out) */
		mix[0] = calls++;
		mix[1] = so_ticks();
		mix[2] = so_random();
		br_sha256_update(&own, mix, sizeof(mix));
		copy = own;
		br_sha256_out(&copy, digest);
		memcpy(out, digest, take);
		out += take;
		len -= take;
	}
}

void so_tls_set_test_root(const u8 *point)
{
	test_root_set = point != NULL;
	if (point)
		memcpy(test_root, point, sizeof(test_root));
}

static const uint16_t suites[] = {
	BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
	BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
	BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
	BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
	BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
	BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
};

static void profile(Tls *t, const char *host)
{
	br_ssl_client_context *cc = &t->cc;
	br_x509_minimal_context *xc = &t->xc;
	u32 n = ROOTS, mix[4];
	unsigned char seed[32];
	br_sha256_context copy;

	memcpy(t->anchors, so_tls_roots, sizeof(so_tls_roots));
	if (test_root_set) {
		br_x509_trust_anchor *a = &t->anchors[n++];
		a->dn.data = (unsigned char *)test_root_dn;
		a->dn.len = sizeof(test_root_dn);
		a->flags = BR_X509_TA_CA;
		a->pkey.key_type = BR_KEYTYPE_EC;
		a->pkey.key.ec.curve = BR_EC_secp256r1;
		a->pkey.key.ec.q = test_root;
		a->pkey.key.ec.qlen = sizeof(test_root);
	}

	br_ssl_client_zero(cc);
	br_ssl_engine_set_versions(&cc->eng, BR_TLS12, BR_TLS12);
	br_x509_minimal_init(xc, &br_sha256_vtable, t->anchors, n);
	br_ssl_engine_set_suites(&cc->eng, suites, sizeof(suites) / sizeof(suites[0]));
	br_ssl_engine_set_rsavrfy(&cc->eng, &br_rsa_i31_pkcs1_vrfy);
	br_ssl_engine_set_ec(&cc->eng, &br_ec_prime_i31);
	br_ssl_engine_set_ecdsa(&cc->eng, &br_ecdsa_i31_vrfy_asn1);
	br_x509_minimal_set_rsa(xc, &br_rsa_i31_pkcs1_vrfy);
	br_x509_minimal_set_ecdsa(xc, &br_ec_prime_i31, &br_ecdsa_i31_vrfy_asn1);
	br_ssl_engine_set_hash(&cc->eng, br_sha256_ID, &br_sha256_vtable);
	br_ssl_engine_set_hash(&cc->eng, br_sha384_ID, &br_sha384_vtable);
	br_x509_minimal_set_hash(xc, br_sha256_ID, &br_sha256_vtable);
	br_x509_minimal_set_hash(xc, br_sha384_ID, &br_sha384_vtable);
	br_ssl_engine_set_prf_sha256(&cc->eng, &br_tls12_sha256_prf);
	br_ssl_engine_set_prf_sha384(&cc->eng, &br_tls12_sha384_prf);
	br_ssl_engine_set_chacha20(&cc->eng, &br_chacha20_ct_run);
	br_ssl_engine_set_poly1305(&cc->eng, &br_poly1305_ctmul_run);
	br_ssl_engine_set_chapol(&cc->eng, &br_sslrec_in_chapol_vtable, &br_sslrec_out_chapol_vtable);
	br_ssl_engine_set_aes_ctr(&cc->eng, &br_aes_small_ctr_vtable);
	br_ssl_engine_set_ghash(&cc->eng, &br_ghash_ctmul32);
	br_ssl_engine_set_gcm(&cc->eng, &br_sslrec_in_gcm_vtable, &br_sslrec_out_gcm_vtable);
	br_ssl_engine_set_x509(&cc->eng, &xc->vtable);
	br_ssl_engine_set_buffer(&cc->eng, t->iobuf, sizeof(t->iobuf), 0);

	/* The seed: the pool so far, the time, the generator of the session. */
	mix[0] = so_random();
	mix[1] = so_ticks();
	mix[2] = so_random();
	mix[3] = (u32)(size_t)t;
	so_tls_add_entropy(mix, sizeof(mix));
	copy = pool;
	br_sha256_out(&copy, seed);
	br_ssl_engine_inject_entropy(&cc->eng, seed, sizeof(seed));
	so_tls_add_entropy(seed, sizeof(seed));	/* (the next seed is another) */
	memset(seed, 0, sizeof(seed));

	br_ssl_client_reset(cc, host, 0);
}

/* Runs the engine until it can take (BR_SSL_SENDAPP) or has (BR_SSL_RECVAPP)
 * application bytes: 1; 0 when `timeout_ms` passed first; < 0 closed. */
static int pump(unsigned want, u32 timeout_ms)
{
	br_ssl_engine_context *eng = &tls->cc.eng;
	u32 start = so_time_ms();
	for (;;) {
		unsigned state = br_ssl_engine_current_state(eng);
		unsigned char *buf;
		size_t len;
		int n;
		if (state & BR_SSL_CLOSED) {
			last_error = br_ssl_engine_last_error(eng);
			return SO_TLS_FAILED;
		}
		if (state & BR_SSL_SENDREC) {
			buf = br_ssl_engine_sendrec_buf(eng, &len);
			n = so_tcp_send(buf, (u32)len, 5000);
			if (n < 0)
				return n;
			stats.sent += (u32)n;
			br_ssl_engine_sendrec_ack(eng, (size_t)n);
			continue;
		}
		if (state & want)
			return 1;
		if (!(state & BR_SSL_RECVREC))
			return SO_TLS_FAILED;	/* (unread application bytes in the way: not with this caller) */
		if (cancel_flag && *cancel_flag)
			return SO_TCP_CANCELLED;
		if (so_time_ms() - start >= timeout_ms)
			return 0;
		buf = br_ssl_engine_recvrec_buf(eng, &len);
		n = so_tcp_recv(buf, (u32)len, timeout_ms < IO_STEP_MS ? timeout_ms : IO_STEP_MS);
		if (n < 0)
			return n;
		if (n > 0) {
			stats.received += (u32)n;
			so_tls_add_entropy(&n, sizeof(n));	/* (when it came) */
			br_ssl_engine_recvrec_ack(eng, (size_t)n);
		}
	}
}

int so_tls_open(const char *host, u32 addr, u16 port, int secure, u32 timeout_ms, volatile int *cancel)
{
	u32 start = so_time_ms();
	int res;

	so_tls_close();
	memset(&stats, 0, sizeof(stats));
	last_error = 0;
	cancel_flag = cancel;
	res = so_tcp_open(addr, port, timeout_ms, cancel);
	stats.connect_ms = so_time_ms() - start;
	so_tls_add_entropy(&stats, sizeof(stats));
	if (res < 0)
		return res;
	is_open = 1;
	is_secure = secure;
	if (!secure)
		return 0;
	tls = (Tls *)so_malloc(sizeof(Tls));
	if (!tls) {
		so_tls_close();
		return SO_TLS_NO_MEMORY;
	}
	profile(tls, host);
	start = so_time_ms();
	res = pump(BR_SSL_SENDAPP, timeout_ms);
	stats.handshake_ms = so_time_ms() - start;
	if (res <= 0) {
		if (!last_error)
			last_error = br_ssl_engine_last_error(&tls->cc.eng);
		so_tls_close();
		return res < 0 ? res : SO_TCP_TIMEOUT;
	}
	stats.suite = tls->cc.eng.session.cipher_suite;
	stats.version = tls->cc.eng.session.version;
	return 0;
}

int so_tls_write(const u8 *data, u32 len)
{
	u32 done = 0;
	if (!is_open)
		return SO_TCP_CLOSED;
	if (!is_secure)
		return so_tcp_send(data, len, 5000);
	while (done < len) {
		unsigned char *buf;
		size_t room;
		int res = pump(BR_SSL_SENDAPP, 5000);
		if (res <= 0)
			return res < 0 ? res : SO_TCP_TIMEOUT;
		buf = br_ssl_engine_sendapp_buf(&tls->cc.eng, &room);
		if (room > len - done)
			room = len - done;
		memcpy(buf, data + done, room);
		br_ssl_engine_sendapp_ack(&tls->cc.eng, room);
		done += (u32)room;
	}
	br_ssl_engine_flush(&tls->cc.eng, 0);
	/* (the record leaves in the pump: it sends before it looks at `want`) */
	if (pump(BR_SSL_SENDAPP, 5000) < 0)
		return SO_TLS_FAILED;
	return (int)len;
}

int so_tls_read(u8 *buf, u32 size, u32 timeout_ms)
{
	unsigned char *from;
	size_t len;
	int res;
	if (!is_open)
		return SO_TCP_CLOSED;
	if (!is_secure)
		return so_tcp_recv(buf, size, timeout_ms);
	res = pump(BR_SSL_RECVAPP, timeout_ms);
	if (res <= 0)
		return res;
	from = br_ssl_engine_recvapp_buf(&tls->cc.eng, &len);
	if (len > size)
		len = size;
	memcpy(buf, from, len);
	br_ssl_engine_recvapp_ack(&tls->cc.eng, len);
	return (int)len;
}

void so_tls_close(void)
{
	if (tls) {
		if (so_tcp_is_open() && !(br_ssl_engine_current_state(&tls->cc.eng) & BR_SSL_CLOSED)) {
			/* the closing alert, if it leaves at once */
			unsigned char *buf;
			size_t len;
			br_ssl_engine_close(&tls->cc.eng);
			if (br_ssl_engine_current_state(&tls->cc.eng) & BR_SSL_SENDREC) {
				buf = br_ssl_engine_sendrec_buf(&tls->cc.eng, &len);
				so_tcp_send(buf, (u32)len, 200);
			}
		}
		/* (the keys of the session) */
		memset(tls, 0, sizeof(Tls));
		so_free(tls);
		tls = NULL;
	}
	so_tcp_close();
	is_open = 0;
}

int so_tls_error(void)
{
	return last_error;
}

void so_tls_stats(SoTlsStats *out)
{
	*out = stats;
}
