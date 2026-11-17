/* slippi_online/so_mm_ws.c
 * Matchmaking against Slippi's WebSocket service (Slippi Dolphin after
 * 3.6.4): slippi-rust-extensions' matchmaking client (client.rs) with the
 * same messages, timeouts and retries.
 *
 * The connection is TCP and TLS, whose calls take time (a handshake is a
 * second of arithmetic on this processor), so a session runs on a worker
 * thread of its own, below every thread the game or the netcode waits on.
 * The network thread starts it, watches its state (so_mmws_poll) and keeps
 * the router's mapping of the netplay socket open with STUN keepalives.
 * The connection is closed as soon as the match arrives: nothing of this
 * runs during a game.
 *
 * The join message holds the play key: it is built by the network thread,
 * kept only until the service has it, and never logged.
 */
#define SO_FILE_ID "mmws"
#include "so_mm.h"
#include "so_alloc.h"
#include "so_platform.h"
#include "so_socket.h"
#include "so_tcp.h"
#include "so_tls.h"
#include "so_util.h"
#include "so_ws.h"
#include "string.h"

#define CONNECT_TIMEOUT_MS 5000
#define HANDSHAKE_TIMEOUT_MS 10000
#define POLL_MS 250
#define CONNECT_ATTEMPTS 3
#define RESUME_WINDOW_MS 15000
#define RETRY_DELAY_MS 1000
#define JOIN_TIMEOUT_MS 5000
#define PING_AFTER_MS 5000
#define LIVENESS_TIMEOUT_MS 15000

#define WORKER_STACK 0x4000
#define WORKER_PRIORITY 0x30
#define STACK_FILL 0xC5

/* what the worker has come to */
enum { W_IDLE, W_CONNECTING, W_JOINING, W_QUEUED, W_MATCHED, W_FAILED };

typedef struct {
	SoEvent wake;	/* a session to run */
	volatile int running;	/* the worker is in a session */
	volatile int cancel;
	volatile int stage;	/* W_* */
	int started;	/* the thread exists */
	u8 *stack;

	/* the session's orders: written by the network thread while the worker is idle */
	char host[64], path[64];
	u32 addr;
	u16 port;
	int secure, default_port;
	char join[1536];	/* (the play key is in it) */
	int join_len;
	int pending;	/* a session waits for the worker to finish the one called off */

	/* what the worker found */
	char ticket_id[96], token[640];
	char match[SO_WS_MESSAGE_MAX];
	int match_len;
	char error[128], latest_version[24];

	/* the network thread's side */
	SoMmState state;
	SoMatchResult result;
	SoStun stun;
	u32 keepalive_ms;
	u16 udp_port;
} Session;

static Session *sp;
#define S (*sp)

static void fail(const char *text)
{
	strlcpy(S.error, text, sizeof(S.error));
	so_log("mm: %s", text);
	S.stage = W_FAILED;
}

/* Sleeps in small steps so that a search called off is noticed: 1 then. */
static int sleep_unless_cancelled(u32 ms)
{
	u32 start = so_time_ms();
	while (so_time_ms() - start < ms) {
		if (S.cancel)
			return 1;
		so_sleep_us(50000);
	}
	return S.cancel;
}

static void say_connection(void)
{
	SoTlsStats t;
	SoTcpStats tcp;
	so_tls_stats(&t);
	so_tcp_stats(&tcp);
	if (S.secure)
		so_log("mm: connected in %u ms, TLS handshake %u ms (suite %04x, %u bytes in, %u out; %u socket calls, "
			"longest %u us)", (unsigned)t.connect_ms, (unsigned)t.handshake_ms, t.suite, (unsigned)t.received,
			(unsigned)t.sent, (unsigned)tcp.calls, (unsigned)tcp.longest_us);
	else
		so_log("mm: connected in %u ms (no TLS: a test server)", (unsigned)t.connect_ms);
}

/* One connection, from the upgrade to its end: 1 the session is over (a
 * match, an error, called off), 0 the connection was lost with the ticket
 * perhaps still in the queue, -1 the service never took the join. */
