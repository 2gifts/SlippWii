/* slippi_online/so_socket.c
 * Non-blocking UDP over IOS (see so_socket.h): a receive thread and a sender
 * thread make the blocking IOS calls; callers only touch memory.
 *
 * The IOS ioctl layouts follow libogc's network_wii.c (zlib licence) and the
 * kernel's net.c, which ports it.
 */
#define SO_FILE_ID "socket"
#include "so_socket.h"
#include "so_alloc.h"
#include "so_util.h"
#include "net.h"
#include "ipc.h"
#include "syscalls.h"
#include "string.h"

extern s32 top_fd;
extern u32 current_ip_address;
extern char __slippi_online_rx_stack_addr, __slippi_online_rx_stack_size;
extern char __slippi_online_done_stack_addr, __slippi_online_done_stack_size;	/* sender */

#define THREAD_PRIORITY 0x78
#define RX_SLOTS 32
#define TX_SLOTS 16

typedef struct {
	u32 addr;
	u16 port;
	u16 len;
	u64 arrived_us;
	u8 data[SO_DATAGRAM_MAX];
} RxSlot;

/* A datagram waiting for the sender thread. IOS reads `data` and the
 * parameters, so they are 32-byte aligned and in the kernel's data section. */
typedef struct {
	u8 data[SO_DATAGRAM_MAX];
	s32 socket;
	u32 addr;	/* 0: the socket is connected (no destination) */
	u16 port;
	u16 len;
	u64 queued_us;
} __attribute__((aligned(32))) TxSlot;

static volatile s32 sock = -1;
static s32 aux_sock = -1;	/* send-only, for diagnostics */
static volatile u32 sock_generation;
static u16 sock_port;
static SoEvent *wake_event;
static SoEvent rx_open_event;

static RxSlot *rx_ring;
static u32 rx_head, rx_tail;	/* head: next write; tail: next read */
static SoMutex rx_lock;

static TxSlot *tx_ring;	/* on the online heap, as rx_ring: the kernel's data region is full */
static u32 tx_head, tx_tail;	/* head: next write; tail: next to send */
static SoMutex tx_lock;
static SoEvent tx_event;
static struct sendto_params tx_params __attribute__((aligned(32)));
static ioctlv tx_vec[2] __attribute__((aligned(32)));

static SoSocketStats stats;
static SoMutex stats_lock;
static int initialised;

/* ---- IOS socket calls ---------------------------------------------------- */

static void fill_sockaddr(u8 *out, u32 addr, u16 port)
{
	struct sockaddr_in *sin = (struct sockaddr_in *)out;
	memset(out, 0, 8);
	sin->sin_len = 8;
	sin->sin_family = AF_INET;
	sin->sin_port = port;
	sin->sin_addr.s_addr = addr;
}

static u8 rx_buffer[SO_DATAGRAM_MAX] __attribute__((aligned(32)));
static u8 rx_from[32] __attribute__((aligned(32)));
static u32 rx_params[8] __attribute__((aligned(32)));
static ioctlv rx_vec[3] __attribute__((aligned(32)));

static s32 ios_recvfrom(s32 s, u32 *addr, u16 *port)
{
	s32 n;
	rx_params[0] = (u32)s;
	rx_params[1] = 0;
	memset(rx_from, 0, sizeof(rx_from));
	rx_from[0] = 8;
	rx_vec[0].data = rx_params;
	rx_vec[0].len = 8;
	rx_vec[1].data = rx_buffer;
	rx_vec[1].len = sizeof(rx_buffer);
	rx_vec[2].data = rx_from;
	rx_vec[2].len = 8;
	n = IOS_Ioctlv(top_fd, IOCTLV_SO_RECVFROM, 1, 2, rx_vec);
	if (n >= 0) {
		struct sockaddr_in *sin = (struct sockaddr_in *)rx_from;
		*addr = sin->sin_addr.s_addr;
		*port = sin->sin_port;
	}
	return n;
}

/* ---- threads ------------------------------------------------------------- */

