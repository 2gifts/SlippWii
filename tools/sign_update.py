"""The updater's manifest and the key that signs it.

    python tools/sign_update.py new-key                       # once: makes the release key, writes upd_key.h
    python tools/sign_update.py manifest ZIP 1.5.0 --note "What is new, a line"   # dist/update.txt
    python tools/sign_update.py verify dist/update.txt [--zip ZIP]
    python tools/sign_update.py public                        # the key's public half (upd_key.h again)

SlippWii's "Check for Updates" (Nintendont/loader/source/update.c) downloads update.txt from the
latest release on GitHub, and installs the zip it names only if the text is signed with the release
key and the zip has the SHA-256 it says. So a release has two files: the zip and its update.txt.

The key is ECDSA P-256. Its private half is one file on the release maker's PC, outside the repo
(SLIPPWII_UPDATE_KEY, or .slippwii/update-key.pem in the home folder): keep a copy somewhere safe and
give it to nobody. Without it no release can be signed, and installed copies fall back to updating
by SD card; with it, anyone could. The public half is compiled into the loader
(Nintendont/loader/source/update/upd_key.h). Uses the openssl program (Git for Windows has one).
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
KEY_H = ROOT / 'Nintendont/loader/source/update/upd_key.h'
HEAD = 'SlippWii update 1\n'
ZIP_URL = 'https://github.com/2gifts/SlippWii/releases/download/v{version}/SlippWii-v{version}.zip'
MANIFEST_MAX = 4096


def default_key():
    return Path(os.environ.get('SLIPPWII_UPDATE_KEY') or Path.home() / '.slippwii/update-key.pem')


def openssl():
    found = shutil.which('openssl')
    if found:
        return found
    for base in (os.environ.get('ProgramFiles', 'C:/Program Files'), os.environ.get('ProgramW6432', '')):
        for sub in ('Git/mingw64/bin/openssl.exe', 'Git/usr/bin/openssl.exe'):
            if base and (Path(base) / sub).exists():
                return str(Path(base) / sub)
    sys.exit('the openssl program is needed (Git for Windows has one: run this from Git Bash)')


def run(*args, data=None):
    r = subprocess.run([openssl(), *map(str, args)], input=data, capture_output=True)
    if r.returncode:
        sys.exit(f'openssl {args[0]} failed: {r.stderr.decode(errors="replace").strip()[-300:]}')
    return r.stdout


def new_key(path):
    """Makes a P-256 key at `path` (which must not exist)."""
    path = Path(path)
    if path.exists():
        sys.exit(f'{path} exists: a new key would make every installed SlippWii refuse the next release')
    path.parent.mkdir(parents=True, exist_ok=True)
    run('ecparam', '-name', 'prime256v1', '-genkey', '-noout', '-out', path)
    return path


def public_point(key):
    """The key's public half: 65 bytes, 04 then X then Y."""
    der = run('ec', '-in', key, '-pubout', '-outform', 'DER')
    point = der[-65:]
    if len(der) < 65 or point[0] != 4:
        sys.exit('not a P-256 key')
    return point


def write_key_h(point, note):
    rows = [', '.join(f'0x{b:02X}' for b in point[i:i + 12]) for i in range(0, 65, 12)]
    KEY_H.write_text(
        '/* loader/source/update/upd_key.h\n'
        f' * {note}\n'
        ' * Written by tools/sign_update.py: the public half of the key that signs a release\'s update.txt\n'
        ' * (ECDSA P-256: 04, X, Y). The private half is not in the repo. */\n'
        'static const unsigned char upd_release_key[65] = {\n\t' + ',\n\t'.join(rows) + ',\n};\n', newline='\n')


def sign(key, data):
    """The DER signature of `data` (ECDSA P-256 over its SHA-256)."""
    with tempfile.TemporaryDirectory() as d:
        f = Path(d) / 'data'
        f.write_bytes(data)
        return run('dgst', '-sha256', '-sign', key, f)


