"""SlippWii's own changes to Slippi's menu files: the Online Play menu's "Exit".

Slippi's Online Play menu has an option SlippWii has no use for: "Log-out", which Slippi itself
no longer shows. SlippWii shows it in every state, as "Exit": the same place in the list, Slippi's
own "Are you sure?" box, and then back to the Homebrew Channel (tools/ppc/online_menu.s shows
the option, the kernel takes Slippi's log-out command as the exit, tools/ppc/exit.s leaves).
"Update", which Slippi shows alone when its server wants a newer version, leaves the same way:
a console is updated from SlippWii's own start menu.

What has to change in Slippi's files for that is small, and the same in the English (.usd) and
the Japanese (.dat) set:

    MnMaAll   the option's name, a 176x30 picture (IA4): "Log-out" -> "Exit"
    SdMenu    the line under the menu for "Log-out": "Switch to a different user."
                  -> "Return to the Homebrew Channel."
              and for "Update": "Required update available."
                  -> "Exit and choose Check for Updates."

Nothing of Slippi's or Melee's is shipped for it: the picture is SlippWii's own drawing, in the
style of Slippi's own option names (tools/menu_exit_label.png; white letters, a black outline two
pixels wide), and the lines are text. The loader puts them into the files it has made from the
disc and Slippi's patches (Nintendont/loader/source/slippwii.c, slippwii_edits.h): each place is
changed only if it holds exactly what Slippi's file has there, so a start with the change already
made does nothing, and any other file is left as it is.

    python tools/menu_edits.py                  writes Nintendont/loader/source/slippwii_edits.h
    python tools/menu_edits.py --check          the header is what this file would write
    python tools/menu_edits.py --files DIR      Slippi's files in DIR are what the table expects
    python tools/menu_edits.py --apply DIR      changes the files in DIR (as the loader would)
    python tools/menu_edits.py --draw           draws the picture again (Windows' Arial Black)

tools/slippi_files.py applies them to the files it makes on a PC.
"""
import argparse
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LABEL_PNG = ROOT / 'tools/menu_exit_label.png'
HEADER = ROOT / 'Nintendont/loader/source/slippwii_edits.h'
LABEL_W, LABEL_H = 176, 30

EXIT_LINE = 'Return to the Homebrew Channel.'
UPDATE_LINE = 'Exit and choose Check for Updates.'

# Where, in Slippi's files (the release tools/slippi_files.py pins): the file's offset, the
# length and the CRC-32 of what Slippi has there. The same in the .usd and the .dat.
#   (name without its extension, offset, length, CRC-32 of Slippi's bytes, what goes there)
PLACES = (
    ('MnMaAll', 0x42C0, 5632, 0x2A917E8B, 'label'),    # the "Log-out" picture (30 rows, stored as 32)
    ('SdMenu', 0x00E0, 96, None, 'exit_line'),         # text 0x649, with the room after it
    ('SdMenu', 0x0080, 96, None, 'update_line'),       # text 0x64A
)
OLD_LINES = {'exit_line': 'Switch to a different user.', 'update_line': 'Required update available.'}
EXTENSIONS = ('usd', 'dat')


def sis_text(line, room=96):
    """A menu description line as Slippi's are stored: gray, fitted to the box, then the letters
    (two bytes each: 0x20 and digits 0-9, A-Z, a-z in that order; 0x1A a space; 0x20E7 a period)."""
    out = bytearray.fromhex('100CAAAAAA0E00B300B316060000000018')
    for ch in line:
        if ch == ' ':
            out.append(0x1A)
        elif ch == '.':
            out += b'\x20\xE7'
        elif ch.isdigit():
            out += bytes((0x20, ord(ch) - ord('0')))
        elif 'A' <= ch <= 'Z':
            out += bytes((0x20, 0x0A + ord(ch) - ord('A')))
        elif 'a' <= ch <= 'z':
            out += bytes((0x20, 0x24 + ord(ch) - ord('a')))
        else:
            raise SystemExit(f'no code known for {ch!r} in a menu line')
    out += bytes.fromhex('190F0D00')
    if len(out) > room:
        raise SystemExit(f'{line!r} needs {len(out)} bytes, and its place has {room}')
    return bytes(out) + bytes(room - len(out))


