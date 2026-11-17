/* slippi_online/so_ws.c
 * WebSocket client over so_tls (see so_ws.h).
 */
#define SO_FILE_ID "ws"
#include "so_ws.h"
#include "so_alloc.h"
#include "so_platform.h"
#include "so_tcp.h"
#include "so_tls.h"
#include "so_util.h"
#include "string.h"

#define OP_CONTINUE 0x0
#define OP_TEXT 0x1
#define OP_CLOSE 0x8
#define OP_PING 0x9
#define OP_PONG 0xA
#define RX_SIZE (SO_WS_MESSAGE_MAX + 16)
#define TX_SIZE 2048

typedef struct {
	u8 rx[RX_SIZE];
	u32 have;	/* bytes of rx filled */
	u8 tx[TX_SIZE];
	u32 message;	/* bytes of a text message gathered so far from its fragments */
	int in_message;
} Ws;

static Ws *ws;
static int http_status;

/* Moves the rest of rx down to its start (the kernel has no memmove of its own). */
static void keep_from(u32 at)
{
	u32 i, rest = ws->have - at;
	for (i = 0; i < rest; i++)
		ws->rx[i] = ws->rx[at + i];
	ws->have = rest;
}

static int send_frame(u8 opcode, const u8 *payload, u32 len)
{
	u32 r = so_random();
	u8 mask[4] = { (u8)(r >> 24), (u8)(r >> 16), (u8)(r >> 8), (u8)r };
	int n = so_ws_build_frame(ws->tx, TX_SIZE, opcode, payload, len, mask);
	if (n < 0)
		return SO_WS_TOO_BIG;
	n = so_tls_write(ws->tx, (u32)n);
	return n < 0 ? n : 0;
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

int so_ws_open(const char *host, u16 port, int default_port, const char *path, u32 timeout_ms)
{
	u32 start = so_time_ms(), i;
	u8 nonce[16];
	char key[28], at[8];
	int n;

	http_status = 0;
	if (!ws) {
		ws = (Ws *)so_malloc(sizeof(Ws));
		if (!ws)
			return SO_WS_NO_MEMORY;
	}
	ws->have = 0;
	ws->message = 0;
	ws->in_message = 0;
	for (i = 0; i < sizeof(nonce); i += 4) {
		u32 r = so_random();
		memcpy(nonce + i, &r, 4);
	}
	base64(nonce, sizeof(nonce), key);
	at[0] = '\0';
	if (!default_port)
		so_snprintf(at, sizeof(at), ":%u", (unsigned)port);
	n = so_snprintf((char *)ws->tx, TX_SIZE,
		"GET %s HTTP/1.1\r\nHost: %s%s\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n"
		"Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: %s\r\n\r\n", path, host, at, key);
	if (n <= 0 || n >= TX_SIZE)
		return SO_WS_TOO_BIG;
	n = so_tls_write(ws->tx, (u32)n);
	if (n < 0)
		return n;
	/* The answer's head, to its empty line; what follows it is the first frame's. */
	for (;;) {
		u32 waited = so_time_ms() - start;
		for (i = 0; i + 4 <= ws->have; i++)
			if (!memcmp(ws->rx + i, "\r\n\r\n", 4))
				break;
		if (i + 4 <= ws->have) {
			if (ws->have >= 12 && !memcmp(ws->rx, "HTTP/1.", 7))
				http_status = (ws->rx[9] - '0') * 100 + (ws->rx[10] - '0') * 10 + (ws->rx[11] - '0');
			keep_from(i + 4);
			return http_status == 101 ? 0 : SO_WS_REFUSED;
		}
		if (ws->have >= RX_SIZE)
			return SO_WS_TOO_BIG;
		if (waited >= timeout_ms)
			return SO_TCP_TIMEOUT;
		n = so_tls_read(ws->rx + ws->have, RX_SIZE - ws->have, timeout_ms - waited);
		if (n < 0)
			return n;
		ws->have += (u32)n;
	}
}

int so_ws_send_text(const char *text, u32 len)
{
	if (!ws)
		return SO_WS_CLOSED;
	return send_frame(OP_TEXT, (const u8 *)text, len);
}

int so_ws_read_text(char *out, u32 size, u32 timeout_ms)
{
	u32 start = so_time_ms();
	if (!ws)
		return SO_WS_CLOSED;
	for (;;) {
		u8 opcode, *payload;
		u32 len, waited;
		int fin, used, n;
		used = so_ws_parse_frame(ws->rx, ws->have, SO_WS_MESSAGE_MAX, &opcode, &fin, &payload, &len);
		if (used < 0)
			return SO_WS_TOO_BIG;
		if (used > 0) {
			int done = 0, res = 0;
			if (opcode == OP_TEXT || (opcode == OP_CONTINUE && ws->in_message)) {
				if (opcode == OP_TEXT)
					ws->message = 0;
				if (ws->message + len + 1 > size) {
					res = SO_WS_TOO_BIG;
				} else {
					memcpy(out + ws->message, payload, len);
					ws->message += len;
					ws->in_message = !fin;
					if (fin) {
						out[ws->message] = '\0';
						done = 1;
					}
				}
			} else if (opcode == OP_PING) {
				/* (the answer is built from the payload before the frame leaves rx) */
				res = send_frame(OP_PONG, payload, len > 125 ? 125 : len);
			} else if (opcode == OP_CLOSE) {
				res = SO_WS_CLOSED;
			}
			keep_from((u32)used);
			if (res < 0)
				return res;
			if (done)
				return (int)ws->message;
			continue;
		}
		waited = so_time_ms() - start;
		if (waited >= timeout_ms)
			return 0;
		n = so_tls_read(ws->rx + ws->have, RX_SIZE - ws->have, timeout_ms - waited);
		if (n < 0)
			return n;
		ws->have += (u32)n;
	}
}

void so_ws_close(void)
{
	if (!ws)
		return;
	send_frame(OP_CLOSE, NULL, 0);	/* (fails quietly on a closed stream) */
	so_free(ws);
	ws = NULL;
}

int so_ws_status(void)
{
	return http_status;
}
