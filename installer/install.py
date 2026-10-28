"""SlippWii installer: sets up a Wii's SD card for Slippi Online.

    Windows: double-click "Install SlippWii.bat" (it runs this with a private
             copy of Python).
    macOS / Linux: python3 installer/install.py

It finds the SD card, your Melee disc image and your Slippi account (the one
Slippi Launcher on this computer is logged in to), then writes:

    SD:/apps/SlippWii/              the app, for the Homebrew Channel
    SD:/slippwii/user.json          your Slippi account (your login: keep it private)
    SD:/slippwii/netplay.gct        Slippi's online code   } downloaded from Project
    SD:/slippwii/files/             Slippi's menu files    } Slippi, patched from your disc
    SD:/slippwii/config.ini         settings (only if there is none yet)

Nothing else on the card is touched; Slippi Nintendont (apps/, its
slippi_nincfg.bin) and your games stay as they are. Options for scripting:
--sd, --iso, --account, --yes.
"""
import argparse
import ctypes
import datetime
import json
import os
import platform
import shutil
import string
import subprocess
import sys
import traceback
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import slippi_files  # noqa: E402
from slippi_files import SetupError  # noqa: E402

VERSION = (HERE / 'VERSION').read_text().strip() if (HERE / 'VERSION').exists() else 'dev'
ISSUES = 'https://github.com/2gifts/SlippWii/issues'
LOG = []
STEPS = 5
WINDOWS = os.name == 'nt'


# ---- output ------------------------------------------------------------------

def say(text=''):
    print(text)
    LOG.append(text)


def step(n, title):
    say()
    say(f'[{n}/{STEPS}] {title}')


def ask(question, default=True, auto=False):
    if auto:
        return default
    hint = 'Y/n' if default else 'y/N'
    while True:
        try:
            answer = input(f'{question} [{hint}] ').strip().lower()
        except EOFError:
            return default
        if not answer:
            return default
        if answer in ('y', 'yes'):
            return True
        if answer in ('n', 'no'):
            return False


def ask_path(question):
    """A path typed or dragged into the window (quotes stripped)."""
    try:
        text = input(question + ' ').strip().strip('"').strip("'").strip()
    except EOFError:
        return None
    return Path(text) if text else None


# ---- the SD card -------------------------------------------------------------

def drives():
    """[(root Path, label, total bytes, removable)] for the mounted drives."""
    found = []
    if WINDOWS:
        k32 = ctypes.windll.kernel32
        mask = k32.GetLogicalDrives()
        for i, letter in enumerate(string.ascii_uppercase):
            if not mask & (1 << i) or letter in 'AB':
                continue
            root = f'{letter}:\\'
            kind = k32.GetDriveTypeW(root)
            if kind not in (2, 3):  # removable, fixed
                continue
            name = ctypes.create_unicode_buffer(261)
            if not k32.GetVolumeInformationW(root, name, 261, None, None, None, None, 0):
                continue  # an empty card reader
            total = ctypes.c_ulonglong()
            k32.GetDiskFreeSpaceExW(root, None, ctypes.byref(total), None)
            found.append((Path(root), name.value, total.value, kind == 2))
    else:
        bases = [Path('/Volumes')] if platform.system() == 'Darwin' else \
            [Path('/media') / os.environ.get('USER', ''), Path('/run/media') / os.environ.get('USER', ''), Path('/media')]
        for base in bases:
            if base.is_dir():
                for root in sorted(base.iterdir()):
                    if root.is_dir() and not root.is_symlink() and root != Path('/media') / os.environ.get('USER', ''):
                        try:
                            total = shutil.disk_usage(root).total
                        except OSError:
                            continue
                        found.append((root, root.name, total, True))
    return found


def looks_like_wii_card(root):
    return any((root / name).exists() for name in ('apps', 'games', 'private', 'slippwii', 'slippi_nincfg.bin'))


