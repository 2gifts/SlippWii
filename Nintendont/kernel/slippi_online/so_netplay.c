/* slippi_online/so_netplay.c
 * Peer-to-peer netplay (see so_netplay.h). Function names in comments refer
 * to Slippi Dolphin's SlippiNetplayClient, which this follows step by step.
 */
#include "so_netplay.h"
#include "so_alloc.h"
#include "so_platform.h"
#include "so_util.h"
#include "string.h"
#include "enet/enet.h"

#define CONNECT_TIMEOUT_MS 8000
#define MAX_CONNECTIONS 16
#define PREP_QUEUE 8
#define CHAT_DISABLED 0x10	/* SlippiPremadeText::CHAT_MSG_CHAT_DISABLED */

enum {
	MSG_PAD = 0x80,
	MSG_PAD_ACK = 0x81,
	MSG_SELECTIONS = 0x82,
	MSG_CONNECTION_SELECTED = 0x83,
	MSG_CHAT = 0x84,
	MSG_GAME_PREP_STEP = 0x85,
	MSG_SYNCED_STATE = 0x86,
};

/* One ENet connection to an opponent (Dolphin's activeConnections). */
typedef struct {
	ENetPeer *peer;
	u32 host;
	u16 port;
	u8 player_idx;
	u8 disconnected;
} Connection;

typedef struct {
	SoNpStatus status;
	ENetHost *host;
	u8 remote_count;
	u8 local_idx;
	u8 decider;
	u8 log_packets;
	u8 drop_pct;
	u16 ack_delay_us;
	/* With ack_delay_us: the newest ack, held back until our next send
	 * (ENet sends whatever it has queued on every service call). */
	u8 ack_pending;
	s32 ack_frame;
	ENetPeer *ack_peer;
	u64 ack_due_us;
	u32 started_ms;

	ENetPeer *server[SO_REMOTE_MAX];	/* the connection used per opponent */
	ENetAddress remote_addr[SO_REMOTE_MAX];
	u8 connected[SO_REMOTE_MAX];
	Connection conns[MAX_CONNECTIONS];
	u8 player_active[SO_MAX_PLAYERS];
	u32 disconnect_reason;	/* ours, or the one an opponent sent */
	u32 pending_reason;	/* sent with our disconnects */

	/* game */
	int game_started;
	SoLocalPads local_pads;
	SoRemotePads remote_pads[SO_REMOTE_MAX];
	SoAckTimers ack_timers[SO_REMOTE_MAX];
	SoTimeSync time_sync[SO_REMOTE_MAX];
	s32 last_frame[SO_REMOTE_MAX];	/* lastFrameTiming */
	u64 last_frame_us[SO_REMOTE_MAX];
	s32 last_acked[SO_REMOTE_MAX];
	u32 ping_us[SO_REMOTE_MAX];
	u64 ping_sum_us;
	u32 ping_count;
	s32 checksum_frame[SO_REMOTE_MAX];
	u32 checksum[SO_REMOTE_MAX];

	/* menus */
	SoSelections local_sel;
	SoSelections remote_sel[SO_REMOTE_MAX];
	int chat_message;
	u8 chat_player;
	int connection_selected;
	SoGamePrepStep prep[PREP_QUEUE];
	u32 prep_head, prep_count;
	SoSyncedState synced[SO_REMOTE_MAX];

	SoNpStats stats;
	u8 packet[16 + SO_LOCAL_PAD_RING * SO_PAD_SIZE];
} Netplay;

static Netplay *np;

/* ---- helpers ------------------------------------------------------------- */

/* PlayerIdxFromPort: player index -> remote index. */
static u8 remote_from_player(u8 player)
{
	return player > np->local_idx ? (u8)(player - 1) : player;
}

u8 so_np_remote_player_idx(int remote)
{
	return (u8)(remote >= np->local_idx ? remote + 1 : remote);
}

static int same_address(const ENetPeer *a, u32 host, u16 port)
{
	return a && a->address.host == host && a->address.port == port;
}

static Connection *find_connection(const ENetPeer *peer)
{
	int i;
	for (i = 0; i < MAX_CONNECTIONS; i++) {
		Connection *c = &np->conns[i];
		if (c->peer == peer && c->host == peer->address.host && c->port == peer->address.port)
			return c;
	}
	return NULL;
}

