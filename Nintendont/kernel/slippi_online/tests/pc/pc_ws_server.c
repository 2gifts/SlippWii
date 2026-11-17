/* PC test tools: a small WebSocket server (plain TCP, Winsock), for the test
 * matchmaking server's side of Slippi's WebSocket service (pc_lobby_mm.c).
 * Text messages only; a few connections; polled, never blocking.
 *
 * TLS is not done here: tools/sweep/tls_front.py takes the Wii's TLS
 * connection and passes the bytes on. It starts each connection with a
 * PROXY protocol line ("PROXY TCP4 <source> <dest> <port> <port>\r\n") so
 * that the server still learns where the client is, as a real service
 * behind a load balancer does.
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "bearssl.h"
#include "so_ws.h"
#include "pc_ws_server.h"

#define BUF 16384

typedef struct {
	SOCKET s;
	int open, upgraded;
	char peer[20];
	u8 rx[BUF];
	u32 have;
} Conn;

static SOCKET listener = INVALID_SOCKET;
static Conn conns[WS_MAX_CONNS];

int ws_server_open(u16 port)
{
	struct sockaddr_in at;
	u_long on = 1;
	int yes = 1;
	listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listener == INVALID_SOCKET)
		return -1;
	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
	memset(&at, 0, sizeof(at));
	at.sin_family = AF_INET;
	at.sin_port = htons(port);
	at.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(listener, (struct sockaddr *)&at, sizeof(at)) != 0 || listen(listener, 8) != 0)
		return -1;
	ioctlsocket(listener, FIONBIO, &on);
	return 0;
}

const char *ws_server_peer(int conn)
{
	return conns[conn].peer;
}

void ws_server_drop(int conn)
{
	Conn *c = &conns[conn];
	if (c->open) {
		closesocket(c->s);
		c->open = 0;
	}
}

static void send_all(Conn *c, const u8 *data, u32 len)
{
	u32 done = 0;
	int tries = 0;
	while (done < len && tries < 200) {
		int n = send(c->s, (const char *)data + done, (int)(len - done), 0);
		if (n > 0) {
			done += (u32)n;
		} else if (WSAGetLastError() == WSAEWOULDBLOCK) {
			Sleep(5);
			tries++;
		} else {
			break;
		}
	}
}

static void send_frame(Conn *c, u8 opcode, const u8 *payload, u32 len)
{
	static u8 frame[BUF + 8];
	u32 n = 0;
	if (len > BUF)
		return;
	frame[n++] = (u8)(0x80 | opcode);
	if (len < 126) {
		frame[n++] = (u8)len;
	} else {
		frame[n++] = 126;
		frame[n++] = (u8)(len >> 8);
		frame[n++] = (u8)len;
	}
	memcpy(frame + n, payload, len);
	send_all(c, frame, n + len);
}

void ws_server_send(int conn, const char *text)
{
	if (conns[conn].open && conns[conn].upgraded)
		send_frame(&conns[conn], 0x1, (const u8 *)text, (u32)strlen(text));
}

void ws_server_close(int conn)
{
	if (conns[conn].open && conns[conn].upgraded)
		send_frame(&conns[conn], 0x8, NULL, 0);
	ws_server_drop(conn);
}

static void base64(const u8 *in, u32 len, char *out)
{
	static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	u32 i;
	for (i = 0; i < len; i += 3) {
		u32 v = (u32)in[i] << 16 | (i + 1 < len ? (u32)in[i + 1] << 8 : 0) | (i + 2 < len ? in[i + 2] : 0);
		*out++ = digits[v >> 18];
		*out++ = digits[(v >> 12) & 63];
		*out++ = i + 1 < len ? digits[(v >> 6) & 63] : '=';
		*out++ = i + 2 < len ? digits[v & 63] : '=';
	}
	*out = '\0';
}

/* The HTTP upgrade (after an optional PROXY line): 1 done, 0 more bytes needed, -1 not one. */
static int upgrade(Conn *c)
{
	static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
	char *text = (char *)c->rx, *end, *key, *line_end, accept[32], reply[256];
	br_sha1_context sha;
	u8 digest[20];
	u32 used;
	int n;
	c->rx[c->have] = '\0';
	if (c->have >= 6 && !memcmp(text, "PROXY ", 6)) {
		char *eol = strstr(text, "\r\n"), *src;
		if (!eol)
			return 0;
		src = strchr(text + 6, ' ');	/* "PROXY TCP4 <source> ..." */
		if (src) {
			char *stop = strchr(src + 1, ' ');
			u32 len = stop ? (u32)(stop - src - 1) : 0;
			if (len && len < sizeof(c->peer)) {
				memcpy(c->peer, src + 1, len);
				c->peer[len] = '\0';
			}
		}
		used = (u32)(eol + 2 - text);
		memmove(c->rx, c->rx + used, c->have - used + 1);
		c->have -= used;
	}
	end = strstr(text, "\r\n\r\n");
	if (!end)
		return c->have >= BUF - 1 ? -1 : 0;
	key = strstr(text, "Sec-WebSocket-Key:");
	if (!key || key > end || strncmp(text, "GET ", 4) != 0)
		return -1;
	key += 18;
	while (*key == ' ')
		key++;
	line_end = strstr(key, "\r\n");
	br_sha1_init(&sha);
	br_sha1_update(&sha, key, (size_t)(line_end - key));
	br_sha1_update(&sha, guid, sizeof(guid) - 1);
	br_sha1_out(&sha, digest);
	base64(digest, sizeof(digest), accept);
	n = snprintf(reply, sizeof(reply), "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
		"Sec-WebSocket-Accept: %s\r\n\r\n", accept);
	send_all(c, (const u8 *)reply, (u32)n);
	used = (u32)(end + 4 - text);
	memmove(c->rx, c->rx + used, c->have - used);
	c->have -= used;
	c->upgraded = 1;
	return 1;
}

