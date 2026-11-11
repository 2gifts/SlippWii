"""A run's games one a line: which stretches of each were level and which a refresh behind (by the refreshes
each press took, tools/latency_probe), and the screen's repeats and jumps in it.

    python tools/latency/phases.py RUN_DIR

"4rf f129-667 (17)": 17 presses in a row, at the game's frames 129 to 667, took 4 refreshes from the
controller's reading to the picture. With 2 delay frames 4 is level and 5 a refresh behind; with 1, 3 and 4.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import analyze  # noqa: E402


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    presses = [p for p in analyze.read(sys.argv[1]) if analyze.scene_name(p['scene'], p['delay']).startswith('online game')]
    games, cur = [], []
    for p in presses:
        if cur and p['logic_r1'] < cur[-1]['logic_r1']:
            games.append(cur)
            cur = []
        cur.append(p)
    if cur:
        games.append(cur)
    for i, g in enumerate(games):
        runs = []
        for p in g:
            r = p['refreshes']
            if runs and runs[-1][0] == r:
                runs[-1][2] = p['logic_r1']
                runs[-1][3] += 1
            else:
                runs.append([r, p['logic_r1'], p['logic_r1'], 1])
        print(f'game {i + 1:2}: ' + '  '.join(f'{r}rf f{a}-{b} ({n})' for r, a, b, n in runs)
              + f"   shown twice {g[-1]['repeats']}, jumps {g[-1]['jumps']}")


if __name__ == '__main__':
    main()