def find_sd(args):
    if args.sd:
        root = Path(args.sd)
        if not root.is_dir():
            raise SetupError(f'{root} is not a folder or drive.')
        return root
    while True:
        cards = [d for d in drives() if d[3] and d[2] < 2_200_000_000_000]
        wii = [d for d in cards if looks_like_wii_card(d[0])]
        choices = wii or cards
        if len(choices) == 1:
            root, label, total, _ = choices[0]
            what = 'the Wii SD card' if wii else 'a removable drive'
            if ask(f'Found {what} {root} ("{label}", {total / 1e9:.1f} GB). Install SlippWii on it?', True, args.yes):
                return root
        elif choices:
            say('Removable drives:')
            for i, (root, label, total, _) in enumerate(choices, 1):
                mark = '  (has Homebrew Channel apps)' if looks_like_wii_card(root) else ''
                say(f'  {i}. {root}  "{label}"  {total / 1e9:.1f} GB{mark}')
            try:
                pick = input('Type the number of your Wii\'s SD card and press Enter: ').strip()
            except EOFError:
                pick = ''
            if pick.isdigit() and 1 <= int(pick) <= len(choices):
                return choices[int(pick) - 1][0]
            continue
        else:
            say('No SD card found. Put your Wii\'s SD card in this computer (a card reader is fine).')
        if args.yes:
            raise SetupError('no SD card (use --sd)')
        typed = ask_path('Then press Enter to look again, or type the card\'s drive or folder (like E:) and press Enter:')
        if typed:
            root = Path(str(typed) + ('\\' if WINDOWS and len(str(typed)) == 2 else ''))
            if root.is_dir():
                return root
            say(f'{typed} was not found.')


# ---- the disc ----------------------------------------------------------------

DISC_TYPES = ('.iso', '.gcm', '.ciso')


def disc_id(path):
    """(game id, revision) of a disc image, or None."""
    try:
        with open(path, 'rb') as f:
            head = f.read(8)
            if head[:4] == b'CISO':
                f.seek(0x8000)
                head = f.read(8)
        return head[:6].decode('latin1'), head[7]
    except OSError:
        return None


def is_melee(path):
    return disc_id(path) == ('GALE01', 2)


def launcher_dirs():
    if WINDOWS:
        return [Path(os.environ.get('APPDATA', '')) / 'Slippi Launcher']
    if platform.system() == 'Darwin':
        return [Path.home() / 'Library/Application Support/Slippi Launcher']
    return [Path.home() / '.config/Slippi Launcher']


def launcher_iso():
    for base in launcher_dirs():
        try:
            path = json.loads((base / 'Settings').read_text(encoding='utf-8'))['settings']['isoPath']
        except (OSError, ValueError, KeyError, TypeError):
            continue
        if path and Path(path).is_file():
            return Path(path)
    return None


def discs_on(root, depth=3):
    out = []

    def walk(folder, level):
        try:
            entries = list(os.scandir(folder))
        except OSError:
            return
        for e in entries:
            try:
                if e.is_file() and e.name.lower().endswith(DISC_TYPES) and e.stat().st_size > 100_000_000:
                    out.append(Path(e.path))
                elif e.is_dir() and level < depth and not e.name.startswith(('.', '$')) \
                        and e.name.lower() not in ('windows', 'program files', 'program files (x86)', 'appdata'):
                    walk(e.path, level + 1)
            except OSError:
                pass

    walk(root, 0)
    return out


