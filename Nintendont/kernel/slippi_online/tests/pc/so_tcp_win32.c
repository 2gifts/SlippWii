/* PC test build: so_tcp.h on Winsock (the Wii's is so_tcp.c, on IOS). */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "so_tcp.h"
#include "so_platform.h"
#include "string.h"

static SOCKET sock = INVALID_SOCKET;
static SoTcpStats stats;

/* 1 readable or writable within the time, 0 not */
static int wait_for(SOCKET s, int writing, u32 timeout_ms)
{
	fd_set set, bad;
	struct timeval tv;
	FD_ZERO(&set);
	FD_ZERO(&bad);
	FD_SET(s, &set);
	FD_SET(s, &bad);
	tv.tv_sec = (long)(timeout_ms / 1000);
	tv.tv_usec = (long)(timeout_ms % 1000) * 1000;
	return select(0, writing ? NULL : &set, writing ? &set : NULL, &bad, &tv) > 0;
}

int so_tcp_open(u32 addr, u16 port, u32 timeout_ms, volatile int *cancel)
{
	struct sockaddr_in to;
	u_long on = 1;
	u32 start = so_time_ms();
	int flag = 1;
	SOCKET s;

	so_tcp_close();
	memset(&stats, 0, sizeof(stats));
	s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == INVALID_SOCKET)
		return -1;
	ioctlsocket(s, FIONBIO, &on);
	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&flag, sizeof(flag));
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_port = htons(port);
	to.sin_addr.s_addr = htonl(addr);
	stats.connect_calls = 1;
	if (connect(s, (struct sockaddr *)&to, sizeof(to)) != 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
		stats.connect_first = stats.connect_last = -WSAGetLastError();
		closesocket(s);
		return stats.connect_first;
	}
	for (;;) {
		int err = 0, len = sizeof(err);
		if (wait_for(s, 1, 50)) {
			getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
			if (err) {
				closesocket(s);
				return stats.connect_last = -err;
			}
			break;
		}
		if (cancel && *cancel) {
			closesocket(s);
			return SO_TCP_CANCELLED;
		}
		if (so_time_ms() - start >= timeout_ms) {
			closesocket(s);
			return SO_TCP_TIMEOUT;
		}
	}
	sock = s;
	return 0;
}

int so_tcp_send(const u8 *data, u32 len, u32 timeout_ms)
{
	u32 start = so_time_ms(), done = 0;
	if (sock == INVALID_SOCKET)
		return SO_TCP_CLOSED;
	while (done < len) {
		int n = send(sock, (const char *)data + done, (int)(len - done), 0);
		stats.calls++;
		if (n > 0) {
			done += (u32)n;
			continue;
		}
		if (WSAGetLastError() != WSAEWOULDBLOCK)
			return -WSAGetLastError();
		if (so_time_ms() - start >= timeout_ms)
			return SO_TCP_TIMEOUT;
		wait_for(sock, 1, 10);
	}
	return (int)len;
}

int so_tcp_recv(u8 *buf, u32 size, u32 timeout_ms)
{
	int n;
	if (sock == INVALID_SOCKET)
		return SO_TCP_CLOSED;
	if (!wait_for(sock, 0, timeout_ms))
		return 0;
	n = recv(sock, (char *)buf, (int)size, 0);
	stats.calls++;
	if (n > 0)
		return n;
	if (n == 0)
		return SO_TCP_CLOSED;
	return WSAGetLastError() == WSAEWOULDBLOCK ? 0 : -WSAGetLastError();
}

void so_tcp_close(void)
{
	if (sock != INVALID_SOCKET) {
		closesocket(sock);
		sock = INVALID_SOCKET;
	}
}

int so_tcp_is_open(void)
{
	return sock != INVALID_SOCKET;
}

void so_tcp_stats(SoTcpStats *out)
{
	*out = stats;
}
