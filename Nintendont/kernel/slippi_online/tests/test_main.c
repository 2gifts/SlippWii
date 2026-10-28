/* Host unit tests for the platform-independent slippi_online modules.
 * Build and run with tests/run_tests.sh. */
#include <stdio.h>
#include <stdlib.h>
#include "so_alloc.h"
#include "so_config.h"
#include "so_json.h"
#include "so_mm.h"
#include "so_pads.h"
#include "so_text.h"
#include "so_files.h"
#include "so_util.h"
#include "string.h"
#include "host_platform.h"
#include "test.h"

int test_failures;
int test_checks;

/* ---- so_util ------------------------------------------------------------- */

static void test_snprintf(void)
{
	char buf[32];
	CHECK_INT(so_snprintf(buf, sizeof(buf), "%d|%u|%x|%X", -12, 34u, 0xbeefu, 0xabu), 14);
	CHECK_STR(buf, "-12|34|beef|AB");
	so_snprintf(buf, sizeof(buf), "[%5d][%-4s][%04x]", 42, "ab", 0x1f);
	CHECK_STR(buf, "[   42][ab  ][001f]");
	so_snprintf(buf, sizeof(buf), "%.3s|%.*s|%c%%", "abcdef", 2, "xyz", 'q');
	CHECK_STR(buf, "abc|xy|q%");
	so_snprintf(buf, sizeof(buf), "%lld|%llu", -5000000000LL, 18000000000ULL);
	CHECK_STR(buf, "-5000000000|18000000000");
	/* Truncation always terminates and reports what was written. */
	CHECK_INT(so_snprintf(buf, 6, "%s", "abcdefgh"), 5);
	CHECK_STR(buf, "abcde");
	CHECK_INT(so_snprintf(buf, 1, "%d", 123), 0);
	CHECK_STR(buf, "");
}

static void test_parse(void)
{
	const char *end;
	u32 addr;
	char text[16];
	CHECK_INT(so_parse_long("-123x", &end), -123);
	CHECK_INT(*end, 'x');
	CHECK_INT(so_parse_ipv4("10.0.0.47", &addr), 0);
	CHECK_INT(addr, 0x0A00002F);
	CHECK(so_parse_ipv4("10.0.0", &addr) != 0);
	CHECK(so_parse_ipv4("256.1.1.1", &addr) != 0);
	CHECK(so_parse_ipv4("1.2.3.4x", &addr) != 0);
	CHECK(so_parse_ipv4("mm.slippi.gg", &addr) != 0);
	CHECK(so_parse_ipv4("1.2.3.0004", &addr) != 0);
	CHECK(so_parse_long("99999999999999999999999", NULL) > 0);
	{
		u16 port;
		CHECK_INT(so_parse_endpoint("10.0.0.2:41234", &addr, &port), 0);
		CHECK_INT(addr, 0x0A000002);
		CHECK_INT(port, 41234);
		CHECK(so_parse_endpoint("10.0.0.2", &addr, &port) != 0);
		CHECK(so_parse_endpoint("10.0.0.2:0", &addr, &port) != 0);
		CHECK(so_parse_endpoint("10.0.0.2:70000", &addr, &port) != 0);
		CHECK(so_parse_endpoint("10.0.0.2:1x", &addr, &port) != 0);
	}
	so_format_ipv4(0x0A00002F, text, sizeof(text));
	CHECK_STR(text, "10.0.0.47");
}

static void test_byte_order(void)
{
	u8 b[4];
	so_put_u32(b, 0x12345678);
	CHECK(b[0] == 0x12 && b[3] == 0x78);
	CHECK_INT(so_get_u32(b), 0x12345678);
	so_put_u16(b, 0xABCD);
	CHECK_INT(so_get_u16(b), 0xABCD);
}

/* ---- so_json ------------------------------------------------------------- */

