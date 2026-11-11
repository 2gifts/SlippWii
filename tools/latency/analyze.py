"""Input latency from the Wii's own measurements (tools/latency_probe): reads the LAT lines of a
run's log and says how long a button press takes to reach the screen, by what was being played.

    python tools/latency/analyze.py RUN_DIR_OR_LOG [...] [--all]

Each press the probe followed gives:

  show  from the first controller reading with the button down (R1) to the retrace that put the
        first frame with it on the screen
  gap   how long before R1 that port was last read with the button up: the press came somewhere
        in between, so a finger pressing at any moment waits half of it on average before R1
        (and up to one controller poll more: `sr` ms apart, 0 = Melee's own two a field)

so press -> retrace is about show + gap / 2, the same on any display. On a CRT the light follows
the retrace by where the line is: the top of the picture about 1.3 ms later, the middle 9 ms.

With the test controller (tools/latency/tadpole) the press itself is known: its age at R1 comes in
the analog values, and replaces the gap / 2 estimate.
"""
import argparse
import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path

LAT = re.compile(r'LAT n=(\d+) p=(\d+)>(\d+) b=([0-9a-f]{4}) sc=([0-9a-f]{8}) d=(\d+) gap=(\d+) copy=(\d+) '
                 r'show=(\d+) vct=(\d+)/(\d+) rf=(\d+) rd=(\d+) cp=(\d+)')
LATX = re.compile(r'LATx n=(\d+) sr=(\d+) poll=([0-9a-f]{8}) logic=(\d+)/(\d+) ib=([0-9a-f]{12}) to=(\d+) per=(\d+)'
                  r'(?: rp=(\d+)/(\d+) jp=(\d+))?')
DIRECT = re.compile(r'PERF rb dx \| ([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8})')
RING = re.compile(r'PERF rb dy \| ([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8})')
GAME_END = re.compile(r'PERF rb w \d+/\d+/\d+ \| .* dir (\d+)/(\d+)/(\d+)(?:/(\d+))?(?:/(\d+))?')
BUTTONS = {0x0001: 'Left', 0x0002: 'Right', 0x0004: 'Down', 0x0008: 'Up', 0x0010: 'Z', 0x0020: 'R', 0x0040: 'L',
           0x0100: 'A', 0x0200: 'B', 0x0400: 'X', 0x0800: 'Y', 0x1000: 'Start'}
TOP_MS, MIDDLE_MS = 1.3, 9.0  # after the retrace: the first picture line, and the middle one


def log_of(path):
    path = Path(path)
    if path.is_dir():
        for name in ('watch.log', 'wii.log'):
            if (path / name).exists():
                return path / name
        sys.exit(f'{path}: no watch.log or wii.log')
    return path


def scene_name(scene, delay):
    major, minor = scene >> 24, scene & 0xFF
    if major == 8 and minor == 2:
        return f'online game, delay {delay}'
    if minor == 2 and major in (2, 3, 4, 5, 6, 7, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25):
        return 'offline game'
    if major == 8:
        return 'online menus'
    return 'menus'


def inband_age_us(ib):
    """The press's age at R1 from the test controller's analog values (tools/latency/tadpole: four
    nibbles of it in the sticks, in 16 us; its number and a check in the triggers), in us, or None."""
    n = [(b - 256 if b > 127 else b) + 8 for b in ib[:4]]
    if not all(0 <= v <= 15 for v in n) or not 16 <= ib[4] <= 31:
        return None
    if ib[5] != 1 + ((sum(n) + ib[4] - 16) & 15):
        return None
    return (n[0] << 12 | n[1] << 8 | n[2] << 4 | n[3]) * 16


def read(path):
    presses, extra = {}, {}
    for line in log_of(path).read_text(errors='replace').splitlines():
        m = LAT.search(line)
        if m:
            n = int(m.group(1))
            presses[n] = dict(n=n, port=int(m.group(2)), use=int(m.group(3)), bit=int(m.group(4), 16),
                              scene=int(m.group(5), 16), delay=int(m.group(6)), gap=int(m.group(7)),
                              copy=int(m.group(8)), show=int(m.group(9)), vct_r1=int(m.group(10)),
                              vct_show=int(m.group(11)), refreshes=int(m.group(12)), reads=int(m.group(13)),
                              copies=int(m.group(14)))
        m = LATX.search(line)
        if m:
            extra[int(m.group(1))] = dict(sr=int(m.group(2)), poll=int(m.group(3), 16), logic_r1=int(m.group(4)),
                                          logic_copy=int(m.group(5)), ib=bytes.fromhex(m.group(6)),
                                          timeouts=int(m.group(7)), period=int(m.group(8)),
                                          repeats=int(m.group(9) or 0), retraces=int(m.group(10) or 0),
                                          jumps=int(m.group(11) or 0))
    for n, p in presses.items():
        p.update(extra.get(n, dict(sr=-1, poll=0, logic_r1=0, logic_copy=0, ib=bytes(6), timeouts=0, period=0,
                                   repeats=0, retraces=0, jumps=0)))
        p['age'] = inband_age_us(p['ib'])
    return [presses[n] for n in sorted(presses)]


