"""What really reached the screen: decodes the frame marker RB_TRACE draws (rollback.c, tr_marker)
from a capture of the Wii's video output (run_sweep.py --capture), and compares it with what the
game's own record says it showed.

    python tools/sweep/decode_marker.py RUN_DIR [--still out.png]

The capture card takes one field out of every two refreshes and drops every sixth of those, so
steady play reads steps of 2 frames between pictures, and 4 where it dropped one. A frame shown
twice makes the steps odd until the jump that follows. So this is a sample of the screen, not every
refresh: a hold of one refresh is seen about half the time, longer ones nearly always. Its use is
that it can't be fooled by the game's own bookkeeping: odd steps the record has no event for are
something the record doesn't see. The marker standing still for several pictures is out of play
(a pause, the end of a game), and doesn't count.

The card's clock runs a little fast against the Wii's video: about 180 s into a capture and then
every 8001 pictures (320 s), whatever is playing, it takes the same Wii frame twice (one picture's
step of 0 between normal steps; the pictures differ by analog noise, so it's a fresh sample). Those
holds are the card's, not the screen's (found 2026-11-08: all six captures checked sit on that
lattice), and are listed apart as card slips.
"""
import argparse
import json
import re
import shutil
import subprocess
from collections import Counter
from pathlib import Path

import numpy as np

W, H = 720, 480
X0, STEP, ROWS = 86, 28.1, list(range(30, 44))	# where the card puts tr_marker's blocks
FFMPEG = shutil.which('ffmpeg') or str(next(Path.home().glob(
    'AppData/Local/Microsoft/WinGet/Packages/Gyan.FFmpeg*/ffmpeg-*/bin/ffmpeg.exe')))


def frames(path):
    p = subprocess.Popen([FFMPEG, '-v', 'quiet', '-i', str(path), '-f', 'rawvideo', '-pix_fmt', 'gray', '-'],
                         stdout=subprocess.PIPE)
    while True:
        raw = p.stdout.read(W * H)
        if len(raw) < W * H:
            return
        yield np.frombuffer(raw, np.uint8).reshape(H, W)


def read_marker(img):
    """The frame number's low 8 bits, or None where no marker is on the picture."""
    means = [img[ROWS[0]:ROWS[-1] + 1, int(X0 + i * STEP) + 6:int(X0 + (i + 1) * STEP) - 6].mean() for i in range(10)]
    if means[0] < 170 or means[1] > 90:
        return None
    if any(90 <= m <= 170 for m in means[2:]):
        return None
    gray = 0
    for m in means[2:]:
        gray = gray << 1 | int(m > 128)
    value = 0
    while gray:
        value ^= gray
        gray >>= 1
    return value


def nudge_stops(run_dir):
    """Per game, the frames where the Wii stopped ending frames a line early. The card's sampling
    changes phase about 10-13 frames later, which reads as a pair of odd steps with no repeat in
    the record, and doesn't happen in a run without nudges. Listed apart, not counted as stutter;
    what a real display does there is a hand check."""
    log = run_dir / 'wii.log'
    if not log.exists():
        log = run_dir / 'watch.log'
    text = subprocess.run(['python', str(Path(__file__).parents[1] / 'rollback/trace_view.py'), str(log), '--events'],
                          capture_output=True, text=True, encoding='utf-8', errors='replace').stdout
    return [[int(m) for m in re.findall(r'(?m)^\s+(\d+)\s+[\d.]+s frames stop ending a line early', g)]
            for g in re.split(r'(?m)^(?==== game)', text)[1:]]


def record_events(run_dir):
    """The game's own record: per game, the frames it says it showed twice, and where play ended."""
    log = run_dir / 'wii.log'
    if not log.exists():
        log = run_dir / 'watch.log'
    text = subprocess.run(['python', str(Path(__file__).parents[1] / 'rollback/trace_view.py'), str(log), '--events'],
                          capture_output=True, text=True, encoding='utf-8', errors='replace').stdout
    games = []
    for g in re.split(r'(?m)^(?==== game)', text)[1:]:
        games.append([int(m) for m in re.findall(r'frame (\d+) shown again', g)])
    return games


