/* Host unit tests for the WebSocket matchmaking of Slippi Dolphin after
 * 3.6.4: its messages (so_mm_msg.c), WebSocket frames (so_ws_frame.c), STUN
 * (so_stun.c) and the settings that go with them; that version's pads and
 * drop reports (so_pads.c); the switch between the two versions.
 */
#include "test.h"
#include "so_config.h"
#include "so_mm.h"
#include "so_pads.h"
#include "so_stun.h"
#include "so_util.h"
#include "so_ws.h"

static void test_url(void)
{
	char host[64], path[64];
	u16 port;
	int secure;
	CHECK_INT(so_mm_parse_url("wss://matchmaking.slippi.gg/v1/queue", &secure, host, sizeof(host), &port, path, sizeof(path)), 0);
	CHECK_INT(secure, 1);
	CHECK_STR(host, "matchmaking.slippi.gg");
	CHECK_INT(port, 443);
	CHECK_STR(path, "/v1/queue");
	CHECK_INT(so_mm_parse_url("ws://127.0.0.1:8099/v1/queue", &secure, host, sizeof(host), &port, path, sizeof(path)), 0);
	CHECK_INT(secure, 0);
	CHECK_STR(host, "127.0.0.1");
	CHECK_INT(port, 8099);
	CHECK_INT(so_mm_parse_url("ws://name", &secure, host, sizeof(host), &port, path, sizeof(path)), 0);
	CHECK_INT(port, 80);
	CHECK_STR(path, "/");
	CHECK(so_mm_parse_url("https://slippi.gg/", &secure, host, sizeof(host), &port, path, sizeof(path)) < 0);
	CHECK(so_mm_parse_url("wss:///v1", &secure, host, sizeof(host), &port, path, sizeof(path)) < 0);
	CHECK(so_mm_parse_url("wss://name:x/", &secure, host, sizeof(host), &port, path, sizeof(path)) < 0);
	CHECK(so_mm_parse_url("wss://name:70000/", &secure, host, sizeof(host), &port, path, sizeof(path)) < 0);
	CHECK(so_mm_parse_url("wss://name/v1", &secure, host, 4, &port, path, sizeof(path)) < 0);
	CHECK(so_mm_parse_url("wss://name/v1/queue", &secure, host, sizeof(host), &port, path, 4) < 0);
}