/* Blocks in recvfrom and queues each datagram. */
static u32 ReceiveThread(void *unused)
{
	(void)unused;
	for (;;) {
		s32 s = sock, n;
		u32 generation = sock_generation, addr = 0;
		u16 port = 0;
		SO_TRACE(SO_THREAD_RX);
		if (s < 0) {
			so_event_wait(&rx_open_event, 0);
			continue;
		}
		SO_TRACE(SO_THREAD_RX);
		n = ios_recvfrom(s, &addr, &port);
		SO_TRACE(SO_THREAD_RX);
		if (generation != sock_generation)
			continue;	/* the socket was closed or replaced while we waited */
		if (n < 0) {
			so_mutex_lock(&stats_lock);
			stats.receive_errors++;
			so_mutex_unlock(&stats_lock);
			so_sleep_us(5000);
			continue;
		}
		so_mutex_lock(&rx_lock);
		if (rx_head - rx_tail >= RX_SLOTS) {
			so_mutex_unlock(&rx_lock);
			so_mutex_lock(&stats_lock);
			stats.receive_dropped++;
			so_mutex_unlock(&stats_lock);
		} else {
			RxSlot *slot = &rx_ring[rx_head % RX_SLOTS];
			u32 len = (u32)n > SO_DATAGRAM_MAX ? SO_DATAGRAM_MAX : (u32)n;
			slot->addr = addr;
			slot->port = port;
			slot->len = (u16)len;
			slot->arrived_us = so_time_us();
			memcpy(slot->data, rx_buffer, len);
			rx_head++;
			so_mutex_unlock(&rx_lock);
			so_mutex_lock(&stats_lock);
			stats.received++;
			so_mutex_unlock(&stats_lock);
			if (wake_event)
				so_event_signal(wake_event);
		}
	}
	return 0;
}

/* Sends the queued datagrams one by one with a blocking IOS sendto (the
 * layout libogc's net_sendto uses). Asynchronous IOS socket requests froze
 * the console in the M2 hardware runs, so the kernel never issues them. */
static u32 SenderThread(void *unused)
{
	(void)unused;
	for (;;) {
		TxSlot *slot = NULL;
		u64 started;
		s32 res;
		u32 took;
		SO_TRACE(SO_THREAD_DONE);
		so_mutex_lock(&tx_lock);
		if (tx_tail != tx_head)
			slot = &tx_ring[tx_tail % TX_SLOTS];
		so_mutex_unlock(&tx_lock);
		if (!slot) {
			so_event_wait(&tx_event, 0);
			continue;
		}
		memset(&tx_params, 0, sizeof(tx_params));
		tx_params.socket = (u32)slot->socket;
		tx_params.flags = 0;
		tx_params.has_destaddr = slot->addr != 0;
		if (slot->addr)
			fill_sockaddr(tx_params.destaddr, slot->addr, slot->port);
		tx_vec[0].data = slot->data;
		tx_vec[0].len = slot->len;
		tx_vec[1].data = &tx_params;
		tx_vec[1].len = sizeof(tx_params);
		started = so_time_us();
		SO_TRACE(SO_THREAD_DONE);
		res = IOS_Ioctlv(top_fd, IOCTLV_SO_SENDTO, 2, 0, tx_vec);
		SO_TRACE(SO_THREAD_DONE);
		took = (u32)(so_time_us() - started);
		so_mutex_lock(&stats_lock);
		if (res < 0)
			stats.send_errors++;
		else
			stats.sent++;
		if (took > stats.send_us_max)
			stats.send_us_max = took;
		stats.send_us_sum += took;
		so_mutex_unlock(&stats_lock);
		so_mutex_lock(&tx_lock);
		tx_tail++;
		so_mutex_unlock(&tx_lock);
	}
	return 0;
}

/* ---- API ----------------------------------------------------------------- */

int so_socket_init(void)
{
	if (initialised)
		return 0;
	rx_ring = (RxSlot *)so_malloc(sizeof(RxSlot) * RX_SLOTS);
	tx_ring = (TxSlot *)so_malloc(sizeof(TxSlot) * TX_SLOTS);
	if (!rx_ring || !tx_ring)
		return -1;
	if (so_mutex_init(&rx_lock) < 0 || so_mutex_init(&tx_lock) < 0 || so_mutex_init(&stats_lock) < 0 ||
	    so_event_init(&rx_open_event) < 0)
		return -2;
	if (so_event_init(&tx_event) < 0)
		return -3;
	if (so_thread_start(SenderThread, NULL, &__slippi_online_done_stack_addr,
			(u32)(size_t)&__slippi_online_done_stack_size, THREAD_PRIORITY) < 0 ||
	    so_thread_start(ReceiveThread, NULL, &__slippi_online_rx_stack_addr,
			(u32)(size_t)&__slippi_online_rx_stack_size, THREAD_PRIORITY) < 0)
		return -4;
	initialised = 1;
	return 0;
}

void so_socket_set_wake(SoEvent *wake)
{
	wake_event = wake;
}

