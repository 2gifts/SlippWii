"""Finds where two replays of the same online game part ways: the Wii's own
replay (tools/wii_mirror.py) and Slippi Dolphin's (Documents/Slippi). Both
sides simulate the same inputs, so every frame's state must match; the first
frame that differs, and in which values, is where the desync began.

    python tools/slp_compare.py WII.slp DOLPHIN.slp [--context N]

Dolphin rolls back, so its replay can hold a frame more than once: the last
copy is the final one. Values that only describe the local controller (the
physical buttons and raw stick) are listed apart: they may differ by design.
"""
import argparse
import struct
import sys

# (offset from the command byte, size, format, name), per the .slp spec.
PRE = [
    (0x07, 4, '>I', 'random seed'), (0x0B, 2, '>H', 'action state'), (0x0D, 4, '>f', 'x'),
    (0x11, 4, '>f', 'y'), (0x15, 4, '>f', 'facing'), (0x3C, 4, '>f', 'percent'),
]
# The inputs the game used: a difference here shows in the state at once.
PRE_INPUTS = [
    (0x19, 4, '>f', 'stick x'), (0x1D, 4, '>f', 'stick y'), (0x21, 4, '>f', 'c-stick x'),
    (0x25, 4, '>f', 'c-stick y'), (0x29, 4, '>f', 'trigger'), (0x2D, 4, '>I', 'buttons'),
]
PRE_LOCAL = [
    (0x31, 2, '>H', 'physical buttons'), (0x33, 4, '>f', 'physical L'), (0x37, 4, '>f', 'physical R'),
    (0x3B, 1, '>b', 'raw stick x'), (0x40, 1, '>b', 'raw stick y'),
]
POST = [
    (0x07, 1, '>B', 'character'), (0x08, 2, '>H', 'action state'), (0x0A, 4, '>f', 'x'), (0x0E, 4, '>f', 'y'),
    (0x12, 4, '>f', 'facing'), (0x16, 4, '>f', 'percent'), (0x1A, 4, '>f', 'shield'),
    (0x1E, 1, '>B', 'last attack'), (0x1F, 1, '>B', 'combo count'), (0x20, 1, '>B', 'last hit by'),
    (0x21, 1, '>B', 'stocks'), (0x22, 4, '>f', 'action frame'), (0x26, 1, '>B', 'flags 1'),
    (0x27, 1, '>B', 'flags 2'), (0x28, 1, '>B', 'flags 3'), (0x29, 1, '>B', 'flags 4'), (0x2A, 1, '>B', 'flags 5'),
    (0x2F, 1, '>B', 'airborne'), (0x30, 2, '>H', 'ground id'),
    (0x32, 1, '>B', 'jumps left'), (0x33, 1, '>B', 'l-cancel'), (0x34, 1, '>B', 'hurtbox state'),
    (0x35, 4, '>f', 'self air speed x'), (0x39, 4, '>f', 'self speed y'), (0x3D, 4, '>f', 'knockback x'),
    (0x41, 4, '>f', 'knockback y'), (0x45, 4, '>f', 'self ground speed x'), (0x49, 4, '>f', 'hitlag'),
    (0x4D, 4, '>I', 'animation'), (0x51, 2, '>H', 'hit by instance'), (0x53, 2, '>H', 'instance id'),
]
POST_MISC = [(0x2B, 4, '>f', 'misc (hitstun)')]
FRAME_START = [(0x05, 4, '>I', 'random seed'), (0x09, 4, '>I', 'scene frame counter')]
ITEM = [
    (0x05, 2, '>H', 'type'), (0x07, 1, '>B', 'state'), (0x08, 4, '>f', 'facing'), (0x0C, 4, '>f', 'speed x'),
    (0x10, 4, '>f', 'speed y'), (0x14, 4, '>f', 'x'), (0x18, 4, '>f', 'y'), (0x1C, 2, '>H', 'damage taken'),
    (0x1E, 4, '>f', 'expiration'), (0x2A, 1, '>B', 'owner'),
]
# Item-specific bytes (missile type, turnip face, ...); for other items they
# can hold whatever memory held before, which differs between machines.
ITEM_SCRATCH = [
    (0x26, 1, '>B', 'misc 1'), (0x27, 1, '>B', 'misc 2'), (0x28, 1, '>B', 'misc 3'), (0x29, 1, '>B', 'misc 4'),
]
# What a desync is: these diverging (as Slippi's checksum: positions, actions;
# plus damage, stocks and the RNG, which drive everything after)
CORE = {'random seed', 'action state', 'x', 'y', 'facing', 'percent', 'character', 'shield', 'stocks',
        'airborne', 'ground id', 'jumps left', 'misc (hitstun)'}