static int serve(void)
{
	static const char ping[] = "{\"type\":\"ping\"}", cancel[] = "{\"type\":\"cancel\"}";
	u32 opened = so_time_ms(), last_frame = opened;
	int pinged = 0, acknowledged = 0;
	char *text = S.match;	/* (free until a match is in it) */

	for (;;) {
		int n, kind;
		if (S.cancel) {
			so_ws_send_text(cancel, sizeof(cancel) - 1);
			return 1;
		}
		n = so_ws_read_text(text, SO_WS_MESSAGE_MAX, POLL_MS);
		if (n < 0) {
			so_log("mm: the connection ended (%d, TLS %d)", n, so_tls_error());
			return 0;
		}
		if (n > 0) {
			last_frame = so_time_ms();
			pinged = 0;
			kind = so_mm_ws_kind(text, (u32)n);
			if (kind == SO_MMWS_JOINED) {
				if (so_mm_parse_joined(text, (u32)n, S.ticket_id, sizeof(S.ticket_id), S.token, sizeof(S.token)) == 0) {
					acknowledged = 1;
					/* the service has the join: the play key need not stay */
					memset(S.join, 0, sizeof(S.join));
					S.join_len = 0;
					if (S.stage != W_QUEUED)
						so_log("mm: in queue");
					S.stage = W_QUEUED;
				}
			} else if (kind == SO_MMWS_MATCH) {
				S.match_len = n;
				return 2;
			} else if (kind == SO_MMWS_ERROR) {
				char error[128];
				so_mm_parse_ws_error(text, (u32)n, error, sizeof(error), S.latest_version, sizeof(S.latest_version));
				if (S.latest_version[0])
					so_log("mm: server's latest version is %s", S.latest_version);
				fail(error);
				return 1;
			}
		}
		if (!acknowledged && so_time_ms() - opened >= JOIN_TIMEOUT_MS) {
			so_log("mm: the service did not acknowledge the connection in time");
			return S.ticket_id[0] ? 0 : -1;
		}
		if (so_time_ms() - last_frame >= LIVENESS_TIMEOUT_MS) {
			so_log("mm: nothing from the service for %u s", LIVENESS_TIMEOUT_MS / 1000);
			return 0;
		}
		if (so_time_ms() - last_frame >= PING_AFTER_MS && !pinged) {
			pinged = 1;
			so_ws_send_text(ping, sizeof(ping) - 1);
		}
	}
}

static void run_session(void)
{
	u32 connect_failures = 0, resume_deadline = 0;
	int resuming = 0;

	S.stage = W_CONNECTING;
	for (;;) {
		char first[sizeof(S.ticket_id) + sizeof(S.token) + 64];
		int res, len, ended;

		if (S.cancel)
			break;
		res = so_tls_open(S.host, S.addr, S.port, S.secure, CONNECT_TIMEOUT_MS + (S.secure ? HANDSHAKE_TIMEOUT_MS : 0),
			&S.cancel);
		if (res == 0) {
			say_connection();
			res = so_ws_open(S.host, S.port, S.default_port, S.path, HANDSHAKE_TIMEOUT_MS);
			if (res < 0)
				so_log("mm: the WebSocket did not open (%d, HTTP %d)", res, so_ws_status());
		} else if (res != SO_TCP_CANCELLED) {
			/* (with what the socket calls answered and how far the handshake got: a console's log
			 * is all there is to go by) */
			SoTlsStats t;
			SoTcpStats tcp;
			so_tls_stats(&t);
			so_tcp_stats(&tcp);
			so_log("mm: could not connect (%d, TLS error %d): connect %d..%d in %u calls and %u ms; handshake %u ms, "
				"%u bytes in, %u out; %u socket calls, longest %u us", res, so_tls_error(), (int)tcp.connect_first,
				(int)tcp.connect_last, (unsigned)tcp.connect_calls, (unsigned)t.connect_ms, (unsigned)t.handshake_ms,
				(unsigned)t.received, (unsigned)t.sent, (unsigned)tcp.calls, (unsigned)tcp.longest_us);
		}
		if (res == 0) {
			if (S.ticket_id[0]) {
				len = so_mm_build_resume(first, sizeof(first), S.ticket_id, S.token);
				res = len < 0 ? -1 : so_ws_send_text(first, (u32)len);
			} else {
				res = so_ws_send_text(S.join, (u32)S.join_len);
			}
			memset(first, 0, sizeof(first));
		}
		if (res < 0) {
			so_ws_close();
			so_tls_close();
			if (S.cancel)
				break;
			connect_failures++;
			if (resuming ? (s32)(so_time_ms() - resume_deadline) >= 0 : connect_failures >= CONNECT_ATTEMPTS) {
				/* (a certificate that is not trusted is its own message: the player can do nothing
				 * about the network then, and a newer SlippWii may know the authority) */
				fail(S.ticket_id[0] ? "Lost connection to the mm server" :
					so_tls_error() == 62 ? "Slippi server certificate not trusted" : "Failed to connect to mm server");
				break;
			}
			if (sleep_unless_cancelled(RETRY_DELAY_MS))
				break;
			continue;
		}
		connect_failures = 0;
		if (S.stage == W_CONNECTING)
			S.stage = W_JOINING;

		ended = serve();
		so_ws_close();
		so_tls_close();
		if (ended == 2) {
			S.stage = W_MATCHED;
			break;
		}
		if (ended == 1)
			break;
		if (ended < 0) {
			fail("Failed to join mm queue");
			break;
		}
		if (!S.ticket_id[0]) {
			fail("Lost connection to the mm server");
			break;
		}
		/* The ticket may still be queued: take it up again, for a while. */
		if (!resuming) {
			resuming = 1;
			resume_deadline = so_time_ms() + RESUME_WINDOW_MS;
		} else if ((s32)(so_time_ms() - resume_deadline) >= 0) {
			fail("Lost connection to the mm server");
			break;
		}
		so_log("mm: connection lost; taking the ticket up again");
		if (sleep_unless_cancelled(RETRY_DELAY_MS))
			break;
	}
	memset(S.join, 0, sizeof(S.join));
	S.join_len = 0;
	memset(S.token, 0, sizeof(S.token));
	S.ticket_id[0] = '\0';
	if (S.cancel && S.stage != W_MATCHED && S.stage != W_FAILED)
		S.stage = W_IDLE;
}

