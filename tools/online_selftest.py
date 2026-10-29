"""PC side of the Slippi Online self-test on a real Wii (M2).

The Wii runs Slippi Nintendont with the slippi_online kernel module and
selftest=1: after Melee has booted it searches Slippi Direct for PEER#002,
then plays a synthetic lockstep match at 59.94 Hz and checks every pad it
receives. This PC provides the other side, entirely locally:

- the fake matchmaking server and the fake Dolphin peer from the 3DS fork's
  test tools (slippi_fake_mm.exe, slippi_fake_peer.exe); they speak the same
  wire protocol as mm.slippi.gg and Slippi Dolphin, with dummy accounts;
- a UDP listener for the Wii's log lines (log_host=) and its status in
  Slippi's LAN broadcast (status_beacon=1).

Nothing here talks to Slippi's servers, and the Wii's real user.json is never
read or written: the test points the Wii at a dummy account file.

    python tools/online_selftest.py setup --sd E:     # write the Wii's test config to the SD card
    python tools/online_selftest.py run               # then boot the Wii and wait for PASS/FAIL
    python tools/online_selftest.py pc                # no Wii: the client code built for Windows
                                                      # (Nintendont/kernel/slippi_online/tests/pc)

Playing by hand (M4): the Wii runs Slippi's real online menus and codeset;
you pick Online > Direct on the Wii, enter PEER#002, and play a PC opponent
(so_pc_opponent.exe: Fox, walking and jumping) through the fake server:

    python tools/online_selftest.py setup --play --sd E: --iso "E:/games/Melee.iso"
    python tools/online_selftest.py play --boot       # then open the Homebrew Channel

Playing on Slippi's real servers with your own account (the only mode that
does): the account file is copied to the SD card as-is and never printed; the
Wii sends its playKey only to mm.slippi.gg. watch boots the Wii and records
its log (it never sees the key) with no fake server or opponent:

    python tools/online_selftest.py setup --real --sd E: --account path/to/user.json
    python tools/online_selftest.py watch --boot

Options: --frames N (default 3600 = 1 min), --ack-delay-us N, --drop PCT (the
Wii drops that share of its pad packets), --delay N, --tools DIR,
--second-is-host (the Wii becomes the decider, port 1).
Windows may ask to allow the fake server and peer through the firewall:
allow them on private networks, or the Wii cannot reach them.
"""
import argparse
import json
import os
import select
import shutil
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
DEFAULT_TOOLS = ROOT.parent / 'slippi-3ds/native-3ds/build/slippi-tools'
OUT = ROOT / 'build/online-selftest'

LOG_PORT, BROADCAST_PORT, MM_PORT = 51450, 20582, 43113
CONFIG_MARKER = '# written by slippi-wii/tools/online_selftest.py'

WII_USER = {'uid': 'local-test-uid-wii', 'playKey': 'dummy-play-key-wii', 'connectCode': 'WIIT#001',
            'displayName': 'Test Wii', 'latestVersion': '3.6.4'}
PEER_USER = {'uid': 'local-test-uid-peer', 'playKey': 'dummy-play-key-peer', 'connectCode': 'PEER#002',
             'displayName': 'Fake Dolphin', 'latestVersion': '3.6.4'}