def manifest(key, version, zip_bytes, notes=(), url=None):
    """A release's update.txt, signed."""
    url = url or ZIP_URL.format(version=version)
    for line in (version, url, *notes):
        if any(ord(ch) < 0x20 or ord(ch) > 0x7E for ch in line):
            sys.exit(f'plain ASCII only: {line!r}')
    if len(notes) > 3 or any(len(n) > 63 for n in notes):
        sys.exit('at most 3 notes of at most 63 characters (what the screen shows)')
    body = (HEAD + f'version={version}\nzip={url}\nsize={len(zip_bytes)}\n'
            f'sha256={hashlib.sha256(zip_bytes).hexdigest()}\n' + ''.join(f'note={n}\n' for n in notes)).encode()
    text = body + b'sig=' + sign(key, body).hex().encode() + b'\n'
    if len(text) > MANIFEST_MAX:
        sys.exit('the manifest is too long')
    return text


def verify(text, point, zip_bytes=None):
    """What a manifest says if it is signed by the key with this public point; exits otherwise."""
    end = len(text) - (1 if text.endswith(b'\n') else 0)
    start = text.rfind(b'\n', 0, end) + 1
    if not text.startswith(HEAD.encode()) or not text[start:end].startswith(b'sig='):
        sys.exit('not a manifest')
    spki = bytes.fromhex('3059301306072a8648ce3d020106082a8648ce3d030107034200') + point
    with tempfile.TemporaryDirectory() as d:
        d = Path(d)
        (d / 'pub.der').write_bytes(spki)
        (d / 'sig').write_bytes(bytes.fromhex(text[start + 4:end].decode()))
        (d / 'body').write_bytes(text[:start])
        r = subprocess.run([openssl(), 'dgst', '-sha256', '-verify', str(d / 'pub.der'), '-keyform', 'DER',
                            '-signature', str(d / 'sig'), str(d / 'body')], capture_output=True)
    if r.returncode:
        sys.exit('the signature is not by this key')
    fields = dict(line.split('=', 1) for line in text[len(HEAD):start].decode().splitlines() if '=' in line)
    if zip_bytes is not None:
        if int(fields['size']) != len(zip_bytes) or fields['sha256'] != hashlib.sha256(zip_bytes).hexdigest():
            sys.exit('the manifest is for another zip')
    return fields


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--key', help='the private key file (default: SLIPPWII_UPDATE_KEY, or .slippwii/update-key.pem at home)')
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('new-key')
    sub.add_parser('public')
    m = sub.add_parser('manifest')
    m.add_argument('zip')
    m.add_argument('version')
    m.add_argument('--note', action='append', default=[])
    m.add_argument('--url', help='where the zip will be (default: the release of that version on GitHub)')
    m.add_argument('--out', help='default: update.txt beside the zip')
    v = sub.add_parser('verify')
    v.add_argument('manifest')
    v.add_argument('--zip')
    args = ap.parse_args()
    key = Path(args.key) if args.key else default_key()
    if args.cmd == 'new-key':
        new_key(key)
        write_key_h(public_point(key), 'SlippWii\'s release key.')
        print(f'the release key is {key}\nKeep a copy somewhere safe, and give it to nobody.\n{KEY_H} has its public half: commit it.')
        return
    if not key.exists():
        sys.exit(f'no key at {key} (new-key makes one; --key or SLIPPWII_UPDATE_KEY say where it is)')
    point = public_point(key)
    if args.cmd == 'public':
        write_key_h(point, 'SlippWii\'s release key.')
        print(point.hex())
    elif args.cmd == 'manifest':
        data = Path(args.zip).read_bytes()
        out = Path(args.out) if args.out else Path(args.zip).parent / 'update.txt'
        text = manifest(key, args.version, data, args.note, args.url)
        verify(text, point, data)
        out.write_bytes(text)
        print(f'{out} ({len(text)} bytes), for {Path(args.zip).name} ({len(data)} bytes)')
    elif args.cmd == 'verify':
        fields = verify(Path(args.manifest).read_bytes(), point, Path(args.zip).read_bytes() if args.zip else None)
        print('signed by this key:', fields)


if __name__ == '__main__':
    main()