static void test_json_read(void)
{
	static const char text[] =
		" {\"type\":\"get-ticket-resp\",\"isHost\":true,\"matchId\":\"mode.direct-x\","
		"\"players\":[{\"port\":1,\"isLocalPlayer\":false,\"displayName\":\"Ab\\\"cd \\u00e9\\ud83d\\ude00\","
		"\"ipAddress\":\"1.2.3.4:5\",\"rank\":{\"rating\":1234.5}},"
		"{\"port\":2,\"isLocalPlayer\":true,\"chatMessages\":[1,2]}],"
		"\"stages\":[3, 8 ,31],\"items\":0,\"neg\":-7,\"exp\":1e3,\"nil\":null} ";
	SoJson doc, players, p0, p1, stages, v, rank;
	char s[64];
	long n;
	CHECK_INT(so_json_parse(&doc, text, sizeof(text) - 1), 0);
	CHECK_INT(so_json_type(&doc), SO_JSON_OBJECT);
	so_json_get_string(&doc, "type", s, sizeof(s), "");
	CHECK_STR(s, "get-ticket-resp");
	CHECK_INT(so_json_get_bool(&doc, "isHost", 0), 1);
	CHECK_INT(so_json_get_long(&doc, "neg", 0), -7);
	CHECK_INT(so_json_get_long(&doc, "missing", 99), 99);
	CHECK_INT(so_json_get_long(&doc, "matchId", 98), 98);	/* wrong type: default */
	CHECK_INT(so_json_get(&doc, "nil", &v), 0);
	CHECK_INT(so_json_type(&v), SO_JSON_NULL);

	CHECK_INT(so_json_get(&doc, "players", &players), 0);
	CHECK_INT(so_json_count(&players), 2);
	CHECK_INT(so_json_at(&players, 0, &p0), 0);
	CHECK_INT(so_json_at(&players, 1, &p1), 0);
	CHECK(so_json_at(&players, 2, &v) != 0);
	CHECK_INT(so_json_get_long(&p0, "port", 0), 1);
	CHECK_INT(so_json_get_bool(&p1, "isLocalPlayer", 0), 1);
	so_json_get_string(&p0, "displayName", s, sizeof(s), "");
	CHECK_STR(s, "Ab\"cd \xC3\xA9\xF0\x9F\x98\x80");
	CHECK_INT(so_json_get(&p0, "rank", &rank), 0);
	CHECK_INT(so_json_get_long(&rank, "rating", 0), 1234);
	/* A key that only appears in a nested object is not a member. */
	CHECK(so_json_get(&doc, "port", &v) != 0);

	CHECK_INT(so_json_get(&doc, "stages", &stages), 0);
	CHECK_INT(so_json_count(&stages), 3);
	CHECK_INT(so_json_at(&stages, 1, &v), 0);
	CHECK_INT(so_json_long(&v, &n), 0);
	CHECK_INT(n, 8);

	/* Too small an output buffer fails rather than truncating. */
	CHECK(so_json_get(&doc, "matchId", &v) == 0 && so_json_string(&v, s, 5) < 0);
	CHECK_STR(s, "");
}

static void test_json_invalid(void)
{
	static const char *bad[] = {
		"", "{", "{\"a\":}", "{\"a\" 1}", "[1,]", "[1 2]", "{\"a\":1,}", "\"abc",
		"\"a\\x\"", "\"a\nb\"", "tru", "-", "1.", "1e", "{} {}", "[\"\\u12\"]",
		"[[[[[[[[[[[[[[[[[[[[1]]]]]]]]]]]]]]]]]]]",
	};
	unsigned i;
	SoJson doc;
	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		if (so_json_parse(&doc, bad[i], strlen(bad[i])) == 0) {
			printf("  accepted invalid JSON: %s\n", bad[i]);
			test_failures++;
		}
		test_checks++;
	}
	CHECK_INT(so_json_parse(&doc, "[]", 2), 0);
	CHECK_INT(so_json_count(&doc), 0);
	CHECK_INT(so_json_parse(&doc, "{}", 2), 0);
	CHECK(so_json_get(&doc, "a", &doc) != 0);
}

static void test_json_write(void)
{
	char buf[256];
	SoJsonWriter w;
	so_jw_init(&w, buf, sizeof(buf));
	so_jw_object_begin(&w);
	so_jw_key(&w, "a");
	so_jw_long(&w, -1);
	so_jw_key(&w, "b");
	so_jw_array_begin(&w);
	so_jw_long(&w, 130);
	so_jw_long(&w, 96);
	so_jw_array_end(&w);
	so_jw_key(&w, "c");
	so_jw_object_begin(&w);
	so_jw_key(&w, "d");
	so_jw_string(&w, "q\"\\\n\x01");
	so_jw_key(&w, "e");
	so_jw_bool(&w, 1);
	so_jw_object_end(&w);
	so_jw_key(&w, "f");
	so_jw_array_begin(&w);
	so_jw_array_end(&w);
	so_jw_object_end(&w);
	CHECK_INT(so_jw_finish(&w), (int)strlen(buf));
	CHECK_STR(buf, "{\"a\":-1,\"b\":[130,96],\"c\":{\"d\":\"q\\\"\\\\\\n\\u0001\",\"e\":true},\"f\":[]}");

	/* Overflow is reported, never written past the buffer. */
	so_jw_init(&w, buf, 8);
	so_jw_object_begin(&w);
	so_jw_key(&w, "long-key");
	so_jw_string(&w, "value");
	so_jw_object_end(&w);
	CHECK_INT(so_jw_finish(&w), -1);
	CHECK((int)strlen(buf) < 8);

	/* Round trip through the reader. */
	{
		SoJson doc;
		char s[16];
		so_jw_init(&w, buf, sizeof(buf));
		so_jw_object_begin(&w);
		so_jw_key(&w, "k");
		so_jw_string(&w, "x\ty");
		so_jw_object_end(&w);
		CHECK_INT(so_json_parse(&doc, buf, so_jw_finish(&w)), 0);
		so_json_get_string(&doc, "k", s, sizeof(s), "");
		CHECK_STR(s, "x\ty");
	}
}

