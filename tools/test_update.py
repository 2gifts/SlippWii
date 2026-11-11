"""Tests the updater's core on this PC (Nintendont/loader/source/update/: the manifest and its signature,
the HTTPS client, the zip, the files' way onto the card).

    python tools/test_update.py [--cc "python -m ziglang cc"] [--live] [-v]

The same C the Wii runs is built as a PC program (Nintendont/loader/tests/update/) with BearSSL
(tools/build_bearssl.py --host builds it if it is not there). A web server on this PC plays GitHub:
redirects, chunked answers, broken downloads. A folder plays the SD card, with writes and renames
made to fail on demand. The signing key is made for the run and thrown away.

--live also downloads SlippWii 1.4.0's zip from GitHub over TLS and checks its SHA-256 (needs the
internet).
"""
import argparse
import hashlib
import http.server
import io
import os
import random
import shutil
import subprocess
import sys
import tempfile
import threading
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import sign_update  # noqa: E402

SRC = ROOT / 'Nintendont/loader/source/update'
TESTS = ROOT / 'Nintendont/loader/tests/update'
BEAR = ROOT / 'build/bearssl'
OUT = ROOT / 'build/update-test'
LIVE_ZIP = 'https://github.com/2gifts/SlippWii/releases/download/v1.4.0/SlippWii-v1.4.0.zip'
LIVE_SHA = '0a1df02af94873a6fb36eaf2702e646fb8640e13e14c7cc3fe0d45aa4f0ec7c8'

failures = 0
verbose = False


def check(name, ok, detail=''):
    global failures
    if not ok:
        failures += 1
    if verbose or not ok:
        print(f'{"ok  " if ok else "FAIL"} {name}{"" if ok else ": " + str(detail)[:300]}')


def build(cc):
    lib = BEAR / 'host/libbearssl.a'
    if not lib.exists():
        subprocess.run([sys.executable, str(ROOT / 'tools/build_bearssl.py'), '--host', '--cc', cc], check=True)
    OUT.mkdir(parents=True, exist_ok=True)
    exe = OUT / ('upd_test.exe' if os.name == 'nt' else 'upd_test')
    cmd = [*cc.split(), '-O1', '-Wall', '-I', str(SRC), '-I', str(TESTS), '-I', str(ROOT / 'build/bearssl-src/bearssl-0.6/inc'),
           *(str(SRC / f) for f in ('upd_core.c', 'upd_http.c', 'puff.c')), str(TESTS / 'upd_host.c'), str(TESTS / 'upd_test.c'),
           str(lib), *(['-lws2_32'] if os.name == 'nt' else []), '-o', str(exe)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f'the test program did not build:\n{r.stdout}{r.stderr}')
    return exe


def run(exe, *args, env=None):
    r = subprocess.run([str(exe), *map(str, args)], capture_output=True, text=True, env={**os.environ, **(env or {})}, timeout=120)
    return r.stdout.strip()


def release_zip(seed, extra=()):
    """A zip laid out as a release's (tools/make_release.py), its files' contents by `seed`."""
    rnd = random.Random(seed)
    files = {
        'apps/SlippWii/boot.dol': rnd.randbytes(300_000) + bytes(100_000) + b'boot' * 20_000,
        'apps/SlippWii/meta.xml': f'<app><version>{seed}</version></app>'.encode(),
        'apps/SlippWii/icon.png': rnd.randbytes(4000),
        'slippwii/netplay.gct': rnd.randbytes(57_000),
        'slippwii/files/MxDb.dat': rnd.randbytes(20_000) + bytes(30_000),
        'slippwii/patches/MnMaAll.usd.diff': rnd.randbytes(9000),
        'slippwii/patches/manifest.txt': f'# SlippWii {seed}\ncopy netplay.gct 57000 00000000\n'.encode(),
        'slippwii/copy-account.ps1': b'Write-Host "account"\r\n' * 40,
        'slippwii/PUT user.json HERE.txt': b'put it here\r\n',
        'slippwii/NOTICE.txt': b'notice\r\n',
        'slippwii/LICENSE.txt': b'GPL\r\n' * 3000,
        'Copy my Slippi account to SD.bat': b'@echo off\r\n',
        'SlippWii README.txt': f'SlippWii {seed}\r\n'.encode(),
        'slippwii/files/empty.dat': b'',
        **dict(extra),
    }
    packed = io.BytesIO()
    with zipfile.ZipFile(packed, 'w', zipfile.ZIP_DEFLATED) as z:
        for i, (name, data) in enumerate(files.items()):
            z.writestr(zipfile.ZipInfo(name), data, zipfile.ZIP_STORED if i == 2 else zipfile.ZIP_DEFLATED)
    return packed.getvalue(), files