int so_socket_open(u16 port)
{
	u8 name[32] __attribute__((aligned(32)));
	s32 s, res;
	if (!initialised)
		return -1;
	if (sock >= 0 && sock_port == port)
		return 0;	/* already bound there (see so_socket.h) */
	so_socket_close();
	SO_TRACE(SO_THREAD_NET);
	SO_PHASE("sock socket()");
	s = socket(top_fd, AF_INET, SOCK_DGRAM, IPPROTO_IP);
	SO_TRACE(SO_THREAD_NET);
	SO_PHASE("sock bind()");
	if (s < 0)
		return s;
	fill_sockaddr(name, INADDR_ANY, port);
	res = bind(top_fd, s, (struct sockaddr *)name);
	SO_TRACE(SO_THREAD_NET);
	SO_PHASE(res < 0 ? "sock bind failed" : "sock bound");
	so_log("socket: %d bound to port %u (%d)", (int)s, (unsigned)port, (int)res);
	if (res < 0) {
		close(top_fd, s);
		return res;
	}
	so_mutex_lock(&rx_lock);
	rx_head = rx_tail = 0;
	so_mutex_unlock(&rx_lock);
	sock_port = port;
	sock_generation++;
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
	s32 s = sock;
	if (s < 0)
		return;
	sock = -1;
	sock_generation++;
	close(top_fd, s);	/* also ends the receive thread's pending recvfrom */
}

int so_socket_is_open(void)
{
	return sock >= 0;
}

u16 so_socket_port(void)
{
	return sock_port;
}

int so_socket_send(u32 addr, u16 port, const void *data, u32 len)
{
	SoSlice slice;
	slice.data = data;
	slice.len = len;
	return so_socket_sendv(addr, port, &slice, 1);
}

/* Queues one datagram for the sender thread (memory only). */
static int send_on(s32 s, u32 addr, u16 port, const SoSlice *slices, int count)
{
	TxSlot *slot;
	u32 len = 0, depth;
	int i;
	for (i = 0; i < count; i++)
		len += slices[i].len;
	if (s < 0 || len > SO_DATAGRAM_MAX)
		return -1;
	so_mutex_lock(&tx_lock);
	depth = tx_head - tx_tail;
	if (depth >= TX_SLOTS) {
		so_mutex_unlock(&tx_lock);
		so_mutex_lock(&stats_lock);
		stats.send_dropped++;
		so_mutex_unlock(&stats_lock);
		return -2;
	}
	slot = &tx_ring[tx_head % TX_SLOTS];
	len = 0;
	for (i = 0; i < count; i++) {
		memcpy(slot->data + len, slices[i].data, slices[i].len);
		len += slices[i].len;
	}
	slot->socket = s;
	slot->addr = addr;
	slot->port = port;
	slot->len = (u16)len;
	slot->queued_us = so_time_us();
	tx_head++;
	so_mutex_unlock(&tx_lock);
	so_mutex_lock(&stats_lock);
	if (depth + 1 > stats.send_queue_max)
		stats.send_queue_max = depth + 1;
	so_mutex_unlock(&stats_lock);
	so_event_signal(&tx_event);
	return (int)len;
}

int so_socket_sendv(u32 addr, u16 port, const SoSlice *slices, int count)
{
	return send_on(sock, addr, port, slices, count);
}

int so_socket_send_aux(u32 addr, u16 port, const void *data, u32 len)
{
	static u32 aux_addr;
	static u16 aux_port;
	SoSlice slice;
	if (!initialised)
		return -1;
	if (aux_sock >= 0 && (aux_addr != addr || aux_port != port))
		return -1;	/* one destination per session */
	if (aux_sock < 0) {
		/* A socket connected to the PC: the M1 measurements' proven way to
		 * send to it. */
		u8 name[32] __attribute__((aligned(32)));
		s32 s = socket(top_fd, AF_INET, SOCK_DGRAM, IPPROTO_IP);
		if (s < 0)
			return s;
		fill_sockaddr(name, addr, port);
		if (connect(top_fd, s, (struct sockaddr *)name) < 0) {
			close(top_fd, s);
			return -2;
		}
		aux_sock = s;
		aux_addr = addr;
		aux_port = port;
	}
	slice.data = data;
	slice.len = len;
	{
		/* A connected UDP socket keeps failing once the PC's port was
		 * closed (an ICMP "port unreachable"): reopen it, at most every
		 * 2 s, so the log comes back when a listener does. */
		static u32 last_reopen_ms;
		int res = send_on(aux_sock, 0, 0, &slice, 1);
		if (res < 0 && so_time_ms() - last_reopen_ms > 2000) {
			last_reopen_ms = so_time_ms();
			close(top_fd, aux_sock);
			aux_sock = -1;
		}
		return res;
	}
}

