"""Slippi's match quality rule, scored for the players of a sweep's games.

Slippi Dolphin's development source (2026-11) ends a Ranked match when a debt reaches 30.
Every 150 frames from frame 120 the worst of three signals moves it: the 150 frames' real
time (10%, 50%, 75% over), how far the game ran ahead of the opponent's newest input on
average (2, 3, 4.75 frames) and its stalls at the rollback limit; mild +4, bad +8, severe
+15, a stall event +9, shorter stalls +3, a healthy interval -1 (the rule in full:
Nintendont/kernel/slippi_online/so_mq.h, which is the Wii's own count of it).

Each player judges the game by what it sees itself, so a game has two scores. This reads
them from a run with --net (the relay's record of every pad datagram, relay-passes.txt):

  the PC player    its frames and the opponent's newest input at each, by the relay's
                   clock; its stalls from Slippi Dolphin's own log
  the opponent     a Wii: what its kernel counted itself (the "quality" lines of its
                   log). A PC (--baseline): the same as the PC player, from the relay

The relay is on the PC player's machine, so its clock is that player's to well under a
millisecond. It is not a Wii's: a Wii has an input about 2 ms after the relay hands it
over and its pad reaches the relay about 2 ms after its frame, so by the relay a Wii's
depth reads about 0.4 frames shallower than the Wii found it (run 20261113-153954: 1.06
against 1.44). The Wii's own count is the one to go by; --relay-opponent prints the other.

    python tools/sweep/match_quality.py RUN_DIR [RUN_DIR...]
    python tools/sweep/match_quality.py --selftest     # this port against the kernel's C

A PC against PC run (--baseline) is the yardstick: what the PC player's own Dolphin would
have said of the same games through the same connection against another PC.
"""
import argparse
import bisect
import collections
import random
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]

ROLLBACK_MAX_FRAMES = 7
FRAME_TIME_US = 16683
WARMUP_FRAMES = 120
INTERVAL_FRAMES = 150
LATE_GAME_FRAME = 3600
LATE_GAME_MODIFIER = 1.15
TERMINATE_DEBT = 30
MILD_DEBT, BAD_DEBT, SEVERE_DEBT, HEALTHY_DEBT = 4, 8, 15, -1
OVERRUN_TIERS = (0.1, 0.5, 0.75)
DEPTH_TIERS = (2.0, 3.0, 4.75)
MIN_JUDGED_DELAY = 2
STALL_MERGE_GAP_US = 500_000
MIN_STALL_PASSES = 5
STALL_DEBT, SHORT_STALL_DEBT = 9, 3


def tier_debt(value, tiers, modifier):
    mild, bad, severe = (t * modifier for t in tiers)
    if value >= severe:
        return SEVERE_DEBT
    if value >= bad:
        return BAD_DEBT
    if value >= mild:
        return MILD_DEBT
    return HEALTHY_DEBT


