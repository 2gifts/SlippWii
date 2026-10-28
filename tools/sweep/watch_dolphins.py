"""Prints what each running Slippi Dolphin is doing: scene, the autoplay's
round and step, and its game frame. For developing the sweep.

    python tools/sweep/watch_dolphins.py [SECONDS] [EVERY]
"""
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dolphin_mem  # noqa: E402

BLOB = Path(__file__).resolve().parents[2] / 'build/sweep/autoplay-a.c2.bin'
BOT_OFFSET = 8 + 0x40  # the C2 header, then the code's bot_data (build_autoplay.py prints it)
SCENES = {0x0008: 'css', 0x0108: 'sss', 0x0208: 'game', 0x0308: 'results', 0x0408: 'vs', 0x0001: 'menu'}


def dolphins():
    out = []
    for line in subprocess.run(['tasklist'], capture_output=True, text=True).stdout.splitlines():
        if 'Slippi Dolphin' in line:
            pid = int(line.split()[2])
            try:
                d = dolphin_mem.Dolphin(pid)
            except OSError:
                continue
            mem = d.read(0x80000000, 0x1800000)
            sig = BLOB.read_bytes()[8:40] if BLOB.exists() else None
            i = mem.find(sig) if sig else -1
            out.append((pid, d, 0x80000000 + i - 8 + BOT_OFFSET if i >= 0 else None))
    return out


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 60
    every = float(sys.argv[2]) if len(sys.argv) > 2 else 5
    ds = dolphins()
    end = time.time() + seconds
    while time.time() < end:
        row = []
        for pid, d, bot in ds:
            w = d.u32(0x80479D30)
            key = ((w & 0xFF) << 8) | (w >> 24)
            s = f'{pid}: {SCENES.get(key, hex(key))}'
            if bot:
                s += f' round {d.u8(bot + 0xA0)} step {d.u8(bot + 0xA1)} t {d.u32(bot + 0x98)}'
            row.append(s)
        print(time.strftime('%X'), ' | '.join(row), flush=True)
        time.sleep(every)


if __name__ == '__main__':
    main()
