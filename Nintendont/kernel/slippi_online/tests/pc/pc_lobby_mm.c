/* A matchmaking server for the modes with more than two players, for
 * tools/sweep/lobby.py (never talks to Slippi; the 3DS fork's fake server
 * pairs Direct only):
 *
 *   so_pc_lobby_mm PORT [--external-ip A.B.C.D] [--party-size N] [--items N]
 *                       [--stages 3,8,28,31,32] [--order CODE,CODE,...] [--seconds S]
 *                       [--ext CODE=A.B.C.D:PORT ...]
 *                       [--ws-port TCP_PORT] [--latest VERSION] [--ws-drop-after S]
 *
 * It speaks mm.slippi.gg's protocol as Slippi Dolphin uses it (ENet, 3
 * channels, one JSON text per reliable packet on channel 0) and answers with
 * what Dolphin's SlippiMatchmaking::handleMatchmaking reads:
 *   Direct (2): two tickets, each searching for the other's connect code;
 *   Teams (3):  four tickets with the same code;
 *   Party (4):  --party-size tickets (default 4), in the order they came;
 *   Unranked (1): the first two tickets.
 * The first ticket of a lobby is port 1 and the host (the "decider": Dolphin
 * takes the random seed from its first opponent when it is not the host
 * itself, which is the host only when the host is port 1). --order: ports go
 * by where each player's connect code is in this list instead.
 * --external-ip: every player is told this one external address, so each is
 * given the others' LAN addresses (players behind one router). --ext: that
 * player's external address instead (a player elsewhere: the others reach it
 * there, and it them at theirs; tools/sweep/lobby.py --net with a Wii).
 * Built on the client's own ENet and sockets.
 *
 * --ws-port: it also speaks the WebSocket service of Slippi Dolphin after
 * 3.6.4 (slippi-rust-extensions, matchmaking/src: join, joined, status,
 * match, error, ping, cancel, resume) on that TCP port, without TLS
 * (tools/sweep/tls_front.py in front gives the Wii its TLS). Players of both
 * services are matched with each other, so a console that queues the new way
 * can meet a Slippi Dolphin 3.6.4 that queues the old way. A WebSocket
 * player's address is where its connection comes from and the UDP port its
 * join names (the service never sees the UDP socket: that is what the
 * client's STUN lookup is for). --latest: a join whose appVersion is under
 * VERSION gets an error that names it, as Slippi's does. --ws-drop-after:
 * each WebSocket connection is cut once, S seconds after its join, so the
 * client has to take its ticket up again (resume).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "so_alloc.h"
#include "so_json.h"
#include "so_platform.h"
#include "so_socket.h"
#include "so_util.h"
#include "enet/enet.h"
#include "pc_ws_server.h"

char __slippi_online_stack_addr, __slippi_online_stack_size;
char __slippi_online_rx_stack_addr, __slippi_online_rx_stack_size;
char __slippi_online_done_stack_addr, __slippi_online_done_stack_size;
char __slippi_online_test_stack_addr, __slippi_online_test_stack_size;
char __slippi_online_diag_stack_addr, __slippi_online_diag_stack_size;

enum { MAX_TICKETS = 16, MAX_LOBBY = 4 };

typedef struct {
	ENetPeer *peer;	/* NULL: a WebSocket player */
	int ws;	/* its connection, or -1 while it is cut off (the ticket waits 30 s for a resume) */
	u32 joined_ms, status_ms, gone_ms;
	int dropped;	/* --ws-drop-after has been done to it */
	char ticket_id[24], token[40];
	int active, mode;
	char uid[64], name[64], code[32], lan[40], ext[40], version[48];
	char search[80];	/* the searched code's bytes, in hex */
} Ticket;

static Ticket tickets[MAX_TICKETS];
static ENetHost *host;
static const char *external_ip, *stages = "3,8,28,31,32", *order = "";
static int party_size = 4, matches;
static const char *ext_of[MAX_LOBBY];	/* --ext: "CODE=address" */
static int ext_n;
static long items = 0;
static const char *latest = "";
static int ws_drop_after;

static void say(const char *text)
{
	printf("%s\n", text);
	fflush(stdout);
}