class Tracker:
    """One player's score of one game: the rule as Slippi's source has it, in doubles."""

    def __init__(self, delay):
        self.delay = delay
        self.debt = self.max_debt = 0
        self.verdict_frame = None
        self.interval_start_us = None
        self.last_interval_frame = 0
        self.depth_sum = self.depth_samples = 0
        self.last_stall_us = None
        self.current_stall_passes = 0
        self.interval_stall_events = self.interval_stalled_passes = 0
        self.stall_events = 0
        self.intervals = []  # each: a dict (frame, change, time, depth, stalls, ratio, avg_depth, ...)

    def on_frame(self, frame, time_us, is_stall, latest_remote_frame):
        """One pass. Returns the interval's dict when this pass ended one."""
        shortfall = max(MIN_JUDGED_DELAY - self.delay, 0)
        self.depth_sum += min(max(frame - latest_remote_frame - shortfall, 0), ROLLBACK_MAX_FRAMES + 1)
        self.depth_samples += 1
        if is_stall:
            if frame <= WARMUP_FRAMES:
                return None
            self.interval_stalled_passes += 1
            if self.last_stall_us is None or max(time_us - self.last_stall_us, 0) > STALL_MERGE_GAP_US:
                self.current_stall_passes = 0
            self.last_stall_us = time_us
            self.current_stall_passes += 1
            if self.current_stall_passes == MIN_STALL_PASSES:
                self.interval_stall_events += 1
                self.stall_events += 1
            return None
        if frame < WARMUP_FRAMES or (frame - WARMUP_FRAMES) % INTERVAL_FRAMES or frame == self.last_interval_frame:
            return None
        self.last_interval_frame = frame
        depth_sum, samples = self.depth_sum, self.depth_samples
        events, stalled = self.interval_stall_events, self.interval_stalled_passes
        self.depth_sum = self.depth_samples = 0
        self.interval_stall_events = self.interval_stalled_passes = 0
        start, self.interval_start_us = self.interval_start_us, time_us
        if start is None:
            return None  # the first boundary only starts the clock
        ratio = max(time_us - start, 0) / (FRAME_TIME_US * INTERVAL_FRAMES)
        avg_depth = depth_sum / max(samples, 1)
        modifier = LATE_GAME_MODIFIER if frame > LATE_GAME_FRAME else 1.0
        speed = tier_debt(ratio - 1.0, OVERRUN_TIERS, modifier)
        depth = tier_debt(avg_depth, DEPTH_TIERS, modifier)
        stall = STALL_DEBT if events else SHORT_STALL_DEBT if stalled else HEALTHY_DEBT
        change = max(speed, depth, stall)
        self.debt = max(self.debt + change, 0)
        self.max_debt = max(self.max_debt, self.debt)
        if self.debt >= TERMINATE_DEBT and self.verdict_frame is None:
            self.verdict_frame = frame
        out = {'frame': frame, 'change': change, 'time': speed, 'depth': depth, 'stalls': stall, 'ratio': ratio,
               'avg_depth': avg_depth, 'events': events, 'stalled': stalled, 'debt': self.debt,
               'depth_sum': depth_sum, 'samples': samples}
        self.intervals.append(out)
        return out

    def summary(self):
        n = len(self.intervals)
        return {
            'intervals': n, 'max_debt': self.max_debt, 'debt': self.debt, 'verdict_frame': self.verdict_frame,
            'unhealthy': sum(1 for i in self.intervals if i['change'] > 0),
            'by_time': sum(1 for i in self.intervals if i['time'] > 0),
            'by_depth': sum(1 for i in self.intervals if i['depth'] > 0),
            'by_stalls': sum(1 for i in self.intervals if i['stalls'] > 0),
            'stall_events': self.stall_events,
            'depth_sum': sum(i['depth_sum'] for i in self.intervals),
            'samples': sum(i['samples'] for i in self.intervals),
            'debt_added': sum(i['change'] for i in self.intervals if i['change'] > 0),
            'changes': [i['change'] for i in self.intervals],
        }


# ---- a run's record ----------------------------------------------------------------------------

def read_passes(path):
    """relay-passes.txt -> {'A': {'sent': [(t_us, frame)], 'got': [(t_us, frame)]}, 'B': ...}"""
    sides = {s: {'sent': [], 'got': []} for s in 'AB'}
    for line in path.read_text().splitlines():
        p = line.split()
        if len(p) != 4 or p[1] not in sides:
            continue
        sides[p[1]]['sent' if p[0] == 'S' else 'got'].append((int(float(p[2]) * 1e6), int(p[3])))
    return sides


def split_games(events, drop=60):
    """A side's pad frames restart with each game: [(t, frame)] -> a list a game."""
    games, last = [], None
    for t, f in events:
        if last is None or f < last - drop:
            games.append([])
            last = f
        games[-1].append((t, f))
        last = max(last, f)
    return games


def dolphin_stalls(log_path):
    """Slippi Dolphin's own halts at the rollback limit, a list a game: [(frame, latest, log_ms)].
    A game is from its selections to its report: what follows (GAME! runs on, Dolphin logs
    "Halting" each frame, the frame rising) is not a wait."""
    games, over = [], True
    stamp = re.compile(r'^(\d+):(\d+):(\d+) ')
    halt = re.compile(r'Halting for one frame due to rollback limit \(frame: (-?\d+) \| latest: (-?\d+) \|')
    for line in log_path.read_text(encoding='utf-8', errors='replace').splitlines():
        if 'Setting match selections' in line:
            games.append([])
            over = False
        elif 'GameEndMethod' in line:
            over = True
        m = halt.search(line)
        if m and not over and games:
            s = stamp.match(line)
            ms = (int(s.group(1)) * 60 + int(s.group(2))) * 1000 + int(s.group(3)) if s else 0
            games[-1].append((int(m.group(1)), int(m.group(2)), ms))
    return games