/* How much of the worker's stack has been written to (diagnostics). */
static u32 stack_used(void)
{
	u32 i;
	for (i = 0; i < WORKER_STACK && S.stack[i] == STACK_FILL; i++)
		;
	return WORKER_STACK - i;
}

static u32 Worker(void *unused)
{
	(void)unused;
	for (;;) {
		so_event_wait(&S.wake, 0);
		if (!S.running)
			continue;
		run_session();
		so_log("mm: session over (stage %d); the worker's stack: %u of %u bytes", S.stage, (unsigned)stack_used(),
			WORKER_STACK);
		S.running = 0;
	}
	return 0;
}

static void launch(void)
{
	S.cancel = 0;
	S.pending = 0;
	S.match_len = 0;
	S.ticket_id[0] = '\0';
	S.stage = W_CONNECTING;
	S.running = 1;
	so_event_signal(&S.wake);
}

int so_mmws_start(const SoConfig *config, const SoUser *user, const SoSearch *search, u32 server, u16 port,
	const SoStun *stun)
{
	char lan[32], lan_ip[16], join[sizeof(S.join)];
	u32 local;
	int len;

	if (!sp) {
		sp = (Session *)so_malloc(sizeof(Session));
		if (!sp)
			return -1;
		memset(sp, 0, sizeof(Session));
		if (so_event_init(&S.wake) < 0)
			return -1;
	}
	so_mmws_stop();
	memset(&S.result, 0, sizeof(S.result));
	S.error[0] = '\0';
	S.latest_version[0] = '\0';
	S.stun = *stun;
	S.udp_port = port;
	S.keepalive_ms = so_time_ms();

	if (!S.started) {
		S.stack = (u8 *)so_malloc(WORKER_STACK);
		if (!S.stack)
			return -1;
		memset(S.stack, STACK_FILL, WORKER_STACK);
		if (so_thread_start(Worker, NULL, S.stack + WORKER_STACK, WORKER_STACK, WORKER_PRIORITY) < 0) {
			strlcpy(S.error, "Failed to create mm client", sizeof(S.error));
			return -1;
		}
		S.started = 1;
	}

	local = config->lan_ip ? config->lan_ip : so_local_ip();
	so_format_ipv4(local, lan_ip, sizeof(lan_ip));
	so_snprintf(lan, sizeof(lan), "%s:%u", lan_ip, (unsigned)port);
	len = so_mm_build_join(join, sizeof(join), config, user, search, local ? lan : "", port, stun);
	if (len < 0) {
		strlcpy(S.error, "Failed to build mm ticket", sizeof(S.error));
		return -2;
	}
	/* (the worker reads its orders only between sessions: one called off may still be ending) */
	{
		int waited;
		for (waited = 0; S.running && waited < 300; waited++)
			so_sleep_us(10000);
		if (S.running) {
			strlcpy(S.error, "Failed to create mm client", sizeof(S.error));
			return -4;
		}
	}
	{
		int secure;
		u16 url_port;
		if (so_mm_parse_url(config->mm_url, &secure, S.host, sizeof(S.host), &url_port, S.path, sizeof(S.path)) < 0) {
			strlcpy(S.error, "Failed to create mm client", sizeof(S.error));
			return -3;
		}
		S.secure = secure;
		S.port = url_port;
		S.default_port = url_port == (secure ? 443 : 80);
	}
	S.addr = server;
	memcpy(S.join, join, sizeof(S.join));
	S.join_len = len;
	memset(join, 0, sizeof(join));
	/* Tests: a root of their own, never against Slippi's hosts. */
	so_tls_set_test_root(config->test_tls_root_set && !so_config_is_real_mm(config) ? config->test_tls_root : NULL);
	so_tls_add_entropy(search, sizeof(*search));

	so_log("mm: connecting to %s (mode %d, LAN address %s, NAT %s%s)", S.host, search->mode, local ? lan : "unknown",
		so_stun_nat_type(stun), stun->have[0] || stun->have[1] ? "" : ": no STUN answer");
	S.state = SO_MM_CONNECTING;
	launch();
	return 0;
}