static void send_text(ENetPeer *peer, const char *text)
{
	ENetPacket *packet = enet_packet_create(text, strlen(text), ENET_PACKET_FLAG_RELIABLE);
	if (packet && enet_peer_send(peer, 0, packet) < 0)
		enet_packet_destroy(packet);
	enet_host_flush(host);
}

static void write_player(SoJsonWriter *w, const Ticket *t, int port, int local)
{
	static const char *chat[16] = { "ggs", "one more", "brb", "good luck", "well played", "that was fun", "thanks",
		"too good", "sorry", "my b", "lol", "wow", "gotta go", "one sec", "let's play again later", "bad connection" };
	int i;
	so_jw_object_begin(w);
	so_jw_key(w, "chatMessages");
	so_jw_array_begin(w);
	for (i = 0; i < 16; i++)
		so_jw_string(w, chat[i]);
	so_jw_array_end(w);
	so_jw_key(w, "connectCode");
	so_jw_string(w, t->code);
	so_jw_key(w, "displayName");
	so_jw_string(w, t->name);
	so_jw_key(w, "ipAddress");
	so_jw_string(w, t->ext);
	so_jw_key(w, "ipAddressLan");
	so_jw_string(w, t->lan);
	so_jw_key(w, "isBot");
	so_jw_bool(w, 0);
	so_jw_key(w, "isLocalPlayer");
	so_jw_bool(w, local);
	so_jw_key(w, "port");
	so_jw_long(w, port);
	so_jw_key(w, "uid");
	so_jw_string(w, t->uid);
	so_jw_object_end(w);
}

static void send_match(Ticket **lobby, int n, int mode)
{
	static const char *names[] = { "ranked", "unranked", "direct", "teams", "party" };
	static char text[8192];
	char match_id[96], line[400];
	const char *s;
	int me, i;
	so_snprintf(match_id, sizeof(match_id), "mode.%s-fake-%u-%d", names[mode], (unsigned)so_time_ms(), ++matches);
	for (me = 0; me < n; me++) {
		SoJsonWriter w;
		so_jw_init(&w, text, sizeof(text));
		so_jw_object_begin(&w);
		so_jw_key(&w, "isHost");
		so_jw_bool(&w, me == 0);
		if (mode == 4 && items) {
			so_jw_key(&w, "items");
			so_jw_long(&w, items);
		}
		so_jw_key(&w, "matchId");
		so_jw_string(&w, match_id);
		so_jw_key(&w, "players");
		so_jw_array_begin(&w);
		for (i = 0; i < n; i++)
			write_player(&w, lobby[i], i + 1, i == me);
		so_jw_array_end(&w);
		so_jw_key(&w, "stages");
		so_jw_array_begin(&w);
		for (s = stages; *s; s++) {
			so_jw_long(&w, atoi(s));
			while (*s && *s != ',')
				s++;
			if (!*s)
				break;
		}
		so_jw_array_end(&w);
		so_jw_key(&w, "type");
		so_jw_string(&w, lobby[me]->peer ? "get-ticket-resp" : "match");
		so_jw_object_end(&w);
		if (so_jw_finish(&w) < 0) {
			say("mm: the reply does not fit");
			return;
		}
		if (lobby[me]->peer) {
			send_text(lobby[me]->peer, text);
		} else {
			ws_server_send(lobby[me]->ws, text);
			ws_server_close(lobby[me]->ws);
		}
	}
	so_snprintf(line, sizeof(line), "mm: MATCHED %s, %d players:", match_id, n);
	for (i = 0; i < n; i++)
		so_snprintf(line + strlen(line), sizeof(line) - strlen(line), " %s (port %d, %s / %s)%s", lobby[i]->code,
			i + 1, lobby[i]->ext, lobby[i]->lan, i ? "" : " host");
	say(line);
	for (i = 0; i < n; i++)
		lobby[i]->active = 0;
}

/* A player's place in --order (unlisted: after the listed, as they came). */
static int seat(const Ticket *t)
{
	const char *at = strstr(order, t->code);
	return at ? (int)(at - order) : 1000 + (int)(t - tickets);
}

