"""What a PC player gets against the Wii, against what it gets against a PC:
the same games through the same connections (run_sweep.py --net), once with a
second Slippi Dolphin in the Wii's place (--baseline), once with the Wii.

    python tools/sweep/ab.py --wii-ip WII_IP [--rounds 4] [--nets 15,3 30,5 45,5] [--pc-nets 32,5]

--pc-nets runs a PC only. A Wii answers about 3 ms slower than a PC, so the same wire
shows it a higher ping: these are for comparing the two at the same ping.

Writes build/sweep/ab-<stamp>.md, the PC player's side of each run: Slippi
Dolphin's own log (halts, time sync, speed) and its own replays (rollbacks).
"""
import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
RUNS = ROOT / 'build/sweep/runs'
UNFREEZE_FRAME = 84  # (Slippi's UNFREEZE_INPUTS_FRAME) up to here the players cannot move
sys.path.insert(0, str(HERE))
import slp_rollbacks  # noqa: E402


def sweep(wii_ip, rounds, net, extra):
    """Runs one sweep; (its run directory, its exit code)."""
    before = set(RUNS.iterdir())
    code = subprocess.run([sys.executable, HERE / 'run_sweep.py', '--wii-ip', wii_ip, '--rounds', str(rounds),
                           '--net', net, *extra], capture_output=True, text=True).returncode
    return max(set(RUNS.iterdir()) - before, key=lambda p: p.stat().st_mtime), code


def during_games(log):
    '''The log without what follows each game's report up to the next selections. A game
    over runs on through GAME! whatever the opponent sends: Dolphin still logs "Halting"
    each frame, its frame count rising, and "packet too small", and neither is a wait.'''
    kept, over = [], False
    for line in log.splitlines():
        if 'GameEndMethod' in line:
            over = True
        elif 'Setting match selections' in line:
            over = False
        if not over:
            kept.append(line)
    return '\n'.join(kept)


def felt(run_dir):
    '''The PC player's side of a run, as numbers.'''
    log = during_games((run_dir / 'dolphin-online.log').read_text(encoding='utf-8', errors='replace'))
    halts = [int(x) for x in re.findall(r'Halting for one frame due to rollback limit \(frame: (-?\d+) \|', log)]
    sync = [(int(f), int(n)) for f, n in re.findall(r'Halting on frame (\d+) due to time sync\. Offset: -?\d+ us\. Frames: (\d+)', log)]
    advances = [(int(f), int(n)) for f, n in re.findall(r'Advancing on frame (\d+) due to time sync\. Offset: -?\d+ us\. Frames: (\d+)', log)]
    speeds = [float(s) for s in re.findall(r'New speed: ([\d.]+)%', log)]
    rb = slp_rollbacks.total(sorted((run_dir / 'dolphin-replays').glob('*.slp')))
    report = (run_dir / 'report.md').read_text(encoding='utf-8', errors='replace')
    verdict = re.search(r'(\d+) games compared: (\d+) match, (\d+) desync', report)
    late = re.search(r"left later than the PC player's by (-?[\d.]+) ms", report)
    games = max(rb['games'], 1)
    return {
        'games': rb['games'], 'match': int(verdict.group(2)) if verdict else 0,
        'desync': int(verdict.group(3)) if verdict else 0,
        'rb_min': rb['rollbacks'] * 3600 / max(rb['frames'], 1),
        'depth': rb['resimulated'] / max(rb['rollbacks'], 1), 'deepest': max((i + 1 for i, c in enumerate(rb['depth']) if c), default=0),
        'halts_play': sum(1 for x in halts if x > UNFREEZE_FRAME) / games,
        'start_frames': (sum(1 for x in halts if x <= UNFREEZE_FRAME) + sum(n for f, n in sync if f <= UNFREEZE_FRAME)) / games,
        'sync_play': sum(n for f, n in sync if f > UNFREEZE_FRAME) / games,
        'advance_play': sum(n for f, n in advances if f > UNFREEZE_FRAME) / games,
        'too_small': log.count('packet too small'),
        'speed': f'{min(speeds):.1f}-{max(speeds):.1f}%' if speeds else '-',
        'late': float(late.group(1)) if late else None,
    }


def row(net, who, f, code):
    late = f'{f["late"]:.1f} ms' if f['late'] is not None else '-'
    crash = '' if code == 0 else ' **FAILED**'
    return (f'| {net} | {who} | {f["match"]}/{f["games"]}{crash} | {f["rb_min"]:.0f} | {f["depth"]:.2f} (deepest {f["deepest"]}) | '
            f'{f["halts_play"]:.1f} | {f["sync_play"]:.1f} | {f["advance_play"]:.1f} | {f["start_frames"]:.0f} | {f["too_small"]} | {f["speed"]} | {late} |')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--wii-ip', required=True)
    ap.add_argument('--rounds', type=int, default=4)
    ap.add_argument('--nets', nargs='+', default=['15,3', '30,5', '45,5', '60,8,1'],
                    help='one-way delay, jitter (ms), loss (%%), both ways')
    ap.add_argument('--pc-nets', nargs='*', default=[], help='the same, against a PC only')
    ap.add_argument('--no-mirror', action='store_true',
                    help="the Wii sends no replay stream, as a player's does (its replays then are not compared)")
    ap.add_argument('--seed', type=int, default=0, help='random inputs: another seed, other games')
    args = ap.parse_args()
    out = ROOT / 'build/sweep' / f'ab-{time.strftime("%Y%m%d-%H%M%S")}.md'
    lines = ['# The PC player, against a PC and against the Wii', '',
             f'{args.rounds} games a run. Connection: one-way delay, jitter (ms), loss (%), both ways.', '',
             '| Connection | Opponent | Replays match | Rollbacks a minute | Frames a rollback | Halts a game, in play | '
             'Time sync frames a game, in play | Time sync advances a game, in play | Frames waited while frozen | Too-small packets | Dolphin speed | Opponent\'s input later by |',
             '|---|---|---|---|---|---|---|---|---|---|---|---|']
    seed = ['--seed', str(args.seed)]
    pc, wii = ('PC', ['--baseline', *seed]), ('Wii', ['--rollback', '0', *seed, *(['--no-mirror'] if args.no_mirror else [])])
    for net, who, extra in [(n, *o) for n in args.nets for o in (pc, wii)] + [(n, *pc) for n in args.pc_nets]:
        run_dir, code = sweep(args.wii_ip, args.rounds, net, extra)
        lines.append(row(net, who, felt(run_dir), code))
        print(lines[-1], f'({run_dir.name})', flush=True)
        out.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print(out)


if __name__ == '__main__':
    main()
