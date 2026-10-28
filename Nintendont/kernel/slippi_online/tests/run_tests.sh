#!/bin/sh
# Builds and runs the slippi_online host unit tests with any C99 compiler:
#   sh run_tests.sh            (uses $CC, default gcc)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/.."
OUT="${TMPDIR:-/tmp}/slippi_online_tests"
mkdir -p "$OUT"
${CC:-gcc} -std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-parameter -Werror \
	-fsanitize=undefined -fsanitize-trap=undefined \
	-I"$HERE/shim" -I"$HERE" -I"$SRC" \
	"$HERE/test_main.c" "$HERE/host_platform.c" \
	"$SRC/so_util.c" "$SRC/so_json.c" "$SRC/so_alloc.c" "$SRC/so_config.c" "$SRC/so_pads.c" "$SRC/so_mm_msg.c" "$SRC/so_text.c" "$SRC/so_files.c" \
	-o "$OUT/test_main" 2>&1
"$OUT/test_main"