void so_mmws_poll(void)
{
	u8 junk[64];
	u32 addr;
	u16 port;

	if (!sp || S.state == SO_MM_IDLE || S.state == SO_MM_MATCHED || S.state == SO_MM_ERROR)
		return;
	/* Keeps the router's mapping of the netplay socket open while the search
	 * runs; the answers are dropped, so the netplay host does not find them. */
	if (so_time_ms() - S.keepalive_ms >= SO_STUN_KEEPALIVE_MS) {
		S.keepalive_ms = so_time_ms();
		so_stun_keepalive(&S.stun);
	}
	while (so_socket_receive(&addr, &port, junk, sizeof(junk), NULL) > 0)
		;

	switch (S.stage) {
	case W_JOINING:
		S.state = SO_MM_JOINING;
		break;
	case W_QUEUED:
		if (S.state != SO_MM_SEARCHING)
			SO_PHASE("mm in queue");
		S.state = SO_MM_SEARCHING;
		break;
	case W_MATCHED:
		if (S.running)
			break;	/* (the worker is closing the connection) */
		{
			char error[128];
			if (so_mm_parse_match_ws(S.match, (u32)S.match_len, &S.result, error, sizeof(error)) < 0) {
				strlcpy(S.error, error, sizeof(S.error));
				so_log("mm: %s", error);
				S.state = SO_MM_ERROR;
			} else {
				const SoMatchResult *r = &S.result;
				so_log("mm: matched (%s): %d players, we are port %d%s", r->match_id, r->player_count,
					r->players[r->local_player_index].port, r->is_host ? ", decider" : "");
				SO_PHASE("mm matched");
				S.state = SO_MM_MATCHED;
			}
			memset(S.match, 0, sizeof(S.match));
		}
		break;
	case W_FAILED:
		if (!S.running)
			S.state = SO_MM_ERROR;
		break;
	default:
		break;
	}
}

void so_mmws_stop(void)
{
	if (!sp)
		return;
	if (S.running)
		S.cancel = 1;
	S.state = SO_MM_IDLE;
}

SoMmState so_mmws_state(void)
{
	return sp ? S.state : SO_MM_IDLE;
}

const char *so_mmws_error(void)
{
	return sp ? S.error : "Failed to create mm client";
}

const char *so_mmws_latest_version(void)
{
	return sp ? S.latest_version : "";
}

const SoMatchResult *so_mmws_result(void)
{
	return sp ? &S.result : NULL;
}
