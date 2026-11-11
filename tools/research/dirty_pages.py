"""Measure how much of Slippi's savestate regions changes per frame during a
real match: play a replay in a copy of the playback Dolphin and diff memory.

    python tools/research/dirty_pages.py REPLAY.slp   (Windows; copies the playback Dolphin into build/)
"""
import json, shutil, subprocess, sys, time, os
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/sweep'))
import dolphin_mem as m, unthrottle

replay = Path(sys.argv[1]).resolve()
src = Path(os.environ['APPDATA']) / 'Slippi Launcher/playback'
dst = ROOT / 'build/sweep/dolphin-playback'
if not dst.exists():
    shutil.copytree(src, dst)
comm = dst / 'comm.json'
comm.write_text(json.dumps({'mode': 'normal', 'replay': str(replay), 'isRealTimeMode': False,
                            'commandId': str(time.time())}))
proc = subprocess.Popen([str(dst / 'Slippi Dolphin.exe'), '-i', str(comm), '-e', os.environ.get('MELEE_ISO', 'SSBMv102.iso')])
time.sleep(4)
try:
    unthrottle.unthrottle(proc.pid)
except Exception:
    pass
d = None
for _ in range(60):
    try:
        d = m.Dolphin(proc.pid)
        break
    except Exception:
        time.sleep(1)

REGIONS = [(0x80005520, 0x80005940), (0x803b7240, 0x804DEC00), (0x8065c000, 0x8071b000)]


def heap():
    return d.u32(0x804d76b8), d.u32(0x804d76bc)


def in_game():
    return d.u8(0x80479D30) == 0x0E or d.u8(0x80479D30) == 0x02  # VS in-game (playback uses VS)


# wait for the match
t = time.time()
while time.time() - t < 90:
    if d.u32(0x80479D60) > 200 and d.u8(0x80479D30) == 0x0E:
        break
    time.sleep(0.2)
regions = REGIONS + [heap()]
total = sum(b - a for a, b in regions)
print('scene %08x frame %d; regions %.2f MB (heap %08x-%08x)' % (d.u32(0x80479D30), d.u32(0x80479D60), total / 1e6, *regions[3]))

LINE, PAGE = 32, 4096


def snap():
    f = d.u32(0x80479D60)
    data = [d.read(a, b - a) for a, b in regions]
    f2 = d.u32(0x80479D60)
    return f, f2, data


def diff(a, b, unit):
    n = 0
    for x, y in zip(a, b):
        for i in range(0, len(x), unit):
            if x[i:i + unit] != y[i:i + unit]:
                n += 1
    return n


ever_pages = set()
results = []
prev = snap()
first = prev
t0=time.time()
k=0
while time.time()-t0 < float(os.environ.get('DIRTY_SECONDS','60')) and d.u8(0x80479D30) == 0x0E:
    k+=1
    time.sleep(0.15)
    cur = snap()
    if cur[0] != cur[1] or prev[0] != prev[1]:
        prev = cur
        continue  # torn snapshot: the frame advanced while reading
    df = cur[0] - prev[0]
    if df <= 0:
        prev = cur
        continue
    lines = diff(prev[2], cur[2], LINE)
    pages = 0
    for ri, (x, y) in enumerate(zip(prev[2], cur[2])):
        base = regions[ri][0]
        for i in range(0, len(x), PAGE):
            if x[i:i + PAGE] != y[i:i + PAGE]:
                pages += 1
                ever_pages.add((base + i) // PAGE)
    results.append((df, lines * LINE, pages * PAGE))
    prev = cur
import statistics
one=[r for r in results if r[0]==1]
print(f'samples {len(results)}; per 1-frame step: lines KB median {statistics.median([r[1] for r in one])/1024 if one else -1:.0f} max {max([r[1] for r in one])/1024 if one else -1:.0f}; pages KB median {statistics.median([r[2] for r in one])/1024 if one else -1:.0f} max {max([r[2] for r in one])/1024 if one else -1:.0f}')
print(f'any step: lines KB max {max(r[1] for r in results)/1024:.0f}, pages KB max {max(r[2] for r in results)/1024:.0f}')
print(f'pages that changed at all over the run: {len(ever_pages) * PAGE / 1e6:.2f} MB of {total / 1e6:.2f} MB')
print('last frame', d.u32(0x80479D60))
subprocess.run(['taskkill', '/F', '/PID', str(proc.pid)], capture_output=True)
