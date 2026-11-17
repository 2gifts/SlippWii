/* slippi_online/so_tcp.c
 * One TCP connection through IOS (see so_tcp.h). The socket is non-blocking:
 * each IOS call returns at once, and the waiting is done here in short
 * sleeps, so a call can time out and a search can be called off (a blocking
 * IOS connect or receive cannot be). The ioctl layouts follow libogc's
 * network_wii.c (zlib licence), as so_socket.c's do.
 */
#define SO_FILE_ID "tcp"
#include "so_tcp.h"
#include "so_alloc.h"
#include "so_platform.h"
#include "string.h"
#include "../net.h"

extern s32 top_fd;

#define STEP_US 10000
#define BOUNCE 2048
/* IOS's socket errors (SO_E*) */
#define E_AGAIN -6
#define E_ALREADY -7
#define E_INPROGRESS -26
#define E_ISCONN -30
#define F_GETFL 3
#define F_SETFL 4
#define IOS_O_NONBLOCK 0x04

/* What IOS reads and writes: every piece 32-byte aligned */
typedef struct {
	u32 params[8] __attribute__((aligned(32)));
	ioctlv vec[3] __attribute__((aligned(32)));
	u8 name[32] __attribute__((aligned(32)));
	u8 bounce[BOUNCE] __attribute__((aligned(32)));
} Io;

static Io *io;
static s32 sock = -1;
static SoTcpStats stats;

static s32 timed(s32 res, u64 since)
{
	u32 took = (u32)(so_time_us() - since);
	stats.calls++;
	if (took > stats.longest_us)
		stats.longest_us = took;
	return res;
}

int so_tcp_open(u32 addr, u16 port, u32 timeout_ms, volatile int *cancel)
{
	struct sockaddr_in *sin;
	struct connect_params *cp;
	u32 start = so_time_ms();
	s32 s, res;

	so_tcp_close();
	memset(&stats, 0, sizeof(stats));
	if (!io) {
		io = (Io *)so_malloc(sizeof(Io));
		if (!io)
			return SO_TCP_NO_MEMORY;
	}
	if (top_fd < 0)
		return -62;
	io->params[0] = AF_INET;
	io->params[1] = SOCK_STREAM;
	io->params[2] = IPPROTO_IP;
	s = timed(IOS_Ioctl(top_fd, IOCTL_SO_SOCKET, io->params, 12, 0, 0), so_time_us());
	if (s < 0)
		return s;
	io->params[0] = (u32)s;
	io->params[1] = F_SETFL;
	io->params[2] = IOS_O_NONBLOCK;
	res = timed(IOS_Ioctl(top_fd, IOCTL_SO_FCNTL, io->params, 12, 0, 0), so_time_us());
	if (res < 0) {
		so_log("tcp: the socket would not take non-blocking (%d)", (int)res);
		close(top_fd, s);
		return res;
	}
	for (;;) {
		cp = (struct connect_params *)io->params;
		memset(cp, 0, sizeof(*cp));
		cp->socket = (u32)s;
		cp->has_addr = 1;
		sin = (struct sockaddr_in *)cp->name;
		sin->sin_len = 8;
		sin->sin_family = AF_INET;
		sin->sin_port = port;
		sin->sin_addr.s_addr = addr;
		res = timed(IOS_Ioctl(top_fd, IOCTL_SO_CONNECT, cp, sizeof(*cp), 0, 0), so_time_us());
		if (!stats.connect_calls++)
			stats.connect_first = res;
		stats.connect_last = res;
		if (res == 0 || res == E_ISCONN)
			break;
		if (res != E_INPROGRESS && res != E_ALREADY && res != E_AGAIN) {
			close(top_fd, s);
			return res;
		}
		if (cancel && *cancel) {
			close(top_fd, s);
			return SO_TCP_CANCELLED;
		}
		if (so_time_ms() - start >= timeout_ms) {
			close(top_fd, s);
			return SO_TCP_TIMEOUT;
		}
		so_sleep_us(STEP_US);
	}
	sock = s;
	return 0;
}

int so_tcp_send(const u8 *data, u32 len, u32 timeout_ms)
{
	struct sendto_params *sp;
	u32 start = so_time_ms(), done = 0;
	s32 res;
	if (sock < 0)
		return SO_TCP_CLOSED;
	while (done < len) {
		u32 n = len - done > BOUNCE ? BOUNCE : len - done;
		memcpy(io->bounce, data + done, n);
		sp = (struct sendto_params *)io->params;
		memset(sp, 0, sizeof(*sp));
		sp->socket = (u32)sock;
		io->vec[0].data = io->bounce;
		io->vec[0].len = n;
		io->vec[1].data = sp;
		io->vec[1].len = sizeof(*sp);
		res = timed(IOS_Ioctlv(top_fd, IOCTLV_SO_SENDTO, 2, 0, io->vec), so_time_us());
		if (res > 0) {
			done += (u32)res;
			continue;
		}
		if (res != 0 && res != E_AGAIN)
			return res;
		if (so_time_ms() - start >= timeout_ms)
			return SO_TCP_TIMEOUT;
		so_sleep_us(STEP_US);
	}
	return (int)len;
}

int so_tcp_recv(u8 *buf, u32 size, u32 timeout_ms)
{
	u32 start = so_time_ms();
	s32 res;
	if (sock < 0)
		return SO_TCP_CLOSED;
	if (size > BOUNCE)
		size = BOUNCE;
	for (;;) {
		io->params[0] = (u32)sock;
		io->params[1] = 0;
		io->vec[0].data = io->params;
		io->vec[0].len = 8;
		io->vec[1].data = io->bounce;
		io->vec[1].len = size;
		io->vec[2].data = NULL;
		io->vec[2].len = 0;
		res = timed(IOS_Ioctlv(top_fd, IOCTLV_SO_RECVFROM, 1, 2, io->vec), so_time_us());
		if (res > 0) {
			memcpy(buf, io->bounce, (u32)res);
			return res;
		}
		if (res == 0)
			return SO_TCP_CLOSED;
		if (res != E_AGAIN)
			return res;
		if (so_time_ms() - start >= timeout_ms)
			return 0;
		so_sleep_us(STEP_US);
	}
}

void so_tcp_close(void)
{
	if (sock >= 0) {
		close(top_fd, sock);
		sock = -1;
	}
}

int so_tcp_is_open(void)
{
	return sock >= 0;
}

void so_tcp_stats(SoTcpStats *out)
{
	*out = stats;
}
