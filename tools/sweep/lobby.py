"""Four players by themselves: Slippi's Teams or Party mode, played by bots on
this PC and the LAN. Three copies of Slippi Dolphin, and a fourth player that
is the Wii (--wii-ip) or one more Dolphin copy (the reference: what four PCs
do). Everything stays here: the lobby server is so_pc_lobby_mm.exe
(tests/pc/build.sh), the accounts are dummies, no Slippi server is reached.

    python tools/sweep/lobby.py --mode teams|party [--wii-ip IP] [--rounds N]

Every player's replay of every game is compared with the first Dolphin's;
the report is build/sweep/runs/<stamp>/report.md. Teams: all four enter the
same lobby code. Party: all four queue. --wii-seat N puts the Wii in port N
(port 1 is the host, whose random seed and stage count for all).
Against more than one opponent the Wii's game waits for inputs (so_exi.c,
rollback_wanted); --force-rollback and --lag are for measuring why.
One run at a time, as run_sweep.py; stop.py stops this too.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / 'tools'))
import decode_marker  # noqa: E402
import dolphin_copy  # noqa: E402
import match_quality as mq  # noqa: E402
import run_sweep as rs  # noqa: E402
import slp_compare  # noqa: E402
import unthrottle  # noqa: E402

# the first is the Wii, or its stand-in (dolphin_copy.py names an account by its code's letters: all different)
CODES = ['WIIT#001', 'PTWO#002', 'PTRI#003', 'PFOR#004']
LOBBY_CODE = 'TEAM#444'
PORTS = [51900, 51500, 51600, 51700]  # each Dolphin copy plays from its own
MODES = {'teams': 3, 'party': 4}
# stage icons the fake server's list allows (no Fountain of Dreams with four players)
LOBBY_STAGES = [24, 25, 26, 18, 6]
MM = Path(os.environ.get('TEMP', '/tmp')) / 'slippi_online_pc/so_pc_lobby_mm.exe'


def make_plan(rounds, players):
    """Round i: player k on ORDER[i + 6k], every player on the same stage."""
    order = rs.ORDER
    return [([order[(i + 6 * k) % len(order)] for k in range(players)], LOBBY_STAGES[i % len(LOBBY_STAGES)])
            for i in range(rounds)]


def seats_of(args):
    """The connect codes in the order of their ports."""
    seats = CODES[1:args.players]
    seats.insert(min(args.wii_seat, args.players) - 1, CODES[0])
    return seats


def autoplay_codes(run_dir, plan, args):
    """One bot per player; the first one's also as the Wii's boot code."""
    seats = seats_of(args)
    for k in range(args.players):
        name = 'autoplay-wii' if k == 0 else f'autoplay-p{k + 1}'
        spec = {'rounds': [[chars[k], stage] for chars, stage in plan], 'lras_s': args.lras_s,
                'seed': args.seed * 8 + k + 1, 'person': 0 if args.plain_bot else 1, 'mode': MODES[args.mode],
                'team': args.teams[seats.index(CODES[k])] if args.teams else 0}
        (run_dir / f'plan-p{k + 1}.json').write_text(json.dumps(spec))
        rs.run(sys.executable, HERE / 'build_autoplay.py', '--plan', run_dir / f'plan-p{k + 1}.json',
               '--out', run_dir / name)


def build_wii(run_dir, plan, args, ip):
    """The Wii's test build, as run_sweep.py makes it (its bot is ours)."""
    opts = ['--wii-ip', args.wii_ip, '--kernel', f'selftest_code={LOBBY_CODE}']
    if args.net:
        opts += ['--kernel', 'lan_ip=127.0.0.2']  # the address the others are given for it: the relay's
    if args.delay != 2:
        opts += ['--kernel', f'delay={args.delay}']
    if not args.lockstep:
        opts += ['--rollback', str(args.lag)]
    if args.lag or args.force_rollback:
        opts += ['--kernel', 'test_multi_rollback=1']
    for kv in args.kernel:
        opts += ['--kernel', kv]
    for b in args.boot_arg:
        opts += [f'--boot-arg={b}']
    if args.next:
        opts += ['--next'] + ([] if args.wii_old_mm else ['--next-mm'])
    ns = rs.parser().parse_args(opts)
    rs.autoplay_codes = lambda *a, **k: None
    rs.build(run_dir, plan, ns, ip)