def pc_ip():
    """This PC's LAN address (no packet is sent)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(('192.0.2.1', 9))
        return s.getsockname()[0]
    finally:
        s.close()


def wii_config(ip, frames, ack_delay_us, drop, delay, boot_quiet_s=None, log_to_pc=True, stage_pause_s=3,
               step_pause_ms=0):
    lines = [
        CONFIG_MARKER,
        '# Self-test against the local fake server; dummy account. Delete this',
        '# file (or put your own settings back) to play normally.',
        'user_path=/slippwii/test-user.json',
        f'mm_host={ip}',
        f'mm_port={MM_PORT}',
        f'delay={delay}',
        'selftest=1',
        f"selftest_code={PEER_USER['connectCode']}",
        f'selftest_frames={frames}',
        f'log_host={ip}:{LOG_PORT}',
        'status_beacon=1',
        f'ack_delay_us={ack_delay_us}',
        f'test_drop_pct={drop}',
        'selftest_async_probe=1',
        f'selftest_stage_pause_s={stage_pause_s}',
        f'diag_step_pause_ms={step_pause_ms}',
    ]
    if boot_quiet_s is not None:
        lines.append(f'boot_quiet_s={boot_quiet_s}')
    if not log_to_pc:
        lines = [l for l in lines if not l.startswith('log_host=')]
    return '\n'.join(lines) + '\n'


def real_config(ip, delay):
    """config.ini for Slippi's real servers (mm.slippi.gg, the default host)."""
    return '\n'.join([
        CONFIG_MARKER + ': real Slippi servers',
        '# Your account: /slippwii/user.json. The log below goes only to this PC.',
        'user_path=/slippwii/user.json',
        f'delay={delay}',
        'selftest=0',
        f'log_host={ip}:{LOG_PORT}',
        'status_beacon=1',
    ]) + '\n'


def peer_config(ip, delay):
    return '\n'.join([
        CONFIG_MARKER + ': local fake server only',
        f"opponent={WII_USER['connectCode']}",
        f'delay={delay}',
        f'mm_host={ip}',
        f'mm_port={MM_PORT}',
    ]) + '\n'


def setup_real(args, folder):
    """Copies the player's Slippi account to the SD card without reading it
    aloud: only its connect code is shown."""
    if not args.account:
        sys.exit('setup --real needs --account (your Slippi user.json)')
    src = Path(args.account)
    try:
        account = json.loads(src.read_text(encoding='utf-8'))
    except (OSError, ValueError) as e:
        sys.exit(f'{src} is not a readable user.json ({type(e).__name__})')
    if not all(account.get(k) for k in ('uid', 'playKey', 'connectCode')):
        sys.exit(f'{src} is not a Slippi user.json (uid, playKey, connectCode)')
    dest = folder / 'user.json'
    if dest.exists() and dest.read_bytes() != src.read_bytes():
        backup = folder / f'user.json.bak-{time.strftime("%Y%m%d-%H%M%S")}'
        dest.rename(backup)
        print(f'kept the previous user.json as {backup.name}')
    shutil.copyfile(src, dest)
    print(f"copied the account {account['connectCode']} to {dest}")
    return real_config(args.pc_ip or pc_ip(), args.delay)


def setup(args):
    sd = Path(args.sd.rstrip('\\/') + '/')
    if args.play:
        # Slippi's codeset and menu files, from the player's own disc.
        build = [sys.executable, str(HERE / 'slippi_files.py'), '--out', str(sd)]
        build += ['--iso', args.iso] if args.iso else ['--files', args.files] if args.files else []
        if len(build) == 4:
            sys.exit('setup --play needs --iso (or --files)')
        subprocess.run(build, check=True)
    if not sd.exists():
        sys.exit(f'{sd} not found')
    folder = sd / 'slippwii'
    folder.mkdir(exist_ok=True)
    config = folder / 'config.ini'
    if config.exists() and CONFIG_MARKER not in config.read_text(encoding='utf-8', errors='replace'):
        backup = folder / f'config.ini.bak-{time.strftime("%Y%m%d-%H%M%S")}'
        config.rename(backup)
        print(f'kept your config.ini as {backup.name}')
    if args.real:
        config.write_text(setup_real(args, folder), encoding='utf-8', newline='\n')
        print(f'wrote {config}: Slippi\'s real servers, log to this PC')
        return
    ip = args.pc_ip or pc_ip()
    text = wii_config(ip, args.frames, args.ack_delay_us, args.drop, args.delay, args.boot_quiet,
                      stage_pause_s=args.stage_pause, step_pause_ms=args.step_pause_ms)
    if args.play:
        # The game drives the client (the self-test would keep the console codeset).
        text = text.replace('selftest=1', 'selftest=0')
    config.write_text(text, encoding='utf-8', newline='\n')
    (folder / 'test-user.json').write_text(json.dumps(WII_USER, indent=2) + '\n', encoding='utf-8', newline='\n')
    print(f'wrote {config} (PC {ip}) and the dummy test-user.json; your user.json is untouched')


