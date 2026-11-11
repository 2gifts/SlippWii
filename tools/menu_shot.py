"""Takes a picture of the loader's Slippi Settings page from a capture card, with nobody at the console.

    python tools/menu_shot.py --wii-ip WII_IP [--rows 0 5 6 ...] [--dol Nintendont/loader/loader.dol]

For each row, the loader is sent to the Homebrew Channel (wiiload) with the launch argument
"slippwii-menu-test=ROW": it opens on the Slippi Settings page with the cursor on that row (so the
row's description is on the right), and goes back to the Homebrew Channel after 25 s
(Nintendont/loader/source/menu.c, SlippWiiMenuTest). The Homebrew Channel takes about 10 s to
receive the loader, which then needs a few more to start: a frame is taken 20 s after the send and
saved as build/menu/row-ROW.png. The settings shown are the ones on the Wii's SD card.

The loader must be one with SlippWiiMenuTest in its device selection too (after 2026-11-10): an
older one waits there for a button.

The capture device is the one the sweeps use (ffmpeg, DirectShow "AV TO USB2.0").
"""
import argparse
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'tools/sweep'))
import wii_boot  # noqa: E402
import decode_marker  # noqa: E402


def hbc(wii_ip, timeout_s):
    """Seconds until the Homebrew Channel takes a connection, or None."""
    start = time.time()
    while time.time() - start < timeout_s:
        try:
            with socket.create_connection((wii_ip, wii_boot.WIILOAD_PORT), timeout=2):
                return round(time.time() - start)
        except OSError:
            time.sleep(2)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--wii-ip', default=os.environ.get('SLIPPWII_WII_IP'), required=not os.environ.get('SLIPPWII_WII_IP'))
    ap.add_argument('--rows', type=int, nargs='+', default=[0], help='the cursor rows to show (0: the first setting)')
    ap.add_argument('--dol', default=str(ROOT / 'Nintendont/loader/loader.dol'), help='the loader to boot (as tools/build.sh makes it)')
    ap.add_argument('--device', default='AV TO USB2.0', help='the capture device (DirectShow name)')
    args = ap.parse_args()
    out = ROOT / 'build/menu'
    out.mkdir(parents=True, exist_ok=True)
    dol = Path(args.dol).read_bytes()
    for row in args.rows:
        if hbc(args.wii_ip, 60) is None:
            sys.exit('the Homebrew Channel does not answer')
        time.sleep(2)
        data = wii_boot.wiiload_packet(dol, wii_boot.ARGV0 + b'\0' + f'slippwii-menu-test={row}'.encode() + b'\0')
        with socket.create_connection((args.wii_ip, wii_boot.WIILOAD_PORT), timeout=10) as s:
            s.sendall(data)
        time.sleep(20)
        png = out / f'row-{row}.png'
        r = subprocess.run([decode_marker.FFMPEG, '-y', '-loglevel', 'error', '-f', 'dshow', '-rtbufsize', '100M',
                            '-vcodec', 'mjpeg', '-video_size', '720x480', '-framerate', '25', '-i', f'video={args.device}',
                            '-frames:v', '1', '-update', '1', str(png)], capture_output=True, text=True)
        print(f'row {row}: {png}' if png.exists() and not r.returncode else f'row {row}: no picture ({r.stderr.strip()[-200:]})')
        time.sleep(22)  # (the loader leaves by itself)
    back = hbc(args.wii_ip, 60)
    print('the Homebrew Channel is back' if back is not None else 'the Wii did NOT come back to the Homebrew Channel')


if __name__ == '__main__':
    main()
