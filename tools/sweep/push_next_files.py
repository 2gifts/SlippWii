"""Puts the game files of Slippi's next version on the Wii's card for tests, beside the player's own.

    python tools/sweep/push_next_files.py --wii-ip WII_IP [--iso Melee.iso] [--skip-build]

A build that speaks what comes after Slippi 3.6.4 (docs/NEXT_SLIPPI.md) needs that version's codeset
and menu files. The card in the Wii has the released version's, in /slippwii, and a test must not
replace those. So this installs that version's files, as a release would ship them (the codeset,
Slippi's own files, its patches and linked files for the disc's, the manifest: tools/slippi_files.py
release_files), into a folder of their own, /slippwii-next, with the updater's test mode
(tools/update_wii_test.py: a test build of the loader, a web server on this PC, a signing key made
for the run, nobody at the console), and leaves them there. A test then starts the loader with
"slippwii-files-root=/slippwii-next", so that it makes the menu files from the disc in that folder
as a release's loader does in /slippwii, and its kernel looks there first
(test_files_root=/slippwii-next/slippwii): tools/sweep/run_sweep.py --next does both.

Run it again whenever the files change (another codeset, tools/slippi_files.py's choices). The
folder can be deleted from the card at any time; nothing but these tests reads it.
"""
import argparse
import hashlib
import io
import socket
import sys
import time
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'tools/sweep'))
import sign_update  # noqa: E402
import slippi_files  # noqa: E402
import update_wii_test  # noqa: E402
import wii_boot  # noqa: E402

FOLDER = '/slippwii-next'
OUT = ROOT / 'build/next/push'
def make_zip(iso):
    shipped = slippi_files.release_files(slippi_files.open_disc(iso), 'next-test', next_version=True)
    # (the updater wants an app in every release; this one lands in the test's folder and is never run)
    shipped['apps/SlippWii/boot.dol'] = (ROOT / 'dist/sd/apps/SlippWii/boot.dol').read_bytes()
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, 'w', zipfile.ZIP_DEFLATED) as z:
        for name, data in sorted(shipped.items()):
            z.writestr(zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0)), data, zipfile.ZIP_DEFLATED)
    return buf.getvalue(), len(shipped)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--wii-ip', required=True)
    ap.add_argument('--iso', default=str(Path.home() / 'Downloads/SSB/SSBMv102.iso'), help='the NTSC 1.02 disc image')
    ap.add_argument('--skip-build', action='store_true', help='send the test loader built last time, with its key')
    args = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    data, count = make_zip(args.iso)
    (OUT / 'next-files.zip').write_bytes(data)
    print(f'{count} files, {len(data) // 1024} KB zipped (sha256 {hashlib.sha256(data).hexdigest()[:16]})')
    key = OUT / 'test-key.pem'
    if not args.skip_build or not key.exists():
        if key.exists():
            key.unlink()
        sign_update.new_key(key)
        print('building the test loader (a minute or two)...')
        update_wii_test.build(sign_update.public_point(key))
        (OUT / 'loader-test.dol').write_bytes((update_wii_test.OUT / 'loader-test.dol').read_bytes())
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.connect((args.wii_ip, 9))
        pc = s.getsockname()[0]
    srv = update_wii_test.Server(pc)
    srv.files['next.zip'] = data
    srv.files['next.txt'] = sign_update.manifest(key, '9.9.9', data, ["Test files: Slippi's development codeset"],
                                                 url=f'{srv.url}/chunked/next.zip')
    if update_wii_test.hbc(args.wii_ip, 90) is None:
        sys.exit('the Homebrew Channel does not answer')
    time.sleep(2)
    argument = f'slippwii-update-test={srv.url}/plain/next.txt;{FOLDER};keep'
    start = time.time()
    with socket.create_connection((args.wii_ip, wii_boot.WIILOAD_PORT), timeout=10) as s:
        s.sendall(wii_boot.wiiload_packet((OUT / 'loader-test.dol').read_bytes(), wii_boot.ARGV0 + b'\0' + argument.encode() + b'\0'))
    while time.time() - start < 180 and not srv.reports:
        time.sleep(1)
    back = update_wii_test.hbc(args.wii_ip, 120)
    if not srv.reports:
        sys.exit(f'no report from the Wii in {round(time.time() - start)} s; requests seen: {srv.seen[-5:]}')
    r = srv.reports[-1]
    ok = r.get('result') == '0' and r.get('files') == r.get('verified') == r.get('left') == str(count)
    print(f'{"installed" if ok else "FAILED"} in {FOLDER} after {round(time.time() - start)} s: {r}; the Homebrew Channel '
          + ('came back' if back is not None else 'did NOT come back'))
    sys.exit(0 if ok and back is not None else 1)


if __name__ == '__main__':
    main()