def find_disc(args, sd):
    if args.iso:
        return Path(args.iso)
    candidates = []
    iso = launcher_iso()
    if iso:
        candidates.append((iso, 'the one Slippi Launcher uses'))
    for folder in (sd / 'games', sd):
        if folder.is_dir():
            candidates += [(p, 'on the SD card') for p in discs_on(folder, 2 if folder == sd else 3)]
    for root, _, _, removable in drives():
        if removable and root != sd and (root / 'games').is_dir():
            candidates += [(p, f'on {root}') for p in discs_on(root / 'games')]
    for folder in (Path.home() / 'Downloads', Path.home() / 'Desktop', Path.home() / 'Documents'):
        if folder.is_dir():
            candidates += [(p, f'in {folder.name}') for p in discs_on(folder, 2)]
    seen = set()
    for path, where in candidates:
        key = str(path).lower()
        if key in seen:
            continue
        seen.add(key)
        if is_melee(path):
            if ask(f'Found Melee NTSC 1.02 ({where}):\n  {path}\nUse it?', True, args.yes):
                return path
    if args.yes:
        raise SetupError('no NTSC 1.02 Melee disc image found (use --iso)')
    say('SlippWii needs your Melee disc image (NTSC 1.02, .iso), the same one Slippi uses.')
    while True:
        typed = ask_path('Drag the .iso file into this window, then press Enter:')
        if typed and typed.is_file():
            ident = disc_id(typed)
            if ident == ('GALE01', 2):
                return typed
            if ident and ident[0] == 'GALE01':
                say(f'That is Melee revision {ident[1]}; Slippi needs NTSC 1.02 (revision 2).')
            else:
                say('That is not NTSC Melee (GALE01).')
        elif typed:
            say(f'{typed} was not found.')


# ---- the Slippi account --------------------------------------------------------

def account_files():
    found = []
    for base in launcher_dirs():
        for sub in ('netplay', 'netplay-beta'):
            for rel in ('User/Slippi/user.json', 'Slippi Dolphin.app/Contents/Resources/User/Slippi/user.json'):
                p = base / sub / rel
                if p.is_file():
                    found.append(p)
    if not WINDOWS:
        for p in (Path.home() / '.config/SlippiOnline/Slippi/user.json',
                  Path.home() / 'Library/Application Support/com.project-slippi.dolphin/netplay/User/Slippi/user.json'):
            if p.is_file():
                found.append(p)
    return found


