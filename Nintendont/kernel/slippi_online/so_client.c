/* slippi_online/so_client.c
 * Lifecycle and network thread (see so_client.h).
 *
 * Search flow, as in SlippiMatchmaking: matchmaking, then netplay on the
 * same port. If the opponents cannot be reached within 8 s, search again
 * (Teams gives up instead). Unlike Dolphin, which picks a new random port per
 * ticket unless its port is forced, the port stays the same for the session.
 */
#define SO_FILE_ID "client"
#include "so_client.h"
#include "so_alloc.h"
#include "so_diag.h"
#include "so_exi.h"
#include "so_files.h"
#include "so_platform.h"
#include "so_selftest.h"
#include "so_socket.h"
#include "so_util.h"
#include "string.h"
#include "enet/enet.h"

static void load_delay(void);
static void save_delay(void);

extern char __slippi_online_stack_addr, __slippi_online_stack_size;

#define NET_THREAD_PRIORITY 0x78
#define SERVICE_US 5000	/* ENet timers while active; datagrams wake us at once */
#define IDLE_US 100000
#define STATUS_PERIOD_MS 1000
#define LOG_PERIOD_MS 250

static struct {
	SoConfig config;
	SoUser user;
	SoMutex lock;
	SoEvent wake;
	SoClientState state;
	char error[128];
	SoSearch search;
	u8 search_requested;
	u8 cancel_requested;
	u16 port;
	u32 searches;	/* tickets this search, including retries */
	SoMatchResult match;
	u32 status_ms;
	u32 log_sent_ms;
	u32 init_ms;
	int main_priority;	/* as last set (SO_MAIN_THREAD_PRIORITY at start) */
	u8 game_asked;	/* the game has asked for a match: boot is over */
} c;

static const char *const state_names[] = {
	"off", "idle", "searching", "connecting", "connected", "disconnected", "error",
};

const char *so_client_state_name(SoClientState state)
{
	return (unsigned)state < sizeof(state_names) / sizeof(state_names[0]) ? state_names[state] : "?";
}

/* While searching or playing, the kernel's main loop runs below IOS's
 * network module (online_main_priority); otherwise at its usual 0x50. */
volatile u32 so_main_loop_sleep_us = 20;

/* Pads from the EXI device (main thread) to the network thread: one
 * producer, one consumer, no lock. */
#define PAD_QUEUE 32
typedef struct {
	SoLocalPad pad;
	u8 has_pad;
	u64 queued_us;
} QueuedPad;
static QueuedPad *pad_queue;	/* on the online heap (the kernel's bss is full) */
static volatile u32 pad_head, pad_tail;
static u32 pad_latency_max;

void so_client_queue_pad(const SoLocalPad *pad)
{
	u32 h = pad_head;
	if (!pad_queue || h - pad_tail >= PAD_QUEUE)
		return;	/* the network thread is far behind; it resends what is not acked */
	pad_queue[h % PAD_QUEUE].has_pad = pad != NULL;
	if (pad)
		pad_queue[h % PAD_QUEUE].pad = *pad;
	pad_queue[h % PAD_QUEUE].queued_us = so_time_us();
	pad_head = h + 1;
	so_event_signal(&c.wake);
}

u32 so_client_take_pad_latency_us(void)
{
	u32 v = pad_latency_max;
	pad_latency_max = 0;
	return v;
}

/* Sends the queued pads (the lock is held). */
static void send_queued_pads(void)
{
	while (pad_queue && pad_tail != pad_head) {
		u32 t = pad_tail;
		u32 waited = (u32)(so_time_us() - pad_queue[t % PAD_QUEUE].queued_us);
		if (so_np_status() == SO_NP_CONNECTED)
			so_np_send_pad(pad_queue[t % PAD_QUEUE].has_pad ? &pad_queue[t % PAD_QUEUE].pad : NULL);
		if (waited > pad_latency_max)
			pad_latency_max = waited;
		pad_tail = t + 1;
	}
}

static void update_main_priority(void)
{
	int online = c.state == SO_CLIENT_SEARCHING || c.state == SO_CLIENT_CONNECTING ||
		c.state == SO_CLIENT_CONNECTED;
	int want = online && c.config.online_main_priority ? c.config.online_main_priority : SO_MAIN_THREAD_PRIORITY;
	so_main_loop_sleep_us = online && c.config.online_loop_us ? c.config.online_loop_us : 20;
	if (want == c.main_priority)
		return;
	if (so_set_main_thread_priority(want) >= 0) {
		so_log("client: kernel main thread priority 0x%02x", want);
		c.main_priority = want;
	}
}