CARD_SLIP_PERIOD = 8001	# pictures between the card's own repeated samples
CARD_SLIP_FIRST = range(4300, 4900)	# where the first one landed in every capture checked


def card_slips(holds):
    """Of the one-picture holds (global picture numbers), those that are the card's: a whole number
    of periods (within 3 pictures) from another hold, or where the first slip lands."""
    def on(a, b):
        k = round((b - a) / CARD_SLIP_PERIOD)
        return k and abs(b - a - k * CARD_SLIP_PERIOD) <= 3
    lattice = {h for h in holds if any(on(h, o) for o in holds if o != h)}
    if not lattice:
        lattice = {h for h in holds if h % CARD_SLIP_PERIOD in CARD_SLIP_FIRST}
    if lattice:	# the lattice's other places
        base = min(lattice)
        lattice |= {h for h in holds if abs((h - base) - round((h - base) / CARD_SLIP_PERIOD) * CARD_SLIP_PERIOD) <= 3}
    return lattice


def summarize(run_dir, still=None):
    run_dir = Path(run_dir)
    pictures = unread = 0
    runs, cur, last = [], [], None
    gidx, gcur = [], []	# each marker's picture number
    for img in frames(run_dir / 'capture.mkv'):
        pictures += 1
        if still and pictures == 3000:
            from PIL import Image
            Image.fromarray(img).save(still)
        v = read_marker(img)
        if v is None:
            unread += 1
            if len(cur) > 100:
                runs.append(cur)
                gidx.append(gcur)
            cur, gcur, last = [], [], None
            continue
        cur.append(v)
        gcur.append(pictures - 1)
    if len(cur) > 100:
        runs.append(cur)
        gidx.append(gcur)
    holds = []
    for r, gi in zip(runs, gidx):
        s = [(r[i] - r[i - 1]) & 255 for i in range(1, len(r))]
        holds += [gi[i + 1] for i in range(2, len(s) - 2)
                  if s[i] == 0 and all(x in (2, 4) for x in s[i - 2:i] + s[i + 1:i + 3])]
    slips = card_slips(holds)
    steps = Counter()
    odd = []
    in_play = 0
    near_nudge = card = 0
    stops = nudge_stops(run_dir)
    for g, r in enumerate(runs):
        s = [min((r[i] - r[i - 1]) & 255, (r[i - 1] - r[i]) & 255) for i in range(1, len(r))]
        at = r[0]
        for i, step in enumerate(s):
            at += step
            if at < 84:	# as the scorecard: from when the players can move
                continue
            # within 8 pictures of the marker standing still: out of play
            near = s[max(i - 8, 0):i + 9]
            if i < 8 or sum(1 for x in near if x == 0) >= 3:
                continue
            steps[step] += 1
            in_play += 1
            if step not in (2, 4):
                if step == 0 and gidx[g][i + 1] in slips:
                    card += 1
                elif any(0 <= at - f <= 25 for f in (stops[g] if g < len(stops) else [])):
                    near_nudge += 1
                else:
                    odd.append((g, i, r[i + 1], step))
    out = {'pictures': pictures, 'unread': unread, 'stretches': len(runs), 'steps_in_play': dict(sorted(steps.items())),
           'minutes_in_play': round(in_play / 25 / 60, 2), 'off_rhythm_steps': len(odd), 'after_a_nudge': near_nudge,
           'card_slips': card}
    events = record_events(run_dir)
    shown = [f & 255 for g in events for f in g]
    unmatched = [o for o in odd if not any(((o[2] - f + 6) & 255) <= 12 for f in shown)]
    out['repeats_in_record'] = sum(len(g) for g in events)
    out['off_rhythm_not_in_record'] = len(unmatched)
    out['first_not_in_record'] = unmatched[:12]
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('run_dir')
    ap.add_argument('--still', help='save a captured picture here, for looking at')
    args = ap.parse_args()
    print(json.dumps(summarize(args.run_dir, args.still)))


if __name__ == '__main__':
    main()
