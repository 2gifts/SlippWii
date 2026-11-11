"""Builds BearSSL, the TLS library of the loader's updater (Nintendont/loader/source/update.c).

    python tools/build_bearssl.py                 # for the Wii: build/bearssl/ppc/libbearssl.a
    python tools/build_bearssl.py --install       # and into Nintendont/loader/extlibs (lib and headers; committed)
    python tools/build_bearssl.py --host          # for this PC (the updater's tests): build/bearssl/host/

GitHub's servers take TLS 1.2 and later; the Wii's own SSL (IOS, /dev/net/ssl) is older. BearSSL
(https://bearssl.org, MIT licence, Thomas Pornin) is small, plain C, and needs no operating system:
the updater gives it a socket's read and write. The release's source is pinned by SHA-256 and kept
in build/bearssl-dl/; nothing of it is run, only compiled.

Built without an operating system's random numbers or clock (the updater seeds it and checks no
certificate dates: what it installs is verified by SlippWii's own signature, tools/sign_update.py).
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
URL = 'https://bearssl.org/bearssl-0.6.tar.gz'
SHA256 = '6705bba1714961b41a728dfc5debbe348d2966c117649392f8c8139efc83ff14'
DL = ROOT / 'build/bearssl-dl/bearssl-0.6.tar.gz'
SRC = ROOT / 'build/bearssl-src/bearssl-0.6'
PPC_BIN = ROOT / '.toolchain/opt/devkitpro/devkitPPC/bin'
EXTLIBS = ROOT / 'Nintendont/loader/extlibs'
# No system random numbers, no system clock: there is none to ask on a Wii.
CONFIG = ['-DBR_USE_URANDOM=0', '-DBR_USE_WIN32_RAND=0', '-DBR_USE_UNIX_TIME=0', '-DBR_USE_WIN32_TIME=0']


def source():
    if not SRC.is_dir():
        if not DL.exists():
            DL.parent.mkdir(parents=True, exist_ok=True)
            print(f'downloading {URL}')
            req = urllib.request.Request(URL, headers={'User-Agent': 'curl'})
            DL.write_bytes(urllib.request.urlopen(req, timeout=60).read())
        got = hashlib.sha256(DL.read_bytes()).hexdigest()
        if got != SHA256:
            sys.exit(f'{DL} is not BearSSL 0.6 as pinned (sha256 {got})')
        SRC.parent.mkdir(parents=True, exist_ok=True)
        with tarfile.open(DL) as t:
            for m in t.getmembers():  # (only plain files and folders under the archive's own folder)
                if not (m.isfile() or m.isdir()) or not m.name.startswith('bearssl-0.6/') or '..' in m.name.split('/'):
                    continue
                t.extract(m, SRC.parent)
    return sorted((SRC / 'src').rglob('*.c'))


def build(name, cc, ar, flags):
    out = ROOT / 'build/bearssl' / name
    obj = out / 'obj'
    obj.mkdir(parents=True, exist_ok=True)
    files = source()

    def one(c):
        o = obj / (c.stem + '.o')
        if o.exists() and o.stat().st_mtime > c.stat().st_mtime:
            return o, ''
        r = subprocess.run([*cc, *flags, *CONFIG, '-I', str(SRC / 'inc'), '-I', str(SRC / 'src'), '-c', str(c), '-o', str(o)],
                           capture_output=True, text=True)
        return o, (r.stdout + r.stderr if r.returncode else '')

    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        results = list(pool.map(one, files))
    bad = [(o, e) for o, e in results if e]
    if bad:
        sys.exit(f'{len(bad)} files did not compile; the first:\n{bad[0][1][-2000:]}')
    lib = out / 'libbearssl.a'
    if lib.exists():
        lib.unlink()
    # (a response file: 277 paths are more than a Windows command line takes)
    rsp = out / 'objects.rsp'
    rsp.write_text('\n'.join('"' + str(o).replace('\\', '/') + '"' for o, _ in results))
    subprocess.run([*ar, 'rcs', str(lib), f'@{rsp}'], check=True)
    print(f'{lib}: {len(results)} objects, {lib.stat().st_size // 1024} KB')
    return lib


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--install', action='store_true', help="copy the Wii's library and the headers into the loader's extlibs")
    ap.add_argument('--host', action='store_true', help='build for this PC instead')
    ap.add_argument('--cc', default=os.environ.get('CC', 'gcc'), help='the PC compiler for --host (e.g. "python -m ziglang cc")')
    ap.add_argument('--ar', default=os.environ.get('AR', ''), help='the PC archiver for --host (default: ar, or zig ar with a zig compiler)')
    args = ap.parse_args()
    if args.host:
        cc = args.cc.split()
        ar = args.ar.split() if args.ar else (cc[:-1] + ['ar'] if 'ziglang' in args.cc else ['ar'])
        build('host', cc, ar, ['-O2', '-std=c99'])
        return
    lib = build('ppc', [str(PPC_BIN / 'powerpc-eabi-gcc')], [str(PPC_BIN / 'powerpc-eabi-ar')],
                ['-O2', '-std=gnu99', '-DGEKKO', '-mrvl', '-mcpu=750', '-meabi', '-mhard-float',
                 '-ffunction-sections', '-fdata-sections'])
    if args.install:
        shutil.copy(lib, EXTLIBS / 'lib/libbearssl.a')
        for h in sorted((SRC / 'inc').glob('bearssl*.h')):
            shutil.copy(h, EXTLIBS / 'include' / h.name)
        shutil.copy(SRC / 'LICENSE.txt', EXTLIBS / 'include/bearssl_LICENSE.txt')
        print(f'installed into {EXTLIBS} (lib/libbearssl.a, include/bearssl*.h)')


if __name__ == '__main__':
    main()