static void add_connection(ENetPeer *peer, u8 player_idx)
{
	Connection *c = find_connection(peer);
	int i;
	if (!c) {
		/* A free slot, else one whose connection is over. */
		for (i = 0; i < MAX_CONNECTIONS && !c; i++)
			if (!np->conns[i].peer)
				c = &np->conns[i];
		for (i = 0; i < MAX_CONNECTIONS && !c; i++)
			if (np->conns[i].disconnected)
				c = &np->conns[i];
		if (!c) {
			so_log("np: too many connections; ignoring one");
			return;
		}
	}
	c->peer = peer;
	c->host = peer->address.host;
	c->port = peer->address.port;
	c->player_idx = player_idx;
	c->disconnected = 0;
	np->player_active[player_idx] = 1;
}

/* AreAllPeersDisconnectedForKey */
static int all_disconnected_for(u32 host, u16 port)
{
	int i;
	for (i = 0; i < MAX_CONNECTIONS; i++) {
		const Connection *c = &np->conns[i];
		if (c->peer && c->host == host && c->port == port && !c->disconnected)
			return 0;
	}
	return 1;
}

static int all_disconnected(void)
{
	int i;
	for (i = 0; i < MAX_CONNECTIONS; i++)
		if (np->conns[i].peer && !np->conns[i].disconnected)
			return 0;
	return 1;
}

static const char *ip_text(u32 host, char *buf)
{
	so_format_ipv4(host, buf, 16);
	return buf;
}

static void send_to_peer(ENetPeer *peer, const u8 *data, u32 len, u8 channel, u32 flags)
{
	ENetPacket *packet = enet_packet_create(data, len, flags);
	if (!packet)
		return;
	if (enet_peer_send(peer, channel, packet) < 0)
		enet_packet_destroy(packet);	/* not connected: ENet did not take it */
}

static void send_ack(ENetPeer *peer, s32 frame)
{
	u8 ack[6];
	ack[0] = MSG_PAD_ACK;
	so_put_u32(ack + 1, (u32)frame);
	ack[5] = np->local_idx;
	send_to_peer(peer, ack, sizeof(ack), 2, ENET_PACKET_FLAG_UNSEQUENCED);
	np->stats.acks_sent++;
}

/* Sends everything queued, a held-back ack included: queued together, ENet
 * puts them in one datagram per peer. */
static void flush(void)
{
	if (np->ack_pending) {
		np->ack_pending = 0;
		send_ack(np->ack_peer, np->ack_frame);
	}
	enet_host_flush(np->host);
}

/* Acks a pad packet. Acking only the newest frame is enough: the sender
 * keeps its highest acked frame, and its ping timer skips older entries. */
static void ack_pad(ENetPeer *peer, s32 frame, u64 now_us)
{
	if (!np->ack_delay_us) {
		send_ack(peer, frame);
		flush();
		return;
	}
	if (np->ack_pending && np->ack_peer != peer)
		flush();	/* another opponent's: send it first */
	if (!np->ack_pending)
		np->ack_due_us = now_us + np->ack_delay_us;
	np->ack_pending = 1;
	np->ack_peer = peer;
	np->ack_frame = frame;
}

/* Send: to the chosen connection of every opponent still connected. Pads
 * and acks go unsequenced on channel 1; everything else reliable on 0. */
static void send_all(const u8 *data, u32 len)
{
	int unreliable = data[0] == MSG_PAD || data[0] == MSG_PAD_ACK, i;
	for (i = 0; i < np->remote_count; i++) {
		Connection *c;
		if (!np->server[i])
			continue;
		c = find_connection(np->server[i]);
		if (c && c->disconnected)
			continue;
		send_to_peer(np->server[i], data, len, unreliable ? 1 : 0,
			unreliable ? ENET_PACKET_FLAG_UNSEQUENCED : ENET_PACKET_FLAG_RELIABLE);
	}
	flush();
}

/* Big-endian packet reader; `ok` turns 0 when the packet runs out. */
typedef struct {
	const u8 *p;
	u32 left;
	int ok;
} Reader;