static void test_join(void)
{
	SoConfig c;
	SoUser u;
	SoSearch s;
	SoStun stun;
	char out[1024];
	so_config_defaults(&c);
	strcpy(c.app_version, "3.4.0");
	memset(&u, 0, sizeof(u));
	strcpy(u.uid, "u");
	strcpy(u.play_key, "k");
	strcpy(u.connect_code, "ME#1");
	strcpy(u.display_name, "Me");
	memset(&s, 0, sizeof(s));
	s.mode = SO_MODE_DIRECT;
	s.connect_code[0] = 0x82;
	s.connect_code[1] = 0x60;
	s.connect_code_len = 2;
	memset(&stun, 0, sizeof(stun));
	stun.have[0] = stun.have[1] = 1;
	stun.addr[0] = stun.addr[1] = 0x01020304;
	stun.port[0] = stun.port[1] = 41000;
	/* slippi-rust-extensions' own example (protocol.rs, join_serializes_with_service_field_names),
	 * with the candidates its client adds (client.rs, build_join): byte for byte what Dolphin sends */
	CHECK(so_mm_build_join(out, sizeof(out), &c, &u, &s, "192.168.1.5:41000", 41000, &stun) > 0);
	CHECK_STR(out,
		"{\"type\":\"join\",\"user\":{\"uid\":\"u\",\"playKey\":\"k\",\"connectCode\":\"ME#1\",\"displayName\":\"Me\"},"
		"\"search\":{\"mode\":2,\"connectCode\":[130,96]},\"appVersion\":\"3.4.0\",\"port\":41000,"
		"\"ipAddressLan\":\"192.168.1.5:41000\","
		"\"candidates\":[{\"ip\":\"1.2.3.4\",\"port\":41000,\"type\":\"srflx\"},"
		"{\"ip\":\"192.168.1.5\",\"port\":41000,\"type\":\"host\"}],\"natType\":\"cone\"}");

	/* The port opponents send to is the one a STUN server saw, not the socket's own. */
	stun.port[0] = 50123;
	CHECK(so_mm_build_join(out, sizeof(out), &c, &u, &s, "192.168.1.5:41000", 41000, &stun) > 0);
	CHECK(strstr(out, "\"port\":50123,\"ipAddressLan\"") != NULL);
	CHECK(strstr(out, "\"natType\":\"symmetric\"") != NULL);

	/* No STUN answer and no LAN address: no candidates, the socket's own port, NAT unknown. */
	memset(&stun, 0, sizeof(stun));
	s.connect_code_len = 0;
	s.mode = SO_MODE_UNRANKED;
	CHECK(so_mm_build_join(out, sizeof(out), &c, &u, &s, "", 41000, &stun) > 0);
	CHECK_STR(out,
		"{\"type\":\"join\",\"user\":{\"uid\":\"u\",\"playKey\":\"k\",\"connectCode\":\"ME#1\",\"displayName\":\"Me\"},"
		"\"search\":{\"mode\":1,\"connectCode\":[]},\"appVersion\":\"3.4.0\",\"port\":41000,"
		"\"ipAddressLan\":\"\",\"natType\":\"unknown\"}");
	/* One server's answer is a candidate, and not enough to know the NAT. */
	stun.have[1] = 1;
	stun.addr[1] = 0xCB007105;
	stun.port[1] = 41011;
	CHECK(so_mm_build_join(out, sizeof(out), &c, &u, &s, "", 41000, &stun) > 0);
	CHECK(strstr(out, "\"port\":41011,") != NULL);
	CHECK(strstr(out, "\"candidates\":[{\"ip\":\"203.0.113.5\",\"port\":41011,\"type\":\"srflx\"}],\"natType\":\"unknown\"}") != NULL);
	CHECK_INT(so_mm_build_join(out, 80, &c, &u, &s, "", 41000, &stun), -1);

	CHECK(so_mm_build_resume(out, sizeof(out), "t-1", "tok\"en") > 0);
	CHECK_STR(out, "{\"type\":\"resume\",\"ticketId\":\"t-1\",\"token\":\"tok\\\"en\"}");
}

