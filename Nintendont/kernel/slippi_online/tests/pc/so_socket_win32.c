/* PC test build: so_socket.h on Winsock, with the same structure as the IOS
 * version (a receive thread feeding a ring and signalling the wake event). */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "so_socket.h"
#include "so_util.h"
#include "string.h"

#define RX_SLOTS 32

typedef struct {
	u32 addr;
	u16 port;
	u16 len;
	u64 arrived_us;
	u8 data[SO_DATAGRAM_MAX];
} RxSlot;

static SOCKET sock = INVALID_SOCKET, aux = INVALID_SOCKET;
static u16 sock_port;
static SoEvent *wake_event;
static SoEvent rx_open_event;
static RxSlot ring[RX_SLOTS];
static u32 rx_head, rx_tail;
static SoMutex rx_lock, stats_lock;
static SoSocketStats stats;

static u32 ReceiveThread(void *unused)
{
	static u8 buf[SO_DATAGRAM_MAX];
	(void)unused;
	for (;;) {
		struct sockaddr_in from;
		int from_len = sizeof(from), n;
		SOCKET s = sock;
		if (s == INVALID_SOCKET) {
			so_event_wait(&rx_open_event, 0);
			continue;
		}
		n = recvfrom(s, (char *)buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
		if (n < 0) {
			/* WSAECONNRESET: an ICMP "port unreachable" from an earlier send. */
			if (WSAGetLastError() != WSAECONNRESET) {
				so_mutex_lock(&stats_lock);
				stats.receive_errors++;
				so_mutex_unlock(&stats_lock);
				Sleep(5);
			}
			continue;
		}
		so_mutex_lock(&rx_lock);
		if (rx_head - rx_tail >= RX_SLOTS) {
			stats.receive_dropped++;
		} else {
			RxSlot *slot = &ring[rx_head % RX_SLOTS];
			slot->addr = ntohl(from.sin_addr.s_addr);
			slot->port = ntohs(from.sin_port);
			slot->len = (u16)n;
			slot->arrived_us = so_time_us();
			memcpy(slot->data, buf, n);
			rx_head++;
			stats.received++;
		}
		so_mutex_unlock(&rx_lock);
		if (wake_event)
			so_event_signal(wake_event);
	}
	return 0;
}

int so_socket_init(void)
{
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		return -1;
	so_mutex_init(&rx_lock);
	so_mutex_init(&stats_lock);
	so_event_init(&rx_open_event);
	return so_thread_start(ReceiveThread, NULL, NULL, 0, 0) < 0 ? -2 : 0;
}

void so_socket_set_wake(SoEvent *wake)
{
	wake_event = wake;
}

int so_socket_open(u16 port)
{
	struct sockaddr_in name;
	SOCKET s;
	if (sock != INVALID_SOCKET && sock_port == port)
		return 0;
	so_socket_close();
	s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == INVALID_SOCKET)
		return -1;
	memset(&name, 0, sizeof(name));
	name.sin_family = AF_INET;
	name.sin_port = htons(port);
	if (bind(s, (struct sockaddr *)&name, sizeof(name)) != 0) {
		closesocket(s);
		return -2;
	}
	so_socket_flush();
	sock_port = port;
	sock = s;
	so_event_signal(&rx_open_event);
	return 0;
}

void so_socket_flush(void)
{
	so_mutex_lock(&rx_lock);
	rx_tail = rx_head;
	so_mutex_unlock(&rx_lock);
}

void so_socket_close(void)
{
	SOCKET s = sock;
	if (s == INVALID_SOCKET)
		return;
	sock = INVALID_SOCKET;
	closesocket(s);
}

int so_socket_is_open(void)
{
	return sock != INVALID_SOCKET;
}

u16 so_socket_port(void)
{
	return sock_port;
}

static int send_on(SOCKET s, u32 addr, u16 port, const SoSlice *slices, int count)
{
	static u8 buf[SO_DATAGRAM_MAX];
	struct sockaddr_in to;
	u32 len = 0;
	int i, n;
	for (i = 0; i < count; i++) {
		if (len + slices[i].len > sizeof(buf))
			return -1;
		memcpy(buf + len, slices[i].data, slices[i].len);
		len += slices[i].len;
	}
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_addr.s_addr = htonl(addr);
	to.sin_port = htons(port);
	n = sendto(s, (const char *)buf, (int)len, 0, (struct sockaddr *)&to, sizeof(to));
	so_mutex_lock(&stats_lock);
	if (n < 0)
		stats.send_errors++;
	else
		stats.sent++;
	so_mutex_unlock(&stats_lock);
	return n < 0 ? -1 : n;
}

int so_socket_send(u32 addr, u16 port, const void *data, u32 len)
{
	SoSlice slice;
	slice.data = data;
	slice.len = len;
	return so_socket_sendv(addr, port, &slice, 1);
}

int so_socket_sendv(u32 addr, u16 port, const SoSlice *slices, int count)
{
	if (sock == INVALID_SOCKET)
		return -1;
	return send_on(sock, addr, port, slices, count);
}

int so_socket_send_aux(u32 addr, u16 port, const void *data, u32 len)
{
	SoSlice slice;
	if (aux == INVALID_SOCKET)
		aux = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	slice.data = data;
	slice.len = len;
	return send_on(aux, addr, port, &slice, 1);
}

int so_socket_receive(u32 *addr, u16 *port, void *buf, u32 size, u64 *arrived_us)
{
	int n = 0;
	so_mutex_lock(&rx_lock);
	if (rx_tail != rx_head) {
		RxSlot *slot = &ring[rx_tail % RX_SLOTS];
		n = slot->len < size ? slot->len : (int)size;
		memcpy(buf, slot->data, n);
		*addr = slot->addr;
		*port = slot->port;
		if (arrived_us)
			*arrived_us = slot->arrived_us;
		rx_tail++;
	}
	so_mutex_unlock(&rx_lock);
	return n;
}

int so_socket_pending(void)
{
	int pending;
	so_mutex_lock(&rx_lock);
	pending = rx_tail != rx_head;
	so_mutex_unlock(&rx_lock);
	return pending;
}

int so_socket_wait(u32 timeout_us)
{
	u64 deadline = so_time_us() + timeout_us;
	while (!so_socket_pending() && wake_event && so_time_us() < deadline)
		so_event_wait(wake_event, (u32)(deadline - so_time_us()));
	return so_socket_pending();
}

int so_resolve(const char *host, u32 *addr)
{
	/* Test builds only take addresses: no lookups of real servers. */
	return so_parse_ipv4(host, addr);
}

u32 so_local_ip(void)
{
	struct sockaddr_in to, me;
	int len = sizeof(me);
	SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	u32 ip = 0;
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_addr.s_addr = htonl(0xC0000201);	/* 192.0.2.1 (TEST-NET): nothing is sent */
	to.sin_port = htons(9);
	if (connect(s, (struct sockaddr *)&to, sizeof(to)) == 0 && getsockname(s, (struct sockaddr *)&me, &len) == 0)
		ip = ntohl(me.sin_addr.s_addr);
	closesocket(s);
	return ip;
}

void so_socket_stats(SoSocketStats *out)
{
	so_mutex_lock(&stats_lock);
	*out = stats;
	so_mutex_unlock(&stats_lock);
}

int so_socket_experiment_async_send(u32 addr, u16 port, const void *data, u32 len, int reply)
{
	(void)reply;	/* no IOS on the PC: an ordinary send */
	return so_socket_send(addr, port, data, len);
}

void so_socket_stats_peek(SoSocketStats *out)
{
	*out = stats;
}
