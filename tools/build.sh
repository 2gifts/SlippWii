#!/bin/sh
# Build SlippWii (Slippi Nintendont with Slippi Online) from Git Bash.
# make cannot handle the space in "Melee Decomp", so the project is built
# through the drive letter W: (subst). Usage: sh tools/build.sh [--sd E:]
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd -W 2>/dev/null || pwd)"
if [ ! -d /w/Nintendont ]; then
	cmd //c "subst W: \"$(cygpath -w "$HERE")\"" || true
fi
cd /w/Nintendont
. /w/tools/env.sh
make clean >/dev/null 2>&1 || true
# One job: the upstream Makefile can compile MeleeCodes.c while the gecko
# headers it includes are still being generated (a broken build, no error).
make windows=1 -j1 > /w/build.log 2>&1 || { grep -n -i "error" /w/build.log | head -20; exit 1; }
APP="/w/dist/sd/apps/SlippWii"
mkdir -p "$APP"
cp loader/loader.dol "$APP/boot.dol"
cp /w/release/apps/SlippWii/meta.xml /w/release/apps/SlippWii/icon.png "$APP/"
echo "built: $(sha256sum loader/loader.dol | cut -c1-16)  -> $APP/boot.dol"
if [ "$1" = "--sd" ] && [ -n "$2" ]; then
	SD="/$(echo "$2" | tr -d ':' | tr 'A-Z' 'a-z')"
	mkdir -p "$SD/apps/SlippWii"
	cp "$APP"/* "$SD/apps/SlippWii/"
	powershell -Command "Write-VolumeCache $(echo "$2" | tr -d ':')"
	echo "copied to $2"
fi
