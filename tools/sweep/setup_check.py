"""Each game of a lobby.py or run_sweep.py run: did the Wii set up the same game as the PCs?

    python tools/sweep/setup_check.py build/sweep/runs/RUN [RUN ...]

The Wii logs each game's setup (`exi: the game: ... seed S, sum X; ports: ...`, 1.8.1 and later).
A PC's replay has the game-info block and the seed in its game start event. This compares, game by
game, the seed, the characters and teams, and the sum of the setup block taken the Wii's way.
"""
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from slp_compare import read_events  # noqa: E402


def wii_games(log):
    """[(seed, sum, ports text)] per game: the first setup line of each game (it is logged again later)."""
    out, last = [], None
    for line in log.splitlines():
        m = re.search(r'exi: the game: .*seed ([0-9a-f]{8}), (?:setup )?sum ([0-9a-f]{8}); ports: (.*)', line)
        if m:
            key = (m.group(1), m.group(2))
            if key != last:
                out.append((int(m.group(1), 16), int(m.group(2), 16), m.group(3)))
            last = key
        elif 'game over:' in line:
            last = None
    return out


def replay_game(path, wii_ports=None):
    """wii_ports: the Wii's 'char/colour/team/shade' per port; its colour and shade are put into the
    replay's block before the sum (in Teams the game puts the team's colour in, so they differ)"""
    for c, p in read_events(path):
        if c == 0x36:
            gi = bytearray(p[5:5 + 0x138])
            for i, w in enumerate(wii_ports or []):
                v = [int(n) for n in w.rstrip('*').split('/') if n.isdigit()]
                if len(v) == 4:	# (a port cut off at the log's line length is left as it is)
                    gi[0x63 + i * 0x24], gi[0x67 + i * 0x24] = v[1], v[3]
            seed = struct.unpack('>I', p[0x13D:0x141])[0]
            s = 0
            for b in gi:
                s = (s * 31 + b) & 0xFFFFFFFF
            ports = []
            for i in range(4):
                q = gi[0x60 + i * 0x24:0x60 + (i + 1) * 0x24]
                ports.append('-' if q[1] == 3 else f'{q[0]}/{q[9]}')
            return seed, s, ' '.join(ports)
    return None


def check(run):
    run = Path(run)
    log = (run / 'wii.log').read_text('utf-8', 'replace') if (run / 'wii.log').exists() else ''
    wii = wii_games(log)
    dirs = sorted(run.glob('replays-p*')) or sorted(run.glob('dolphin-replays'))
    reps = sorted(dirs[0].glob('*.slp'), key=lambda p: p.stat().st_mtime) if dirs else []
    print(f'{run.name}: the Wii set up {len(wii)} games, {len(reps)} replays in {dirs[0].name if dirs else "-"}')
    bad, rows, diffs, cut = 0, [], [], []
    for k, (w, r) in enumerate(zip(wii, reps)):
        g = replay_game(r, w[2].split())
        if not g:
            print(f'  game {k + 1}: no game start in {r.name}')
            continue
        seed, s, ports = g
        wports = ' '.join('-' if p == '-' else '/'.join(p.rstrip('*').split('/')[0:3:2]) for p in w[2].split())
        if len(w[2].split()) < 4 or len(w[2].split()[-1].split('/')) < 3:	# (cut off: compare what is there)
            ports = ' '.join(ports.split()[:len(wports.split())])
            wports = ' '.join(x for x in wports.split() if x.count('/') == 1 or x == '-')
            ports = ' '.join(ports.split()[:len(wports.split())])
        flags = []
        if seed != w[0]:
            flags.append(f'SEED the Wii {w[0]:08x} the replay {seed:08x}')
        if wports != ports:
            flags.append(f'PORTS the Wii {wports} the replay {ports}')
        diffs.append((s - w[1]) & 0xFFFFFFFF)
        cut.append(any((len(x.split('/')) < 4 or x.endswith('/')) and x != '-' for x in w[2].split()) or len(w[2].split()) < 4)
        bad += bool(flags)
        rows.append((k, seed, ports, flags))
    # (the game writes the same few bytes into its copy of the block before the replay takes it: the
    # sums differ by a constant; a game whose difference is not the run's usual one was set up otherwise)
    usual = max(set(diffs), key=diffs.count) if diffs else 0
    for (k, seed, ports, flags), d in zip(rows, diffs):
        if d != usual and cut[k]:
            flags.append('setup block not checked (the log line was cut off)')
        elif d != usual:
            flags.append(f'SETUP BLOCK differs (sum difference {d:08x}, usually {usual:08x})')
            bad += 1
        print(f'  game {k + 1}: seed {seed:08x}, ports {ports}: ' + ('; '.join(flags) if flags else 'the same'))
    return bad


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    sys.exit(1 if sum(check(r) for r in sys.argv[1:]) else 0)
