"""One line a run, for comparing runs side by side: the Wii player's lag and screen, what the engine did,
and what the PC player saw.

    python tools/latency/compare.py NAME=RUN_DIR [NAME=RUN_DIR ...]

lag: the median from the controller's reading to the retrace, and how many presses took the fewest refreshes;
screen: frames shown twice and jumps in play; read: how early the pad is read; skips/ended: direct_track's
(docs/ROLLBACK.md); 3-loop, 4-loop: passes with a rollback of 2 and 3 frames; PC: how late the Wii's inputs
left, the PC player's rollbacks a minute and frames a rollback, its halts in play (the run's report.md).
"""
import re
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import analyze  # noqa: E402


def one(name, run):
    run = Path(run)
    log = (run / 'wii.log').read_text(errors='replace')
    report = (run / 'report.md').read_text(errors='replace') if (run / 'report.md').exists() else ''
    presses = [p for p in analyze.read(run) if analyze.scene_name(p['scene'], p['delay']).startswith('online game')
               and p['gap'] < 100000 and p['show'] < 500000]
    show = [p['show'] / 1000 for p in presses]
    by = {}
    for p in presses:
        by[p['refreshes']] = by.get(p['refreshes'], 0) + 1
    fast = min(by) if by else 0
    # the screen: each game's last press has its totals
    retraces = repeats = jumps = 0
    for i, p in enumerate(presses):
        if i + 1 == len(presses) or presses[i + 1]['retraces'] < p['retraces']:
            retraces += p['retraces']
            repeats += p['repeats']
            jumps += p['jumps']
    games = len(re.findall(r'game checksums', log))
    match = len(re.findall(r'game checksums: \d+ frames compared with the opponent\'s, 0 differ', log))
    earl = [int(v) for v in re.findall(r'earl (\d+) vb', log)]
    ndg = [int(v) for v in re.findall(r' ndg (\d+)', log)]
    dirs = re.findall(r' dir (\d+)/(\d+)/(\d+)', log)
    deep = re.findall(r' dir \d+/\d+/\d+/(\d+)', log)  # each game's passes too long for a frame level
    carry = re.findall(r' dir \d+/\d+/\d+/\d+/(\d+)', log)  # and the opponent's number it began with
    waits = re.findall(r'PERF rb w (\d+)/(\d+)/', log)
    p3 = [int(m) for m in re.findall(r'PERF rb p \d+/\d+/\d+ \d+/\d+/\d+ +\| (\d+)/', log)]
    p4 = [int(m) for m in re.findall(r'PERF rb p \d+/\d+/\d+ \d+/\d+/\d+ +\| \d+/\d+/\d+ (\d+)/', log)]
    late = re.search(r"left later than the PC player's by (-?[\d.]+) ms", report)
    rb = re.search(r"PC player's rollbacks: (\d+) rollbacks in (\d+) frames \((\d+) a minute\), (\d+) frames run again", report)
    halts = re.search(r'(\d+) in play\. Its time sync', report)
    gaps = len(re.findall(r'poll gap:', log))
    mins = retraces / 3596 if retraces else 0
    print(f'{name:10} {match}/{games} games | lag {statistics.median(show) if show else 0:5.1f} ms, '
          f'{by.get(fast, 0)}/{len(presses)} at {fast} refreshes | screen {mins:4.1f} min: {repeats} twice, {jumps} jumps'
          f' | read {statistics.mean(earl) / 1000 if earl else 0:.1f} ms early, {sum(ndg)} frames ended early, '
          f'skips/ended {sum(int(d[0]) for d in dirs)}/{sum(int(d[1]) for d in dirs)}, waits {sum(int(w[0]) for w in waits)}'
          f' | 3-loop {sum(p3)} 4-loop {sum(p4)}{", too long a game " + ",".join(deep) if deep else ""}{", began with " + ",".join(carry) if carry else ""}'
          f' | PC: Wii {late.group(1) if late else "?"} ms late, {rb.group(3) if rb else "?"} rb/min, '
          f'{int(rb.group(4)) / max(1, int(rb.group(1))) if rb else 0:.2f} deep, halts in play {halts.group(1) if halts else "?"}'
          + (f' | POLL GAPS {gaps}' if gaps else ''))


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for arg in sys.argv[1:]:
        name, _, run = arg.partition('=')
        one(name, run or name)
