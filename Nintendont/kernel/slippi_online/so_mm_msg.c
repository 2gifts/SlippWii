/* slippi_online/so_mm_msg.c
 * Matchmaking messages (see so_mm.h): building create-ticket and reading
 * the server's replies, following SlippiMatchmaking::startMatchmaking and
 * handleMatchmaking.
 */
#include "so_mm.h"
#include "so_json.h"
#include "so_util.h"
#include "string.h"

int so_mm_build_ticket(char *out, int size, const SoConfig *config, const SoUser *user,
	const SoSearch *search, const char *lan_address)
{
	SoJsonWriter w;
	int i;
	/* Keys in sorted order, as nlohmann::json writes them. */
	so_jw_init(&w, out, size);
	so_jw_object_begin(&w);
	so_jw_key(&w, "appVersion");
	so_jw_string(&w, config->app_version);
	so_jw_key(&w, "ipAddressLan");
	so_jw_string(&w, lan_address);
	so_jw_key(&w, "search");
	so_jw_object_begin(&w);
	so_jw_key(&w, "connectCode");
	so_jw_array_begin(&w);
	for (i = 0; i < search->connect_code_len; i++)
		so_jw_long(&w, search->connect_code[i]);
	so_jw_array_end(&w);
	so_jw_key(&w, "mode");
	so_jw_long(&w, search->mode);
	so_jw_object_end(&w);
	so_jw_key(&w, "type");
	so_jw_string(&w, "create-ticket");
	so_jw_key(&w, "user");
	so_jw_object_begin(&w);
	so_jw_key(&w, "connectCode");
	so_jw_string(&w, user->connect_code);
	so_jw_key(&w, "displayName");
	so_jw_string(&w, user->display_name);
	so_jw_key(&w, "playKey");
	so_jw_string(&w, user->play_key);
	so_jw_key(&w, "uid");
	so_jw_string(&w, user->uid);
	so_jw_object_end(&w);
	so_jw_object_end(&w);
	return so_jw_finish(&w);
}

/* Parses the reply and checks its type; on failure sets `error`. */
static int open_reply(const char *text, u32 len, const char *type, SoJson *doc, char *error, int error_size,
	const char *invalid_text)
{
	char got[32];
	if (so_json_parse(doc, text, len) < 0 || so_json_type(doc) != SO_JSON_OBJECT) {
		strlcpy(error, invalid_text, error_size);
		return -1;
	}
	so_json_get_string(doc, "type", got, sizeof(got), "");
	if (strcmp(got, type) != 0) {
		strlcpy(error, invalid_text, error_size);
		return -1;
	}
	so_json_get_string(doc, "error", error, error_size, "");
	return error[0] ? -2 : 0;
}

int so_mm_parse_ticket_reply(const char *text, u32 len, char *error, int error_size)
{
	SoJson doc;
	return open_reply(text, len, "create-ticket-resp", &doc, error, error_size,
		"Invalid response when joining mm queue");
}

/* "a.b.c.d:port" -> the part before the colon. */
static void host_part(const char *address, char *out, int size)
{
	const char *colon = strchr(address, ':');
	int n = colon ? (int)(colon - address) : (int)strlen(address);
	if (n >= size)
		n = size - 1;
	memcpy(out, address, n);
	out[n] = '\0';
}

static void read_player(const SoJson *el, SoMmPlayer *p)
{
	SoJson chat, item;
	int i;
	memset(p, 0, sizeof(*p));
	p->is_local = (u8)so_json_get_bool(el, "isLocalPlayer", 0);
	so_json_get_string(el, "uid", p->uid, sizeof(p->uid), "");
	so_json_get_string(el, "displayName", p->display_name, sizeof(p->display_name), "");
	so_json_get_string(el, "connectCode", p->connect_code, sizeof(p->connect_code), "");
	p->port = (u8)so_json_get_long(el, "port", 0);
	p->is_bot = (u8)so_json_get_bool(el, "isBot", 0);
	/* Dolphin keeps a player's chat messages only when there are exactly 16. */
	if (so_json_get(el, "chatMessages", &chat) == 0 && so_json_count(&chat) == SO_CHAT_MESSAGES) {
		p->has_chat_messages = 1;
		for (i = 0; i < SO_CHAT_MESSAGES; i++) {
			so_json_at(&chat, i, &item);
			if (so_json_string(&item, p->chat_messages[i], SO_CHAT_TEXT_MAX) < 0)
				p->has_chat_messages = 0;
		}
	}
}

