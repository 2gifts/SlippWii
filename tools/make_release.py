"""Makes the SlippWii release zip from the committed tree and the built app.

    sh tools/build.sh                     # dist/sd/apps/SlippWii/boot.dol
    python tools/make_release.py 0.1.0    # dist/SlippWii-v0.1.0.zip

The zip holds "Install SlippWii.bat" (Windows), installer/install.py (any
system with Python 3), the app and the licence. It refuses a dirty tree
(either repository) or an app built before the last commit, unless
--allow-dirty.
"""
import argparse
import datetime
import hashlib
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / 'dist/sd/apps/SlippWii'
INSTALLER = ['install.py', 'slippi_files.py', 'vcdiff.py', 'start.ps1']

README = """SlippWii {version}: unofficial Slippi Online for the Wii
=========================================================

SlippWii is NOT made by, affiliated with or endorsed by Project Slippi or
Nintendo. It is free and open source (GPLv2):
https://github.com/2gifts/SlippWii

You need: a Wii with the Homebrew Channel, a USB LAN adapter (wired
internet; Wi-Fi is too slow), its SD card, your NTSC 1.02 Melee disc image
(.iso) and a Slippi account (log in once with Slippi Launcher, https://slippi.gg).

Windows
  1. Put the Wii's SD card in this PC.
  2. Double-click "Install SlippWii.bat" and answer its questions.
     It finds the SD card, your Melee .iso and your Slippi account by
     itself, downloads Slippi's files from Project Slippi and writes
     everything to the card.

macOS / Linux
  python3 installer/install.py

Then put the card in the Wii, open the Homebrew Channel and start SlippWii.
Slippi Nintendont, if you have it, stays installed and works as before.

Help, known issues and bug reports:
https://github.com/2gifts/SlippWii
"""


def git(*args, cwd=ROOT):
    return subprocess.run(['git', *args], cwd=cwd, capture_output=True, text=True, check=True).stdout.strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('version')
    ap.add_argument('--allow-dirty', action='store_true')
    args = ap.parse_args()
    v = args.version.lstrip('v')

    if not (APP / 'boot.dol').is_file():
        sys.exit(f'{APP / "boot.dol"} is missing: run sh tools/build.sh first')
    if not args.allow_dirty:
        for repo in (ROOT, ROOT / 'Nintendont'):
            dirty = git('status', '--porcelain', '--untracked-files=no', cwd=repo)
            if dirty:
                sys.exit(f'{repo} has uncommitted changes:\n{dirty}')
        built = (APP / 'boot.dol').stat().st_mtime
        last = int(git('log', '-1', '--format=%ct', cwd=ROOT / 'Nintendont'))
        if built < last:
            sys.exit('boot.dol is older than the last Nintendont commit: run sh tools/build.sh')
    metadata = (ROOT / 'Nintendont/common/include/Metadata.h').read_text()
    if f'SLIPPWII_VERSION\t"{v}"' not in metadata:
        sys.exit(f'Nintendont/common/include/Metadata.h does not say SLIPPWII_VERSION "{v}"')

    meta = (ROOT / 'release/apps/SlippWii/meta.xml').read_text(encoding='utf-8')
    meta = meta.replace('<version>0.1.0</version>', f'<version>{v}</version>')
    meta = meta.replace('<release_date></release_date>',
                        f'<release_date>{datetime.date.today():%Y%m%d}000000</release_date>')

    out = ROOT / f'dist/SlippWii-v{v}.zip'
    top = f'SlippWii-v{v}/'
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr(top + 'Install SlippWii.bat', (ROOT / 'installer/Install SlippWii.bat').read_bytes())
        z.writestr(top + 'README.txt', README.format(version=v).replace('\n', '\r\n'))
        z.writestr(top + 'LICENSE.txt', (ROOT / 'LICENSE').read_bytes())
        for name in INSTALLER:
            z.writestr(top + 'installer/' + name, (ROOT / 'installer' / name).read_bytes())
        z.writestr(top + 'installer/VERSION', v + '\n')
        z.writestr(top + 'installer/app/SlippWii/boot.dol', (APP / 'boot.dol').read_bytes())
        z.writestr(top + 'installer/app/SlippWii/icon.png', (ROOT / 'release/apps/SlippWii/icon.png').read_bytes())
        z.writestr(top + 'installer/app/SlippWii/meta.xml', meta)
    sha = hashlib.sha256(out.read_bytes()).hexdigest()
    dol = hashlib.sha256((APP / 'boot.dol').read_bytes()).hexdigest()
    print(f'{out} ({out.stat().st_size // 1024} KB)\n  zip sha256 {sha}\n  boot.dol sha256 {dol}\n'
          f'  source: SlippWii {git("rev-parse", "--short", "HEAD")}')


if __name__ == '__main__':
    main()
