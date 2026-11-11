"""The parts of Slippi's savestate regions a console savestate leaves out,
written into tools/rollback/glue.s (between "# the heap" and "# end of the
list"): sorted, merged, not overlapping (rollback.c's page classes need that).

Slippi Dolphin's own (SlippiSavestate.cpp: sound, the XFB/VI block, a few
words), and the state of the I/O drivers: on a PC the disc and ARAM finish
their transfers at once, so a savestate never falls inside one; on a Wii it
often does (Melee streams its music from the disc), and a driver rewound under
a transfer crashes or hangs the game. The rule: what the main thread writes
is rewound, what interrupts and I/O completions write is not (as Slippi
already does for most of the sound). The OS alarm queue is left out too
(its links are rebuilt after each load, rollback.c relink_alarms). Ranges
from the Melee decomp's config/GALE01/splits.txt.

    python tools/rollback/excludes.py
"""
from pathlib import Path

GLUE = Path(__file__).with_name('glue.s')

SLIPPI = [(0x804031A0, 0x804031C4), (0x80407FB4, 0x80408300), (0x80433C64, 0x80452AE4),
          (0x804A8D78, 0x804C07E0), (0x804C0980, 0x804C1F78), (0x804C28E0, 0x804C627C),
          (0x804D6030, 0x804D6034), (0x804D603C, 0x804D6040), (0x804D7218, 0x804D721C),
          (0x804D7228, 0x804D7230), (0x804D7474, 0x804D747C), (0x804D74F0, 0x804D7540),
          (0x804D7548, 0x804D754C), (0x804D7558, 0x804D757C), (0x804D7580, 0x804D758C),
          (0x804D759C, 0x804D75A0), (0x804D7720, 0x804D7724), (0x804D7740, 0x804D7748),
          (0x804D774C, 0x804D7760), (0x804D7788, 0x804D7798), (0x804D77BC, 0x804D77C0),
          (0x804D77C8, 0x804D77CC), (0x804D77D0, 0x804D77D4), (0x804D77E0, 0x804D77E4),
          (0x804DE358, 0x804DE3D8), (0x804DE7F0, 0x804DE870)]

# Written by the sound engine's interrupts: AX, HSD's synth and audio
# driver (whole: Slippi leaves out most of them), and of the game's sound
# manager (lbaudio_ax, otherwise rewound: it points at game objects) only its
# sound bank loader, which the bank loads' completions drive (fn_80026C04): its
# tables, the bank file it names, its memory counts. Rewound, a finished bank
# could be pending again, and a scene change waited for it forever.
SOUND = [(0x803BB340, 0x803BB380, 'lbaudio_ax bank loader: cur_ssm_file'),
         (0x804031A0, 0x80403240, 'AXVPB .data'),
         (0x80407D90, 0x804083B8, 'synth, axdriver .data'),
         (0x804337C4, 0x80433C24, 'lbaudio_ax bank loader: its tables'),
         (0x804A8D78, 0x804C07E0, 'AX .bss'),
         (0x804C28E0, 0x804C62A0, 'synth, axdriver .bss'),
         (0x804D6018, 0x804D6050, 'synth, axdriver .sdata'),
         (0x804D6438, 0x804D6454, 'lbaudio_ax bank loader: its memory counts'),
         (0x804D74F0, 0x804D75A0, 'AX .sbss'),
         (0x804D7720, 0x804D77F0, 'synth, axdriver .sbss')]

IO = [(0x80400C08, 0x80401048, 'dsp, dvd .data'),
      (0x80402358, 0x80402420, 'OSSerial .data'),
      (0x80402C90, 0x80402FF0, 'pad, vi .data'),
      (0x804083B8, 0x80408480, 'devcom .data'),
      (0x804A7488, 0x804A76C8, 'dvdlow, dvd, dvdqueue, fstload .bss'),
      (0x804A7DB8, 0x804A7FB8, 'OSSerial .bss'),
      (0x804A89A0, 0x804A8B38, 'pad, vi .bss'),
      (0x804C62A0, 0x804CE380, 'devcom .bss: requests, queues, relay buffers'),
      (0x804D5B80, 0x804D5BA4, 'dvd .sdata'),
      (0x804D5C30, 0x804D5C60, 'pad .sdata'),
      (0x804D6050, 0x804D6060, 'devcom .sdata'),
      (0x804D7210, 0x804D72F0, 'dsp, dvd .sbss'),
      (0x804D7358, 0x804D7360, 'the OS alarm queue (relinked after loads)'),
      (0x804D73C8, 0x804D73D8, 'OSSerial .sbss'),
      (0x804D73F0, 0x804D74F0, 'pad, vi, ai, ar, arq .sbss'),
      (0x804D77F0, 0x804D7810, 'devcom .sbss')]


def merged():
    items = sorted([(a, b, '') for a, b in SLIPPI] + SOUND + IO)
    out = []
    for a, b, c in items:
        if out and a <= out[-1][1]:
            pa, pb, pc = out[-1]
            out[-1] = (pa, max(pb, b), pc + ('; ' if pc and c else '') + c)
        else:
            out.append((a, b, c))
    return out


def main():
    text = GLUE.read_text()
    head, tail = '\t.long\t0, 0\t\t\t\t# the heap\n', '\t.long\t0, 0\t\t\t\t# end of the list'
    start, end = text.index(head) + len(head), text.index(tail)
    lines = ''.join(f'\t.long\t0x{a:08X}, 0x{b:08X}' + (f'\t\t# {c}' if c else '') + '\n' for a, b, c in merged())
    GLUE.write_text(text[:start] + lines + text[end:])
    print(f'{GLUE}: {len(merged())} excludes')


if __name__ == '__main__':
    main()
