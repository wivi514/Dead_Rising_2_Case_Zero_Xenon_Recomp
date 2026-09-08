#!/usr/bin/env python3
"""Draw the AppImage / desktop icon for the Linux release (part 104).

WHY THIS EXISTS. An AppImage needs an icon and a .desktop entry, and this repository
ships NO Capcom art by rule (release-plan §2.1: nothing game-derived leaves the
recompiled image). So the icon is ours: a dark rounded tile with the two letters,
drawn here from a system font so it can be regenerated identically rather than kept as
an opaque binary nobody can vouch for. Dev-only (PIL); the PNG it writes is what ships.

    python3 tools/release/make_icon.py            -> tools/release/icon/cz_runtime.png
"""
import os
import sys

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.exit("PIL is needed to regenerate the icon (pip install pillow); the checked-in PNG is what ships")

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "icon", "cz_runtime.png")
SIZE = 256

FONTS = [
    "/usr/share/fonts/google-noto/NotoSans-Bold.ttf",
    "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Bold.ttf",
    "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
]


def main():
    img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    # tile: near-black with a warning-orange rim, the palette of a hazard sign rather than
    # of anything the game owns
    d.rounded_rectangle((8, 8, SIZE - 9, SIZE - 9), radius=48, fill=(24, 24, 28, 255),
                        outline=(232, 120, 24, 255), width=10)
    font = None
    for f in FONTS:
        if os.path.exists(f):
            font = ImageFont.truetype(f, 132)
            break
    if font is None:
        font = ImageFont.load_default()
    text = "CZ"
    box = d.textbbox((0, 0), text, font=font)
    w, h = box[2] - box[0], box[3] - box[1]
    x = (SIZE - w) // 2 - box[0]
    y = (SIZE - h) // 2 - box[1] - 6
    d.text((x + 3, y + 4), text, font=font, fill=(0, 0, 0, 160))
    d.text((x, y), text, font=font, fill=(240, 240, 236, 255))
    # the "0" of Case Zero as a thin ring under the letters
    d.ellipse((SIZE // 2 - 34, SIZE - 66, SIZE // 2 + 34, SIZE - 30), outline=(232, 120, 24, 255), width=6)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    img.save(OUT, optimize=True)
    print(f"wrote {OUT} ({os.path.getsize(OUT)} bytes, {SIZE}x{SIZE})")


if __name__ == "__main__":
    main()
