/* slippi_online/so_text.c
 * Game strings, connect codes and premade text (see so_text.h). Each follows
 * Slippi Dolphin: ConvertStringForGame / ConvertNarrowSpecialSHIFTJIS
 * (Common/StringUtil.cpp), ConvertConnectCodeForGame (EXI_DeviceSlippi.cpp),
 * SlippiPremadeText.h and CEXISlippi::loadPremadeText.
 */
#include "so_text.h"
#include "so_util.h"
#include "string.h"

/* ---- game strings -------------------------------------------------------- */

/* ASCII punctuation Melee only shows in its full-width Shift-JIS form. */
static u16 fullwidth(char c)
{
	static const char narrow[] = "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";
	static const u16 wide[] = {
		0x8149, 0x8168, 0x8194, 0x8190, 0x8193, 0x8195, 0x8166, 0x8169, 0x816a, 0x8196, 0x817b,
		0x8143, 0x817c, 0x8144, 0x815e, 0x8146, 0x8147, 0x8183, 0x8181, 0x8184, 0x8148, 0x8197,
		0x816d, 0x815f, 0x816e, 0x814f, 0x8151, 0x814d, 0x816f, 0x8162, 0x8170, 0x8160,
	};
	const char *hit = c ? strchr(narrow, c) : NULL;
	return hit ? wide[hit - narrow] : 0;
}

/* Length of the UTF-8 sequence starting with `lead`. */
static int utf8_length(u8 lead)
{
	if (lead < 0x80)
		return 1;
	if ((lead & 0xE0) == 0xC0)
		return 2;
	if ((lead & 0xF0) == 0xE0)
		return 3;
	if ((lead & 0xF8) == 0xF0)
		return 4;
	return 1;
}

void so_game_string(const char *utf8, int n, u8 *out)
{
	int size = 2 * n + 1, len = 0, chars = 0;
	const u8 *p = (const u8 *)utf8;
	memset(out, 0, size);
	while (*p && chars < n) {
		int step = utf8_length(*p);
		chars++;
		if (*p < 0x80) {
			u16 wide = fullwidth((char)*p);
			if (wide) {
				if (len + 2 > size - 1)
					break;
				out[len++] = (u8)(wide >> 8);
				out[len++] = (u8)wide;
			} else {
				if (len + 1 > size - 1)
					break;
				out[len++] = *p;
			}
		}
		/* Non-ASCII: no CP932 table in the kernel; dropped. */
		while (step-- && *p)
			p++;
	}
}

void so_game_connect_code(const char *code, u8 out[10])
{
	int len = 0;
	memset(out, 0, 10);
	for (; *code && len < 10; code++) {
		if (*code == '#') {
			if (len + 2 > 10)
				break;
			out[len++] = 0x81;
			out[len++] = 0x94;
		} else {
			out[len++] = (u8)*code;
		}
	}
}

/* ---- premade text --------------------------------------------------------- */

static const struct {
	u8 id;
	const char *text;
} chat_messages[] = {
	{ 0x88, "ggs" }, { 0x81, "one more" }, { 0x82, "brb" }, { 0x84, "good luck" },
	{ 0x18, "well played" }, { 0x11, "that was fun" }, { 0x12, "thanks" }, { 0x14, "too good" },
	{ 0x28, "sorry" }, { 0x21, "my b" }, { 0x22, "lol" }, { 0x24, "wow" },
	{ 0x48, "gotta go" }, { 0x41, "one sec" }, { 0x42, "lets play again later" }, { 0x44, "bad connection" },
	{ SO_CHAT_DISABLED, "player has chat disabled" },
};

const char *so_chat_message_text(u8 id)
{
	unsigned i;
	for (i = 0; i < sizeof(chat_messages) / sizeof(chat_messages[0]); i++)
		if (chat_messages[i].id == id)
			return chat_messages[i].text;
	return NULL;
}

static const char *const templates[] = {
	NULL,
	"<LEFT><KERN><COLOR, 229, 76, 76>%s:<S><COLOR, 255, 255, 255>%s<END>",
	"<LEFT><KERN><COLOR, 59, 189, 255>%s:<S><COLOR, 255, 255, 255>%s<END>",
	"<LEFT><KERN><COLOR, 255, 203, 4>%s:<S><COLOR, 255, 255, 255>%s<END>",
	"<LEFT><KERN><COLOR, 0, 178, 2>%s:<S><COLOR, 255, 255, 255>%s<END>",
	"<FIT><COLOR, 243, 75, 75>Are<S>You<COLOR, 0, 175, 75><S>Sure?<END>",
	"<LEFT><KERN><COLOR, 0, 178, 2>%s<S><COLOR, 255, 255, 255>has<S>chat<S>disabled<S><END>",
};

/* Melee's text opcodes with the parameter kinds Dolphin's CODES table gives
 * them ('b' u8, 's' u16). */
static const struct {
	const char *name;
	u8 opcode;
	const char *params;
} tags[] = {
	{ "CENTER", 0x10, "" }, { "/CENTER", 0x11, "" }, { "/COLOR", 0x0D, "" }, { "COLOR", 0x0C, "bbb" },
	{ "END", 0x00, "" }, { "FIT", 0x18, "" }, { "KERN", 0x16, "" }, { "LEFT", 0x12, "" },
	{ "BR", 0x03, "" }, { "/FIT", 0x19, "" }, { "/KERN", 0x17, "" }, { "OFFSET", 0x07, "ss" },
	{ "RESET", 0x01, "" }, { "/LEFT", 0x13, "" }, { "/RIGHT", 0x15, "" }, { "/SCALE", 0x0B, "" },
	{ "/TEXTBOX", 0x0F, "" }, { "SCALE", 0x0A, "bbbb" }, { "TEXTBOX", 0x0E, "ss" }, { "S", 0x1A, "" },
};

