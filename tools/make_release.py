"""Makes the SlippWii release zip: the files to put on the SD card.

    sh tools/build.sh                                          # dist/sd/apps/SlippWii/boot.dol
    python tools/make_release.py 1.1.0 --iso Melee.iso         # dist/SlippWii-v1.1.0.zip

The zip mirrors the SD card (extract it onto the card):

    apps/SlippWii/                    the app (Homebrew Channel)
    slippwii/netplay.gct              Slippi's online code (as Slippi Dolphin 3.6.4 runs it)
    slippwii/files/                   Slippi's own menu files
    slippwii/patches/*.diff           Slippi's patches for Melee's menu files, applied on
                                      the Wii to the player's disc (Nintendont/loader/
                                      source/slippwii.c); nothing of Melee's is shipped
    slippwii/patches/manifest.txt     what each file must be (size, CRC-32): the patches
                                      carry no checksums of their own
    Copy my Slippi account to SD.bat  copies user.json from Slippi Launcher (slippwii/
                                      copy-account.ps1)

The Slippi files come from Project Slippi's GitHub (pinned in
tools/slippi_files.py). --iso is your own NTSC 1.02 disc: the manifest needs
the patched files' sizes and CRC-32s, and only those are kept. The tree
must be clean and boot.dol built after the last commit, unless
--allow-dirty.
"""
import argparse
import datetime
import hashlib
import io
import re
import subprocess
import sys
import tempfile
import zipfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import slippi_files  # noqa: E402

APP = ROOT / 'dist/sd/apps/SlippWii'
REL = ROOT / 'release'

README = """SlippWii {version}: unofficial Slippi Online for the Wii
=========================================================

SlippWii is NOT made by, affiliated with or endorsed by Project Slippi or
Nintendo. It is free and open source (GPLv2): https://github.com/2gifts/SlippWii

You need: a Wii with the Homebrew Channel, a USB LAN adapter (wired internet;
Wi-Fi is too slow), your NTSC 1.02 Melee disc image (the same .iso Slippi uses;
put it where Nintendont finds it, e.g. SD:/games/), and a Slippi account
(log in once with Slippi Launcher on your PC: https://slippi.gg).

1. Extract this zip onto your Wii's SD card (it adds the apps and slippwii
   folders; nothing else is changed).
2. Double-click "Copy my Slippi account to SD.bat" on the SD card. It copies
   your Slippi login from Slippi Launcher. (macOS/Linux: see the README online.)
3. Put the card in the Wii, start SlippWii from the Homebrew Channel and pick
   Melee. The first start takes a minute: SlippWii makes Slippi's online menus
   from your disc. Then: Online Play > Direct or Unranked.

Slippi Nintendont, if you have it, keeps working as before.
Help and bug reports: https://github.com/2gifts/SlippWii
"""

NOTICE = """SlippWii {version} - what is in this folder

netplay.gct, files/ and patches/*.diff are Project Slippi's, unchanged except
that netplay.gct leaves out Slippi Dolphin's music hooks and "Normal Lag
Reduction" (Dolphin-only), as a console needs:
  - netplay.gct: Slippi's online code (slippi-ssbm-asm, GPLv3), from
    https://github.com/project-slippi/Ishiiruka ({stable}, Data/Sys/GameSettings/GALE01r2.ini)
  - files/, patches/: Slippi Dolphin's game files and patches (GPLv2+), from
    https://github.com/project-slippi/dolphin ({release}, Data/Sys/GameFiles/GALE01/)
SlippWii applies the patches on the Wii to your own Melee disc; no Melee
files are included. SlippWii itself is GPLv2 (LICENSE.txt); source:
https://github.com/2gifts/SlippWii
"""


def git(*args, cwd=ROOT):
    return subprocess.run(['git', *args], cwd=cwd, capture_output=True, text=True, check=True).stdout.strip()


