#!/usr/bin/env python3
"""Reads RB_TRACE's record (rollback.c) from a Wii log: what the player saw and heard in each game.

    trace_view.py LOG [--events] [--around SECONDS]

For each game: frames by how far the picture moved, refreshes from poll to screen, retraces by
how late they were handled, gaps in the sound's interrupt, how long before its refresh each
frame was drawn. Then what happened in the moments before each of the player's marks (a D-pad
press when something felt wrong), and how often each kind of event happens near a mark
compared with anywhere else. With --events, every event."""
import argparse
import re
from collections import Counter
from pathlib import Path

KINDS = {0: 'poll-state', 1: 'repeat', 2: 'jump', 3: 'lag', 4: 'load', 5: 'pass', 6: 'late-vi', 7: 'audio', 8: 'wait', 9: 'poll',
         10: 'MARK', 11: 'capture', 12: 'slow-frame', 13: 'second', 14: 'SHIFT', 15: 'disc'}
REFRESH_S = 1 / 59.94
LIST = re.compile(r'PERF rb T(head|move|lag |line|audi|behi|slac|cost|scrn)\s*\|\s*([\d ]*)')
EVENTS = re.compile(r'PERF rb E([ 0-9a-f]*)\|([ 0-9a-f]*)')


def describe(kind, a, b):
    return {0: (f'frames {"start" if a == 0xFE else "stop"} ending a line early, at frame {b}' if a in (0xFD, 0xFE) else
                f'late retrace: vi_retrace called from {b:08x}' if a == 0xFA else
                f'  PI cause {b >> 16:04x} mask {b & 0xFFFF:04x} (PE finish 0x400, VI 0x100)' if a == 0xF8 else
                f'  VI at {["load in","load out","capture in"][a-0xF5]}: line {b>>16} DI0..3 {b&1}{b>>1&1}{b>>2&1}{b>>3&1} PI VI cause {b>>4&1} mask {b>>5&1} EE {b>>6&1}' if a in (0xF5,0xF6,0xF7) else
                f'late retrace: VI line {b & 0x7FF} as vi_retrace began, path {b >> 16}' if a == 0xF9 else
                f'pad read now {b} us before the retrace' if a == 0xF4 else
                f'nudge over: {b >> 16} frames ended a line early, the pad read at least {b & 0xFFFF} us early' if a == 0xF3 else
                (f'late retrace interrupted at {b:08x}' if a == 0xFC else f'late retrace: VI line {b >> 16 & 0x7FF} at its interrupt{" (through)" if b >> 31 else ""}, {b & 0xFFFF} us after the AI') if a in (0xFB, 0xFC) else
                f'  saved {b >> 16} end {b & 0xFFFF}' if a == 0xFF else
                f'poll {b >> 16}: game at {b & 0xFFFF}, active {a & 1} load {a >> 1 & 1} busy {a >> 2 & 1} EE {a >> 3 & 1} queue {a >> 4 & 7} nested {a >> 7}'),
            1: f'frame {b & 0xFFFF} shown again (buffers {a >> 4},{a & 15}; held {b >> 16 & 1} busy {b >> 17 & 1} older {b >> 18 & 1} queue {b >> 19})',
            2: f'picture moved {a - 256 if a > 127 else a} frames, to {b}',
            3: f'poll to screen now {a} refreshes',
            4: f'rollback load, depth {a}, {b} us with interrupts off',
            5: f'main loop pass: {a} engine loops, {b} us',
            6: f'retrace {b} us after the last, VI line {a}',
            7: f'sound interrupt {b} us after the last, DSP state {a}',
            8: f'waited {b} us for an input ({"got it" if a else "in vain"})',
            9: f'poll took {b} us',
            10: f'*** the player\'s mark (D-pad {a:#x}), frame {b}',
            11: f'capture of {a} pages, {b} us with interrupts off',
            12: f'frame took {b} us from poll to drawn, {a / 4} ms of it to the copy',
            13: f'this second: poll to copy at most {a / 4} ms; poll to drawn at most {b >> 16}, on average {b & 0xFFFF} us',
            14: f'*** frame numbers shifted: an idle poll is now {a - 256 if a > 127 else a} from the game, frame {b}',
            15: f'  {chr(a)} {b} us before' if a else f'{b} polls found the disc busy in this second'}[kind]