def score_side(sent_game, got, delay, log_stalls=None):
    """One player's score of one game. sent_game: its own pad datagrams [(t, newest frame)];
    got: every pad datagram handed to it [(t, newest frame)], all games (times sorted);
    log_stalls: its halts by its own log [(frame, latest, ms)], or None: then a pad sent
    again (no new frame) with the opponent over the limit behind is a halted pass."""
    tracker = Tracker(delay)
    got_t = [t for t, _ in got]
    # the opponent's newest frame handed over by a time, within the opponent's current game
    newest, top = [], None
    for _, f in got:
        top = f if top is None or f < top - 60 else max(top, f)
        newest.append(top)

    def latest_at(t):
        i = bisect.bisect_right(got_t, t) - 1
        return newest[i] if i >= 0 else 0

    stalls_by_frame = collections.defaultdict(list)
    for frame, latest, ms in log_stalls or []:
        stalls_by_frame[frame].append((latest, ms))
    last_frame, last_t, finalized = 0, None, 0
    for t, f in sent_game:
        frame = f - delay
        if frame <= last_frame:
            # no new frame: a skip (the time sync's, or a halt). By the relay alone, a halt
            # when the opponent is past the limit behind the frame it could not run
            if log_stalls is None and last_frame + 1 - latest_at(t) > ROLLBACK_MAX_FRAMES:
                tracker.on_frame(last_frame + 1, t, True, latest_at(t))
            continue
        for skipped in range(last_frame + 1, frame + 1):
            # (a pad datagram lost before the relay, or two frames in one: each frame is a pass)
            when = t if last_t is None else last_t + (t - last_t) * (skipped - last_frame) // (frame - last_frame)
            halts = stalls_by_frame.get(skipped, ())
            for k, (latest, ms) in enumerate(halts):
                # its log's halts on this frame, at the log's own spacing before the pass that ran it
                tracker.on_frame(skipped, when - (halts[-1][1] - ms) * 1000 - FRAME_TIME_US, True, latest)
            # Dolphin's latest_frame is the newest of the 7 inputs it would hand the game from the
            # finalized frame on (GetSlippiRemotePad): after a burst, not all of it at once
            head = latest_at(when)
            tracker.on_frame(skipped, when, False, min(head, finalized + ROLLBACK_MAX_FRAMES - 1))
            finalized = min(head, skipped)
        last_frame, last_t = frame, t
    return tracker


def infer_delay(games, default=2):
    """A game's first pad datagram carries frames up to 1 + delay."""
    firsts = [g[0][1] - 1 for g in games if g and 1 <= g[0][1] - 1 <= 9]
    return collections.Counter(firsts).most_common(1)[0][0] if firsts else default