int so_socket_receive(u32 *addr, u16 *port, void *buf, u32 size, u64 *arrived_us)
{
	int n = 0;
	so_mutex_lock(&rx_lock);
	if (rx_tail != rx_head) {
		RxSlot *slot = &rx_ring[rx_tail % RX_SLOTS];
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
	while (!so_socket_pending() && wake_event) {
		u64 now = so_time_us();
		if (now >= deadline)
			break;
		so_event_wait(wake_event, (u32)(deadline - now));
	}
	return so_socket_pending();
}

int so_socket_experiment_async_send(u32 addr, u16 port, const void *data, u32 len, int reply)
{
	static u8 buf[256] __attribute__((aligned(32)));
	static struct sendto_params params __attribute__((aligned(32)));
	static ioctlv vec[2] __attribute__((aligned(32)));
	static union {
		struct ipcmessage m;
		u8 room[64];
	} msg __attribute__((aligned(32)));
	static u32 queue_heap[8] __attribute__((aligned(32)));
	static s32 queue = -1;
	struct ipcmessage *done = NULL;
	s32 res;
	if (sock < 0 || len > sizeof(buf))
		return -1;
	if (queue < 0 && (queue = mqueue_create(queue_heap, 8)) < 0)
		return queue;
	memcpy(buf, data, len);
	memset(&params, 0, sizeof(params));
	params.socket = (u32)sock;
	params.has_destaddr = 1;
	fill_sockaddr(params.destaddr, addr, port);
	vec[0].data = buf;
	vec[0].len = len;
	vec[1].data = &params;
	vec[1].len = sizeof(params);
	memset(&msg, 0, sizeof(msg));
	res = IOS_IoctlvAsync(top_fd, IOCTLV_SO_SENDTO, 2, 0, vec, queue, &msg.m);
	if (res < 0)
		return res;
	mqueue_recv(queue, &done, 0);
	res = (s32)done->result;
	if (reply)
		mqueue_ack(done, 0);
	return res;
}

/* IOS fills a hostent (libogc layout) in our buffer; its pointers are
 * relative to where IOS placed the strings, so they are rebased the way
 * libogc's net_gethostbyname does. */
typedef struct {
	u32 h_name;
	u32 h_aliases;
	u16 h_addrtype;
	u16 h_length;
	u32 h_addr_list;
} IosHostent;

#define DNS_REPLY_SIZE 0x460
#define DNS_NAME_SIZE 256

/* The buffers come from the online heap (32-byte aligned, as IOS needs):
 * the kernel's data region has no room for them, and a lookup is rare. */
static int resolve(const char *host, u32 *addr, u8 *dns_reply, char *dns_name)
{
	IosHostent *h = (IosHostent *)dns_reply;
	u32 offset, *list;
	s32 res;
	int len = strlen(host) + 1;
	if (len > DNS_NAME_SIZE)
		return -1;
	memcpy(dns_name, host, len);
	memset(dns_reply, 0, DNS_REPLY_SIZE);
	res = IOS_Ioctl(top_fd, IOCTL_SO_GETHOSTBYNAME, dns_name, len, dns_reply, DNS_REPLY_SIZE);
	if (res < 0)
		return res;
	if (!h->h_name || !h->h_addr_list || h->h_length != 4)
		return -2;
	offset = h->h_name - ((u32)dns_reply + 0x10);
	list = (u32 *)(h->h_addr_list - offset);
	if ((u8 *)list < dns_reply || (u8 *)list >= dns_reply + DNS_REPLY_SIZE || !list[0])
		return -3;
	*addr = *(u32 *)(list[0] - offset);
	return 0;
}

int so_resolve(const char *host, u32 *addr)
{
	u8 *buf;
	int res;
	if (so_parse_ipv4(host, addr) == 0)
		return 0;
	buf = (u8 *)so_malloc(DNS_REPLY_SIZE + DNS_NAME_SIZE);
	if (!buf)
		return -4;
	res = resolve(host, addr, buf, (char *)buf + DNS_REPLY_SIZE);
	so_free(buf);
	return res;
}

u32 so_local_ip(void)
{
	s32 ip = IOS_Ioctl(top_fd, IOCTL_SO_GETHOSTID, 0, 0, 0, 0);
	/* A valid address may be negative as an s32 (192.168.x.x); IOS errors
	 * are small negative numbers. */
	if (ip == 0 || (ip < 0 && ip > -0x10000))
		return 0;
	current_ip_address = (u32)ip;
	return (u32)ip;
}

void so_socket_stats_peek(SoSocketStats *out)
{
	memcpy(out, (const void *)&stats, sizeof(*out));
}

void so_socket_stats(SoSocketStats *out)
{
	so_mutex_lock(&stats_lock);
	*out = stats;
	stats.send_queue_max = 0;
	stats.send_us_max = 0;
	so_mutex_unlock(&stats_lock);
}