/* ---- so_alloc ------------------------------------------------------------ */

static void test_alloc(void)
{
	static u8 arena[4096 + 64];
	SoAllocStats st;
	void *a, *b, *c, *d;
	CHECK_INT(so_alloc_init(arena + 3, 4096), 0);
	a = so_malloc(100);
	b = so_malloc(1);
	c = so_malloc(200);
	CHECK(a && b && c);
	CHECK(((size_t)a & 31) == 0 && ((size_t)b & 31) == 0 && ((size_t)c & 31) == 0);
	memset(a, 0xAA, 100);
	memset(b, 0xBB, 1);
	memset(c, 0xCC, 200);
	CHECK(((u8 *)a)[99] == 0xAA && ((u8 *)b)[0] == 0xBB && ((u8 *)c)[0] == 0xCC);
	so_alloc_stats(&st);
	CHECK_INT(st.blocks, 3);

	/* Freeing the middle block, then its neighbours, merges everything. */
	so_free(b);
	d = so_malloc(1);
	CHECK(d == b);	/* first fit reuses the hole */
	so_free(d);
	so_free(a);
	so_free(c);
	so_alloc_stats(&st);
	CHECK_INT(st.used, 0);
	CHECK_INT(st.blocks, 0);
	a = so_malloc(4000);	/* only possible once the arena is one block again */
	CHECK(a != NULL);
	CHECK(so_malloc(200) == NULL);
	so_alloc_stats(&st);
	CHECK_INT(st.failures, 1);
	so_free(a);

	/* A bad free is refused and logged, not corrupting the arena. */
	so_free((u8 *)arena + 200);
	CHECK(strstr(test_last_log(), "bad free") != NULL);
	CHECK(so_malloc(0) == NULL);
	a = so_malloc(4000);
	CHECK(a != NULL);
	so_free(a);
}


/* ---- so_config ----------------------------------------------------------- */

static void test_config(void)
{
	static const char ini[] =
		"# comment\r\n"
		"; also a comment\n"
		"mm_host = 10.0.0.29\n"
		"  mm_port=43114  \n"
		"delay=4\n"
		"delay=12\n"			/* out of range: ignored */
		"selftest_code=TEST#123\n"
		"lan_ip=10.0.0.9\n"
		"log_host=10.0.0.29:51451\n"
		"bogus=1\n"
		"no equals sign\n"
		"selftest=1";			/* no final newline */
	SoConfig c;
	so_config_defaults(&c);
	CHECK_STR(c.mm_host, "mm.slippi.gg");
	CHECK(so_config_is_real_mm(&c));
	CHECK_INT(c.delay, 2);
	so_config_parse(&c, ini, sizeof(ini) - 1);
	CHECK_STR(c.mm_host, "10.0.0.29");
	CHECK(!so_config_is_real_mm(&c));
	CHECK_INT(c.mm_port, 43114);
	CHECK_INT(c.delay, 4);
	CHECK_STR(c.selftest_code, "TEST#123");
	CHECK_INT(c.lan_ip, 0x0A000009);
	CHECK_INT(c.log_host, 0x0A00001D);
	CHECK_INT(c.log_port, 51451);
	CHECK_INT(c.selftest, 1);
	CHECK_STR(c.app_version, SO_APP_VERSION_DEFAULT);
}

static void test_user(void)
{
	static const char good[] =
		"{\"uid\":\"abcDEF123\",\"playKey\":\"secret-key\",\"connectCode\":\"TEST#123\","
		"\"displayName\":\"Test\",\"latestVersion\":\"3.4.1\",\"extra\":[1,{\"x\":2}]}";
	static const char no_key[] = "{\"uid\":\"a\",\"connectCode\":\"TEST#123\"}";
	SoUser u;
	CHECK_INT(so_user_parse(&u, good, sizeof(good) - 1), 0);
	CHECK_STR(u.uid, "abcDEF123");
	CHECK_STR(u.play_key, "secret-key");
	CHECK_STR(u.connect_code, "TEST#123");
	CHECK_STR(u.display_name, "Test");
	CHECK_STR(u.latest_version, "3.4.1");
	CHECK(so_user_parse(&u, no_key, sizeof(no_key) - 1) < 0);
	CHECK(so_user_parse(&u, "not json", 8) < 0);
}

