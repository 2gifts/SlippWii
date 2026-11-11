"""Tests "Check for Updates" on a Wii, with nobody at the console.

    python tools/update_wii_test.py --wii-ip WII_IP [--scenarios github uptodate ...] [--skip-build]
    python tools/update_wii_test.py --wii-ip WII_IP --production

A test build of the loader is made (Nintendont/loader/source/update_test.inc gets a signing key made
for this run; the repo's files are put back afterwards) and sent to the Homebrew Channel with the
launch argument "slippwii-update-test=URL;FOLDER": the updater runs by itself against a web server
on this PC, installs into FOLDER on the Wii's card (/slippwii-update-test: never the card's root,
where the player's SlippWii is), reads the files back, takes them off the card again, tells this
PC how it went and returns to the Homebrew Channel. A capture card takes pictures of its screens
(build/update-wii-test/).

Scenarios:
  github    a newer version whose zip is SlippWii 1.4.0's on GitHub: the Wii's TLS, GitHub's
            redirects, 2 MB over the internet, 27 files onto the card and read back
  local     the same zip from this PC, in chunks: no internet needed
  uptodate  the latest version is the one installed: said so, nothing downloaded
  badsig    update information signed by another key: refused
  tampered  a zip with one bit changed: refused, nothing written
  replace   two runs into the same folder: SlippWii 1.3.1's zip from this PC, left on the card, then
            1.4.0's from GitHub over it: files that are there are replaced
  real      GitHub's own "latest release" address, as a player's SlippWii asks it (until a release
            has update information there, the answer is "none"); GitHub is not told how it went, so
            the pictures are the result

--production is another build and one run: the release key's public half in place of a test key, and
the build told it is version 0.0.1. It asks GitHub's own "latest release" address, so what it checks
is what players' consoles will: that the published update.txt is signed with the release key, and
that the published zip downloads and installs (into the test's folder). Before a release carries
update.txt it ends with "no update information" (404); right after publishing one it is the proof.

Only the loader runs: no game is started, so nothing contacts Slippi.
"""
import argparse
import hashlib
import http.server
import os
import re
import shutil
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'tools/sweep'))
import sign_update  # noqa: E402
import wii_boot  # noqa: E402
import decode_marker  # noqa: E402

INC = ROOT / 'Nintendont/loader/source/update_test.inc'
DOL = ROOT / 'Nintendont/loader/loader.dol'
DIST = ROOT / 'dist/sd/apps/SlippWii/boot.dol'
KEY_H = ROOT / 'Nintendont/loader/source/update/upd_key.h'
OUT = ROOT / 'build/update-wii-test'
FOLDER = '/slippwii-update-test'
GITHUB_ZIP = 'https://github.com/2gifts/SlippWii/releases/download/v1.4.0/SlippWii-v1.4.0.zip'
GITHUB_SHA = '0a1df02af94873a6fb36eaf2702e646fb8640e13e14c7cc3fe0d45aa4f0ec7c8'
REAL_MANIFEST = 'https://github.com/2gifts/SlippWii/releases/latest/download/update.txt'


def hbc(wii_ip, timeout_s):
    start = time.time()
    while time.time() - start < timeout_s:
        try:
            with socket.create_connection((wii_ip, wii_boot.WIILOAD_PORT), timeout=2):
                return round(time.time() - start)
        except OSError:
            time.sleep(2)
    return None


def build(point, version=None):
    """The loader with a test's key in it (its public point) and, if given, the version it is to say it is;
    the repo's files as they were afterwards."""
    saved = {p: p.read_bytes() for p in (INC, DOL, DIST) if p.exists()}
    try:
        text = '/* TEST BUILD (tools/update_wii_test.py) */\n#define UPD_TEST_KEY ' + ', '.join(f'0x{b:02X}' for b in point) + '\n'
        if version:
            text += f'#define UPD_TEST_VERSION "{version}"\n'
        INC.write_text(text, newline='\n')
        r = subprocess.run(['sh', 'tools/build.sh'], capture_output=True, text=True, cwd=ROOT)
        if r.returncode:
            sys.exit(f'build failed:\n{r.stdout[-3000:]}{r.stderr[-3000:]}')
        OUT.mkdir(parents=True, exist_ok=True)
        shutil.copy(DOL, OUT / 'loader-test.dol')
    finally:
        for p, b in saved.items():
            p.write_bytes(b)


