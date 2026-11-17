"""The kernel's layouts of the game's online buffers against Slippi's own source.

    python tools/check_exi_layout.py

The game's codes (slippi-ssbm-asm, Online/Online.s) say where each field of the 0xB0 request (TXB),
its answer (RXB) and the 0xB3 answer (MSRB) is; the kernel's EXI device
(Nintendont/kernel/slippi_online/so_exi.c) has the same numbers under its own names, for Slippi
3.6.4 and for the version after it. This holds the two to each other and fails on any that differ
(tools/build.sh runs it): a field read one byte off plays every test whose value happens to be 0
there. That was "is leaving" read from the controller's port, 2026-11-16: fine from port 1, the
only port the test bot had played from, and the end of every game from any other.

The next version's codes are a second clone, slippi-ssbm-asm-next (docs/NEXT_SLIPPI.md); without it
only 3.6.4's numbers are checked, and this says so.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from rollback import slippi_offsets  # noqa: E402

SO_EXI = ROOT / 'Nintendont/kernel/slippi_online/so_exi.c'
# (the kernel's name, Slippi's name, the version it is of: None for both)
PAIRS = (
    ('TXB_DELAY', 'TXB_DELAY', None),
    ('TXB_PAD', 'TXB_PAD', None),
    ('TXB_SIZE_364', 'TXB_SIZE', False),
    ('TXB_SIZE_NEXT', 'TXB_SIZE', True),
    ('TXB_INPUT_SOURCE_INDEX', 'TXB_INPUT_SOURCE_INDEX', True),
    ('TXB_IS_LEAVING', 'TXB_IS_LEAVING', True),
    ('RXB_SIZE_364', 'RXB_SIZE', False),
    ('RXB_SIZE_NEXT', 'RXB_SIZE', True),
    ('RXB_DESPAWN', 'RXB_DESPAWN_FRAMES', True),
    ('RXB_PREDICTED', 'RXB_OPNT_PREDICTED_INPUTS', True),
    ('RXB_NEXT_RNG', 'RXB_NEXT_PAD_RNG_BITS', True),
    ('MSRB_SIZE_364', 'MSRB_SIZE', False),
    ('MSRB_SIZE_NEXT', 'MSRB_SIZE', True),
    ('MSRB_IS_BOT', 'MSRB_IS_BOT', True),
)


def kernel_defines():
    found = {}
    for m in re.finditer(r'^#define[ \t]+(\w+)[ \t]+(0[xX][0-9A-Fa-f]+|\d+)\b', SO_EXI.read_text(encoding='utf-8'), re.M):
        found[m.group(1)] = int(m.group(2), 0)
    return found


def main():
    ours = kernel_defines()
    have_next = slippi_offsets.NEXT_ONLINE_S.is_file()
    theirs = {False: slippi_offsets.constants(False), True: slippi_offsets.constants(True) if have_next else None}
    wrong, checked = [], 0
    for name, slippi, version in PAIRS:
        if name not in ours:
            wrong.append(f'{name}: not a #define of {SO_EXI.name} any more (tools/check_exi_layout.py looks for it)')
            continue
        for which in ((False, True) if version is None else (version,)):
            if theirs[which] is None:
                continue
            if slippi not in theirs[which]:
                wrong.append(f"{slippi}: not in Slippi's {'next ' if which else ''}Online.s")
            elif theirs[which][slippi] != ours[name]:
                wrong.append(f"{name} is {ours[name]} in {SO_EXI.name}; Slippi's {'next version' if which else '3.6.4'} "
                             f'has {slippi} = {theirs[which][slippi]}')
            checked += 1
    if wrong:
        sys.exit('the EXI layouts are not Slippi\'s:\n  ' + '\n  '.join(wrong))
    print(f"so_exi.c: {checked} offsets and sizes are Slippi's own"
          + ('' if have_next else ' (3.6.4 only: no slippi-ssbm-asm-next checkout for the next version)'))


if __name__ == '__main__':
    main()
