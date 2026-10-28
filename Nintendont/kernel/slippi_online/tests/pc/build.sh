#!/bin/sh
# Builds the PC test versions of the Wii client (Win32; any MinGW gcc):
#   sh build.sh   -> $OUT/so_pc_selftest.exe (the self-test)
#                    $OUT/so_pc_game.exe     (fake Melee driving the EXI device)
#                    $OUT/so_pc_opponent.exe (an opponent to play the Wii by hand)
# (default OUT: a build dir under TMPDIR)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../.."
OUT="${OUT:-${TMPDIR:-/tmp}/slippi_online_pc}"
mkdir -p "$OUT"
build() {
	main="$1"
	exe="$2"
	${CC:-gcc} -std=gnu99 -O2 -g -Wall -Wno-unused-parameter -DENET_IOS \
		-I"$HERE" -I"$HERE/../shim" -I"$SRC" -I"$SRC/enet/include" \
		"$main" "$HERE/so_platform_win32.c" "$HERE/so_socket_win32.c" \
		"$SRC/so_util.c" "$SRC/so_alloc.c" "$SRC/so_json.c" "$SRC/so_config.c" "$SRC/so_pads.c" \
		"$SRC/so_mm_msg.c" "$SRC/so_mm.c" "$SRC/so_netplay.c" "$SRC/so_client.c" "$SRC/so_selftest.c" \
		"$SRC/so_diag.c" "$SRC/so_exi.c" "$SRC/so_text.c" "$SRC/so_files.c" \
		"$SRC"/enet/*.c \
		-lws2_32 -lwinmm -o "$exe"
	echo "$exe"
}
build "$HERE/pc_main.c" "$OUT/so_pc_selftest.exe"
build "$HERE/pc_game.c" "$OUT/so_pc_game.exe"
build "$HERE/pc_opponent.c" "$OUT/so_pc_opponent.exe"