static u8 rd_u8(Reader *r)
{
	if (r->left < 1) {
		r->ok = 0;
		return 0;
	}
	r->left--;
	return *r->p++;
}

static u16 rd_u16(Reader *r)
{
	u16 v;
	if (r->left < 2) {
		r->ok = 0;
		return 0;
	}
	v = so_get_u16(r->p);
	r->p += 2;
	r->left -= 2;
	return v;
}

static u32 rd_u32(Reader *r)
{
	u32 v;
	if (r->left < 4) {
		r->ok = 0;
		return 0;
	}
	v = so_get_u32(r->p);
	r->p += 4;
	r->left -= 4;
	return v;
}

/* ---- receiving (OnData) -------------------------------------------------- */

static void on_pad(Reader *r, ENetPeer *peer, u64 now_us)
{
	s32 frame = (s32)rd_u32(r), checksum_frame;
	u8 player = rd_u8(r), remote;
	u32 checksum;
	int i, live = 0, current_live = 0, conn_idx = 0, added;
	s64 offset;
	checksum_frame = (s32)rd_u32(r);
	checksum = rd_u32(r);
	remote = remote_from_player(player);
	if (!r->ok || remote >= np->remote_count) {
		np->stats.pad_packets_bad++;
		return;
	}
	np->stats.pad_packets_received++;

	/* Two connections to the same opponent: the lower player index keeps
	 * this one and closes the others. */
	for (i = 0; i < np->remote_count; i++)
		if (same_address(np->server[i], peer->address.host, peer->address.port)) {
			conn_idx = i;
			break;
		}
	for (i = 0; i < MAX_CONNECTIONS; i++) {
		Connection *c = &np->conns[i];
		if (!c->peer || c->disconnected || c->host != peer->address.host || c->port != peer->address.port)
			continue;
		live++;
		if (c->peer == peer)
			current_live = 1;
	}
	if (current_live && live > 1 && np->local_idx < player) {
		char ip[16];
		np->server[conn_idx] = peer;
		so_log("np: closing duplicate connections to %s:%u", ip_text(peer->address.host, ip),
			(unsigned)peer->address.port);
		for (i = 0; i < MAX_CONNECTIONS; i++) {
			Connection *c = &np->conns[i];
			if (!c->peer || c->disconnected || c->peer == peer || c->host != peer->address.host ||
			    c->port != peer->address.port)
				continue;
			enet_peer_disconnect(c->peer, 0);
			c->disconnected = 1;
		}
	}

	/* When their pad left them, compared with when ours for the same frame
	 * left us. Before our game starts, "now" stands for frame 0. */
	if (np->game_started)
		offset = so_time_sync_sample(now_us, np->ping_us[remote], frame, np->last_frame[remote],
			np->last_frame_us[remote]);
	else
		offset = so_time_sync_sample(now_us, np->ping_us[remote], frame, 0, now_us);
	so_time_sync_add(&np->time_sync[remote], (s32)offset);

	added = so_remote_pads_add(&np->remote_pads[remote], frame, r->p, r->left);
	if (added < 0) {
		np->stats.pad_packets_bad++;
		so_log("np: bad pad packet (frame %d, %u bytes of pads)", (int)frame, (unsigned)r->left);
		return;
	}
	np->checksum_frame[remote] = checksum_frame;
	np->checksum[remote] = checksum;
	if (np->log_packets)
		so_log("np: pad %d from player %u (+%d)", (int)frame, (unsigned)player, added);

	/* Ack only packets that brought new frames. */
	if (added > 0)
		ack_pad(peer, frame, now_us);
	else
		np->stats.pad_packets_stale++;
}

static void on_ack(Reader *r, u64 now_us)
{
	s32 frame = (s32)rd_u32(r);
	u8 remote = remote_from_player(rd_u8(r));
	s64 rtt;
	if (!r->ok || remote >= np->remote_count)
		return;
	np->stats.acks_received++;
	if (frame > np->last_acked[remote])
		np->last_acked[remote] = frame;
	rtt = so_ack_timers_ack(&np->ack_timers[remote], frame, now_us);
	if (rtt >= 0) {
		np->ping_us[remote] = (u32)rtt;
		np->ping_sum_us += (u64)rtt;
		np->ping_count++;
	}
}

