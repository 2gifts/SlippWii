"""Makes the SD card files SlippWii needs, from your own Melee disc.

On a PC, Slippi Dolphin runs Slippi's online codeset and serves Slippi's menu
files to the game. On a Wii, SlippWii does the same from the SD card:

    /slippwii/netplay.gct     the online codeset: what Slippi Dolphin runs,
                              minus its music hooks (on a PC, Dolphin plays
                              the music; on a Wii the game does, as normal)
    /slippwii/files/<name>    Slippi's menu files: the files Slippi Dolphin
                              ships, and its patches applied to your disc's
                              menus

Both come from the Slippi Dolphin release PC players run (pinned below,
downloaded from Project Slippi's GitHub and checked by SHA-256), so the Wii
runs the same game code as its opponents. Nothing from the disc or from
Slippi is shipped with SlippWii: the patches are applied to your own disc
(an NTSC 1.02 .iso, .gcm or .ciso, or a folder of its extracted files).

The installer (install.py) uses this; developers can run it directly:

    python installer/slippi_files.py --iso "Melee.iso" --out E:/
    python installer/slippi_files.py --files extracted/files --out staging/

Nothing is written outside --out/slippwii/; user.json and config.ini there
are left alone.
"""
import argparse
import hashlib
import os
import re
import struct
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from vcdiff import decode  # noqa: E402

# Downloads are kept here (a developer checkout's build/ folder, or next to
# the installer).
CACHE = (HERE.parent / 'build/slippi-release') if (HERE.parent / 'Nintendont').exists() else HERE / 'downloads'

# The Slippi Dolphin releases PC players run, and their files' hashes. The
# codeset is the stable one (Slippi Launcher's default, 3.6.4, the version the
# Wii reports to matchmaking); it differs from mainline beta.19's only in
# cosmetic code (delay text, desync sound, a results screen hook, replay
# version 3.19.1). The game files are byte-identical in both.
RELEASE = 'v4.0.0-mainline-beta.19'
RAW = f'https://raw.githubusercontent.com/project-slippi/dolphin/{RELEASE}/Data/Sys'
STABLE_RAW = 'https://raw.githubusercontent.com/project-slippi/Ishiiruka/v3.6.4/Data/Sys'
CODESET = ('GameSettings/GALE01r2.ini', 'b30b294df5c0d92deb3129afdfbc894a48aabeb1bce0cf92cce8c4e5408a2587')
GAME_FILES = {
    'GameSetup.dat': '2c7842f8864c962ea0e8b9e629552b36e2669b739c86d17ba5dae53d75ed9acc',
    'GameSetup_gui.dat': 'aa083e7ec6a7ad96c32efb474b6915242aeb0627c2fa6369b205783b844d701e',
    'MnExtAll.dat.diff': 'b6df247ddf12d0118d4b1deaf1d3cf8878ebb4d60b29fda7ad6a324246b05e22',
    'MnExtAll.usd.diff': 'c729c61a65d92aeaf4ab15ca7ec5a40ead336055b0defce9c74503e2651c9b18',
    'MnMaAll.dat.diff': '4f38bfa9716064fd59315c26210ec6383b62ee0d4445beb9b6cca1f09134ea75',
    'MnMaAll.usd.diff': 'e0e261203e013f7e51c29f808165ca36dca10296c3e613b1b225e0a6d4978263',
    'MnSlMap.dat.diff': '43860c806406b85e05d312401548ef7b0da15e62d44bbd279ff2885e4ea3ec27',
    'MnSlMap.usd.diff': 'dcf99eeaab8f981a2aad25e341eda79bc9b417dd098fc39ff20cb328fbc1510d',
    'MxDb.dat': '58e96852c9b6a1ec1fe53b5de6c307465a72684094d6dbcd16dd89f6a29b76c5',
    'MxScn.dat': '7012066e3e039b8f022582d59657d8206eebf13e4c39f4a56fe3b313cc9942bb',
    'SdMenu.dat.diff': 'b284732a79b787129704e81c110d9ac11210bc3e8f579e27e8e38525b656fe50',
    'SdMenu.usd.diff': '00a92643e41af867dd22ba159f7ec022e6aa3b7288b537b2ba00cf162f5579cb',
    'SdSlChr.dat.diff': '0194bcbb4fd26df4e499edd52ea7a64743a493515b07d6114cf378c8720a9405',
    'SdSlChr.usd.diff': 'fde2f393c405244f9100a0522a769d4fcce9e892dd9b87b0e0c38670ef132f38',
    'SlippiCSS.dat': 'f864eed3f494f1a1710760973f25a7f5be4bb3dc4fc4734c2a721ca62f4916f3',
    'slpCSS.dat': '3b1a3f254b37cf5ce4f56b38d237d9809d0b11e72d05320f0e56fa3abd7a1692',
}

