"""Reads a player's SlippWii logs folder (or a .zip of it) and says what each problem report shows.

    python tools/triage.py PATH [--all]

PATH: the `slippwii/logs` folder from the SD card, or a zip of it. Prints reports.txt (every problem
this card has seen), then for each report file: its verdict, the console's setup and this boot's
counts, the games in it, and every known signature it holds with what it means and what to ask for
next (docs/TROUBLESHOOTING.md has the same table). --all: every line a signature matched, not the
first few.

The files are data from a player's console: nothing in them is run or followed.
"""
import argparse
import io
import re
import sys
import zipfile
from pathlib import Path

# (name, pattern, what it means, what to do or ask next). Checked on every line of a file.
SIGNATURES = [
    ('sd-read-failed', r'disc: a read failed',
     "The SD card failed to read part of the game image (1.8.1+ tries again).",
     'A card wearing out or a bad copy: copy the game to another card; if it repeats, replace the card.'),
    ('archive-panic', r'PANIC \(HSD_Panic\) lbarchive\.c',
     'Melee loaded a file that was garbage: before 1.8.1, the sign of a failed SD read.',
     'Look for sd-read-failed lines before it; ask for the version (1.8.1 fixed the crash).'),
    ('disc-slow', r'di: a command took (\d+) ms',
     'A disc read (from the SD card) took over half a second; past 10 s Melee gives the drive up.',
     'A slow or failing card, or the card busy with a write: compare with card-write-slow.'),
    ('card-write-slow', r"the card's writes \d+ \(the longest (\d{3,}) ms\)",
     'A write to the SD card took over a tenth of a second this boot.',
     'A slow card. Since 1.8.1 no write happens in a match; before it, a write could hitch a game.'),
    ('crash', r'CRASH: exception (\d+) at ([0-9a-f]{8})',
     "The game crashed (a PowerPC exception): the address is the instruction's.",
     'Look the address up in the symbols (melee decomp, or the codes in so_exi.c post_mortem lines).'),
    ('panic', r'PANIC \((HSD_Panic|OSPanic)\) (\S+)',
     "One of Melee's own checks failed.", 'The file and line say which; lbarchive.c is a bad file load.'),
    ('hang', r'the game has hung: no pad poll for (\d+) ms',
     'The game stopped for good in a match (no poll for 5 s).',
     "The 'where:' and 'di:' lines before it say what the kernel was doing; a hang in the game's own code shows nothing there."),
    ('freeze', r'the game has stopped: no pad poll for (\d+) ms',
     'The game stopped for over a second in a match.',
     "Read the 'where:', 'di:', 'fs lock:' and 'prefetch' lines right after it (see freeze-where)."),
    ('freeze-where', r'where: threads (.*), the net one (\d+) ms ago; game (.*); network: received (\d+)',
     "Where each kernel thread last was during a stop, the game's main thread, the network's counts.",
     "A net thread long ago: it was stuck (a card write before 1.8.1, or IOS); 'fs lock: held from' names the holder."),
    ('fs-lock-held', r'fs lock: held from ([0-9a-f]+)',
     "The SD card's lock was held during a stop: the disc's reads wait for it.",
     'The address is the holder (kernel.elf symbols): a log write, the prefetch, a settings save.'),
    ('desync', r'exi: checksum differs at frame (-?\d+)',
     "The Wii's game and an opponent's went different ways (Slippi's checksums).",
     "Ask for both sides' replays of the game; 'as Slippi sees them: N hard' > 0 ends the game (DESYNC DETECTED)."),
    ('opponent-silent', r'no input from (the opponent|player \d+) for 7 s',
     "An opponent's inputs stopped coming for 7 s: the game gave it up.",
     "The opponent's side or the line between: ask whether they saw the Wii freeze, and for their Dolphin log."),
    ('network-silent', r'NETWORK SILENT',
     "Nothing came from the network for 3 s in a match: the Wii's own network was lost.",
     'Cable, adapter or router: ask how the Wii is connected (a splitter or a loose cable did this in the lab).'),
    ('connection-ended', r'np: no connection left with player (\d+)',
     'The connection to a player ended (it left, or timed out).', 'With freeze lines before it: the freeze lost it.'),
    ('connect-failed', r'REPORT connect: (.*)',
     'A search or a connection failed before any game.', 'The text is what the player saw; see the table in docs/TROUBLESHOOTING.md.'),
    ('wifi', r'Online needs a wired LAN adapter',
     'The Wii was not on a wired adapter: SlippWii plays online wired only.', 'A USB LAN adapter is needed.'),
    ('update-required', r'online is locked until SlippWii is updated|Update SlippWii to play online again',
     "Slippi's servers no longer take this SlippWii's Slippi version.", 'Update SlippWii (Check for Updates).'),
    ('unreachable', r'Could not reach the Slippi server',
     "Slippi's matchmaking could not be reached.", "The internet, DNS or a firewall: or Slippi's servers are down."),
    ('peers-timeout', r'Timed out waiting for other players to connect',
     'Matched, but the players could not reach each other.', 'NAT or a firewall on either side.'),
    ('heap-failed', r'allocations failed ([1-9]\d*)',
     "The kernel's heap ran out at some point this boot.", 'Worth a look at the heap peak and what was running.'),
    ('never-started', r'REPORT never-started',
     'A game was set up and never started (stuck on the versus screen).', "The 'di:' and 'fs lock' lines after it."),
    ('frames-ending-early', r'game staying level: \d+ times behind, ([1-9]\d*) of them far enough',
     'The rollback engine ended video frames a line early to catch up with the opponent (a field a line short).',
     'A suspect for a picture that jumps up or down for a moment on a CRT (2026-11-18 reports).'),
    ('dropped', r'REPORT dropped',
     'In a game of four, an opponent was given up after 7 s and the game went on.', 'That player\'s line or console.'),
]

