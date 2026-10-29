"""Runs the loader's SlippWii start-up (Nintendont/loader/source/slippwii.c)
on a PC, against the release zip's files on fake SD/USB folders: every disc
format, games on SD or USB, missing or broken accounts, a wrong disc,
incomplete files and the second start. The loader's libogc/FatFS/screen
calls are stand-ins (Nintendont/loader/tests/host/); the screen is printed.

    python tools/test_loader_slippwii.py --iso Melee.iso --zip dist/SlippWii-vX.Y.Z.zip [--cc gcc] [-v]
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import slippi_files  # noqa: E402

LOADER = ROOT / 'Nintendont/loader'
ACCOUNT = b'{"uid":"dummy-uid","playKey":"dummy-key","connectCode":"TEST#001","displayName":"Test Wii"}'
READY_TEXT = "getting Slippi's online menus ready"


class Fail(Exception):
    pass


def manifest(root):
    out = []
    for line in (root / 'slippwii/patches/manifest.txt').read_text().splitlines():
        if line and not line.startswith('#'):
            kind, path, size, crc = line.split()
            out.append((kind, path, int(size), int(crc, 16)))
    return out


def check_files(root, entries):
    for _, path, size, crc in entries:
        f = root / 'slippwii' / path
        if not f.exists():
            raise Fail(f'{f} is missing')
        data = f.read_bytes()
        if len(data) != size or zlib.crc32(data) != crc:
            raise Fail(f'{f} is wrong ({len(data)} bytes, {zlib.crc32(data):08x})')


def link_or_copy(src, dst):
    dst.parent.mkdir(parents=True, exist_ok=True)
    try:
        os.link(src, dst)
    except OSError:
        shutil.copyfile(src, dst)


def make_ciso(iso, out):
    """Nintendont's CISO: 2 MiB blocks, all-zero blocks left out."""
    bs = 0x200000
    flags = bytearray(0x8000 - 8)
    with open(iso, 'rb') as f, open(out, 'wb') as o:
        o.write(bytes(0x8000))
        i = 0
        while True:
            b = f.read(bs)
            if not b:
                break
            if b.strip(b'\0'):
                flags[i] = 1
                o.write(b.ljust(bs, b'\0'))
            i += 1
        o.seek(0)
        o.write(b'CISO' + struct.pack('<I', bs) + bytes(flags))