/* The lobby the newest ticket completes, if any. */
static void try_match(Ticket *t)
{
	Ticket *lobby[MAX_LOBBY];
	int want = t->mode == 3 || t->mode == 4 ? party_size : 2, n = 0, i;	/* (Teams: of 3 too, as Slippi's) */
	for (i = 0; i < MAX_TICKETS && n < want; i++) {
		Ticket *o = &tickets[i];
		if (!o->active || o->mode != t->mode || (!o->peer && o->ws < 0))
			continue;
		if (t->mode == 3 && strcmp(o->search, t->search) != 0)
			continue;
		lobby[n++] = o;
	}
	if (t->mode == 2) {
		/* Direct: by each other's codes (the searched code is full-width
		 * Shift-JIS: 0x82 then 0x1F + the ASCII letter or digit, '#' 0x81 0x94) */
		n = 0;
		for (i = 0; i < MAX_TICKETS; i++) {
			Ticket *o = &tickets[i];
			if (o->active && o->mode == 2 && o != t && (o->peer || o->ws >= 0) && !strcmp(o->search, t->search + 40) &&
			    !strcmp(t->search, o->search + 40)) {
				lobby[0] = o;
				lobby[1] = t;
				n = 2;
				break;
			}
		}
	}
	if (n == want) {
		int j;
		for (i = 1; i < n; i++)
			for (j = i; j > 0 && seat(lobby[j]) < seat(lobby[j - 1]); j--) {
				Ticket *swap = lobby[j];
				lobby[j] = lobby[j - 1];
				lobby[j - 1] = swap;
			}
		send_match(lobby, n, t->mode);
	}
}

/* A connect code as the game's name entry writes it, in hex (what a Direct search holds). */
static void code_hex(const char *code, char *out, int size)
{
	int n = 0;
	for (; *code && n + 4 < size; code++) {
		u8 hi = 0x82, lo = (u8)*code;
		if (*code == '#') {
			hi = 0x81;
			lo = 0x94;
		} else if (*code >= '0' && *code <= '9') {
			lo = (u8)(0x4F + (*code - '0'));
		} else if (*code >= 'A' && *code <= 'Z') {
			lo = (u8)(0x60 + (*code - 'A'));
		}
		n += so_snprintf(out + n, size - n, "%02x%02x", hi, lo);
	}
	out[n] = '\0';
}

/* An error to a player of either service. */
static void refuse(ENetPeer *peer, int ws, const char *error, const char *latest_version)
{
	char text[300];
	so_snprintf(text, sizeof(text), "{\"error\":\"%s\"%s%s%s,\"type\":\"%s\"}", error,
		latest_version[0] ? ",\"latestVersion\":\"" : "", latest_version, latest_version[0] ? "\"" : "",
		peer ? "create-ticket-resp" : "error");
	if (peer) {
		send_text(peer, text);
	} else {
		ws_server_send(ws, text);
		ws_server_close(ws);
	}
}