def games(path):
    game, out = None, []
    for line in Path(path).read_text(encoding='utf-8', errors='replace').splitlines():
        m = LIST.search(line)
        if m:
            name, values = m[1].strip(), [int(x) for x in m[2].split()]
            if name == 'head':
                game = {'refreshes': values[0], 'count': values[1], 'lost': values[2], 'events': []}
                out.append(game)
            elif game is not None:
                game[name] = values
            continue
        m = EVENTS.search(line)
        if m and game is not None:
            for word in (m[1] + ' ' + m[2]).split():
                w0, b = int(word[:8], 16), int(word[8:], 16)
                game['events'].append((w0 >> 12, (w0 >> 8) & 0xF, w0 & 0xFF, b))
    return out


def show(g, number, args):
    print(f'=== game {number}: {g["refreshes"]} refreshes ({g["refreshes"] * REFRESH_S:.0f} s), '
          f'{g["count"]} events{f", {g["lost"]} LOST" if g["lost"] else ""}')
    moved = g.get('move', [0] * 5)
    print(f'  shown: {moved[1]} next frame, {moved[0]} repeats, {moved[2]} moved 2, {moved[3]} moved 3, {moved[4]} other')
    print(f'  poll to screen, refreshes 0..7+: {g.get("lag")}')
    print(f'  retraces by VI lines late (0,1,2-3,..128-255,256+): {g.get("line")}')
    print(f'  sound interrupt gaps (<5.5,6,7,8,10,15,more ms; 10 on: a block late): {g.get("audi")}; DSP behind {g.get("behi", [0])[0]}')
    print(f'  frames drawn, ms before their refresh (late,<1,<2..6+): {g.get("slac")}')
    print(f'  frames drawn, whole ms from their poll (0..18, more): {g.get("cost")}')
    print(f'  frames on screen, whole ms from their poll (14..32, more): {g.get("scrn")}')
    if args.seconds:
        for r, k, a, b in g['events']:
            if k == 13:
                print(f'    {r * REFRESH_S:7.1f} s  to copy at most {a / 4:5.2f} ms; to drawn at most {(b >> 16) / 1000:6.2f}, average {(b & 0xFFFF) / 1000:6.2f} ms')
    g['events'] = [e for e in g['events'] if e[1] != 13]
    print('  events:', ', '.join(f'{KINDS[k]} {n}' for k, n in sorted(Counter(e[1] for e in g['events']).items())))
    start = 240  # the game's start is its own story
    play = [e for e in g['events'] if e[0] > start]
    print('  after the first 4 s:', ', '.join(f'{KINDS[k]} {n}' for k, n in sorted(Counter(e[1] for e in play).items())))
    if args.events:
        for r, k, a, b in g['events']:
            print(f'  {r:7d} {r * REFRESH_S:7.2f}s {describe(k, a, b)}')
        return
    marks = [e for e in g['events'] if e[1] == 10]
    window = int(args.around / REFRESH_S)
    near = Counter()
    for m in marks:
        print(f'  --- mark at {m[0] * REFRESH_S:.2f} s, the {args.around} s before it:')
        last = None
        for r, k, a, b in g['events']:
            if m[0] - window <= r <= m[0] + 6:
                text = describe(k, a, b)
                if text != last:
                    print(f'      {(r - m[0]) * REFRESH_S:+.3f} s  {text}')
                last = text
                near[k] += 1
    if marks:
        share = len(marks) * window / max(g['refreshes'], 1)
        print(f'  near marks ({100 * share:.0f}% of the game) vs in all:',
              ', '.join(f'{KINDS[k]} {near[k]}/{n}' for k, n in sorted(Counter(e[1] for e in g['events']).items()) if k != 10))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('log')
    ap.add_argument('--events', action='store_true', help='every event')
    ap.add_argument('--around', type=float, default=1.0, help='seconds before a mark to show')
    ap.add_argument('--seconds', action='store_true', help="the frames' cost, second by second")
    args = ap.parse_args()
    found = games(args.log)
    if not found:
        raise SystemExit('no RB_TRACE record in this log')
    for i, g in enumerate(found, 1):
        show(g, i, args)


if __name__ == '__main__':
    main()