NAMES = {0x37: 'pre-frame', 0x38: 'post-frame', 0x3A: 'frame start', 0x3B: 'item'}


def read_events(path):
    d = open(path, 'rb').read()
    i = d.find(b'raw[$U#l')
    if i < 0:
        sys.exit(f'{path}: not a .slp')
    n = struct.unpack('>I', d[i + 8:i + 12])[0]
    raw = d[i + 12:i + 12 + n] if n else d[i + 12:]
    if raw[0] != 0x35:
        sys.exit(f'{path}: no event sizes at the start')
    sizes = {}
    for k in range((raw[1] - 1) // 3):
        o = 2 + 3 * k
        sizes[raw[o]] = struct.unpack('>H', raw[o + 1:o + 3])[0]
    pos, events = 1 + raw[1], []
    while pos < len(raw) and raw[pos] in sizes:
        c = raw[pos]
        events.append((c, b'\0' + raw[pos + 1:pos + 1 + sizes[c]]))	# index = spec offset
        pos += 1 + sizes[c]
    return events


def index(path):
    """{key: payload}; later copies (rollbacks) replace earlier ones."""
    out, start = {}, None
    for c, p in read_events(path):
        if c == 0x36:
            start = p
        elif c in (0x37, 0x38):
            out[(struct.unpack('>i', p[1:5])[0], c, p[5], p[6])] = p
        elif c == 0x3A:
            out[(struct.unpack('>i', p[1:5])[0], c, 0, 0)] = p
        elif c == 0x3B:
            out[(struct.unpack('>i', p[1:5])[0], c, struct.unpack('>I', p[0x22:0x26])[0], 0)] = p
    return start, out


def field(p, off, size, fmt):
    if off + size > len(p):
        return None
    return struct.unpack(fmt, p[off:off + size])[0]


def diff(a, b, table):
    out = []
    for off, size, fmt, name in table:
        va, vb = field(a, off, size, fmt), field(b, off, size, fmt)
        if va is None or vb is None:
            continue
        if va != vb and not (isinstance(va, float) and va != va and vb != vb):
            out.append((name, va, vb))
    return out


def fmt_value(v):
    return f'{v:.6g}' if isinstance(v, float) else f'0x{v:x}' if isinstance(v, int) and v > 9 else str(v)


def who(key):
    frame, c, a, b = key
    if c in (0x37, 0x38):
        return f'{NAMES[c]} port {a + 1}{" (follower)" if b else ""}'
    if c == 0x3B:
        return f'item spawn {a}'
    return NAMES[c]


def summary(events, frame):
    parts = []
    for key in sorted(k for k in events if k[0] == frame and k[1] == 0x38):
        p = events[key]
        parts.append(f'P{key[2] + 1}{" Nana" if key[3] else ""} as 0x{field(p, 0x08, 2, ">H"):x} '
                     f'({field(p, 0x0A, 4, ">f"):.2f}, {field(p, 0x0E, 4, ">f"):.2f}) '
                     f'{field(p, 0x16, 4, ">f"):.0f}% stocks {field(p, 0x21, 1, ">B")}')
    return '; '.join(parts)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('wii')
    ap.add_argument('dolphin')
    ap.add_argument('--context', type=int, default=3, help='frames of both sides to show before the split')
    args = ap.parse_args()
    start_a, a = index(args.wii)
    start_b, b = index(args.dolphin)
    stages = [field(s, 0x13, 2, '>H') if s else None for s in (start_a, start_b)]
    if stages[0] != stages[1]:
        print(f'note: different stages ({stages[0]} / {stages[1]}): the same game?')
    frames_a = sorted({k[0] for k in a})
    frames_b = sorted({k[0] for k in b})
    print(f'Wii:     frames {frames_a[0]}..{frames_a[-1]} ({args.wii})')
    print(f'Dolphin: frames {frames_b[0]}..{frames_b[-1]} ({args.dolphin})')
    local, inputs, scratch, soft_diffs, soft_count = {}, {}, {}, {}, [0]
    first = None
    for key in sorted(set(a) & set(b)):
        tables = {0x37: PRE, 0x38: POST, 0x3A: FRAME_START, 0x3B: ITEM}[key[1]]
        d = diff(a[key], b[key], tables)
        if key[1] == 0x37:
            for item in diff(a[key], b[key], PRE_LOCAL):
                local.setdefault(item[0], key[0])
            for name, va, vb in diff(a[key], b[key], PRE_INPUTS):
                inputs.setdefault(name, (key, va, vb))
        if key[1] == 0x3B:
            for name, va, vb in diff(a[key], b[key], ITEM_SCRATCH):
                scratch.setdefault(name, (key, va, vb))
        if key[1] == 0x38:
            # The misc action value is hitstun only in hitstun (DamageHi1..
            # DamageFlyRoll, 0x4B-0x5B); in other actions it is a union that
            # can hold leftover memory (seen: Pichu, 106 frames, nothing else
            # different). Hitstun counts as state; the rest as scratch.
            for name, va, vb in diff(a[key], b[key], POST_MISC):
                if 0x4B <= (field(a[key], 0x08, 2, '>H') or 0) <= 0x5B:
                    d.append((name, va, vb))
                else:
                    scratch.setdefault('fighter misc value', (key, va, vb))
        # Core state (positions, actions, damage, stocks, the RNG, items)
        # diverging is a desync, as Slippi's own checksum sees it. Other values
        # (flags, speeds, timers) differing while the core stays the same never
        # spread: listed as soft (seen once: a flags-5 bit for one entry frame).
        soft = [x for x in d if key[1] in (0x37, 0x38) and x[0] not in CORE]
        d = [x for x in d if x not in soft]
        for name, va, vb in soft:
            soft_diffs.setdefault(name, (key, va, vb))
            soft_count[0] += 1
        if d and first is None:
            first = key[0]
        if first is not None and key[0] == first and d:
            for name, va, vb in d:
                print(f'  frame {key[0]} {who(key)}: {name}: Wii {fmt_value(va)}, Dolphin {fmt_value(vb)}')
        if first is not None and key[0] > first:
            break
    only_a = sorted({k for k in a if k not in b and k[0] <= min(frames_a[-1], frames_b[-1])})
    only_b = sorted({k for k in b if k not in a and k[0] <= min(frames_a[-1], frames_b[-1])})
    if first is None:
        print('MATCH: every frame both replays have is the same')
    else:
        print(f'DESYNC: the replays first differ at frame {first} (listed above)')
        for f in range(first - args.context, first + 2):
            print(f'  {f:6} Wii:     {summary(a, f)}')
            print(f'  {"":6} Dolphin: {summary(b, f)}')
    for name, keys in (('only in the Wii\'s', only_a), ('only in Dolphin\'s', only_b)):
        if keys:
            print(f'{len(keys)} events {name} replay, first: frame {keys[0][0]} {who(keys[0])}')
    if soft_diffs:
        print(f'soft differences (core state the same): {soft_count[0]} values')
    for title, found in (('inputs differ', inputs), ('scratch values differ (leftover memory?)', scratch),
                         ('soft difference', soft_diffs)):
        for name, (key, va, vb) in sorted(found.items(), key=lambda x: x[1][0]):
            print(f'{title}: first at frame {key[0]} {who(key)}: {name}: Wii {fmt_value(va)}, '
                  f'Dolphin {fmt_value(vb)}')
    if local:
        print('controller-only values differ (by design): ' +
              ', '.join(f'{n} from frame {f}' for n, f in sorted(local.items(), key=lambda x: x[1])))
    return 1 if first is not None else 0


if __name__ == '__main__':
    sys.exit(main())
