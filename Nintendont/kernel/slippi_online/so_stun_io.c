/* slippi_online/so_stun_io.c
 * STUN binding requests through the session socket (see so_stun.h).
 */
#define SO_FILE_ID "stun"
#include "so_stun.h"
#include "so_platform.h"
#include "so_socket.h"
#include "so_util.h"
#include "string.h"

#define ATTEMPTS 3
#define ATTEMPT_WAIT_MS 500

static void new_id(u8 id[12])
{
	int i;
	for (i = 0; i < 12; i += 4) {
		u32 r = so_random() ^ so_ticks();
		memcpy(id + i, &r, 4);
	}
}

/* One server: 0 with the address it saw, or < 0. */
static int ask(u32 server, u16 server_port, u32 *addr, u16 *port, volatile u8 *cancel)
{
	u8 request[SO_STUN_REQUEST], id[12], reply[512];
	int attempt;
	new_id(id);
	so_stun_build_request(request, id);
	for (attempt = 0; attempt < ATTEMPTS; attempt++) {
		u32 start = so_time_ms();
		if (so_socket_send(server, server_port, request, sizeof(request)) < 0)
			return -1;
		while (so_time_ms() - start < ATTEMPT_WAIT_MS) {
			u32 from;
			u16 from_port;
			int n;
			if (cancel && *cancel)
				return -2;
			so_socket_wait(100000);
			while ((n = so_socket_receive(&from, &from_port, reply, sizeof(reply), NULL)) > 0)
				if (so_stun_parse_response(id, reply, (u32)n, addr, port) == 0)
					return 0;
		}
	}
	return -1;
}

int so_stun_discover(const char *hosts, SoStun *out, volatile u8 *cancel)
{
	int answers = 0, tried = 0;
	memset(out, 0, sizeof(*out));
	while (*hosts && answers < 2 && tried < SO_STUN_SERVERS) {
		char name[64];
		const char *end = strchr(hosts, ','), *colon;
		u32 n = end ? (u32)(end - hosts) : (u32)strlen(hosts), server, addr = 0;
		u16 server_port = 3478, port = 0;
		if (n >= sizeof(name))
			n = sizeof(name) - 1;
		memcpy(name, hosts, n);
		name[n] = '\0';
		hosts = end ? end + 1 : hosts + strlen(hosts);
		colon = strchr(name, ':');
		if (colon) {
			u32 p = 0;
			const char *d;
			for (d = colon + 1; *d >= '0' && *d <= '9'; d++)
				p = p * 10 + (u32)(*d - '0');
			server_port = (u16)p;
			name[colon - name] = '\0';
		}
		if (!name[0])
			continue;
		tried++;
		if (cancel && *cancel)
			break;
		if (so_parse_ipv4(name, &server) < 0 && so_resolve(name, &server) < 0) {
			so_log("stun: could not resolve %s", name);
			continue;
		}
		if (ask(server, server_port, &addr, &port, cancel) < 0) {
			so_log("stun: no answer from %s", name);
			continue;
		}
		if (!answers) {
			out->server_addr = server;
			out->server_port = server_port;
		}
		out->have[answers] = 1;
		out->addr[answers] = addr;
		out->port[answers] = port;
		answers++;
	}
	return answers;
}

void so_stun_keepalive(const SoStun *stun)
{
	u8 request[SO_STUN_REQUEST], id[12];
	if (!stun->server_addr)
		return;
	new_id(id);
	so_stun_build_request(request, id);
	so_socket_send(stun->server_addr, stun->server_port, request, sizeof(request));
}
