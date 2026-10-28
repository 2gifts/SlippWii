/* slippi_online/so_config.c
 * config.ini and user.json (see so_config.h).
 */
#include "so_config.h"
#include "so_json.h"
#include "so_platform.h"
#include "so_util.h"
#include "string.h"

void so_config_defaults(SoConfig *config)
{
	memset(config, 0, sizeof(*config));
	strlcpy(config->user_path, SO_USER_PATH, sizeof(config->user_path));
	strlcpy(config->mm_host, SO_MM_HOST_DEFAULT, sizeof(config->mm_host));
	config->mm_port = SO_MM_PORT_DEFAULT;
	strlcpy(config->app_version, SO_APP_VERSION_DEFAULT, sizeof(config->app_version));
	config->delay = 2;
	config->boot_quiet_s = 60;
	config->selftest_stage_pause_s = 3;
	/* Above the network threads (0x78): Melee's EXI requests (the 0xB0 every
	 * frame, the replay data) are served first. A low main loop (0x30)
	 * let network work hold them for milliseconds: missed refreshes. */
	config->online_main_priority = 0x7C;
	config->online_loop_us = 200;
	config->log_port = 51450;
	config->selftest_frames = 600;
	config->chat = 1;
}

static int parse_ranged(const char *value, long min, long max, long *out)
{
	const char *end;
	long v = so_parse_long(value, &end);
	if (end == value || *end != '\0' || v < min || v > max)
		return -1;
	*out = v;
	return 0;
}

static int copy_value(char *out, int size, const char *value)
{
	if ((int)strlen(value) >= size)
		return -1;
	strlcpy(out, value, size);
	return 0;
}

/* Applies one setting; returns < 0 for a bad value or unknown key. */
static int apply(SoConfig *c, const char *key, const char *value)
{
	long v;
	if (!strcmp(key, "user_path"))
		return copy_value(c->user_path, sizeof(c->user_path), value);
	if (!strcmp(key, "mm_host"))
		return copy_value(c->mm_host, sizeof(c->mm_host), value);
	if (!strcmp(key, "app_version"))
		return copy_value(c->app_version, sizeof(c->app_version), value);
	if (!strcmp(key, "selftest_code"))
		return copy_value(c->selftest_code, sizeof(c->selftest_code), value);
	if (!strcmp(key, "lan_ip"))
		return so_parse_ipv4(value, &c->lan_ip);
	if (!strcmp(key, "log_host"))
		return so_parse_endpoint(value, &c->log_host, &c->log_port) == 0 ? 0 :
			so_parse_ipv4(value, &c->log_host);

#define RANGED(name, field, min, max) \
	if (!strcmp(key, name)) { \
		if (parse_ranged(value, min, max, &v) < 0) \
			return -1; \
		c->field = v; \
		return 0; \
	}
	RANGED("mm_port", mm_port, 1, 65535)
	RANGED("local_port", local_port, 0, 65535)
	RANGED("delay", delay, 1, 9)
	RANGED("log_packets", log_packets, 0, 1)
	RANGED("boot_quiet_s", boot_quiet_s, 0, 255)
	RANGED("status_beacon", status_beacon, 0, 1)
	RANGED("ack_delay_us", ack_delay_us, 0, 50000)
	RANGED("selftest", selftest, 0, 1)
	RANGED("selftest_frames", selftest_frames, 1, 36000)
	RANGED("selftest_allow_real_mm", selftest_allow_real_mm, 0, 1)
	RANGED("selftest_async_probe", selftest_async_probe, 0, 1)
	RANGED("selftest_stage_pause_s", selftest_stage_pause_s, 1, 120)
	RANGED("diag_step_pause_ms", diag_step_pause_ms, 0, 10000)
	RANGED("selftest_memory_walk", selftest_memory_walk, 0, 1)
	RANGED("diag_main_priority", diag_main_priority, 0, 0x7F)
	RANGED("online_main_priority", online_main_priority, 0, 0x7F)
	RANGED("online_loop_us", online_loop_us, 20, 5000)
	RANGED("chat", chat, 0, 1)
	RANGED("test_drop_pct", test_drop_pct, 0, 90)
#undef RANGED
	return -2;
}