# Code blocks left out, by the source the codeset names for them: Dolphin
# plays the music on a PC (these mute the game's and tell Dolphin what to
# play), while on a Wii the game plays it itself.
DROPPED = (
    'Online/Core/Music/StartSong.asm',
    'Online/Core/Music/Stop.asm',
    'Online/Core/Music/VolumeChange.asm',
    'Online/Core/PreventFileAlarms/MuteMusic.asm',
    'Online/Core/PreventFileAlarms/PreventMusicAlarm.asm',
)

# Codes left out by the address they patch: "Normal Lag Reduction", which is
# Dolphin-only. It copies frames to display buffer -1, harmless where the copy
# never reaches RAM; on a console the GPU's write hangs the bus (Melee froze at
# its first frame). Frame pacing only: the game logic is unchanged. (The kernel
# drops it too, for older files.) "Polling Drift Fix + VB" stays: online play
# needs one pad poll per engine frame (without it, a Stadium transformation
# put the Wii's sent inputs 4 polls behind the ones it played: a desync).
DROPPED_ADDRESSES = (
    0x803761EC, 0x80376238,  # Normal Lag Reduction [Hannes Mann]
)

GCT_HEADER = bytes.fromhex('00D0C0DE00D0C0DE')
GCT_FOOTER = bytes.fromhex('FF00000000000000')


class SetupError(Exception):
    """A problem the person installing can fix (the message says how)."""


def download(url):
    """Three tries, then Windows' own curl.exe (some PCs' Python has no
    certificate for GitHub)."""
    last = None
    for attempt in range(3):
        try:
            with urllib.request.urlopen(url, timeout=60) as r:
                return r.read()
        except OSError as e:
            last = e
            time.sleep(2 * (attempt + 1))
    curl = Path(os.environ.get('SystemRoot', 'C:/Windows')) / 'System32/curl.exe'
    if curl.exists():
        res = subprocess.run([str(curl), '-fsSL', '--retry', '3', url], capture_output=True)
        if res.returncode == 0:
            return res.stdout
    raise SetupError(f'could not download {url} ({last}). Check your internet connection '
                     'and that your PC\'s date and time are right, then try again.')


def fetch(rel, sha, dest, raw=RAW, log=print):
    """A pinned release file, from the cache or Project Slippi's GitHub."""
    if dest.exists() and hashlib.sha256(dest.read_bytes()).hexdigest() == sha:
        return dest.read_bytes()
    url = f'{raw}/{rel}'
    log(f'  downloading {rel.split("/")[-1]}')
    data = download(url)
    got = hashlib.sha256(data).hexdigest()
    if got != sha:
        raise SetupError(f'{rel}: the download is not the expected file (SHA-256 {got}); try again later')
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(data)
    return data


# ---- the codeset -------------------------------------------------------------

