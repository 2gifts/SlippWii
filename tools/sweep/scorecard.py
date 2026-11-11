"""A run against the goal (docs/ROLLBACK_PLAN.md): INVALID, FAIL or PASS, target by target.

    python tools/sweep/scorecard.py RUN [RUN...] [--save-baseline]

RUN: a directory under build/sweep/runs. A PC against PC run (--baseline) with --save-baseline goes in
build/sweep/baselines.json, by connection, and Wii runs are then held against it.

INVALID means the run can't say anything: inputs that aren't like a person's, no games, no record.
Too little data never passes: a zero-event target with too few minutes reads SHORT, with how much it
proves so far (3 / minutes: zero events in N minutes bounds the rate at 3 per N, at 95%).
"""
import argparse
import collections
import json
import math
import re
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
import ab  # noqa: E402

RUNS = ROOT / 'build/sweep/runs'
BASELINES = ROOT / 'build/sweep/baselines.json'
FATAL = re.compile(r'CRASH:|PANIC|DESYNC at|rb MISS|rb LOST|rb VERIFY|PAD SENT AHEAD IS WRONG|PPC HUNG')
PERSON_CHANGES_PER_S = (15, 45)	# a person's input changes a second, from real replays: 29 (bots used to do 11.6)
UNFREEZE = 84
LATE_RETRACE_LINE = 22	# handled this late, the frame shows a refresh late, with nothing in the record's repeats: the capture card saw it at 22-31
PLAY_FROM_S = UNFREEZE / 60	# from when the players can move: a hitch at GO is as visible as any
MINUTES_FOR_ZERO = 30