def ia4(png):
    """The picture as the game stores it: 8x4 blocks, a byte a pixel (alpha, brightness; 4 bits each)."""
    from PIL import Image
    im = Image.open(png).convert('RGBA')
    if im.size != (LABEL_W, LABEL_H):
        raise SystemExit(f'{png} is {im.size}, not {LABEL_W}x{LABEL_H}')
    px = im.load()
    out = bytearray()
    for by in range(0, (LABEL_H + 3) // 4 * 4, 4):
        for bx in range(0, LABEL_W, 8):
            for y in range(by, by + 4):
                for x in range(bx, bx + 8):
                    r, _, _, a = px[x, y] if y < LABEL_H else (0, 0, 0, 0)
                    out.append((a + 8) // 17 << 4 | (r + 8) // 17)
    return bytes(out)


def draw():
    """SlippWii's "Exit", drawn as Slippi's option names are: Arial Black a little narrowed, cap
    height 19 pixels, and every point within two pixels of a letter black. The numbers are the
    best fit to Slippi's seven names."""
    import numpy as np
    from PIL import Image, ImageDraw, ImageFilter, ImageFont
    ss, size, squeeze, thin, baseline, outline = 8, 27.5, 0.86, 1, 24.5, 2.0
    font = ImageFont.truetype('C:/Windows/Fonts/ariblk.ttf', int(round(size * ss)))
    tmp = Image.new('L', (LABEL_W * ss * 3, LABEL_H * ss * 3), 0)
    ImageDraw.Draw(tmp).text((100, 400), 'Exit', font=font, fill=255, anchor='ls')
    box = tmp.getbbox()
    glyphs = tmp.crop((box[0], 0, box[2], tmp.size[1]))
    width = int(round(glyphs.size[0] * squeeze))
    glyphs = glyphs.resize((width, glyphs.size[1]), Image.LANCZOS)
    big = Image.new('L', (LABEL_W * ss, LABEL_H * ss), 0)
    big.paste(glyphs, ((LABEL_W * ss - width) // 2, int(round(baseline * ss)) - 400))
    big = big.filter(ImageFilter.MinFilter(thin * 2 + 1))   # an eighth of a pixel lighter
    fill = np.asarray(big) >= 128
    r = int(round(outline * ss))
    around = np.zeros_like(fill)
    sums = np.cumsum(np.pad(fill, ((0, 0), (1, 0))), axis=1)
    h, w = fill.shape
    for dy in range(-r, r + 1):
        reach = int((r * r - dy * dy) ** 0.5)
        lo, hi = np.clip(np.arange(w) - reach, 0, w), np.clip(np.arange(w) + reach + 1, 0, w)
        row = (sums[:, hi] - sums[:, lo]) > 0
        if dy >= 0:
            around[dy:, :] |= row[:h - dy, :]
        else:
            around[:h + dy, :] |= row[-dy:, :]

    def small(mask):
        return np.rint(mask.reshape(LABEL_H, ss, LABEL_W, ss).mean(axis=(1, 3)) * 15).astype(np.uint8) * 17
    rgba = np.zeros((LABEL_H, LABEL_W, 4), np.uint8)
    rgba[..., 0] = rgba[..., 1] = rgba[..., 2] = small(fill)
    rgba[..., 3] = small(around)
    Image.fromarray(rgba, 'RGBA').save(LABEL_PNG, optimize=True)
    print(f'{LABEL_PNG}: drawn')


def new_bytes():
    return {'label': ia4(LABEL_PNG), 'exit_line': sis_text(EXIT_LINE), 'update_line': sis_text(UPDATE_LINE)}


def edits():
    """(file, offset, length, CRC-32 of Slippi's bytes, CRC-32 of SlippWii's, what, SlippWii's bytes)"""
    new = new_bytes()
    out = []
    for stem, offset, length, old_crc, what in PLACES:
        if old_crc is None:
            old_crc = zlib.crc32(sis_text(OLD_LINES[what], length))
        if len(new[what]) != length:
            raise SystemExit(f'{what} is {len(new[what])} bytes, and its place has {length}')
        for ext in EXTENSIONS:
            out.append((f'{stem}.{ext}', offset, length, old_crc, zlib.crc32(new[what]), what, new[what]))
    return out


def header_text():
    new = new_bytes()
    lines = ['/* Generated by tools/menu_edits.py (which says what this is): do not edit. */',
             '#ifndef SLIPPWII_EDITS_H', '#define SLIPPWII_EDITS_H', '',
             'typedef struct {',
             '\tconst char *file;\t/* in /slippwii/files/ */',
             '\tu32 offset, size;',
             "\tu32 old_crc, new_crc;\t/* CRC-32 of Slippi's bytes there, and of SlippWii's */",
             '\tconst u8 *data;', '} SlippWiiEdit;', '']
    for what in ('label', 'exit_line', 'update_line'):
        data = new[what]
        lines.append(f'static const u8 slippwii_edit_{what}[{len(data)}] = {{')
        for i in range(0, len(data), 16):
            lines.append('\t' + ' '.join(f'0x{b:02x},' for b in data[i:i + 16]))
        lines += ['};', '']
    lines.append('static const SlippWiiEdit slippwii_edits[] = {')
    for name, offset, length, old_crc, new_crc, what, _ in edits():
        lines.append(f'\t{{ "{name}", 0x{offset:x}, {length}, 0x{old_crc:08x}, 0x{new_crc:08x}, slippwii_edit_{what} }},')
    lines += ['};', '#define SLIPPWII_EDIT_COUNT (sizeof(slippwii_edits) / sizeof(slippwii_edits[0]))', '',
              '#endif', '']
    return '\n'.join(lines)


def state(path, offset, length, old_crc, new_crc):
    """'old' (Slippi's bytes), 'new' (SlippWii's), 'other' or 'missing'."""
    if not path.exists():
        return 'missing'
    with open(path, 'rb') as f:
        f.seek(offset)
        crc = zlib.crc32(f.read(length))
    return 'old' if crc == old_crc else 'new' if crc == new_crc else 'other'


def apply(files_dir, log=None):
    """Changes Slippi's files in files_dir as the loader does. Returns how many places it changed."""
    changed = 0
    for name, offset, length, old_crc, new_crc, what, data in edits():
        path = Path(files_dir) / name
        was = state(path, offset, length, old_crc, new_crc)
        if was == 'old':
            with open(path, 'r+b') as f:
                f.seek(offset)
                f.write(data)
            changed += 1
        if log:
            log(f'  {name} {what}: ' + {'old': 'changed', 'new': 'already changed', 'other': 'not Slippi\'s: left',
                                       'missing': 'no such file'}[was])
    return changed


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--check', action='store_true', help='the header is what this file would write')
    ap.add_argument('--files', help="Slippi's files in this folder are what the table expects")
    ap.add_argument('--apply', help='change the files in this folder')
    ap.add_argument('--draw', action='store_true', help="draw the picture again (needs Windows' Arial Black)")
    args = ap.parse_args()
    if args.draw:
        draw()
    if args.files:
        bad = 0
        for name, offset, length, old_crc, new_crc, what, _ in edits():
            was = state(Path(args.files) / name, offset, length, old_crc, new_crc)
            print(f'{name} {what} at 0x{offset:x}: {was}')
            bad += was in ('other', 'missing')
        sys.exit(1 if bad else 0)
    if args.apply:
        print(apply(args.apply, print), 'places changed')
        return
    text = header_text()
    if args.check:
        if HEADER.read_text() != text:
            sys.exit(f'{HEADER} is not what tools/menu_edits.py writes: run it')
        print(f'{HEADER.name} is current')
        return
    HEADER.write_text(text, newline='\n')
    print(f'{HEADER}: {len(edits())} places')


if __name__ == '__main__':
    main()
