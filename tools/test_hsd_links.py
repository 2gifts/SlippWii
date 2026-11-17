"""Tests the resolving of Slippi's file links from the disc: the loader's C (Nintendont/loader/source/
hsd_links.c, built here for the PC) and the tools' Python (tools/hsd_links.py).

    python tools/test_hsd_links.py --iso Melee.iso [--cc "python -m ziglang cc"] [--slippi-hsd PATH]

Both must turn the two linked files of Slippi's development source (tools/slippi_files.py, NEXT_LINKED)
into exactly the bytes pinned there, which are what Slippi's own tool makes of them from an NTSC 1.02
disc (slippi-rust-extensions, `slippi-hsd resolve`; --slippi-hsd runs that tool too, when you have
built one). Also: every archive of the disc that the links name comes out of both unchanged when it
is only read and written again; and a link that leads nowhere, a missing disc file and a damaged
archive are refused, not crashed on.
"""
import argparse
import hashlib
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import hsd_links  # noqa: E402
import slippi_files  # noqa: E402

SOURCE = ROOT / 'Nintendont/loader/source'
DISC_FILES = ('GmTou1p.dat', 'GmTou1p.usd', 'GmTou2p.dat', 'GmTou2p.usd', 'IfAll.usd', 'IfVsCam.usd', 'MnMaAll.usd',
              'MnSlChr.usd', 'MnSlMap.usd')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--iso', required=True)
    ap.add_argument('--cc', default='gcc')
    ap.add_argument('--slippi-hsd', help="Slippi's own tool, to compare with as well")
    args = ap.parse_args()
    disc = slippi_files.open_disc(args.iso)
    failures = 0

    def check(ok, text):
        nonlocal failures
        failures += not ok
        print(('ok   ' if ok else 'FAIL ') + text)

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        exe = tmp / 'hsd_test.exe'
        subprocess.run([*args.cc.split(), '-std=gnu99', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-DHSD_STANDALONE',
                        '-fsanitize=undefined', '-fsanitize-trap=undefined', '-I', str(SOURCE),
                        str(ROOT / 'Nintendont/loader/tests/hsd/hsd_test.c'), str(SOURCE / 'hsd_links.c'), '-o', str(exe)],
                       check=True)
        files = tmp / 'disc'
        files.mkdir()
        for name in DISC_FILES:
            (files / name).write_bytes(disc.file(name))

        def c_resolve(path, out, folder=files):
            r = subprocess.run([str(exe), str(path), str(folder), str(out)], capture_output=True, text=True)
            return r.returncode, r.stdout.strip()

        for name, want in slippi_files.NEXT_LINKED.items():
            linked = slippi_files.fetch(f'GameFiles/GALE01/{name}', slippi_files.NEXT_GAME_FILES[name],
                                        slippi_files.CACHE / 'next/GameFiles' / name, slippi_files.NEXT_RAW)
            (tmp / name).write_bytes(linked)
            made, links, _ = hsd_links.resolve_file(linked, disc.file)
            check(hashlib.sha256(made).hexdigest() == want, f'{name}: Python resolves its {links} links to the pinned file')
            code, said = c_resolve(tmp / name, tmp / ('c-' + name))
            check(code == 0 and (tmp / ('c-' + name)).read_bytes() == made, f"{name}: the loader's C makes the same bytes ({said})")
            if args.slippi_hsd:
                subprocess.run([str(Path(args.slippi_hsd).resolve()), 'resolve', str(tmp / name), '--disc', str(files), '-o', str(tmp / ('r-' + name))],
                               check=True, capture_output=True)
                check((tmp / ('r-' + name)).read_bytes() == made, f"{name}: Slippi's own tool makes the same bytes")

        for name in DISC_FILES:
            raw = (files / name).read_bytes()
            check(hsd_links.serialize(hsd_links.parse(raw)) == raw, f'{name}: read and written again by Python, unchanged')
            code, said = c_resolve(files / name, tmp / 'same.bin')
            check(code == 0 and (tmp / 'same.bin').read_bytes() == raw, f'{name}: and by the C, unchanged')

        # what must be refused
        css = (tmp / 'SlippiCSS_gui.dat').read_bytes()
        empty = tmp / 'empty'
        empty.mkdir()
        code, said = c_resolve(tmp / 'SlippiCSS_gui.dat', tmp / 'x.bin', empty)
        check(code == 1 and 'not on the disc' in said, f'C: a disc without the files is refused ({said})')
        try:
            hsd_links.resolve_file(css, lambda n: None)
            check(False, 'Python: a disc without the files is refused')
        except hsd_links.LinkError as e:
            check('not on the disc' in str(e), f'Python: a disc without the files is refused ({e})')
        # (the other region's file: the same structure, another asset)
        other = tmp / 'other'
        other.mkdir()
        for name in DISC_FILES:
            twin = name[:-3] + ('dat' if name.endswith('usd') else 'usd')
            data = disc.file(twin)
            (other / name).write_bytes(data if data else disc.file(name))
        code, said = c_resolve(tmp / 'SlippiCSS_gui.dat', tmp / 'twin.bin', other)
        check(code != 0 or hashlib.sha256((tmp / 'twin.bin').read_bytes()).hexdigest() != slippi_files.NEXT_LINKED['SlippiCSS_gui.dat'],
              "C: another disc's files do not give the pinned file (the loader's CRC check then refuses them)")
        try:
            hsd_links.resolve_file(css, lambda n: (other / n).read_bytes())
            check(False, "Python: another disc's files are refused")
        except hsd_links.LinkError as e:
            check('not the original disc' in str(e), "Python: another disc's files are refused by the links' hashes")
        for at, what in ((0, 'its size word'), (9, 'its pointer count'), (len(css) // 2, 'a byte of its data'), (len(css) - 40, 'a name')):
            bad = bytearray(css)
            bad[at] ^= 0x40
            (tmp / 'bad.dat').write_bytes(bad)
            code, said = c_resolve(tmp / 'bad.dat', tmp / 'bad.bin')
            ok = code == 1 or (code == 0 and hashlib.sha256((tmp / 'bad.bin').read_bytes()).hexdigest() !=
                               slippi_files.NEXT_LINKED['SlippiCSS_gui.dat'])
            check(ok, f'C: a file damaged at {what} is refused or gives another result, without a crash (exit {code}: {said})')
        (tmp / 'short.dat').write_bytes(css[:1000])
        code, said = c_resolve(tmp / 'short.dat', tmp / 'short.bin')
        check(code == 1, f'C: a cut-off file is refused ({said})')
    print(f'{failures} failures')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