/* A create-ticket (ENet, `peer`) or a join (WebSocket connection `ws`). */
static void on_ticket(ENetPeer *peer, int ws, const char *data, u32 len)
{
	SoJson doc, user, search, code, el;
	Ticket t, *slot = NULL;
	char ip[20], play_key[128], line[512];
	int i, n;
	memset(&t, 0, sizeof(t));
	if (so_json_parse(&doc, data, len) < 0 || so_json_get(&doc, "user", &user) < 0 || so_json_get(&doc, "search", &search) < 0) {
		refuse(peer, ws, "Malformed create-ticket (fake server)", "");
		say("mm: a ticket that does not parse");
		return;
	}
	t.peer = peer;
	t.ws = ws;
	t.joined_ms = t.status_ms = so_time_ms();
	t.active = 1;
	so_json_get_string(&user, "uid", t.uid, sizeof(t.uid), "");
	so_json_get_string(&user, "displayName", t.name, sizeof(t.name), "");
	so_json_get_string(&user, "connectCode", t.code, sizeof(t.code), "");
	so_json_get_string(&user, "playKey", play_key, sizeof(play_key), "");
	so_json_get_string(&doc, "ipAddressLan", t.lan, sizeof(t.lan), "");
	so_json_get_string(&doc, "appVersion", t.version, sizeof(t.version), "");
	t.mode = (int)so_json_get_long(&search, "mode", -1);
	n = 0;
	if (so_json_get(&search, "connectCode", &code) == 0)
		for (i = 0; i < so_json_count(&code) && n + 2 < 40; i++) {
			long b = 0;
			so_json_at(&code, i, &el);
			so_json_long(&el, &b);
			if (!b)
				break;	/* (Dolphin sends the 18 bytes it was given: the code, then zeroes) */
			n += so_snprintf(t.search + n, 40 - n, "%02x", (unsigned)b & 0xFF);
		}
	code_hex(t.code, t.search + 40, 40);
	if (peer) {
		so_format_ipv4(peer->address.host, ip, sizeof(ip));
		so_snprintf(t.ext, sizeof(t.ext), "%s:%u", external_ip ? external_ip : ip, (unsigned)peer->address.port);
	} else {
		/* (the join's port: what a STUN server saw of the UDP socket, or the socket's own) */
		so_snprintf(ip, sizeof(ip), "%s", ws_server_peer(ws));
		so_snprintf(t.ext, sizeof(t.ext), "%s:%ld", external_ip ? external_ip : ip, so_json_get_long(&doc, "port", 0));
	}
	for (i = 0; i < ext_n; i++) {
		size_t code_len = strlen(t.code);
		if (!strncmp(ext_of[i], t.code, code_len) && ext_of[i][code_len] == '=')
			so_snprintf(t.ext, sizeof(t.ext), "%s", ext_of[i] + code_len + 1);
	}
	/* (the play key is in the ticket: only its length is printed) */
	if (peer) {
		so_snprintf(line, sizeof(line), "mm: <- create-ticket: %s (%s), mode %d, searched code [%s], appVersion %s, LAN %s, "
			"seen at %s:%u, play key of %u characters", t.code, t.name, t.mode, t.search, t.version, t.lan, ip,
			(unsigned)peer->address.port, (unsigned)strlen(play_key));
	} else {
		char nat[16], candidates[200] = "";
		SoJson list;
		so_json_get_string(&doc, "natType", nat, sizeof(nat), "(none)");
		if (so_json_get(&doc, "candidates", &list) == 0)
			for (i = 0; i < so_json_count(&list); i++) {
				char kind[12], cip[20];
				so_json_at(&list, i, &el);
				so_json_get_string(&el, "type", kind, sizeof(kind), "?");
				so_json_get_string(&el, "ip", cip, sizeof(cip), "?");
				so_snprintf(candidates + strlen(candidates), sizeof(candidates) - strlen(candidates), " %s %s:%ld", kind,
					cip, so_json_get_long(&el, "port", 0));
			}
		so_snprintf(line, sizeof(line), "mm: <- join (WebSocket): %s (%s), mode %d, searched code [%s], appVersion %s, LAN %s, "
			"connection from %s, port %ld, NAT %s, candidates:%s, play key of %u characters", t.code, t.name, t.mode,
			t.search, t.version, t.lan, ip, so_json_get_long(&doc, "port", 0), nat, candidates,
			(unsigned)strlen(play_key));
	}
	say(line);
	if (!t.uid[0] || !play_key[0] || !t.code[0] || t.mode < 1 || t.mode > 4) {
		refuse(peer, ws, "Malformed create-ticket (fake server)", "");
		return;
	}
	if (latest[0] && so_version_cmp(t.version, latest) < 0) {
		so_snprintf(line, sizeof(line), "mm: appVersion %s is under %s: turned away", t.version, latest);
		say(line);
		refuse(peer, ws, "Your version of Slippi is out of date (fake server)", latest);
		return;
	}
	if (peer) {
		send_text(peer, "{\"type\":\"create-ticket-resp\"}");
	} else {
		so_snprintf(t.ticket_id, sizeof(t.ticket_id), "ticket-%u", (unsigned)so_random());
		so_snprintf(t.token, sizeof(t.token), "token-%08x%08x", (unsigned)so_random(), (unsigned)so_random());
		so_snprintf(line, sizeof(line), "{\"type\":\"joined\",\"ticketId\":\"%s\",\"token\":\"%s\"}", t.ticket_id, t.token);
		ws_server_send(ws, line);
	}
	for (i = 0; i < MAX_TICKETS; i++)
		if (tickets[i].active && ((peer && tickets[i].peer == peer) || !strcmp(tickets[i].uid, t.uid)))
			tickets[i].active = 0;	/* a new ticket replaces the player's old one */
	for (i = 0; i < MAX_TICKETS && !slot; i++)
		if (!tickets[i].active)
			slot = &tickets[i];
	if (!slot) {
		say("mm: no room for another ticket");
		return;
	}
	*slot = t;
	try_match(slot);
}