def crlf(text):
    return text.replace('\r\n', '\n').replace('\n', '\r\n').encode('utf-8')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('version')
    ap.add_argument('--iso', required=True, help='your NTSC 1.02 Melee disc (only the patched files\' CRC-32s are kept)')
    ap.add_argument('--allow-dirty', action='store_true')
    args = ap.parse_args()
    v = args.version.lstrip('v')

    if not (APP / 'boot.dol').is_file():
        sys.exit(f'{APP / "boot.dol"} is missing: run sh tools/build.sh first')
    if not args.allow_dirty:
        dirty = git('status', '--porcelain', '--untracked-files=no')
        if dirty:
            sys.exit(f'uncommitted changes:\n{dirty}')
        if (APP / 'boot.dol').stat().st_mtime < int(git('log', '-1', '--format=%ct')):
            sys.exit('boot.dol is older than the last commit: run sh tools/build.sh')
    if f'SLIPPWII_VERSION\t"{v}"' not in (ROOT / 'Nintendont/common/include/Metadata.h').read_text():
        sys.exit(f'Nintendont/common/include/Metadata.h does not say SLIPPWII_VERSION "{v}"')

    meta = (REL / 'apps/SlippWii/meta.xml').read_text(encoding='utf-8')
    meta = re.sub(r'<version>[^<]*</version>', f'<version>{v}</version>', meta)
    meta = meta.replace('<release_date></release_date>',
                        f'<release_date>{datetime.date.today():%Y%m%d}000000</release_date>')

    # Slippi's files, and what the patched ones must come out as.
    ini = slippi_files.fetch(slippi_files.CODESET[0], slippi_files.CODESET[1],
                             slippi_files.CACHE / 'GALE01r2-stable.ini', slippi_files.STABLE_RAW).decode('utf-8')
    gct, _, _ = slippi_files.build_gct(ini)
    disc = slippi_files.open_disc(args.iso)
    shipped, manifest = {}, ['# SlippWii ' + v + ': what each file must be (kind, path in slippwii/, size, CRC-32).',
                             '# "patch": made on the Wii from the disc with patches/<name>.diff; "copy": shipped.']
    manifest.append(f'copy netplay.gct {len(gct)} {zlib.crc32(gct):08x}')
    shipped['slippwii/netplay.gct'] = gct
    for name, sha in slippi_files.GAME_FILES.items():
        data = slippi_files.fetch(f'GameFiles/GALE01/{name}', sha, slippi_files.CACHE / 'GameFiles' / name)
        if name.endswith('.diff'):
            target = name[:-5]
            made = slippi_files.decode(disc.file(target), data)
            manifest.append(f'patch files/{target} {len(made)} {zlib.crc32(made):08x}')
            shipped[f'slippwii/patches/{name}'] = data
        else:
            manifest.append(f'copy files/{name} {len(data)} {zlib.crc32(data):08x}')
            shipped[f'slippwii/files/{name}'] = data
    shipped['slippwii/patches/manifest.txt'] = ('\n'.join(manifest) + '\n').encode()

    out = ROOT / f'dist/SlippWii-v{v}.zip'
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('apps/SlippWii/boot.dol', (APP / 'boot.dol').read_bytes())
        z.writestr('apps/SlippWii/meta.xml', meta)
        z.writestr('apps/SlippWii/icon.png', (REL / 'apps/SlippWii/icon.png').read_bytes())
        for path, data in shipped.items():
            z.writestr(path, data)
        z.writestr('slippwii/copy-account.ps1', crlf((REL / 'slippwii/copy-account.ps1').read_text(encoding='utf-8')))
        z.writestr('slippwii/PUT user.json HERE.txt', crlf((REL / 'slippwii/PUT user.json HERE.txt').read_text(encoding='utf-8')))
        z.writestr('slippwii/NOTICE.txt', crlf(NOTICE.format(version=v, stable=slippi_files.STABLE_RAW.split('/')[-3],
                                                             release=slippi_files.RELEASE)))
        z.writestr('slippwii/LICENSE.txt', crlf((ROOT / 'LICENSE').read_text(encoding='utf-8')))
        z.writestr('Copy my Slippi account to SD.bat', crlf((REL / 'Copy my Slippi account to SD.bat').read_text(encoding='utf-8')))
        z.writestr('SlippWii README.txt', crlf(README.format(version=v)))
    sha = hashlib.sha256(out.read_bytes()).hexdigest()
    print(f'{out} ({out.stat().st_size // 1024} KB)\n  zip sha256 {sha}\n'
          f'  boot.dol sha256 {hashlib.sha256((APP / "boot.dol").read_bytes()).hexdigest()}\n'
          f'  source: SlippWii {git("rev-parse", "--short", "HEAD")}')


if __name__ == '__main__':
    main()
