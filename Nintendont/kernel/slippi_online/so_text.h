/* slippi_online/so_text.h
 * Text the way Slippi Dolphin hands it to Melee:
 *   - game strings (ConvertStringForGame): Shift-JIS, ASCII punctuation in its
 *     full-width form, 2n+1 bytes for n characters;
 *   - connect codes (ConvertConnectCodeForGame): '#' full-width, 10 bytes;
 *   - premade text (SlippiPremadeText): Melee's text opcodes, used for the
 *     chat and logout dialogs.
 * The kernel has no CP932 table: characters outside ASCII are dropped (as
 * Dolphin drops characters its maps lack).
 */
#ifndef SO_TEXT_H
#define SO_TEXT_H

#include "global.h"

/* `utf8` cut to `n` characters, as 2n+1 bytes (zero-padded) in `out`. */
void so_game_string(const char *utf8, int n, u8 *out);

/* "ABCD#123" -> 10 bytes. */
void so_game_connect_code(const char *code, u8 out[10]);

enum {
	SO_TEXT_CHAT_P1 = 1,	/* .. P4 = 4: "<name>: <message>" */
	SO_TEXT_LOGOUT = 5,
	SO_TEXT_CHAT_DISABLED = 6,
};
#define SO_CHAT_DISABLED 0x10

/* Premade text `id` (with `name` and the chat message for `param` for the
 * chat ids); returns its length, or -1 if `out` is too small or the chat
 * message id is unknown. */
int so_premade_text(int id, u8 param, const char *name, u8 *out, int size);

/* The chat message for a pad-direction id ("ggs" for 0x88), or NULL. */
const char *so_chat_message_text(u8 id);

#endif