/* ---- the WebSocket service ---------------------------------------------- */

static Ticket *ticket_of(int ws)
{
	int i;
	for (i = 0; i < MAX_TICKETS; i++)
		if (tickets[i].active && !tickets[i].peer && tickets[i].ws == ws)
			return &tickets[i];
	return NULL;
}

static void ws_text(int ws, const char *text, u32 len)
{
	SoJson doc;
	char type[16], line[200];
	Ticket *t = ticket_of(ws);
	int i;
	if (so_json_parse(&doc, text, len) < 0) {
		say("mm: a WebSocket message that does not parse");
		return;
	}
	so_json_get_string(&doc, "type", type, sizeof(type), "");
	if (!strcmp(type, "join")) {
		on_ticket(NULL, ws, text, len);
	} else if (!strcmp(type, "ping")) {
		ws_server_send(ws, "{\"type\":\"pong\"}");
		say("mm: <- ping");
	} else if (!strcmp(type, "cancel")) {
		so_snprintf(line, sizeof(line), "mm: <- cancel%s%s", t ? " from " : "", t ? t->code : "");
		say(line);
		if (t)
			t->active = 0;
	} else if (!strcmp(type, "resume")) {
		char id[24], token[40];
		so_json_get_string(&doc, "ticketId", id, sizeof(id), "");
		so_json_get_string(&doc, "token", token, sizeof(token), "");
		for (i = 0; i < MAX_TICKETS; i++) {
			Ticket *o = &tickets[i];
			if (o->active && !o->peer && o->ws < 0 && !strcmp(o->ticket_id, id) && !strcmp(o->token, token)) {
				o->ws = ws;
				o->status_ms = so_time_ms();
				so_snprintf(line, sizeof(line), "{\"type\":\"joined\",\"ticketId\":\"%s\",\"token\":\"%s\"}", o->ticket_id,
					o->token);
				ws_server_send(ws, line);
				so_snprintf(line, sizeof(line), "mm: <- resume: %s is back after %u ms", o->code,
					(unsigned)(so_time_ms() - o->gone_ms));
				say(line);
				try_match(o);
				return;
			}
		}
		say("mm: <- resume of a ticket that is not waiting");
		refuse(NULL, ws, "Unknown ticket (fake server)", "");
	} else {
		so_snprintf(line, sizeof(line), "mm: <- a WebSocket message of type '%s'", type);
		say(line);
	}
}

static void ws_closed(int ws)
{
	Ticket *t = ticket_of(ws);
	char line[120];
	if (!t)
		return;
	t->ws = -1;
	t->gone_ms = so_time_ms();
	so_snprintf(line, sizeof(line), "mm: %s's connection is gone; its ticket waits for a resume", t->code);
	say(line);
}

/* The queue's status to each waiting WebSocket player every 3 s; tickets cut off for 30 s are dropped. */
static void ws_tick(void)
{
	u32 now = so_time_ms();
	char line[120];
	int i;
	for (i = 0; i < MAX_TICKETS; i++) {
		Ticket *t = &tickets[i];
		if (!t->active || t->peer)
			continue;
		if (t->ws < 0) {
			if (now - t->gone_ms >= 30000) {
				t->active = 0;
				so_snprintf(line, sizeof(line), "mm: %s's ticket is dropped: silent for 30 s", t->code);
				say(line);
			}
			continue;
		}
		if (ws_drop_after && !t->dropped && now - t->joined_ms >= (u32)ws_drop_after * 1000) {
			int ws = t->ws;
			t->dropped = 1;
			so_snprintf(line, sizeof(line), "mm: cutting %s's connection (--ws-drop-after)", t->code);
			say(line);
			ws_server_drop(ws);
			ws_closed(ws);
			continue;
		}
		if (now - t->status_ms >= 3000) {
			t->status_ms = now;
			so_snprintf(line, sizeof(line), "{\"type\":\"status\",\"secondsInQueue\":%u.%u}",
				(unsigned)((now - t->joined_ms) / 1000), (unsigned)((now - t->joined_ms) % 1000 / 100));
			ws_server_send(t->ws, line);
		}
	}
}