def tree(root):
    return {str(p.relative_to(root)).replace('\\', '/'): p.read_bytes() for p in sorted(Path(root).rglob('*')) if p.is_file()}


class Server:
    """GitHub's part: files by path, with the ways an answer can come."""

    def __init__(self):
        self.files = {}
        outer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'

            def log_message(self, *a):
                pass

            def do_GET(self):
                outer.seen.append((self.path, self.headers.get('User-Agent', ''), self.headers.get('Host', '')))
                kind, _, rest = self.path.lstrip('/').partition('/')
                body = outer.files.get(rest)
                if kind == 'to' and rest:  # a redirect, relative
                    return self.answer(302, b'', {'Location': '/' + rest})
                if kind == 'abs' and rest:  # a redirect, absolute, with headers to fill a head
                    return self.answer(301, b'moved', {'Location': f'{outer.url}/{rest}', 'X-Long': 'x' * 6000})
                if kind == 'loop':
                    return self.answer(302, b'', {'Location': self.path})
                if body is None:
                    return self.answer(404, b'Not Found')
                if kind == 'plain':
                    return self.answer(200, body)
                if kind == 'chunked':
                    self.send_response(200)
                    self.send_header('Transfer-Encoding', 'chunked')
                    self.send_header('Connection', 'close')
                    self.end_headers()
                    rnd, at = random.Random(len(body)), 0
                    while at < len(body):
                        n = rnd.choice((1, 7, 300, 4096, 70_000))
                        piece = body[at:at + n]
                        ext = b';name=value' if rnd.random() < 0.3 else b''
                        self.wfile.write(f'{len(piece):X}'.encode() + ext + b'\r\n' + piece + b'\r\n')
                        at += n
                    self.wfile.write(b'0\r\n\r\n')
                    return
                if kind == 'nolength':
                    self.send_response(200)
                    self.send_header('Connection', 'close')
                    self.end_headers()
                    self.wfile.write(body)
                    self.close_connection = True
                    return
                if kind == 'cut':  # says the whole length, sends half
                    self.send_response(200)
                    self.send_header('Content-Length', str(len(body)))
                    self.send_header('Connection', 'close')
                    self.end_headers()
                    self.wfile.write(body[:len(body) // 2])
                    self.close_connection = True
                    return
                return self.answer(404, b'Not Found')

            def answer(self, status, body, headers=None):
                self.send_response(status)
                for k, v in (headers or {}).items():
                    self.send_header(k, v)
                self.send_header('Content-Length', str(len(body)))
                self.send_header('Connection', 'close')
                self.end_headers()
                self.wfile.write(body)
                self.close_connection = True

        self.seen = []
        self.httpd = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.httpd.handle_error = lambda *a: None  # (a client that hangs up early is part of the tests)
        self.url = f'http://127.0.0.1:{self.httpd.server_address[1]}'
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()


def main():
    global verbose
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--cc', default=os.environ.get('CC', 'gcc'))
    ap.add_argument('--live', action='store_true', help="also download 1.4.0's zip from GitHub")
    ap.add_argument('-v', action='store_true')
    args = ap.parse_args()
    verbose = args.v
    exe = build(args.cc)
    work = Path(tempfile.mkdtemp(prefix='slippwii-update-test-'))
    try:
        tests(exe, work, args)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print(f'RESULT: {"all passed" if not failures else f"{failures} FAILED"}')
    sys.exit(1 if failures else 0)


def tests(exe, work, args):
    # ---- versions ----
    for a, b, want in (('1.4.0', '1.5.0', -1), ('1.5.0', '1.4.0', 1), ('1.5.0', '1.5.0', 0), ('1.10.0', '1.9.9', 1),
                       ('1.5.0-beta.2', '1.5.0', -1), ('1.5.0-beta.2', '1.5.0-beta.10', -1), ('1.5.0', '1.5.0-beta.3', 1),
                       ('2.0.0', '1.99.99', 1), ('1.4.0', '1.4.1', -1), ('1.5.0-beta.1', '1.4.0', 1)):
        check(f'version {a} against {b}', run(exe, 'version', a, b) == str(want))

    # ---- what a release may put on the card ----
    _, layout = release_zip(1)
    for name in layout:
        check(f'allowed: {name}', run(exe, 'allowed', name) == '1')
    for name in ('slippwii/user.json', 'slippwii/config.ini', 'slippwii/logs/crash-1.txt', 'slippwii/ready.txt',
                 'slippwii/rumble.txt', 'slippwii_nincfg.bin', 'apps/SlippWii/../x', '../x.txt', '/x.txt', 'apps/Other/boot.dol',
                 'apps/SlippWii/sub/boot.dol', 'boot.elf', 'games/x.iso', 'slippwii/files/a/b', 'sd:/x.txt', 'a\\b.txt',
                 'saves/x.raw', 'slippwii/', 'apps/SlippWii-Rollback/boot.dol'):
        check(f'refused: {name}', run(exe, 'allowed', name) == '0')
    if (ROOT / 'tools/make_release.py').exists():
        import make_release
        if hasattr(make_release, 'update_allows'):
            names = list(layout) + ['slippwii/user.json', 'x/y.txt', 'boot.elf', 'slippwii/files/a/b', 'apps/SlippWii/boot.dol']
            check("make_release.py's rule is the loader's", all((run(exe, 'allowed', n) == '1') == make_release.update_allows(n) for n in names))

    # ---- the manifest ----
    key = sign_update.new_key(work / 'key.pem')
    other = sign_update.new_key(work / 'other.pem')
    pub, other_pub = sign_update.public_point(key).hex(), sign_update.public_point(other).hex()
    zip1, files1 = release_zip(1)
    good = sign_update.manifest(key, '1.5.0', zip1, ['Line one', 'Line two'])
    sha = hashlib.sha256(zip1).hexdigest()

    def manifest(text, k=pub, flags=0):
        (work / 'm.txt').write_bytes(text)
        return run(exe, 'manifest', work / 'm.txt', k, flags)

    got = manifest(good)
    check('a signed manifest is read', got == f'version=1.5.0 zip={sign_update.ZIP_URL.format(version="1.5.0")} size={len(zip1)} '
          f'sha256={sha} note=[Line one] note=[Line two]', got)
    check('signed by another key: refused', manifest(good, other_pub) == 'error -2')
    check('a changed version: refused', manifest(good.replace(b'version=1.5.0', b'version=1.5.1')) == 'error -2')
    check('a changed checksum: refused', manifest(good.replace(sha.encode(), ('0' * 64).encode())) == 'error -2')
    check('no signature: refused', manifest(good[:good.rfind(b'sig=')]) == 'error -1')
    check('a cut signature: refused', manifest(good[:-20] + b'\n') in ('error -1', 'error -2'))
    check('not a manifest: refused', manifest(b'<html>Not Found</html>\n') == 'error -1')
    check('empty: refused', manifest(b'') == 'error -1')
    elsewhere = sign_update.manifest(key, '1.5.0', zip1, url='https://example.com/SlippWii.zip')
    check('a signed manifest whose zip is elsewhere: refused', manifest(elsewhere) == 'error -3')
    check('... but read by a test build', manifest(elsewhere, flags=1).startswith('version=1.5.0 zip=https://example.com/'))
    body = good[:good.rfind(b'sig=')] + b'later=a key of a later version\n'
    later = body + b'sig=' + sign_update.sign(key, body).hex().encode() + b'\n'
    check('a key this version does not know is skipped', manifest(later).startswith('version=1.5.0 '), manifest(later))
    body = good[:good.rfind(b'sig=')].replace(b'\n', b'\r\n')
    check('CRLF line ends: refused', manifest(body + b'sig=' + sign_update.sign(key, body).hex().encode() + b'\r\n').startswith('error'))
    body = good[:good.rfind(b'sig=')].replace(f'size={len(zip1)}'.encode(), b'size=99999999')
    check('a zip of 99 MB: refused', manifest(body + b'sig=' + sign_update.sign(key, body).hex().encode() + b'\n') == 'error -1')

    # ---- downloads ----
    srv = Server()
    blob = random.Random(7).randbytes(400_000)
    srv.files.update({'blob': blob, 'small': b'hello', 'empty': b''})

    def get(path, max_size=1 << 20, flags=1):
        out = work / 'got.bin'
        if out.exists():
            out.unlink()
        line = run(exe, 'get', srv.url + path if path.startswith('/') else path, out, max_size, flags)
        return line, out.read_bytes() if out.exists() else b''

    for kind in ('plain', 'chunked', 'nolength'):
        line, data = get(f'/{kind}/blob')
        check(f'download, {kind}', line.startswith('status 200 bytes 400000') and data == blob, line)
    line, data = get('/to/abs/to/plain/blob')
    check('three redirects (relative, absolute with 6 KB of headers, relative)', line.startswith('status 200') and data == blob, line)
    line, data = get('/plain/empty')
    check('an empty file', line.startswith('status 200 bytes 0'), line)
    line, _ = get('/plain/nothing-here')
    check('404 is told as 404', line.startswith('status 404'), line)
    line, _ = get('/loop/x')
    check('a redirect loop ends', line.startswith('status -12'), line)
    line, _ = get('/plain/blob', max_size=1000)
    check('a file larger than the room: refused', line.startswith('status -13'), line)
    line, _ = get('/chunked/blob', max_size=1000)
    check('... in chunks too', line.startswith('status -13'), line)
    line, _ = get('/cut/blob')
    check('a download that breaks off', line.startswith('status -10'), line)
    line, _ = get('/plain/small', flags=0)
    check('plain http is refused outside tests', line.startswith('status -12'), line)
    line, _ = get('http://127.0.0.1:9/x')
    check('nobody answers', line.startswith('status -10'), line)
    check('the request names the client and the host', srv.seen[0][1] == 'SlippWii-test' and srv.seen[0][2] == srv.url[7:], srv.seen[0])

    # ---- onto the card ----
    def install(zip_bytes, root, env=None):
        (work / 'in.zip').write_bytes(zip_bytes)
        return run(exe, 'install', work / 'in.zip', root, env=env)

    card = work / 'card'
    card.mkdir()
    mine = {'slippwii/user.json': b'{"playKey": "not a real one"}', 'slippwii/config.ini': b'rollback=1\n',
            'slippwii/files/MnMaAll.usd': b'made on the Wii', 'slippwii_nincfg.bin': b'settings', 'games/melee.iso': b'disc'}
    for name, data in mine.items():
        (card / name).parent.mkdir(parents=True, exist_ok=True)
        (card / name).write_bytes(data)
    line = install(zip1, card)
    check('a first install', line == f'result 0 files {len(files1)} []' and tree(card) == {**mine, **files1}, line)
    zip2, files2 = release_zip(2)
    line = install(zip2, card)
    check("an update: the release's files replaced, the player's left alone, no temporary file left",
          line == f'result 0 files {len(files2)} []' and tree(card) == {**mine, **files2}, line)

    def verify(zip_bytes, root, command='verify'):
        (work / 'in.zip').write_bytes(zip_bytes)
        return run(exe, command, work / 'in.zip', root)

    check('read back after the install: every file', verify(zip2, card) == f'verified {len(files2)}', verify(zip2, card))
    # (releases 1 and 2 share the files whose contents do not go by the version: those still read right)
    same = sum(1 for n in files1 if files1[n] == files2[n])
    check('read back against another release: only the files they share', verify(zip1, card) == f'verified {same}', verify(zip1, card))
    (card / 'slippwii/netplay.gct').write_bytes(files2['slippwii/netplay.gct'][:-1] + b'\x00')
    check('one file changed on the card: seen', verify(zip2, card) == f'verified {len(files2) - 1}')
    (card / 'slippwii/netplay.gct').write_bytes(files2['slippwii/netplay.gct'])

    before = tree(card)
    zip3, files3 = release_zip(3)
    line = install(zip3, card, env={'UPD_FAIL_WRITE': '5'})
    check('the card refuses the fifth file: nothing changed', line.startswith('result -22') and tree(card) == before, line)
    line = install(zip3, card, env={'UPD_FAIL_RENAME': '4'})
    after = tree(card)
    check('the card fails while files are replaced: said so, and the app is still the old one',
          line.startswith('result -23') and after['apps/SlippWii/boot.dol'] == files2['apps/SlippWii/boot.dol'], line)
    line = install(zip3, card)
    check('... and the update run again completes', line == f'result 0 files {len(files3)} []' and tree(card) == {**mine, **files3}, line)

    before = tree(card)
    bad, _ = release_zip(4, extra={'slippwii/user.json': b'{}'})
    check('a zip with the account in it: refused whole', install(bad, card).startswith('result -21') and tree(card) == before)
    bad, _ = release_zip(4, extra={'apps/Other/boot.dol': b'x'})
    check('a zip with another app in it: refused whole', install(bad, card).startswith('result -21') and tree(card) == before)
    zip4, _ = release_zip(4)
    damaged = bytearray(zip4)
    damaged[5000] ^= 0x40
    check('a damaged zip: refused whole', install(bytes(damaged), card).startswith('result -20') and tree(card) == before)
    check('half a zip: refused whole', install(zip4[:len(zip4) // 2], card).startswith('result -20') and tree(card) == before)
    check('not a zip: refused whole', install(b'<html>' * 100, card).startswith('result -20') and tree(card) == before)

    scratch = work / 'scratch'
    scratch.mkdir()
    (scratch / 'slippwii').mkdir()
    (scratch / 'slippwii/user.json').write_bytes(b'kept')
    install(zip1, scratch)
    check("a test's files taken off again: none left, and nothing else gone",
          verify(zip1, scratch, 'remove') == 'verified 0' and tree(scratch) == {'slippwii/user.json': b'kept'})

    newest = sorted((ROOT / 'dist').glob('SlippWii-v*.zip'), key=lambda p: p.stat().st_mtime)
    if newest:
        real = work / 'real'
        real.mkdir()
        with zipfile.ZipFile(newest[-1]) as z:
            want = {n: z.read(n) for n in z.namelist()}
        line = install(newest[-1].read_bytes(), real)
        check(f'{newest[-1].name} itself installs, every byte as Python unpacks it',
              line == f'result 0 files {len(want)} []' and tree(real) == want, line)

    # ---- the whole of it ----
    card2 = work / 'card2'
    card2.mkdir()
    srv.files['release.zip'] = zip1
    srv.files['update.txt'] = sign_update.manifest(key, '1.5.0', zip1, ['A note'], url=f'{srv.url}/to/chunked/release.zip')

    def update(have, k=pub, name='update.txt'):
        return run(exe, 'update', f'{srv.url}/to/plain/{name}', k, card2, have, 1)

    line = update('1.5.0')
    check('the same version: up to date, nothing downloaded', line.endswith('up to date (1.5.0, the latest is 1.5.0)') and not tree(card2), line)
    check('a newer one installed: up to date', update('1.6.0-beta.1').endswith('up to date (1.6.0-beta.1, the latest is 1.5.0)'))
    line = update('1.4.0', other_pub)
    check('a manifest by another key: refused, nothing installed', 'check -2' in line and not tree(card2), line)
    srv.files['update-wrong.txt'] = sign_update.manifest(key, '1.5.0', zip2, url=f'{srv.url}/plain/release.zip')
    line = update('1.4.0', name='update-wrong.txt')
    check('a zip of another size than the manifest says: refused, nothing installed', 'download -13' in line and not tree(card2), line)
    changed = bytearray(zip1)
    changed[len(changed) // 3] ^= 1
    srv.files['changed.zip'] = bytes(changed)
    srv.files['update-changed.txt'] = sign_update.manifest(key, '1.5.0', zip1, url=f'{srv.url}/plain/changed.zip')
    line = update('1.4.0', name='update-changed.txt')
    check('a zip with one bit changed on the way: refused, nothing installed', 'download -20' in line and not tree(card2), line)
    line = update('1.4.0', name='no-such.txt')
    check('no manifest on the server: said so', line.startswith('check 404'), line)
    line = update('1.4.0')
    check('an older version: the update is installed', line.endswith(f'installed 0 files {len(files1)} []') and tree(card2) == files1, line)

    # ---- the release key, and a release made with it ----
    import re
    point = bytes(int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})', (SRC / 'upd_key.h').read_text()))
    check('upd_key.h holds a public key', len(point) == 65 and point[0] == 4)
    real = sign_update.default_key()
    if real.exists():
        check("the release key on this PC is the one whose public half is in upd_key.h", sign_update.public_point(real) == point)
    for released in sorted((ROOT / 'dist').glob('update-v*.txt')):
        version = released.stem[len('update-v'):]
        got = manifest(released.read_bytes(), point.hex())
        zip_path = ROOT / f'dist/SlippWii-v{version}.zip'
        right = got.startswith(f'version={version} zip={sign_update.ZIP_URL.format(version=version)} ')
        if zip_path.exists():
            right = right and f'size={zip_path.stat().st_size} sha256={hashlib.sha256(zip_path.read_bytes()).hexdigest()}' in got
        check(f'dist/{released.name}: signed with the release key, for that version and its zip', right, got)

    if args.live:
        line, data = get(LIVE_ZIP, max_size=4 << 20, flags=0)
        check("GitHub, over TLS 1.2: 1.4.0's zip, as published", line.startswith('status 200') and hashlib.sha256(data).hexdigest() == LIVE_SHA, line)
    srv.httpd.shutdown()


if __name__ == '__main__':
    main()