/* ---- so_pads ------------------------------------------------------------- */

static void make_pad(SoLocalPad *p, s32 frame)
{
	int i;
	memset(p, 0, sizeof(*p));
	p->frame = frame;
	p->checksum_frame = frame - 10;
	p->checksum = 0xC0DE0000u + (u32)frame;
	for (i = 0; i < SO_PAD_SIZE; i++)
		p->pad[i] = (u8)(frame * 7 + i);
}

static void test_local_pads(void)
{
	static SoLocalPads q;
	static u8 packet[2048];
	SoLocalPad p;
	s32 f;
	int n;
	so_local_pads_reset(&q);
	CHECK_INT(so_local_pads_encode(&q, 1, packet, sizeof(packet)), 0);
	for (f = 1; f <= 5; f++) {
		make_pad(&p, f);
		so_local_pads_push(&q, &p);
	}
	so_local_pads_drop_acked(&q, 3);	/* frames 3..5 stay */
	CHECK_INT(q.count, 3);
	n = so_local_pads_encode(&q, 1, packet, sizeof(packet));
	CHECK_INT(n, 14 + 3 * 8);
	CHECK_INT(packet[0], 0x80);
	CHECK_INT(so_get_u32(packet + 1), 5);
	CHECK_INT(packet[5], 1);
	CHECK_INT((s32)so_get_u32(packet + 6), 5 - 10);
	CHECK_INT(so_get_u32(packet + 10), 0xC0DE0005u);
	CHECK_INT(packet[14], (u8)(5 * 7));	/* newest first */
	CHECK_INT(packet[14 + 16], (u8)(3 * 7));
	CHECK_INT(so_local_pads_encode(&q, 1, packet, 20), -1);

	/* A peer that never acks: the queue stops at newest - 128. */
	so_local_pads_reset(&q);
	for (f = 1; f <= 300; f++) {
		make_pad(&p, f);
		so_local_pads_push(&q, &p);
		so_local_pads_drop_acked(&q, 0);
	}
	CHECK_INT(q.count, 129);
	CHECK_INT(so_local_pads_at(&q, 128)->frame, 300 - 128);
}

static void test_remote_pads(void)
{
	static SoRemotePads q;
	u8 wire[8 * 4], out[8];
	int i;
	so_remote_pads_reset(&q);
	CHECK(so_remote_pads_get(&q, 1, out) < 0);
	/* Frames 1-3, newest first. */
	for (i = 0; i < 3; i++)
		memset(wire + i * 8, 3 - i, 8);
	CHECK_INT(so_remote_pads_add(&q, 3, wire, 24), 3);
	CHECK_INT(q.head_frame, 3);
	CHECK(so_remote_pads_get(&q, 1, out) == 0 && out[0] == 1);
	CHECK(so_remote_pads_get(&q, 3, out) == 0 && out[7] == 3);
	CHECK(so_remote_pads_get(&q, 4, out) < 0);
	CHECK(so_remote_pads_get(&q, 0, out) < 0);
	/* A resend overlapping old frames only adds the new ones. */
	for (i = 0; i < 4; i++)
		memset(wire + i * 8, 5 - i, 8);
	CHECK_INT(so_remote_pads_add(&q, 5, wire, 32), 2);
	CHECK(so_remote_pads_get(&q, 4, out) == 0 && out[0] == 4);
	CHECK(so_remote_pads_get(&q, 5, out) == 0 && out[0] == 5);
	/* An old packet adds nothing (and must not be acked). */
	CHECK_INT(so_remote_pads_add(&q, 4, wire, 32), 0);
	/* Too short for the frames it claims, or more than 128. */
	CHECK_INT(so_remote_pads_add(&q, 9, wire, 24), -1);
	CHECK_INT(so_remote_pads_add(&q, 5 + 129, wire, 32), -1);
	/* The ring forgets frames older than its size. */
	so_remote_pads_reset(&q);
	{
		static u8 many[128 * 8];
		s32 f;
		for (f = 128; f <= 512; f += 128)
			CHECK_INT(so_remote_pads_add(&q, f, many, sizeof(many)), 128);
		CHECK(so_remote_pads_get(&q, 512 - 255, out) == 0);
		CHECK(so_remote_pads_get(&q, 512 - 256, out) < 0);
	}
}

