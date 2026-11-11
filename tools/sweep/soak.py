"""Games through a list of connections, again and again, until something fails
or the time is up: what a rollback build must survive before anyone plays on it.

    python tools/sweep/soak.py --wii-ip WII_IP --nets 45,5 60,8,1 --rounds 8 --hours 6 [--keep-going]

Prints a line a run and appends it to build/sweep/soak-<stamp>.md. Stops at the first
run that fails, unless --keep-going.
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


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--wii-ip', required=True)
    ap.add_argument('--nets', nargs='+', default=['45,5', '60,8,1'])
    ap.add_argument('--rounds', type=int, default=8)
    ap.add_argument('--hours', type=float, default=6)
    ap.add_argument('--keep-going', action='store_true')
    ap.add_argument('--kernel', action='append', default=[])
    args = ap.parse_args()
    out = ROOT / 'build/sweep' / f'soak-{time.strftime("%Y%m%d-%H%M%S")}.md'
    end, games, failures, late, i = time.time() + args.hours * 3600, 0, 0, 0, 0
    while time.time() < end:
        net = args.nets[i % len(args.nets)]
        i += 1
        before = set(RUNS.iterdir())
        cmd = [sys.executable, HERE / 'run_sweep.py', '--wii-ip', args.wii_ip, '--rollback', '0',
               '--net', net, '--rounds', str(args.rounds),
               # other inputs and other matchups each run: more of the game gets played
               '--seed', str(i), '--shift', str(1 + i * 7 % 25)]
        for kv in args.kernel:
            cmd += ['--kernel', kv]
        res = subprocess.run(cmd, capture_output=True, text=True)
        new = set(RUNS.iterdir()) - before
        run = max(new, key=lambda p: p.stat().st_mtime) if new else None
        code = res.returncode
        if code and (run is None or not (run / 'wii.log').exists()):
            # a build failing, say: not a failure of the Wii's
            sys.exit('the sweep did not start:\n' + res.stdout[-1500:] + res.stderr[-1500:])
        report = (run / 'report.md').read_text(encoding='utf-8', errors='replace') if (run / 'report.md').exists() else ''
        verdict = report.splitlines()[2] if len(report.splitlines()) > 2 else '(no report)'
        failure = (run / 'failure.txt').read_text(encoding='utf-8', errors='replace').strip() if (run / 'failure.txt').exists() else ''
        # (run_sweep goes on after a desync: here it must stop)
        m = re.search(r'(\d+) games compared: (\d+) match, (\d+) desync', report)
        if not code and (not m or int(m[3]) or int(m[1]) != args.rounds):
            code = 1
            failure = 'games that do not match: ' + (m[0] if m else '(no comparison)')
        late += sum(int(x) for x in re.findall(r' sv=(\d+)', (run / 'wii.log').read_text(encoding='utf-8', errors='replace')))
        games += args.rounds if code == 0 else 0
        failures += code != 0
        line = f'{time.strftime("%H:%M")} {net} {run.name}: {"FAILED " + failure[:150] if code else "ok"} | {verdict[:90]} | total {games} clean games, {failures} failures, {late} savestates saved late'
        print(line, flush=True)
        with out.open('a', encoding='utf-8') as f:
            f.write(line + '\n')
        if code and not args.keep_going:
            sys.exit(1)


if __name__ == '__main__':
    main()