def make_fst(iso, folder, revision=None, game_id=None):
    """An extracted game: sys/boot.bin and the files the patches need."""
    disc = slippi_files.open_disc(iso)
    (folder / 'sys').mkdir(parents=True)
    head = bytearray(disc.read(0, 0x440))
    if revision is not None:
        head[7] = revision
    if game_id:
        head[:6] = game_id
    (folder / 'sys/boot.bin').write_bytes(head)
    (folder / 'files').mkdir()
    for name in slippi_files.GAME_FILES:
        if name.endswith('.diff'):
            (folder / 'files' / name[:-5]).write_bytes(disc.file(name[:-5]))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--iso', required=True)
    ap.add_argument('--zip', required=True)
    ap.add_argument('--cc', default='gcc')
    ap.add_argument('-v', action='store_true', help='print every screen')
    args = ap.parse_args()
    iso = Path(args.iso).resolve()
    failures = 0
    with tempfile.TemporaryDirectory(dir=iso.parent) as tmp:   # same drive: hard links to the ISO
        tmp = Path(tmp)
        exe = tmp / 'slippwii_host.exe'
        host = LOADER / 'tests/host'
        subprocess.run([args.cc, '-Wall', '-Werror', '-O1', '-I', str(host / 'include'), '-I', str(host),
                        '-I', str(LOADER / 'include'), '-I', str(LOADER / 'source'), str(host / 'loader_stubs.c'),
                        str(LOADER / 'source/slippwii.c'), str(LOADER / 'source/vcdiff.c'), '-o', str(exe)], check=True)
        ciso = tmp / 'melee.ciso'
        make_ciso(iso, ciso)

        def card(name, account=ACCOUNT, release=True):
            root = tmp / name
            root.mkdir()
            if release:
                zipfile.ZipFile(args.zip).extractall(root)
            if account is not None:
                (root / 'slippwii').mkdir(exist_ok=True)
                (root / 'slippwii/user.json').write_bytes(account)
            return root

        def run(game_path, sd, usb=None, game_dev='sd', answers='', cmd=0, drive=None):
            env = dict(os.environ, SW_SD=str(sd), SW_GAME_DEV=game_dev, SW_ANSWERS=answers)
            if usb:
                env['SW_USB'] = str(usb)
            if drive:
                env['SW_DRIVE'] = str(drive)
            res = subprocess.run([str(exe), game_path, str(cmd)], env=env, capture_output=True, text=True)
            out = res.stdout
            if args.v:
                print(out)
            return res.returncode, out

        def case(title, fn):
            nonlocal failures
            try:
                fn()
                print(f'ok   {title}')
            except Fail as e:
                print(f'FAIL {title}: {e}')
                failures += 1

        def expect(cond, text, out=''):
            if not cond:
                raise Fail(text + ('\n' + out if out else ''))

        def iso_on_sd():
            sd = card('sd1')
            link_or_copy(iso, sd / 'games/melee.iso')
            code, out = run('/games/melee.iso', sd)
            expect(code == 0 and READY_TEXT in out, 'did not make the files', out)
            expect('RESULT network=1 account=Slippi account: Test Wii (TEST#001)' in out, 'account line', out)
            check_files(sd, manifest(sd))
            expect((sd / 'slippwii/ready.txt').exists(), 'no ready.txt')
            code, out = run('/games/melee.iso', sd)
            expect(code == 0 and READY_TEXT not in out and 'network=1' in out, 'the second start did the work again', out)
            (sd / 'slippwii/files/MnMaAll.usd').unlink()
            code, out = run('/games/melee.iso', sd)
            expect(READY_TEXT in out, 'a deleted file was not made again', out)
            check_files(sd, manifest(sd))
        case('ISO on the SD card: files made and checked; second start skips; a deleted file is made again', iso_on_sd)

        def no_account(answer):
            def t():
                sd = card(f'noacct{answer}', account=None)
                link_or_copy(iso, sd / 'games/melee.iso')
                code, out = run('/games/melee.iso', sd, answers=answer)
                expect('no Slippi account on the SD card' in out and 'Copy my Slippi account to SD.bat' in out,
                       'no explanation', out)
                if answer == 'A':
                    expect(code == 0 and 'RESULT network=0' in out and READY_TEXT not in out, 'A: not offline', out)
                else:
                    expect(code == 3 and 'EXIT' in out, 'B: did not go back', out)
            return t
        case('no account, A: explained, Melee starts without online play', no_account('A'))
        case('no account, B: explained, back to the Homebrew Channel', no_account('B'))

        def bad_account():
            sd = card('badacct', account=b'{"connectCode":"X#1"}')
            link_or_copy(iso, sd / 'games/melee.iso')
            code, out = run('/games/melee.iso', sd, answers='A')
            expect('is not a Slippi account' in out and 'RESULT network=0' in out, 'not explained', out)
        case('a broken user.json: explained', bad_account)

        def ciso_case():
            sd = card('ciso')
            link_or_copy(ciso, sd / 'games/melee.ciso')
            code, out = run('/games/melee.ciso', sd)
            expect(code == 0 and 'network=1' in out, 'failed', out)
            check_files(sd, manifest(sd))
        case('CISO image', ciso_case)

        def fst_case():
            sd = card('fst')
            make_fst(iso, sd / 'games/GALE01')
            code, out = run('/games/GALE01/', sd)
            expect(code == 0 and 'network=1' in out, 'failed', out)
            check_files(sd, manifest(sd))
        case('extracted game (FST folder)', fst_case)

        def drive_case():
            sd = card('drive')
            code, out = run('di', sd, cmd=1, drive=iso)
            expect(code == 0 and 'network=1' in out, 'failed', out)
            check_files(sd, manifest(sd))
        case('a real disc in the drive', drive_case)

        def usb_case():
            sd = card('usb-sd')
            usb = tmp / 'usb'
            link_or_copy(iso, usb / 'games/melee.iso')
            code, out = run('/games/melee.iso', sd, usb=usb, game_dev='usb')
            expect(code == 0 and 'network=1' in out and 'TEST#001' in out, 'failed', out)
            check_files(usb, manifest(sd))
            expect((usb / 'slippwii/user.json').read_bytes() == ACCOUNT, 'the account was not put on the USB drive')
            expect(not (sd / 'slippwii/files/MnMaAll.usd').exists(), 'patched files written to the SD card')
            # a new account copied to the SD card later replaces the USB drive's
            new = ACCOUNT.replace(b'TEST#001', b'NEWW#002')
            (sd / 'slippwii/user.json').write_bytes(new)
            code, out = run('/games/melee.iso', sd, usb=usb, game_dev='usb')
            expect('NEWW#002' in out and (usb / 'slippwii/user.json').read_bytes() == new, 'the new account was not used', out)
        case('game on a USB drive: everything is put on the USB drive', usb_case)

        def wrong_disc():
            sd = card('wrong')
            make_fst(iso, sd / 'games/GALE01')
            f = sd / 'games/GALE01/files/SdMenu.usd'
            data = bytearray(f.read_bytes())
            for i in range(0, len(data), 97):
                data[i] ^= 0x55
            f.write_bytes(data)
            code, out = run('/games/GALE01/', sd, answers='A')
            expect('could not make' in out and 'SdMenu.usd' in out and 'NTSC 1.02' in out, 'not explained', out)
            expect('RESULT network=0' in out, 'A: not offline', out)
            expect(not (sd / 'slippwii/ready.txt').exists(), 'marked ready')
        case('a modded disc (a menu file changed): explained, no online play', wrong_disc)

        def revision():
            sd = card('rev1')
            make_fst(iso, sd / 'games/GALE01', revision=1)
            code, out = run('/games/GALE01/', sd, answers='A')
            expect('revision 1.01, not 1.02' in out and 'RESULT network=0' in out, 'not explained', out)
        case('Melee 1.01: explained', revision)

        def other_game():
            sd = card('other')
            make_fst(iso, sd / 'games/GALJ01', game_id=b'GALJ01')
            code, out = run('/games/GALJ01/', sd)
            expect(code == 0 and 'SCREEN' not in out and 'network=1' in out, 'touched another game', out)
        case('not NTSC Melee: left alone', other_game)

        def missing_patch():
            sd = card('nopatch')
            link_or_copy(iso, sd / 'games/melee.iso')
            (sd / 'slippwii/patches/MnSlMap.usd.diff').unlink()
            code, out = run('/games/melee.iso', sd, answers='A')
            expect('MnSlMap.usd.diff' in out and 'missing' in out and 'network=0' in out, 'not explained', out)
        case('a patch missing (incomplete copy): explained', missing_patch)

        def damaged_copy():
            sd = card('damaged')
            link_or_copy(iso, sd / 'games/melee.iso')
            f = sd / 'slippwii/files/slpCSS.dat'
            f.write_bytes(f.read_bytes()[:-10] + bytes(10))
            code, out = run('/games/melee.iso', sd, answers='A')
            expect('slpCSS.dat' in out and 'damaged' in out and 'network=0' in out, 'not explained', out)
        case('a shipped file damaged: explained', damaged_copy)

        def old_layout():
            sd = card('old', release=False)
            link_or_copy(iso, sd / 'games/melee.iso')
            code, out = run('/games/melee.iso', sd)
            expect(code == 0 and READY_TEXT not in out and 'TEST#001' in out, 'failed', out)
        case('no manifest (files made on a PC): used as they are', old_layout)

    print('PASS' if not failures else f'{failures} FAILED')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
