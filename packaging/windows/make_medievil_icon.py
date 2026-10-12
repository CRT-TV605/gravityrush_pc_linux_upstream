#!/usr/bin/env python3
"""Draws the port's icon (launcher/medievil.ico and .png): an original tombstone under a ghostly
green glow on a night-purple disc. No game artwork is used. Needs Pillow."""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parents[2]
S = 1024  # drawn large, scaled down for each icon size


def draw():
    image = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(image)
    d.ellipse((24, 24, S - 24, S - 24), fill=(24, 18, 38, 255), outline=(70, 60, 96, 255), width=18)
    # Green glow behind the stone.
    glow = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse((250, 170, 774, 700), fill=(110, 210, 60, 170))
    image.alpha_composite(glow.filter(ImageFilter.GaussianBlur(90)))
    # Tombstone: rounded top, slab body, grass line.
    stone, edge = (150, 146, 160, 255), (96, 92, 108, 255)
    d.rectangle((340, 420, 684, 820), fill=stone)
    d.ellipse((340, 250, 684, 594), fill=stone)
    d.line((340, 420, 340, 820), fill=edge, width=14)
    d.line((684, 420, 684, 820), fill=edge, width=14)
    d.arc((340, 250, 684, 594), 180, 360, fill=edge, width=14)
    # A cross carved into the stone.
    carve = (78, 74, 92, 255)
    d.rectangle((492, 360, 532, 640), fill=carve)
    d.rectangle((420, 440, 604, 480), fill=carve)
    # Graveyard ground.
    d.chord((150, 760, 874, 1060), 180, 360, fill=(38, 74, 30, 255))
    mask = Image.new('L', (S, S), 0)
    ImageDraw.Draw(mask).ellipse((24, 24, S - 24, S - 24), fill=255)
    out = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    out.paste(image, (0, 0), mask)
    return out


def main():
    icon = draw()
    icon.resize((256, 256), Image.LANCZOS).save(ROOT / 'launcher' / 'medievil.png')
    icon.save(ROOT / 'launcher' / 'medievil.ico', sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    print('launcher/medievil.ico, launcher/medievil.png')


if __name__ == '__main__':
    main()