static void test_ack_timers(void)
{
	static SoAckTimers t;
	so_ack_timers_reset(&t);
	so_ack_timers_push(&t, 10, 1000);
	so_ack_timers_push(&t, 11, 2000);
	so_ack_timers_push(&t, 12, 3000);
	CHECK_INT(so_ack_timers_ack(&t, 11, 9000), 7000);	/* drops 10 */
	CHECK_INT(t.count, 1);
	CHECK_INT(so_ack_timers_ack(&t, 11, 9000), -1);	/* already used */
	CHECK_INT(so_ack_timers_ack(&t, 13, 9000), -1);	/* never sent: clears 12 */
	CHECK_INT(t.count, 0);
}

static void test_time_sync(void)
{
	static SoTimeSync s;
	int valid, i;
	so_time_sync_reset(&s);
	so_time_sync_offset(&s, &valid);
	CHECK_INT(valid, 0);
	/* 30 samples: the extreme thirds are ignored. */
	for (i = 0; i < 10; i++)
		so_time_sync_add(&s, -1000000);
	for (i = 0; i < 10; i++)
		so_time_sync_add(&s, 2000 + i);
	for (i = 0; i < 10; i++)
		so_time_sync_add(&s, 1000000);
	CHECK_INT(so_time_sync_offset(&s, &valid), 2004);
	CHECK_INT(valid, 1);
	/* Older samples are replaced. */
	for (i = 0; i < 30; i++)
		so_time_sync_add(&s, -500);
	CHECK_INT(so_time_sync_offset(&s, &valid), -500);
	/* One sample works as in Dolphin (trim 0). */
	so_time_sync_reset(&s);
	so_time_sync_add(&s, 77);
	CHECK_INT(so_time_sync_offset(&s, &valid), 77);
	/* We sent frame 100 at t=1 s; their frame 100 arrives 10 ms later with
	 * a 4 ms ping: they sent it 8 ms after us, so we are 8 ms ahead. */
	CHECK_INT(so_time_sync_sample(1010000, 4000, 100, 100, 1000000), 8000);
	/* Their frame 99 at the same moment: one more frame ahead. */
	CHECK_INT(so_time_sync_sample(1010000, 4000, 99, 100, 1000000), 8000 + SO_FRAME_US);
}

/* ---- so_mm messages ------------------------------------------------------ */

static void test_mm_ticket(void)
{
	SoConfig c;
	SoUser u;
	SoSearch s;
	char out[1024], err[128];
	/* "TEST#123" as the game sends it: full-width Shift-JIS. */
	static const u8 code[] = { 0x82, 0x73, 0x82, 0x64, 0x82, 0x72, 0x82, 0x73,
		0x81, 0x94, 0x82, 0x50, 0x82, 0x51, 0x82, 0x52 };
	so_config_defaults(&c);
	memset(&u, 0, sizeof(u));
	strcpy(u.uid, "UID1");
	strcpy(u.play_key, "KEY1");
	strcpy(u.connect_code, "ABCD#0");
	strcpy(u.display_name, "Ab\"cd");
	memset(&s, 0, sizeof(s));
	s.mode = SO_MODE_DIRECT;
	memcpy(s.connect_code, code, sizeof(code));
	s.connect_code_len = sizeof(code);
	CHECK(so_mm_build_ticket(out, sizeof(out), &c, &u, &s, "10.0.0.47:41234") > 0);
	CHECK_STR(out,
		"{\"appVersion\":\"3.6.4\",\"ipAddressLan\":\"10.0.0.47:41234\","
		"\"search\":{\"connectCode\":[130,115,130,100,130,114,130,115,129,148,130,80,130,81,130,82],\"mode\":2},"
		"\"type\":\"create-ticket\","
		"\"user\":{\"connectCode\":\"ABCD#0\",\"displayName\":\"Ab\\\"cd\",\"playKey\":\"KEY1\",\"uid\":\"UID1\"}}");
	CHECK_INT(so_mm_build_ticket(out, 64, &c, &u, &s, "x"), -1);

	{
		static const char ok[] = "{\"type\":\"create-ticket-resp\"}";
		static const char refused[] = "{\"type\":\"create-ticket-resp\",\"error\":\"Nope\"}";
		static const char other[] = "{\"type\":\"other\"}";
		CHECK_INT(so_mm_parse_ticket_reply(ok, strlen(ok), err, sizeof(err)), 0);
		CHECK(so_mm_parse_ticket_reply(refused, strlen(refused), err, sizeof(err)) < 0);
		CHECK_STR(err, "Nope");
		CHECK(so_mm_parse_ticket_reply(other, strlen(other), err, sizeof(err)) < 0);
	}
	CHECK_STR(err, "Invalid response when joining mm queue");
}

