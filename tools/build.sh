#!/bin/sh
# Build SlippWii (Slippi Nintendont with Slippi Online) from Git Bash.
# make cannot handle a space in the path ("Melee Decomp"), so such a checkout is
# built through the drive letter W: (subst). Usage: sh tools/build.sh [--sd E:]
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
case "$ROOT" in
*" "*)
	WIN="$(cygpath -w "$ROOT")"
	# (MSYS's argument conversion off: it mangles the quoted Windows path)
	if ! MSYS2_ARG_CONV_EXCL='*' cmd /c subst | tr -d '\r' | grep -qiFx "W:\\: => $WIN"; then
		if [ -d /w/Nintendont ]; then
			# never build (and clean) another checkout by mistake
			echo "W: is another checkout: free it with: subst W: /D" >&2
			exit 1
		fi
		MSYS2_ARG_CONV_EXCL='*' cmd /c subst W: "$WIN"
	fi
	ROOT=/w
	;;
esac
cd "$ROOT/Nintendont"
. "$ROOT/tools/env.sh"
make clean >/dev/null 2>&1 || true
# One job: the upstream Makefile can compile MeleeCodes.c while the gecko
# headers it includes are still being generated (a broken build, no error).
make windows=1 -j1 > "$ROOT/build.log" 2>&1 || { grep -n -i "error" "$ROOT/build.log" | head -20; exit 1; }
APP="$ROOT/dist/sd/apps/SlippWii"
mkdir -p "$APP"
cp loader/loader.dol "$APP/boot.dol"
cp "$ROOT/release/apps/SlippWii/meta.xml" "$ROOT/release/apps/SlippWii/icon.png" "$APP/"
echo "built: $(sha256sum loader/loader.dol | cut -c1-16)  -> $APP/boot.dol"
if [ "$1" = "--sd" ] && [ -n "$2" ]; then
	SD="/$(echo "$2" | tr -d ':' | tr 'A-Z' 'a-z')"
	mkdir -p "$SD/apps/SlippWii"
	cp "$APP"/* "$SD/apps/SlippWii/"
	powershell -Command "Write-VolumeCache $(echo "$2" | tr -d ':')"
	echo "copied to $2"
fi
