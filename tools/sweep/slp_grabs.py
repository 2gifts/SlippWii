"""How much grabbing a Slippi replay holds: per player, the frames spent catching (action states
0xD4-0xDE) and caught (0xDF-0xE8), and the grabs that held (CatchWait, 0xD8, entered). For the
sweep's --grabby bot: a check that its games are made of what it is there for.

    python tools/sweep/slp_grabs.py GAME.slp [...]
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from slp_compare import read_events  # noqa: E402


def grabs(path):
    """{port: [catching frames, caught frames, grabs held, throws]} from post-frame updates (0x38),
    the last one written for each frame (a rollback writes a frame again)."""
    last = {}
    for c, p in read_events(path):
        if c != 0x38:
            continue
        f = struct.unpack('>i', p[1:5])[0]  # (p[0] is the command byte)
        port, follower = p[5], p[6]
        if follower:
            continue
        state = struct.unpack('>H', p[8:10])[0]
        last[(f, port)] = state
    out = {}
    prev = {}
    for (f, port), state in sorted(last.items()):
        o = out.setdefault(port, [0, 0, 0, 0])
        if 0xD4 <= state <= 0xDE:
            o[0] += 1
        if 0xDF <= state <= 0xE8:
            o[1] += 1
        before = prev.get(port)
        if state == 0xD8 and before != 0xD8 and before != 0xD9:
            o[2] += 1
        if 0xDB <= state <= 0xDE and not (before is not None and 0xDB <= before <= 0xDE):
            o[3] += 1
        prev[port] = state
    return out


def main():
    for name in sys.argv[1:]:
        g = grabs(name)
        parts = [f'port {p + 1}: catching {v[0]}, caught {v[1]}, grabs {v[2]}, throws {v[3]}' for p, v in sorted(g.items())]
        print(f'{Path(name).name}: ' + '; '.join(parts))


if __name__ == '__main__':
    main()
