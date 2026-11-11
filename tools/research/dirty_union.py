"""Which parts of Slippi's savestate regions a match ever writes: plays
replays in a copy of Slippi's playback Dolphin (unthrottled), snapshots the
regions as often as it can, and prints the union of the 4 KB pages (and
32-byte lines) that changed, per replay and over all of them, with the
page list for a savestate region table (docs/research/ROLLBACK.md).

    python tools/research/dirty_union.py REPLAY.slp [REPLAY.slp ...] [--out pages.txt]
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/sweep'))
import dolphin_mem  # noqa: E402

ISO = os.environ.get('MELEE_ISO', 'SSBMv102.iso')
FIXED = [(0x80005520, 0x80005940), (0x803B7240, 0x804DEC00), (0x8065C000, 0x8071B000)]
PAGE, LINE = 4096, 32


def playback_copy():
    dst = ROOT / 'build/sweep/dolphin-playback'
    if not dst.exists():
        shutil.copytree(Path(os.environ['APPDATA']) / 'Slippi Launcher/playback', dst)
    ini = dst / 'User/Config/Dolphin.ini'
    text = ini.read_text(encoding='utf-8') if ini.exists() else '[Core]\n'
    if 'EmulationSpeed' not in text:
        text = text.replace('[Core]\n', '[Core]\nEmulationSpeed = 0.00000000\n', 1)
    ini.write_text(text, encoding='utf-8')
    return dst


def one(dolphin, replay):
    comm = dolphin / 'comm.json'
    comm.write_text(json.dumps({'mode': 'normal', 'replay': str(replay), 'isRealTimeMode': False,
                                'commandId': str(time.time())}))
    proc = subprocess.Popen([str(dolphin / 'Slippi Dolphin.exe'), '-i', str(comm), '-e', ISO])
    try:
        d = None
        for _ in range(60):
            try:
                d = dolphin_mem.Dolphin(proc.pid)
                break
            except Exception:
                time.sleep(1)
        t = time.time()
        while time.time() - t < 120 and not (d.u8(0x80479D30) == 0x0E and d.u32(0x80479D60) > 120):
            time.sleep(0.1)
        regions = FIXED + [(d.u32(0x804D76B8), d.u32(0x804D76BC))]
        pages, lines = set(), set()
        prev = [d.read(a, b - a) for a, b in regions]
        first_frame = d.u32(0x80479D60)
        last_frame = first_frame
        moved = time.time()
        while d.u8(0x80479D30) == 0x0E and time.time() - moved < 3:
            f = d.u32(0x80479D60)
            if f > last_frame:
                moved = time.time()
            cur = [d.read(a, b - a) for a, b in regions]
            for (a, _), x, y in zip(regions, prev, cur):
                if x == y:
                    continue
                for i in range(0, len(x), PAGE):
                    if x[i:i + PAGE] != y[i:i + PAGE]:
                        pages.add(a + i)
                        for j in range(i, min(i + PAGE, len(x)), LINE):
                            if x[j:j + LINE] != y[j:j + LINE]:
                                lines.add(a + j)
            prev = cur
            last_frame = max(last_frame, d.u32(0x80479D60))
        total = sum(b - a for a, b in regions)
        return pages, lines, total, last_frame - first_frame, regions
    finally:
        subprocess.run(['taskkill', '/F', '/PID', str(proc.pid)], capture_output=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('replays', nargs='+')
    ap.add_argument('--out')
    args = ap.parse_args()
    dolphin = playback_copy()
    all_pages, all_lines = set(), set()
    for r in args.replays:
        pages, lines, total, frames, regions = one(dolphin, Path(r).resolve())
        all_pages |= pages
        all_lines |= lines
        print(f'{Path(r).name}: {frames} frames; pages {len(pages) * PAGE / 1e6:.2f} MB, '
              f'lines {len(lines) * LINE / 1e6:.2f} MB of {total / 1e6:.2f} MB; heap {regions[3][0]:08x}-{regions[3][1]:08x}; '
              f'union so far: pages {len(all_pages) * PAGE / 1e6:.2f} MB')
    print(f'ALL {len(args.replays)} replays: pages {len(all_pages) * PAGE / 1e6:.2f} MB, lines {len(all_lines) * LINE / 1e6:.2f} MB')
    if args.out:
        Path(args.out).write_text(''.join(f'{p:08x}\n' for p in sorted(all_pages)))


if __name__ == '__main__':
    main()
