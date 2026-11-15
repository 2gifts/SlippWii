/* A matchmaking server that turns an old version away as Slippi's does, for
 * tools/test_online_gates.py (the 3DS fork's fake server refuses a version
 * but does not name the newer one):
 *
 *   so_pc_fake_mm PORT LATEST_VERSION ticket|match [SECONDS]
 *
 * A create-ticket whose appVersion is under LATEST_VERSION gets an error and
 * "latestVersion": with `ticket` in the create-ticket-resp, with `match` in a
 * get-ticket-resp after the ticket was taken (where Slippi Dolphin 3.6.4
 * reads it: SlippiMatchmaking::handleMatchmaking). Any other ticket is taken
 * and left waiting. Built on the client's own ENet and sockets.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "so_alloc.h"
#include "so_json.h"
#include "so_platform.h"
#include "so_socket.h"
#include "so_util.h"
#include "enet/enet.h"

char __slippi_online_stack_addr, __slippi_online_stack_size;
char __slippi_online_rx_stack_addr, __slippi_online_rx_stack_size;
char __slippi_online_done_stack_addr, __slippi_online_done_stack_size;
char __slippi_online_test_stack_addr, __slippi_online_test_stack_size;
char __slippi_online_diag_stack_addr, __slippi_online_diag_stack_size;

static void send_json(ENetHost *host, ENetPeer *peer, const char *text)
{
	ENetPacket *packet = enet_packet_create(text, strlen(text), ENET_PACKET_FLAG_RELIABLE);
	if (packet && enet_peer_send(peer, 0, packet) < 0)
		enet_packet_destroy(packet);
	enet_host_flush(host);
	printf("mm: -> %s\n", text);
	fflush(stdout);
}

int main(int argc, char **argv)
{
	ENetAddress address;
	ENetHost *host;
	ENetEvent event;
	const char *latest;
	char reply[256];
	void *arena;
	u32 arena_size, end_ms;
	int at_match;
	if (argc < 4) {
		fprintf(stderr, "usage: so_pc_fake_mm PORT LATEST_VERSION ticket|match [SECONDS]\n");
		return 2;
	}
	latest = argv[2];
	at_match = !strcmp(argv[3], "match");
	if (so_platform_init() < 0)
		return 1;
	arena = so_platform_arena(&arena_size);
	if (so_alloc_init(arena, arena_size) < 0 || so_socket_init() < 0 || enet_ios_initialize() < 0)
		return 1;
	address.host = ENET_HOST_ANY;
	address.port = (u16)atoi(argv[1]);
	host = enet_host_create(&address, 4, 3, 0, 0);
	if (!host) {
		fprintf(stderr, "could not open UDP port %s\n", argv[1]);
		return 1;
	}
	printf("fake matchmaking server on UDP %s: versions under %s are turned away, in the %s reply\n", argv[1], latest,
		at_match ? "get-ticket" : "create-ticket");
	fflush(stdout);
	end_ms = so_time_ms() + (u32)(argc > 4 ? atoi(argv[4]) : 30) * 1000;
	while ((s32)(end_ms - so_time_ms()) > 0) {
		if (enet_host_service(host, &event, 0) <= 0) {
			so_sleep_us(2000);
			continue;
		}
		if (event.type == ENET_EVENT_TYPE_RECEIVE) {
			SoJson doc;
			char version[48] = "";
			if (so_json_parse(&doc, (const char *)event.packet->data, (u32)event.packet->dataLength) == 0)
				so_json_get_string(&doc, "appVersion", version, sizeof(version), "");
			/* (the play key is in the ticket: only its version is printed) */
			printf("mm: <- create-ticket from a client: appVersion=%s; %u bytes\n", version,
				(unsigned)event.packet->dataLength);
			if (so_version_cmp(version, latest) >= 0) {
				send_json(host, event.peer, "{\"type\":\"create-ticket-resp\"}");
			} else if (at_match) {
				send_json(host, event.peer, "{\"type\":\"create-ticket-resp\"}");
				so_snprintf(reply, sizeof(reply), "{\"error\":\"Please update Slippi (fake server)\","
					"\"latestVersion\":\"%s\",\"type\":\"get-ticket-resp\"}", latest);
				send_json(host, event.peer, reply);
			} else {
				so_snprintf(reply, sizeof(reply), "{\"error\":\"Please update Slippi (fake server)\","
					"\"latestVersion\":\"%s\",\"type\":\"create-ticket-resp\"}", latest);
				send_json(host, event.peer, reply);
			}
			enet_packet_destroy(event.packet);
		}
	}
	enet_host_destroy(host);
	return 0;
}
