/* slippi_online/so_mm.c
 * Matchmaking client state machine (see so_mm.h). Timeouts follow
 * SlippiMatchmaking: 10 s to connect to the server, 5 s for the
 * create-ticket reply, none while queued, up to 3 s to leave the server.
 */
#define SO_FILE_ID "mm"
#include "so_mm.h"
#include "so_alloc.h"
#include "so_platform.h"
#include "so_socket.h"
#include "so_util.h"
#include "string.h"
#include "enet/enet.h"

#define CONNECT_TIMEOUT_MS 10000
#define TICKET_TIMEOUT_MS 5000
#define LEAVE_TIMEOUT_MS 3000

typedef struct {
	SoMmState state;
	ENetHost *host;
	ENetPeer *server;
	u16 port;
	u32 state_since_ms;
	char error[128];
	char latest_version[24];
	SoMatchResult result;
	/* Kept until the server connection is up. */
	char ticket[1024];
	int ticket_len;
} Mm;

/* On the online heap (the kernel's data region is nearly full), from the
 * first search on. */
static Mm *mmp;
#define mm (*mmp)

static void enter(SoMmState state)
{
	mm.state = state;
	mm.state_since_ms = so_time_ms();
}

static void destroy_host(void)
{
	if (mm.host) {
		enet_host_destroy(mm.host);	/* also closes the socket */
		mm.host = NULL;
	}
	mm.server = NULL;
	/* The ticket holds the play key. */
	memset(mm.ticket, 0, sizeof(mm.ticket));
	mm.ticket_len = 0;
}

static void fail(const char *text)
{
	strlcpy(mm.error, text, sizeof(mm.error));
	so_log("mm: %s", text);
	if (mm.server)
		enet_peer_disconnect_now(mm.server, 0);
	destroy_host();
	enter(SO_MM_ERROR);
}

static void send_json(const char *text, int len)
{
	ENetPacket *packet = enet_packet_create(text, len, ENET_PACKET_FLAG_RELIABLE);
	if (packet && enet_peer_send(mm.server, 0, packet) < 0)
		enet_packet_destroy(packet);
	enet_host_flush(mm.host);
}

int so_mm_start(const SoConfig *config, const SoUser *user, const SoSearch *search, u32 server, u16 port)
{
	ENetAddress bind_address, server_address;
	char lan[32], lan_ip[16];
	u32 local;

	if (!mmp) {
		mmp = (Mm *)so_malloc(sizeof(Mm));
		if (!mmp)
			return -1;
		memset(mmp, 0, sizeof(Mm));
	}
	SO_STEP("mm stop");
	so_mm_stop();
	memset(&mm.result, 0, sizeof(mm.result));
	mm.error[0] = '\0';
	mm.port = port;

	bind_address.host = ENET_HOST_ANY;
	bind_address.port = port;
	SO_TRACE(SO_THREAD_NET);
	SO_STEP("mm host create");
	mm.host = enet_host_create(&bind_address, 1, 3, 0, 0);
	SO_TRACE(SO_THREAD_NET);
	if (!mm.host) {
		fail("Failed to create mm client");
		return -1;
	}

	/* The LAN address lets an opponent behind the same external IP reach
	 * us directly. */
	SO_STEP("mm local ip");
	local = config->lan_ip ? config->lan_ip : so_local_ip();
	SO_TRACE(SO_THREAD_NET);
	so_format_ipv4(local, lan_ip, sizeof(lan_ip));
	so_snprintf(lan, sizeof(lan), "%s:%u", local ? lan_ip : "", (unsigned)port);
	SO_STEP("mm build ticket");
	mm.ticket_len = so_mm_build_ticket(mm.ticket, sizeof(mm.ticket), config, user, search, local ? lan : "");
	if (mm.ticket_len < 0) {
		fail("Failed to build mm ticket");
		return -2;
	}

	server_address.host = server;
	server_address.port = config->mm_port;
	SO_TRACE(SO_THREAD_NET);
	SO_STEP("mm connect");
	mm.server = enet_host_connect(mm.host, &server_address, 3, 0);
	if (!mm.server) {
		fail("Failed to start connection to mm server");
		return -3;
	}
	SO_TRACE(SO_THREAD_NET);
	SO_STEP("mm flush");
	enet_host_flush(mm.host);
	SO_TRACE(SO_THREAD_NET);
	SO_STEP("mm connecting");
	so_step_pause_us = 0;	/* diagnostics: hold the start-up steps only */
	so_log("mm: connecting from port %u (mode %d, LAN address %s)", (unsigned)port, search->mode,
		local ? lan : "unknown");
	enter(SO_MM_CONNECTING);
	return 0;
}