static void set_state(SoClientState state)
{
	if (c.state != state)
		so_log("client: %s -> %s", so_client_state_name(c.state), so_client_state_name(state));
	c.state = state;
	c.status_ms = 0;	/* refresh the broadcast status now */
	update_main_priority();
}

static void set_error(const char *text)
{
	strlcpy(c.error, text, sizeof(c.error));
	set_state(SO_CLIENT_ERROR);
}

static void stop_all(void)
{
	so_np_stop();
	so_mm_stop();
}

#define PORT_ATTEMPTS 15	/* as SlippiMatchmaking */

/* Opens the session's UDP socket, trying new random ports when binding
 * fails (unless local_port fixes it). One port serves the whole session,
 * like Dolphin with a forced netplay port, so the socket never has to close
 * (see so_socket.h). The lock is held. */
static int open_session_socket(void)
{
	int attempt, res = -1;
	for (attempt = 0; attempt < PORT_ATTEMPTS; attempt++) {
		if (!c.port)
			c.port = c.config.local_port ? c.config.local_port : (u16)(41000 + so_random() % 10000);
		res = so_socket_open(c.port);
		if (res >= 0)
			return 0;
		so_log("client: could not open UDP port %u (%d)", (unsigned)c.port, res);
		if (c.config.local_port)
			break;
		c.port = 0;
	}
	return res;
}

/* Starts (or restarts) matchmaking. Called with the lock held; drops it
 * while the server's name resolves, since DNS blocks. */
static void begin_search(void)
{
	char host[sizeof(c.config.mm_host)];
	u32 server;
	int res;

	c.searches++;
	strlcpy(host, c.config.mm_host, sizeof(host));
	SO_TRACE(SO_THREAD_NET);
	SO_STEP("mm resolve");
	so_mutex_unlock(&c.lock);
	res = so_resolve(host, &server);
	so_mutex_lock(&c.lock);
	SO_TRACE(SO_THREAD_NET);
	if (c.cancel_requested || c.search_requested)
		return;	/* superseded while resolving */
	if (res < 0) {
		so_log("client: could not resolve %s (%d)", host, res);
		set_error("Could not reach the Slippi server");
		return;
	}
	SO_TRACE(SO_THREAD_NET);
	SO_STEP("mm open socket");
	if (open_session_socket() < 0) {
		set_error("Failed to create mm client");
		return;
	}
	SO_TRACE(SO_THREAD_NET);
	if (so_mm_start(&c.config, &c.user, &c.search, server, c.port) < 0) {
		set_error(so_mm_error());
		return;
	}
	set_state(SO_CLIENT_SEARCHING);
}

/* One step of the state machine; the lock is held. */
static void step(void)
{
	switch (c.state) {
	case SO_CLIENT_SEARCHING:
		so_mm_poll();
		if (so_mm_state() == SO_MM_ERROR) {
			set_error(so_mm_error());
		} else if (so_mm_state() == SO_MM_MATCHED) {
			c.match = *so_mm_result();
			so_mm_stop();
			if (so_np_start(&c.match, c.port, &c.config) < 0)
				set_error("Failed to start netplay");
			else
				set_state(SO_CLIENT_CONNECTING);
		}
		break;
	case SO_CLIENT_CONNECTING:
		so_np_poll();
		if (so_np_status() == SO_NP_CONNECTED) {
			set_state(SO_CLIENT_CONNECTED);
		} else if (so_np_status() == SO_NP_FAILED) {
			so_np_stop();
			if (c.search.mode == SO_MODE_TEAMS) {
				set_error("Timed out waiting for other players to connect");
			} else {
				so_log("client: could not reach the opponent; searching again");
				begin_search();
			}
		}
		break;
	case SO_CLIENT_CONNECTED:
		so_np_poll();
		if (so_np_status() == SO_NP_DISCONNECTED)
			set_state(SO_CLIENT_DISCONNECTED);
		break;
	case SO_CLIENT_DISCONNECTED:
		so_np_poll();	/* lets the disconnects complete */
		break;
	default:
		break;
	}
}

/* Sends pending log lines to the PC (log_host) in batched datagrams, at most
 * every LOG_PERIOD_MS: IOS sends are slow. */