static void test_service_messages(void)
{
	static const char joined[] = "{\"type\":\"joined\",\"ticketId\":\"abc-123\",\"token\":\"secret\"}";
	static const char status[] = "{\"type\":\"status\",\"secondsInQueue\":12.5}";
	static const char error[] = "{\"type\":\"error\",\"error\":\"Too old\",\"latestVersion\":\"3.6.5\"}";
	static const char bare_error[] = "{\"type\":\"error\"}";
	static const char match[] =
		"{\"type\":\"match\",\"matchId\":\"mode.unranked-1\",\"isHost\":true,\"players\":["
		"{\"uid\":\"a\",\"displayName\":\"A\",\"connectCode\":\"AAA#1\",\"port\":1,\"isLocalPlayer\":true,"
		"\"ipAddress\":\"203.0.113.5:41001\",\"ipAddressLan\":\"192.168.1.2:41000\","
		"\"rank\":{\"rating\":1500.5,\"updateCount\":3,\"mmr\":1490.25}},"
		"{\"uid\":\"b\",\"displayName\":\"B\",\"connectCode\":\"BBB#1\",\"port\":2,\"isLocalPlayer\":false,"
		"\"ipAddress\":\"198.51.100.7:41011\",\"ipAddressLan\":\"192.168.1.2:41010\"}],\"stages\":[3,8]}";
	char ticket[16], token[16], text[128], latest[24];
	SoMatchResult r;

	CHECK_INT(so_mm_ws_kind(joined, sizeof(joined) - 1), SO_MMWS_JOINED);
	CHECK_INT(so_mm_ws_kind(status, sizeof(status) - 1), SO_MMWS_STATUS);
	CHECK_INT(so_mm_ws_kind(error, sizeof(error) - 1), SO_MMWS_ERROR);
	CHECK_INT(so_mm_ws_kind(match, sizeof(match) - 1), SO_MMWS_MATCH);
	CHECK_INT(so_mm_ws_kind("{\"type\":\"pong\"}", 15), SO_MMWS_OTHER);
	CHECK_INT(so_mm_ws_kind("{}", 2), SO_MMWS_OTHER);
	CHECK_INT(so_mm_ws_kind("[1]", 3), SO_MMWS_MALFORMED);
	CHECK_INT(so_mm_ws_kind("{\"type\":", 8), SO_MMWS_MALFORMED);

	CHECK_INT(so_mm_parse_joined(joined, sizeof(joined) - 1, ticket, sizeof(ticket), token, sizeof(token)), 0);
	CHECK_STR(ticket, "abc-123");
	CHECK_STR(token, "secret");
	/* a token that does not fit is refused, not cut */
	CHECK(so_mm_parse_joined(joined, sizeof(joined) - 1, ticket, sizeof(ticket), token, 4) < 0);
	CHECK(so_mm_parse_joined(status, sizeof(status) - 1, ticket, sizeof(ticket), token, sizeof(token)) < 0);

	so_mm_parse_ws_error(error, sizeof(error) - 1, text, sizeof(text), latest, sizeof(latest));
	CHECK_STR(text, "Too old");
	CHECK_STR(latest, "3.6.5");
	so_mm_parse_ws_error(bare_error, sizeof(bare_error) - 1, text, sizeof(text), latest, sizeof(latest));
	CHECK_STR(text, "Received an error from the mm server");
	CHECK_STR(latest, "");

	/* The match is the old get-ticket-resp under its new type: the opponent's address is the
	 * STUN-mapped one the service passes on (members Dolphin does not read, such as rank, are skipped). */
	CHECK_INT(so_mm_parse_match_ws(match, sizeof(match) - 1, &r, text, sizeof(text)), 0);
	CHECK_STR(r.match_id, "mode.unranked-1");
	CHECK_INT(r.player_count, 2);
	CHECK_INT(r.is_host, 1);
	CHECK_INT(r.local_player_index, 0);
	CHECK_INT(r.players[1].addr, 0xC6336407);
	CHECK_INT(r.players[1].udp_port, 41011);
	CHECK_INT(r.stage_count, 2);
	/* (and each service's parser takes only its own type) */
	CHECK(so_mm_parse_match(match, sizeof(match) - 1, &r, text, sizeof(text), latest, sizeof(latest)) < 0);
}

