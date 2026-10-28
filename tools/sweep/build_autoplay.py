"""Builds the sweep's autoplay Gecko code (tools/sweep/autoplay/): one C2
code at HSD_PadRenewRawStatus that plays a controller port through Slippi
Online's menus and games, following a plan of rounds.

    python tools/sweep/build_autoplay.py --plan plan.json --port 0 --out build/sweep/autoplay

plan.json: {"rounds": [[css_icon, sss_icon], ...], "lras_s": 90, "seed": 1}
(css_icon: index in Melee's CSS icon table; sss_icon: the hovered-stage
index on the stage select screen, or 255 for whatever is under the cursor).

Writes OUT.gecko.txt (Dolphin GameSettings lines) and OUT.c2.bin (the code,
for the Wii's boot codes). The code must be position independent (tools/pic_code.py).
"""
import argparse
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import pic_code  # noqa: E402

HERE = Path(__file__).resolve().parent / 'autoplay'
HOOK = 0x803769FC  # HSD_PadRenewRawStatus entry (mflr r0)


def build_blob():
    return pic_code.build(HERE / 'glue.s', [HERE / 'autoplay.c'], start='.ap_start', tail='.ap_tail')


def fill(blob, offset, plan, port, lras_s, seed):
    rounds = plan['rounds'][:64]
    data = struct.pack('>IBBBBff', 0x4155544F, port, len(rounds), lras_s, seed, 1.0, 0.5)
    data += b''.join(struct.pack('>BB', c, s) for c, s in rounds).ljust(128, b'\0')
    blob[offset:offset + len(data)] = data


def c2(code):
    if len(code) % 4:
        raise SystemExit('code is not whole instructions')
    body = bytes(code) + (b'\0' * 4 if (len(code) // 4) % 2 else bytes.fromhex('6000000000000000'))
    return struct.pack('>II', 0xC2000000 | (HOOK & 0x01FFFFFF), len(body) // 8) + body


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--plan', required=True)
    ap.add_argument('--port', type=int, default=0)
    ap.add_argument('--out', required=True)
    args = ap.parse_args()
    plan = json.loads(Path(args.plan).read_text())
    blob, syms = build_blob()
    fill(blob, syms['bot_data'], plan, args.port, plan.get('lras_s', 90), plan.get('seed', 1))
    if blob[-4:] != bytes.fromhex('7C0802A6'):
        sys.exit('the code does not end with the replaced instruction (mflr r0)')
    code = c2(blob)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    Path(str(out) + '.c2.bin').write_bytes(code)
    lines = ['$Sweep Autoplay [slippi-wii]'] + [f'{code[i:i + 4].hex().upper()} {code[i + 4:i + 8].hex().upper()}'
                                             for i in range(0, len(code), 8)]
    Path(str(out) + '.gecko.txt').write_text('\n'.join(lines) + '\n')
    print(f'autoplay: {len(code)} bytes, bot data at +{syms["bot_data"]:#x}, {len(plan["rounds"])} rounds, '
          f'port {args.port}')


if __name__ == '__main__':
    main()
