/* Test controls for host_platform.c. */
#ifndef HOST_PLATFORM_H
#define HOST_PLATFORM_H

#include "global.h"

void test_set_time_us(u64 us);
void test_advance_us(u64 us);
void test_log_echo(int on);
const char *test_last_log(void);
/* The disc so_disc_read reads (NULL: none). */
void test_set_disc(const u8 *disc, u32 len);
/* The files under /slippwii/logs are kept in memory: one's bytes (NULL: no such file), and all gone */
const char *test_log_file(const char *path, u32 *len);
void test_log_files_reset(void);
extern int host_diag_log;	/* what so_diag_log_setting says */
extern int host_log_files_fail;	/* writes there fail (no card) */

#endif