def wii_log_scores(log_path):
    """What the Wii's kernel counted itself: a dict a game, from its "game quality" lines."""
    out = []
    a = re.compile(r"game quality \(Slippi's rule\): (\d+) intervals, delay (\d+), debt at most (-?\d+) of \d+, (-?\d+) at the end, "
                   r'depth (\d+)\.(\d+); Ranked: (ended|not ended) \(frame (-?\d+)\)')
    b = re.compile(r'game quality, unhealthy intervals: time (\d+)/(\d+)/(\d+), depth (\d+)/(\d+)/(\d+) \(mild/bad/severe\), '
                   r'stalls (\d+) short, (\d+) with an event \((\d+) events\)')
    c = re.compile(r'quality: frame (\d+): debt \+(\d+) to (\d+) ')
    noted = {}
    for line in log_path.read_text(encoding='utf-8', errors='replace').splitlines():
        m = c.search(line)
        if m:
            noted[int(m.group(1))] = int(m.group(2))
            continue
        m = a.search(line)
        if m:
            n = int(m.group(1))
            # every interval's change: the ones it logged (a game's first 10 unhealthy ones), -1 for the rest
            changes = [noted.get(WARMUP_FRAMES + INTERVAL_FRAMES * (k + 1), HEALTHY_DEBT) for k in range(n)]
            out.append({'intervals': n, 'delay': int(m.group(2)), 'max_debt': int(m.group(3)),
                        'debt': int(m.group(4)), 'avg_depth': int(m.group(5)) + int(m.group(6)) / 100,
                        'verdict_frame': int(m.group(8)) if m.group(7) == 'ended' else None, 'changes': changes,
                        'debt_added': sum(x for x in changes if x > 0)})
            noted = {}
            continue
        m = b.search(line)
        if m and out:
            v = [int(x) for x in m.groups()]
            out[-1].update(by_time=sum(v[0:3]), by_depth=sum(v[3:6]), by_stalls=v[6] + v[7], stall_events=v[8])
            # (an interval can be unhealthy by more than one signal: by its changes when they are all known)
            known = sum(1 for x in out[-1]['changes'] if x > 0)
            out[-1]['unhealthy'] = max(known, v[0] + v[1] + v[2], v[3] + v[4] + v[5], v[6] + v[7])
    return out


MATCH_INTERVALS = 72  # 3 minutes: a Ranked game's usual length, where a sweep's games are 40 s


def as_matches(games, n=MATCH_INTERVALS):
    """The games' intervals end to end, cut into matches of 3 minutes, each from a debt of 0:
    (matches the rule would have ended, matches). A sweep's games are too short for a slow
    climb to reach the limit; the same connection held for a real game's length may."""
    changes = [x for g in games for x in g.get('changes', [])]
    ended = matches = 0
    for i in range(0, len(changes) - n + 1, n):
        debt, hit = 0, False
        for x in changes[i:i + n]:
            debt = max(debt + x, 0)
            hit = hit or debt >= TERMINATE_DEBT
        matches += 1
        ended += hit
    return ended, matches


def score_run(run_dir):
    """{'pc': [summary a game], 'opponent': [...], 'wii_log': [...] or None, 'delays': (pc, opponent)}"""
    run_dir = Path(run_dir)
    passes = run_dir / 'relay-passes.txt'
    if not passes.exists():
        return None
    sides = read_passes(passes)
    games = {s: split_games(sides[s]['sent']) for s in 'AB'}
    delays = {s: infer_delay(games[s]) for s in 'AB'}
    log = run_dir / 'dolphin-online.log'
    halts = dolphin_stalls(log) if log.exists() else []
    out = {'pc': [], 'opponent': [], 'delays': (delays['A'], delays['B'])}
    # (a game with too few frames is the results screen's polls or a search's leftovers)
    played = {s: [g for g in games[s] if g[-1][1] - g[0][1] > 300] for s in 'AB'}
    stalls_ok = len(halts) == len(played['A'])
    out['stalls_from'] = "Dolphin's log" if stalls_ok else 'the relay (the log has %d games, the relay %d)' % (len(halts), len(played['A']))
    for i, g in enumerate(played['A']):
        out['pc'].append(score_side(g, sides['A']['got'], delays['A'], halts[i] if stalls_ok else None).summary())
    for g in played['B']:
        out['opponent'].append(score_side(g, sides['B']['got'], delays['B']).summary())
    wii = run_dir / 'wii.log'
    out['wii_log'] = wii_log_scores(wii) if wii.exists() else None
    return out


def total(games):
    """Sums a list of game summaries: what a table row shows."""
    n = len(games)
    intervals = sum(g['intervals'] for g in games)
    samples = sum(g.get('samples', 0) for g in games)
    return {
        'games': n, 'intervals': intervals,
        'minutes': intervals * INTERVAL_FRAMES / 3600,
        'unhealthy': sum(g.get('unhealthy', 0) for g in games),
        'by_time': sum(g.get('by_time', 0) for g in games), 'by_depth': sum(g.get('by_depth', 0) for g in games),
        'by_stalls': sum(g.get('by_stalls', 0) for g in games),
        'stall_events': sum(g.get('stall_events', 0) for g in games),
        'avg_depth': (sum(g['depth_sum'] for g in games) / samples if samples else
                      sum(g.get('avg_depth', 0) * g['intervals'] for g in games) / max(intervals, 1)),
        'max_debt': max((g['max_debt'] for g in games), default=0),
        'ended': sum(1 for g in games if g['verdict_frame'] is not None),
        'debt_added': sum(g.get('debt_added', 0) for g in games),
        'as_matches': as_matches(games),
    }


