"""Makes the rollback beta's zip: the app alone, to sit beside an installed SlippWii.

    python tools/make_beta.py 1.4.0-beta.3

The zip adds one folder to the SD card, apps/SlippWii-Rollback ("SlippWii Rollback (beta)" in the
Homebrew Channel). It uses the slippwii folder that SlippWii 1.3 installed, so nothing of Slippi's
is in it.

What is built, and how a beta differs from the tree as committed:

  - the kernel's compiled-in settings (Nintendont/kernel/slippi_online/so_test_config.inc) are
    "rollback=1" and nothing else: no log host, no test setting;
  - the online boot codes (Nintendont/kernel/gecko/g_online_boot.bin) are built with --rollback:
    the engine a player has (tools/build_online_boot.py);
  - tools/build.sh makes the loader with the kernel in it.

The repo's files are put back afterwards, the release's boot.dol among them (dist/sd/apps/SlippWii).
Writes build/release-beta/SlippWii-Rollback-vVERSION.zip and build/release-beta/pkg/ (the same,
unzipped). The tree must be clean (or --allow-dirty), and SLIPPWII_VERSION
(Nintendont/common/include/Metadata.h) must be VERSION: it is what the loader's screen and a log
on the card say.
"""
import argparse
import datetime
import hashlib
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NIN = ROOT / 'Nintendont'
INC = NIN / 'kernel/slippi_online/so_test_config.inc'
BOOT = NIN / 'kernel/gecko/g_online_boot.bin'
DOL = NIN / 'loader/loader.dol'
DIST = ROOT / 'dist/sd/apps/SlippWii/boot.dol'
OUT = ROOT / 'build/release-beta'
APP = 'apps/SlippWii-Rollback'

META = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<app version="1">
	<name>SlippWii Rollback (beta)</name>
	<coder>2gifts (Slippi Nintendont: FIX94, crediar, Fizzi, et al.)</coder>
	<version>{version}</version>
	<release_date>{date}</release_date>
	<short_description>Slippi Online with rollback netcode (beta)</short_description>
	<long_description>Beta: Slippi Online with rollback netcode, from a real Wii against PC players (Direct and Unranked). Needs SlippWii 1.3.0 or later installed: it uses the same slippwii folder. Needs a wired USB LAN adapter.

Unofficial and open source (GPLv2). Not affiliated with or endorsed by Project Slippi or Nintendo. Based on Slippi Nintendont.

https://github.com/2gifts/SlippWii</long_description>
	<no_ios_reload/>
	<ahb_access/>
</app>
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('version', help='e.g. 1.4.0-beta.3')
    ap.add_argument('--allow-dirty', action='store_true', help='build with uncommitted changes in the tree')
    args = ap.parse_args()
    v = args.version
    if not re.fullmatch(r'\d+\.\d+\.\d+-beta\.\d+', v):
        sys.exit('the version of a beta: X.Y.Z-beta.N')
    dirty = subprocess.run(['git', 'status', '--porcelain'], capture_output=True, text=True, cwd=ROOT).stdout.strip()
    if dirty and not args.allow_dirty:
        sys.exit(f'the tree is not clean (--allow-dirty to build anyway):\n{dirty}')
    meta_h = (NIN / 'common/include/Metadata.h').read_text(encoding='utf-8', errors='replace')
    have = re.search(r'#define SLIPPWII_VERSION\s+"([^"]*)"', meta_h).group(1)
    if have != v:
        sys.exit(f'SLIPPWII_VERSION is "{have}" (Nintendont/common/include/Metadata.h): it must be "{v}"')

    saved = {p: p.read_bytes() for p in (INC, BOOT, DOL, DIST) if p.exists()}
    try:
        INC.write_text('/* The rollback beta: rollback on, and nothing else (tools/make_beta.py) */\n"rollback=1\\n"\n')
        subprocess.run([sys.executable, str(ROOT / 'tools/build_online_boot.py'), '--rollback'], check=True, cwd=ROOT)
        codes = BOOT.read_bytes()
        r = subprocess.run(['sh', 'tools/build.sh'], capture_output=True, text=True, cwd=ROOT)
        if r.returncode:
            sys.exit(f'build failed:\n{r.stdout[-3000:]}{r.stderr[-3000:]}')
        dol = DOL.read_bytes()
    finally:
        for p, b in saved.items():
            p.write_bytes(b)

    pkg = OUT / 'pkg' / APP
    if (OUT / 'pkg').exists():
        shutil.rmtree(OUT / 'pkg')
    pkg.mkdir(parents=True)
    meta = META.format(version=v, date=f'{datetime.date.today():%Y%m%d}')
    icon = (ROOT / 'release/apps/SlippWii/icon.png').read_bytes()
    (pkg / 'boot.dol').write_bytes(dol)
    (pkg / 'meta.xml').write_text(meta, encoding='utf-8', newline='\n')
    (pkg / 'icon.png').write_bytes(icon)
    out = OUT / f'SlippWii-Rollback-v{v}.zip'
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr(f'{APP}/boot.dol', dol)
        z.writestr(f'{APP}/icon.png', icon)
        z.writestr(f'{APP}/meta.xml', meta)
    head = subprocess.run(['git', 'rev-parse', '--short', 'HEAD'], capture_output=True, text=True, cwd=ROOT).stdout.strip()
    print(f'{out} ({out.stat().st_size // 1024} KB), from {head}{" with uncommitted changes" if dirty else ""}\n'
          f'  zip sha256 {hashlib.sha256(out.read_bytes()).hexdigest()}\n'
          f'  boot.dol sha256 {hashlib.sha256(dol).hexdigest()} ({len(dol)} bytes)\n'
          f'  online boot codes {len(codes)} bytes; the repo\'s files are as they were')


if __name__ == '__main__':
    main()