def read_account(path):
    """The account's fields, or None if this is not a Slippi user.json."""
    try:
        data = json.loads(Path(path).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return None
    if not isinstance(data, dict) or not all(data.get(k) for k in ('uid', 'playKey', 'connectCode')):
        return None
    return data


def describe(account):
    return f'{account.get("displayName") or "(no name)"} ({account["connectCode"]})'


def find_account(args):
    if args.account:
        account = read_account(args.account)
        if not account:
            raise SetupError(f'{args.account} is not a Slippi user.json')
        return Path(args.account), account
    accounts = []
    for path in account_files():
        account = read_account(path)
        if account and all(a[1]['uid'] != account['uid'] for a in accounts):
            accounts.append((path, account))
    for path, account in accounts:
        if ask(f'Found your Slippi account: {describe(account)}. Use it on the Wii?', True, args.yes):
            return path, account
    say()
    if accounts:
        say('To use another account: log in to it in Slippi Launcher and run this installer again,')
        say('or use its user.json file (Slippi Launcher keeps it in its netplay/User/Slippi folder).')
    else:
        say('No Slippi account was found on this computer. To make one (free):')
        say('  1. Install Slippi Launcher from https://slippi.gg and open it.')
        say('  2. Log in (or create an account), then click "Play" once so it sets up Dolphin.')
        say('  3. Run this installer again.')
        say('(Or, if you have your user.json file from another computer, you can use that.)')
    if args.yes:
        raise SetupError('no Slippi account found (use --account)')
    while True:
        typed = ask_path('Drag a user.json into this window and press Enter (or just press Enter to skip):')
        if not typed:
            return None, None
        account = read_account(typed)
        if account:
            return typed, account
        say('That is not a Slippi user.json.')


# ---- the installation ------------------------------------------------------------

CONFIG_INI = """# SlippWii settings. Every line is optional; delete this file to get the defaults.
#
# Input delay in frames (1-9) for online games. You can also change it on the
# Wii: the "Delay Frames" buttons on the online character select screen (that
# choice is kept in delay.txt and wins over this line).
delay=2
#
# Quick chat (D-pad) in online games: 1 = on, 0 = off.
chat=1
"""


def app_folder():
    for path in (HERE / 'app/SlippWii', HERE.parent / 'dist/sd/apps/SlippWii'):
        if (path / 'boot.dol').is_file():
            return path
    raise SetupError('the SlippWii app files are missing. Extract the whole zip and run the installer from there.')


def install(sd, disc_path, account_path, account):
    app = app_folder()
    dest_app = sd / 'apps' / 'SlippWii'
    folder = sd / 'slippwii'
    folder.mkdir(parents=True, exist_ok=True)

    say('Slippi\'s files from your disc:')
    slippi_files.make_files(slippi_files.open_disc(disc_path), sd, log=say)

    if account:
        dest = folder / 'user.json'
        if dest.exists() and dest.read_bytes() != Path(account_path).read_bytes():
            old = read_account(dest)
            backup = folder / f'user.json.old-{datetime.datetime.now():%Y%m%d-%H%M%S}'
            dest.replace(backup)
            say(f'  replaced the account {describe(old) if old else "(unreadable)"} (kept as {backup.name})')
        shutil.copyfile(account_path, dest)
        say(f'  your account: {describe(account)}')

    config = folder / 'config.ini'
    if not config.exists():
        config.write_text(CONFIG_INI, encoding='utf-8', newline='\n')
        say('  settings: config.ini (input delay 2)')

    dest_app.mkdir(parents=True, exist_ok=True)
    for f in app.iterdir():
        if f.is_file():
            shutil.copyfile(f, dest_app / f.name)
    say(f'  the app: {dest_app}')


def flush(sd):
    if WINDOWS and len(str(sd)) <= 3:
        subprocess.run(['powershell', '-NoProfile', '-Command', f'Write-VolumeCache {str(sd)[0]}'],
                       capture_output=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('iso_arg', nargs='?', help=argparse.SUPPRESS)  # a disc dropped on the .bat
    ap.add_argument('--sd', help='the SD card (drive or folder)')
    ap.add_argument('--iso', help='your NTSC 1.02 Melee disc image')
    ap.add_argument('--account', help='a Slippi user.json')
    ap.add_argument('--yes', action='store_true', help='answer yes to every question')
    args = ap.parse_args()
    args.iso = args.iso or args.iso_arg or None

    say(f'SlippWii {VERSION} installer: unofficial Slippi Online for the Wii')
    say('(Not affiliated with Project Slippi or Nintendo. Open source: https://github.com/2gifts/SlippWii)')
    try:
        step(1, 'The SD card')
        sd = find_sd(args)
        step(2, 'Your Melee disc (NTSC 1.02)')
        disc = find_disc(args, sd)
        step(3, 'Your Slippi account')
        account_path, account = find_account(args)
        if not account:
            say('  Skipped: the Wii will not be able to play online until you run the installer')
            say('  again with an account (or copy your user.json to SD:/slippwii/user.json).')
        step(4, f'Installing to {sd}')
        install(sd, disc, account_path, account)
        flush(sd)
        step(5, 'Done!')
        say('Next:')
        say('  1. Safely eject the SD card and put it in your Wii.')
        say('  2. Connect the Wii to your router with a USB LAN adapter (Wi-Fi is too slow),')
        say('     and set it up in Wii Settings > Internet if you have not yet.')
        say('  3. Open the Homebrew Channel, start SlippWii, pick Melee, then')
        say('     Online Play > Direct (or Unranked) as on Slippi Dolphin.')
        say('Slippi Nintendont, if you have it, is untouched: both can be on the card.')
        code = 0
    except SetupError as e:
        say()
        say(f'Stopped: {e}')
        say('Nothing else was changed. Fix that and run the installer again.')
        code = 1
    except KeyboardInterrupt:
        say()
        say('Stopped.')
        code = 1
    except Exception:
        say()
        say('Something went wrong (a bug in the installer):')
        say(traceback.format_exc())
        say(f'Please report it with install-log.txt: {ISSUES}')
        code = 2
    try:
        (HERE / 'install-log.txt').write_text('\n'.join(LOG) + '\n', encoding='utf-8')
    except OSError:
        pass
    return code


if __name__ == '__main__':
    sys.exit(main())