/* The ASCII part of Dolphin's 287-entry CHAR_MAP (its index is the text
 * byte's low part); everything else in the map is Japanese. */
static int char_index(char c)
{
	static const struct {
		char c;
		u16 index;
	} punctuation[] = {
		{ ',', 230 }, { '.', 231 }, { ':', 233 }, { ';', 234 }, { '?', 235 }, { '!', 236 },
		{ '^', 237 }, { '_', 238 }, { '/', 240 }, { '~', 241 }, { '|', 242 }, { '\'', 243 },
		{ '"', 244 }, { '(', 245 }, { ')', 246 }, { '[', 247 }, { ']', 248 }, { '{', 249 },
		{ '}', 250 }, { '+', 251 }, { '-', 252 }, { '=', 254 }, { '<', 255 }, { '>', 256 },
		{ '$', 258 }, { '%', 259 }, { '#', 260 }, { '&', 261 }, { '*', 262 }, { '@', 263 },
	};
	unsigned i;
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'A' && c <= 'Z')
		return 10 + c - 'A';
	if (c >= 'a' && c <= 'z')
		return 36 + c - 'a';
	for (i = 0; i < sizeof(punctuation) / sizeof(punctuation[0]); i++)
		if (punctuation[i].c == c)
			return punctuation[i].index;
	return -1;
}

typedef struct {
	u8 *out;
	int size, len, overflow;
} Out;

static void emit(Out *o, u8 b)
{
	if (o->len >= o->size) {
		o->overflow = 1;
		return;
	}
	o->out[o->len++] = b;
}

static char *trim(char *s)
{
	char *end;
	while (*s == ' ')
		s++;
	end = s + strlen(s);
	while (end > s && end[-1] == ' ')
		*--end = '\0';
	return s;
}

/* One token between '<' and '>' (or text between tags), as Dolphin's
 * GetPremadeTextData treats it: split on ',', a known tag name takes its
 * parameters, anything else is text (only up to its first ','). */
static int emit_token(Out *o, char *token)
{
	char *fields[6];
	int count = 0, i;
	char *p = token;
	unsigned t;
	/* split on ',', dropping empty pieces */
	while (*p && count < 6) {
		char *comma = strchr(p, ',');
		if (comma)
			*comma = '\0';
		if (*p)
			fields[count++] = p;
		if (!comma)
			break;
		p = comma + 1;
	}
	if (count == 0)
		return 0;
	for (t = 0; t < sizeof(tags) / sizeof(tags[0]); t++) {
		if (strcmp(tags[t].name, fields[0]) != 0)
			continue;
		if (count - 1 != (int)strlen(tags[t].params))
			return -1;
		emit(o, tags[t].opcode);
		for (i = 0; tags[t].params[i]; i++) {
			long v = so_parse_long(trim(fields[i + 1]), NULL);
			if (tags[t].params[i] == 'b') {
				emit(o, (u8)v);
			} else {
				emit(o, (u8)(v >> 8));
				emit(o, (u8)v);
			}
		}
		return 0;
	}
	for (p = fields[0]; *p; p++) {
		char c = *p;
		int index;
		/* the delimiters a name carried in disguise (loadPremadeText) */
		if (c == '\\')
			c = '<';
		else if (c == '`')
			c = '>';
		index = char_index(c);
		if (index >= 0) {
			emit(o, (u8)(0x20 | (index >> 8)));
			emit(o, (u8)index);
		}
	}
	return 0;
}

/* Spaces become <S>; '<' '>' ',' are disguised or removed, as
 * loadPremadeText does for player names. */
static void prepare_name(const char *in, char *out, int size)
{
	int n = 0;
	for (; *in && n < size - 4; in++) {
		char c = *in;
		if (c == '\\' || c == '`' || c == ',')
			continue;
		if (c == '<')
			c = '\\';
		else if (c == '>')
			c = '`';
		if (c == ' ') {
			memcpy(out + n, "<S>", 3);
			n += 3;
		} else {
			out[n++] = c;
		}
	}
	out[n] = '\0';
}

int so_premade_text(int id, u8 param, const char *name, u8 *out, int size)
{
	char text[400], arg_name[160], arg_msg[96];
	char *p, *gt;
	Out o;
	if (id < 1 || id > 6)
		return -1;
	if (id <= SO_TEXT_CHAT_P1 + 3) {
		const char *msg;
		prepare_name(name ? name : "", arg_name, sizeof(arg_name));
		if (param == SO_CHAT_DISABLED) {
			so_snprintf(text, sizeof(text), templates[SO_TEXT_CHAT_DISABLED], arg_name);
		} else {
			if (!(msg = so_chat_message_text(param)))
				return -1;
			prepare_name(msg, arg_msg, sizeof(arg_msg));
			so_snprintf(text, sizeof(text), templates[id], arg_name, arg_msg);
		}
	} else {
		strlcpy(text, templates[id], sizeof(text));
	}

	o.out = out;
	o.size = size;
	o.len = 0;
	o.overflow = 0;
	/* Split on '>', then each piece on '<'. */
	for (p = text; *p; p = gt + 1) {
		char *lt;
		gt = strchr(p, '>');
		if (gt)
			*gt = '\0';
		while ((lt = strchr(p, '<'))) {
			*lt = '\0';
			if (*p && emit_token(&o, p) < 0)
				return -1;
			p = lt + 1;
		}
		if (*p && emit_token(&o, p) < 0)
			return -1;
		if (!gt)
			break;
	}
	emit(&o, 0x00);	/* Dolphin always appends an end */
	return o.overflow ? -1 : o.len;
}