static void on_message(const char *text, u32 len)
{
	char error[128];
	if (mm.state == SO_MM_JOINING) {
		if (so_mm_parse_ticket_reply(text, len, error, sizeof(error)) < 0) {
			fail(error);
			return;
		}
		so_log("mm: in queue");
		SO_PHASE("mm in queue");
		enter(SO_MM_SEARCHING);
	} else if (mm.state == SO_MM_SEARCHING) {
		const SoMatchResult *r = &mm.result;
		const SoMmPlayer *local;
		if (so_mm_parse_match(text, len, &mm.result, error, sizeof(error), mm.latest_version,
				sizeof(mm.latest_version)) < 0) {
			if (mm.latest_version[0])
				so_log("mm: server's latest version is %s", mm.latest_version);
			fail(error);
			return;
		}
		local = &r->players[r->local_player_index];
		so_log("mm: matched (%s): %d players, we are port %d%s", r->match_id, r->player_count, local->port,
			r->is_host ? ", decider" : "");
		SO_PHASE("mm matched");
		/* Leave the server before netplay takes over the port. */
		enet_peer_disconnect(mm.server, 0);
		enet_host_flush(mm.host);
		enter(SO_MM_LEAVING);
	}
}

void so_mm_poll(void)
{
	if (!mmp)
		return;
	ENetEvent event;
	u32 waited;
	if (!mm.host)
		return;
	while (mm.host && enet_host_service(mm.host, &event, 0) > 0) {
		switch (event.type) {
		case ENET_EVENT_TYPE_CONNECT:
			if (mm.state == SO_MM_CONNECTING && event.peer == mm.server) {
				send_json(mm.ticket, mm.ticket_len);
				memset(mm.ticket, 0, sizeof(mm.ticket));
				so_log("mm: connected; create-ticket sent");
				enter(SO_MM_JOINING);
			}
			break;
		case ENET_EVENT_TYPE_RECEIVE:
			on_message((const char *)event.packet->data, (u32)event.packet->dataLength);
			enet_packet_destroy(event.packet);
			break;
		case ENET_EVENT_TYPE_DISCONNECT:
			if (mm.state == SO_MM_LEAVING) {
				mm.server = NULL;
				destroy_host();
				enter(SO_MM_MATCHED);
			} else if (mm.state == SO_MM_CONNECTING) {
				fail("Failed to connect to mm server");
			} else {
				fail("Lost connection to the mm server");
			}
			break;
		default:
			break;
		}
	}

	waited = so_time_ms() - mm.state_since_ms;
	switch (mm.state) {
	case SO_MM_CONNECTING:
		if (waited >= CONNECT_TIMEOUT_MS)
			fail("Failed to connect to mm server");
		break;
	case SO_MM_JOINING:
		if (waited >= TICKET_TIMEOUT_MS)
			fail("Failed to join mm queue");
		break;
	case SO_MM_LEAVING:
		if (waited >= LEAVE_TIMEOUT_MS) {
			/* The server did not confirm; drop the connection. */
			enet_peer_reset(mm.server);
			mm.server = NULL;
			destroy_host();
			enter(SO_MM_MATCHED);
		}
		break;
	default:
		break;
	}
}

void so_mm_stop(void)
{
	if (!mmp)
		return;
	if (mm.host) {
		if (mm.server)
			enet_peer_disconnect_now(mm.server, 0);
		destroy_host();
	}
	enter(SO_MM_IDLE);
}

SoMmState so_mm_state(void)
{
	return mmp ? mm.state : SO_MM_IDLE;
}

const char *so_mm_error(void)
{
	return mmp ? mm.error : "Failed to create mm client";
}

const SoMatchResult *so_mm_result(void)
{
	return mmp ? &mm.result : NULL;
}

u16 so_mm_port(void)
{
	return mmp ? mm.port : 0;
}