def direct_events(path):
    """The engine's notes on frames a refresh sooner (rollback.c, direct_log), a game a line."""
    game, ring, out = [], [], []
    for line in log_of(path).read_text(errors='replace').splitlines():
        m = GAME_END.search(line)
        if m:
            game, ring = [], []
            out.append((tuple(int(g) for g in m.groups() if g is not None), game, ring))
            continue
        m = RING.search(line)
        if m and out:
            w = [int(g, 16) for g in m.groups()]
            ring.append(dict(tick=w[0] >> 16, loop=w[0] >> 12 & 15, queued=w[0] >> 8 & 15, xfb=(w[0] >> 4 & 15, w[0] & 15),
                             start=w[1] / 60750, poll=w[2] / 60750, built=(w[3] & 0xFFFFFFF) / 60750 if w[3] else None,
                             flags=w[3] >> 28))
            continue
        m = DIRECT.search(line)
        if m and out:
            w = [int(g, 16) for g in m.groups()]
            game.append(dict(what=w[0] >> 28, loops=w[0] >> 20 & 0xFF, frame=w[0] & 0xFFFFF, busy=w[1] / 60750,
                             since=w[2] / 60750, run=w[3] >> 16, retraces=w[3] >> 8 & 0xFF, queued=w[3] & 0xFF))
    return out


