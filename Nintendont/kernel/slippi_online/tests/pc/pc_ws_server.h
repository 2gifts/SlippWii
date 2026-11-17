/* PC test tools: a small WebSocket server (pc_ws_server.c). */
#ifndef PC_WS_SERVER_H
#define PC_WS_SERVER_H

#include "global.h"

#define WS_MAX_CONNS 16

/* Listens on TCP `port` (every address). 0, or -1. */
int ws_server_open(u16 port);
/* Takes new connections and reads what has come; never blocks. on_open: the
 * upgrade is done; on_text: a text message; on_close: the connection is gone
 * (its number may be given to a new one after the call). */
void ws_server_service(void (*on_open)(int conn), void (*on_text)(int conn, const char *text, u32 len),
	void (*on_close)(int conn));
void ws_server_send(int conn, const char *text);
/* A close frame, then the connection is dropped (on_close is not called). */
void ws_server_close(int conn);
/* Drops the connection without a close frame, as a network fault would. */
void ws_server_drop(int conn);
/* The client's address: the PROXY line's, or the socket's. */
const char *ws_server_peer(int conn);

#endif