static char *trim(char *s)
{
	char *end;
	while (*s == ' ' || *s == '\t')
		s++;
	end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
		*--end = '\0';
	return s;
}

void so_config_parse(SoConfig *config, const char *text, u32 len)
{
	char line[128];
	u32 pos = 0;
	while (pos < len) {
		u32 n = 0;
		char *key, *value, *eq;
		int res;
		while (pos < len && text[pos] != '\n') {
			if (n + 1 < sizeof(line))
				line[n++] = text[pos];
			pos++;
		}
		pos++;	/* '\n' */
		line[n] = '\0';
		if (line[0] == '#' || line[0] == ';')
			continue;
		key = trim(line);
		if (!*key)
			continue;
		eq = strchr(key, '=');
		if (!eq) {
			so_log("config: ignoring line without '=': %s", key);
			continue;
		}
		*eq = '\0';
		key = trim(key);
		value = trim(eq + 1);
		res = apply(config, key, value);
		if (res == -2)
			so_log("config: unknown setting '%s'", key);
		else if (res < 0)
			so_log("config: bad value for '%s'", key);
	}
}

int so_user_parse(SoUser *user, const char *text, u32 len)
{
	SoJson doc;
	memset(user, 0, sizeof(*user));
	if (so_json_parse(&doc, text, len) < 0 || so_json_type(&doc) != SO_JSON_OBJECT)
		return -1;
	so_json_get_string(&doc, "uid", user->uid, sizeof(user->uid), "");
	so_json_get_string(&doc, "playKey", user->play_key, sizeof(user->play_key), "");
	so_json_get_string(&doc, "connectCode", user->connect_code, sizeof(user->connect_code), "");
	so_json_get_string(&doc, "displayName", user->display_name, sizeof(user->display_name), "");
	so_json_get_string(&doc, "latestVersion", user->latest_version, sizeof(user->latest_version), "");
	if (!user->uid[0] || !user->play_key[0] || !strchr(user->connect_code, '#'))
		return -2;
	return 0;
}

int so_config_is_real_mm(const SoConfig *config)
{
	return strstr(config->mm_host, "slippi.gg") != NULL;
}

/* Test builds only (slippi-wii tools/sweep writes it, then restores the
 * empty default): settings compiled in, applied over config.ini, so an
 * automated test needs nothing on the SD card. */
static const char test_overrides[] =
#include "so_test_config.inc"
;

int so_config_load(SoConfig *config, SoUser *user)
{
	static char text[2048];
	int n, res;

	so_config_defaults(config);
	n = so_read_file(SO_CONFIG_PATH, text, sizeof(text) - 1);
	if (n >= 0)
		so_config_parse(config, text, (u32)n);
	if (test_overrides[0]) {
		so_config_parse(config, test_overrides, (u32)strlen(test_overrides));
		/* A test server never gets the player's own account. */
		if (!so_config_is_real_mm(config))
			strlcpy(config->user_path, SO_TEST_USER_PATH, sizeof(config->user_path));
		so_log("config: TEST BUILD: compiled-in settings over %s", SO_CONFIG_PATH);
	}

	memset(user, 0, sizeof(*user));
	n = so_read_file(config->user_path, text, sizeof(text) - 1);
	if (n < 0) {
		so_log("account: %s not found", config->user_path);
		return -1;
	}
	res = so_user_parse(user, text, (u32)n);
	memset(text, 0, sizeof(text));	/* do not leave the play key around */
	if (res < 0) {
		so_log("account: %s is not a valid Slippi user.json", config->user_path);
		return -2;
	}
	/* Never the play key itself. */
	so_log("account: %s (%s), play key %d chars", user->connect_code, user->display_name,
		(int)strlen(user->play_key));
	return 0;
}
