"""The rollbacks a Slippi replay holds: Slippi writes every frame each time it
runs, so a frame number that goes back is a rollback, as deep as the frames
run again. What the player who recorded it saw corrected on screen.

    python tools/sweep/slp_rollbacks.py GAME.slp [...]
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from slp_compare import read_events  # noqa: E402

DEPTHS = 8  # 1..7, then 8+


def rollbacks(path):
    """{'frames': played, 'rollbacks': n, 'resimulated': frames run again,
    'depth': [count of depth 1..7, 8+]}"""
    first = newest = top = None
    n = again = 0
    depth = [0] * DEPTHS
    for c, p in read_events(path):
        if c != 0x3A:  # frame start, one per frame run
            continue
        f = struct.unpack('>i', p[1:5])[0]
        if newest is not None and f <= newest:
            d = newest - f + 1
            n += 1
            depth[min(d, DEPTHS) - 1] += 1
            again += d
        if first is None:
            first = top = f
        newest, top = f, max(top, f)  # runs forward again from here
    played = top - first + 1 if first is not None else 0
    return {'frames': played, 'rollbacks': n, 'resimulated': again, 'depth': depth}


def total(paths):
    out = {'games': 0, 'frames': 0, 'rollbacks': 0, 'resimulated': 0, 'depth': [0] * DEPTHS}
    for p in paths:
        r = rollbacks(p)
        out['games'] += 1
        for k in ('frames', 'rollbacks', 'resimulated'):
            out[k] += r[k]
        out['depth'] = [a + b for a, b in zip(out['depth'], r['depth'])]
    return out


def describe(t):
    if not t['frames']:
        return 'no frames'
    per_min = t['rollbacks'] * 3600 / t['frames']
    depths = ' '.join(f'{i + 1}{"+" if i == DEPTHS - 1 else ""}:{c}' for i, c in enumerate(t['depth']) if c)
    return (f'{t["rollbacks"]} rollbacks in {t["frames"]} frames ({per_min:.0f} a minute), '
            f'{t["resimulated"]} frames run again; by depth {depths or "-"}')


if __name__ == '__main__':
    for a in sys.argv[1:]:
        print(f'{a}: {describe(total([a]))}')