static void test_mm_match(void)
{
	static SoMatchResult r;
	char err[128], latest[24];
	/* We are port 2; port 1 shares our external IP, so its LAN address is
	 * used. */
	static const char same_lan[] =
		"{\"type\":\"get-ticket-resp\",\"matchId\":\"mode.direct-abc\",\"isHost\":false,"
		"\"players\":["
		"{\"port\":1,\"uid\":\"U1\",\"displayName\":\"Abcd\",\"connectCode\":\"ABCD#0\","
		"\"ipAddress\":\"203.0.113.5:50001\",\"ipAddressLan\":\"10.0.0.29:41001\",\"isLocalPlayer\":false,"
		"\"chatMessages\":[\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\",\"h\",\"i\",\"j\",\"k\",\"l\",\"m\",\"n\",\"o\",\"p\"]},"
		"{\"port\":2,\"uid\":\"U2\",\"displayName\":\"Test\",\"connectCode\":\"TEST#123\","
		"\"ipAddress\":\"203.0.113.5:50002\",\"ipAddressLan\":\"10.0.0.47:41002\",\"isLocalPlayer\":true,"
		"\"chatMessages\":[\"too few\"]}],"
		"\"stages\":[31,32],\"items\":0}";
	static const char other_wan[] =
		"{\"type\":\"get-ticket-resp\",\"isHost\":true,\"players\":["
		"{\"port\":1,\"ipAddress\":\"198.51.100.7:50001\",\"isLocalPlayer\":true},"
		"{\"port\":2,\"ipAddress\":\"203.0.113.9:50009\",\"ipAddressLan\":\"10.0.0.2:41000\"}]}";
	static const char outdated[] =
		"{\"type\":\"get-ticket-resp\",\"error\":\"Please update\",\"latestVersion\":\"3.7.0\"}";
	static const char no_local[] =
		"{\"type\":\"get-ticket-resp\",\"players\":[{\"port\":1,\"ipAddress\":\"1.2.3.4:5\"},"
		"{\"port\":2,\"ipAddress\":\"1.2.3.4:6\"}]}";

	CHECK_INT(so_mm_parse_match(same_lan, sizeof(same_lan) - 1, &r, err, sizeof(err), latest, sizeof(latest)), 0);
	CHECK_STR(r.match_id, "mode.direct-abc");
	CHECK_INT(r.player_count, 2);
	CHECK_INT(r.local_player_index, 1);
	CHECK_INT(r.is_host, 0);
	CHECK_STR(r.players[0].display_name, "Abcd");
	CHECK_INT(r.players[0].addr, 0x0A00001D);	/* LAN */
	CHECK_INT(r.players[0].udp_port, 41001);
	CHECK_INT(r.players[0].has_chat_messages, 1);
	CHECK_STR(r.players[0].chat_messages[15], "p");
	CHECK_INT(r.players[1].has_chat_messages, 0);
	CHECK_INT(r.stage_count, 2);
	CHECK_INT(r.stages[1], 32);

	CHECK_INT(so_mm_parse_match(other_wan, sizeof(other_wan) - 1, &r, err, sizeof(err), latest, sizeof(latest)), 0);
	CHECK_INT(r.is_host, 1);
	CHECK_INT(r.local_player_index, 0);
	CHECK_INT(r.players[1].addr, 0xCB007109);	/* external */
	CHECK_INT(r.players[1].udp_port, 50009);
	CHECK_INT(r.stage_count, 6);	/* defaults, with FoD for singles */
	CHECK_INT(r.stages[5], 0x2);

	CHECK(so_mm_parse_match(outdated, sizeof(outdated) - 1, &r, err, sizeof(err), latest, sizeof(latest)) < 0);
	CHECK_STR(err, "Please update");
	CHECK_STR(latest, "3.7.0");
	CHECK(so_mm_parse_match(no_local, sizeof(no_local) - 1, &r, err, sizeof(err), latest, sizeof(latest)) < 0);
	{
		static const char no_players[] = "{\"type\":\"get-ticket-resp\"}";
		CHECK(so_mm_parse_match(no_players, strlen(no_players), &r, err, sizeof(err), latest, sizeof(latest)) < 0);
		CHECK_STR(err, "Invalid response when getting mm status");
	}
}