def line(name, t):
    if not t['games']:
        return f'{name}: no games'
    pct = 100 * t['unhealthy'] / max(t['intervals'], 1)
    return (f"{name}: {t['games']} games, {t['minutes']:.1f} min judged; average depth {t['avg_depth']:.2f} frames; "
            f"unhealthy intervals {t['unhealthy']} of {t['intervals']} ({pct:.0f}%: time {t['by_time']}, depth {t['by_depth']}, "
            f"stalls {t['by_stalls']}; {t['stall_events']} stall events); debt at most {t['max_debt']} of {TERMINATE_DEBT}; "
            f"games the rule would have ended: {t['ended']}; as 3-minute matches: {t['as_matches'][0]} of {t['as_matches'][1]}")


# ---- this port against the kernel's -----------------------------------------------------------

DRIVER = r'''
#include <stdio.h>
#include "so_mq.h"
int main(void)
{
	SoMq q;
	int delay, frame, stalled, latest, res;
	unsigned long long t;
	if (scanf("%d", &delay) != 1)
		return 1;
	so_mq_start(&q, (u8)delay);
	while (scanf("%d %llu %d %d", &frame, &t, &stalled, &latest) == 4) {
		res = so_mq_frame(&q, frame, t, stalled, latest);
		if (res)
			printf("%d %d %d %d %d %d %d%c", frame, (int)q.change, (int)q.debt, (int)q.signal[0], (int)q.signal[1],
				(int)q.signal[2], res, 10);
	}
	return 0;
}
'''


def selftest():
    so = ROOT / 'Nintendont/kernel/slippi_online'
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        (tmp / 'driver.c').write_text(DRIVER)
        exe = tmp / 'mq_driver.exe'
        subprocess.run([sys.executable, '-m', 'ziglang', 'cc', '-O1', '-I', so / 'tests/shim', '-I', so,
                        tmp / 'driver.c', so / 'so_mq.c', '-o', exe], check=True)
        rng, intervals, verdicts, unhealthy = random.Random(7), 0, 0, 0
        for game in range(200):
            kind, delay = game % 4, rng.randint(1, 3)
            t, behind, slow = rng.randint(5000, 1000000), rng.randrange(2 if kind == 0 else 5), 0
            tracker, text, mine = Tracker(delay), [str(delay)], []

            def feed(frame, t, stalled, latest):
                text.append(f'{frame} {t} {int(stalled)} {latest}')
                was = tracker.verdict_frame
                i = tracker.on_frame(frame, t, stalled, latest)
                if i:
                    first = was is None and tracker.verdict_frame is not None
                    mine.append(f"{frame} {i['change']} {i['debt']} {i['time']} {i['depth']} {i['stalls']} {2 if first else 1}")

            for frame in range(1, 5401):
                if rng.randrange(40) == 0:
                    behind = rng.randrange(2 if kind == 0 else 10 if kind == 3 else 5)
                if kind == 3 and rng.randrange(300) == 0:
                    slow = rng.randrange(4)
                if kind >= 2 and rng.randrange(1500 if kind == 2 else 200) == 0:
                    for _ in range(rng.randint(1, 12)):
                        t += 14000 + rng.randrange(6000)
                        feed(frame, t, True, frame - 8)
                for _ in range(2 if rng.randrange(250) == 0 else 1):
                    t += FRAME_TIME_US * (100, 110, 150, 175)[slow] // 100 + (rng.randrange(400) if rng.randrange(3) == 0 else 0)
                    feed(frame, t, False, frame - behind + rng.randrange(2))
            theirs = subprocess.run([exe], input='\n'.join(text) + '\n', capture_output=True, text=True, check=True).stdout.split('\n')
            theirs = [x.strip() for x in theirs if x.strip()]
            if theirs != mine:
                bad = next((a, b) for a, b in zip(theirs + [''], mine + ['']) if a != b)
                sys.exit(f'game {game}: the kernel says "{bad[0]}", this port "{bad[1]}"')
            intervals += len(mine)
            unhealthy += sum(1 for m in mine if int(m.split()[1]) > 0)
            verdicts += tracker.verdict_frame is not None
    print(f"selftest: this port and the kernel's so_mq.c agree on {intervals} intervals of 200 games "
          f'({unhealthy} unhealthy, {verdicts} games past the limit)')


