#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Generate the Ambience icon in the vendor home-screen style.

Same treatment as the fake-engine icon: mint-cyan glyph with a soft glow and
solid, rounded strokes, drawn for the tile's Q_ThemeImage plate.  Our own art.
"""
import os
from PIL import Image, ImageDraw

SIZE = 99
OUT = os.path.join(os.path.dirname(__file__), "..", "artifacts", "assets", "ambient")

# Same palette as the older SCOURGE tiles (battery/wifi/tweaks/reboot use
# (47,111,237) blue line art), so the six home-screen tiles read as one set
# instead of two mint glyphs beside four blue ones (2026-11-24).
MINT = (104, 236, 222)
DIM = (120, 140, 140)


def glyph(d, color, width):
    cx, cy = SIZE / 2, SIZE / 2 + 4
    # bulb: filled head with a short neck, the way the stock glyphs read
    d.ellipse([cx - 21, cy - 21, cx + 21, cy + 21], fill=color)
    d.rectangle([cx - 9, cy + 16, cx + 9, cy + 26], fill=color)
    # rays around the head, rounded caps
    for dx, dy in ((0, -34), (-30, -22), (30, -22), (-36, 2), (36, 2)):
        x1, y1 = cx + dx, cy + dy
        x2, y2 = cx + dx * 0.72, cy + dy * 0.72
        d.line([x1, y1, x2, y2], fill=color, width=width)


def make(color):
    img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    for extra, alpha in ((9, 18), (6, 30), (3, 55)):
        layer = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
        glyph(ImageDraw.Draw(layer), (color[0], color[1], color[2], alpha), 5 + extra)
        img = Image.alpha_composite(img, layer)
    solid = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    glyph(ImageDraw.Draw(solid), (color[0], color[1], color[2], 255), 5)
    return Image.alpha_composite(img, solid)


def main():
    os.makedirs(OUT, exist_ok=True)
    make(MINT).save(os.path.join(OUT, "ambient_icon_n.png"))
    make(DIM).save(os.path.join(OUT, "ambient_icon_d.png"))
    print("wrote", OUT)


if __name__ == "__main__":
    main()
