#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Generate the Battery home-screen icons: mint glyph + soft glow, like the
Ambience/Fake-engine set (2026-11-24: unify all six tiles on mint/glow)."""
import os
from PIL import Image, ImageDraw, ImageFilter

SIZE = 99
OUT = os.path.join(os.path.dirname(__file__), "..", "artifacts", "assets", "battery")
MINT = (104, 236, 222)
DIM = (120, 140, 140)


def glow(img, color, blur=5, alpha=110):
    a = img.split()[3].filter(ImageFilter.GaussianBlur(blur)).point(lambda v: v * alpha // 255)
    layer = Image.new("RGBA", img.size, (color[0], color[1], color[2], 0))
    layer.putalpha(a)
    return Image.alpha_composite(layer, img)


def draw_icon(color):
    img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    w = 5
    left, top, right, bottom = 12, 26, 78, 78
    d.rounded_rectangle([left, top, right, bottom], radius=8, outline=color, width=w)
    d.rounded_rectangle([right + 3, 40, right + 12, 64], radius=3, outline=color, width=w)
    bolt = [(46, 36), (34, 55), (44, 55), (38, 70), (56, 48), (45, 48), (51, 36)]
    d.polygon(bolt, fill=color)
    return img


def main():
    os.makedirs(OUT, exist_ok=True)
    glow(draw_icon((*MINT, 255)), MINT).save(os.path.join(OUT, "battery_icon_n.png"))
    glow(draw_icon((*DIM, 255)), DIM, alpha=60).save(os.path.join(OUT, "battery_icon_d.png"))
    print("wrote", OUT)


if __name__ == "__main__":
    main()
