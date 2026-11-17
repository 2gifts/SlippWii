"""Which Slippi this checkout builds for: Slippi 3.6.4, or the version after it.

The one switch is SO_SLIPPI_NEXT in Nintendont/kernel/slippi_online/so_config.h (the kernel's
defaults follow it there). The tools read it here: the boot codes (the rollback engine is built for
one version's codes: tools/build_online_boot.py), Slippi's files and codes in a release
(tools/make_release.py), and the test harness (tools/sweep), which runs a switched checkout against
Slippi's development Dolphin only. docs/NEXT_SLIPPI.md has the steps for the day Slippi releases.

    python tools/slippi_version.py            # says which
    python tools/slippi_version.py next       # switches the checkout (then: python tools/build_online_boot.py,
    python tools/slippi_version.py stable     #  sh tools/build.sh, the tests of docs/NEXT_SLIPPI.md, and commit)
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / 'Nintendont/kernel/slippi_online/so_config.h'
LINE = re.compile(r'^(#define SO_SLIPPI_NEXT )([01])[ \t]*$', re.M)


def next_release():
    """True in a checkout switched to the Slippi after 3.6.4."""
    found = LINE.findall(HEADER.read_text(encoding='utf-8'))
    if len(found) != 1:
        sys.exit(f'{HEADER}: no "#define SO_SLIPPI_NEXT 0" (or 1) line, or more than one')
    return found[0][1] == '1'


def version_default():
    """The Slippi version a build of this checkout reports (SO_APP_VERSION_DEFAULT)."""
    text = HEADER.read_text(encoding='utf-8')
    block = re.search(r'#if SO_SLIPPI_NEXT\n#define SO_APP_VERSION_DEFAULT "([^"]+)".*\n#else\n'
                      r'#define SO_APP_VERSION_DEFAULT "([^"]+)"', text)
    if not block:
        sys.exit(f'{HEADER}: SO_APP_VERSION_DEFAULT is not where tools/slippi_version.py looks for it')
    return block.group(1 if next_release() else 2)


def switch(to_next):
    raw = HEADER.read_bytes().decode('utf-8')
    if len(LINE.findall(raw.replace('\r\n', '\n'))) != 1:
        sys.exit(f'{HEADER}: no "#define SO_SLIPPI_NEXT 0" (or 1) line, or more than one')
    crlf = '\r\n' in raw
    text = LINE.sub(lambda m: m.group(1) + ('1' if to_next else '0'), raw.replace('\r\n', '\n'))
    HEADER.write_bytes((text.replace('\n', '\r\n') if crlf else text).encode('utf-8'))


def main():
    if len(sys.argv) > 2 or (len(sys.argv) == 2 and sys.argv[1] not in ('next', 'stable')):
        sys.exit(__doc__)
    if len(sys.argv) == 2:
        switch(sys.argv[1] == 'next')
    print(f'this checkout builds for Slippi {version_default()}'
          + (' (the version after 3.6.4: SO_SLIPPI_NEXT 1)' if next_release() else ''))
    if len(sys.argv) == 2:
        print('now: python tools/build_online_boot.py (the committed boot codes follow the switch), then sh tools/build.sh')


if __name__ == '__main__':
    main()