static void test_ws_frames(void)
{
	static const u8 mask[4] = { 0x37, 0xFA, 0x21, 0x3D };
	u8 frame[400], big[300], *payload, opcode;
	u32 len, i;
	int fin, n;

	/* RFC 6455's own example: a masked "Hello" */
	n = so_ws_build_frame(frame, sizeof(frame), 0x1, (const u8 *)"Hello", 5, mask);
	CHECK_INT(n, 11);
	CHECK(!memcmp(frame, "\x81\x85\x37\xfa\x21\x3d\x7f\x9f\x4d\x51\x58", 11));
	/* every prefix asks for more; the whole frame parses, unmasked in place */
	for (i = 0; i < 11; i++)
		CHECK_INT(so_ws_parse_frame(frame, i, 1000, &opcode, &fin, &payload, &len), 0);
	CHECK_INT(so_ws_parse_frame(frame, 11, 1000, &opcode, &fin, &payload, &len), 11);
	CHECK_INT(opcode, 1);
	CHECK_INT(fin, 1);
	CHECK_INT(len, 5);
	CHECK(!memcmp(payload, "Hello", 5));

	/* a server's frame (not masked), with another frame's first bytes behind it */
	memcpy(frame, "\x81\x05Hello\x89\x00", 9);
	CHECK_INT(so_ws_parse_frame(frame, 9, 1000, &opcode, &fin, &payload, &len), 7);
	CHECK(!memcmp(payload, "Hello", 5));
	CHECK_INT(so_ws_parse_frame(frame + 7, 2, 1000, &opcode, &fin, &payload, &len), 2);
	CHECK_INT(opcode, 9);
	CHECK_INT(len, 0);

	/* 126 bytes and more: the 16-bit length */
	for (i = 0; i < sizeof(big); i++)
		big[i] = (u8)i;
	n = so_ws_build_frame(frame, sizeof(frame), 0x1, big, sizeof(big), mask);
	CHECK_INT(n, 4 + 4 + 300);
	CHECK_INT(frame[1], 0x80 | 126);
	CHECK_INT(so_ws_parse_frame(frame, (u32)n - 1, 1000, &opcode, &fin, &payload, &len), 0);
	CHECK_INT(so_ws_parse_frame(frame, (u32)n, 1000, &opcode, &fin, &payload, &len), n);
	CHECK_INT(len, 300);
	CHECK(!memcmp(payload, big, sizeof(big)));
	/* a fragment (no FIN), a frame over the limit, a 64-bit length */
	memcpy(frame, "\x01\x03" "abc", 5);
	CHECK_INT(so_ws_parse_frame(frame, 5, 1000, &opcode, &fin, &payload, &len), 5);
	CHECK_INT(fin, 0);
	n = so_ws_build_frame(frame, sizeof(frame), 0x1, big, sizeof(big), mask);
	CHECK_INT(so_ws_parse_frame(frame, (u32)n, 200, &opcode, &fin, &payload, &len), -1);
	memcpy(frame, "\x81\x7f\x00\x00\x00\x00\x00\x00\x00\x03" "abc", 13);
	CHECK_INT(so_ws_parse_frame(frame, 13, 1000, &opcode, &fin, &payload, &len), 13);
	CHECK_INT(len, 3);
	memcpy(frame, "\x81\x7f\x00\x00\x00\x01\x00\x00\x00\x03" "abc", 13);
	CHECK_INT(so_ws_parse_frame(frame, 13, 1000, &opcode, &fin, &payload, &len), -1);
	CHECK_INT(so_ws_build_frame(frame, 16, 0x1, big, 100, mask), -1);
}

