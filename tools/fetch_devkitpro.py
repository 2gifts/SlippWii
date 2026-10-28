"""Fetch a portable devkitPro (devkitPPC + devkitARM + libogc) for Windows.

The versions match the image Slippi Nintendont's CI builds with
(nikhilnarayana/devkitpro-slippi, 2025-09): devkitPPC r47.1, devkitARM r66,
libogc 2.13.0. Each package is pinned by SHA-256 and unpacked into
.toolchain/ (nothing is installed system-wide), with MSYS2's `make`, which
runs in Git Bash. The project path has a space, which make cannot handle:
build from a drive letter (`subst W: <slippi-wii>`), see tools/env.sh.

    python tools/fetch_devkitpro.py
"""
import hashlib
import io
import tarfile
import urllib.request
from compression import zstd
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT/'.toolchain'
CACHE = OUT/'downloads'
UA = {'User-Agent': 'pacman/6.0.2 (Windows x86_64) libalpm/13.0.2'}
DKP = 'https://pkg.devkitpro.org/packages/'
PACKAGES = [
    (DKP + 'windows/x86_64/devkitPPC-r47.1-1-x86_64.pkg.tar.zst',
     'e862eb24f5d7172cf19da886983af7fd49b0067c575e391480a5709a796e7f65'),
    (DKP + 'windows/x86_64/devkitARM-r66-1-x86_64.pkg.tar.zst',
     'be0a5eb3435ac3988855048b7d1db0577e10a140ea79ec11edc7a47d65477779'),
    (DKP + 'devkitarm-crtls-1.2.6-1-any.pkg.tar.zst', '65faf10f62c219a1a8a3a182b0c4c56da7992599035ea32b3d79e39e79f964bb'),
    (DKP + 'devkitppc-rules-1.2.1-1-any.pkg.tar.zst', '0c3394da451c9dfb3b428d9d61b044eb1eafb947b2b5090ff88525f55c785278'),
    (DKP + 'devkitarm-rules-1.6.0-4-any.pkg.tar.zst', '35023b2b5f07a2dcfb600063854c712206341fb5df3ffbaf8ff825d3cbc15fcc'),
    (DKP + 'libogc-2.13.0-1-any.pkg.tar.zst', 'bc7ca41d08da15e7a7efef888572fbb767b96b21a214902f0641b5cf41acc6e1'),
    (DKP + 'libfat-ogc-2.1.0-1-any.pkg.tar.zst', '7f018172c6145db94a6f437a528662838e3bfd5dc3f8c29cb832e34ef2d9253b'),
    (DKP + 'windows/x86_64/gamecube-tools-1.0.7-1-x86_64.pkg.tar.zst',
     '7c68dba282b62f2a80dda94b6af1f7fef9f584648db1ae62e8f3d346492b569f'),
    (DKP + 'windows/x86_64/general-tools-1.4.4-1-x86_64.pkg.tar.zst',
     'a0fcea1a26c4f472c2ac98c409847dbec9d8e86ed82cd8b02131454f63d239f1'),
    (DKP + 'dkp-toolchain-vars-1.0.5-1-any.pkg.tar.zst', 'e6e26af3147061090572b61c576114ee9a45d5ed8c77c7d93cc5f5559198c288'),
]
MAKE = ('https://repo.msys2.org/msys/x86_64/make-4.4.1-3-x86_64.pkg.tar.zst',
        'af0bdba17f06fe037f0194069adaa31a8fe45f1a11381501896aea1fae37bd5d')


def fetch(url, sha256):
    path = CACHE/url.rsplit('/', 1)[1]
    if not path.exists() or hashlib.sha256(path.read_bytes()).hexdigest() != sha256:
        with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=600) as r:
            path.write_bytes(r.read())
    if hashlib.sha256(path.read_bytes()).hexdigest() != sha256:
        raise SystemExit(f'{path.name}: checksum mismatch')
    return path


def unpack(blob, dest):
    with tarfile.open(fileobj=io.BytesIO(zstd.decompress(blob))) as t:
        links = []
        for m in t.getmembers():
            if m.name.startswith('.') or not m.name:
                continue  # .PKGINFO, .MTREE, .INSTALL
            target = (dest/m.name).resolve()
            if not target.is_relative_to(dest.resolve()):
                raise ValueError(f'unsafe path {m.name}')
            if m.isdir():
                target.mkdir(parents=True, exist_ok=True)
            elif m.isfile():
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(t.extractfile(m).read())
            elif m.issym() or m.islnk():
                links.append((m, target))
        for m, target in links:  # Windows: copy the link's target file
            source = (target.parent/m.linkname) if m.issym() else dest/m.linkname
            if source.is_file():
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(source.read_bytes())


def main():
    CACHE.mkdir(parents=True, exist_ok=True)
    for url, sha256 in PACKAGES:
        unpack(fetch(url, sha256).read_bytes(), OUT)
        print('unpacked', url.rsplit('/', 1)[1])
    unpack(fetch(*MAKE).read_bytes(), OUT/'msys')
    print('unpacked', MAKE[0].rsplit('/', 1)[1])


if __name__ == '__main__':
    main()
