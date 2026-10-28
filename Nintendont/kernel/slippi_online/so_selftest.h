/* slippi_online/so_selftest.h
 * Hardware self-test, enabled with selftest=1 in config.ini: a Direct
 * search for selftest_code, then a synthetic lockstep match of
 * selftest_frames frames at 59.94 Hz against a peer that uses the same test
 * pattern (the PC test peer), checking every pad that arrives. Results go to
 * the log (and the PC log sink).
 *
 * It refuses Slippi's real server unless selftest_allow_real_mm=1.
 */
#ifndef SO_SELFTEST_H
#define SO_SELFTEST_H

#include "global.h"
#include "so_config.h"

void so_selftest_start(const SoConfig *config);
/* -1 while running (or not started), 0 failed, 1 passed. */
int so_selftest_result(void);

/* The deterministic test pad for `player_idx` at `frame` (8 wire bytes),
 * and the fake checksum of a frame; shared with the PC test tools. */
void so_test_pad(int player_idx, s32 frame, u8 out[8]);
u32 so_test_checksum(s32 frame);

/* "ABCD#123" -> full-width Shift-JIS, as the name entry screen sends a
 * connect code. Returns the byte count, or -1 for an invalid character. */
int so_encode_connect_code(const char *code, u8 *out, int size);

#endif
