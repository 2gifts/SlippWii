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
#include "so_logfile.h"
#include "so_prefetch.h"
#include "so_platform.h"
#include "so_selftest.h"
#include "so_socket.h"
#include "so_util.h"
#include "string.h"
#include "enet/enet.h"

static void load_delay(void);
static void save_delay(void);
static void load_rumble(void);
static void save_rumble(void);
static void load_ping_display(void);
static void save_ping_display(void);
static void load_latest_version(void);
static void save_latest_version(void);
static void note_latest_version(const char *version);
static int on_wifi(int quiet);

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
	u64 sender_checked_us;
	u8 game_asked;	/* the game has asked for a match: boot is over */
	/* the newest version Slippi's server has named (SO_LATEST_VERSION_PATH),
	 * and whether it is over this build's */
	char latest[24];
	u8 latest_dirty;
	u8 outdated;
} c;

static const char *const state_names[] = {
	"off", "idle", "searching", "connecting", "connected", "disconnected", "error",
};

const char *so_client_state_name(SoClientState state)
{
	return (unsigned)state < sizeof(state_names) / sizeof(state_names[0]) ? state_names[state] : "?";
}

/* While searching or playing, the kernel's main loop runs above the threads
 * that could hold up the game's polls (online_main_priority) and sleeps longer
 * between passes (online_loop_us); otherwise at its usual 0x50 and 20 us. */
volatile u32 so_main_loop_sleep_us = 20;
volatile u32 so_main_loop_quick;

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
static u32 pad_send_sum, pad_send_n, pad_send_max;	/* so_np_send_pad's time, us */

/* (3.7.0) An input suggestion (so_exi.c), through the same queue: its target frame in `frame`, its
 * seq in `checksum`, its inputs in `pad`. */
void so_client_queue_suggestion(const SoLocalPad *s)
{
	u32 h = pad_head;
	if (!pad_queue || h - pad_tail >= PAD_QUEUE)
		return;
	pad_queue[h % PAD_QUEUE].has_pad = 2;
	pad_queue[h % PAD_QUEUE].pad = *s;
	pad_queue[h % PAD_QUEUE].queued_us = so_time_us();
	pad_head = h + 1;
	so_event_signal(&c.wake);
}

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

/* How long handing a pad to the network took (ENet and the socket's send),
 * on average and at most, since the last call. */
void so_client_take_pad_send_us(u32 *avg, u32 *max)
{
	*avg = pad_send_n ? pad_send_sum / pad_send_n : 0;
	*max = pad_send_max;
	pad_send_sum = pad_send_n = pad_send_max = 0;
}

u32 so_client_take_pad_latency_us(void)
{
	u32 v = pad_latency_max;
	pad_latency_max = 0;
	return v;
}

/* Sends the queued pads (the lock is held). */
/* How long the network thread held the lock the game's polls wait for, and doing what: sending
 * the pads, ENet's service */
static u32 hold_max_us[2], hold_long[2];

static void note_hold(int what, u64 us)
{
	if (us > hold_max_us[what])
		hold_max_us[what] = (u32)us;
	if (us > 1500)
		hold_long[what]++;
}

void so_client_log_holds(void)
{
	so_log("game lock: sending pads held it up to %u us, %u over 1.5 ms; ENet's service up to %u us, %u over 1.5 ms",
		(unsigned)hold_max_us[0], (unsigned)hold_long[0], (unsigned)hold_max_us[1], (unsigned)hold_long[1]);
	hold_max_us[0] = hold_max_us[1] = hold_long[0] = hold_long[1] = 0;
}

