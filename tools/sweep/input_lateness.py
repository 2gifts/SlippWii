"""How late the opponent's inputs are, as the PC player has them: a run's relay-passes.txt.

    python tools/sweep/input_lateness.py RUN_DIR [RUN_DIR...]

The relay (net_relay.py --passes) notes every pad datagram: "S A t frame" when the PC player's own
(A) reaches it, "S B ..." the opponent's, "D A ..." when one is handed to A. A player sends its
pad once for each frame it runs, so for each frame this gives two times:

  sent later      how much later the opponent sent its input for a frame than the PC player sent
                  its own (the two games' frames against each other, with the opponent's time to
                  get its input out): about 0 between two Dolphins, 1 to 3 ms with a Wii
  in hand later   how long after sending its own input for a frame the PC player has the
                  opponent's for it: the connection's delay on top

and from the second, in whole frames, the rollback depth Slippi's match-quality rule counts
(match_quality.py) at 2 delay frames: frames late, less 2. Depth moves in steps: a connection that
sits a millisecond or two under a whole frame shows a few milliseconds more as most of a frame
(docs/RANKED_READINESS.md, "Measured"). The tail is the opponent falling behind: a Wii at a high
ping, whose rollbacks take longer than a frame.
"""
import bisect
import collections
import math
import statistics
import sys
from pathlib import Path

FRAME_MS = 16.683


def games(path):
    """A dict a game: {'SA': {frame: time}, 'SB': ..., 'DA': ...} (a frame's first datagram)."""
    out, current, newest = [], None, None
    for line in Path(path).read_text().splitlines():
        p = line.split()
        if len(p) != 4:
            continue
        key, t, frame = p[0] + p[1], float(p[2]), int(p[3])
        if key == 'SA':
            if newest is None or frame < newest - 600:   # the frames start again: a new game
                current = collections.defaultdict(dict)
                out.append(current)
                newest = frame
            newest = max(newest, frame)
        if current is not None:
            current[key].setdefault(frame, t)
    return out


def in_hand(handed, frame):
    """When the pad of `frame` was first in A's hands: the first datagram handed over whose newest
    frame is that one or a later one (a lost datagram's frames come with the next)."""
    frames, times = handed
    i = bisect.bisect_left(frames, frame)
    return times[i] if i < len(frames) else None


def spread(values):
    v = sorted(values)

    def at(q):
        return v[min(len(v) - 1, int(q * len(v)))]
    return (f'mean {statistics.mean(v):.2f}, median {statistics.median(v):.2f}, 10% under {at(.1):.1f}, '
            f'10% over {at(.9):.1f}, 1% over {at(.99):.1f}, most {v[-1]:.1f} ms')


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for arg in sys.argv[1:]:
        path = Path(arg) / 'relay-passes.txt' if Path(arg).is_dir() else Path(arg)
        sent, total = [], []
        for g in games(path):
            frames = sorted(g['DA'])
            earliest, t = {}, float('inf')
            for f in reversed(frames):      # (jitter keeps the order, but be sure)
                t = min(t, g['DA'][f])
                earliest[f] = t
            handed = (frames, [earliest[f] for f in frames])
            for f in sorted(set(g['SA']) & set(g['SB'])):
                if f <= 200:
                    continue                # (the countdown, and the time sync finding its level)
                sent.append((g['SB'][f] - g['SA'][f]) * 1000)
                t = in_hand(handed, f)
                if t is not None:
                    total.append((t - g['SA'][f]) * 1000)
        if not total:
            print(f'{path}: no frames (a run with --net has relay-passes.txt)')
            continue
        late = collections.Counter(math.ceil(x / FRAME_MS) for x in total)
        print(f'{path.parent.name}: {len(total)} frames')
        print('  sent later:', spread(sent))
        print('  in hand later:', spread(total))
        print('  whole frames late: ' + ', '.join(f'{k}: {100 * late[k] / len(total):.0f}%' for k in sorted(late))
              + f'; depth at 2 delay frames {statistics.mean(max(0, math.ceil(x / FRAME_MS) - 2) for x in total):.2f}')


if __name__ == '__main__':
    main()
