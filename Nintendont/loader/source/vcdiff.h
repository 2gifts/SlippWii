/* SlippWii: a minimal VCDIFF (RFC 3284) decoder for Slippi's menu file
 * patches (Slippi Dolphin's Data/Sys/GameFiles/GALE01/<name>.diff): the default
 * code table, no secondary compressor, and the open-vcdiff Adler-32 window
 * checksum, which is verified. Plain C, no allocation: also built on a PC
 * for its test (tools/test_vcdiff_c.py). Slippi's patches have no window
 * checksums, so a wrong source of the right size is not detected here: the
 * caller checks the result (vcdiff_crc32). */
#ifndef SLIPPWII_VCDIFF_H
#define SLIPPWII_VCDIFF_H

#include <stddef.h>

/* The target's size, or -1 (and *error) if the delta can't be read. */
long vcdiff_target_size(const unsigned char *delta, size_t delta_len, const char **error);

/* CRC-32 (zlib's), continuing from `crc` (0 to start). Slippi's patches
 * carry no checksums of their own, so the loader checks each patched file
 * against the release's manifest. */
unsigned long vcdiff_crc32(unsigned long crc, const unsigned char *p, size_t n);

/* Applies `delta` to `source` into `out` (vcdiff_target_size bytes). 0, or
 * -1 and *error: a damaged delta, or a source other than the one the delta
 * was made for (a window checksum differs). */
int vcdiff_decode(const unsigned char *source, size_t source_len, const unsigned char *delta, size_t delta_len,
	unsigned char *out, size_t out_len, const char **error);

#endif
