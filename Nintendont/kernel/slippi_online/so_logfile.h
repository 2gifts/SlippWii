/* The kernel's log on the SD card, for a problem report (so_logfile.c): a crash's
 * report always, the whole log with the loader's Diagnostic Log switch on. */
#ifndef SO_LOGFILE_H
#define SO_LOGFILE_H

#include "global.h"

/* Once, when the heap is there. diag: the Diagnostic Log switch. */
void so_logfile_init(int diag);
/* 1 while every line goes to a file (the switch is on, and the card takes it) */
int so_logfile_diag(void);
/* A line of the log (so_log gives each; so_log_private's are not given) */
void so_logfile_line(const char *line);
/* The game has crashed or hung: this boot's last lines, and what comes, as a crash file */
void so_logfile_crash(void);
/* What is pending is written at the next service, in a game or not */
void so_logfile_now(void);
/* The writes (the network thread: not the game's loop). in_game: only every 5 s
 * then; now_anyway: whatever is pending. Returns the bytes written. */
int so_logfile_service(int in_game, int now_anyway);
/* The file being written, or NULL */
const char *so_logfile_path(void);
/* The card's writes this boot, the longest of them, and all of them together */
void so_logfile_stats(u32 *writes, u32 *longest_us, u32 *all_us);
/* Takes out of a line, in place, what a person would not post: an IPv4
 * address's last two numbers, a connect code but for its first character.
 * Returns the line's new length. */
u32 so_logfile_scrub(char *line);
#ifdef SO_LOGFILE_TEST
void so_logfile_test_reset(void);
#endif

#endif