def start_of(path):
    """(stage id, [external character id per port or None]) from a replay's start."""
    try:
        start, _ = slp_compare.index(path)
        chars = []
        for port in range(4):
            kind = start[0x66 + 0x24 * port]  # player type: 0 human, 3 none
            chars.append(start[0x65 + 0x24 * port] if kind == 0 else None)
        return slp_compare.field(start, 0x13, 2, '>H'), chars, start[0x0D] if len(start) > 0x0D else None
    except (Exception, SystemExit):
        return None, [None] * 4, None


def teams_of(path):
    """Each port's team in a Teams game, by a replay's start ('red', 'blue', 'green'), else ''."""
    try:
        start, _ = slp_compare.index(path)
        if not start[0x0D]:
            return [''] * 4
        return [' (' + ('red', 'blue', 'green')[start[0x6E + 0x24 * port] % 3] + ')' for port in range(4)]
    except (Exception, SystemExit):
        return [''] * 4


def char_name(ext):
    if ext is None:
        return '-'
    if ext == 19:
        return 'Sheik'
    return rs.CHARS.get(rs.EXTERNAL_TO_ICON.get(ext, -1), f'#{ext}')


def frames_of(path):
    try:
        _, events = slp_compare.index(path)
        return max(k[0] for k in events)
    except (Exception, SystemExit, ValueError):
        return None


def quality(run_dir, args, names):
    """With --net: each Dolphin's game by Slippi's match quality rule, against each of the others,
    from the relay's record of the pads (lobby_relay.py --passes): its frames by the relay's clock and
    the other's newest input in its hands at each. Dolphin's own rule goes by its first opponent only;
    here every pair is scored, so the first account's (the Wii's, or its stand-in's) cost shows for each."""
    path = run_dir / 'lobby-passes.txt'
    if not path.exists():
        return []
    sent, got = {}, {}
    for line in path.read_text().splitlines():
        f = line.split()
        if len(f) != 5:
            continue
        (sent if f[0] == 'S' else got).setdefault((f[1], f[2]), []).append((int(float(f[3]) * 1e6), int(f[4])))
    players = [f'p{k + 1}' for k in range(args.players)]
    lines = ['', f'By Slippi\'s match quality rule, each Dolphin against each other player (net {args.net}): the '
             'average rollback depth, and the unhealthy 150-frame intervals of all.', '',
             '| Player | ' + ' | '.join(f'against {names[k]}' for k in range(args.players)) + ' |',
             '|---|' + '---|' * args.players]
    for k, me in enumerate(players):
        if k == 0 and args.wii_ip:
            continue  # (the Wii by its own count: its log's "game quality" lines, below)
        cells = []
        for j, other in enumerate(players):
            if other == me:
                cells.append('')
                continue
            games = [g for g in mq.split_games(sent.get((me, other), [])) if g[-1][1] - g[0][1] > 300]
            scores = [mq.score_side(g, got.get((me, other), []), 2).summary() for g in games]
            t = mq.total(scores) if scores else None
            cells.append(f"{t['avg_depth']:.2f}, {t['unhealthy']} of {t['intervals']}" if t and t['intervals'] else '-')
        lines.append(f'| {names[k]} | ' + ' | '.join(cells) + ' |')
    return lines