/* SlippiPlayerSelections::Merge */
static void merge_selections(SoSelections *into, const SoSelections *s)
{
	into->rng_offset = s->rng_offset;
	if (s->is_stage_selected) {
		into->stage_id = s->stage_id;
		into->is_stage_selected = 1;
		into->alt_stage_mode = s->alt_stage_mode;
	}
	if (s->is_character_selected) {
		into->character_id = s->character_id;
		into->character_color = s->character_color;
		into->team_id = s->team_id;
		into->is_character_selected = 1;
	}
}

static void reset_selections(SoSelections *s)
{
	u8 player = s->player_idx, alt = s->alt_stage_mode;
	memset(s, 0, sizeof(*s));
	s->player_idx = player;
	s->alt_stage_mode = alt;	/* Reset() keeps it */
}

static void on_selections(Reader *r)
{
	SoSelections s;
	u8 remote;
	memset(&s, 0, sizeof(s));
	s.character_id = rd_u8(r);
	s.character_color = rd_u8(r);
	s.is_character_selected = rd_u8(r) != 0;
	s.player_idx = rd_u8(r);
	s.stage_id = rd_u16(r);
	s.is_stage_selected = rd_u8(r) != 0;
	s.rng_offset = rd_u32(r);
	s.team_id = rd_u8(r);
	s.alt_stage_mode = rd_u8(r);
	remote = remote_from_player(s.player_idx);
	if (!r->ok || remote >= np->remote_count) {
		so_log("np: bad selections packet");
		return;
	}
	so_log("np: selections from player %u: char %u/%u%s, stage %u%s", (unsigned)s.player_idx,
		(unsigned)s.character_id, (unsigned)s.character_color, s.is_character_selected ? " (picked)" : "",
		(unsigned)s.stage_id, s.is_stage_selected ? " (picked)" : "");
	merge_selections(&np->remote_sel[remote], &s);
	/* The next game cannot start before this message, so the per-game
	 * remote state restarts here. */
	np->game_started = 0;
	so_remote_pads_reset(&np->remote_pads[remote]);
}

static int chat_id_allowed(int id)
{
	static const u8 allowed[] = { 136, 129, 130, 132, 34, 40, 33, 36, 72, 66, 68, 65, 24, 18, 20, 17,
		CHAT_DISABLED };
	unsigned i;
	for (i = 0; i < sizeof(allowed); i++)
		if (allowed[i] == id)
			return 1;
	return 0;
}

static void on_chat(Reader *r)
{
	int id = (int)rd_u32(r);
	u8 player = rd_u8(r);
	if (!r->ok || !chat_id_allowed(id)) {
		so_log("np: bad chat message %d", id);
		return;
	}
	np->chat_message = id;
	np->chat_player = player;
}

static void on_game_prep_step(Reader *r)
{
	SoGamePrepStep s;
	s.step_idx = rd_u8(r);
	s.char_selection = rd_u8(r);
	s.char_color_selection = rd_u8(r);
	s.stage_selections[0] = rd_u8(r);
	s.stage_selections[1] = rd_u8(r);
	if (!r->ok)
		return;
	if (np->prep_count == PREP_QUEUE) {
		np->prep_head = (np->prep_head + 1) % PREP_QUEUE;
		np->prep_count--;
	}
	np->prep[(np->prep_head + np->prep_count) % PREP_QUEUE] = s;
	np->prep_count++;
}

static void on_synced_state(Reader *r)
{
	SoSyncedState s;
	u8 remote = remote_from_player(rd_u8(r));
	u32 id_len = rd_u32(r), i;
	memset(&s, 0, sizeof(s));
	if (!r->ok || remote >= np->remote_count || id_len > r->left)
		return;
	for (i = 0; i < id_len; i++) {
		char c = (char)rd_u8(r);
		if (i < sizeof(s.match_id) - 1)
			s.match_id[i] = c;
	}
	s.game_index = rd_u32(r);
	s.tiebreak_index = rd_u32(r);
	s.seconds_remaining = rd_u32(r);
	for (i = 0; i < 4; i++) {
		s.stocks_remaining[i] = rd_u8(r);
		s.current_health[i] = rd_u16(r);
	}
	if (r->ok)
		np->synced[remote] = s;
}