static void send_queued_pads(void)
{
	while (pad_queue && pad_tail != pad_head) {
		u32 t = pad_tail;
		u32 waited = (u32)(so_time_us() - pad_queue[t % PAD_QUEUE].queued_us);
		if (so_np_status() == SO_NP_CONNECTED) {
			u64 t0 = so_time_us();
			u32 took;
			if (pad_queue[t % PAD_QUEUE].has_pad == 2)
				so_np_send_suggestion(pad_queue[t % PAD_QUEUE].pad.frame, pad_queue[t % PAD_QUEUE].pad.checksum,
					pad_queue[t % PAD_QUEUE].pad.pad);
			else
				so_np_send_pad(pad_queue[t % PAD_QUEUE].has_pad ? &pad_queue[t % PAD_QUEUE].pad : NULL);
			took = (u32)(so_time_us() - t0);
			pad_send_sum += took;
			pad_send_n++;
			if (took > pad_send_max)
				pad_send_max = took;
		}
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
	static u8 reported;
	strlcpy(c.error, text, sizeof(c.error));
	set_state(SO_CLIENT_ERROR);
	/* (a report: written now, out of a game, and ended; three a boot at most, a player may try again
	 * and again) */
	if (reported < 3) {
		reported++;
		so_logfile_event("connect", text, 1);
	}
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
	char host[sizeof(c.config.mm_url)], path[sizeof(c.config.mm_url)], stun_hosts[sizeof(c.config.stun_hosts)];
	SoStun stun;
	u32 server = c.config.test_mm_addr;
	u16 url_port;
	int res, secure, ws = c.config.mm_url[0] != 0;

	c.searches++;
	memset(&stun, 0, sizeof(stun));
	strlcpy(host, c.config.mm_host, sizeof(host));
	strlcpy(stun_hosts, c.config.stun_hosts, sizeof(stun_hosts));
	if (ws) {
		/* Slippi's WebSocket service (so_mm_ws.c): it never sees the session's
		 * UDP socket, so that is opened first and STUN servers are asked where
		 * opponents can reach it. */
		if (so_mm_parse_url(c.config.mm_url, &secure, host, sizeof(host), &url_port, path, sizeof(path)) < 0) {
			so_log("client: mm_url is not a ws:// or wss:// address");
			set_error("Failed to create mm client");
			return;
		}
		if (open_session_socket() < 0) {
			set_error("Failed to create mm client");
			return;
		}
		so_socket_flush();
	}
	SO_TRACE(SO_THREAD_NET);
	SO_STEP("mm resolve");
	so_mutex_unlock(&c.lock);
	/* Wired only: over Wi-Fi an IOS send takes 13-18 ms, and the opponent gets
	 * every input late (so_socket.h). */
	res = on_wifi(0) ? -100 : ws && server ? 0 : so_resolve(host, &server);
	if (ws && res == 0) {
		int answers = so_stun_discover(stun_hosts, &stun, (volatile u8 *)&c.cancel_requested);
		char seen[16];
		so_format_ipv4(stun.addr[0], seen, sizeof(seen));
		if (answers)
			so_log_private("client: STUN: %d answers, the socket is seen at %s:%u (NAT %s)", answers, seen,
				(unsigned)stun.port[0], so_stun_nat_type(&stun));
		else
			so_log("client: STUN lookup failed, the service will use the address it observes");
	}
	so_mutex_lock(&c.lock);
	SO_TRACE(SO_THREAD_NET);
	if (c.cancel_requested || c.search_requested)
		return;	/* superseded while resolving */
	if (res == -100) {
		set_error("Online needs a wired LAN adapter (no Wi-Fi)");
		return;
	}
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
	if ((ws ? so_mm_start_ws(&c.config, &c.user, &c.search, server, c.port, &stun) :
			so_mm_start(&c.config, &c.user, &c.search, server, c.port)) < 0) {
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
			note_latest_version(so_mm_latest_version());
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
 * every LOG_PERIOD_MS (IOS sends are slow) and not in the boot's quiet time;
 * now_anyway (the kernel's exit: the last lines, a crash's report) both. */
static void forward_logs(int now_anyway)
{
	enum { DATAGRAM = 1200 };
	static char *datagram;	/* on the online heap: the kernel's data is full */
	char line[192];
	int len, n;
	u32 now = so_time_ms();
	if (!c.config.log_host ||
	    (!now_anyway && (now - c.log_sent_ms < LOG_PERIOD_MS || !so_client_traffic_allowed())))
		return;
	if (!datagram && !(datagram = so_malloc(DATAGRAM)))
		return;
	c.log_sent_ms = now;
	len = so_snprintf(datagram, DATAGRAM, "SLPWII LOGS\n");
	while ((n = so_log_take(line, sizeof(line))) > 0) {
		if (len + n + 1 >= DATAGRAM) {
			so_socket_send_aux(c.config.log_host, c.config.log_port, datagram, len);
			len = so_snprintf(datagram, DATAGRAM, "SLPWII LOGS\n");
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

/* The log on the SD card (so_logfile.c) and the problems it reports, looked at by the kernel's lowest
 * thread (so_diag.c, every 100 ms), never the network thread: a card's write cannot hold up an input.
 * Nothing is written in a match or while the disc is busy (logfile_quiet), but a crash's file. In a
 * match, by the game's polls (one a frame; Slippi's own waits keep polling):
 *  - none for FREEZE_MS: where the kernel stands is said at once (so_diag_where: the disc, the card's
 *    lock, the threads, the network), and when the game goes on, a freeze report;
 *  - none for HUNG_MS: a hang, its file at once (a hang sends no report: the game has stopped).
 * And at any time: the card's failed reads (ISO.c) or a disc command over half a second, a report. */
#define FREEZE_MS 1000
#define HUNG_MS 5000
/* (a freeze's start, given; or how long ago it was) */
static u32 frozen_ms(u32 since)
{
	static u32 start;
	if (since)
		start = since;
	return since ? 0 : so_time_ms() - start;
}

static int logfile_quiet(void)
{
	/* (not from the moment every player has picked: the versus screen and the game load next, and a
	 * write held them up, 3.5 frames of the PC's wait a game with the Diagnostic Log, 2026-11-18) */
	/* (nor while the prefetch reads the next game's files: a write held it up, and the game's load then
	 * read from the card what it would have found in the scratch: 13 frames of the PC's wait a game) */
	return !so_exi_in_match() && !so_exi_game_coming() && so_exi_poll_age_ms() >= 300 && so_disc_idle_ms() >= 300 &&
		so_pf_idle_ms() >= 300 && !so_fs_wanted();
}

void so_client_log_tick(void)
{
	static u32 said_ms, polls, skips, failed_seen, slow_seen, disc_said;
	static u8 frozen, hung, polled;	/* (polled: this match has had polls) */
	u32 age = so_exi_poll_age_ms(), now = so_time_ms(), failed, slow;
	char text[100];
	so_logfile_quiet = logfile_quiet;
	if (so_exi_in_match()) {
		/* (a match's scene is there before its first poll, and the last poll then is the game
		 * before's: only a match that has had polls can have stopped) */
		if (age < 300) {
			polled = 1;
			if (frozen && !hung) {
				so_snprintf(text, sizeof(text), "the game stopped for %u ms in a match, then went on", (unsigned)frozen_ms(0));
				so_logfile_event("freeze", text, 0);	/* (the report ends with the game) */
			}
			frozen = hung = 0;
		}
		if (polled && age >= FREEZE_MS && !frozen && so_exi_game_running()) {
			frozen = 1;
			frozen_ms(now - age);
			so_log("the game has stopped: no pad poll for %u ms in a match; where things stand:", (unsigned)age);
			so_diag_where();
		}
		if (polled && age >= HUNG_MS && !hung && so_exi_game_running()) {
			hung = 1;
			so_exi_report_last();	/* (its last polls, and the engine's last captures and loads) */
			so_snprintf(text, sizeof(text), "no pad poll for %u ms in a match: the game has hung", (unsigned)age);
			so_logfile_crash();
			so_logfile_event("hang", text, 0);
		}
		if (so_logfile_diag() && now - said_ms >= 5000) {
			SoExiStats st;
			so_exi_stats(&st);
			if (said_ms && now - said_ms < 6000)
				so_log("diag: a match's last 5 s: %u polls, %u of them held for the opponent's input, ping %u ms",
					(unsigned)(st.b0_polls - polls), (unsigned)(st.b0_skips - skips), (unsigned)(so_np_ping_us(0) / 1000));
			polls = st.b0_polls;
			skips = st.b0_skips;
			said_ms = now;
		}
	} else {
		said_ms = 0;
		polled = frozen = hung = 0;
	}
	/* (one report a boot, at the first; after it a line at most every 10 s: a dying card fails often) */
	so_disc_counters(&failed, &slow);
	if ((failed != failed_seen || slow != slow_seen) && (!disc_said || now - disc_said >= 10000)) {
		so_snprintf(text, sizeof(text), "%u reads of the card failed, %u disc commands took over half a second (this boot)",
			(unsigned)failed, (unsigned)slow);
		failed_seen = failed;
		slow_seen = slow;
		if (!disc_said)
			so_logfile_event("disc", text, !so_exi_in_match());
		else
			so_log("disc: %s", text);
		disc_said = now | 1;
	}
	so_logfile_service(logfile_quiet(), 0);
}

/* The kernel is closing down: no more SD card writes (so_client_stop), then
 * no more network (so_client_close). */
static volatile int stopping, stopped_io, closing, closed, flush_asked;

static u32 NetworkThread(void *unused)
{
	u64 now;
	(void)unused;
	for (;;) {
		if (stopping && !stopped_io) {
			so_logfile_service(1, 1);	/* (the card's last write: the game is over) */
			stopped_io = 1;
		}
		if (flush_asked) {
			forward_logs(1);
			if (!stopped_io)
				so_logfile_service(1, 1);
			flush_asked = 0;
		}
		if (closing) {
			so_mutex_lock(&c.lock);
			stop_all();	/* the opponent is told */
			so_mutex_unlock(&c.lock);
			forward_logs(1);
			so_socket_close();
			closed = 1;
			for (;;)
				so_event_wait(&c.wake, 0);
		}
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
		/* Tests: the PC's "SLPWII EXIT" comes to the session socket, so it
		 * is open from the boot's quiet time on, not only from a search. */
		if (c.config.test_control && !so_socket_is_open() && so_client_traffic_allowed())
			open_session_socket();
		SO_TRACE(SO_THREAD_NET);
		{
			u64 t0 = so_time_us(), t1, t2;
			send_queued_pads();	/* first: they are the game's inputs */
			t1 = so_time_us();
			step();
			t2 = so_time_us();
			update_status();
			note_hold(0, t1 - t0);
			note_hold(1, t2 - t1);
		}
		so_mutex_unlock(&c.lock);

		forward_logs(0);
		if (stopping)
			continue;
		now = so_time_us();
		if (now - c.sender_checked_us > 1000000) {
			c.sender_checked_us = now;
			so_socket_check_sender();
		}
		so_exi_save_history();	/* after a search added a code (an SD write; not in a game) */
		save_delay();
		save_rumble();
		save_ping_display();
		save_latest_version();
		so_exi_desync_service();	/* (its lines; the card's writes are so_client_log_tick's) */
		so_exi_start_watch();
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
	if (c.config.chat == 1)
		c.config.chat = (u8)so_quick_chat_setting();	/* (the loader's setting; config.ini's chat= stands if it is not 1) */
	so_logfile_init(so_diag_log_setting());	/* (lines before this are in no file: the heap's and the config's) */
	load_delay();
	load_rumble();
	load_ping_display();
	load_latest_version();
	so_step_pause_us = (u32)c.config.diag_step_pause_ms * 1000;
	so_diag_start(&c.config);
	if (so_config_is_real_mm(&c.config))
		so_log("client: matchmaking %s:%u, app version %s, delay %u, chat %u", c.config.mm_host,
			(unsigned)c.config.mm_port, c.config.app_version, (unsigned)c.config.delay, (unsigned)c.config.chat);
	else
		so_log("client: TEST matchmaking server %s:%u, chat %u", c.config.mm_host, (unsigned)c.config.mm_port,
			(unsigned)c.config.chat);
	on_wifi(0);	/* (for the log: a search asks again) */

	SO_PHASE("init threads");
	if (so_thread_start(NetworkThread, NULL, &__slippi_online_stack_addr, (u32)(size_t)&__slippi_online_stack_size,
			c.config.online_net_priority ? c.config.online_net_priority : NET_THREAD_PRIORITY) < 0)
		return -4;

	so_mutex_lock(&c.lock);
	set_state(res == 0 ? SO_CLIENT_IDLE : SO_CLIENT_OFF);
	so_mutex_unlock(&c.lock);
	SO_PHASE(res < 0 ? "no account" : "idle");
	if (res < 0)
		return -5;

	/* Slippi's codeset and menu files, for the game's online modes. */
	so_files_init();
	if (so_client_online_game()) {
		so_files_preload();
		if (!c.config.prefetch_off)
			so_pf_init();
	}
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
	int n;
	if (c.config.test_control)
		return;	/* tests: config.ini's, not what a player last chose on this card */
	n = so_read_file(SO_DELAY_PATH, text, sizeof(text) - 1);
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

/* The online CSS's On/Off switches, each saved in its own one-character file,
 * which wins over config.ini. */
static void load_switch(const char *path, u8 *value)
{
	char text[8];
	int n = so_read_file(path, text, sizeof(text) - 1);
	if (n > 0 && (text[0] == '0' || text[0] == '1'))
		*value = (u8)(text[0] - '0');
}

static void save_switch(const char *path, u8 value, const char *what)
{
	char text[3];
	text[0] = (char)('0' + value);
	text[1] = '\n';
	text[2] = 0;
	if (so_write_file(path, text, 2) < 0)
		so_log("client: could not save the %s setting", what);
}

#define SO_RUMBLE_PATH "/slippwii/rumble.txt"
static volatile int rumble_dirty;

int so_client_set_rumble(int on)
{
	if ((on == 0 || on == 1) && on != c.config.rumble) {
		c.config.rumble = (u8)on;
		rumble_dirty = 1;
		so_log("client: rumble %s (set on the Wii)", on ? "on" : "off");
	}
	return c.config.rumble;
}

static void load_rumble(void)
{
	load_switch(SO_RUMBLE_PATH, &c.config.rumble);
}

static void save_rumble(void)
{
	if (!rumble_dirty)
		return;
	rumble_dirty = 0;
	save_switch(SO_RUMBLE_PATH, c.config.rumble, "rumble");
}

#define SO_PING_PATH "/slippwii/ping.txt"
static volatile int ping_display_dirty;

int so_client_set_ping_display(int on)
{
	if ((on == 0 || on == 1) && on != c.config.ping_display) {
		c.config.ping_display = (u8)on;
		ping_display_dirty = 1;
		so_log("client: ping display %s (set on the Wii)", on ? "on" : "off");
	}
	return c.config.ping_display;
}

static void load_ping_display(void)
{
	load_switch(SO_PING_PATH, &c.config.ping_display);
}

static void save_ping_display(void)
{
	if (!ping_display_dirty)
		return;
	ping_display_dirty = 0;
	save_switch(SO_PING_PATH, c.config.ping_display, "ping display");
}

const SoUser *so_client_user(void)
{
	return &c.user;
}

/* ---- Slippi's version ----------------------------------------------------
 * Slippi's matchmaking turns away a version it no longer supports, naming
 * its latest. Slippi Dolphin keeps that and locks its online modes until it
 * is updated; so does this, in a file of its own (user.json holds the play
 * key, and is never written). user.json's own latestVersion is not gone by:
 * it is as old as the day the file was copied to the card. */

/* A test server's goes to a file of its own, and is read back only when a
 * test asks: a real console's never comes from a test. */
static const char *latest_version_path(void)
{
	return so_config_is_real_mm(&c.config) ? SO_LATEST_VERSION_PATH : "/slippwii/test-slippi-version.txt";
}

static void set_latest_version(const char *version)
{
	strlcpy(c.latest, version, sizeof(c.latest));
	c.outdated = so_version_cmp(c.latest, c.config.app_version) > 0;
	if (c.outdated)
		so_log("client: Slippi is at version %s and this SlippWii speaks %s: online is locked until SlippWii is updated",
			c.latest, c.config.app_version);
}

static void load_latest_version(void)
{
	char text[sizeof(c.latest)];
	int n, i;
	if (!so_config_is_real_mm(&c.config) && !c.config.test_latest_keep)
		return;
	n = so_read_file(latest_version_path(), text, sizeof(text) - 1);
	if (n <= 0)
		return;
	text[n] = 0;
	for (i = 0; i < n; i++)
		if (text[i] == '\r' || text[i] == '\n')
			text[i] = 0;
	set_latest_version(text);
}

static void note_latest_version(const char *version)
{
	if (!version[0] || so_version_cmp(version, c.latest) <= 0)
		return;
	set_latest_version(version);
	c.latest_dirty = 1;
}

static void save_latest_version(void)
{
	char text[sizeof(c.latest) + 2];
	if (!c.latest_dirty)
		return;
	c.latest_dirty = 0;
	if (so_write_file(latest_version_path(), text, (u32)so_snprintf(text, sizeof(text), "%s\n", c.latest)) < 0)
		so_log("client: could not save Slippi's version");
}

int so_client_outdated(void)
{
	return c.outdated;
}

/* ---- wired only ------------------------------------------------------------
 * 1 when the console's network is its Wi-Fi: the connection chosen in the
 * Wii's settings is a wireless one, and the link does not say wired either.
 * Where IOS gives only one of the two, that one decides; where it gives
 * neither, nothing is refused. A line in the log each time (at start-up,
 * which the card's log has, and at each search). */
static int on_wifi(int quiet)
{
	u32 link, flags;
	u8 raw[3];
	int got = so_net_link(&link, &flags, raw), wifi = so_net_is_wifi(got, link, flags);
	if (c.config.test_wifi)
		wifi = 1;
	if (!quiet) {
		so_log("client: network: %s (link %d, connections %02x %02x %02x%s)", wifi ? "Wi-Fi: online is refused" :
			((got & 2) && (flags & SO_CONNECTION_WIRED)) || ((got & 1) && link == SO_LINK_WIRED) ? "wired" :
			got ? "not Wi-Fi" : "unknown", (int)link, raw[0], raw[1], raw[2],
			c.config.test_wifi ? "; a test's Wi-Fi" : "");
	}
	return wifi;
}

void so_client_find_match(const SoSearch *search)
{
	so_mutex_lock(&c.lock);
	c.game_asked = 1;
	if (c.state != SO_CLIENT_OFF && search->mode == SO_MODE_RANKED) {
		/* SlippWii does not play Ranked (Slippi's protected mode; its set
		 * rules and reporting are not implemented). The menu hides it
		 * (tools/ppc/online_menu.s); this is the backstop. */
		so_log("client: Ranked is not available on SlippWii");
		c.cancel_requested = 1;
		c.search_requested = 0;
		set_error("Ranked is not available on SlippWii");
	} else if (c.state != SO_CLIENT_OFF && c.outdated) {
		/* (the menu locks the online modes, 0xB9: this is the backstop, and the
		 * search during which the server said so) */
		c.cancel_requested = 1;
		c.search_requested = 0;
		set_error("Update SlippWii to play online again");
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

void so_client_stop(void)
{
	int waited;
	stopping = 1;
	so_event_signal(&c.wake);
	for (waited = 0; !stopped_io && waited < 500; waited++)
		so_sleep_us(1000);
	so_log("client: stopped for the exit (%s)", stopped_io ? "no SD writes" : "the network thread did not answer");
}

/* The kernel's log to the PC now, even in the boot's quiet time: the exit's
 * lines (a crash's report among them) before the PPC reloads IOS. */
void so_client_flush_logs(void)
{
	int waited;
	flush_asked = 1;
	so_event_signal(&c.wake);
	for (waited = 0; flush_asked && waited < 300; waited++)
		so_sleep_us(1000);
}

/* The end of the kernel's exit: the session ends, the last logs go out and
 * the socket closes, so no receive is left pending in IOS's network stack
 * while the loader has IOS reloaded. */
void so_client_close(void)
{
	int waited;
	closing = 1;
	so_event_signal(&c.wake);
	for (waited = 0; !closed && waited < 500; waited++)
		so_sleep_us(1000);
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
