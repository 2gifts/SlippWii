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
/* The game is out of step with an opponent's (so_exi.c, check_sums): this boot's last lines, and what
 * comes until that game is over (so_logfile_report_end), as a desync file. With the switch on, or a
 * crash's file open, they are in that file. */
void so_logfile_desync(void);
/* The game is over: a desync's file ends here (a later game's is a new one); a crash's goes on */
void so_logfile_report_end(void);
/* What is pending is written at the next service the card may be written at */
void so_logfile_now(void);
/* A problem, for the reports: `kind` one word (crash, hang, freeze, desync, lost, dropped, never-started,
 * connect, disc), `text` one line. Said in the log as "REPORT kind: text"; the first one before a file is
 * opened is that file's verdict (its first lines) and names it (kind-N.txt); each goes into
 * /slippwii/logs/reports.txt with the file it is in. Starts a report as so_logfile_desync does (written
 * when the card may be; ended at the game's end, or at once with `ended`). */
void so_logfile_event(const char *kind, const char *text, int ended);
/* Whether the card may be written now (no match, the disc idle): so_client.c's, called before each
 * write; none (tests): always. A crash's file is written whatever it says: that game has stopped. */
extern int (*so_logfile_quiet)(void);
/* The writes: from the kernel's low-priority watcher (so_diag.c), never the network thread's loop.
 * quiet: so_logfile_quiet's answer now; now_anyway: whatever is pending. Returns the bytes written. */
int so_logfile_service(int quiet, int now_anyway);
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
