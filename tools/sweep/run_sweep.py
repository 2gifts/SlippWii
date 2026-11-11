"""Automated Wii-vs-Dolphin sweep: the Wii and a local copy of Slippi
Dolphin play each other by themselves (the autoplay), round after round,
across stages and characters; every game's two replays are compared and the
Wii's own log is kept. Everything stays on this PC and the LAN: the fake
matchmaking server, dummy accounts, no Slippi servers.

    python tools/sweep/run_sweep.py [--rounds N] [--lras-s S] [--wii-ip IP]
    (then open the Homebrew Channel on the Wii; the build is sent to it)

Steps: plan -> autoplay codes for both sides -> a TEST build of Nintendont
(compiled-in test settings and the autoplay; the repo's own files are put
back right after) -> fake server + Dolphin copy + Wii boot + replay mirror
-> waits for the rounds -> report in build/sweep/runs/<stamp>/report.md.

Unattended: the run stops early on a crash, panic or stall in the
Wii's log, and at the end the Wii goes back to the Homebrew Channel by itself
(test_control, tools/ppc/test_exit.s; a crash goes back on its own), ready
for the next run. With --rollback LAG the Wii rolls back, its opponent's
inputs shown LAG frames late (test_lag_frames); with --spike MS as well,
held back MS every 5 s, as in a lag spike (test_spike_ms).

With --human a person plays the Wii, against the PC's bot: to feel a connection
(--net) rather than measure one. Enter the bot's code (PEER#002) in Direct on the
Wii, and pick a character. It runs until --rounds games are over or a file named
"stop" appears in the run's directory; D-pad presses are marked in the record.
"""
import argparse
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
NIN = ROOT / 'Nintendont'
OUT = ROOT / 'build/sweep'
TOOLS = Path(os.environ.get('SLIPPWII_TEST_TOOLS', ROOT.parent / 'slippi-3ds/native-3ds/build/slippi-tools'))
ISO = Path.home() / 'Downloads/SSB/SSBMv102.iso'
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / 'tools'))
import dolphin_copy  # noqa: E402
import decode_marker  # noqa: E402
import unthrottle  # noqa: E402

# CSS icon index -> name (Melee's CSS icon table, 0x803F0B24)
CHARS = {0: 'Dr. Mario', 1: 'Mario', 2: 'Luigi', 3: 'Bowser', 4: 'Peach', 5: 'Yoshi', 6: 'DK', 7: 'Falcon',
         8: 'Ganondorf', 9: 'Falco', 10: 'Fox', 11: 'Ness', 12: 'Ice Climbers', 13: 'Kirby', 14: 'Samus',
         15: 'Zelda', 16: 'Link', 17: 'Young Link', 18: 'Pichu', 19: 'Pikachu', 20: 'Jigglypuff', 21: 'Mewtwo',
         22: 'Game & Watch', 23: 'Marth', 24: 'Roy'}
# SSS icon index -> (name, stage id in replays)
STAGES = {24: ('Battlefield', 31), 25: ('Final Destination', 32), 26: ('Dreamland', 28),
          18: ('Pokemon Stadium', 3), 8: ('Fountain of Dreams', 2), 6: ("Yoshi's Story", 8)}
# Characters with randomness or extra state first: they matter most.
ORDER = [12, 4, 22, 2, 14, 5, 19, 10, 9, 23, 15, 20, 7, 8, 0, 1, 6, 3, 16, 17, 11, 13, 18, 21, 24]
WII_CODE, DOLPHIN_CODE = 'WIIT#001', 'PEER#002'
WII_PORT = 41234  # the Wii's session port in sweeps (local_port): where "SLPWII EXIT" goes
# --net: the PC player's Dolphin plays from PC_PORT, the stand-in Dolphin
# (--baseline) from STANDIN_PORT; the relay (net_relay.py) holds the address
# each one's opponent is given, and passes the packets on, late. The
# Dolphins then reach the fake matchmaking server at 127.0.0.1: it answers
# from where they play, and this PC's address there is the relay's.
PC_PORT, STANDIN_PORT = 51500, 51600
FAILURES = re.compile(r'CRASH:|PANIC|rb MISS|rb LOST|rb VERIFY|rb WALK [1-9]|PAD SENT AHEAD IS WRONG|PPC HUNG|GAME STUCK')  # (a desync is reported, the games go on)
STALL_S = 120  # no Wii log line for this long once its games started: stuck
STALL_FIRST_S = 300  # before them (matchmaking, a first boot)
GAME_EVENTS = re.compile(r'rolls back|waits for inputs|game over')  # a game started or ended
IDLE_MARGIN_S = 120  # past a game's length (--lras-s) with no game starting or ending: stuck in the menus


def pc_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(('192.0.2.1', 9))
        return s.getsockname()[0]
    finally:
        s.close()