def parse_ini(text):
    """Dolphin's Gecko::LoadCodes: [Gecko] codes in order, and [Gecko_Enabled]."""
    section, enabled, codes, current = None, set(), [], None
    for line in text.splitlines():
        s = line.strip()
        if s.startswith('['):
            section = s
        elif section == '[Gecko_Enabled]' and s.startswith('$'):
            enabled.add(s[1:].strip())
        elif section == '[Gecko]':
            if s.startswith('$'):
                current = (s[1:].split('[')[0].strip(), [])
                codes.append(current)
            elif current and re.match(r'^[0-9A-Fa-f]{8} [0-9A-Fa-f]{8}', s):
                note = s.split('#', 1)[1].strip() if '#' in s else ''
                current[1].append((int(s[:8], 16), int(s[9:17], 16), note))
    return [(name, lines) for name, lines in codes if name in enabled]


def code_blocks(lines):
    """Splits a code's lines into Gecko codes (type 04 is one line, C2 is
    1 + its line count). The codeset only uses these two."""
    i = 0
    while i < len(lines):
        address, value, note = lines[i]
        kind = (address >> 24) & 0xFE
        if kind == 0xC2:
            n = 1 + value
        elif kind == 0x04:
            n = 1
        else:
            raise SystemExit(f'unexpected Gecko code type {address:08X} in the codeset')
        if i + n > len(lines):
            raise SystemExit(f'truncated code at {address:08X}')
        yield note, lines[i:i + n]
        i += n


def build_gct(ini_text):
    out, dropped, kept = bytearray(GCT_HEADER), [], 0
    for name, lines in parse_ini(ini_text):
        for note, block in code_blocks(lines):
            if any(note.startswith(d) for d in DROPPED):
                dropped.append(note)
                continue
            if 0x80000000 | (block[0][0] & 0x01FFFFFF) in DROPPED_ADDRESSES:
                dropped.append(f'{block[0][0]:08X}')
                continue
            for address, value, _ in block:
                out += struct.pack('>II', address, value)
            kept += len(block)
    missing = [d for d in DROPPED if not any(n.startswith(d) for n in dropped)]
    if missing:
        raise SystemExit(f'codeset changed: no block for {missing}')
    out += GCT_FOOTER
    return bytes(out), kept, dropped


# ---- the disc ----------------------------------------------------------------

CISO_HEADER = 0x8000


class Iso:
    """A GameCube disc image (.iso/.gcm, or Nintendont's .ciso): the files
    of its FST, by name."""

    def __init__(self, path):
        self.f = open(path, 'rb')
        self.blocks = None
        head = self.f.read(CISO_HEADER)
        if head[:4] == b'CISO':
            # 'CISO', block size (LE), a map byte per block: 1 = stored, in order
            self.block_size = struct.unpack('<I', head[4:8])[0]
            used, self.blocks = 0, []
            for flag in head[8:CISO_HEADER]:
                self.blocks.append(used if flag == 1 else None)
                used += flag == 1
        header = self.read(0, 0x440)
        if header[0x200:0x204] == b'NKIT':
            raise SetupError(f'{path} is an NKit image. Convert it back to a full .iso '
                             '(NKit Processing App, "Recover") and use that.')
        self.game_id, self.version = header[:6].decode('latin1'), header[7]
        fst_offset, fst_size = struct.unpack('>II', header[0x424:0x42C])
        fst = self.read(fst_offset, fst_size)
        count = struct.unpack('>I', fst[8:12])[0]
        names = fst[count * 12:]
        self.files = {}
        for i in range(1, count):
            flags_name, offset, size = struct.unpack('>III', fst[i * 12:i * 12 + 12])
            if flags_name >> 24:
                continue  # a directory
            start = flags_name & 0xFFFFFF
            name = names[start:names.index(b'\0', start)].decode('latin1')
            self.files.setdefault(name, (offset, size))

    def read(self, offset, size):
        if self.blocks is None:
            self.f.seek(offset)
            return self.f.read(size)
        out = bytearray()
        while size > 0:
            index, within = divmod(offset, self.block_size)
            n = min(size, self.block_size - within)
            stored = self.blocks[index] if index < len(self.blocks) else None
            if stored is None:
                out += bytes(n)
            else:
                self.f.seek(CISO_HEADER + stored * self.block_size + within)
                out += self.f.read(n)
            offset += n
            size -= n
        return bytes(out)

    def file(self, name):
        if name not in self.files:
            return None
        return self.read(*self.files[name])


