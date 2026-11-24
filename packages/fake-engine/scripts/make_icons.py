#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Generate the fake-engine icon in the vendor home-screen style.

The stock tiles (Map, Navigation, Phone) are a rounded plate with a bright
mint-cyan glyph and a soft glow - solid shapes, rounded caps, no thin outlines.
These icons are only the GLYPH: the plate comes from the tile's
Q_ThemeImage background.  Our own drawing, nothing traced.
"""
import math
import os
from PIL import Image, ImageDraw

SIZE = 99
OUT = os.path.join(os.path.dirname(__file__), "..", "artifacts", "assets", "fake-engine")

# Match the other SCOURGE tiles: (47,111,237) blue line art (2026-11-24).
MINT = (104, 236, 222)
DIM = (120, 140, 140)


def glyph(d, color, width):
    """Speedometer: filled arc, six ticks, needle at 3/4, round hub."""
    cx, cy, r = SIZE / 2, SIZE / 2 + 10, 30
    box = [cx - r, cy - r, cx + r, cy + r]
    d.arc(box, start=155, end=385, fill=color, width=width)
    for k in range(7):
        a = math.radians(155 + k * 230.0 / 6.0)
        x1 = cx + (r - 10) * math.cos(a)
        y1 = cy + (r - 10) * math.sin(a)
        x2 = cx + (r - 3) * math.cos(a)
        y2 = cy + (r - 3) * math.sin(a)
        d.line([x1, y1, x2, y2], fill=color, width=max(2, width - 2))
    na = math.radians(155 + 0.75 * 230.0)
    tip = (cx + (r - 9) * math.cos(na), cy + (r - 9) * math.sin(na))
    d.line([cx, cy, tip[0], tip[1]], fill=color, width=width + 1)
    d.ellipse([cx - 7, cy - 7, cx + 7, cy + 7], fill=color)
    # plate/v8 hint under the gauge, rounded caps
    d.line([cx - 15, cy + 24, cx + 15, cy + 24], fill=color, width=width)


def make(color):
    img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    # glow: three passes, each wider and fainter, then the solid glyph
    for extra, alpha in ((9, 26), (6, 40), (3, 70)):
        layer = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
        glyph(ImageDraw.Draw(layer), (color[0], color[1], color[2], alpha), 5 + extra)
        img = Image.alpha_composite(img, layer)
    solid = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    glyph(ImageDraw.Draw(solid), (color[0], color[1], color[2], 255), 5)
    return Image.alpha_composite(img, solid)


def main():
    os.makedirs(OUT, exist_ok=True)
    make(MINT).save(os.path.join(OUT, "engine_icon_n.png"))
    make(DIM).save(os.path.join(OUT, "engine_icon_d.png"))
    print("wrote", OUT)


if __name__ == "__main__":
    main()