def make_plan(rounds, shift=12):
    """Every character on both sides, every legal stage: round i has the Wii
    on ORDER[i], Dolphin on ORDER[i + 12], stage i % 6."""
    stages = list(STAGES)
    plan = []
    for i in range(rounds):
        plan.append((ORDER[i % len(ORDER)], ORDER[(i + shift + i // len(ORDER)) % len(ORDER)], stages[i % len(stages)]))
    return plan


def run(*cmd, **kw):
    res = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)
    if res.returncode:
        sys.exit(f'{cmd[0]} failed:\n{res.stdout[-2000:]}{res.stderr[-2000:]}')
    return res.stdout


def autoplay_codes(run_dir, plan, args):
    """Both sides' autoplay codes (the Wii's as a C2 code and as Dolphin's
    text, which the --baseline stand-in plays)."""
    # (--menu-latency: the Wii's bot stays on the main menu, pressing the d-pad: autoplay.c's seed 0xEC)
    for side, col, seed in (('wii', 0, 0xEC if args.menu_latency else args.seed * 2 + 1),
                            ('dolphin', 1, args.seed * 2 + 2)):
        spec = {'rounds': [[r[col], r[2]] for r in plan], 'lras_s': args.lras_s, 'seed': seed,
                'timer_s': args.timer_s, 'timer_rounds': args.timer_rounds, 'idle_rounds': args.idle_rounds,
                'over_limit': (args.over_limit_p1, args.over_limit_p2),
                'no_text_fix_rounds': args.no_text_fix_rounds, 'person': 0 if args.plain_bot else 1}
        (run_dir / f'plan-{side}.json').write_text(json.dumps(spec))
        run(sys.executable, HERE / 'build_autoplay.py', '--plan', run_dir / f'plan-{side}.json',
            '--out', run_dir / f'autoplay-{side}')


def build(run_dir, plan, args, ip):
    """Autoplay codes for both sides, then the Wii's test build."""
    autoplay_codes(run_dir, plan, args)
    inc = NIN / 'kernel/slippi_online/so_test_config.inc'
    boot = NIN / 'kernel/gecko/g_online_boot.bin'
    dol = NIN / 'loader/loader.dol'
    original_inc, original_boot = inc.read_bytes(), boot.read_bytes()
    update_inc = NIN / 'loader/source/update_test.inc'
    original_update_inc = update_inc.read_bytes()
    if args.update_first:
        # (the loader's updater takes a key made for this run, and with it the test's launch argument: update.c)
        import sign_update
        key = run_dir / 'update-test-key.pem'
        sign_update.new_key(key)
        update_inc.write_text('/* TEST BUILD (tools/sweep/run_sweep.py --update-first) */\n#define UPD_TEST_KEY '
                              + ', '.join(f'0x{b:02X}' for b in sign_update.public_point(key)) + '\n', newline='\n')
    original_dol = dol.read_bytes() if dol.exists() else None
    settings = (f'mm_host={ip}\\nmm_port=43113\\nselftest_code={DOLPHIN_CODE}\\n'
                f'log_host={ip}:51450\\nstatus_beacon=1\\ntest_control=1\\nlocal_port={WII_PORT}\\n')
    if args.rollback is not None:
        # (rollback itself is the kernel's default since 1.4.0: a run has it as a player does)
        settings += f'test_lag_frames={args.rollback}\\ntest_spike_ms={args.spike}\\n'
    else:
        settings += 'rollback=0\\n'  # (and no engine in the codes, below: the game waits for inputs)
    if args.net:
        settings += 'lan_ip=127.0.0.2\\n'  # the address its opponent is given: the relay's
    for kv in args.kernel:
        # the kernel reads decimal only, and keeps its default for anything else
        if not re.fullmatch(r'\w+=(-?\d+|[\d.:]+|[^\d].*)', kv):
            sys.exit(f'--kernel {kv}: write numbers in decimal')
    settings += ''.join(f'{kv}\\n' for kv in args.kernel)
    try:
        inc.write_text(f'/* TEST BUILD (tools/sweep/run_sweep.py) */\n"{settings}"\n')
        run(sys.executable, ROOT / 'tools/build_online_boot.py',
            # (the deep check's build has neither the probe nor the stall profiler: its engine is the player's, and full)
            *(['--rollback-trace'] if args.trace else ['--latency'] if args.latency
              else [] if args.verify_deep or args.walk or args.hwtest else ['--stall']),
            '--test-exit', *([] if args.human else ['--autoplay', run_dir / 'autoplay-wii.c2.bin']),
            *(['--no-delay-buttons'] if args.no_delay_buttons else []),
            # (the probe, the autoplay and the player's engine fit beside each other without the ping display)
            *(['--no-ping-display', '--no-text-args'] if args.latency and not args.human else []),
            # (a person's game with the probe keeps the delay buttons and the text fix, and drops only the ping display)
            *(['--no-ping-display'] if args.latency and args.human else []),
            # (nor does the bot's verify build fit with it, since the engine's direct_track)
            *(['--no-ping-display'] if (args.verify or args.verify_all) and not args.latency and not args.human else []),
            # (nor the bot's build with the engine a player has: without the delay buttons, so not built for size)
            *(['--no-ping-display'] if args.no_delay_buttons and not (args.verify or args.verify_all or args.latency or args.human) else []),
            # (the autoplay and the delay buttons fit beside each other only with the engine built for size)
            *(['--small-engine'] if not args.human and not args.no_delay_buttons else []),
            # (the engine is in every build since 1.4.0: a run without --rollback is the one that leaves it out)
            *([] if args.rollback is not None else ['--no-rollback']),
            *(['--rollback-verify'] if args.verify or args.verify_all else []),
            *(['--rollback-verify-all'] if args.verify_all else []),
            *(['--rollback-verify-deep', '--no-ping-display', '--no-text-args'] if args.verify_deep else []),
            *(['--rollback-hwtest', '--no-ping-display', '--no-text-args'] if args.hwtest else []),
            # (with the probe: the walk that fits beside it, memory pieces only, a crash report its word)
            *(['--rollback-walk', '--no-ping-display', '--no-text-args'] if args.walk else []))
        print('building the Wii test build (a minute or two)...')
        run('sh', ROOT / 'tools/build.sh', cwd=ROOT)
        shutil.copy(NIN / 'loader/loader.dol', run_dir / 'loader-sweep.dol')
    finally:
        inc.write_bytes(original_inc)
        boot.write_bytes(original_boot)
        update_inc.write_bytes(original_update_inc)
        if original_dol is not None:
            dol.write_bytes(original_dol)  # the normal build stays the default one
    print(f'test build: {run_dir / "loader-sweep.dol"} (repo files restored)')


def send_exit(wii_ip):
    """Asks the Wii to go back to the Homebrew Channel (test_control)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    for _ in range(3):
        s.sendto(b'SLPWII EXIT', (wii_ip, WII_PORT))
        time.sleep(0.2)
    s.close()


def wait_hbc(wii_ip, timeout_s=120):
    """Seconds until the Homebrew Channel answers (its wiiload port), or None."""
    start = time.time()
    while time.time() - start < timeout_s:
        try:
            with socket.create_connection((wii_ip, 4299), timeout=2):
                return round(time.time() - start)
        except OSError:
            time.sleep(2)
    return None


def wii_lines(run_dir):
    """The Wii's log lines so far (the watcher's output)."""
    path = run_dir / 'watch.log'
    if not path.exists():
        return []
    return [l[6:] for l in path.read_text(encoding='utf-8', errors='replace').splitlines() if l.startswith('wii | ')]


def loader_cfg(run_dir, replays=False, codes=0, screen=0, diag_log=False):
    """The Wii's loader settings for a run (a NIN_CFG, CommonConfig.h): Slippi Replays on (it saves each game
    on the other drive), SlippWii's switches (bits 16-19: Slippi Dolphin's optional codes; 20: the log on the
    SD card) and the Screen setting (1 default, 2 16:9 to 73:60, 3 widescreen)."""
    import wii_boot
    cfg = bytearray(wii_boot.BASE_CFG.read_bytes())
    flags = (int.from_bytes(cfg[8:12], 'big') | (1 << 14 if replays else 0) | (codes & 0xF) << 16 | (1 << 20 if diag_log else 0)
             | (1 << 21 if codes & 16 else 0))  # (16: No Music, the bit after the log's)
    cfg[8:12] = flags.to_bytes(4, 'big')
    if screen:
        cfg[288 + 6 * 4:288 + 7 * 4] = screen.to_bytes(4, 'big')  # MeleeCodeOptions[MELEE_CODES_SCREEN_OPTION_ID]
    path = run_dir / 'nincfg-run.bin'
    path.write_bytes(cfg)
    return path


def start(name, cmd, run_dir, **kw):
    log = open(run_dir / f'{name}.log', 'w', encoding='utf-8')
    return subprocess.Popen([str(c) for c in cmd], stdout=log, stderr=subprocess.STDOUT, **kw), log


def processes(name):
    """[(pid, executable path, command line)] for processes with this image name."""
    ps = (f"Get-CimInstance Win32_Process -Filter \"name='{name}'\" | "
          "Select-Object ProcessId,ExecutablePath,CommandLine | ConvertTo-Json -Compress")
    out = subprocess.run(['powershell', '-NoProfile', '-Command', ps], capture_output=True, text=True).stdout.strip()
    if not out:
        return []
    items = json.loads(out)
    items = items if isinstance(items, list) else [items]
    return [(i['ProcessId'], i.get('ExecutablePath') or '', i.get('CommandLine') or '') for i in items]


LOCK = OUT / 'sweep.lock'


def other_sweeps():
    """[(pid, command line)] of other run_sweep.py processes on this PC (not this one or its launcher)."""
    mine = {os.getpid(), os.getppid()}
    return [(pid, cmd) for pid, _, cmd in processes('python.exe') if 'run_sweep.py' in cmd and pid not in mine]


def take_lock():
    """One sweep at a time. Two share the Wii, the build's swapped files and each other's helpers: a run
    that ends (or gives up on a silent log) sends the Wii its exit and kills every run's listener and
    relay. A batch stopped by its shell alone leaves its run_sweep.py alive to do that to the next one:
    2026-11-10, three runs each ended the next one's game 2 minutes in, and it read as a frozen console."""
    others = other_sweeps()
    if others:
        sys.exit('another sweep is running (' + ', '.join(f'process {pid}' for pid, _ in others)
                 + '): let it end, or stop it with python tools/sweep/stop.py')
    LOCK.parent.mkdir(parents=True, exist_ok=True)
    LOCK.write_text(f'{os.getpid()} {time.strftime("%Y-%m-%d %H:%M:%S")}')


