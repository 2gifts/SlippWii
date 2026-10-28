/* slippi_online/so_socket.h
 * Non-blocking UDP for the Nintendont kernel.
 *
 * IOS socket calls block for milliseconds (on a USB LAN adapter, sendto takes
 * 8-11 ms; on Wi-Fi 17 ms or more; measured in hardware tests M1-M1c), so
 * nothing on the netcode's path may wait on IOS:
 *   - sends go into a queue; a sender thread makes the blocking IOS sendto
 *     calls (asynchronous IOS socket requests froze the console in the M2
 *     hardware runs, so the kernel never issues them);
 *   - a receive thread stays blocked in recvfrom and queues each datagram,
 *     with its arrival time, then signals the wake event.
 * so_socket_send and so_socket_receive only touch memory.
 *
 * One socket at a time, kept open for the whole session: matchmaking and
 * then netplay run on the same port, one ENet host after the other. Closing
 * a socket while the receive thread is blocked on it is avoided on purpose
 * (IOS's behaviour there is unverified). ENet flushes the datagrams a
 * finished host left unread; ones arriving later (an opponent already
 * connecting) wait for the next host.
 * Addresses are IPv4 in network byte order, which on the Wii is the native
 * (big-endian) order.
 */
#ifndef SO_SOCKET_H
#define SO_SOCKET_H

#include "global.h"
#include "so_platform.h"

#define SO_DATAGRAM_MAX 1536

int so_socket_init(void);

/* The receive thread signals `wake` (optional) whenever a datagram arrives,
 * so one thread can wait for network input and other work together. */
void so_socket_set_wake(SoEvent *wake);

/* Opens the UDP socket and binds it to `port` (nothing to do when it is
 * already bound there). Returns 0 or < 0. */
int so_socket_open(u16 port);
/* Drops every datagram waiting. */
void so_socket_flush(void);
void so_socket_close(void);
int so_socket_is_open(void);
u16 so_socket_port(void);

/* Queues a datagram; returns len, or < 0 when the send queue is full (the
 * datagram is dropped, as UDP may) or the socket is closed. */
int so_socket_send(u32 addr, u16 port, const void *data, u32 len);

/* The same for a datagram gathered from several pieces. */
typedef struct {
	const void *data;
	u32 len;
} SoSlice;
int so_socket_sendv(u32 addr, u16 port, const SoSlice *slices, int count);

/* Sends from a second, send-only socket, which works whether or not the main
 * socket is open (the PC log sink uses it). */
int so_socket_send_aux(u32 addr, u16 port, const void *data, u32 len);

/* Copies the oldest received datagram; returns its length, or 0 when none is
 * waiting. `arrived_us` (optional) receives its arrival time (so_time_us). */
int so_socket_receive(u32 *addr, u16 *port, void *buf, u32 size, u64 *arrived_us);
int so_socket_pending(void);
/* Waits up to `timeout_us` for a datagram (needs a wake event); returns
 * so_socket_pending(). */
int so_socket_wait(u32 timeout_us);

/* DNS lookup through IOS (blocks; call only from the network thread). */
int so_resolve(const char *host, u32 *addr);
/* This console's IPv4 address, or 0 when it has none yet. */
u32 so_local_ip(void);

/* Diagnostics only: one asynchronous IOS sendto on the session socket, the
 * way builds 1-4 sent (and froze the console), optionally answering the
 * completion with IOS_ResourceReply as they did. Returns the send's result. */
int so_socket_experiment_async_send(u32 addr, u16 port, const void *data, u32 len, int reply);

typedef struct {
	u32 sent, send_dropped, send_errors;
	u32 received, receive_dropped, receive_errors;
	u32 send_queue_max;	/* deepest send queue since the last call */
	u32 send_us_max;	/* longest IOS sendto since the last call */
	u32 send_us_sum;	/* all IOS sendto time (for the average) */
} SoSocketStats;

/* Copies the counters; the *_max fields reset. */
void so_socket_stats(SoSocketStats *out);
/* The same without the lock and without resetting (diagnostics beacon). */
void so_socket_stats_peek(SoSocketStats *out);

#endif