static void forward_logs(void)
{
	static char datagram[1200];
	char line[192];
	int len, n;
	u32 now = so_time_ms();
	if (!c.config.log_host || now - c.log_sent_ms < LOG_PERIOD_MS || !so_client_traffic_allowed())
		return;
	c.log_sent_ms = now;
	len = so_snprintf(datagram, sizeof(datagram), "SLPWII LOGS\n");
	while ((n = so_log_take(line, sizeof(line))) > 0) {
		if (len + n + 1 >= (int)sizeof(datagram)) {
			so_socket_send_aux(c.config.log_host, c.config.log_port, datagram, len);
			len = so_snprintf(datagram, sizeof(datagram), "SLPWII LOGS\n");
		}
		memcpy(datagram + len, line, n);
		len += n;
		datagram[len++] = '\n';
	}
	if (len > 12)
		so_socket_send_aux(c.config.log_host, c.config.log_port, datagram, len);
}

/* The short status in the LAN broadcast's nickname field. */
static void update_status(void)
{
	u32 now = so_time_ms();
	if (c.status_ms && now - c.status_ms < STATUS_PERIOD_MS)
		return;
	c.status_ms = now ? now : 1;
	if (c.state == SO_CLIENT_CONNECTED)
		so_set_status("SO connected %ums", (unsigned)(so_np_ping_us(0) / 1000));
	else if (c.state == SO_CLIENT_SEARCHING)
		so_set_status("SO searching #%u", (unsigned)c.searches);
	else
		so_set_status("SO %s", so_client_state_name(c.state));
}

static u32 NetworkThread(void *unused)
{
	(void)unused;
	for (;;) {
		u32 wait = IDLE_US;
		if (c.state == SO_CLIENT_SEARCHING || c.state == SO_CLIENT_CONNECTING ||
		    c.state == SO_CLIENT_CONNECTED || c.state == SO_CLIENT_DISCONNECTED)
			wait = so_np_wait_hint_us(SERVICE_US);
		SO_TRACE(SO_THREAD_NET);
		if (wait)
			so_event_wait(&c.wake, wait);

		SO_TRACE(SO_THREAD_NET);
		so_mutex_lock(&c.lock);
		if (c.cancel_requested) {
			c.cancel_requested = 0;
			stop_all();
			set_state(SO_CLIENT_IDLE);
		}
		if (c.search_requested) {
			c.search_requested = 0;
			stop_all();
			c.searches = 0;
			begin_search();
		}
		SO_TRACE(SO_THREAD_NET);
		send_queued_pads();	/* first: they are the game's inputs */
		step();
		update_status();
		so_mutex_unlock(&c.lock);

		forward_logs();
		so_exi_save_history();	/* after a search added a code (an SD write; not in a game) */
		save_delay();
	}
	return 0;
}

int so_client_init(void)
{
	void *arena;
	u32 arena_size;
	int res;
	if (so_platform_init() < 0)
		return -1;
	so_capture_main_thread();
	c.main_priority = SO_MAIN_THREAD_PRIORITY;
	SO_PHASE("init");
	arena = so_platform_arena(&arena_size);
	if (so_alloc_init(arena, arena_size) < 0)
		return -1;
	if (so_mutex_init(&c.lock) < 0 || so_event_init(&c.wake) < 0)
		return -2;
	pad_queue = (QueuedPad *)so_malloc(PAD_QUEUE * sizeof(QueuedPad));
	SO_PHASE("init socket");
	if (so_socket_init() < 0 || enet_ios_initialize() < 0 || so_np_init() < 0)
		return -3;
	so_socket_set_wake(&c.wake);

	c.init_ms = so_time_ms();
	SO_PHASE("init config");
	res = so_config_load(&c.config, &c.user);
	load_delay();
	so_step_pause_us = (u32)c.config.diag_step_pause_ms * 1000;
	so_diag_start(&c.config);
	if (so_config_is_real_mm(&c.config))
		so_log("client: matchmaking %s:%u, app version %s, delay %u", c.config.mm_host,
			(unsigned)c.config.mm_port, c.config.app_version, (unsigned)c.config.delay);
	else
		so_log("client: TEST matchmaking server %s:%u", c.config.mm_host, (unsigned)c.config.mm_port);

	SO_PHASE("init threads");
	if (so_thread_start(NetworkThread, NULL, &__slippi_online_stack_addr, (u32)(size_t)&__slippi_online_stack_size,
			NET_THREAD_PRIORITY) < 0)
		return -4;

	so_mutex_lock(&c.lock);
	set_state(res == 0 ? SO_CLIENT_IDLE : SO_CLIENT_OFF);
	so_mutex_unlock(&c.lock);
	SO_PHASE(res < 0 ? "no account" : "idle");
	if (res < 0)
		return -5;

	/* Slippi's codeset and menu files, for the game's online modes. */
	so_files_init();
	if (so_client_online_game())
		so_files_preload();
	if (c.config.selftest)
		so_selftest_start(&c.config);
	return 0;
}