int main(int argc, char **argv)
{
	ENetAddress address;
	ENetEvent event;
	int ws_port = 0;
	void *arena;
	u32 arena_size, end_ms, seconds = 3600;
	char line[160];
	int i;
	if (argc < 2) {
		fprintf(stderr, "usage: so_pc_lobby_mm PORT [--external-ip A.B.C.D] [--party-size N] [--items N] "
			"[--stages LIST] [--order CODES] [--seconds S]\n");
		return 2;
	}
	for (i = 2; i + 1 < argc; i += 2) {
		if (!strcmp(argv[i], "--external-ip"))
			external_ip = argv[i + 1];
		else if (!strcmp(argv[i], "--party-size"))
			party_size = atoi(argv[i + 1]);
		else if (!strcmp(argv[i], "--items"))
			items = strtol(argv[i + 1], NULL, 0);
		else if (!strcmp(argv[i], "--stages"))
			stages = argv[i + 1];
		else if (!strcmp(argv[i], "--order"))
			order = argv[i + 1];
		else if (!strcmp(argv[i], "--ext") && ext_n < MAX_LOBBY)
			ext_of[ext_n++] = argv[i + 1];
		else if (!strcmp(argv[i], "--seconds"))
			seconds = (u32)atoi(argv[i + 1]);
		else if (!strcmp(argv[i], "--ws-port"))
			ws_port = atoi(argv[i + 1]);
		else if (!strcmp(argv[i], "--latest"))
			latest = argv[i + 1];
		else if (!strcmp(argv[i], "--ws-drop-after"))
			ws_drop_after = atoi(argv[i + 1]);
		else {
			fprintf(stderr, "unknown option %s\n", argv[i]);
			return 2;
		}
	}
	if (party_size < 2 || party_size > MAX_LOBBY) {
		fprintf(stderr, "--party-size: 2 to %d\n", MAX_LOBBY);
		return 2;
	}
	if (so_platform_init() < 0)
		return 1;
	arena = so_platform_arena(&arena_size);
	if (so_alloc_init(arena, arena_size) < 0 || so_socket_init() < 0 || enet_ios_initialize() < 0)
		return 1;
	address.host = ENET_HOST_ANY;
	address.port = (u16)atoi(argv[1]);
	host = enet_host_create(&address, MAX_TICKETS, 3, 0, 0);
	if (!host) {
		fprintf(stderr, "could not open UDP port %s\n", argv[1]);
		return 1;
	}
	so_snprintf(line, sizeof(line), "fake matchmaking server for lobbies on UDP %s (Party: %d players, items %ld; stages %s)",
		argv[1], party_size, items, stages);
	say(line);
	if (ws_port) {
		if (ws_server_open((u16)ws_port) < 0) {
			fprintf(stderr, "could not open TCP port %d\n", ws_port);
			return 1;
		}
		so_snprintf(line, sizeof(line), "and Slippi's WebSocket service (no TLS) on TCP %d", ws_port);
		say(line);
	}
	end_ms = so_time_ms() + seconds * 1000;
	while ((s32)(end_ms - so_time_ms()) > 0) {
		if (ws_port) {
			ws_server_service(NULL, ws_text, ws_closed);
			ws_tick();
		}
		if (enet_host_service(host, &event, 0) <= 0) {
			so_sleep_us(2000);
			continue;
		}
		if (event.type == ENET_EVENT_TYPE_RECEIVE) {
			on_ticket(event.peer, -1, (const char *)event.packet->data, (u32)event.packet->dataLength);
			enet_packet_destroy(event.packet);
		} else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
			for (i = 0; i < MAX_TICKETS; i++)
				if (tickets[i].peer && tickets[i].peer == event.peer)
					tickets[i].active = 0;
		}
	}
	enet_host_destroy(host);
	return 0;
}
