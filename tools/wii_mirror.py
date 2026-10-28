"""Records the Wii's own replays: Slippi Nintendont streams each game's
replay data on TCP 51441 (the protocol Slippi Launcher's console mirror
uses); this saves one .slp per game, to compare with Slippi Dolphin's own
replay of the same game (tools/slp_compare.py).

    python tools/wii_mirror.py [--wii-ip WII] [--out DIR] [--dolphin DIR]

Runs until Ctrl+C, reconnecting whenever the Wii is not there yet. With
--dolphin (Slippi Dolphin's replay folder, e.g. a share on the PC that played
the Wii), each game is compared with Dolphin's replay of it as soon as that
appears.
"""
import argparse
import os
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PORT = 51441
HANDSHAKE, REPLAY, KEEPALIVE = 1, 2, 3


def ubj_key(name):
    return b'i' + bytes([len(name)]) + name.encode()


def handshake(cursor=0, token=0):
    """The client handshake exactly as slippi-js encodes it: the Wii reads
    its fields at fixed offsets (SlippiCommunication.c)."""
    body = (b'{' + ubj_key('type') + b'U' + bytes([HANDSHAKE]) +
            ubj_key('payload') + b'{' +
            ubj_key('cursor') + b'[$U#i\x08' + struct.pack('>Q', cursor) +
            ubj_key('clientToken') + b'[$U#i\x04' + struct.pack('>I', token) +
            ubj_key('isRealtime') + b'F' + b'}}')
    assert body[8] == HANDSHAKE and body[33:41] == struct.pack('>Q', cursor)
    return struct.pack('>I', len(body)) + body


class Reader:
    """Just enough UBJSON for the Wii's messages."""

    def __init__(self, data):
        self.d, self.i = data, 0

    def byte(self):
        self.i += 1
        return self.d[self.i - 1]

    def length(self):
        t = chr(self.byte())
        return self.number(t)

    def number(self, t):
        fmt = {'i': '>b', 'U': '>B', 'I': '>h', 'l': '>i', 'L': '>q', 'd': '>f', 'D': '>d'}[t]
        n = struct.calcsize(fmt)
        v = struct.unpack(fmt, self.d[self.i:self.i + n])[0]
        self.i += n
        return v

    def value(self, t=None):
        t = t or chr(self.byte())
        if t == '{':
            out = {}
            while chr(self.d[self.i]) != '}':
                n = self.length()
                key = self.d[self.i:self.i + n].decode()
                self.i += n
                out[key] = self.value()
            self.i += 1
            return out
        if t == '[':
            if chr(self.d[self.i]) == '$':
                self.i += 1
                et = chr(self.byte())
                assert chr(self.byte()) == '#'
                n = self.length()
                if et == 'U':
                    v = self.d[self.i:self.i + n]
                    self.i += n
                    return bytes(v)
                return [self.value(et) for _ in range(n)]
            out = []
            while chr(self.d[self.i]) != ']':
                out.append(self.value())
            self.i += 1
            return out
        if t == 'S':
            n = self.length()
            v = self.d[self.i:self.i + n].decode('utf-8', 'replace')
            self.i += n
            return v
        if t == 'T':
            return True
        if t == 'F':
            return False
        if t == 'Z':
            return None
        return self.number(t)


def slp_file(raw):
    """A .slp around raw replay data (metadata left empty)."""
    return (b'{U\x03raw[$U#l' + struct.pack('>I', len(raw)) + raw +
            b'U\x08metadata{}}')


