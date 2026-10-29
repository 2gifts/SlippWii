"""Names Melee 1.02 addresses from the Wii's diagnostics log.

Melee's own functions come from the decomp's symbol map; addresses inside
the online codeset (loaded by Slippi's bootloader at the address the log's
"codeset loaded ... for XXXXXXXX" line gives) are named by the Slippi
source of the code block they fall in.

    python tools/so_symbolize.py --gct-base 8065cc80 80375380 8065d000 ...
    python tools/so_symbolize.py --gct-base 8065cc80 --log build/online-selftest/<run>/wii.log
"""
import argparse
import bisect
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
import slippi_files as builder  # noqa: E402

SYMBOLS = ROOT.parent / 'slippi-3ds/config/GALE01/symbols.txt'


def load_symbols():
    starts, names = [], []
    pattern = re.compile(r'^(\S+) = \.text:0x([0-9A-Fa-f]{8}); // type:function size:0x([0-9A-Fa-f]+)')
    for line in SYMBOLS.read_text(encoding='utf-8', errors='replace').splitlines():
        m = pattern.match(line)
        if m:
            starts.append(int(m.group(2), 16))
            names.append((m.group(1), int(m.group(3), 16)))
    order = sorted(range(len(starts)), key=lambda i: starts[i])
    return [starts[i] for i in order], [names[i] for i in order]


def codeset_blocks():
    """(start offset, end offset, source) of each code block in netplay.gct."""
    ini = (builder.CACHE / 'GALE01r2-stable.ini').read_text(encoding='utf-8')
    blocks, off, last = [], 8, ''
    for _, lines in builder.parse_ini(ini):
        for note, block in builder.code_blocks(lines):
            last = note or last
            if any(note.startswith(d) for d in builder.DROPPED):
                continue
            size = 8 * len(block)
            blocks.append((off, off + size, last))
            off += size
    return blocks


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--gct-base', type=lambda v: int(v, 16), help='where the bootloader loaded the codeset')
    ap.add_argument('--log', help='name every address in this log')
    ap.add_argument('addresses', nargs='*')
    args = ap.parse_args()
    starts, names = load_symbols()
    blocks = codeset_blocks() if args.gct_base else []

    def name(address):
        if args.gct_base and args.gct_base <= address < args.gct_base + 0x10000:
            off = address - args.gct_base
            for start, end, note in blocks:
                if start <= off < end:
                    return f'codeset+{off:#x} in {note}'
            return f'codeset+{off:#x}'
        i = bisect.bisect_right(starts, address) - 1
        if i >= 0 and address < starts[i] + names[i][1]:
            return f'{names[i][0]}+{address - starts[i]:#x}'
        return '?'

    addresses = [int(a, 16) for a in args.addresses]
    if args.log:
        text = Path(args.log).read_text(encoding='utf-8', errors='replace')
        addresses += sorted({int(a, 16) for a in re.findall(r'\b(80[0-9a-fA-F]{6})\b', text)})
    for a in addresses:
        print(f'{a:08x}  {name(a)}')


if __name__ == '__main__':
    main()