static void test_stun(void)
{
	static const u8 id[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
	u8 request[SO_STUN_REQUEST], reply[80];
	SoStun stun;
	u32 addr = 0;
	u16 port = 0;
	u32 n;

	so_stun_build_request(request, id);
	CHECK(!memcmp(request, "\x00\x01\x00\x00\x21\x12\xa4\x42", 8));
	CHECK(!memcmp(request + 8, id, 12));

	/* a success response with XOR-MAPPED-ADDRESS: 203.0.113.7:41234 */
	memcpy(reply, "\x01\x01\x00\x0c\x21\x12\xa4\x42", 8);
	memcpy(reply + 8, id, 12);
	memcpy(reply + 20, "\x00\x20\x00\x08\x00\x01", 6);
	reply[26] = (u8)((41234 >> 8) ^ 0x21);
	reply[27] = (u8)((41234 & 0xFF) ^ 0x12);
	reply[28] = 203 ^ 0x21;
	reply[29] = 0 ^ 0x12;
	reply[30] = 113 ^ 0xA4;
	reply[31] = 7 ^ 0x42;
	CHECK_INT(so_stun_parse_response(id, reply, 32, &addr, &port), 0);
	CHECK_INT(addr, 0xCB007107);
	CHECK_INT(port, 41234);
	/* another request's answer, an error response, a short one */
	reply[19] ^= 1;
	CHECK(so_stun_parse_response(id, reply, 32, &addr, &port) < 0);
	reply[19] ^= 1;
	reply[1] = 0x11;
	CHECK(so_stun_parse_response(id, reply, 32, &addr, &port) < 0);
	reply[1] = 0x01;
	CHECK(so_stun_parse_response(id, reply, 5, &addr, &port) < 0);

	/* an attribute of odd length first (padded to four bytes), then a plain MAPPED-ADDRESS
	 * elsewhere, then the XOR one: the XOR one is taken */
	memcpy(reply, "\x01\x01\x00\x24\x21\x12\xa4\x42", 8);
	memcpy(reply + 8, id, 12);
	memcpy(reply + 20, "\x80\x22\x00\x05hello\x00\x00\x00", 12);
	memcpy(reply + 32, "\x00\x01\x00\x08\x00\x01\x03\xe8\x0a\x00\x00\x01", 12);
	memcpy(reply + 44, "\x00\x20\x00\x08\x00\x01", 6);
	reply[50] = (u8)((5000 >> 8) ^ 0x21);
	reply[51] = (u8)((5000 & 0xFF) ^ 0x12);
	reply[52] = 198 ^ 0x21;
	reply[53] = 51 ^ 0x12;
	reply[54] = 100 ^ 0xA4;
	reply[55] = 2 ^ 0x42;
	CHECK_INT(so_stun_parse_response(id, reply, 56, &addr, &port), 0);
	CHECK_INT(addr, 0xC6336402);
	CHECK_INT(port, 5000);
	/* without the XOR one, the plain one */
	n = 44;
	reply[3] = 0x18;
	CHECK_INT(so_stun_parse_response(id, reply, n, &addr, &port), 0);
	CHECK_INT(addr, 0x0A000001);
	CHECK_INT(port, 1000);
	/* an attribute that claims more bytes than there are: no address */
	reply[3] = 0x0C;
	reply[23] = 0x40;
	CHECK(so_stun_parse_response(id, reply, 32, &addr, &port) < 0);

	memset(&stun, 0, sizeof(stun));
	CHECK_STR(so_stun_nat_type(&stun), "unknown");
	stun.have[0] = 1;
	stun.addr[0] = 0x01010101;
	stun.port[0] = 40000;
	CHECK_STR(so_stun_nat_type(&stun), "unknown");
	stun.have[1] = 1;
	stun.addr[1] = 0x01010101;
	stun.port[1] = 40000;
	CHECK_STR(so_stun_nat_type(&stun), "cone");
	stun.port[1] = 40001;
	CHECK_STR(so_stun_nat_type(&stun), "symmetric");
}

static void test_next_config(void)
{
	static const char ini[] =
		"mm_url=wss://matchmaking.slippwii.test:8443/v1/queue\n"
		"stun_hosts=10.0.0.29:3478,10.0.0.29:3479\n"
		"test_mm_addr=10.0.0.29\n"
		"test_tls_root=04"
		"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
		"202122232425262728292A2B2C2D2E2F303132333435363738393a3b3c3d3e3f\n";
	SoConfig c;
	so_config_defaults(&c);
	CHECK_STR(c.mm_url, SO_MM_URL_DEFAULT);
	CHECK_STR(c.stun_hosts, SO_STUN_HOSTS_DEFAULT);
	CHECK_INT(c.test_tls_root_set, 0);
	CHECK(so_config_is_real_mm(&c));
	so_config_parse(&c, ini, sizeof(ini) - 1);
	CHECK_STR(c.mm_url, "wss://matchmaking.slippwii.test:8443/v1/queue");
	CHECK_STR(c.stun_hosts, "10.0.0.29:3478,10.0.0.29:3479");
	CHECK_INT(c.test_mm_addr, 0x0A00001D);
	CHECK_INT(c.test_tls_root_set, 1);
	CHECK_INT(c.test_tls_root[0], 4);
	CHECK_INT(c.test_tls_root[1], 0);
	CHECK_INT(c.test_tls_root[43], 0x2A);
	CHECK_INT(c.test_tls_root[64], 0x3F);
	/* the service's address decides whether this is Slippi's own (mm_host is the old service's) */
	CHECK(!so_config_is_real_mm(&c));
	strcpy(c.mm_url, "wss://matchmaking.slippi.gg/v1/queue");
	CHECK(so_config_is_real_mm(&c));
	/* a key of the wrong length, or not hex, is not taken */
	so_config_defaults(&c);
	so_config_parse(&c, "test_tls_root=0401\n", 19);
	CHECK_INT(c.test_tls_root_set, 0);
}

/* Pads of 9 bytes on the wire (3.6.5: the inputs and the frame's RNG bits), 8 as before. */
static void test_wire_pads(void)
{
	static SoLocalPads local;
	static SoRemotePads q;
	SoLocalPad p;
	u8 packet[64], wire[9 * 3], out[SO_PAD_SIZE];
	int f, b;

	so_pad_wire = 9;
	so_local_pads_reset(&local);
	for (f = 1; f <= 2; f++) {
		memset(&p, 0, sizeof(p));
		p.frame = f;
		for (b = 0; b < SO_PAD_SIZE; b++)
			p.pad[b] = (u8)(f * 16 + b);
		so_local_pads_push(&local, &p);
	}
	CHECK_INT(so_local_pads_encode(&local, 1, packet, sizeof(packet)), 14 + 2 * 9);
	CHECK_INT(packet[14], 0x20);		/* newest first */
	CHECK_INT(packet[14 + 8], 0x28);	/* its RNG bits */
	CHECK_INT(packet[14 + 9], 0x10);
	CHECK_INT(packet[14 + 17], 0x18);
	CHECK_INT(so_local_pads_encode(&local, 1, packet, 14 + 2 * 9 - 1), -1);

	so_remote_pads_reset(&q);
	for (f = 0; f < 3; f++)	/* frames 3, 2, 1 */
		for (b = 0; b < 9; b++)
			wire[f * 9 + b] = (u8)((3 - f) * 16 + b);
	CHECK_INT(so_remote_pads_add(&q, 3, wire, 26), -1);	/* a byte short */
	CHECK_INT(so_remote_pads_add(&q, 3, wire, 27), 3);
	CHECK(so_remote_pads_get(&q, 1, out) == 0 && out[0] == 0x10 && out[8] == 0x18);
	CHECK(so_remote_pads_get(&q, 3, out) == 0 && out[0] == 0x30 && out[8] == 0x38);

	/* 3.6.4's: 8 bytes, and the ninth kept as 0 */
	so_pad_wire = 8;
	CHECK_INT(so_local_pads_encode(&local, 1, packet, sizeof(packet)), 14 + 2 * 8);
	CHECK_INT(packet[14 + 8], 0x10);
	so_remote_pads_reset(&q);
	memset(out, 0xEE, sizeof(out));
	CHECK_INT(so_remote_pads_add(&q, 3, wire, 24), 3);
	CHECK(so_remote_pads_get(&q, 3, out) == 0 && out[0] == 0x30 && out[7] == 0x37 && out[8] == 0);
	so_pad_wire = SO_PAD_INPUT;
}

static void fill_queue(SoRemotePads *q, s32 from, s32 to)
{
	u8 pad[SO_PAD_SIZE];
	s32 f;
	for (f = from; f <= to; f++) {
		memset(pad, (u8)f, sizeof(pad));
		pad[8] = (u8)(f ^ 0x80);
		so_remote_pads_push(q, f, pad);
	}
}

/* A player lost in a game of several: what each tells the others of its inputs, and what is taken. */
static void test_drop_reports(void)
{
	static SoRemotePads ours, theirs;
	u8 report[SO_DROP_REPORT_MAX], out[SO_PAD_SIZE], pad[SO_PAD_SIZE];
	int len, f;

	so_pad_wire = 9;
	/* nothing held: frame 0, no pads */
	so_remote_pads_reset(&theirs);
	CHECK_INT(so_drop_report_encode(&theirs, report), 5);
	CHECK_INT(so_get_u32(report), 0);
	CHECK_INT(report[4], 0);

	fill_queue(&theirs, 1, 10);
	len = so_drop_report_encode(&theirs, report);
	CHECK_INT(len, 5 + 10 * 9);
	CHECK_INT(so_get_u32(report), 10);
	CHECK_INT(report[4], 10);
	CHECK_INT(report[5], 10);		/* newest first */
	CHECK_INT(report[5 + 8], 10 ^ 0x80);
	CHECK_INT(report[5 + 9 * 9], 1);

	/* we had it to frame 7: frames 8 to 10 come from the report, the ones we had stay ours */
	so_remote_pads_reset(&ours);
	fill_queue(&ours, 1, 7);
	memset(pad, 0x55, sizeof(pad));
	so_remote_pads_push(&ours, 7, pad);	/* (ours of frame 7, not what the report has for it) */
	ours.count--;
	CHECK_INT(so_drop_report_apply(&ours, 10, report + 5, report[4]), 3);
	CHECK_INT(ours.head_frame, 10);
	CHECK(so_remote_pads_get(&ours, 7, out) == 0 && out[0] == 0x55);
	CHECK(so_remote_pads_get(&ours, 8, out) == 0 && out[0] == 8 && out[8] == (8 ^ 0x80));
	CHECK(so_remote_pads_get(&ours, 10, out) == 0 && out[0] == 10 && out[8] == (10 ^ 0x80));
	/* the same report again, or an older one: nothing new */
	CHECK_INT(so_drop_report_apply(&ours, 10, report + 5, report[4]), 0);
	CHECK_INT(so_drop_report_apply(&ours, 4, report + 5, 4), 0);
	CHECK_INT(ours.head_frame, 10);

	/* 64 inputs at most, of the newest */
	so_remote_pads_reset(&theirs);
	fill_queue(&theirs, 1, 100);
	len = so_drop_report_encode(&theirs, report);
	CHECK_INT(len, 5 + 64 * 9);
	CHECK_INT(so_get_u32(report), 100);
	CHECK_INT(report[4], 64);
	CHECK_INT(report[5 + 63 * 9], 100 - 63);
	/* a report that does not reach back to our newest: the frames between hold the pad before them */
	so_remote_pads_reset(&ours);
	fill_queue(&ours, 1, 30);
	CHECK_INT(so_drop_report_apply(&ours, 100, report + 5, report[4]), 70);
	CHECK(so_remote_pads_get(&ours, 31, out) == 0 && out[0] == 30);
	CHECK(so_remote_pads_get(&ours, 36, out) == 0 && out[0] == 30);
	CHECK(so_remote_pads_get(&ours, 37, out) == 0 && out[0] == 37);
	CHECK(so_remote_pads_get(&ours, 100, out) == 0 && out[0] == 100);
	/* more than a packet's worth ahead: not taken */
	so_remote_pads_reset(&ours);
	fill_queue(&ours, 1, 5);
	CHECK_INT(so_drop_report_apply(&ours, 5 + 129, report + 5, report[4]), -1);
	CHECK_INT(ours.head_frame, 5);
	/* we had nothing of it at all */
	so_remote_pads_reset(&ours);
	so_remote_pads_reset(&theirs);
	fill_queue(&theirs, 1, 3);
	so_drop_report_encode(&theirs, report);
	CHECK_INT(so_drop_report_apply(&ours, 3, report + 5, report[4]), 3);
	CHECK(so_remote_pads_get(&ours, 1, out) == 0 && out[0] == 1);
	CHECK(so_remote_pads_get(&ours, 3, out) == 0 && out[0] == 3);

	/* 3.6.4's pads in a report (never sent: the report came with 3.6.5; the size is the wire's) */
	so_pad_wire = 8;
	CHECK_INT(so_drop_report_encode(&theirs, report), 5 + 3 * 8);
	so_pad_wire = 9;

	/* its last input, held until its character leaves */
	for (f = 0; f < 2; f++) {
		so_remote_pads_reset(&ours);
		if (f)
			fill_queue(&ours, 1, 10);
		so_remote_pads_repeat(&ours, 26);
		CHECK_INT(ours.head_frame, 26);
		CHECK(so_remote_pads_get(&ours, 11, out) == 0 && out[0] == (f ? 10 : 0) && out[8] == (f ? (10 ^ 0x80) : 0));
		CHECK(so_remote_pads_get(&ours, 26, out) == 0 && out[0] == (f ? 10 : 0));
		so_remote_pads_repeat(&ours, 20);	/* (already past it) */
		CHECK_INT(ours.head_frame, 26);
	}
	so_pad_wire = SO_PAD_INPUT;
}

/* The one switch for the Slippi version a build is for (SO_SLIPPI_NEXT, so_config.h): this file is
 * built and run both ways (run_tests.sh). */
static void test_version_switch(void)
{
	static const char test_ini[] = "mm_host=10.0.0.29\nmm_port=43113\n";
	static const char next_ini[] = "mm_host=10.0.0.29\nmm_url=wss://matchmaking.slippwii.test:8443/v1/queue\napp_version=3.6.5-dev.1\n";
	static const char theirs[] = "mm_url=wss://matchmaking.slippi.gg/other\napp_version=9.9.9\n";
	SoConfig c;

	so_config_defaults(&c);
	CHECK(so_config_is_real_mm(&c));
	CHECK_INT(so_config_finish(&c), 0);
#if SO_SLIPPI_NEXT
	CHECK_STR(c.app_version, "3.7.0");
	CHECK_STR(c.mm_url, "wss://matchmaking.slippi.gg/v1/queue");
	CHECK(so_config_next(&c));
#else
	CHECK_STR(c.app_version, "3.6.4");
	CHECK_STR(c.mm_url, "");
	CHECK(!so_config_next(&c));
#endif
	/* a test that names its server gets that one, the old way, in either build: never Slippi's */
	so_config_parse(&c, test_ini, sizeof(test_ini) - 1);
	CHECK_STR(c.mm_url, "");
	CHECK(!so_config_is_real_mm(&c));
	CHECK_INT(so_config_finish(&c), 0);
	CHECK_STR(c.mm_url, "");
	/* and the new way with the address after it; the version it speaks is then the test's too */
	so_config_defaults(&c);
	so_config_parse(&c, next_ini, sizeof(next_ini) - 1);
	CHECK(!so_config_is_real_mm(&c));
	CHECK_INT(so_config_finish(&c), 0);
	CHECK_STR(c.mm_url, "wss://matchmaking.slippwii.test:8443/v1/queue");
	CHECK(so_config_next(&c));
	/* against Slippi's own, neither the version nor the service is config.ini's to choose */
	so_config_defaults(&c);
	so_config_parse(&c, theirs, sizeof(theirs) - 1);
	CHECK(so_config_is_real_mm(&c));
	CHECK_INT(so_config_finish(&c), 1);
	CHECK_STR(c.app_version, SO_APP_VERSION_DEFAULT);
	CHECK_STR(c.mm_url, SO_MM_URL_DEFAULT);
	so_config_defaults(&c);
	so_config_parse(&c, "mm_url=\n", 8);
	CHECK(so_config_is_real_mm(&c));	/* (mm_host is still Slippi's) */
	so_config_finish(&c);
	CHECK_STR(c.mm_url, SO_MM_URL_DEFAULT);
	strcpy(c.mm_url, "wss://MatchMaking.SLIPPI.GG/v1/queue");
	CHECK(so_config_is_real_mm(&c));	/* (a host's name in any case) */
	strcpy(c.mm_url, "wss://matchmaking.slippi.g");
	CHECK(!so_config_is_real_mm(&c));
}

void test_next(void)
{
	test_wire_pads();
	test_drop_reports();
	test_version_switch();
	test_url();
	test_join();
	test_service_messages();
	test_ws_frames();
	test_stun();
	test_next_config();
}
