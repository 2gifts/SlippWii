#!/bin/sh
# Builds and runs the slippi_online host unit tests with any C99 compiler:
#   sh run_tests.sh            (uses $CC, default gcc)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/.."
OUT="${TMPDIR:-/tmp}/slippi_online_tests"
mkdir -p "$OUT"
# Twice: as a build for Slippi 3.6.4 and as one for the version after it (SO_SLIPPI_NEXT, so_config.h).
for NEXT in 0 1; do
${CC:-gcc} -std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-parameter -Werror -DSO_LOGFILE_TEST -DSO_SLIPPI_NEXT=$NEXT \
	-fsanitize=undefined -fsanitize-trap=undefined \
	-I"$HERE/shim" -I"$HERE" -I"$SRC" \
	"$HERE/test_main.c" "$HERE/test_mq.c" "$HERE/test_next.c" "$HERE/host_platform.c" \
	"$SRC/so_util.c" "$SRC/so_json.c" "$SRC/so_alloc.c" "$SRC/so_config.c" "$SRC/so_pads.c" "$SRC/so_mm_msg.c" "$SRC/so_text.c" "$SRC/so_files.c" "$SRC/so_logfile.c" "$SRC/so_mq.c" "$SRC/so_stun.c" "$SRC/so_ws_frame.c" \
	-o "$OUT/test_main$NEXT" 2>&1
echo "== SO_SLIPPI_NEXT=$NEXT"
"$OUT/test_main$NEXT"
done
