"""A SlippWii desync report against an opponent's Slippi replay of the same game: the same inputs and
the same setup (then the Wii's game itself went another way), or not (then the netcode or the setup).

    python tools/desync_compare.py desync-N.txt Game_....slp

The report (or any SlippWii log with a desync in it) holds, for the 12 frames up to the first
differing checksum, every input the Wii had from each opponent, and the game's setup line with its
seed (1.8.1 and later). The replay's pre-frame events hold what that PC played with. SlippWii's frame
F is the replay's frame F - 123 (checked 2026-11-19 on a game with a test's desync and no real one).
Player N in the report is port N in the replay. The files are data: nothing in them is run.
"""
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from slp_compare import read_events  # noqa: E402

OFFSET = 123


def replay(path):
    """({(frame, port): (buttons, stick x raw)}, seed, [(char, colour, team) per port])"""
    inputs, seed, ports = {}, None, []
    for c, p in read_events(path):
        if c == 0x36 and seed is None:
            gi = p[5:5 + 312]
            seed = struct.unpack('>I', p[0x13D:0x141])[0]
            for i in range(4):
                q = gi[0x60 + i * 0x24:0x60 + (i + 1) * 0x24]
                ports.append((q[0], q[3], q[9]) if q[1] != 3 else None)
        elif c == 0x37 and not p[6]:
            f = struct.unpack('>i', p[1:5])[0] + OFFSET
            inputs[(f, p[5] + 1)] = (struct.unpack('>H', p[0x31:0x33])[0], p[0x3B])
    return inputs, seed, ports


def report(path):
    text = Path(path).read_text('utf-8', 'replace')
    pads, setups, first = {}, [], None
    for line in text.splitlines():
        m = re.search(r"desync: player (\d)'s inputs, frames (-?\d+) to -?\d+: (.*)", line)
        if m:
            pl, f0 = int(m.group(1)), int(m.group(2))
            for k, h in enumerate(m.group(3).split()):
                if not h.startswith('-'):
                    pads.setdefault((f0 + k, pl), h)
        m = re.search(r'exi: the game: (.*)', line)
        if m and first is None:
            setups.append(m.group(1))	# (the last before the desync is that game's)
        m = re.search(r'checksum differs at frame (-?\d+): ours ([0-9a-f]+), player (\d)\'s ([0-9a-f]+)', line)
        if m and first is None:
            first = m.groups()
    return pads, setups, first


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    pads, setups, first = report(sys.argv[1])
    inputs, seed, ports = replay(sys.argv[2])
    if first:
        f, ours, pl, theirs = first
        ours, theirs = int(ours, 16), int(theirs, 16)
        d = (ours & 0xFFFF) - (theirs & 0xFFFF)
        print(f'first differing checksum: frame {f}, against player {pl}: positions {"the same" if not d else f"off by {d}"}, '
              f'action states {"the same" if ours >> 16 == theirs >> 16 else "differ"}')
    setup = setups[-1] if setups else ''
    m = re.search(r'seed ([0-9a-f]{8})', setup)
    if m:
        same = int(m.group(1), 16) == seed
        print(f'seed: the Wii {m.group(1)}, the replay {seed:08x}: {"SAME" if same else "DIFFERENT: the game was set up differently"}')
    else:
        print(f'seed: the replay {seed:08x}; the report has none (before 1.8.1)')
    m = re.search(r'ports: (.*)', setup)
    if m:
        wii = [None if p == '-' else tuple(int(v) for v in p.rstrip('*').split('/')[:3]) for p in m.group(1).split()]
        for i, (w, r) in enumerate(zip(wii, ports)):
            if w and r and (w[0], w[2]) != (r[0], r[2]):
                print(f'port {i + 1}: the Wii character {w[0]} team {w[2]}, the replay character {r[0]} team {r[2]}: DIFFERENT')
    same = differ = missing = 0
    for (f, pl), h in sorted(pads.items()):
        b = bytes.fromhex(h)
        r = inputs.get((f, pl))
        if r is None:
            missing += 1
        elif r == (int.from_bytes(b[0:2], 'big'), b[2]):
            same += 1
        else:
            differ += 1
            print(f'  frame {f} player {pl}: the Wii had buttons {b[0:2].hex()} stick {b[2]:02x}, '
                  f'the replay {r[0]:04x} {r[1]:02x}')
    print(f'inputs: {same} the same, {differ} different, {missing} not in the replay')
    if differ:
        print('=> the Wii played other inputs than the PC: the netcode (which frames each pad was given to)')
    elif same:
        print('=> the same inputs: the Wii\'s game went another way by itself (setup, seed, or game state)')


if __name__ == '__main__':
    main()