int so_client_online_game(void)
{
	const u8 *gct;
	return c.state != SO_CLIENT_OFF && !c.config.selftest && so_files_gct(&gct) > 0;
}

const SoConfig *so_client_config(void)
{
	return &c.config;
}

#define SO_DELAY_PATH "/slippwii/delay.txt"
static volatile int delay_dirty;

int so_client_step_delay(int step)
{
	int d = c.config.delay + step;
	if (d < 1)
		d = 1;
	if (d > 9)
		d = 9;
	if (d != c.config.delay) {
		c.config.delay = (u8)d;
		delay_dirty = 1;
		so_log("client: delay %d (set on the Wii)", d);
	}
	return d;
}

/* delay.txt, written from the online CSS's delay buttons, wins over config.ini. */
static void load_delay(void)
{
	char text[8];
	int n = so_read_file(SO_DELAY_PATH, text, sizeof(text) - 1);
	if (n > 0 && text[0] >= '1' && text[0] <= '9')
		c.config.delay = (u8)(text[0] - '0');
}

static void save_delay(void)
{
	char text[3];
	if (!delay_dirty)
		return;
	delay_dirty = 0;
	text[0] = (char)('0' + c.config.delay);
	text[1] = '\n';
	text[2] = 0;
	if (so_write_file(SO_DELAY_PATH, text, 2) < 0)
		so_log("client: could not save the delay");
}

const SoUser *so_client_user(void)
{
	return &c.user;
}

void so_client_find_match(const SoSearch *search)
{
	so_mutex_lock(&c.lock);
	c.game_asked = 1;
	if (c.state != SO_CLIENT_OFF && search->mode == SO_MODE_RANKED) {
		/* SlippWii does not play Ranked (Slippi's protected mode; its set
		 * rules and reporting are not implemented). The menu hides it
		 * (tools/ppc/hide_ranked.s); this is the backstop. */
		so_log("client: Ranked is not available on SlippWii");
		c.cancel_requested = 1;
		c.search_requested = 0;
		set_error("Ranked is not available on SlippWii");
	} else if (c.state != SO_CLIENT_OFF) {
		c.search = *search;
		c.search_requested = 1;
		c.cancel_requested = 0;
		c.error[0] = '\0';
		set_state(SO_CLIENT_SEARCHING);
	}
	so_mutex_unlock(&c.lock);
	so_event_signal(&c.wake);
}

void so_client_cancel(void)
{
	so_mutex_lock(&c.lock);
	if (c.state != SO_CLIENT_OFF) {
		c.cancel_requested = 1;
		c.search_requested = 0;
		/* The game's next question must already see it (0xBA); the network
		 * thread tears down. */
		set_state(SO_CLIENT_IDLE);
	}
	so_mutex_unlock(&c.lock);
	so_event_signal(&c.wake);
}

int so_client_diagnostics(void)
{
	return c.config.status_beacon;
}

const char *so_client_beacon(void)
{
	return c.config.status_beacon ? so_diag_beacon() : NULL;
}

void so_client_apply_settings(const char *text, u32 len)
{
	so_mutex_lock(&c.lock);
	so_config_parse(&c.config, text, len);
	so_step_pause_us = (u32)c.config.diag_step_pause_ms * 1000;
	so_mutex_unlock(&c.lock);
}

int so_client_open_socket(u16 *port)
{
	int res;
	so_mutex_lock(&c.lock);
	res = open_session_socket();
	*port = c.port;
	so_mutex_unlock(&c.lock);
	return res;
}

int so_client_traffic_allowed(void)
{
	return c.game_asked || so_time_ms() - c.init_ms >= (u32)c.config.boot_quiet_s * 1000;
}

SoClientState so_client_state(void)
{
	return c.state;
}

void so_client_error(char *out, int size)
{
	so_mutex_lock(&c.lock);
	strlcpy(out, c.state == SO_CLIENT_ERROR ? c.error : "", size);
	so_mutex_unlock(&c.lock);
}

const SoMatchResult *so_client_match(void)
{
	return &c.match;
}

void so_client_lock(void)
{
	so_mutex_lock(&c.lock);
}

void so_client_unlock(void)
{
	so_mutex_unlock(&c.lock);
}
