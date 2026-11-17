/* slippi_online/so_stun.h
 * Just enough of STUN (RFC 5389) to learn the public address of the netplay
 * socket, as Slippi Dolphin does before it joins the matchmaking queue
 * (SlippiMatchmaking.cpp, startMatchmaking; slippi-rust-extensions,
 * matchmaking/src/stun.rs). The matchmaking service is reached over TCP and
 * never sees the UDP socket, so the client tells it where opponents can
 * reach that socket; the requests also open the router's mapping, and one
 * every 15 s keeps it open while the search runs.
 */
#ifndef SO_STUN_H
#define SO_STUN_H

#include "global.h"

#define SO_STUN_REQUEST 20
#define SO_STUN_SERVERS 4
#define SO_STUN_KEEPALIVE_MS 15000

/* What two servers saw of the socket. */
typedef struct {
	u8 have[2];
	u32 addr[2];
	u16 port[2];
	/* the first server that answered: the keepalives' target */
	u32 server_addr;
	u16 server_port;
} SoStun;

/* A binding request with the transaction id `id` (12 bytes). */
void so_stun_build_request(u8 out[SO_STUN_REQUEST], const u8 id[12]);
/* The address in a success response to the request with `id`: 0, or < 0
 * when the bytes are not one or carry no IPv4 address. */
int so_stun_parse_response(const u8 id[12], const u8 *data, u32 len, u32 *addr, u16 *port);
/* "cone" (both servers saw one address: opponents can reach it too),
 * "symmetric" (each destination gets its own port) or "unknown". */
const char *so_stun_nat_type(const SoStun *stun);

/* Asks the servers of `hosts` ("name:port,name:port,..." , up to
 * SO_STUN_SERVERS) in turn through the open session socket until two have
 * answered: each up to 3 requests, 500 ms apart. Blocks (DNS, then up to
 * 1.5 s a server): network thread only, the client's lock not held. Stops
 * early when `*cancel` becomes non-zero. Failing is not fatal: the service
 * then uses the address it sees. Returns the number of answers (0-2). */
int so_stun_discover(const char *hosts, SoStun *out, volatile u8 *cancel);
/* One request to the keepalive target (nothing when there is none). */
void so_stun_keepalive(const SoStun *stun);

#endif