class Folder:
    """Extracted disc files (flat, or in folders)."""

    def __init__(self, path):
        self.root = Path(path)
        self.game_id, self.version = None, None
        sys_dir = self.root.parent / 'sys'
        boot = sys_dir / 'boot.bin'
        if boot.exists():
            header = boot.read_bytes()[:8]
            self.game_id, self.version = header[:6].decode('latin1'), header[7]

    def file(self, name):
        direct = self.root / name
        if direct.exists():
            return direct.read_bytes()
        for path in self.root.rglob(name):
            return path.read_bytes()
        return None


def open_disc(path):
    """An Iso or Folder for this path, checked to be NTSC 1.02 Melee."""
    path = Path(path)
    if not path.exists():
        raise SetupError(f'{path} does not exist')
    disc = Folder(path) if path.is_dir() else Iso(path)
    if disc.game_id is not None and (disc.game_id != 'GALE01' or disc.version != 2):
        raise SetupError(f'{path.name} is {disc.game_id} revision {disc.version}, not NTSC 1.02 Melee '
                         '(GALE01 revision 2), the only version Slippi supports.')
    return disc


def make_files(disc, root, log=print):
    """Writes ROOT/slippwii/netplay.gct and ROOT/slippwii/files/."""
    out = Path(root) / 'slippwii'
    files_out = out / 'files'
    files_out.mkdir(parents=True, exist_ok=True)

    log('Slippi\'s online code (from Project Slippi\'s GitHub):')
    ini = fetch(CODESET[0], CODESET[1], CACHE / 'GALE01r2-stable.ini', STABLE_RAW, log).decode('utf-8')
    gct, lines, dropped = build_gct(ini)
    if len(gct) >= 0x10000:
        raise SystemExit(f'the codeset is {len(gct)} bytes; the bootloader reads under 64 KiB')
    (out / 'netplay.gct').write_bytes(gct)
    log(f'  netplay.gct: {lines} code lines, {len(gct)} bytes')

    log('Slippi\'s menu files (patched from your disc):')
    made = 0
    for name, sha in GAME_FILES.items():
        data = fetch(f'GameFiles/GALE01/{name}', sha, CACHE / 'GameFiles' / name, log=log)
        if name.endswith('.diff'):
            target = name[:-5]
            source = disc.file(target)
            if source is None:
                raise SetupError(f'{target} is not on the disc; is this NTSC 1.02 Melee?')
            try:
                data = decode(source, data)
            except ValueError as e:
                raise SetupError(f'{target}: {e} (is this an unmodified NTSC 1.02 disc?)')
            if len(data) < 4 or int.from_bytes(data[:4], 'big') != len(data):
                raise SetupError(f'{target}: the patched file is not a valid archive '
                                 '(is this an unmodified NTSC 1.02 disc?)')
            name = target
        (files_out / name).write_bytes(data)
        made += 1
    log(f'  {made} menu files')
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument('--iso', help='your NTSC 1.02 Melee disc image (.iso, .gcm or .ciso)')
    src.add_argument('--files', help='or a folder of its extracted files')
    ap.add_argument('--out', required=True, help='SD card (or USB drive) root, or a staging folder')
    args = ap.parse_args()
    try:
        out = make_files(open_disc(args.iso or args.files), args.out)
    except SetupError as e:
        raise SystemExit(str(e))
    print(f'done: {out}')


if __name__ == '__main__':
    main()
