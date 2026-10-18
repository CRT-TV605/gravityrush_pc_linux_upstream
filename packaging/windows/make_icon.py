#!/usr/bin/env python3
"""Builds the port's icon (launcher/gravityrush.ico, .png and .bmp) from packaging/windows/icon_source.png:
the whole picture at every size, nothing of it cut. A transparent background stays transparent, and
its empty margins are trimmed so the subject is centred in a square with a small border. A new
picture without transparency (a full illustration) becomes a square tile with rounded corners.
Needs Pillow.

Usage: python packaging/windows/make_icon.py [new source image]   (a new image replaces icon_source.png)
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'packaging' / 'windows' / 'icon_source.png'
SIZES = (16, 24, 32, 48, 64, 128, 256)
MARGIN = 0.04   # border around the subject, as a fraction of the side
ROUND = 0.18    # corner radius of an opaque picture's tile, as a fraction of its side


def fit_square(image):
    """The subject (non-transparent pixels) centred in a transparent square with a small border."""
    box = image.getchannel('A').getbbox() or (0, 0, image.width, image.height)
    subject = image.crop(box)
    side = round(max(subject.size) / (1 - 2 * MARGIN))
    canvas = Image.new('RGBA', (side, side), (0, 0, 0, 0))
    canvas.paste(subject, ((side - subject.width) // 2, (side - subject.height) // 2), subject)
    return canvas


def rounded_tile(image):
    """An opaque picture as a square tile (centre crop) with rounded, anti-aliased corners."""
    side = min(image.size)
    left, top = (image.width - side) // 2, (image.height - side) // 2
    tile = image.crop((left, top, left + side, top + side)).convert('RGBA')
    big = side * 4  # the mask is drawn larger, then reduced: smooth corners
    mask = Image.new('L', (big, big), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, big - 1, big - 1), round(big * ROUND), fill=255)
    tile.putalpha(mask.resize((side, side), Image.LANCZOS))
    return tile


def main():
    if len(sys.argv) > 1:
        picture = Image.open(sys.argv[1]).convert('RGBA')
        if picture.getchannel('A').getextrema()[0] == 255:  # no transparency: a full illustration
            picture = rounded_tile(picture)
        fit_square(picture).resize((512, 512), Image.LANCZOS).save(SOURCE, optimize=True)
    full = fit_square(Image.open(SOURCE).convert('RGBA'))
    frames = [full.resize((size, size), Image.LANCZOS) for size in SIZES]
    frames[-1].save(ROOT / 'launcher' / 'gravityrush.png')
    # The game window's icon: a BMP SDL loads by itself (run.py then needs no Pillow).
    full.resize((64, 64), Image.LANCZOS).save(ROOT / 'launcher' / 'gravityrush.bmp')
    frames[-1].save(ROOT / 'launcher' / 'gravityrush.ico', sizes=[(s, s) for s in SIZES],
                    append_images=frames[:-1], bitmap_format='png')
    print('launcher/gravityrush.ico, launcher/gravityrush.png, launcher/gravityrush.bmp')


if __name__ == '__main__':
    main()
