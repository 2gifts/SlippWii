/* slippi_online/so_util.h
 * Small, bounded string helpers for the kernel, which has no snprintf.
 */
#ifndef SO_UTIL_H
#define SO_UTIL_H

#include <stdarg.h>
#include "global.h"

/* snprintf subset: %d %i %u %x %X %c %s %p and %%, with flags '-' and '0',
 * a field width, a precision for %s (including "%.*s"), and the 'l' / 'll'
 * length modifiers. Always NUL-terminates when size > 0; returns the length
 * written (truncated output is not counted beyond size - 1). */
int so_vsnprintf(char *out, int size, const char *fmt, va_list ap);
int so_snprintf(char *out, int size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* Parses a decimal integer (optional sign; saturates on overflow); *end
 * receives the first unparsed character. Returns 0 when no digits are
 * present. */
long so_parse_long(const char *text, const char **end);

/* "a.b.c.d" -> IPv4 address in network byte order (big-endian, as the Wii
 * stores it). Returns 0 on success. */
int so_parse_ipv4(const char *text, u32 *addr);
/* "a.b.c.d:port", as matchmaking reports player addresses. Returns 0 on
 * success. */
int so_parse_endpoint(const char *text, u32 *addr, u16 *port);
/* IPv4 (network byte order) -> "a.b.c.d". */
void so_format_ipv4(u32 addr, char *out, int size);

/* Big-endian readers/writers for packet fields (SFML packet encoding). */
static inline void so_put_u16(u8 *p, u16 v) { p[0] = v >> 8; p[1] = v; }
static inline void so_put_u32(u8 *p, u32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static inline u16 so_get_u16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
static inline u32 so_get_u32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

#endif