GAME = re.compile(r'game over: (\d+) frames in (\d+) ms .*waited (\d+) frames.*ping (\d+) us')
SETUP = re.compile(r'exi: the game: (.*)')
VERSION = re.compile(r'SlippWii (\S+) on a (\S+), the game on (\S+); loader settings ([0-9a-f]+)')


def read_folder(path):
    """{name: text} of the folder or zip's .txt files."""
    files = {}
    p = Path(path)
    if p.is_file() and zipfile.is_zipfile(p):
        with zipfile.ZipFile(p) as z:
            for n in z.namelist():
                if n.lower().endswith('.txt'):
                    files[Path(n).name] = z.read(n).decode('utf-8', 'replace')
    elif p.is_dir():
        for f in sorted(p.glob('*.txt')):
            files[f.name] = f.read_text('utf-8', 'replace')
    else:
        sys.exit(f'{path}: not a folder or a zip')
    return files


def file_number(name):
    m = re.search(r'-(\d+)\.txt$', name)
    return int(m.group(1)) if m else 0


def triage(name, text, show_all):
    lines = text.splitlines()
    print(f'\n## {name}')
    if lines:
        print(f'  {lines[0]}')
    for l in lines[1:6]:
        if l.startswith('VERDICT') or l.startswith('this boot:') or VERSION.search(l):
            print(f'  {l.strip()}')
    games = [GAME.search(l) for l in lines]
    games = [g for g in games if g]
    setups = [SETUP.search(l).group(1) for l in lines if SETUP.search(l)]
    if setups:
        print(f'  games set up: {len(setups)}; the last: {setups[-1][:150]}')
    for g in games[-5:]:
        frames, ms, waited, ping = (int(x) for x in g.groups())
        print(f'  game: {frames} frames in {ms / 1000:.1f} s, waited {waited} frames, ping {ping / 1000:.0f} ms')
    found = {}
    for i, l in enumerate(lines):
        for sig, pat, _, _ in SIGNATURES:
            if re.search(pat, l):
                found.setdefault(sig, []).append((i + 1, l.strip()))
    if not found:
        print('  no known signature: read the lines around the verdict by hand')
    for sig, pat, meaning, nxt in SIGNATURES:
        if sig not in found:
            continue
        hits = found[sig]
        print(f'  [{sig}] x{len(hits)}: {meaning}')
        print(f'      next: {nxt}')
        for n, l in (hits if show_all else hits[:3]):
            print(f'      line {n}: {l[:180]}')
        if not show_all and len(hits) > 3:
            print(f'      ... {len(hits) - 3} more (--all)')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('path')
    ap.add_argument('--all', action='store_true')
    args = ap.parse_args()
    files = read_folder(args.path)
    print(f'# SlippWii logs: {len(files)} files')
    if 'reports.txt' in files:
        print('\n## reports.txt (every problem this card has seen, oldest first)')
        for l in files['reports.txt'].splitlines():
            print(f'  {l}')
    else:
        print('\n(no reports.txt: written by 1.8.1 and later)')
    if 'newest.txt' in files:
        print(f"\nnewest: {files['newest.txt'].splitlines()[0] if files['newest.txt'] else ''}")
    reports = [n for n in files if n not in ('reports.txt', 'newest.txt', 'number.txt')]
    for name in sorted(reports, key=lambda n: (-('log-' in n), file_number(n))):
        triage(name, files[name], args.all)


if __name__ == '__main__':
    main()