def kill_leftovers():
    """The sweep's own processes only (never the player's Slippi Dolphin)."""
    subprocess.run(['taskkill', '/F', '/IM', 'slippi_fake_mm.exe'], capture_output=True)
    for pid, path, _ in processes('Slippi Dolphin.exe'):
        if str(OUT).lower() in path.lower():
            subprocess.run(['taskkill', '/F', '/PID', str(pid)], capture_output=True)
    # the log listener, the replay mirror and the relay, from an earlier run
    for pid, _, cmd in processes('python.exe'):
        if 'wii_mirror.py' in cmd or 'net_relay.py' in cmd or ('online_selftest.py' in cmd and ' watch' in cmd):
            subprocess.run(['taskkill', '/F', '/PID', str(pid)], capture_output=True)


# character ID in replays (external) -> the CSS icon whose name CHARS has
EXTERNAL_TO_ICON = {22: 0, 8: 1, 7: 2, 5: 3, 12: 4, 17: 5, 1: 6, 0: 7, 25: 8, 20: 9, 2: 10, 11: 11, 14: 12, 4: 13,
                    16: 14, 18: 15, 19: 15, 6: 16, 21: 17, 24: 18, 13: 19, 15: 20, 10: 21, 3: 22, 9: 23, 23: 24}


def actual(slp):
    """(Wii character, Dolphin character, stage) a replay shows; the Wii is
    port 2 (Dolphin hosts on the fake server). Empty strings if unreadable."""
    try:
        import slp_compare
        start, _ = slp_compare.index(slp)
        names = []
        for port in (1, 0):
            ext = start[0x65 + 0x24 * port]
            name = CHARS.get(EXTERNAL_TO_ICON.get(ext, -1), f'#{ext}')
            names.append('Sheik' if ext == 19 else name)
        stage_id = slp_compare.field(start, 0x13, 2, '>H')
        stage = next((n for n, sid in STAGES.values() if sid == stage_id), f'stage {stage_id}')
        return names[0], names[1], stage
    except Exception:
        return '', '', ''