def plan_text(args):
    """The test plan the Wii asks for: config.ini lines for this run."""
    lines = [
        f'selftest_frames={args.frames}',
        f'delay={args.delay}',
        f'ack_delay_us={args.ack_delay_us}',
        f'test_drop_pct={args.drop}',
        f'selftest_stage_pause_s={args.stage_pause}',
        f'diag_step_pause_ms={args.step_pause_ms}',
        f'selftest_memory_walk={1 if args.memory_walk else 0}',
        f'selftest_async_probe={1 if args.async_probe else 0}',
        f'diag_main_priority={args.main_priority}',
    ]
    return '\n'.join(lines) + '\n'


def answer_probe(sock, data, addr, plan=''):
    """The Wii's requests: "SLPWII PROBE n" -> "SLPWII PROBE-ACK n";
    "SLPWII PLAN?" -> "SLPWII PLAN" and the plan's lines."""
    if data == b'SLPWII PLAN?':
        sock.sendto(b'SLPWII PLAN\n' + plan.encode(), addr)
        return True
    if not data.startswith(b'SLPWII PROBE ') or data.startswith(b'SLPWII PROBE-ACK'):
        return False
    sock.sendto(b'SLPWII PROBE-ACK ' + data[13:], addr)
    return True


def probe_responder(stop, plan):
    """Answers probes and the plan request on LOG_PORT until `stop` is set
    (PC mode)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(('127.0.0.1', LOG_PORT))
    s.settimeout(0.2)
    while not stop.is_set():
        try:
            data, addr = s.recvfrom(4096)
        except (socket.timeout, ConnectionResetError):
            continue
        answer_probe(s, data, addr, plan)
    s.close()


class Run:
    def __init__(self, args):
        self.args = args
        self.ip = args.pc_ip or pc_ip()
        self.tools = Path(args.tools)
        stamp = time.strftime('%Y%m%d-%H%M%S')
        self.out = OUT / stamp
        self.out.mkdir(parents=True, exist_ok=True)
        self.wii_log = open(self.out / 'wii.log', 'w', encoding='utf-8', errors='replace')
        self.procs = []
        self.peer_started = False
        self.result = None
        self.beacon = None
        self.plan = plan_text(args)
        self.hb_log = open(self.out / 'heartbeat.log', 'w', encoding='utf-8')
        self.beacon_log = open(self.out / 'beacon.log', 'w', encoding='utf-8')
        self.async_seen = 0
        self.last_hb, self.last_hb_at, self.hb_lost = None, 0.0, False
        self.hb_where, self.hb_printed = None, 0.0

    def start(self, name, cmd):
        log = open(self.out / f'{name}.log', 'w', encoding='utf-8')
        self.procs.append((name, subprocess.Popen([str(c) for c in cmd], stdout=log, stderr=subprocess.STDOUT), log))

    def start_peer(self):
        peer = self.out / 'peer'
        peer.mkdir(exist_ok=True)
        (peer / 'user.json').write_text(json.dumps(PEER_USER, indent=2), encoding='utf-8')
        (peer / 'config.ini').write_text(peer_config(self.ip, self.args.delay), encoding='utf-8')
        self.start('peer', [self.tools / 'slippi_fake_peer.exe', '--dir', peer, '--frames', self.args.frames,
                            '--linger', 5])
        self.peer_started = True
        print('  (fake Dolphin peer started)')

    def on_wii_line(self, line):
        print(f'wii | {line}')
        self.wii_log.write(line + '\n')
        self.wii_log.flush()
        if 'searching for' in line and not self.peer_started:
            self.start_peer()
        if line.endswith('selftest: PASS'):
            self.result = 'PASS'
        elif line.endswith('selftest: FAIL'):
            self.result = 'FAIL'

    def on_heartbeat(self, text):
        """Prints a heartbeat when a thread moved to another checkpoint."""
        self.hb_log.write(text + '\n')
        self.hb_log.flush()
        self.last_hb, self.last_hb_at = text, time.monotonic()
        if self.hb_lost:
            print(f'!! heartbeat back: {text}')
            self.hb_lost = False
        fields = [f for f in text.split()[2:] if not f.startswith(('t=', 'k='))]
        where = ' '.join(f.split('#')[0] for f in fields)
        if where != self.hb_where or time.monotonic() - self.hb_printed > 10:
            self.hb_where, self.hb_printed = where, time.monotonic()
            print(f'hb  | {text[10:]}')

    def check_heartbeat(self):
        if self.last_hb and not self.hb_lost and time.monotonic() - self.last_hb_at > 3:
            self.hb_lost = True
            print(f'!! Wii heartbeat lost; the last one: {self.last_hb}')
            self.wii_log.write(f'!! heartbeat lost; last: {self.last_hb}\n')

    def on_datagram(self, data, addr, port, sock):
        if port == LOG_PORT and data.startswith(b'SLPWII LOGS\n'):
            for line in data[12:].decode('utf-8', 'replace').splitlines():
                if line:
                    self.on_wii_line(line)
        elif port == LOG_PORT and data.startswith(b'SLPWII HB '):
            self.on_heartbeat(data.decode('utf-8', 'replace'))
        elif port == LOG_PORT and answer_probe(sock, data, addr, self.plan):
            if data == b'SLPWII PLAN?':
                print('      (sent the test plan)')
            pass
        elif port == LOG_PORT and data.startswith(b'SLPWII ASYNC '):
            self.async_seen += 1
            if self.async_seen % 10 == 1:
                print(f'      (experiment datagram: {data.decode("utf-8", "replace")})')
        elif port == BROADCAST_PORT and data.startswith(b'SLIP_READY') and len(data) >= 48:
            # Every broadcast counts: when they stop, the Wii's kernel stopped.
            beacon = data[16:48].split(b'\0')[0].decode('utf-8', 'replace')
            stamp = time.strftime('%H:%M:%S')
            self.beacon_log.write(f'{stamp} {addr[0]} {beacon}\n')
            self.beacon_log.flush()
            self.beacon, self.last_hb_at = beacon, time.monotonic()
            self.last_hb = beacon
            print(f'{stamp} wii | {beacon}')

    def run(self):
        for exe in ('slippi_fake_mm.exe', 'slippi_fake_peer.exe'):
            if not isinstance(self, Watch) and not (self.tools / exe).exists():
                sys.exit(f'{self.tools / exe} not found: build the 3DS fork tools or pass --tools')
        socks = {}
        for port in (LOG_PORT, BROADCAST_PORT):
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            if port == LOG_PORT and hasattr(socket, 'SO_EXCLUSIVEADDRUSE'):
                # Only one listener may get the Wii's log and answer its probes.
                s.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            else:
                s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                s.bind(('0.0.0.0', port))
            except OSError as e:
                sys.exit(f'UDP {port} is in use ({e}): is another online_selftest.py still running?')
            s.settimeout(0.2)
            socks[port] = s
        self.start('fake_mm', [self.tools / 'slippi_fake_mm.exe', '--port', MM_PORT] +
                   (['--second-is-host'] if self.args.second_is_host else []))
        print(f'PC {self.ip}: {"" if isinstance(self, Watch) else f"fake matchmaking on UDP {MM_PORT}, "}'
              f'Wii log on UDP {LOG_PORT}; logs in {self.out}')
        if self.args.boot:
            # Retry until the Homebrew Channel is on screen and answers.
            import wii_boot
            print(f'Waiting for the Homebrew Channel at {self.args.wii_ip} (open it on the Wii)...')
            while True:
                try:
                    wii_boot.boot(self.args.wii_ip, self.args.dol, wii_boot.BASE_CFG, timeout=3)
                    print(f'{time.strftime("%H:%M:%S")} sent the build to the Homebrew Channel; Melee is starting.')
                    break
                except OSError:
                    time.sleep(2)
            # Anything queued while waiting came from an earlier run.
            for s in socks.values():
                s.setblocking(False)
                try:
                    while True:
                        s.recvfrom(4096)
                except OSError:
                    pass
                s.settimeout(0.2)
        else:
            print('Boot the Wii into Melee now.')
        print('The self-test starts once the boot quiet time is over (config.ini boot_quiet_s, default 60 s).')
        deadline = time.monotonic() + self.args.timeout
        try:
            # After PASS/FAIL the Wii runs the async-send experiment; keep
            # recording until it reports "x done" or its broadcasts stop.
            while time.monotonic() < deadline and not (
                    self.beacon == 'x done' or self.beacon and self.beacon.endswith(' x done') or
                    (self.result and self.hb_lost and time.monotonic() - self.last_hb_at > 20) or
                    (getattr(self, 'opponent', None) and self.opponent.poll() is not None)):
                # Wait on both sockets at once: the Wii's probes must be
                # answered at once (waiting on them in turn added ~200 ms).
                ready, _, _ = select.select(list(socks.values()), [], [], 0.2)
                for port, s in socks.items():
                    if s not in ready:
                        continue
                    try:
                        data, addr = s.recvfrom(4096)
                    except (socket.timeout, ConnectionResetError):
                        continue
                    self.on_datagram(data, addr, port, s)
                self.check_heartbeat()
            time.sleep(3)	# the peer's summary
        except KeyboardInterrupt:
            pass
        finally:
            for name, p, log in self.procs:
                if p.poll() is None:
                    p.terminate()
                log.close()
            self.wii_log.close()
            self.hb_log.close()
            self.beacon_log.close()
        opponent_log = self.out / 'opponent.log'
        if opponent_log.exists():
            print('--- PC opponent ---')
            for line in opponent_log.read_text(encoding='utf-8', errors='replace').splitlines():
                if line.startswith('opponent:'):
                    print(line)
        peer_log = self.out / 'peer.log'
        if peer_log.exists():
            print('--- fake peer ---')
            for line in peer_log.read_text(encoding='utf-8', errors='replace').splitlines()[-8:]:
                print(f'peer | {line}')
        print(f'RESULT: {self.result or "no result"}; last broadcast: {self.beacon}; '
              f'experiment datagrams received: {self.async_seen}; logs in {self.out}')
        return 0 if self.result == 'PASS' else 1


class Watch(Run):
    """Slippi's real servers (setup --real): boots the Wii and records its log
    and diagnostics; nothing else runs here. Ends with Ctrl+C or --timeout."""

    def start(self, name, cmd):
        pass  # no fake server

    def start_peer(self):
        pass

    def on_wii_line(self, line):
        print(f'wii | {line}')
        self.wii_log.write(line + '\n')
        self.wii_log.flush()

    def run(self):
        print('Watching the Wii on Slippi\'s real servers: play on it, Ctrl+C here when done.')
        return super().run()


class Play(Run):
    """The Wii plays a person's game against so_pc_opponent through the fake
    server; the run ends when the opponent leaves (after --games games)."""

    def __init__(self, args):
        super().__init__(args)
        self.opponent = None

    def start_peer(self):
        pass  # the opponent starts at once and waits in the queue

    def on_wii_line(self, line):
        print(f'wii | {line}')
        self.wii_log.write(line + '\n')
        self.wii_log.flush()

    def run(self):
        exe = Path(self.args.pc_exe).with_name('so_pc_opponent.exe')
        if not exe.exists():
            sys.exit(f'{exe} not found: run Nintendont/kernel/slippi_online/tests/pc/build.sh')
        peer = self.out / 'peer'
        peer.mkdir(exist_ok=True)
        (peer / 'user.json').write_text(json.dumps(PEER_USER, indent=2), encoding='utf-8')
        (peer / 'config.ini').write_text('\n'.join([
            CONFIG_MARKER + ': local fake server only',
            f'mm_host={self.ip}', f'mm_port={MM_PORT}', f'delay={self.args.delay}',
            f"selftest_code={WII_USER['connectCode']}",
        ]) + '\n', encoding='utf-8')
        self.start('opponent', [exe, peer, self.args.games, self.args.timeout])
        self.opponent = self.procs[-1][1]
        print(f"On the Wii: Online > Direct, enter {PEER_USER['connectCode']}, pick a character, press Start.")
        return super().run()


def run_pc(args):
    """The same test without a Wii: the client code built for Windows."""
    exe = Path(args.pc_exe)
    if args.game:
        exe = exe.with_name('so_pc_game.exe')
    if not exe.exists():
        sys.exit(f'{exe} not found: run Nintendont/kernel/slippi_online/tests/pc/build.sh')
    out = OUT / ('pc-' + time.strftime('%Y%m%d-%H%M%S'))
    (out / 'wii').mkdir(parents=True)
    (out / 'peer').mkdir()
    ip = '127.0.0.1'
    config = wii_config(ip, args.frames, args.ack_delay_us, args.drop, args.delay, boot_quiet_s=0)
    if args.game:
        # fake Melee drives the EXI device instead of the self-test
        config = config.replace('selftest=1', 'selftest=0')
    (out / 'wii/config.ini').write_text(config, encoding='utf-8')
    stop = threading.Event()
    responder = threading.Thread(target=probe_responder, args=(stop, plan_text(args)), daemon=True)
    responder.start()
    (out / 'wii/test-user.json').write_text(json.dumps(WII_USER), encoding='utf-8')
    staged = ROOT / 'build/sd-staging/slippwii'
    if args.game and staged.exists():
        # The builder's codeset and menu files (tools/slippi_files.py)
        shutil.copy(staged / 'netplay.gct', out / 'wii')
        shutil.copytree(staged / 'files', out / 'wii/files')
    (out / 'peer/user.json').write_text(json.dumps(PEER_USER), encoding='utf-8')
    (out / 'peer/config.ini').write_text(peer_config(ip, args.delay), encoding='utf-8')
    tools = Path(args.tools)
    logs = {name: open(out / f'{name}.log', 'w', encoding='utf-8') for name in ('fake_mm', 'peer', 'wii')}
    mm = subprocess.Popen([str(tools / 'slippi_fake_mm.exe'), '--port', str(MM_PORT)] +
                          (['--second-is-host'] if args.second_is_host else []),
                          stdout=logs['fake_mm'], stderr=subprocess.STDOUT)
    time.sleep(0.5)
    peer = subprocess.Popen([str(tools / 'slippi_fake_peer.exe'), '--dir', str(out / 'peer'), '--frames',
                             str(args.frames), '--linger', '3'], stdout=logs['peer'], stderr=subprocess.STDOUT)
    wii_cmd = [str(exe), str(out / 'wii')] + ([str(args.frames)] if args.game else []) + [str(args.timeout)]
    wii = subprocess.Popen(wii_cmd, stdout=logs['wii'],
                           stderr=subprocess.STDOUT)
    codes = {}
    for name, p in (('wii', wii), ('peer', peer)):
        try:
            codes[name] = p.wait(args.timeout)
        except subprocess.TimeoutExpired:
            p.kill()
            codes[name] = 'timeout'
    mm.terminate()
    stop.set()
    for f in logs.values():
        f.close()
    for name in ('wii', 'peer'):
        lines = (out / f'{name}.log').read_text(encoding='utf-8', errors='replace').splitlines()
        for line in [l for l in lines if 'selftest:' in l or 'peer:' in l or l.startswith('game:')][-8:]:
            print(f'{name:4} | {line}')
    ok = codes == {'wii': 0, 'peer': 0}
    print(f'RESULT: {"PASS" if ok else "FAIL"} {codes}; logs in {out}')
    return 0 if ok else 1


def main():
    # The Wii's log can carry any byte; the Windows console must not stop the run.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(errors='replace')
        except AttributeError:
            pass
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('mode', choices=('setup', 'run', 'pc', 'play', 'watch'))
    ap.add_argument('--real', action='store_true',
                    help="setup: Slippi's real servers with your account (--account)")
    ap.add_argument('--account', help='setup --real: your Slippi user.json (copied, never printed)')
    ap.add_argument('--play', action='store_true', help='setup: for play mode (Slippi codeset and menu files)')
    ap.add_argument('--iso', help='setup --play: your NTSC 1.02 Melee disc image')
    ap.add_argument('--files', help='setup --play: or a folder of its extracted files')
    ap.add_argument('--games', type=int, default=5, help='play: games before the PC opponent leaves')
    ap.add_argument('--sd', help='SD card drive (setup)')
    ap.add_argument('--pc-ip', help="this PC's LAN address (default: detected)")
    ap.add_argument('--frames', type=int, default=3600)
    ap.add_argument('--ack-delay-us', type=int, default=0)
    ap.add_argument('--drop', type=int, default=0)
    ap.add_argument('--delay', type=int, default=2)
    ap.add_argument('--boot-quiet', type=int, help='seconds before the Wii sends anything (setup; default 60)')
    ap.add_argument('--stage-pause', type=int, default=3, help='seconds between the probe stages (setup)')
    ap.add_argument('--step-pause-ms', type=int, default=0, help='hold each matchmaking start-up step')
    ap.add_argument('--memory-walk', action='store_true', help='write and verify the free heap before matchmaking')
    ap.add_argument('--async-probe', action='store_true', help='replay the old async sends after the test')
    ap.add_argument('--main-priority', type=lambda v: int(v, 0), default=0,
                    help='also probe with the kernel main thread at this priority (e.g. 0x30)')
    ap.add_argument('--boot', action='store_true', help='run: send the build to the Wii (Homebrew Channel) first')
    ap.add_argument('--wii-ip', default=os.environ.get('SLIPPWII_WII_IP'),
                    help="the Wii's address (default: $SLIPPWII_WII_IP)")
    ap.add_argument('--dol', default=str(ROOT / 'Nintendont/loader/loader.dol'))
    ap.add_argument('--tools', default=str(DEFAULT_TOOLS))
    ap.add_argument('--timeout', type=int, default=900, help='seconds to wait for a result')
    ap.add_argument('--game', action='store_true',
                    help='pc: run fake Melee through the EXI device (so_pc_game.exe) instead of the self-test')
    ap.add_argument('--second-is-host', action='store_true', help='the Wii is the decider (port 1)')
    ap.add_argument('--pc-exe', default=str(Path(os.environ.get('TEMP', '/tmp')) / 'slippi_online_pc/so_pc_selftest.exe'))
    args = ap.parse_args()
    if args.boot and not args.wii_ip:
        sys.exit('--boot needs --wii-ip (or set SLIPPWII_WII_IP)')
    if args.mode == 'setup':
        if not args.sd:
            sys.exit('setup needs --sd')
        setup(args)
        return 0
    if args.mode == 'pc':
        return run_pc(args)
    if args.mode == 'play':
        return Play(args).run()
    if args.mode == 'watch':
        return Watch(args).run()
    return Run(args).run()


if __name__ == '__main__':
    sys.exit(main())