int so_mm_parse_match(const char *text, u32 len, SoMatchResult *result, char *error, int error_size,
	char *latest_version, int latest_version_size)
{
	static const u16 default_stages[] = { 0x3, 0x8, 0x1C, 0x1F, 0x20 };
	SoJson doc, players, stages, el;
	char local_external[24] = "", address[32], host[24];
	int i, n, res, found_local = 0;

	memset(result, 0, sizeof(*result));
	latest_version[0] = '\0';
	res = open_reply(text, len, "get-ticket-resp", &doc, error, error_size,
		"Invalid response when getting mm status");
	if (res == -2)
		so_json_get_string(&doc, "latestVersion", latest_version, latest_version_size, "");
	if (res < 0)
		return res;

	so_json_get_string(&doc, "matchId", result->match_id, sizeof(result->match_id), "");

	/* Players, then the local player's external IP. */
	if (so_json_get(&doc, "players", &players) == 0) {
		n = so_json_count(&players);
		if (n > SO_MAX_PLAYERS) {
			strlcpy(error, "Too many players in match", error_size);
			return -3;
		}
		for (i = 0; i < n; i++) {
			SoMmPlayer *p = &result->players[i];
			so_json_at(&players, i, &el);
			read_player(&el, p);
			if (p->is_local) {
				found_local = 1;
				so_json_get_string(&el, "ipAddress", address, sizeof(address), "1.1.1.1:123");
				host_part(address, local_external, sizeof(local_external));
				result->local_player_index = (u8)(p->port - 1);
			}
		}
		result->player_count = (u8)n;
		if (!found_local || result->local_player_index >= n) {
			strlcpy(error, "Invalid response when getting mm status", error_size);
			return -5;
		}

		/* Where to reach each remote player: its LAN address when we share
		 * an external IP (same LAN, VPN node or CGNAT), else its external
		 * address. */
		for (i = 0; i < n; i++) {
			SoMmPlayer *p = &result->players[i];
			char lan[32];
			if (p->port - 1 == result->local_player_index)
				continue;
			so_json_at(&players, i, &el);
			so_json_get_string(&el, "ipAddress", address, sizeof(address), "1.1.1.1:123");
			so_json_get_string(&el, "ipAddressLan", lan, sizeof(lan), "1.1.1.1:123");
			host_part(address, host, sizeof(host));
			if (strcmp(host, local_external) == 0 && lan[0])
				strlcpy(address, lan, sizeof(address));
			if (so_parse_endpoint(address, &p->addr, &p->udp_port) < 0) {
				strlcpy(error, "Invalid player address from mm server", error_size);
				return -4;
			}
		}
	}
	if (result->player_count < 2) {
		strlcpy(error, "Invalid response when getting mm status", error_size);
		return -5;
	}

	result->is_host = (u8)so_json_get_bool(&doc, "isHost", 0);

	if (so_json_get(&doc, "stages", &stages) == 0) {
		n = so_json_count(&stages);
		for (i = 0; i < n && result->stage_count < SO_MAX_STAGES; i++) {
			long id;
			so_json_at(&stages, i, &el);
			if (so_json_long(&el, &id) == 0)
				result->stages[result->stage_count++] = (u16)id;
		}
	}
	if (result->stage_count == 0) {
		/* Dolphin's fallback; FoD only in singles. */
		for (i = 0; i < (int)(sizeof(default_stages) / sizeof(default_stages[0])); i++)
			result->stages[result->stage_count++] = default_stages[i];
		if (result->player_count == 2)
			result->stages[result->stage_count++] = 0x2;
	}

	result->items = (u32)so_json_get_long(&doc, "items", 0);
	return 0;
}