def connection(run_dir):
    """A run's connection as the relay had it: 'delay,jitter,loss' (ms, ms, percent)."""
    log = Path(run_dir) / 'relay.log'
    m = re.search(r'delay ([\d.]+) ms, jitter ([\d.]+) ms, loss ([\d.]+)%', log.read_text(errors='replace')) if log.exists() else None
    return ','.join(f'{float(x):g}' for x in m.groups()) if m else '?'


def cells(t):
    if not t or not t['games']:
        return '- | - | - | - | -'
    return (f"{t['avg_depth']:.2f} | {t['unhealthy']} of {t['intervals']} ({100 * t['unhealthy'] / max(t['intervals'], 1):.0f}%) | "
            f"{t['debt_added'] / max(t['minutes'], 1e-9):.1f} | {t['max_debt']} | {t['as_matches'][0]} of {t['as_matches'][1]}")


def table(runs):
    """A markdown table, a line a run: the PC player's score and its opponent's."""
    print('| Connection | Opponent | Games | The PC player: average depth | Unhealthy intervals | Debt added a minute | Highest debt | '
          '3-minute matches ended | The opponent: average depth | Unhealthy intervals | Debt added a minute | Highest debt | '
          '3-minute matches ended | Run |')
    print('|' + '---|' * 14)
    for run in runs:
        s = score_run(run)
        if s is None:
            continue
        pc = total(s['pc'])
        other = total(s['wii_log']) if s['wii_log'] else total(s['opponent'])
        print(f"| {connection(run)} | {'Wii' if s['wii_log'] else 'PC'} | {pc['games']} | {cells(pc)} | {cells(other)} | {Path(run).name} |")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('runs', nargs='*')
    ap.add_argument('--table', action='store_true', help='a markdown table, a line a run (a Wii by its own count, a PC by the relay)')
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--games', action='store_true', help='a line a game as well')
    ap.add_argument('--relay-opponent', action='store_true', help="a Wii opponent by the relay's clock too (reads shallow)")
    args = ap.parse_args()
    if args.selftest:
        selftest()
    args.runs = [Path(r) if Path(r).is_dir() else ROOT / 'build/sweep/runs' / r for r in args.runs]
    if args.table:
        table(args.runs)
        return
    for run in args.runs:
        s = score_run(run)
        if s is None:
            print(f'{run.name}: no relay-passes.txt (a run without --net, or from before 2026-11-13)')
            continue
        print(f"{run.name}: delay {s['delays'][0]} (PC player), {s['delays'][1]} (opponent); the PC player's stalls from {s['stalls_from']}")
        print('  ' + line('the PC player', total(s['pc'])))
        if s['wii_log']:
            print('  ' + line('the Wii, by its own count', total(s['wii_log'])))
        if not s['wii_log'] or args.relay_opponent:
            print('  ' + line('the opponent, by the relay', total(s['opponent'])))
        if args.games:
            for who in ('pc', 'wii_log') + (('opponent',) if not s['wii_log'] or args.relay_opponent else ()):
                for i, g in enumerate(s[who] or []):
                    depth = g['depth_sum'] / max(g['samples'], 1) if 'samples' in g else g.get('avg_depth', 0)
                    print(f"    {who} game {i + 1}: {g['intervals']} intervals, depth {depth:.2f}, debt at most {g['max_debt']}, "
                          f"{g['debt']} at the end, unhealthy by time/depth/stalls {g.get('by_time', 0)}/{g.get('by_depth', 0)}/"
                          f"{g.get('by_stalls', 0)}, ended at frame {g['verdict_frame']}")


if __name__ == '__main__':
    main()
