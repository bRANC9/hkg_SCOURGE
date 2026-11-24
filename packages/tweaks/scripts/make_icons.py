#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Tweaks/Reboot glyphs: mint + soft glow, matching the Ambience/Fake-engine set."""
from PIL import Image, ImageDraw, ImageFilter
import os

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                    "artifacts", "assets", "tweaks")
MINT = (104, 236, 222)
DIM = (120, 140, 140)


def glow(img, color, blur=5, alpha=110):
    a = img.split()[3].filter(ImageFilter.GaussianBlur(blur)).point(lambda v: v * alpha // 255)
    layer = Image.new("RGBA", img.size, (color[0], color[1], color[2], 0))
    layer.putalpha(a)
    return Image.alpha_composite(layer, img)


def make(color):
    im = Image.new("RGBA", (99, 99), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    rows = [(20, 22), (49, 64), (78, 38)]
    for y, knob_x in rows:
        d.rounded_rectangle((12, y - 3, 87, y + 3), radius=3, fill=color)
        d.ellipse((knob_x - 10, y - 10, knob_x + 10, y + 10), fill=color)
    return im


def make_power(color):
    im = Image.new("RGBA", (99, 99), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.arc((19, 24, 80, 85), start=300, end=240, fill=color, width=7)
    d.line((49, 17, 49, 54), fill=color, width=7)
    return im


os.makedirs(OUT, exist_ok=True)
glow(make((*MINT, 255)), MINT).save(os.path.join(OUT, "tweaks_icon_n.png"))
glow(make((*DIM, 255)), DIM, alpha=60).save(os.path.join(OUT, "tweaks_icon_d.png"))
glow(make_power((*MINT, 255)), MINT).save(os.path.join(OUT, "reboot_icon_n.png"))
glow(make_power((*DIM, 255)), DIM, alpha=60).save(os.path.join(OUT, "reboot_icon_d.png"))
print("wrote", OUT)