static void on_data(const u8 *data, u32 len, ENetPeer *peer, u64 now_us)
{
	Reader r;
	r.p = data;
	r.left = len;
	r.ok = 1;
	switch (rd_u8(&r)) {
	case MSG_PAD:
		on_pad(&r, peer, now_us);
		break;
	case MSG_PAD_ACK:
		on_ack(&r, now_us);
		break;
	case MSG_SELECTIONS:
		on_selections(&r);
		break;
	case MSG_CHAT:
		on_chat(&r);
		break;
	case MSG_CONNECTION_SELECTED:
		np->connection_selected = 1;
		break;
	case MSG_GAME_PREP_STEP:
		on_game_prep_step(&r);
		break;
	case MSG_SYNCED_STATE:
		on_synced_state(&r);
		break;
	default:
		if (r.ok)
			so_log("np: unknown message 0x%02x", data[0]);
		break;
	}
}

/* ---- connection events (ThreadFunc) -------------------------------------- */

/* The remote whose matchmaking address is the peer's; 0 if none matches. */
static int remote_for_address(const ENetPeer *peer, int use_server)
{
	int i;
	for (i = 0; i < np->remote_count; i++) {
		if (use_server ? same_address(np->server[i], peer->address.host, peer->address.port) :
				(np->remote_addr[i].host == peer->address.host &&
				 np->remote_addr[i].port == peer->address.port))
			return i;
	}
	return 0;
}

static void on_connect(ENetPeer *peer)
{
	char ip[16];
	int i, remote;
	if (np->status == SO_NP_CONNECTING) {
		remote = remote_for_address(peer, 0);
		add_connection(peer, so_np_remote_player_idx(remote));
		so_log("np: connection with %s:%u", ip_text(peer->address.host, ip), (unsigned)peer->address.port);
		/* Already connected to this address: use the newer connection. */
		for (i = 0; i < np->remote_count; i++) {
			if (np->connected[i] && same_address(np->server[i], peer->address.host, peer->address.port)) {
				np->server[i] = peer;
				return;
			}
		}
		/* Match by IP only: some routers change the port. */
		for (i = 0; i < np->remote_count; i++) {
			if (np->remote_addr[i].host == peer->address.host && !np->connected[i]) {
				np->server[i] = peer;
				np->connected[i] = 1;
				break;
			}
		}
	} else {
		remote = remote_for_address(peer, 1);
		add_connection(peer, so_np_remote_player_idx(remote));
		so_log("np: late connection with %s:%u", ip_text(peer->address.host, ip), (unsigned)peer->address.port);
	}
}

static void disconnect_all(void)
{
	int i;
	for (i = 0; i < MAX_CONNECTIONS; i++) {
		Connection *c = &np->conns[i];
		if (c->peer && !c->disconnected) {
			enet_peer_disconnect(c->peer, np->pending_reason);
			c->disconnected = 1;
		}
	}
	flush();
}

static void set_disconnected(void)
{
	if (np->status == SO_NP_DISCONNECTED)
		return;
	np->status = SO_NP_DISCONNECTED;
	disconnect_all();
}

static void on_disconnect(ENetPeer *peer, u32 data)
{
	Connection *c = find_connection(peer);
	char ip[16];
	int i, is_server = 0, all_for_key;
	u32 host = peer->address.host;
	u16 port = peer->address.port;
	so_log("np: %s:%u disconnected (reason %u)", ip_text(host, ip), (unsigned)port, (unsigned)data);
	if (np->status != SO_NP_CONNECTED)
		return;	/* during the connect phase only the timeout matters */
	if (c)
		c->disconnected = 1;
	all_for_key = all_disconnected_for(host, port);
	if (all_for_key && c)
		np->player_active[c->player_idx] = 0;
	/* Only a connection we actually play over counts; a failed duplicate
	 * from a port-changing router does not. */
	for (i = 0; i < np->remote_count; i++)
		if (same_address(np->server[i], host, port))
			is_server = 1;
	if (is_server && data)
		np->disconnect_reason = data;
	if (is_server && all_for_key && all_disconnected())
		set_disconnected();
}

