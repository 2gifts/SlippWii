"""Scores sweep runs (run_sweep.py) for a comparison against main: what the PC player gets, and the
Wii's own smoothness, by network condition. The table it prints is the one the pull request
template asks for (docs/TESTING.md).

    python tools/sweep/score.py --base RUN [RUN ...] --change RUN [RUN ...]
    python tools/sweep/score.py RUN [RUN ...]            (one side only)

RUN is a run's folder or its name under build/sweep/runs. Runs are grouped by the relay's delay and
jitter (relay.log), so pass the same nets and seeds on both sides.

The Wii's smoothness comes from the kernel's own lines, which every build writes:
  "game pacing: N polls over 20 ms apart": a frame shown twice, or one that came late;
  "game checksums: ... N differ": the Wii's game against its opponent's (a desync if any differ).
(Not the stall profiler's SHOWN counters: rollback rewinds the counter they read, and the profiler
itself delays frames. Measure with --no-profiler.)
"""
import argparse
import re
import statistics
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
RUNS = HERE.parents[1] / 'build/sweep/runs'
sys.path.insert(0, str(HERE))
import ab  # noqa: E402


def run_dir(name):
    p = Path(name)
    return p if p.is_dir() else RUNS / name


def net_of(run):
    """'30,5' from the relay's first line, or '?'."""
    log = run / 'relay.log'
    m = re.search(r'delay ([\d.]+) ms, jitter ([\d.]+) ms', log.read_text('utf-8', 'replace')) if log.exists() else None
    return f'{float(m.group(1)):g},{float(m.group(2)):g}' if m else '?'


def wii_side(run):
    log = (run / 'wii.log').read_text('utf-8', 'replace') if (run / 'wii.log').exists() else ''
    slow = sum(int(n) for n in re.findall(r'game pacing: (\d+) polls over 20 ms apart', log))
    compared = re.findall(r'game checksums: (\d+) frames compared with the opponent\'s, (\d+) differ', log)
    games = len(re.findall(r'game over:', log))
    waited = sum(int(n) for n in re.findall(r'game over: \d+ frames .*?waited (\d+) frames', log))
    return {'slow': slow, 'games': games, 'waited': waited,
            'desynced': sum(1 for _, d in compared if int(d)), 'checked': len(compared)}


def score(run):
    f, w = ab.felt(run), wii_side(run)
    q = f['mq'] or {}
    return {'net': net_of(run), 'run': run.name, 'games': w['games'], 'slow': w['slow'], 'waited': w['waited'],
            'desynced': w['desynced'], 'checked': w['checked'], 'rb_min': f['rb_min'], 'depth': f['depth'],
            'halts': f['halts_play'], 'unhealthy': q.get('unhealthy', 0), 'intervals': q.get('intervals', 0)}


def side(names):
    out = []
    for name in names:
        run = run_dir(name)
        try:
            out.append(score(run))
        except Exception as e:  # noqa: BLE001
            print(f'{run.name}: not scored ({e})', file=sys.stderr)
    return out


def cell(rows, net):
    """One side's numbers for a net: sums, and means of the PC's per-minute figures."""
    r = [x for x in rows if x['net'] == net]
    if not r:
        return None
    return {'n': len(r), 'games': sum(x['games'] for x in r), 'slow': sum(x['slow'] for x in r),
            'waited': sum(x['waited'] for x in r), 'desynced': sum(x['desynced'] for x in r),
            'checked': sum(x['checked'] for x in r),
            'rb_min': statistics.mean(x['rb_min'] for x in r), 'depth': statistics.mean(x['depth'] for x in r),
            'halts': statistics.mean(x['halts'] for x in r),
            'unhealthy': sum(x['unhealthy'] for x in r), 'intervals': sum(x['intervals'] for x in r)}


def text(c):
    if c is None:
        return '-'
    return (f"{c['slow']} slow frames in {c['games']} games; PC {c['rb_min']:.0f} rollbacks/min, depth {c['depth']:.2f}, "
            f"halts {c['halts']:.1f}/game, unhealthy {c['unhealthy']} of {c['intervals']}; "
            f"desynced {c['desynced']} of {c['checked']}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('runs', nargs='*')
    ap.add_argument('--base', nargs='+', default=[])
    ap.add_argument('--change', nargs='+', default=[])
    args = ap.parse_args()
    if not (args.runs or args.base or args.change):
        ap.error('no runs')
    base, change = side(args.base or args.runs), side(args.change)
    for x in base + change:
        print(f"{x['run']}  net {x['net']:6s} games {x['games']}  slow {x['slow']:3d}  waited {x['waited']:3d}  "
              f"PC rb/min {x['rb_min']:5.0f} depth {x['depth']:.2f} halts {x['halts']:.1f}  "
              f"unhealthy {x['unhealthy']}/{x['intervals']}  desynced {x['desynced']}/{x['checked']}")
    nets = sorted({x['net'] for x in base + change})
    print()
    if not change:
        for net in nets:
            print(f'{net}: {text(cell(base, net))}')
        return
    print('| Net (one way, jitter) | | `main` | this change |')
    print('|---|---|---|---|')
    for net in nets:
        b, c = cell(base, net), cell(change, net)
        def pair(k, fmt='{:.0f}'):
            return [fmt.format(v[k]) if v else '-' for v in (b, c)]
        lines = [
            ('Wii: slow frames (polls over 20 ms) / games', [f"{v['slow']} / {v['games']}" if v else '-' for v in (b, c)]),
            ('Wii: frames waited for inputs', pair('waited')),
            ('PC: rollbacks per minute', pair('rb_min')),
            ('PC: average rollback depth', pair('depth', '{:.2f}')),
            ('PC: halts at the rollback limit per game', pair('halts', '{:.1f}')),
            ('PC: unhealthy intervals (Slippi\'s rule)', [f"{v['unhealthy']} of {v['intervals']}" if v else '-' for v in (b, c)]),
            ('Games desynced', [f"{v['desynced']} of {v['checked']}" if v else '-' for v in (b, c)]),
        ]
        for label, (x, y) in lines:
            print(f'| {net} | {label} | {x} | {y} |')


if __name__ == '__main__':
    main()