static void test_alloc_integrity(void)
{
	static u8 arena[8192 + 32];
	SoAllocIntegrity in;
	void *a, *b, *c;
	u32 *w;
	CHECK_INT(so_alloc_init(arena, sizeof(arena)), 0);
	a = so_malloc(100);
	so_alloc_fill_free();
	CHECK_INT(so_alloc_check(), 0);
	/* Allocations and frees keep the pattern consistent. */
	b = so_malloc(500);
	c = so_malloc(64);
	so_free(b);
	so_free(a);
	CHECK_INT(so_alloc_check(), 0);
	so_alloc_integrity(&in);
	CHECK_INT(in.bad_kind, 0);
	/* A foreign write into free space is found, with its address. */
	w = (u32 *)((u8 *)c + 2048);
	*w = 0xDEADBEEF;
	CHECK(so_alloc_check() < 0);
	so_alloc_integrity(&in);
	CHECK_INT(in.bad_kind, 2);
	CHECK_INT(in.bad_value, 0xDEADBEEF);
	CHECK_INT(in.bad_addr, (u32)(size_t)w);
	/* A damaged header stops the walk: allocation fails, nothing crashes. */
	CHECK_INT(so_alloc_init(arena, sizeof(arena)), 0);
	a = so_malloc(100);
	((u32 *)a)[-8] = 0x7FFFFFFF;	/* its header's size */
	CHECK(so_malloc(100) == NULL);
	CHECK(so_alloc_check() < 0);
	so_alloc_integrity(&in);
	CHECK_INT(in.bad_kind, 1);
}

/* ---- so_text ------------------------------------------------------------- */

static void test_text(void)
{
	static const u8 logout[] = { 0x18, 0x0C, 0xF3, 0x4B, 0x4B, 0x20, 0x0A, 0x20, 0x35, 0x20, 0x28, 0x1A,
		0x20, 0x22, 0x20, 0x32, 0x20, 0x38, 0x0C, 0x00, 0xAF, 0x4B, 0x1A, 0x20, 0x1C, 0x20, 0x38, 0x20,
		0x35, 0x20, 0x28, 0x20, 0xEB, 0x00, 0x00 };
	u8 out[400], name[31], code[10];
	int n;
	n = so_premade_text(SO_TEXT_LOGOUT, 0, NULL, out, sizeof(out));
	CHECK_INT(n, 35);
	CHECK(memcmp(out, logout, sizeof(logout)) == 0);
	/* "Bo: ggs" in P1's colours: name, ':', <S>, white, message, end. */
	n = so_premade_text(SO_TEXT_CHAT_P1, 0x88, "Bo", out, sizeof(out));
	CHECK(n > 0);
	CHECK(out[0] == 0x12 && out[1] == 0x16 && out[2] == 0x0C && out[3] == 229);
	CHECK(out[6] == 0x20 && out[7] == 11 && out[8] == 0x20 && out[9] == 36 + 14);	/* B o */
	CHECK(out[10] == 0x20 && out[11] == 233 && out[12] == 0x1A);	/* ':' then <S> */
	CHECK(out[n - 2] == 0x00 && out[n - 1] == 0x00);
	/* Chat disabled, and an unknown message id. */
	CHECK(so_premade_text(SO_TEXT_CHAT_P1 + 1, SO_CHAT_DISABLED, "x y", out, sizeof(out)) > 0);
	CHECK_INT(so_premade_text(SO_TEXT_CHAT_P1 + 1, 0x99, "x", out, sizeof(out)), -1);
	CHECK_INT(so_premade_text(SO_TEXT_LOGOUT, 0, NULL, out, 10), -1);

	so_game_string("Ab cd!", 15, name);
	CHECK(memcmp(name, "Ab cd\x81\x49", 7) == 0 && name[7] == 0 && name[30] == 0);
	so_game_string("abcdefghijklmnopqrstu", 15, name);
	CHECK(memcmp(name, "abcdefghijklmno", 15) == 0 && name[15] == 0);
	so_game_string("caf\xC3\xA9#", 15, name);	/* non-ASCII dropped, '#' widened */
	CHECK(memcmp(name, "caf\x81\x94", 5) == 0 && name[5] == 0);
	so_game_connect_code("ABCD#123", code);
	CHECK(memcmp(code, "ABCD\x81\x94" "123\0", 10) == 0);
}

/* so_gct_drop / so_gct_link on a small GCT; and, with SO_TEST_GCT_OLD and
 * SO_TEST_GCT_NEW set, that dropping the Dolphin-only codes from an old
 * builder's netplay.gct gives exactly the new builder's file. */
static u32 load_file(const char *path, u8 *buf, u32 size)
{
	FILE *f = fopen(path, "rb");
	u32 n;
	if (!f)
		return 0;
	n = (u32)fread(buf, 1, size, f);
	fclose(f);
	return n;
}

