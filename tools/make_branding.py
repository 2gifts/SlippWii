"""Draws SlippWii's own loader background and Homebrew Channel icon (plain
text, no Slippi or Nintendo logos: SlippWii is unofficial).

    python tools/make_branding.py

Writes Nintendont/loader/data/background.png (640x480, the loader's
background, built into boot.dol) and release/apps/SlippWii/icon.png
(128x48, the Homebrew Channel icon). Needs Pillow and Windows' Bahnschrift
font.
"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
FONT = 'C:/Windows/Fonts/bahnschrift.ttf'
GRAY = (221, 221, 221, 255)       # Slippi Nintendont's background gray
INK = (255, 255, 255, 255)
GREEN = (33, 186, 69, 255)        # Slippi-like green, for the "Wii"


def font(size, weight='Bold'):
    f = ImageFont.truetype(FONT, size)
    try:
        f.set_variation_by_name(weight)
    except (OSError, ValueError):
        pass
    return f


def background():
    im = Image.new('RGBA', (640, 480), GRAY)
    d = ImageDraw.Draw(im)
    big, small = font(30), font(15, 'SemiBold')
    # bottom right, where Slippi Nintendont has its logo
    right = 568
    w1 = d.textlength('SLIPP', font=big)
    w2 = d.textlength('WII', font=big)
    x = right - w1 - w2
    d.text((x, 376), 'SLIPP', font=big, fill=INK)
    d.text((x + w1, 376), 'WII', font=big, fill=GREEN)
    for i, line in enumerate(('unofficial', 'Slippi Online')):
        d.text((right - d.textlength(line, font=small), 414 + i * 17), line, font=small, fill=INK)
    out = ROOT / 'Nintendont/loader/data/background.png'
    im.save(out)
    return out


def icon():
    im = Image.new('RGBA', (128, 48), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((0, 0, 127, 47), radius=8, fill=(16, 16, 16, 255))
    big = font(26)
    w1 = d.textlength('Slipp', font=big)
    w2 = d.textlength('Wii', font=big)
    x = (128 - w1 - w2) / 2
    d.text((x, 4), 'Slipp', font=big, fill=INK)
    d.text((x + w1, 4), 'Wii', font=big, fill=GREEN)
    small = font(10, 'SemiBold')
    line = 'unofficial Slippi Online'
    d.text(((128 - d.textlength(line, font=small)) / 2, 33), line, font=small, fill=(170, 170, 170, 255))
    out = ROOT / 'release/apps/SlippWii/icon.png'
    out.parent.mkdir(parents=True, exist_ok=True)
    im.save(out)
    return out


if __name__ == '__main__':
    for path in (background(), icon()):
        print(f'wrote {path}')