/* ---- API ----------------------------------------------------------------- */

int so_np_init(void)
{
	if (!np) {
		np = (Netplay *)so_malloc(sizeof(Netplay));
		if (!np)
			return -1;
		memset(np, 0, sizeof(*np));
	}
	return 0;
}

int so_np_start(const SoMatchResult *match, u16 port, const SoConfig *config)
{
	ENetAddress bind_address;
	char ip[16];
	int i, remote = 0;
	if (!np && so_np_init() < 0)
		return -1;
	so_np_stop();
	memset(np, 0, sizeof(*np));
	np->local_idx = match->local_player_index;
	np->decider = match->is_host;
	np->log_packets = config->log_packets;
	np->drop_pct = config->test_drop_pct;
	np->ack_delay_us = config->ack_delay_us;
	np->local_sel.player_idx = np->local_idx;

	/* The same port matchmaking used: that is the one the NATs opened. */
	bind_address.host = ENET_HOST_ANY;
	bind_address.port = port;
	np->host = enet_host_create(&bind_address, 10, 3, 0, 0);
	if (!np->host) {
		so_log("np: could not create the ENet host");
		np->status = SO_NP_FAILED;
		return -2;
	}

	for (i = 0; i < match->player_count && remote < SO_REMOTE_MAX; i++) {
		const SoMmPlayer *p = &match->players[i];
		if (p->port - 1 == np->local_idx)
			continue;
		np->remote_addr[remote].host = p->addr;
		np->remote_addr[remote].port = p->udp_port;
		np->remote_sel[remote].player_idx = so_np_remote_player_idx(remote);
		np->server[remote] = enet_host_connect(np->host, &np->remote_addr[remote], 3, 0);
		if (np->server[remote])
			add_connection(np->server[remote], so_np_remote_player_idx(remote));
		so_log("np: connecting to %s (%s) at %s:%u", p->display_name, p->connect_code, ip_text(p->addr, ip),
			(unsigned)p->udp_port);
		remote++;
	}
	np->remote_count = (u8)remote;
	enet_host_flush(np->host);
	np->started_ms = so_time_ms();
	np->status = SO_NP_CONNECTING;
	return 0;
}

void so_np_poll(void)
{
	ENetEvent event;
	int i, all;
	if (!np || !np->host)
		return;
	while (enet_host_service(np->host, &event, 0) > 0) {
		switch (event.type) {
		case ENET_EVENT_TYPE_CONNECT:
			on_connect(event.peer);
			break;
		case ENET_EVENT_TYPE_RECEIVE:
			on_data(event.packet->data, (u32)event.packet->dataLength, event.peer, so_time_us());
			enet_packet_destroy(event.packet);
			break;
		case ENET_EVENT_TYPE_DISCONNECT:
			on_disconnect(event.peer, event.data);
			break;
		default:
			break;
		}
	}

	if (np->ack_pending && so_time_us() >= np->ack_due_us)
		flush();

	if (np->status == SO_NP_CONNECTING) {
		all = 1;
		for (i = 0; i < np->remote_count; i++)
			if (!np->connected[i])
				all = 0;
		if (all) {
			np->status = SO_NP_CONNECTED;
			SO_PHASE("np connected");
			so_log("np: connected to %d opponent%s", np->remote_count, np->remote_count == 1 ? "" : "s");
		} else if (so_time_ms() - np->started_ms >= CONNECT_TIMEOUT_MS) {
			np->status = SO_NP_FAILED;
			so_log("np: could not connect to every opponent");
		}
	}
}

void so_np_stop(void)
{
	int i;
	if (!np || !np->host)
		return;
	for (i = 0; i < MAX_CONNECTIONS; i++) {
		Connection *c = &np->conns[i];
		if (c->peer && !c->disconnected)
			enet_peer_disconnect_now(c->peer, np->pending_reason);
	}
	enet_host_destroy(np->host);
	np->host = NULL;
	np->ack_pending = 0;
	if (np->status == SO_NP_CONNECTING || np->status == SO_NP_CONNECTED)
		np->status = SO_NP_DISCONNECTED;
}

