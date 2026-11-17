/* slippi_online/so_ws.h
 * A WebSocket client (RFC 6455) on the so_tls stream: the HTTP upgrade,
 * then whole text messages either way. Slippi's matchmaking service speaks
 * JSON texts over one. Used from the matchmaking worker thread only.
 *
 * Kept to what that needs: text messages up to SO_WS_MESSAGE_MAX bytes,
 * pings answered, no extensions, no subprotocol. The server's
 * Sec-WebSocket-Accept is not verified (it proves only that the server
 * speaks WebSocket, which its first message shows as well; verifying it
 * would add SHA-1 to the kernel).
 */
#ifndef SO_WS_H
#define SO_WS_H

#include "global.h"

#define SO_WS_MESSAGE_MAX 8192

/* The upgrade, on a stream so_tls_open has opened. `host` and `port` are
 * for the Host header (the port is left out when it is the scheme's own).
 * Returns 0, or < 0: the stream's errors, SO_WS_REFUSED (the HTTP status is
 * then in so_ws_status), SO_WS_NO_MEMORY. */
int so_ws_open(const char *host, u16 port, int default_port, const char *path, u32 timeout_ms);
/* Sends one text message. Returns 0, or < 0. */
int so_ws_send_text(const char *text, u32 len);
/* The next text message, NUL-terminated, in `out` (`size` bytes): > 0 its
 * length, 0 none within the timeout, < 0 the stream's errors,
 * SO_WS_CLOSED (the server closed the WebSocket) or SO_WS_TOO_BIG. */
int so_ws_read_text(char *out, u32 size, u32 timeout_ms);
/* Sends a close frame (if the stream is still open) and forgets the
 * WebSocket; the stream itself is the caller's to close. */
void so_ws_close(void);
int so_ws_status(void);

#define SO_WS_REFUSED -2200
#define SO_WS_CLOSED -2201
#define SO_WS_TOO_BIG -2202
#define SO_WS_NO_MEMORY -2203

/* ---- frames (no I/O; unit-tested) ------------------------------------------ */

/* Writes a client frame (masked with `mask`) of `opcode` around `len`
 * payload bytes into `out`; returns its length, or -1 when it does not fit. */
int so_ws_build_frame(u8 *out, u32 size, u8 opcode, const u8 *payload, u32 len, const u8 mask[4]);
/* Looks at the frame that starts `data` (`have` bytes so far). Returns its
 * whole length when all of it is there (then *opcode, *fin, *payload and
 * *payload_len describe it, the payload unmasked in place), 0 when more
 * bytes are needed, -1 when it is longer than `limit`. */
int so_ws_parse_frame(u8 *data, u32 have, u32 limit, u8 *opcode, int *fin, u8 **payload, u32 *payload_len);

#endif
