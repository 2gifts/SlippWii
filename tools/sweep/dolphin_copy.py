"""Makes an isolated copy of this PC's Slippi Dolphin for automated tests.

The copy is portable (its own User folder), plays with a dummy account, and
talks only to the local fake matchmaking server: its compiled-in server name
(mm.slippi.gg) is patched to 127.0.0.1, and the user-lookup host its Rust
extension knows (internal.slippi.gg) is patched to a name that never
resolves. The player's own install and account are never touched or read.

    python tools/sweep/dolphin_copy.py NAME CONNECT_CODE [--opponent CODE] [--autoplay GECKO.txt]

NAME is the folder under build/sweep/; CONNECT_CODE the dummy account
(e.g. PEER#002); --opponent is put in its direct-code history (so the
autoplay can use autocomplete); --autoplay adds Gecko code lines, enabled.
"""
import argparse
import json
import os
import shutil
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = Path(os.environ['APPDATA']) / 'Slippi Launcher/netplay'
OUT = ROOT / 'build/sweep'
ISO_DIR = Path.home() / 'Downloads/SSB'
SKIP = {'user.json', 'direct-codes.json', 'Logs', 'ScreenShots', 'StateSaves', 'Dump', 'Cache'}

MM_NAME = b'mm.slippi.gg\0'
# (file, original, replacement): same length, so nothing else moves.
PATCHES = (
    ('slippi_rust_extensions.dll', b'internal.slippi.gg', b'internal.slippi.zz'),
)


def full_width(code):
    """A connect code as Slippi stores it in direct-codes.json (full-width)."""
    return ''.join(chr(ord(c) + 0xFEE0) if '!' <= c <= '~' else c for c in code)


def patch(path, old, new):
    assert len(old) == len(new)
    data = path.read_bytes()
    if data.count(old) != 1:
        sys.exit(f'{path.name}: expected one {old!r}, found {data.count(old)} (a new Dolphin build?)')
    path.write_bytes(data.replace(old, new))


def set_ini(path, section, values):
    """Sets keys in one section of a Dolphin .ini, keeping everything else."""
    lines = path.read_text(encoding='utf-8', errors='replace').splitlines() if path.exists() else []
    out, current, done = [], None, set()
    for line in lines:
        s = line.strip()
        if s.startswith('[') and s.endswith(']'):
            if current == section:
                out += [f'{k} = {v}' for k, v in values.items() if k not in done]
                done |= set(values)
            current = s[1:-1]
        elif current == section and '=' in s and s.split('=')[0].strip() in values:
            k = s.split('=')[0].strip()
            out.append(f'{k} = {values[k]}')
            done.add(k)
            continue
        out.append(line)
    if current == section:
        out += [f'{k} = {v}' for k, v in values.items() if k not in done]
    elif section not in [l.strip()[1:-1] for l in lines if l.strip().startswith('[')]:
        out += [f'[{section}]'] + [f'{k} = {v}' for k, v in values.items()]
    path.write_text('\n'.join(out) + '\n', encoding='utf-8')


def make(name, code, opponent=None, autoplay=None, mm_host='127.0.0.1'):
    """mm_host: where the fake matchmaking server is. Use this PC's LAN
    address when the opponent is another machine (the Wii): Dolphin reports
    the address it reaches the server from as its own, and 127.0.0.1 would
    send the Wii to itself."""
    if len(mm_host) >= len(MM_NAME):
        sys.exit(f'{mm_host}: at most {len(MM_NAME) - 1} characters (it replaces mm.slippi.gg in place)')
    dest = OUT / name
    if dest.exists():
        shutil.rmtree(dest)
    shutil.copytree(SRC, dest, ignore=lambda d, names: [n for n in names if n in SKIP])
    patch(dest / 'Slippi Dolphin.exe', MM_NAME, mm_host.encode().ljust(len(MM_NAME), b'\0'))
    for file, old, new in PATCHES:
        patch(dest / file, old, new)
    user = dest / 'User'
    (user / 'Slippi').mkdir(parents=True, exist_ok=True)
    tag = code.split('#')[0].lower()
    (user / 'Slippi/user.json').write_text(json.dumps({
        'uid': f'local-test-uid-{tag}', 'playKey': f'dummy-play-key-{tag}', 'connectCode': code,
        'displayName': f'Test {code.split("#")[0].title()}', 'latestVersion': '3.6.4'}), encoding='utf-8')
    history = [{'connectCode': full_width(opponent), 'lastPlayed': int(time.time())}] if opponent else []
    (user / 'Slippi/direct-codes.json').write_text(json.dumps(history, ensure_ascii=False), encoding='utf-8')
    replays = dest / 'replays'
    replays.mkdir(exist_ok=True)
    gc = user / 'GC'
    set_ini(user / 'Config/Dolphin.ini', 'Core', {
        'SlippiReplayDir': str(replays), 'SlippiReplayMonthFolders': 'True', 'SlippiSaveReplays': 'True',
        'SlippiEnableSpectator': 'False', 'SlippiJukeboxEnabled': 'False', 'SlippiOnlineDelay': '2',
        'EnableCheats': 'True',  # Dolphin applies user Gecko codes (the autoplay) only with this
        'SlippiForceLanIp': 'False' if mm_host.startswith('127.') else 'True',
        'SlippiLanIp': '' if mm_host.startswith('127.') else mm_host,
        'SIDevice0': '6', 'SIDevice1': '0', 'SIDevice2': '0', 'SIDevice3': '0',
        'MemcardAPath': str(gc / 'MemoryCardA.USA.raw'), 'MemcardBPath': str(gc / 'MemoryCardB.USA.raw'),
    })
    set_ini(user / 'Config/Dolphin.ini', 'Display', {'Fullscreen': 'False', 'RenderToMain': 'True'})
    set_ini(user / 'Config/Dolphin.ini', 'General', {'ISOPaths': '1', 'ISOPath0': str(ISO_DIR)})
    game = user / 'GameSettings/GALE01.ini'
    game.parent.mkdir(parents=True, exist_ok=True)
    text = '[Gecko]\n'
    enabled = '[Gecko_Enabled]\n'
    if autoplay:
        body = Path(autoplay).read_text(encoding='utf-8')
        text += body if body.endswith('\n') else body + '\n'
        # enabled by name, without the [author] part, as Dolphin's own lists
        enabled += ''.join(l.split(' [')[0] + '\n' for l in body.splitlines() if l.startswith('$'))
    game.write_text(text + enabled, encoding='utf-8')
    print(f'{dest}: {code}' + (f', opponent {opponent}' if opponent else '') + (', autoplay' if autoplay else ''))
    return dest


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('name')
    ap.add_argument('code')
    ap.add_argument('--opponent')
    ap.add_argument('--autoplay')
    ap.add_argument('--mm-host', default='127.0.0.1')
    args = ap.parse_args()
    make(args.name, args.code, args.opponent, args.autoplay, args.mm_host)


if __name__ == '__main__':
    main()