SoNpStatus so_np_status(void)
{
	return np ? np->status : SO_NP_IDLE;
}

u8 so_np_remote_count(void)
{
	return np ? np->remote_count : 0;
}

u8 so_np_local_player_idx(void)
{
	return np ? np->local_idx : 0;
}

u32 so_np_wait_hint_us(u32 idle_us)
{
	u64 now, left;
	if (!np || !np->ack_pending)
		return idle_us;
	now = so_time_us();
	if (np->ack_due_us <= now)
		return 0;
	left = np->ack_due_us - now;
	return left < idle_us ? (u32)left : idle_us;
}

int so_np_is_decider(void)
{
	return np && np->decider;
}

int so_np_player_active(u8 player_idx)
{
	return np && player_idx < SO_MAX_PLAYERS && np->player_active[player_idx];
}

u32 so_np_disconnect_reason(void)
{
	return np ? np->disconnect_reason : 0;
}

/* ---- game ---------------------------------------------------------------- */

void so_np_start_game(void)
{
	u64 now = so_time_us();
	int i;
	np->game_started = 0;
	so_local_pads_reset(&np->local_pads);
	for (i = 0; i < np->remote_count; i++) {
		np->last_frame[i] = 0;
		np->last_frame_us[i] = now;
		np->last_acked[i] = 0;
		so_ack_timers_reset(&np->ack_timers[i]);
		reset_selections(&np->remote_sel[i]);
	}
	np->prep_head = np->prep_count = 0;
	reset_selections(&np->local_sel);
}

void so_np_send_pad(const SoLocalPad *pad)
{
	s32 min_acked = 0x7FFFFFFF, frame;
	u64 now;
	int i, len, any = 0;
	if (np->status == SO_NP_FAILED || np->status == SO_NP_DISCONNECTED || !np->host)
		return;
	if (pad)
		so_local_pads_push(&np->local_pads, pad);
	else
		np->stats.resends++;

	/* Drop what every active opponent has acked. */
	for (i = 0; i < np->remote_count; i++) {
		if (!np->player_active[so_np_remote_player_idx(i)])
			continue;
		any = 1;
		if (np->last_acked[i] < min_acked)
			min_acked = np->last_acked[i];
	}
	so_local_pads_drop_acked(&np->local_pads, any ? min_acked : 0x7FFFFFFF);

	len = so_local_pads_encode(&np->local_pads, np->local_idx, np->packet, sizeof(np->packet));
	if (len <= 0)
		return;
	frame = so_local_pads_newest(&np->local_pads)->frame;

	if (np->drop_pct && so_random() % 100 < np->drop_pct)
		np->stats.test_dropped++;	/* tests: lose this packet on purpose */
	else
		send_all(np->packet, (u32)len);
	np->stats.pad_packets_sent++;

	now = so_time_us();
	np->game_started = 1;
	for (i = 0; i < np->remote_count; i++) {
		np->last_frame[i] = frame;
		np->last_frame_us[i] = now;
		so_ack_timers_push(&np->ack_timers[i], frame, now);
	}
}

int so_np_remote_pad(int remote, s32 frame, u8 *out)
{
	if (remote < 0 || remote >= np->remote_count)
		return -1;
	return so_remote_pads_get(&np->remote_pads[remote], frame, out);
}

s32 so_np_remote_head_frame(int remote)
{
	return remote >= 0 && remote < np->remote_count ? np->remote_pads[remote].head_frame : 0;
}

void so_np_remote_checksum(int remote, s32 *frame, u32 *checksum)
{
	*frame = np->checksum_frame[remote];
	*checksum = np->checksum[remote];
}

s32 so_np_time_offset_us(void)
{
	s32 best = 0;
	int i, have = 0, valid;
	for (i = 0; i < np->remote_count; i++) {
		s32 offset;
		if (!np->player_active[so_np_remote_player_idx(i)])
			continue;
		offset = so_time_sync_offset(&np->time_sync[i], &valid);
		if (!valid)
			continue;
		if (!have || offset < best)
			best = offset;
		have = 1;
	}
	return best;
}

