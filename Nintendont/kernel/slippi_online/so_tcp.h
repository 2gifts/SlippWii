/* slippi_online/so_tcp.h
 * One TCP connection, for the matchmaking worker (so_mm_ws.c): Slippi's
 * matchmaking service is a WebSocket over TLS.
 *
 * Every call may block for a short time and is made from the worker thread
 * only, never from a thread on the netcode's path (so_socket.h says why).
 * No call waits longer than its timeout, and the waits are cut short when
 * `*cancel` becomes non-zero, so a search can always be called off.
 */
#ifndef SO_TCP_H
#define SO_TCP_H

#include "global.h"

/* Connects to addr:port (IPv4, network byte order). Returns 0, or < 0:
 * SO_TCP_CANCELLED, SO_TCP_TIMEOUT, or the system's error. */
int so_tcp_open(u32 addr, u16 port, u32 timeout_ms, volatile int *cancel);
/* Sends all of `data`. Returns len, or < 0. */
int so_tcp_send(const u8 *data, u32 len, u32 timeout_ms);
/* Receives up to `size` bytes: > 0 the bytes read, 0 none within the
 * timeout, < 0 the connection is closed or failed (SO_TCP_CLOSED: by the
 * other side). */
int so_tcp_recv(u8 *buf, u32 size, u32 timeout_ms);
void so_tcp_close(void);
int so_tcp_is_open(void);

#define SO_TCP_CANCELLED -2000
#define SO_TCP_TIMEOUT -2001
#define SO_TCP_CLOSED -2002
#define SO_TCP_NO_MEMORY -2003

/* What the last calls cost (diagnostics): system calls made, and the
 * longest one in microseconds. */
typedef struct {
	u32 calls, longest_us;
	s32 connect_first, connect_last;	/* the system's answers to the first and the last connect call */
	u32 connect_calls;
} SoTcpStats;
void so_tcp_stats(SoTcpStats *out);

#endif