def dolphin_felt(run_dir):
    """What Dolphin's player felt of the Wii, from Slippi's online log
    (dolphin_copy.py turns it on): its halts at the rollback limit, before the
    Wii's first input (the Wii still loading) and in play, and its time sync's
    halts and speed changes. Against another PC on a LAN, all of them are 0."""
    path = run_dir / 'dolphin-online.log'
    if not path.exists():
        return 'Dolphin\'s log: none'
    text = path.read_text(encoding='utf-8', errors='replace')
    limit = [int(l) for l in re.findall(r'Halting for one frame due to rollback limit \(frame: -?\d+ \| latest: (-?\d+)',
                                         text)]
    start = sum(1 for l in limit if l <= 0)
    sync = [(int(f), int(n)) for f, n in re.findall(r'Halting on frame (\d+) due to time sync\. Offset: -?\d+ us\. '
                                                    r'Frames: (\d+)', text)]
    speeds = [float(s) for s in re.findall(r'New speed: ([\d.]+)%', text)]
    return (f'Dolphin halted {len(limit)} frames at the rollback limit: {start} before the Wii\'s first input '
            f'(the Wii loading), {len(limit) - start} in play. Its time sync halted '
            f'{sum(n for f, n in sync if f <= 120)} frames at the games\' starts and {sum(n for f, n in sync if f > 120)} '
            f'in play' + (f'; its speed {min(speeds):.2f}-{max(speeds):.2f}%' if speeds else '') + '.')


def net_args(net):
    """--net DELAY[,JITTER[,LOSS]] -> net_relay.py's options"""
    parts = (net.split(',') + ['0', '0'])[:3]
    return ['--delay', parts[0], '--jitter', parts[1], '--loss', parts[2]]


def start_relay(run_dir, a_adv, a_real, b_adv, b_real, net):
    relay = start('relay', [sys.executable, '-u', HERE / 'net_relay.py', a_adv, a_real, b_adv, b_real,
                            *net_args(net), '--trace', run_dir / 'relay-frames.txt'], run_dir)
    time.sleep(0.5)
    unthrottle.unthrottle(relay[0].pid)  # its delays are timed: it must not wait for the CPU
    return relay


def rollbacks_felt(run_dir):
    """The rollbacks the PC player saw (its own replays), and its opponent;
    with --net, how much later the opponent's input for each frame left than
    the PC player's (net_relay.py's 10 s lines, weighted by their frames)."""
    import slp_rollbacks
    out = []
    relay = run_dir / 'relay.log'
    if relay.exists():
        lines = re.findall(r"B's inputs later than A's: median (-?[\d.]+) ms, 10% (-?[\d.]+), 90% (-?[\d.]+) \((\d+)",
                           relay.read_text(encoding='utf-8', errors='replace'))
        n = sum(int(f) for *_, f in lines)
        if n:
            avg = [sum(float(v[k]) * int(v[3]) for v in lines) / n for k in range(3)]
            out.append(f"Its opponent's input for each frame left later than the PC player's by {avg[0]:.1f} ms "
                       f'(the middle of each 10 s; 10% of frames under {avg[1]:.1f}, 90% under {avg[2]:.1f}; '
                       f'{n} frames).')
    for who, folder in (('The PC player', 'dolphin-replays'), ('Its opponent', 'wii-replays')):
        paths = sorted((run_dir / folder).glob('*.slp'))
        if paths:
            out.append(f"{who}'s rollbacks: " + slp_rollbacks.describe(slp_rollbacks.total(paths)) + '.')
    return out


def run_baseline(args, plan, run_dir, ip):
    """--baseline: the same games and the same connection with a second Slippi
    Dolphin in the Wii's place, playing the Wii's autoplay: what the PC player
    feels against a PC, the reference a Wii run with the same --net is
    measured against. The stand-in's replays go where the Wii's would."""
    autoplay_codes(run_dir, plan, args)
    kill_leftovers()
    pc = dolphin_copy.make('dolphin-sweep', DOLPHIN_CODE, WII_CODE, run_dir / 'autoplay-dolphin.gecko.txt',
                           '127.0.0.1', lan_ip=ip, port=PC_PORT)
    standin = dolphin_copy.make('dolphin-sweep-b', WII_CODE, DOLPHIN_CODE, run_dir / 'autoplay-wii.gecko.txt',
                                '127.0.0.1', lan_ip='127.0.0.2', port=STANDIN_PORT)
    procs = [start('fake_mm', [TOOLS / 'slippi_fake_mm.exe', '--port', 43113, '--external-ip', '10.0.0.1'], run_dir)]
    procs.append(start_relay(run_dir, f'{ip}:{PC_PORT}', f'127.0.0.1:{PC_PORT}', f'127.0.0.2:{STANDIN_PORT}',
                             f'{ip}:{STANDIN_PORT}', args.net))
    for name, d in (('dolphin', pc), ('standin', standin)):
        procs.append(start(name, [d / 'Slippi Dolphin.exe', '-e', ISO], run_dir))
        time.sleep(3)  # the PC player's Dolphin first: it hosts, as against the Wii
        unthrottle.unthrottle(procs[-1][0].pid)
    cpu = unthrottle.CpuLoad()
    print(f'baseline {run_dir.name}: {len(plan)} rounds, Slippi Dolphin against Slippi Dolphin, net {args.net}')
    started, failure, done = time.time(), None, 0
    try:
        while True:
            games = sorted((pc / 'replays').rglob('*.slp'), key=lambda f: f.stat().st_mtime)
            if len(games) != done:
                done = len(games)
                print(f'{time.strftime("%H:%M:%S")} {done}/{len(plan)} games', flush=True)
            if done >= len(plan) and time.time() - games[-1].stat().st_mtime > 15:
                break
            if time.time() - started > min(args.timeout_min * 60, (len(plan) + 1) * (args.lras_s + IDLE_MARGIN_S)):
                failure = 'timed out (stuck in the menus?)'
                break
            time.sleep(10)
            cpu.sample()
    except KeyboardInterrupt:
        failure = 'stopped'
    finally:
        for p, log in procs:
            if p.poll() is None:
                p.terminate()
            log.close()
        kill_leftovers()
    for d, name in ((pc, 'dolphin-online.log'), (standin, 'opponent-online.log')):
        if (d / 'User/Logs/dolphin.log').exists():
            shutil.copy(d / 'User/Logs/dolphin.log', run_dir / name)
    pairs = []
    for d, folder, prefix in ((pc, 'dolphin-replays', ''), (standin, 'wii-replays', 'wii-')):
        (run_dir / folder).mkdir(exist_ok=True)
        files = sorted((d / 'replays').rglob('*.slp'), key=lambda f: f.stat().st_mtime)
        for f in files:
            shutil.copy(f, run_dir / folder / (prefix + f.name))
        pairs.append([prefix + f.name for f in files])
    # paired in order, as the mirror pairs the Wii's games with Dolphin's
    (run_dir / 'mirror.log').write_text(''.join(f"mirror: {b} vs Dolphin's {a}:\n" for a, b in zip(*pairs)),
                                        encoding='utf-8')
    if failure:
        print(f'FAILED: {failure}')
        (run_dir / 'failure.txt').write_text(failure + '\n', encoding='utf-8')
    report(run_dir, plan, 0)
    with open(run_dir / 'report.md', 'a', encoding='utf-8') as out:
        out.write("\n## Baseline\n\nThe Wii's column is a second Slippi Dolphin playing the Wii's part, through "
                  f"the same relay (net {args.net}). The PC's CPU during the run: {cpu.summary()}.\n")
    sys.exit(1 if failure else 0)