def slp_events(path):
    d = path.read_bytes()
    i = d.find(b'raw[$U#l')
    n = struct.unpack('>I', d[i + 8:i + 12])[0]
    raw = d[i + 12:i + 12 + n] if n else d[i + 12:]
    sizes = {}
    for k in range((raw[1] - 1) // 3):
        o = 2 + 3 * k
        sizes[raw[o]] = struct.unpack('>H', raw[o + 1:o + 3])[0]
    pos = 1 + raw[1]
    while pos < len(raw) and raw[pos] in sizes:
        c = raw[pos]
        yield c, b'\0' + raw[pos + 1:pos + 1 + sizes[c]]
        pos += 1 + sizes[c]


def replay_numbers(paths):
    """Per replay's player: input changes a second; and visible corrections: how far a character moved
    between the copy of a frame shown first and the copy that replaced it."""
    changes, frames, snaps5, snaps10, largest, total_rb = [], 0, 0, 0, 0.0, 0
    for path in paths:
        pre = {}
        posts = collections.defaultdict(list)
        order = []
        for c, p in slp_events(path):
            if c == 0x3A:
                order.append(struct.unpack('>i', p[1:5])[0])
            elif c == 0x37 and not p[6]:
                pre[(struct.unpack('>i', p[1:5])[0], p[5])] = p
            elif c == 0x38 and not p[6]:
                posts[(struct.unpack('>i', p[1:5])[0], p[5])].append(p)
        for port in {k[1] for k in pre}:
            prev, ch, n = None, 0, 0
            for fr in sorted(k[0] for k in pre if k[1] == port):
                if fr < UNFREEZE:
                    continue
                p = pre[(fr, port)]
                cur = p[0x19:0x29] + p[0x31:0x37]
                n += 1
                ch += prev is not None and cur != prev
                prev = cur
            if n:
                changes.append(ch * 60 / n)
        frames += len(order)
        version, last = collections.Counter(), None
        for fr in order:
            if last is not None and fr <= last:
                total_rb += 1
                for port in (0, 1):
                    copies = posts.get((last, port), [])
                    i = version[last] - 1
                    if 0 <= i and i + 1 < len(copies):
                        a, b = copies[i], copies[i + 1]
                        dist = math.hypot(struct.unpack('>f', b[0x0A:0x0E])[0] - struct.unpack('>f', a[0x0A:0x0E])[0],
                                          struct.unpack('>f', b[0x0E:0x12])[0] - struct.unpack('>f', a[0x0E:0x12])[0])
                        snaps5 += dist > 5
                        snaps10 += dist > 10
                        largest = max(largest, dist)
            version[fr] += 1
            last = fr
    minutes = frames / 3600 or 1
    return {'changes_per_s': [round(c, 1) for c in changes], 'snaps5_min': snaps5 / minutes, 'snaps10_min': snaps10 / minutes,
            'largest_snap': round(largest, 1)}


def wii_record(run):
    """In play, from the game's own record: repeats and jumps, by what's next to them."""
    log = run / 'wii.log'
    if not log.exists():
        log = run / 'watch.log'
    if not log.exists():
        return None
    raw = log.read_text(encoding='utf-8', errors='replace')
    text = subprocess.run([sys.executable, str(HERE.parent / 'rollback/trace_view.py'), str(log), '--events'],
                          capture_output=True, text=True, encoding='utf-8', errors='replace').stdout
    plan = json.loads((run / 'plan-wii.json').read_text()) if (run / 'plan-wii.json').exists() else {}
    lras = plan.get('lras_s', 40) or 1e9
    over = [int(x) / 60 for x in re.findall(r'game over: (\d+) frames', raw)]
    out = {'seconds': 0.0, 'repeats': 0, 'jumps': 0, 'skip': 0, 'rollback': 0, 'other': 0, 'late_retraces': 0, 'games': 0, 'lost': 0}
    for i, g in enumerate(re.split(r'(?m)^(?==== game)', text)[1:]):
        ev = [(int(m[1]), float(m[2]), m[3]) for m in re.finditer(r'(?m)^\s+(\d+)\s+([\d.]+)s (.*)$', g)]
        end = min(over[i] if i < len(over) else 1e9, lras) - 0.5
        if end < 8 or not ev:
            continue
        out['games'] += 1
        out['seconds'] += end - PLAY_FROM_S
        lost = re.search(r'(\d+) LOST', g)
        out['lost'] += int(lost[1]) if lost else 0
        for r, s, e in ev:
            if not PLAY_FROM_S < s < end:
                continue
            if 'shown again' in e:
                out['repeats'] += 1
                near = [x[2] for x in ev if r - 2 <= x[0] <= r + 1]
                if any('rollback load' in x for x in near):
                    out['rollback'] += 1
                elif any(re.search(r'pass: [2-9] engine', x) for x in near):
                    out['skip'] += 1
                else:
                    out['other'] += 1
            elif e.startswith('picture moved'):
                out['jumps'] += 1
            else:
                m = re.match(r'retrace \d+ us after the last, VI line (\d+)', e)
                if m and int(m[1]) >= LATE_RETRACE_LINE:
                    out['late_retraces'] += 1
    out['fatal'] = sorted(set(m[0] for m in FATAL.finditer(raw)))
    return out


def acks(run):
    log = run / 'relay.log'
    if not log.exists():
        return None
    m = re.findall(r"relay: B's acks: median ([\d.]+) ms", log.read_text(encoding='utf-8', errors='replace'))
    return sorted(float(x) for x in m)[len(m) // 2] if m else None


def net_of(run):
    log = run / 'relay.log'
    if not log.exists():
        return '?'
    m = re.search(r'delay ([\d.]+) ms, jitter ([\d.]+) ms, loss ([\d.]+)', log.read_text(encoding='utf-8', errors='replace'))
    return f'{float(m[1]):.0f},{float(m[2]):.0f}' + (f',{float(m[3]):.0f}' if float(m[3]) else '') if m else '?'


def score(run, baselines):
    lines, verdict = [], 'PASS'

    def mark(state):
        nonlocal verdict
        order = ['PASS', 'SHORT', 'MISS', 'FAIL', 'INVALID']
        if order.index(state) > order.index(verdict):
            verdict = state

    def row(name, value, target, state):
        lines.append(f'| {name} | {value} | {target} | {state} |')
        mark(state)

    net = net_of(run)
    base = baselines.get(net, {})

    def against(ok):
        """With no PC against PC run at this connection there's nothing to compare with: never a pass."""
        return 'SHORT' if not base else 'PASS' if ok else 'MISS'

    replays = sorted((run / 'dolphin-replays').rglob('*.slp')) if (run / 'dolphin-replays').exists() else []
    if not replays:
        return f'## {run.name} ({net}): INVALID: no replays\n'
    f = ab.felt(run)
    rp = replay_numbers(replays)
    low, high = PERSON_CHANGES_PER_S
    ok = all(low <= c <= high for c in rp['changes_per_s'])
    row('Input changes a second, each player', rp['changes_per_s'], f'{low}-{high}', 'PASS' if ok else 'INVALID')
    rec = wii_record(run)
    is_wii = rec is not None and rec['games'] > 0
    if is_wii:
        row('Fatal events', rec['fatal'] or 'none', 'none', 'FAIL' if rec['fatal'] else 'PASS')
        minutes = rec['seconds'] / 60
        rate = rec['repeats'] / minutes if minutes else 0
        state = 'MISS' if rec['repeats'] else ('PASS' if minutes >= MINUTES_FOR_ZERO else 'SHORT')
        row('Repeats in play (record)', f"{rec['repeats']} in {minutes:.1f} min = {rate:.1f}/min "
            f"(skip {rec['skip']}, rollback {rec['rollback']}, other {rec['other']}); jumps {rec['jumps']}",
            f'0 in {MINUTES_FOR_ZERO} min', state)
        row('Retraces handled too late, in play', f"{rec['late_retraces']} = {rec['late_retraces'] / minutes if minutes else 0:.1f}/min",
            f'0 in {MINUTES_FOR_ZERO} min', 'MISS' if rec['late_retraces'] else state)
        if rec['lost']:
            row('Record events lost', rec['lost'], 0, 'INVALID')
        if (run / 'capture.mkv').exists():
            import decode_marker
            s = decode_marker.summarize(run)
            row('On screen: off-rhythm steps, in play', f"{s['off_rhythm_steps']} in {s['minutes_in_play']} min; "
                f"{s['off_rhythm_not_in_record']} with nothing in the record; {s['after_a_nudge']} after a nudge"
                + (f"; {s['card_slips']} card slips not counted" if s.get('card_slips') else ''), 'none unseen',
                'MISS' if s['off_rhythm_not_in_record'] else 'PASS')
        a = acks(run)
        if a is not None:
            row('Reply to an input packet', f'{a:.1f} ms', '3 ms or less (PC: 0.05)', 'PASS' if a <= 3 else 'MISS')
    row('PC freezes in play, a game', f"{f['halts_play']:.1f}", 0, 'PASS' if not f['halts_play'] else 'MISS')
    row('PC rollbacks a minute', f"{f['rb_min']:.0f}" + (f" (PC vs PC {base['rb_min']:.0f})" if base else ''),
        'as PC vs PC', against(base and f['rb_min'] <= base['rb_min'] * 1.1))
    row('PC frames re-run a rollback', f"{f['depth']:.2f}, deepest {f['deepest']}" + (f" (PC vs PC {base['depth']:.2f})" if base else ''),
        'within 5%', against(base and f['depth'] <= base['depth'] * 1.05 + 0.01))
    row('PC visible snaps over 5 units a minute', f"{rp['snaps5_min']:.1f}, largest {rp['largest_snap']}" +
        (f" (PC vs PC {base['snaps5_min']:.1f})" if base else ''), 'at most 10% more',
        against(base and rp['snaps5_min'] <= base['snaps5_min'] * 1.1 + 0.5))
    row('PC wait at game start, frames', f"{f['start_frames']:.0f}" + (f" (PC vs PC {base['start_frames']:.0f})" if base else ''),
        'at most 5 above PC', against(base and f['start_frames'] <= base['start_frames'] + 5))
    head = f"## {run.name}: {net}, {f['games']} games: **{verdict}**\n\n| Target | Now | Wanted | |\n|---|---|---|---|\n"
    numbers = {'rb_min': f['rb_min'], 'depth': f['depth'], 'start_frames': f['start_frames'], 'snaps5_min': rp['snaps5_min']}
    return head + '\n'.join(lines) + '\n', net, numbers


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('runs', nargs='+')
    ap.add_argument('--save-baseline', action='store_true')
    args = ap.parse_args()
    baselines = json.loads(BASELINES.read_text()) if BASELINES.exists() else {}
    for name in args.runs:
        out = score(RUNS / name, baselines)
        if isinstance(out, str):
            print(out)
            continue
        text, net, numbers = out
        print(text)
        if args.save_baseline:
            baselines[net] = numbers
            BASELINES.write_text(json.dumps(baselines, indent=1))


if __name__ == '__main__':
    main()