def pct(values, q):
    values = sorted(values)
    return values[min(len(values) - 1, int(q * len(values)))]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('logs', nargs='+')
    ap.add_argument('--all', action='store_true', help='every press, one a line')
    args = ap.parse_args()
    presses = [p for log in args.logs for p in read(log)]
    if not presses:
        sys.exit('no LAT lines: was the build made with --latency, and were buttons pressed?')
    # (a gap of a second or more is a port just plugged in, or the first read of a scene)
    good = [p for p in presses if p['gap'] < 100000 and p['show'] < 500000]
    groups = defaultdict(list)
    for p in good:
        groups[scene_name(p['scene'], p['delay'])].append(p)
    periods = [p['period'] for p in good if 10000 < p['period'] < 25000]
    print(f'{len(presses)} presses followed ({len(presses) - len(good)} left out: a first reading or a stalled scene); '
          f'{presses[-1]["timeouts"]} more never reached the screen (let go too soon, or a loading screen).')
    if periods:
        print(f'A refresh: {statistics.median(periods) / 1000:.2f} ms by the timebase (16.68 expected).')
    print()
    print('| Playing | Presses | Reading -> retrace (ms): median, 10%-90% | Reading gap | Press -> retrace | '
          '-> top of screen | -> middle |')
    print('|---|---|---|---|---|---|---|')
    for name in sorted(groups):
        g = groups[name]
        show = [p['show'] / 1000 for p in g]
        gap = statistics.mean(p['gap'] for p in g) / 1000
        ages = [p['age'] for p in g if p['age'] is not None]
        if len(ages) == len(g):  # the test controller: the press itself
            total = [(p['show'] + p['age']) / 1000 for p in g]
            press = statistics.mean(total)
            how = f'{press:.1f} (measured)'
        else:
            press = statistics.mean(show) + gap / 2
            how = f'{press:.1f} (+ half the gap)'
        print(f'| {name} | {len(g)} | {statistics.median(show):.1f}, {pct(show, 0.1):.1f}-{pct(show, 0.9):.1f} | '
              f'{gap:.1f} ms | {how} | {press + TOP_MS:.1f} | {press + MIDDLE_MS:.1f} |')
    print()
    for name in sorted(groups):
        g = groups[name]
        by = defaultdict(int)
        for p in g:
            by[p['refreshes']] += 1
        srs = sorted({p['sr'] for p in g})
        print(f'{name}: refreshes from the reading to the picture: '
              + ', '.join(f'{k}: {v}' for k, v in sorted(by.items()))
              + f'; controller polls sr={",".join(map(str, srs))}; frames drawn before it: '
              + ', '.join(f'{k}: {v}' for k, v in sorted(_count(p["copies"] for p in g).items())))
    # what the screen did, by scene: its counters start again with each scene, so a scene's last press has its totals
    screen = defaultdict(lambda: [0, 0, 0])
    for i, p in enumerate(good):
        last = i + 1 == len(good) or good[i + 1]['retraces'] < p['retraces'] or             scene_name(good[i + 1]['scene'], good[i + 1]['delay']) != scene_name(p['scene'], p['delay'])
        if last and p['retraces']:
            t = screen[scene_name(p['scene'], p['delay'])]
            t[0] += p['retraces']
            t[1] += p['repeats']
            t[2] += p['jumps']
    for name in sorted(screen):
        n, repeats, jumps = screen[name]
        print(f'{name}: the screen, over {n / 3596:.1f} min in play: {repeats} frames shown twice '
              f'({repeats * 3596 / n:.1f} a minute), {jumps} jumps ({jumps * 3596 / n:.1f} a minute)')
    games = [g for log in args.logs for g in direct_events(log)]
    if any(counts[0] for counts, _, _ in games):
        print()
        print('Frames a refresh sooner, a game a line: skips asked/runs level ended/frames drawn behind/passes too '
              "long for a frame level/the opponent's number it began with (over 2: no skip), then each "
              'event (the first 15): the frame, and for a run ended its length, the late frame: its work in ms '
              '(engine loops), the time from its poll to its render built, retraces in that, polls queued; for a '
              'pass too long for a frame level, its work and the retraces from its poll to its render built (2: late '
              'if the game was level)')
        for n, (counts, events, ring) in enumerate(games, 1):
            print(f'  game {n:2}: {"/".join(map(str, counts))}  ' + '  '.join(
                f'skip@{e["frame"]}' if e['what'] == 2 else
                f'one-a-loop@{e["frame"]}' if e['what'] == 3 else
                f'two-in-one@{e["frame"]}' if e['what'] == 4 else
                f'long@{e["frame"]}: {e["busy"]:.1f} ms ({e["loops"]} loops), {e["retraces"]} retraces'
                if e['what'] == 5 else
                f'ENDED@{e["frame"]} after {e["run"]}: work {e["busy"]:.1f} ms ({e["loops"]} loops), '
                f'{e["since"]:.1f} ms from the poll, {e["retraces"]} retraces, {e["queued"]} queued'
                for e in events))
            if ring and args.all:
                # (the buffers' states: HSD_VI's, 4 its draw not done, 5 done, 6 next on the screen)
                print('            the ticks before its first run ended: tick, its loop in the pass, polls queued, the '
                      'states of the two buffers; started ms after a retrace, ms after the last poll; render built ms after a '
                      'retrace (flags: 1 an older frame in buffer 0, 2 in 1, 4 a frame held at the retrace, 8 nudging)')
                for t in ring:
                    built = f'built {t["built"]:5.1f} flags {t["flags"]:x}' if t['built'] is not None else 'no render'
                    print(f'            {t["tick"] & 0xFFFF:5} loop {t["loop"]} queued {t["queued"]} buffers '
                          f'{t["xfb"][0]}/{t["xfb"][1]}  start {t["start"]:5.1f} after a retrace, {t["poll"]:5.1f} '
                          f'after the poll  {built}')
    if args.all:
        print()
        for p in presses:
            print(f'{p["n"]:4} {scene_name(p["scene"], p["delay"]):22} {BUTTONS.get(p["bit"], hex(p["bit"])):6} '
                  f'port {p["port"]}>{p["use"]} gap {p["gap"] / 1000:6.2f} copy {p["copy"] / 1000:6.2f} '
                  f'show {p["show"] / 1000:6.2f} ms, {p["refreshes"]} refreshes, lines {p["vct_r1"]}/{p["vct_show"]}, '
                  f'{p["reads"]} reads, logic {p["logic_copy"] - p["logic_r1"]} frames, sr {p["sr"]} '
                  f'poll {p["poll"]:08x} ib {p["ib"].hex()}')


def _count(values):
    out = defaultdict(int)
    for v in values:
        out[v] += 1
    return out


if __name__ == '__main__':
    main()
