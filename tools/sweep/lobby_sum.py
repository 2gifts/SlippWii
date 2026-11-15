"""One line for a lobby.py run directory: what the PCs got from the first account, and how the Wii did."""
import re
import sys
from pathlib import Path


def cells(report):
    rows = {}
    for line in report.splitlines():
        m = re.match(r'\| (Dolphin \d) \| (.*) \|$', line)
        if m and ',' in line:
            rows[m.group(1)] = [c.strip() for c in m.group(2).split('|')]
    return rows


def main(run):
    run = Path(run)
    report = (run / 'report.md').read_text(encoding='utf-8', errors='replace')
    head = report.splitlines()[0]
    wii = 'the Wii' in head
    net = re.search(r'\(net ([^)]*)\)', report)
    games = re.search(r'(\d+) games; (\d+) replays compared.*?: (\d+) match', report)
    failed = re.search(r'\*\*FAILED: (.*?)\*\*', report)
    rows = cells(report)
    first, others, bad, n = [], [], 0, 0
    for name, c in rows.items():
        for j, cell in enumerate(c):
            m = re.match(r'([\d.]+), (\d+) of (\d+)', cell)
            if not m:
                continue
            if j == 0:
                first.append(float(m.group(1)))
                bad += int(m.group(2))
                n += int(m.group(3))
            elif name != 'Dolphin 1':
                others.append(float(m.group(1)))
    out = [run.name, 'Wii' if wii else 'PCs', f'net {net.group(1) if net else "-"}',
           f'{games.group(1)} games, {games.group(3)}/{games.group(2)} replays match' if games else 'no games']
    if first:
        out.append('depth against the first account %.2f (unhealthy %d of %d), among the other PCs %.2f'
                   % (sum(first) / len(first), bad, n, sum(others) / max(len(others), 1)))
    relay = (run / 'relay.log').read_text(errors='replace') if (run / 'relay.log').exists() else ''
    m = re.search(r"relay: in all, p1: its inputs later than each one's own, ms: (.*?); its acks after a pad from each, ms: (.*)", relay)
    if m:
        out.append(f'p1 later: {m.group(1)}; acks: {m.group(2)}')
    if wii and (run / 'watch.log').exists():
        log = (run / 'watch.log').read_text(encoding='utf-8', errors='replace')
        waited = re.findall(r'game over: (\d+) frames in \d+ ms \((\d+) at.*?waited (\d+) frames', log)
        delays = re.findall(r"game quality \(Slippi's rule\): \d+ intervals, delay (\d+), debt at most (\d+)", log)
        fine = re.findall(r'so far \d+ predicted, (\d+) fine', log)
        differ = re.findall(r'game checksums: (\d+) frames compared with the opponent\'s, (\d+) differ', log)
        out.append('Wii: waited ' + '/'.join(w[2] for w in waited) + ' frames, delay ' + '/'.join(d[0] for d in delays)
                   + ', debt ' + '/'.join(d[1] for d in delays) + f', level-keeping advances {fine[-1] if fine else "?"} in all'
                   + ', checksums differ ' + '/'.join(d[1] for d in differ)
                   + f', slow disc commands {len(re.findall("di: a command took", log))}')
    if failed:
        out.append('FAILED: ' + failed.group(1)[:90])
    print(' | '.join(out))


if __name__ == '__main__':
    for r in sys.argv[1:]:
        try:
            main(r)
        except Exception as e:  # a run that left no report
            print(r, 'no summary:', e)
