"""The rules SlippWii keeps as a guest on Slippi's servers, tested on the PC build of the
Wii's online client (Nintendont/kernel/slippi_online/tests/pc/build.sh) against the local
fake matchmaking server. Nothing here talks to Slippi's servers.

  the version gate   a server that turns the client's version away, naming a newer one, as
                     Slippi's does: the client keeps that version on the card and searches
                     no more, at this start or the next, until it speaks that version
  wired only         a console on Wi-Fi sends no search
  the client's name  the forms SO_CLIENT_NAME_MODE has ready reach the server

    python tools/test_online_gates.py [--tools DIR] [--exe so_pc_selftest.exe]
"""
import argparse
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_TOOLS = ROOT.parent / 'slippi-3ds/native-3ds/build/slippi-tools'
PORT = 43213  # (not the sweeps' 43113: a sweep may be running)
USER = ('{"uid": "local-test-uid-wii", "playKey": "dummy-play-key-wii", "connectCode": "WIIT#001", '
        '"displayName": "Test Wii", "latestVersion": "3.6.4"}')
STORED = 'test-slippi-version.txt'  # a test server's: never the file a real console reads
failures = 0


def check(what, ok):
    global failures
    failures += not ok
    print(f"  {'ok  ' if ok else 'FAIL'} {what}")


def run(tools, exe, folder, settings, mm_args=(), seconds=7, turns_away=None):
    """One start of the client in `folder` (its card): (the client's log, the server's).
    turns_away: (version, 'ticket' or 'match'): the server is so_pc_fake_mm, which turns versions
    under that one away and names it, in that reply; otherwise the 3DS fork's fake server."""
    (folder / 'config.ini').write_text('\n'.join(
        ['user_path=/slippwii/test-user.json', 'mm_host=127.0.0.1', f'mm_port={PORT}', 'selftest=1',
         'selftest_code=PEER#002', 'boot_quiet_s=0', *settings]) + '\n', encoding='utf-8')
    (folder / 'test-user.json').write_text(USER, encoding='utf-8')
    mm_log, log = folder / 'fake_mm.log', folder / 'client.log'
    with open(mm_log, 'w') as mf, open(log, 'w') as lf:
        server = ([str(exe.with_name('so_pc_fake_mm.exe')), str(PORT), *turns_away, str(seconds + 10)] if turns_away else
                  [str(tools / 'slippi_fake_mm.exe'), '--port', str(PORT), *mm_args])
        mm = subprocess.Popen(server, stdout=mf, stderr=subprocess.STDOUT)
        time.sleep(0.5)
        client = subprocess.Popen([str(exe), str(folder), str(seconds)], stdout=lf, stderr=subprocess.STDOUT)
        try:
            client.wait(seconds + 5)
        except subprocess.TimeoutExpired:
            client.kill()
        mm.terminate()
        mm.wait()
    return (log.read_text(encoding='utf-8', errors='replace'), mm_log.read_text(encoding='utf-8', errors='replace'))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--tools', default=str(DEFAULT_TOOLS), help='where slippi_fake_mm.exe is')
    ap.add_argument('--exe', default=str(Path(os.environ.get('TEMP', '/tmp')) / 'slippi_online_pc/so_pc_selftest.exe'))
    args = ap.parse_args()
    tools, exe = Path(args.tools), Path(args.exe)
    if not exe.exists():
        sys.exit(f'{exe} not found: run Nintendont/kernel/slippi_online/tests/pc/build.sh')
    with tempfile.TemporaryDirectory() as tmp:
        card = Path(tmp)
        tickets = lambda mm: mm.count('<- create-ticket from')

        print("1. The server takes the client's version: a search goes out, nothing is locked")
        log, mm = run(tools, exe, card, [])
        check('a ticket reached the server', tickets(mm) == 1)
        check('its version is 3.6.4', 'appVersion=3.6.4;' in mm)
        check('not locked', 'online is locked' not in log)
        check('nothing kept on the card', not (card / STORED).exists())

        for n, where in ((2, 'ticket'), (3, 'match')):
            away = ('3.6.5', where)
            stored = card / STORED
            if stored.exists():
                stored.unlink()
            print(f"{n}a. The server turns versions under 3.6.5 away, naming it (in its {'create' if where == 'ticket' else 'get'}-ticket reply)")
            log, mm = run(tools, exe, card, [], turns_away=away)
            check('the ticket was sent, once', tickets(mm) == 1)
            check("the server's version was read", "server's latest version is 3.6.5" in log)
            check('online is locked', 'Slippi is at version 3.6.5 and this SlippWii speaks 3.6.4: online is locked' in log)
            check('the version is on the card', stored.exists() and stored.read_text().strip() == '3.6.5')

            print(f'{n}b. The next start, the server as before: locked from the card, and no ticket at all')
            log, mm = run(tools, exe, card, ['test_latest_keep=1'], turns_away=away)
            check('locked at start-up', 'online is locked' in log)
            check('no ticket was sent', tickets(mm) == 0)
            check('the search was refused here', 'Update SlippWii to play online again' in log)

            print(f'{n}c. A client that speaks 3.6.5 (an updated SlippWii), the same card: it searches again')
            log, mm = run(tools, exe, card, ['test_latest_keep=1', 'app_version=3.6.5'], turns_away=away)
            check('not locked', 'online is locked' not in log)
            check('a ticket reached the server, as 3.6.5', tickets(mm) == 1 and 'appVersion=3.6.5;' in mm)

        print('4. A server that refuses without naming a version (the 3DS fake server): its error, and no lock')
        log, mm = run(tools, exe, card, [], ['--min-version', '3.6.5'])
        check('the error reached the client', 'Your version of Slippi is out of date' in log)
        check('nothing is locked on a guess', 'online is locked' not in log)

        print("5. A test's leftover on the card does not lock a later test (it is read only when asked)")
        log, mm = run(tools, exe, card, [])
        check('not locked, a ticket sent', 'online is locked' not in log and tickets(mm) == 1)

        print('6. Wired only: a console on Wi-Fi sends no search')
        log, mm = run(tools, exe, card, ['test_wifi=1'])
        check('refused, and said so', 'Wi-Fi: online is refused' in log)
        check('no ticket was sent', tickets(mm) == 0)

        print("7. The client's name, as build metadata after the version (off unless Slippi asks for it)")
        log, mm = run(tools, exe, card, ['test_client_name=2'])
        check('the server saw it', 'appVersion=3.6.4+slippwii.' in mm)
        print("8. The client's name, as a key of its own")
        log, mm = run(tools, exe, card, ['test_client_name=1'])
        check('the ticket was taken, its version plain', tickets(mm) == 1 and 'appVersion=3.6.4;' in mm)
    print('PASS' if not failures else f'FAIL: {failures} checks')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