void ws_server_service(void (*on_open)(int conn), void (*on_text)(int conn, const char *text, u32 len),
	void (*on_close)(int conn))
{
	int i;
	if (listener == INVALID_SOCKET)
		return;
	for (;;) {
		struct sockaddr_in from;
		int from_len = sizeof(from);
		u_long on = 1;
		SOCKET s = accept(listener, (struct sockaddr *)&from, &from_len);
		Conn *c = NULL;
		if (s == INVALID_SOCKET)
			break;
		for (i = 0; i < WS_MAX_CONNS && !c; i++)
			if (!conns[i].open)
				c = &conns[i];
		if (!c) {
			closesocket(s);
			continue;
		}
		memset(c, 0, sizeof(*c));
		c->s = s;
		c->open = 1;
		ioctlsocket(s, FIONBIO, &on);
		snprintf(c->peer, sizeof(c->peer), "%s", inet_ntoa(from.sin_addr));
	}
	for (i = 0; i < WS_MAX_CONNS; i++) {
		Conn *c = &conns[i];
		int n, gone = 0;
		if (!c->open)
			continue;
		n = recv(c->s, (char *)c->rx + c->have, (int)(BUF - 1 - c->have), 0);
		if (n > 0)
			c->have += (u32)n;
		else if (n == 0 || WSAGetLastError() != WSAEWOULDBLOCK)
			gone = 1;
		if (!gone && !c->upgraded && c->have) {
			int res = upgrade(c);
			if (res < 0)
				gone = 1;
			else if (res > 0 && on_open)
				on_open(i);
		}
		while (!gone && c->upgraded && c->open) {
			u8 opcode, *payload;
			u32 len;
			int fin, used = so_ws_parse_frame(c->rx, c->have, BUF - 16, &opcode, &fin, &payload, &len);
			if (used < 0)
				gone = 1;
			if (used <= 0)
				break;
			if (opcode == 0x1) {
				static char text[BUF];
				memcpy(text, payload, len);
				text[len] = '\0';
				on_text(i, text, len);
			} else if (opcode == 0x9) {
				send_frame(c, 0xA, payload, len);
			} else if (opcode == 0x8) {
				gone = 1;
			}
			memmove(c->rx, c->rx + used, c->have - (u32)used);
			c->have -= (u32)used;
		}
		if (gone && c->open) {
			ws_server_drop(i);
			if (on_close)
				on_close(i);
		}
	}
}