def report(run_dir, plan, started):
    """Pairs each game's comparison (mirror log) with the plan and the Wii's
    per-game stats (its log), and writes report.md + report.json."""
    mirror = (run_dir / 'mirror.log').read_text(encoding='utf-8', errors='replace') if (run_dir / 'mirror.log').exists() else ''
    results = []
    for block in mirror.split('mirror: ')[1:]:
        m = re.match(r'(wii-\S+\.slp) vs Dolphin\'s (\S+):', block)
        if not m:
            continue
        # compared again now, with the current comparer, from the saved pair
        pair = [run_dir / 'wii-replays' / m.group(1), run_dir / 'dolphin-replays' / m.group(2)]
        if all(p.exists() for p in pair):
            block = subprocess.run([sys.executable, ROOT / 'tools/slp_compare.py', *pair, '--context', '2'],
                                   capture_output=True, text=True).stdout
        verdict = 'MATCH' if 'MATCH:' in block else 'DESYNC' if 'DESYNC' in block else 'unknown'
        first = re.search(r'first differ at frame (-?\d+)', block)
        results.append({'wii': m.group(1), 'dolphin': m.group(2), 'verdict': verdict,
                        'frame': int(first.group(1)) if first else None,
                        'scratch': len(re.findall(r'scratch values differ|soft difference:', block)),
                        'detail': [l.strip(' |') for l in block.splitlines()[1:] if 'frame ' in l][:8]})
    wii_log = ''
    logs = sorted((ROOT / 'build/online-selftest').glob('2026*/wii.log'), key=lambda p: p.stat().st_mtime)
    if started and logs and logs[-1].stat().st_mtime >= started:
        shutil.copy(logs[-1], run_dir / 'wii.log')
    if (run_dir / 'wii.log').exists():
        wii_log = (run_dir / 'wii.log').read_text(encoding='utf-8', errors='replace')
    games = re.findall(r'game over: (\d+) frames in (\d+) ms.*?waited (\d+) frames.*?ping (\d+) us', wii_log)
    # game-loop stalls in a game, by length; 6+ retraces overflow Melee's 5-poll
    # pad queue (inputs dropped: the desync risk); shorter ones are mostly the
    # Wii waiting for Dolphin's input (lockstep)
    lengths = [int(n) for n in re.findall(r'STALL vi \d+ logic (?:[2-9]\d{2,}|\d{4,}): ended after (\d+)', wii_log)]
    stalls = f'{sum(n >= 6 for n in lengths)} long (6+ frames), {sum(n < 6 for n in lengths)} short'
    lines = [f'# Sweep {run_dir.name}', '',
             f'{len(results)} games compared: {sum(r["verdict"] == "MATCH" for r in results)} match, '
             f'{sum(r["verdict"] == "DESYNC" for r in results)} desync. Wii game-loop stalls in games: {stalls}.', '',
             'The PC player: ' + dolphin_felt(run_dir), '', *rollbacks_felt(run_dir), '',
             '| # | Wii | Dolphin | Stage | Result | Wii frames | waited | ping |', '|---|---|---|---|---|---|---|---|']
    for i, r in enumerate(results):
        p = plan[i % len(plan)]
        g = games[i] if i < len(games) else ('', '', '', '')
        res = r['verdict'] + (f' at frame {r["frame"]}' if r['frame'] is not None else '')
        if r.get('scratch'):
            res += f' ({r["scratch"]} scratch)'
        wii, dol, stage = actual(run_dir / 'wii-replays' / r['wii'])
        planned = (CHARS[p[0]], CHARS[p[1]], STAGES[p[2]][0])
        cells = [a if a == b or not a else f'{a} (plan: {b})' for a, b in zip((wii, dol, stage), planned)]
        lines.append(f'| {i + 1} | {cells[0]} | {cells[1]} | {cells[2]} | {res} | {g[0]} | {g[2]} | '
                     f'{int(g[3]) / 1000 if g[3] else ""} ms |')
    for i, r in enumerate(results):
        if r['verdict'] == 'DESYNC':
            lines += ['', f'## Game {i + 1}: {r["wii"]} vs {r["dolphin"]}'] + [f'- {d}' for d in r['detail']]
    (run_dir / 'report.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    (run_dir / 'report.json').write_text(json.dumps({'plan': plan, 'results': results, 'games': games,
                                                     'stalls': stalls}, indent=1))
    print('\n'.join(lines))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--rounds', type=int, default=25)
    ap.add_argument('--seed', type=int, default=0, help='random inputs: another seed, other games')
    ap.add_argument('--shift', type=int, default=12, help='Dolphin plays ORDER[i + shift]: other matchups')
    ap.add_argument('--repeat-round', type=int, default=0, metavar='N',
                    help="every round is the plan's round N (1 on): its characters and stage, as a set between two "
                         'players is (the engine goes by the last game against the same opponent)')
    ap.add_argument('--lras-s', type=int, default=40, help='seconds of play before each game is quit (0: never)')
    ap.add_argument('--timer-s', type=int, default=0, help='games reach the timer in this many seconds')
    ap.add_argument('--timer-rounds', type=int, default=0, help='bit mask: the rounds --timer-s is for (1: the first)')
    ap.add_argument('--idle-rounds', type=int, default=0, help='bit mask: rounds with no inputs (nobody is KO\'d)')
    ap.add_argument('--over-limit-p1', type=int, default=0, help='bit mask: rounds where port 1 exceeds the ledge grab limit')
    ap.add_argument('--over-limit-p2', type=int, default=0, help='bit mask: the same for port 2')
    ap.add_argument('--no-text-fix-rounds', type=int, default=0, help='bit mask: rounds without tools/text_args/ (a console crashes)')
    ap.add_argument('--replays', action='store_true', help='the Wii saves its replays too, on the drive the game is not on')
    ap.add_argument('--codes', type=lambda v: int(v, 0), default=0, metavar='BITS',
                    help="the loader's switches for Slippi Dolphin's optional codes: 1 flash red on a failed L-cancel, "
                         "2 no screen shake, 4 center the 2P HUD, 8 freeze Final Destination's background (15: all); 16 No Music")
    ap.add_argument('--screen', type=int, default=0, choices=[0, 1, 2, 3],
                    help="the loader's Screen setting: 1 default, 2 16:9 to 73:60, 3 widescreen (0: as the base settings)")
    ap.add_argument('--diag-log', action='store_true',
                    help="the loader's Diagnostic Log switch: the kernel's log goes to the SD card (/slippwii/logs/)")
    ap.add_argument('--wii-ip', default=os.environ.get('SLIPPWII_WII_IP'),
                    required=not os.environ.get('SLIPPWII_WII_IP'),
                    help="the Wii's address (default: $SLIPPWII_WII_IP)")
    ap.add_argument('--timeout-min', type=int, default=120)
    ap.add_argument('--skip-build', help='reuse this run directory\'s build')
    ap.add_argument('--no-boot', action='store_true', help='the Wii already runs this build: do not send it')
    ap.add_argument('--build-to', help='only build, into this directory (for --skip-build later)')
    ap.add_argument('--report-only', help='write this run directory\'s report again (current comparer)')
    ap.add_argument('--update-first', action='store_true',
                    help='the loader runs "Check for Updates" by itself before it starts Melee (a server here says '
                         'there is none): do the games still play, their kernel on the network the loader used?')
    ap.add_argument('--rollback', type=int, metavar='LAG',
                    help='roll back (tools/rollback), the opponent shown LAG frames late (0-6)')
    ap.add_argument('--spike', type=int, default=0, metavar='MS',
                    help='with --rollback: every 5 s, the opponent held back MS (a lag spike: the Wii '
                         'waits at the rollback limit, then rolls back the most it can)')
    ap.add_argument('--net', metavar='DELAY[,JITTER[,LOSS]]',
                    help='play through net_relay.py: a one-way delay and jitter (ms) and loss (percent), both '
                         'ways, as on an internet connection')
    ap.add_argument('--stage', type=int, choices=list(STAGES), help='every round on this stage (its icon, as in STAGES)')
    ap.add_argument('--human', action='store_true', help='a person plays the Wii, against the bot (see above)')
    ap.add_argument('--latency', action='store_true',
                    help="the input latency probe (tools/latency_probe) in place of the stall profiler: it follows a "
                         "person's presses (--human) or the bot's; tools/latency/analyze.py reads its LAT lines")
    ap.add_argument('--menu-latency', type=int, default=0, metavar='SECONDS',
                    help='with --latency: no games; the bot presses the d-pad on the main menu for this long '
                         "(offline timing: with --kernel test_lag_reduction=1, Melee's own)")
    ap.add_argument('--release-engine', action='store_true',
                    help="with --human: the player's engine (-O2, no record), as in the beta, not the traced one")
    ap.add_argument('--no-delay-buttons', action='store_true', help='no delay buttons on the CSS (blind.py)')
    ap.add_argument('--plain-bot', action='store_true', help="the bot's stick is still: about a third of a person's input changes")
    ap.add_argument('--capture', action='store_true', help='record the video output of the Wii (decode_marker.py reads it)')
    ap.add_argument('--no-mirror', action='store_true',
                    help="timing runs: no replay stream from the Wii, so no replays compared (what a player's Wii does)")
    ap.add_argument('--kernel', action='append', default=[], metavar='KEY=VALUE',
                    help="a line for the Wii's config.ini (test builds; repeatable)")
    ap.add_argument('--baseline', action='store_true',
                    help="with --net: a second Slippi Dolphin plays the Wii's part (what the PC player feels "
                         'against a PC, to measure a Wii run with the same --net against)')
    ap.add_argument('--trace', action='store_true',
                    help='record what the player sees and hears, in place of the stall profiler (tools/rollback/trace_view.py)')
    ap.add_argument('--verify', action='store_true',
                    help='with --rollback: the engine checks its captures and loads, sampled (slower)')
    ap.add_argument('--verify-all', action='store_true',
                    help='with --rollback: every load, all pages (a slow game, for hunting a bad restore)')
    ap.add_argument('--hwtest', action='store_true',
                    help="with --rollback and --no-delay-buttons: at each game's start the engine tests whether memory gives "
                         "a read what was written just before it ('rb HW' in the Wii's log; rollback.c, RB_HWTEST)")
    ap.add_argument('--walk', action='store_true',
                    help="with --rollback: the engine walks the game's allocators' free lists after each load, before "
                         "each capture and at each frame's end ('rb WALK' in the Wii's log when one is first wrong)")
    ap.add_argument('--verify-deep', action='store_true',
                    help="with --rollback and --no-delay-buttons: a player's engine that keeps a sum of each page it saves "
                         "and checks what each deep load puts back ('rb VD' lines in the Wii's log)")
    args = ap.parse_args()
    if (args.verify_deep or args.walk) and (not args.no_delay_buttons or (args.latency and not args.walk) or args.trace
                                            or args.human or args.verify or args.verify_all or (args.verify_deep and args.walk)):
        sys.exit('--verify-deep, --walk: with --no-delay-buttons, and not with --trace, --human or another check '
                 '(--walk alone with --latency)')
    if args.human:  # games last as long as the person takes, the record on; no mirror, as on a player's Wii
        args.lras_s, args.no_mirror = 0, True
        args.trace = not (args.release_engine or args.latency)  # (the record's engine is -Os and traced: slower than a player's)
    if args.report_only:
        run_dir = Path(args.report_only)
        report(run_dir, [tuple(p) for p in json.loads((run_dir / 'plan.json').read_text())], 0)
        return
    take_lock()
    ip = pc_ip()
    plan = make_plan(max(args.rounds, args.repeat_round), args.shift)
    if args.repeat_round:
        plan = [plan[args.repeat_round - 1]] * args.rounds
    if args.stage is not None:
        plan = [(w, d, args.stage) for w, d, _ in plan]
    run_dir = Path(args.skip_build or args.build_to or OUT / 'runs' / time.strftime('%Y%m%d-%H%M%S'))
    run_dir.mkdir(parents=True, exist_ok=True)
    (run_dir / 'plan.json').write_text(json.dumps(plan))
    if args.baseline:
        if not args.net:
            sys.exit('--baseline needs --net')
        return run_baseline(args, plan, run_dir, ip)
    if not args.skip_build:
        build(run_dir, plan, args, ip)
    if args.build_to:
        return
    kill_leftovers()
    idle = unthrottle.CpuLoad()
    time.sleep(1)
    idle.sample()
    if idle.samples[0] > 80:
        print(f'warning: the PC is at {idle.samples[0]:.0f}% CPU before the run starts: '
              'Dolphin may not keep 60 frames a second')
    dolphin = dolphin_copy.make('dolphin-sweep', DOLPHIN_CODE, WII_CODE, run_dir / 'autoplay-dolphin.gecko.txt',
                                '127.0.0.1' if args.net else ip,
                                lan_ip=ip if args.net else None, port=PC_PORT if args.net else None)
    replays = dolphin / 'replays'
    # --net: one external address for both, so each is given the other's LAN
    # address, which the relay holds (the Wii's is 127.0.0.2: lan_ip in build)
    procs = [start('fake_mm', [TOOLS / 'slippi_fake_mm.exe', '--port', 43113,
                               *(['--external-ip', '10.0.0.1'] if args.net else [])], run_dir)]
    if args.net:
        procs.append(start_relay(run_dir, f'{ip}:{PC_PORT}', f'127.0.0.1:{PC_PORT}', f'127.0.0.2:{WII_PORT}',
                                 f'{args.wii_ip}:{WII_PORT}', args.net))
    if not args.no_mirror:
        procs.append(start('mirror', [sys.executable, '-u', ROOT / 'tools/wii_mirror.py', '--wii-ip', args.wii_ip,
                                      '--out', run_dir / 'wii-replays', '--dolphin', replays], run_dir))
    boot = [] if args.no_boot else ['--boot', '--dol', run_dir / 'loader-sweep.dol']
    update_server = None
    if args.update_first and not args.no_boot:
        # "Check for Updates" by itself before the games: a server here says the installed version is the
        # latest, the loader asks it over the network and goes on to start Melee, whose kernel then uses
        # the network the loader used. The Wii's report of the check comes back to the server.
        import sign_update
        import update_wii_test
        update_server = update_wii_test.Server(ip)
        update_server.files['uptodate.txt'] = sign_update.manifest(
            run_dir / 'update-test-key.pem', '0.0.1', b'no zip: nothing is downloaded',
            url='https://github.com/2gifts/SlippWii/releases/download/v0.0.1/none.zip')
        boot += ['--first-arg', f'slippwii-update-then-boot={update_server.url}/plain/uptodate.txt;{update_wii_test.FOLDER}']
    if (args.replays or args.codes or args.screen or args.diag_log) and not args.no_boot:
        boot += ['--cfg', loader_cfg(run_dir, args.replays, args.codes, args.screen, args.diag_log)]
    procs.append(start('watch', [sys.executable, '-u', ROOT / 'tools/online_selftest.py', 'watch', *boot,
                                 '--wii-ip', args.wii_ip, '--timeout', args.timeout_min * 60], run_dir))
    if args.capture:
        procs.append(start('capture', [decode_marker.FFMPEG, '-y', '-f', 'dshow', '-rtbufsize', '200M', '-vcodec', 'mjpeg',
                                       '-video_size', '720x480', '-framerate', '25', '-i', 'video=AV TO USB2.0',
                                       '-c', 'copy', run_dir / 'capture.mkv'], run_dir))
    procs.append(start('dolphin', [dolphin / 'Slippi Dolphin.exe', '-e', ISO], run_dir))
    # Windows 11 throttles a covered window's timer: Dolphin then runs at 47.5
    # frames a second and the Wii waits for it (lockstep). A busy PC starves it
    # the same way, so it also runs at high priority, and the PC's load goes in
    # the report: a Dolphin under 60 frames a second makes the Wii wait at the
    # rollback limit, which looks like a slow Wii.
    time.sleep(3)
    unthrottle.unthrottle(procs[-1][0].pid)
    cpu = unthrottle.CpuLoad()
    started = time.time()
    print(f'sweep {run_dir.name}: {len(plan)} rounds. Open the Homebrew Channel on the Wii '
          f'({args.wii_ip}); the test build is sent to it.')
    failure = None
    try:
        done, seen, last_line_at = 0, 0, time.time()
        events, last_event_at = 0, time.time()
        booted, booted_at = False, 0
        while time.time() - started < args.timeout_min * 60:
            if args.no_mirror:
                n = sum('game over:' in line for line in wii_lines(run_dir))
            else:
                text = (run_dir / 'mirror.log').read_text(encoding='utf-8', errors='replace')
                n = len(re.findall(r'^mirror: wii-\S+ vs Dolphin', text, re.M))
            if n != done:
                done = n
                print(f'{time.strftime("%H:%M:%S")} {done}/{len(plan)} games compared', flush=True)
            if done >= len(plan) or (run_dir / 'stop').exists():
                time.sleep(5)
                # A trace build sends the last game's record after it, at about 12 ms a line: going
                # back to the Homebrew Channel before it's all here loses that game's end (seen: a
                # hand test's last 1100 frames, with a player's mark in them)
                quiet_from, n_lines, give_up = time.time(), len(wii_lines(run_dir)), time.time() + 90
                while args.trace and time.time() < give_up and time.time() - quiet_from < 5:
                    time.sleep(1)
                    n = len(wii_lines(run_dir))
                    if n != n_lines:
                        n_lines, quiet_from = n, time.time()
                break
            lines = wii_lines(run_dir)
            if len(lines) != seen:
                seen, last_line_at = len(lines), time.time()
            bad = next((l for l in lines if FAILURES.search(l)), None)
            if bad:
                failure = f'the Wii reported: {bad}'
                time.sleep(5)  # the rest of its report
                break
            n = sum(1 for l in lines if GAME_EVENTS.search(l))
            if n != events:
                events, last_event_at = n, time.time()
            limit = STALL_S if events else STALL_FIRST_S
            if not booted and 'sent the build' in (run_dir / 'watch.log').read_text(errors='replace'):
                booted, last_line_at = True, time.time()  # the silence counts from the boot
                booted_at = time.time()
            if args.menu_latency and booted and time.time() - booted_at > args.menu_latency + 30:
                break  # (30 s: Melee's start-up, to its main menu)
            if args.human:  # quiet in the menus is no sign of trouble: a person is choosing
                time.sleep(10)
                cpu.sample()
                continue
            if (seen or booted) and time.time() - last_line_at > limit:
                failure = f'the Wii\'s log stopped for {limit} s (stuck?)'
                break
            idle = args.lras_s + IDLE_MARGIN_S
            if events and time.time() - last_event_at > idle:
                failure = f'no game started or ended for {idle} s (stuck in the menus?)'
                break
            time.sleep(10)
            cpu.sample()
    except KeyboardInterrupt:
        failure = 'stopped'
    finally:
        send_exit(args.wii_ip)  # back to the Homebrew Channel for the next run
        for p, log in procs:
            if p.poll() is None:
                p.terminate()
            log.close()
        kill_leftovers()
        if (dolphin / 'User/Logs/dolphin.log').exists():
            shutil.copy(dolphin / 'User/Logs/dolphin.log', run_dir / 'dolphin-online.log')
        if args.no_mirror:  # (the mirror keeps them otherwise) the PC player's rollbacks come from them
            (run_dir / 'dolphin-replays').mkdir(exist_ok=True)
            for f in replays.rglob('*.slp'):
                shutil.copy(f, run_dir / 'dolphin-replays' / f.name)
    if update_server is not None:
        told = update_server.reports[-1] if update_server.reports else None
        line = ('the update check before the games: ' + (', '.join(f'{k} {v}' for k, v in told.items()) if told
                else 'NO REPORT from the Wii') + f'; the server saw {len(update_server.seen)} requests')
        print(line)
        (run_dir / 'update-first.txt').write_text(line + '\n' + '\n'.join(update_server.seen) + '\n', encoding='utf-8')
        if not told or told.get('result') != '1':
            failure = failure or 'the update check before the games did not end as "up to date"'
    if failure:
        print(f'FAILED: {failure}')
        (run_dir / 'failure.txt').write_text(failure + '\n', encoding='utf-8')
    back = wait_hbc(args.wii_ip)
    returned = (f'the Wii was back at the Homebrew Channel {back} s after the exit request' if back is not None
                else 'the Wii did NOT come back to the Homebrew Channel (2 minutes)')
    print(returned)
    report(run_dir, plan, started)
    # the Wii's own account: failures, each game's netcode and checksums, the rollback engine's numbers
    keep = re.compile(r'CRASH|PANIC|DESYNC|rb (MISS|VERIFY)|game (over|checksums|rollback)|PERF rb|rolls back|waits for inputs|exit:')
    with open(run_dir / 'report.md', 'a', encoding='utf-8') as out:
        out.write('\n## The Wii\n\n' + (f'**FAILED: {failure}**\n\n' if failure else '') + returned + '\n\n'
                  f'The PC\'s CPU during the run: {cpu.summary()}.\n\n```\n')
        out.writelines(line + '\n' for line in wii_lines(run_dir) if keep.search(line))
        out.write('```\n')
    sys.exit(1 if failure else 0)


if __name__ == '__main__':
    main()