u32 so_np_ping_us(int remote)
{
	return remote >= 0 && remote < np->remote_count ? np->ping_us[remote] : 0;
}

u32 so_np_take_avg_ping_us(void)
{
	u32 avg = np->ping_count ? (u32)(np->ping_sum_us / np->ping_count) : 0;
	np->ping_sum_us = 0;
	np->ping_count = 0;
	return avg;
}

/* ---- menus --------------------------------------------------------------- */

void so_np_set_selections(const SoSelections *s)
{
	SoSelections *l = &np->local_sel;
	u8 *p = np->packet;
	merge_selections(l, s);
	l->player_idx = np->local_idx;
	p[0] = MSG_SELECTIONS;
	p[1] = l->character_id;
	p[2] = l->character_color;
	p[3] = l->is_character_selected;
	p[4] = l->player_idx;
	so_put_u16(p + 5, l->stage_id);
	p[7] = l->is_stage_selected;
	so_put_u32(p + 8, l->rng_offset);
	p[12] = l->team_id;
	p[13] = l->alt_stage_mode;
	send_all(p, 14);
}

const SoSelections *so_np_local_selections(void)
{
	return &np->local_sel;
}

const SoSelections *so_np_remote_selections(int remote)
{
	return &np->remote_sel[remote];
}

void so_np_send_chat(int message_id)
{
	u8 p[6];
	p[0] = MSG_CHAT;
	so_put_u32(p + 1, (u32)message_id);
	p[5] = np->local_idx;
	send_all(p, sizeof(p));
}

int so_np_take_chat(u8 *player_idx)
{
	int id = np->chat_message;
	*player_idx = np->chat_player;
	np->chat_message = 0;
	np->chat_player = 0;
	return id;
}

void so_np_send_connection_selected(void)
{
	u8 p = MSG_CONNECTION_SELECTED;
	np->connection_selected = 1;
	send_all(&p, 1);
}

int so_np_connection_selected(void)
{
	return np->connection_selected;
}

void so_np_send_game_prep_step(const SoGamePrepStep *s)
{
	u8 p[6];
	p[0] = MSG_GAME_PREP_STEP;
	p[1] = s->step_idx;
	p[2] = s->char_selection;
	p[3] = s->char_color_selection;
	p[4] = s->stage_selections[0];
	p[5] = s->stage_selections[1];
	send_all(p, sizeof(p));
}

int so_np_game_prep_results(u8 step_idx, SoGamePrepStep *out)
{
	/* GetGamePrepResults: discard older steps until the wanted one. */
	while (np->prep_count > 0) {
		SoGamePrepStep *s = &np->prep[np->prep_head];
		if (s->step_idx == step_idx) {
			*out = *s;
			return 0;
		}
		np->prep_head = (np->prep_head + 1) % PREP_QUEUE;
		np->prep_count--;
	}
	return -1;
}

void so_np_send_synced_state(const SoSyncedState *s)
{
	u8 *p = np->packet;
	u32 id_len = strlen(s->match_id), n = 0, i;
	p[n++] = MSG_SYNCED_STATE;
	p[n++] = np->local_idx;
	so_put_u32(p + n, id_len);
	n += 4;
	memcpy(p + n, s->match_id, id_len);
	n += id_len;
	so_put_u32(p + n, s->game_index);
	so_put_u32(p + n + 4, s->tiebreak_index);
	so_put_u32(p + n + 8, s->seconds_remaining);
	n += 12;
	for (i = 0; i < 4; i++) {
		p[n++] = s->stocks_remaining[i];
		so_put_u16(p + n, s->current_health[i]);
		n += 2;
	}
	send_all(p, n);
}

const SoSyncedState *so_np_remote_synced_state(int remote)
{
	return &np->synced[remote];
}

void so_np_force_disconnect(u32 reason)
{
	int i;
	np->pending_reason = reason;
	np->disconnect_reason = reason;
	for (i = 0; i < np->remote_count; i++)
		np->player_active[so_np_remote_player_idx(i)] = 0;
	if (np->status == SO_NP_CONNECTED)
		set_disconnected();
}

void so_np_stats(SoNpStats *out)
{
	*out = np->stats;
}