static void test_gct(void)
{
	static const u8 gct[] = {
		0x00, 0xD0, 0xC0, 0xDE, 0x00, 0xD0, 0xC0, 0xDE,
		0x04, 0x37, 0x61, 0xEC, 0x48, 0x00, 0x00, 0x1C,	/* dropped (04) */
		0xC2, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,	/* kept C2 at 0x80100000, 1 line */
		0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0xC2, 0x37, 0x62, 0x00, 0x00, 0x00, 0x00, 0x01,	/* dropped C2 */
		0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x04, 0x20, 0x00, 0x00, 0x12, 0x34, 0x56, 0x78,	/* kept */
		0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	};
	static const u32 drop[] = { 0x803761EC, 0x80376200 };
	u8 g[sizeof(gct)];
	u32 len;
	memcpy(g, gct, sizeof(g));
	len = so_gct_drop(g, sizeof(g), drop, 2);
	CHECK(len == 8 + 16 + 8 + 8);
	CHECK(so_get_u32(g + 8) == 0xC2100000 && so_get_u32(g + 24) == 0x04200000 && so_get_u32(g + 32) == 0xF0000000);
	/* The C2's last word becomes a branch from load+8+8+4 back to 0x80100004. */
	CHECK(so_gct_link(g, len, 0x80400000) == 1);
	{
		u32 insn = so_get_u32(g + 20), site = 0x80400000 + 8 + 8 + 4;
		s32 disp = (s32)((insn & 0x03FFFFFC) << 6) >> 6;
		CHECK((insn & 0xFC000003) == 0x48000000 && site + (u32)disp == 0x80100004);
	}

	{
		const char *old_path = getenv("SO_TEST_GCT_OLD"), *new_path = getenv("SO_TEST_GCT_NEW");
		static u8 a[0x10000], b[0x10000];
		static const u32 dolphin_only[] = { 0x803761EC, 0x80376238 };	/* as so_files.c */
		u32 la, lb;
		if (!old_path || !new_path)
			return;
		la = load_file(old_path, a, sizeof(a));
		lb = load_file(new_path, b, sizeof(b));
		CHECK(la && lb);
		la = so_gct_drop(a, la, dolphin_only, 2);
		CHECK(la == lb && memcmp(a, b, lb) == 0);
		printf("  real codeset: %u bytes after the drop, same as the builder's\n", (unsigned)la);
	}
}

/* Pokemon Stadium's transformations are found in a disc's file table and
 * served from RAM; other disc files are not. */
static void test_disc_files(void)
{
	static u8 arena[0x20000];
	static u8 disc[0x6000];
	static const char *const names[] = { "GrPs1.dat", "GrPs2.dat", "GrPs3.dat", "GrPs4.dat", "GrPs.dat" };
	const u32 fst = 0x1000, count = 6, strings = fst + count * 12;
	u32 i, str = 0;
	u8 got[0x300];
	CHECK_INT(so_alloc_init(arena, sizeof(arena)), 0);
	memset(disc, 0, sizeof(disc));
	so_put_u32(disc + 0x424, fst);
	so_put_u32(disc + 0x428, count * 12 + 64);
	disc[fst] = 1;	/* the root directory */
	so_put_u32(disc + fst + 8, count);
	for (i = 0; i < 5; i++) {
		u8 *e = disc + fst + (i + 1) * 12;
		u32 at = 0x2000 + i * 0x800, size = 0x100 + i * 0x80;
		so_put_u32(e, str);
		so_put_u32(e + 4, at);
		so_put_u32(e + 8, size);
		memset(disc + at, 0x10 + i, size);
		strcpy((char *)disc + strings + str, names[i]);
		str += strlen(names[i]) + 1;
	}
	test_set_disc(disc, sizeof(disc));
	so_files_init();
	so_files_preload();
	for (i = 0; i < 4; i++) {
		u32 size = 0x100 + i * 0x80, k, same = 1;
		CHECK_INT(so_files_size(names[i]), size);
		CHECK_INT(so_files_read(names[i], got, sizeof(got)), size);
		for (k = 0; k < size; k++)
			same &= got[k] == 0x10 + i;
		CHECK(same);
	}
	CHECK_INT(so_files_size("GrPs.dat"), 0);	/* the disc serves the rest itself */
	test_set_disc(NULL, 0);
}

int main(void)
{
	RUN(test_snprintf);
	RUN(test_parse);
	RUN(test_byte_order);
	RUN(test_json_read);
	RUN(test_json_invalid);
	RUN(test_json_write);
	RUN(test_alloc);
	RUN(test_alloc_integrity);
	RUN(test_config);
	RUN(test_user);
	RUN(test_local_pads);
	RUN(test_remote_pads);
	RUN(test_ack_timers);
	RUN(test_time_sync);
	RUN(test_mm_ticket);
	RUN(test_mm_match);
	RUN(test_text);
	RUN(test_gct);
	RUN(test_disc_files);
	printf("%d checks, %d failures\n", test_checks, test_failures);
	return test_failures ? 1 : 0;
}