class Games:
    """Splits the stream into games: each starts with Event Payloads (0x35)
    and ends with Game End (0x39)."""

    def __init__(self, out, on_saved=None):
        self.out, self.buf, self.sizes, self.game = out, b'', None, None
        self.saved, self.on_saved = 0, on_saved

    def feed(self, data):
        self.buf += data
        while self.buf:
            c = self.buf[0]
            if c == 0x35:
                if len(self.buf) < 2 or len(self.buf) < 1 + self.buf[1]:
                    return
                n = self.buf[1]
                self.sizes = {0x35: n}
                for k in range((n - 1) // 3):
                    o = 2 + 3 * k
                    self.sizes[self.buf[o]] = struct.unpack('>H', self.buf[o + 1:o + 3])[0]
                self.game = bytearray()
            if not self.sizes or c not in self.sizes:
                print(f'mirror: unknown command 0x{c:02x}; skipping to the next game')
                i = self.buf.find(b'\x35', 1)
                self.buf = self.buf[i:] if i > 0 else b''
                self.sizes = None
                continue
            n = 1 + self.sizes[c]
            if len(self.buf) < n:
                return
            if self.game is not None:
                self.game += self.buf[:n]
            self.buf = self.buf[n:]
            if c == 0x39 and self.game is not None:
                self.save()

    def save(self):
        name = time.strftime('wii-%Y%m%dT%H%M%S.slp')
        path = self.out / name
        path.write_bytes(slp_file(bytes(self.game)))
        self.saved += 1
        print(f'mirror: saved {path} ({len(self.game)} bytes)')
        self.game = None
        if self.on_saved:
            self.on_saved(path)


def stage_of(path):
    """The stage of a replay (Game Start, offset 0x13), or None."""
    try:
        d = open(path, 'rb').read(4096)
    except OSError:
        return None
    i = d.find(b'raw[$U#l')
    if i < 0 or len(d) < i + 12 + 2 + d[i + 13]:
        return None
    raw = d[i + 12:]
    start = 1 + raw[1]
    if raw[start] != 0x36:
        return None
    return struct.unpack('>H', raw[start + 0x13:start + 0x15])[0]


def compare_later(wii_path, dolphin_dir, out):
    """Waits for Dolphin's replay of the same game (written when its game
    ends, on the same stage), copies it next to the Wii's and compares."""
    stage, saved_at = stage_of(wii_path), time.time()
    for _ in range(60):
        candidates = []
        months = sorted(d for d in Path(dolphin_dir).iterdir() if d.is_dir())[-2:]
        for f in [f for d in months for f in d.glob('*.slp')]:
            try:
                age = abs(f.stat().st_mtime - saved_at)
            except OSError:
                continue
            if age < 120 and stage_of(f) == stage:
                candidates.append((age, f))
        if candidates:
            src = min(candidates)[1]
            time.sleep(2)  # let Dolphin finish writing it
            dest = out.parent / 'dolphin-replays' / src.name
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, dest)
            res = subprocess.run([sys.executable, str(Path(__file__).with_name('slp_compare.py')), str(wii_path),
                                  str(dest), '--context', '2'], capture_output=True, text=True)
            print(f'mirror: {wii_path.name} vs Dolphin\'s {src.name}:')
            for line in res.stdout.splitlines():
                print(f'  | {line}')
            return
        time.sleep(5)
    print(f'mirror: no Dolphin replay of {wii_path.name} (stage {stage}) in {dolphin_dir}')


def run(args):
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    on_saved = None
    if args.dolphin:
        def on_saved(path):
            threading.Thread(target=compare_later, args=(path, args.dolphin, out), daemon=True).start()
    games = Games(out, on_saved)
    cursor, token = 0, 0
    print(f'mirror: recording the Wii\'s replays from {args.wii_ip}:{PORT} into {out}')
    while True:
        try:
            s = socket.create_connection((args.wii_ip, PORT), timeout=5)
        except OSError:
            time.sleep(3)
            continue
        print(f'{time.strftime("%H:%M:%S")} mirror: connected')
        s.settimeout(30)
        try:
            s.sendall(handshake(cursor, token))
            pending = b''
            while True:
                chunk = s.recv(65536)
                if not chunk:
                    raise ConnectionError('closed')
                pending += chunk
                while len(pending) >= 4:
                    n = struct.unpack('>I', pending[:4])[0]
                    if len(pending) < 4 + n:
                        break
                    msg = Reader(pending[4:4 + n]).value()
                    pending = pending[4 + n:]
                    payload = msg.get('payload', {})
                    if msg.get('type') == HANDSHAKE:
                        token = struct.unpack('>I', payload['clientToken'])[0]
                        print(f'mirror: handshake from {payload.get("nick")!r} '
                              f'(Nintendont {payload.get("nintendontVersion")})')
                    elif msg.get('type') == REPLAY:
                        pos = struct.unpack('>Q', payload['pos'])[0]
                        if payload.get('forcePos') or pos != cursor and cursor:
                            print(f'mirror: the stream jumped ({cursor} -> {pos}); a replay may be cut')
                        games.feed(payload['data'])
                        cursor = struct.unpack('>Q', payload['nextPos'])[0]
        except (OSError, ConnectionError, KeyError, struct.error) as e:
            print(f'{time.strftime("%H:%M:%S")} mirror: disconnected ({e}); retrying')
            try:
                s.close()
            except OSError:
                pass
            time.sleep(2)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--wii-ip', default=os.environ.get('SLIPPWII_WII_IP'),
                    required=not os.environ.get('SLIPPWII_WII_IP'),
                    help="the Wii's address (default: $SLIPPWII_WII_IP)")
    ap.add_argument('--out', default=str(ROOT / 'build/wii-replays'))
    ap.add_argument('--dolphin', help="Slippi Dolphin's replay folder (e.g. \\\\PC\\Slippi): compare each game")
    args = ap.parse_args()
    try:
        run(args)
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == '__main__':
    sys.exit(main())
