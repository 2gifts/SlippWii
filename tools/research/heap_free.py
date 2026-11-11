"""How much of Melee's memory is free during a match: walks the OS heaps'
free lists (OSAlloc: HeapArray of {size, free, allocated}, cells of {prev,
next, size}) while a replay plays in a copy of Slippi's playback Dolphin.
Free heap is where a console savestate could live in fast MEM1
(docs/research/ROLLBACK.md).

    python tools/research/heap_free.py REPLAY.slp [--samples 8]
"""
import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/sweep'))
sys.path.insert(0, str(ROOT / 'tools/research'))
import dolphin_mem  # noqa: E402
from dirty_union import playback_copy, ISO  # noqa: E402

HEAP_ARRAY, NUM_HEAPS = 0x804D7360, 0x804D7364


def heaps(d):
    arr, n = d.u32(HEAP_ARRAY), d.u32(NUM_HEAPS)
    out = []
    for i in range(min(n, 16)):
        size, free = d.u32(arr + i * 12), d.u32(arr + i * 12 + 4)
        if size == 0xFFFFFFFF or size & 0x80000000:
            continue  # unused slot
        total, largest, cell, count = 0, 0, free, 0
        while 0x80000000 <= cell < 0x81800000 + 0x400000 and count < 10000:
            sz = d.u32(cell + 8)
            total += sz
            largest = max(largest, sz)
            cell = d.u32(cell + 4)
            count += 1
        out.append((i, size, total, largest, count))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('replay')
    ap.add_argument('--samples', type=int, default=8)
    args = ap.parse_args()
    dolphin = playback_copy()
    comm = dolphin / 'comm.json'
    comm.write_text(json.dumps({'mode': 'normal', 'replay': str(Path(args.replay).resolve()),
                                'isRealTimeMode': False, 'commandId': str(time.time())}))
    proc = subprocess.Popen([str(dolphin / 'Slippi Dolphin.exe'), '-i', str(comm), '-e', ISO])
    try:
        d = None
        for _ in range(60):
            try:
                d = dolphin_mem.Dolphin(proc.pid)
                break
            except Exception:
                time.sleep(1)
        while not (d.u8(0x80479D30) == 0x0E and d.u32(0x80479D60) > 120):
            time.sleep(0.1)
        print(f'arena {d.u32(0x804D7368):08x}-{d.u32(0x804D736C):08x}, '
              f'HSD heap {d.u32(0x804D76B8):08x}-{d.u32(0x804D76BC):08x}')
        for _ in range(args.samples):
            f = d.u32(0x80479D60)
            hs = heaps(d)
            print(f'frame {f}: ' + '; '.join(f'heap {i}: {size / 1e6:.2f} MB, free {total / 1e6:.2f} MB '
                                              f'(largest {largest / 1e6:.2f} MB, {count} blocks)'
                                              for i, size, total, largest, count in hs))
            if d.u8(0x80479D30) != 0x0E:
                break
            time.sleep(0.5)
    finally:
        subprocess.run(['taskkill', '/F', '/PID', str(proc.pid)], capture_output=True)


if __name__ == '__main__':
    main()
