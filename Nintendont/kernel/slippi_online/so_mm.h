/* slippi_online/so_mm.h
 * Matchmaking client: Slippi Dolphin's SlippiMatchmaking (Ishiiruka v3.6.4)
 * as a non-blocking state machine.
 *
 * Protocol: ENet to the matchmaking server (mm.slippi.gg:43113), one JSON
 * text per reliable packet on channel 0. We send create-ticket; the server
 * answers create-ticket-resp, then get-ticket-resp once it has paired us.
 * The ENet host binds the port that P2P netplay will use afterwards, so the
 * server sees (and hole-punches) the right external port.
 *
 * Driven from the network thread: so_mm_poll() never blocks. Not
 * thread-safe; the client serialises access.
 */
#ifndef SO_MM_H
#define SO_MM_H

#include "global.h"
#include "so_config.h"

/* Dolphin's OnlinePlayMode. */
enum {
	SO_MODE_RANKED = 0,
	SO_MODE_UNRANKED = 1,
	SO_MODE_DIRECT = 2,
	SO_MODE_TEAMS = 3,
};

#define SO_MAX_PLAYERS 4
#define SO_CONNECT_CODE_MAX 24	/* game bytes: 8 full-width Shift-JIS chars + slack */
#define SO_CHAT_MESSAGES 16
#define SO_CHAT_TEXT_MAX 32
#define SO_MAX_STAGES 32

typedef struct {
	u8 mode;
	/* The connect code exactly as the game hands it over (full-width
	 * Shift-JIS from the name entry screen); sent as a byte array. */
	u8 connect_code[SO_CONNECT_CODE_MAX];
	u8 connect_code_len;
} SoSearch;

typedef struct {
	char uid[64];
	char display_name[64];
	char connect_code[16];
	u8 port;	/* 1-4 */
	u8 is_local;
	u8 is_bot;
	u8 has_chat_messages;	/* 0: use the defaults */
	char chat_messages[SO_CHAT_MESSAGES][SO_CHAT_TEXT_MAX];
	u32 addr;	/* remote players: where to connect (LAN or external) */
	u16 udp_port;
} SoMmPlayer;

typedef struct {
	char match_id[96];
	u8 player_count;
	SoMmPlayer players[SO_MAX_PLAYERS];	/* in the server's order (by port) */
	u8 local_player_index;	/* port - 1 */
	u8 is_host;	/* the "decider" */
	u8 stage_count;
	u16 stages[SO_MAX_STAGES];
	u32 items;
} SoMatchResult;

typedef enum {
	SO_MM_IDLE,
	SO_MM_CONNECTING,	/* ENet connect to the server */
	SO_MM_JOINING,	/* create-ticket sent */
	SO_MM_SEARCHING,	/* in the queue */
	SO_MM_LEAVING,	/* matched; disconnecting from the server */
	SO_MM_MATCHED,	/* result ready; the socket is free for netplay */
	SO_MM_ERROR,
} SoMmState;

/* Starts a search. `server` is the resolved matchmaking address; `port`
 * the local UDP port (netplay reuses it). Returns 0, or < 0 (with an error
 * message) when the ENet host cannot be created. */
int so_mm_start(const SoConfig *config, const SoUser *user, const SoSearch *search, u32 server, u16 port);
void so_mm_poll(void);
/* Leaves the queue and frees the socket. */
void so_mm_stop(void);

SoMmState so_mm_state(void);
/* The error text shown to the player (the server's own text when it sent
 * one), valid in SO_MM_ERROR. */
const char *so_mm_error(void);
/* NULL before the first search. */
const SoMatchResult *so_mm_result(void);
u16 so_mm_port(void);

/* ---- messages (so_mm_msg.c; no networking, unit-tested) ------------------ */

/* Writes the create-ticket JSON. `lan_address` is "a.b.c.d:port". Returns
 * the length, or -1 when it does not fit. */
int so_mm_build_ticket(char *out, int size, const SoConfig *config, const SoUser *user,
	const SoSearch *search, const char *lan_address);

/* Checks a create-ticket-resp. Returns 0 when the ticket was accepted, or
 * < 0 with `error` set to the text for the player. */
int so_mm_parse_ticket_reply(const char *text, u32 len, char *error, int error_size);

/* Parses a get-ticket-resp into `result`. Returns 0, or < 0 with `error`
 * set (the server's text when it sent one; `latest_version` then receives
 * its latestVersion, if any). */
int so_mm_parse_match(const char *text, u32 len, SoMatchResult *result, char *error, int error_size,
	char *latest_version, int latest_version_size);

#endif