class Server:
    def __init__(self, host):
        self.files, self.reports, self.seen = {}, [], []
        outer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'

            def log_message(self, *a):
                pass

            def do_GET(self):
                outer.seen.append(f'{time.strftime("%H:%M:%S")} {self.client_address[0]} {self.path[:150]} [{self.headers.get("User-Agent", "")}]')
                kind, _, rest = self.path.lstrip('/').partition('/')
                if kind == 'report':
                    outer.reports.append(dict(kv.split('=', 1) for kv in rest.split(';') if '=' in kv))
                    return self.answer(200, b'ok')
                body = outer.files.get(rest)
                if body is None:
                    return self.answer(404, b'Not Found')
                if kind == 'chunked':
                    self.send_response(200)
                    self.send_header('Transfer-Encoding', 'chunked')
                    self.send_header('Connection', 'close')
                    self.end_headers()
                    for at in range(0, len(body), 30_000):
                        piece = body[at:at + 30_000]
                        self.wfile.write(f'{len(piece):X}\r\n'.encode() + piece + b'\r\n')
                    self.wfile.write(b'0\r\n\r\n')
                    return
                return self.answer(200, body)

            def answer(self, status, body):
                self.send_response(status)
                self.send_header('Content-Length', str(len(body)))
                self.send_header('Connection', 'close')
                self.end_headers()
                self.wfile.write(body)
                self.close_connection = True

        self.httpd = http.server.ThreadingHTTPServer(('0.0.0.0', 0), Handler)
        self.httpd.handle_error = lambda *a: None
        self.url = f'http://{host}:{self.httpd.server_address[1]}'
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()


