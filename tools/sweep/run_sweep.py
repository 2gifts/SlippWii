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
TOOLS = ROOT.parent / 'slippi-3ds/native-3ds/build/slippi-tools'
ISO = Path.home() / 'Downloads/SSB/SSBMv102.iso'
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / 'tools'))
import dolphin_copy  # noqa: E402
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


def build(run_dir, plan, args, ip):
    """Autoplay codes for both sides, then the Wii's test build."""
    for side, col, seed in (('wii', 0, args.seed * 2 + 1), ('dolphin', 1, args.seed * 2 + 2)):
        spec = {'rounds': [[r[col], r[2]] for r in plan], 'lras_s': args.lras_s, 'seed': seed}
        (run_dir / f'plan-{side}.json').write_text(json.dumps(spec))
        run(sys.executable, HERE / 'build_autoplay.py', '--plan', run_dir / f'plan-{side}.json',
            '--out', run_dir / f'autoplay-{side}')
    inc = NIN / 'kernel/slippi_online/so_test_config.inc'
    boot = NIN / 'kernel/gecko/g_online_boot.bin'
    dol = NIN / 'loader/loader.dol'
    original_inc, original_boot = inc.read_bytes(), boot.read_bytes()
    original_dol = dol.read_bytes() if dol.exists() else None
    settings = (f'mm_host={ip}\\nmm_port=43113\\nselftest_code={DOLPHIN_CODE}\\n'
                f'log_host={ip}:51450\\nstatus_beacon=1\\n')
    try:
        inc.write_text(f'/* TEST BUILD (tools/sweep/run_sweep.py) */\n"{settings}"\n')
        run(sys.executable, ROOT / 'tools/build_online_boot.py', '--stall', '--autoplay',
            run_dir / 'autoplay-wii.c2.bin')
        print('building the Wii test build (a minute or two)...')
        run('sh', ROOT / 'tools/build.sh', cwd=ROOT)
        shutil.copy(NIN / 'loader/loader.dol', run_dir / 'loader-sweep.dol')
    finally:
        inc.write_bytes(original_inc)
        boot.write_bytes(original_boot)
        if original_dol is not None:
            dol.write_bytes(original_dol)  # the normal build stays the default one
    print(f'test build: {run_dir / "loader-sweep.dol"} (repo files restored)')


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


def kill_leftovers():
    """The sweep's own processes only (never the player's Slippi Dolphin)."""
    subprocess.run(['taskkill', '/F', '/IM', 'slippi_fake_mm.exe'], capture_output=True)
    for pid, path, _ in processes('Slippi Dolphin.exe'):
        if str(OUT).lower() in path.lower():
            subprocess.run(['taskkill', '/F', '/PID', str(pid)], capture_output=True)
    # the log listener and the replay mirror, from an earlier run
    for pid, _, cmd in processes('python.exe'):
        if 'wii_mirror.py' in cmd or ('online_selftest.py' in cmd and ' watch' in cmd):
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


def report(run_dir, plan, started):
    """Pairs each game's comparison (mirror log) with the plan and the Wii's
    per-game stats (its log), and writes report.md + report.json."""
    mirror = (run_dir / 'mirror.log').read_text(encoding='utf-8', errors='replace')
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
    ap.add_argument('--lras-s', type=int, default=40, help='seconds of play before each game is quit')
    ap.add_argument('--wii-ip', default=os.environ.get('SLIPPWII_WII_IP'),
                    required=not os.environ.get('SLIPPWII_WII_IP'),
                    help="the Wii's address (default: $SLIPPWII_WII_IP)")
    ap.add_argument('--timeout-min', type=int, default=120)
    ap.add_argument('--skip-build', help='reuse this run directory\'s build')
    ap.add_argument('--no-boot', action='store_true', help='the Wii already runs this build: do not send it')
    ap.add_argument('--report-only', help='write this run directory\'s report again (current comparer)')
    args = ap.parse_args()
    if args.report_only:
        run_dir = Path(args.report_only)
        report(run_dir, [tuple(p) for p in json.loads((run_dir / 'plan.json').read_text())], 0)
        return
    ip = pc_ip()
    plan = make_plan(args.rounds, args.shift)
    run_dir = Path(args.skip_build) if args.skip_build else OUT / 'runs' / time.strftime('%Y%m%d-%H%M%S')
    run_dir.mkdir(parents=True, exist_ok=True)
    (run_dir / 'plan.json').write_text(json.dumps(plan))
    if not args.skip_build:
        build(run_dir, plan, args, ip)
    kill_leftovers()
    time.sleep(1)
    dolphin = dolphin_copy.make('dolphin-sweep', DOLPHIN_CODE, WII_CODE, run_dir / 'autoplay-dolphin.gecko.txt', ip)
    replays = dolphin / 'replays'
    procs = [start('fake_mm', [TOOLS / 'slippi_fake_mm.exe', '--port', 43113], run_dir)]
    procs.append(start('mirror', [sys.executable, '-u', ROOT / 'tools/wii_mirror.py', '--wii-ip', args.wii_ip,
                                  '--out', run_dir / 'wii-replays', '--dolphin', replays], run_dir))
    boot = [] if args.no_boot else ['--boot', '--dol', run_dir / 'loader-sweep.dol']
    procs.append(start('watch', [sys.executable, '-u', ROOT / 'tools/online_selftest.py', 'watch', *boot,
                                 '--wii-ip', args.wii_ip, '--timeout', args.timeout_min * 60], run_dir))
    procs.append(start('dolphin', [dolphin / 'Slippi Dolphin.exe', '-e', ISO], run_dir))
    # Windows 11 throttles a covered window's timer: Dolphin then runs at 47.5
    # frames a second and the Wii waits for it (lockstep).
    time.sleep(3)
    unthrottle.unthrottle(procs[-1][0].pid)
    started = time.time()
    print(f'sweep {run_dir.name}: {len(plan)} rounds. Open the Homebrew Channel on the Wii '
          f'({args.wii_ip}); the test build is sent to it.')
    try:
        done = 0
        while time.time() - started < args.timeout_min * 60:
            text = (run_dir / 'mirror.log').read_text(encoding='utf-8', errors='replace')
            n = len(re.findall(r'^mirror: wii-\S+ vs Dolphin', text, re.M))
            if n != done:
                done = n
                print(f'{time.strftime("%H:%M:%S")} {done}/{len(plan)} games compared', flush=True)
            if done >= len(plan):
                time.sleep(5)
                break
            time.sleep(10)
    except KeyboardInterrupt:
        pass
    finally:
        for p, log in procs:
            if p.poll() is None:
                p.terminate()
            log.close()
        kill_leftovers()
    report(run_dir, plan, started)


if __name__ == '__main__':
    main()
