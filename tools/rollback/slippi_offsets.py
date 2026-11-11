"""Offsets in Slippi's online data, read from its own source (slippi-ssbm-asm/
Online/Online.s) so the engine's copy cannot drift from it: -D defines for
rollback.c.
"""
import re
from pathlib import Path

ONLINE_S = Path(__file__).resolve().parents[2] / 'slippi-ssbm-asm/Online/Online.s'
WANTED = ('OFST_R13_ODB_ADDR', 'ODB_INPUT_SOURCE_INDEX', 'ODB_LAST_LOCAL_INPUTS', 'ODB_DELAY_FRAMES',
          'ODB_ROLLBACK_END_FRAME', 'TXB_FRAME', 'TXB_DELAY', 'TXB_PAD', 'TXB_SIZE', 'PAD_REPORT_SIZE',
          'ODB_FRAME', 'ODB_FINALIZED_FRAME', 'ODB_STABLE_FINALIZED_FRAME', 'ODB_SAVESTATE_FRAME',
          'ODB_STABLE_SAVESTATE_FRAME', 'ODB_PLAYER_SAVESTATE_FRAME', 'ODB_SAVESTATE_IS_PREDICTING',
          'ODB_PLAYER_SAVESTATE_IS_PREDICTING', 'ODB_ROLLBACK_IS_ACTIVE', 'ODB_ROLLBACK_SHOULD_LOAD_STATE',
          'ODB_SHOULD_FORCE_PAD_RENEW', 'ODB_STABLE_ROLLBACK_IS_ACTIVE', 'ODB_STABLE_ROLLBACK_END_FRAME', 'ODB_IS_FRAME_ADVANCE',
          'SSCB_WRITE_INDEX', 'SSCB_SSDB_START', 'SSDB_FRAME', 'SSDB_SIZE', 'ROLLBACK_MAX_FRAME_COUNT')


def constants():
    """{name: value} of the .set lines whose values are constant expressions."""
    values = {}
    for line in ONLINE_S.read_text(encoding='utf-8').splitlines():
        m = re.match(r'\s*\.set\s+(\w+)\s*,\s*([^#]+)', line)
        if m:
            try:
                values[m.group(1)] = eval(m.group(2), {'__builtins__': {}}, values)
            except (NameError, SyntaxError):
                pass  # (uses something from another file: not one of ours)
    return values


def defines():
    values = constants()
    return [f'{name}={values[name]}' for name in WANTED]


if __name__ == '__main__':
    print('\n'.join(defines()))