def one_run(args, srv, dol, label, url, extra, expect):
    """One boot of the test loader; True if it went as it must."""
    if hbc(args.wii_ip, 90) is None:
        sys.exit('the Homebrew Channel does not answer')
    time.sleep(2)
    argument = f'slippwii-update-test={url};{FOLDER}{extra}'
    for old in OUT.glob(f'{label}-*.png'):
        old.unlink()
    srv.reports.clear()
    shots = subprocess.Popen([decode_marker.FFMPEG, '-y', '-loglevel', 'error', '-f', 'dshow', '-rtbufsize', '100M',
                              '-vcodec', 'mjpeg', '-video_size', '720x480', '-framerate', '25', '-i', f'video={args.device}',
                              '-vf', 'fps=1/3', '-t', '75', str(OUT / f'{label}-%02d.png')],
                             stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    start = time.time()
    with socket.create_connection((args.wii_ip, wii_boot.WIILOAD_PORT), timeout=10) as s:
        s.sendall(wii_boot.wiiload_packet(dol, wii_boot.ARGV0 + b'\0' + argument.encode() + b'\0'))
    while time.time() - start < 150 and not srv.reports and expect is not None:
        time.sleep(1)
    took = round(time.time() - start)
    back = hbc(args.wii_ip, 120)
    try:
        shots.wait(timeout=90)
    except subprocess.TimeoutExpired:
        shots.kill()
    pictures = len(list(OUT.glob(f'{label}-*.png')))
    came = 'came back' if back is not None else 'did NOT come back'
    if expect is None:  # (GitHub is not told how it went: the pictures are the result)
        print(f'{label}: no report by design; {pictures} pictures in {OUT}; the Homebrew Channel {came}')
        return back is not None
    if not srv.reports:
        print(f'{label}: FAILED, no report from the Wii in {took} s; {pictures} pictures in {OUT}')
        return False
    r = srv.reports[-1]
    ok = bool(expect(r)) and back is not None
    print(f'{label}: {"ok" if ok else "FAILED"} after {took} s: {r}; {pictures} pictures; the Homebrew Channel {came}')
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--wii-ip', default=os.environ.get('SLIPPWII_WII_IP'), required=not os.environ.get('SLIPPWII_WII_IP'))
    ap.add_argument('--scenarios', nargs='+', default=['github', 'uptodate', 'badsig', 'real'],
                    choices=['github', 'local', 'uptodate', 'badsig', 'tampered', 'replace', 'real'])
    ap.add_argument('--production', action='store_true',
                    help="the release key and GitHub's own address, the build saying it is 0.0.1 (see above)")
    ap.add_argument('--manifest-file', help='--production: serve this update.txt from here in place of asking GitHub '
                                            '(a release made and not yet published: is its signature the release key\'s?)')
    ap.add_argument('--zip', default=str(ROOT / 'dist/SlippWii-v1.4.0.zip'), help="SlippWii 1.4.0's zip as published")
    ap.add_argument('--old-zip', default=str(ROOT / 'dist/SlippWii-v1.3.1.zip'), help="replace: an older release's zip")
    ap.add_argument('--skip-build', action='store_true', help="send build/update-wii-test/loader-test.dol and use its key again")
    ap.add_argument('--device', default='AV TO USB2.0', help='the capture device (DirectShow name)')
    args = ap.parse_args()

    OUT.mkdir(parents=True, exist_ok=True)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.connect((args.wii_ip, 9))
        pc = s.getsockname()[0]
    srv = Server(pc)
    installed = lambda r: r['result'] == '0' and r['files'] == r['verified'] == '27' and r['left'] == '0'  # noqa: E731

    if args.production:
        # the public half as the repo has it: what a release build checks with
        point = bytes(int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})', KEY_H.read_text()))
        if len(point) != 65:
            sys.exit(f'{KEY_H} does not hold a key')
        if not args.skip_build:
            print("building the loader with the release key's public half, as version 0.0.1 (a minute or two)...")
            build(point, '0.0.1')
        if args.manifest_file:
            # A release's update.txt before it is published: this PC serves the very file, the Wii checks its
            # signature with the release key's public half and goes for the zip it names, which GitHub does
            # not have yet (the download fails: "latest" in the report is what the Wii read from the file).
            srv.files['update.txt'] = Path(args.manifest_file).read_bytes()
            runs = [('production-file', f'{srv.url}/plain/update.txt', '',
                     lambda r: installed(r) or (r['step'] == 'download' and r['latest'] not in ('', '0.0.1')))]
        else:
            # (what it must say depends on whether a release with update.txt is out: the line printed is the result)
            runs = [('production', REAL_MANIFEST, f';report={srv.url}', lambda r: installed(r) or r['result'] in ('404', '1'))]
    else:
        zip_bytes = Path(args.zip).read_bytes()
        if hashlib.sha256(zip_bytes).hexdigest() != GITHUB_SHA:
            sys.exit(f"{args.zip} is not SlippWii 1.4.0's zip as published")
        key, other = OUT / 'test-key.pem', OUT / 'other-key.pem'
        if not args.skip_build:
            for k in (key, other):
                if k.exists():
                    k.unlink()
                sign_update.new_key(k)
            print('building the test loader (a minute or two)...')
            build(sign_update.public_point(key))
        changed = bytearray(zip_bytes)
        changed[len(changed) // 2] ^= 1
        srv.files.update({
            'release.zip': zip_bytes,
            'changed.zip': bytes(changed),
            'github.txt': sign_update.manifest(key, '9.9.9', zip_bytes, ["A test: 1.4.0's zip from GitHub"], url=GITHUB_ZIP),
            'local.txt': sign_update.manifest(key, '9.9.9', zip_bytes, ['A test: the zip from this PC, in chunks'],
                                              url=f'{srv.url}/chunked/release.zip'),
            'uptodate.txt': sign_update.manifest(key, '0.0.1', zip_bytes, url=GITHUB_ZIP),
            'badsig.txt': sign_update.manifest(other, '9.9.9', zip_bytes, url=GITHUB_ZIP),
            'tampered.txt': sign_update.manifest(key, '9.9.9', zip_bytes, ['A test: a zip with one bit changed'],
                                                 url=f'{srv.url}/plain/changed.zip'),
        })
        if 'replace' in args.scenarios:
            srv.files['old.zip'] = Path(args.old_zip).read_bytes()
            srv.files['old.txt'] = sign_update.manifest(key, '9.9.8', srv.files['old.zip'], ['A test: an older release, left on the card'],
                                                        url=f'{srv.url}/chunked/old.zip')
        m = f'{srv.url}/plain/'
        steps = {  # a scenario's runs: the manifest's address, what follows the folder in the argument, what the report must say
            'github': [(m + 'github.txt', '', installed)],
            'local': [(m + 'local.txt', '', installed)],
            'uptodate': [(m + 'uptodate.txt', '', lambda r: r['result'] == '1' and r['files'] == '0' and r['latest'] == '0.0.1')],
            'badsig': [(m + 'badsig.txt', '', lambda r: r['result'] == '-2' and r['step'] == 'check' and r['files'] == '0')],
            'tampered': [(m + 'tampered.txt', '', lambda r: r['result'] == '-20' and r['step'] == 'download' and r['files'] == '0')],
            'replace': [(m + 'old.txt', ';keep', lambda r: r['result'] == '0' and r['files'] == r['verified'] == r['left'] == '27'),
                        (m + 'github.txt', '', installed)],
            'real': [(REAL_MANIFEST, '', None)],
        }
        runs = []
        for name in args.scenarios:
            for number, (url, extra, expect) in enumerate(steps[name], 1):
                runs.append((name if len(steps[name]) == 1 else f'{name}-{number}', url, extra, expect))

    dol = (OUT / 'loader-test.dol').read_bytes()
    failed = sum(not one_run(args, srv, dol, label, url, extra, expect) for label, url, extra, expect in runs)
    print('what the server saw:')
    for line in srv.seen:
        print('  ' + line)
    print(f'RESULT: {"all passed" if not failed else f"{failed} FAILED"}')
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