def leave_check(run_dir, args):
    """Party after 3.6.4, the Wii having left the game by its bot's quit: (what is wrong or None, a line
    for the report). Each Dolphin must have settled on the same last input of the Wii's, and so on the
    same frame its character is out of the game from (Slippi's "Settled on player" line)."""
    seat = min(args.wii_seat, args.players) - 1
    settled = {}
    for k in range(1, args.players):
        log = run_dir / f'dolphin-p{k + 1}-online.log'
        text = log.read_text(encoding='utf-8', errors='replace') if log.exists() else ''
        m = re.search(rf'Settled on player {seat}\. Last input frame: (-?\d+), despawn frame: (-?\d+)', text)
        settled[f'Dolphin {k + 1}'] = (int(m.group(1)), int(m.group(2))) if m else None
    if any(v is None for v in settled.values()):
        return f'the Wii left the game, and not every Dolphin took its character out: {settled}', ''
    if len(set(settled.values())) != 1:
        return f'the Wii left the game, and the Dolphins took its character out from different frames: {settled}', ''
    last, out = next(iter(settled.values()))
    return None, (f"The Wii left the game by its bot's quit: every Dolphin has its last input as frame {last} and its "
                  f'character out of the game from frame {out}, and played on.')


def report(run_dir, args, plan, failure, cpu, returned):
    """Each game: every player's replay against the first Dolphin's."""
    names = ['the Wii' if args.wii_ip else 'Dolphin 1'] + [f'Dolphin {k + 1}' for k in range(1, args.players)]
    folders = [run_dir / f'replays-p{k + 1}' for k in range(args.players)]
    games = [sorted(f.glob('*.slp'), key=lambda p: p.stat().st_mtime) for f in folders]
    n = max((len(g) for g in games), default=0)
    lines = [f'# Lobby {run_dir.name}: {args.mode}, {args.players} players, {names[0]} in the first account', '']
    if failure:
        lines += [f'**FAILED: {failure}**', '']
    if returned:
        lines += [returned, '']
    rows, match, compared = [], 0, 0
    for i in range(n):
        ref = games[1][i] if i < len(games[1]) else None
        stage_id, chars, _ = start_of(ref) if ref else (None, [None] * 4, None)
        stage = next((nm for nm, sid in rs.STAGES.values() if sid == stage_id), f'stage {stage_id}')
        cells = []
        for k in range(args.players):
            if k == 1:
                continue
            if i >= len(games[k]) or ref is None:
                cells.append('no replay')
                continue
            out = subprocess.run([sys.executable, ROOT / 'tools/slp_compare.py', games[k][i], ref, '--context', '2'],
                                 capture_output=True, text=True).stdout
            (run_dir / f'compare-game{i + 1}-p{k + 1}.txt').write_text(out, encoding='utf-8')
            first = re.search(r'first differ at frame (-?\d+)', out)
            ends = [int(e) for e in re.findall(r'frames -?\d+\.\.(-?\d+)', out)]
            compared += 1
            if 'MATCH:' in out:
                match += 1
                cells.append('match')
            elif first and ends and int(first.group(1)) > min(ends) - 7:
                # (a game ends on frames a Dolphin had run on a guess of someone's input and never ran again:
                # its replay's last frames, as many as it was rolling back, are that guess)
                match += 1
                cells.append('match (but its last frames, not final in one)')
            else:
                cells.append(f'DESYNC at frame {first.group(1)}' if first else 'not compared')
        teams = teams_of(ref) if ref else [''] * 4
        rows.append(f'| {i + 1} | {stage} | ' + ' / '.join(char_name(c) + t for c, t in zip(chars, teams))
                    + f' | {frames_of(ref) if ref else ""} | '
                    + ' | '.join(cells) + ' |')
    lines += [f'{n} games; {compared} replays compared with Dolphin 2\'s: {match} match, {compared - match} do not.', '',
              '| # | Stage | Ports 1-4 | Frames | ' + ' | '.join(names[k] for k in range(args.players) if k != 1) + ' |',
              '|---|---|---|---|' + '---|' * (args.players - 1), *rows, '']
    for k in range(args.players):
        log = run_dir / f'dolphin-p{k + 1}-online.log'
        if not log.exists():
            continue
        text = log.read_text(encoding='utf-8', errors='replace')
        limit = len(re.findall(r'Halting for one frame due to rollback limit', text))
        sync = sum(int(x) for x in re.findall(r'due to time sync\. Offset: -?\d+ us\. Frames: (\d+)', text))
        lines.append(f'{names[k]} (Slippi Dolphin): halted {limit} frames at the rollback limit, {sync} for time sync.')
    lines += quality(run_dir, args, names)
    relay = run_dir / 'relay.log'
    if relay.exists():
        last = [l for l in relay.read_text(errors='replace').splitlines() if 'relay: in all' in l]
        lines += ['', 'By the relay, over the run (p1 is the first account):', '', '```'] + last + ['```']
    lines += ['', f"The PC's CPU during the run: {cpu.summary()}.", '']
    if args.wii_ip:
        keep = re.compile(r'CRASH|PANIC|DESYNC|rb (MISS|VERIFY|LOST)|game (over|checksums|rollback|quality|sockets)|PERF rb|'
                          r'rolls back|waits for inputs|exit:|mm: |np: |client: |di: a command')
        lines += ['## The Wii', '', '```'] + [l for l in rs.wii_lines(run_dir) if keep.search(l)] + ['```', '']
    mm = run_dir / 'lobby_mm.log'
    if mm.exists():
        lines += ['## The lobby server', '', '```'] + [l for l in mm.read_text(errors='replace').splitlines()
                                                      if 'MATCHED' in l] + ['```', '']
    (run_dir / 'report.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print('\n'.join(lines[:12 + n]))
    print(f'report: {run_dir / "report.md"}')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--mode', required=True, choices=list(MODES))
    ap.add_argument('--players', type=int, default=4, choices=[2, 3, 4],
                    help='the lobby size (Teams with 3: 2 against 1, or 1 against 1 against 1)')
    ap.add_argument('--codes', type=lambda v: int(v, 0), default=0, metavar='BITS',
                    help="the Wii's loader switches, as run_sweep.py's --codes (128: Team Indicators)")
    ap.add_argument('--rounds', type=int, default=3)
    ap.add_argument('--lras-s', type=int, default=40, help='seconds of play before each game is quit (0: never)')
    ap.add_argument('--seed', type=int, default=0)
    ap.add_argument('--plain-bot', action='store_true', help="the bots' sticks are still")
    ap.add_argument('--wii-ip', default=None, help="the Wii's address; without it a fourth Dolphin copy plays its part")
    ap.add_argument('--wii-seat', type=int, default=4, choices=[1, 2, 3, 4], help='the port the first account gets')
    ap.add_argument('--teams', type=lambda v: [int(t) for t in v.split(',')], metavar='T,T,T,T',
                    help="Teams: the team each port's player picks with Slippi's team button, 0 red, 1 blue, 2 green "
                         '(0,0,1,1: ports 1 and 2 against 3 and 4). Without it nobody presses the button: all are '
                         'red, and the game draws the teams')
    ap.add_argument('--lockstep', action='store_true',
                    help="a build without the rollback engine (the Wii's game waits for inputs with it too, against "
                         'more than one opponent: this is the build that cannot do otherwise)')
    ap.add_argument('--force-rollback', action='store_true',
                    help='the Wii rolls back against its three opponents (test_multi_rollback): what that costs')
    ap.add_argument('--lag', type=int, default=0, choices=range(0, 7), metavar='FRAMES',
                    help="rolling back: the Wii is shown its opponents' inputs this many frames late (test_lag_frames), "
                         'so every change of an input is a rollback that deep: what its frames then cost '
                         '(with --force-rollback implied)')
    ap.add_argument('--items', type=lambda v: int(v, 0), default=0, help='Party: the item switch bits the server names')
    ap.add_argument('--net', metavar='DELAY[,JITTER[,LOSS]]',
                    help='play through lobby_relay.py: a one-way delay and jitter (ms) and loss (percent) between '
                         'every two players, as on an internet connection')
    ap.add_argument('--delay', type=int, default=2, choices=range(1, 10), metavar='FRAMES',
                    help="the Wii's input delay (Slippi's delay frames; 2 as Slippi's standard)")
    ap.add_argument('--no-mirror', action='store_true',
                    help="timing runs: no replay stream from the Wii, so its replays are not compared (what a player's "
                         "Wii does; the Wii's own count of the frames whose checksums matched is in its log)")
    ap.add_argument('--drop', metavar='PLAYER,SECONDS',
                    help='one Dolphin copy (2 to 4) is closed this long into the first game, as a player who leaves: '
                         'the others play on (its replay ends there; the rest are compared to the end)')
    ap.add_argument('--stages', metavar='ID,ID,...',
                    help="the stages the lobby server allows (its own list without this): one id, and every game is "
                         "on that stage (8: Yoshi's Story)")
    ap.add_argument('--next', action='store_true',
                    help="Slippi's development Dolphin (what comes after 3.6.4) and, for the Wii, a build that speaks "
                         'that version (run_sweep.py --next)')
    ap.add_argument('--wii-old-mm', action='store_true',
                    help='with --next: the Wii queues the old way (ENet, no TLS session); its game is still the next version')
    ap.add_argument('--timeout-min', type=int, default=30)
    ap.add_argument('--capture', action='store_true', help="record the Wii's video output (capture.mkv)")
    ap.add_argument('--skip-build', help="reuse this run directory's build")
    ap.add_argument('--kernel', action='append', default=[], metavar='KEY=VALUE', help="a line for the Wii's config.ini")
    ap.add_argument('--boot-arg', action='append', default=[], metavar='OPTION',
                    help='one more option for tools/build_online_boot.py')
    args = ap.parse_args()
    args.next = args.next or rs.slippi_version.next_release()
    if args.mode == 'teams' and args.players < 3:
        args.players = 4
    if not MM.exists():
        sys.exit(f'{MM}: build it first (Nintendont/kernel/slippi_online/tests/pc/build.sh)')
    rs.take_lock()
    ip = rs.pc_ip()
    plan = make_plan(args.rounds, args.players)
    run_dir = Path(args.skip_build or rs.OUT / 'runs' / time.strftime('%Y%m%d-%H%M%S'))
    run_dir.mkdir(parents=True, exist_ok=True)
    (run_dir / 'plan.json').write_text(json.dumps(plan))
    if not args.skip_build:
        autoplay_codes(run_dir, plan, args)
        if args.wii_ip:
            build_wii(run_dir, [(c[0], c[1], s) for c, s in plan], args, ip)
    rs.kill_leftovers()
    # seats: the server gives ports in this order of connect codes
    seats = seats_of(args)
    dolphins = {}
    for k in range(args.players):
        if k == 0 and args.wii_ip:
            continue
        gecko = run_dir / ('autoplay-wii.gecko.txt' if k == 0 else f'autoplay-p{k + 1}.gecko.txt')
        # (--net: each copy is known to the other copies at a loopback address of its own, which the relay holds)
        dolphins[k] = dolphin_copy.make(f'dolphin-lobby-{k + 1}', CODES[k], None, gecko, '127.0.0.1',
                                        lan_ip=f'127.0.0.{11 + k}' if args.net else ip,
                                        port=PORTS[k], teams_code=LOBBY_CODE, next_version=args.next)
    # one external address for all: each is given the others' LAN addresses. The server sits behind
    # udp_front.py: the firewall lets Python's traffic in from the Wii, not a new program's.
    # --net with the Wii: it is elsewhere for the copies (its external address the relay's 127.0.0.2), and
    # they for it (this PC's address and their ports, the relay's too), so each side is given the other's
    # external address, and the copies each other's loopback ones
    ext = []
    if args.net and args.wii_ip:
        ext = ['--ext', f'{CODES[0]}=127.0.0.2:{rs.WII_PORT}']
        for k in dolphins:
            ext += ['--ext', f'{CODES[k]}={ip}:{PORTS[k]}']
    procs = [rs.start('lobby_mm', [MM, 43114, '--external-ip', '10.0.0.1', '--party-size', args.players,
                                   '--items', args.items, '--order', ','.join(seats),
                                   '--seconds', args.timeout_min * 60 + 120, *ext,
                                   *(['--ws-port', rs.WS_PORT] if args.next else []),
                                   *(['--stages', args.stages] if args.stages else [])], run_dir),
             rs.start('mm_front', [sys.executable, '-u', HERE / 'udp_front.py', 43113, 43114], run_dir)]
    dolphin_env = {}
    if args.next:
        # Slippi's development Dolphins queue over the WebSocket service (no TLS for them), the Wii through
        # TLS in front of it; STUN stand-ins for both (run_sweep.py --next has the same)
        dolphin_env = {'env': dolphin_copy.next_env(f'ws://127.0.0.1:{rs.WS_PORT}/v1/queue')}
        procs.append(rs.start('stun', [sys.executable, '-u', HERE / 'stun_fake.py', *rs.STUN_PORTS,
                                       dolphin_copy.NEXT_STUN_PORTS[0]], run_dir))
        if args.wii_ip:
            procs.append(rs.start('tls_front', [sys.executable, '-u', HERE / 'tls_front.py', '--dir', run_dir, '--listen',
                                                rs.TLS_PORT, '--backend', f'127.0.0.1:{rs.WS_PORT}', '--host', rs.NEXT_HOST],
                                  run_dir))
    if args.net:
        # (lobby_relay.py: each copy at a loopback address for the other copies and, for the Wii, at this
        # PC's address; the Wii at 127.0.0.2 for the copies)
        specs = [f'p{k + 1}=127.0.0.{11 + k}:{PORTS[k]}=127.0.0.1:{PORTS[k]}'
                 + (f'={ip}:{PORTS[k]}' if args.wii_ip else '') for k in dolphins]
        if args.wii_ip:
            specs.insert(0, f'p1=127.0.0.2:{rs.WII_PORT}={args.wii_ip}:{rs.WII_PORT}')
        procs.append(rs.start('relay', [sys.executable, '-u', HERE / 'lobby_relay.py', *specs, *rs.net_args(args.net),
                                        '--passes', run_dir / 'lobby-passes.txt', '--until', run_dir / 'relay-stop'],
                              run_dir))
        time.sleep(0.5)
        unthrottle.unthrottle(procs[-1][0].pid)  # its delays are timed: it must not wait for the CPU
    if args.wii_ip and not args.no_mirror:
        procs.append(rs.start('mirror', [sys.executable, '-u', ROOT / 'tools/wii_mirror.py', '--wii-ip', args.wii_ip,
                                         '--out', run_dir / 'replays-p1'], run_dir))
    if args.wii_ip:
        procs.append(rs.start('watch', [sys.executable, '-u', ROOT / 'tools/online_selftest.py', 'watch', '--boot',
                                        '--dol', run_dir / 'loader-sweep.dol', '--wii-ip', args.wii_ip,
                                        *(['--cfg', rs.loader_cfg(run_dir, codes=args.codes)] if args.codes else []),
                                        *(['--first-arg', 'slippwii-files-root=/slippwii-next'] if args.next else []),
                                        '--timeout', args.timeout_min * 60], run_dir))
    if args.capture and args.wii_ip:
        procs.append(rs.start('capture', [decode_marker.FFMPEG, '-y', '-f', 'dshow', '-rtbufsize', '200M', '-vcodec', 'mjpeg',
                                          '-video_size', '720x480', '-framerate', '25', '-i', 'video=AV TO USB2.0',
                                          '-c', 'copy', run_dir / 'capture.mkv'], run_dir))
    proc_names = [''] * len(procs)
    for k, d in dolphins.items():
        procs.append(rs.start(f'dolphin-p{k + 1}', [d / 'Slippi Dolphin.exe', '-e', rs.ISO], run_dir, **dolphin_env))
        proc_names.append(f'dolphin-p{k + 1}')
        time.sleep(3)
        unthrottle.unthrottle(procs[-1][0].pid)
    cpu = unthrottle.CpuLoad()
    ref = dolphins[1] / 'replays'
    print(f'lobby {run_dir.name}: {args.mode}, {args.players} players, {args.rounds} rounds'
          + (f'. Open the Homebrew Channel on the Wii ({args.wii_ip}); the test build is sent to it.' if args.wii_ip else ''))
    started, failure, done, last_change, dropped = time.time(), None, 0, time.time(), False
    left_wanted, left = bool(args.next and args.mode == 'party' and args.wii_ip and args.lras_s), False
    try:
        while True:
            games = sorted(ref.rglob('*.slp'), key=lambda f: f.stat().st_mtime)
            if len(games) != done:
                done, last_change = len(games), time.time()
                print(f'{time.strftime("%H:%M:%S")} game {done} of {args.rounds} started', flush=True)
            if args.drop and done == 1 and not dropped and time.time() - last_change > int(args.drop.split(',')[1]):
                k = int(args.drop.split(',')[0]) - 1
                for (proc, _), name in zip(procs, proc_names):
                    if name == f'dolphin-p{k + 1}':
                        proc.terminate()
                        print(f'{time.strftime("%H:%M:%S")} Dolphin {k + 1} closed (--drop)', flush=True)
                dropped = True
            if done >= args.rounds and time.time() - games[-1].stat().st_mtime > 20:
                break
            # Party after 3.6.4: the quit takes the Wii out of the game alone and the three Dolphins play on,
            # for up to 8 minutes (their bots never quit). That is the test, and the end of this lobby: each
            # Dolphin must have taken the Wii's character out, all from the same frame (leave_check).
            if left_wanted and any('the player leaves the game' in l for l in rs.wii_lines(run_dir)):
                time.sleep(8)
                left = True
                break
            if args.wii_ip:
                bad = next((l for l in rs.wii_lines(run_dir) if rs.FAILURES.search(l)), None)
                if bad:
                    failure = f'the Wii reported: {bad}'
                    time.sleep(5)
                    break
            if time.time() - started > args.timeout_min * 60:
                failure = 'timed out'
                break
            # (Party's games have no pause, so none is quit: each runs its 5 minutes)
            quiet = (330 if args.mode == 'party' else args.lras_s) + 240
            if time.time() - last_change > quiet:
                failure = f'no game started for {quiet} s (stuck in the menus?)'
                break
            if (run_dir / 'stop').exists():
                break
            time.sleep(5)
            cpu.sample()
    except KeyboardInterrupt:
        failure = 'stopped'
    finally:
        if args.wii_ip:
            rs.send_exit(args.wii_ip)
        if args.net:  # (the relay ends by itself, with the run's numbers)
            (run_dir / 'relay-stop').write_text('')
            time.sleep(1.5)
        for p, log in procs:
            if p.poll() is None:
                p.terminate()
            log.close()
        rs.kill_leftovers()
    for k, d in dolphins.items():
        if (d / 'User/Logs/dolphin.log').exists():
            shutil.copy(d / 'User/Logs/dolphin.log', run_dir / f'dolphin-p{k + 1}-online.log')
        dest = run_dir / f'replays-p{k + 1}'
        dest.mkdir(exist_ok=True)
        for f in sorted((d / 'replays').rglob('*.slp'), key=lambda f: f.stat().st_mtime):
            shutil.copy2(f, dest / f.name)
    returned = None
    if args.wii_ip:
        back = rs.wait_hbc(args.wii_ip)
        returned = (f'The Wii was back at the Homebrew Channel {back} s after the exit request.' if back is not None
                    else 'The Wii did NOT come back to the Homebrew Channel (2 minutes).')
        logs = sorted((ROOT / 'build/online-selftest').glob('2026*/wii.log'), key=lambda p: p.stat().st_mtime)
        if logs and logs[-1].stat().st_mtime >= started:
            shutil.copy(logs[-1], run_dir / 'wii.log')
    if left:
        problem, said = leave_check(run_dir, args)
        failure = failure or problem
        returned = f'{returned or ""} {said}'.strip()
    if failure:
        (run_dir / 'failure.txt').write_text(failure + '\n', encoding='utf-8')
    report(run_dir, args, plan, failure, cpu, returned)
    if rs.LOCK.exists():
        rs.LOCK.unlink()
    sys.exit(1 if failure else 0)


if __name__ == '__main__':
    main()
