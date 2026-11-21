"""Picture glitches in a capture of the Wii's video (run_sweep.py --capture): the whole picture moving
up or down for a moment, and one half of it (top or bottom) flashing a different colour, as players
describe them ("the screen shifts ever so slightly up or down", "flashes a colour, sometimes just the
top half", 2026-11-18).

    python tools/sweep/picture_glitches.py RUN_DIR_OR_MKV [--from S] [--to S] [--stills DIR]

A shift: the picture's row profile (each row's mean brightness, below the test marker's rows) lines
up best with the picture before it moved by a line or more, and with the one after it moved back:
a moment's move, not a camera pan. A flash: one half's mean colour jumps away from both neighbours
by more than FLASH, the other half not. The card takes one field of every two refreshes (see
decode_marker.py), so a glitch that lasts one refresh is seen about half the time.
"""
import argparse
import subprocess
from pathlib import Path

import numpy as np

from decode_marker import FFMPEG

W, H = 720, 480
TOP = 60	# (rows above: the test build's frame marker)
SHIFT_MOST = 6
FLASH = 18.0	# mean level, 0-255


def pictures(path, start=None, end=None):
    cmd = [FFMPEG, '-v', 'quiet']
    if start:
        cmd += ['-ss', str(start)]
    cmd += ['-i', str(path)]
    if end:
        cmd += ['-t', str(end - (start or 0))]
    cmd += ['-f', 'rawvideo', '-pix_fmt', 'rgb24', '-']
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    size = W * H * 3
    while True:
        b = p.stdout.read(size)
        if len(b) < size:
            break
        yield np.frombuffer(b, np.uint8).reshape(H, W, 3)


def best_shift(a, b):
    """The row shift of profile b against a that lines them up best, and how much better it is than none."""
    n = len(a)
    errs = {}
    for s in range(-SHIFT_MOST, SHIFT_MOST + 1):
        x = a[max(0, s):n + min(0, s)]
        y = b[max(0, -s):n - max(0, s)]
        errs[s] = float(np.mean(np.abs(x - y)))
    s = min(errs, key=errs.get)
    return s, errs[0] - errs[s], errs[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('path')
    ap.add_argument('--from', dest='start', type=float)
    ap.add_argument('--to', dest='end', type=float)
    ap.add_argument('--stills', help='save the pictures around each glitch here')
    args = ap.parse_args()
    path = Path(args.path)
    if path.is_dir():
        path = path / 'capture.mkv'
    prev = prev2 = None
    profs = []
    halves = []
    shifts, flashes = [], []
    keep = []
    for i, pic in enumerate(pictures(path, args.start, args.end)):
        g = pic[TOP:].mean(axis=2)
        prof = g.mean(axis=1)
        top = pic[TOP:H // 2].reshape(-1, 3).mean(axis=0)
        bot = pic[H // 2:].reshape(-1, 3).mean(axis=0)
        profs.append(prof)
        halves.append((top, bot))
        if args.stills:
            keep = (keep + [pic])[-3:]
        k = len(profs) - 2	# the picture before this one, between its two neighbours
        if k >= 1:
            dark = profs[k].mean() < 20
            s1, gain1, base1 = best_shift(profs[k - 1], profs[k])
            s2, gain2, _ = best_shift(profs[k], profs[k + 1])
            # moved by s1 against the one before and back by about as much against the one after
            if (not dark and 0 < abs(s1) <= 3 and 0 < abs(s2) <= 3 and s1 + s2 == 0 and gain1 > 1.0 and gain2 > 1.0
                    and gain1 > base1 * 0.5):
                shifts.append((k, s1, gain1))
            for h in (0, 1):
                d1 = np.abs(halves[k][h] - halves[k - 1][h]).max()
                d2 = np.abs(halves[k][h] - halves[k + 1][h]).max()
                o1 = np.abs(halves[k][1 - h] - halves[k - 1][1 - h]).max()
                if d1 > FLASH and d2 > FLASH and o1 < FLASH / 3:
                    flashes.append((k, 'top' if h == 0 else 'bottom', float(min(d1, d2))))
            if args.stills and (shifts and shifts[-1][0] == k or flashes and flashes[-1][0] == k):
                out = Path(args.stills)
                out.mkdir(parents=True, exist_ok=True)
                import zlib, struct as st
                for j, im in enumerate(keep):
                    raw = b''.join(b'\0' + im[r].tobytes() for r in range(H))
                    png = b'\x89PNG\r\n\x1a\n'
                    def chunk(t, d):
                        return st.pack('>I', len(d)) + t + d + st.pack('>I', zlib.crc32(t + d) & 0xffffffff)
                    png += chunk(b'IHDR', st.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b'')
                    (out / f'pic{k:06d}_{j}.png').write_bytes(png)
    n = len(profs)
    t0 = args.start or 0
    print(f'{path}: {n} pictures ({n / 25:.0f} s at 25 a second)')
    print(f'shifts (a moment up or down): {len(shifts)}')
    for k, s, g in shifts[:40]:
        print(f'  {t0 + k / 25:8.2f} s  picture {k}: {s:+d} lines (fit better by {g:.1f})')
    print(f'flashes (one half): {len(flashes)}')
    for k, h, d in flashes[:40]:
        print(f'  {t0 + k / 25:8.2f} s  picture {k}: {h} half, by {d:.0f}')


if __name__ == '__main__':
    main()
