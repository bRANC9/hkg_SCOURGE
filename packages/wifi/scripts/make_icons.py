#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""WiFi glyph: mint + soft glow, matching the Ambience/Fake-engine set."""
from PIL import Image, ImageDraw, ImageFilter
import os

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                    "artifacts", "assets", "wifi")
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
    d.ellipse((44, 66, 55, 77), fill=color)
    d.arc((27, 48, 72, 93), 225, 315, fill=color, width=7)
    d.arc((17, 33, 82, 98), 225, 315, fill=color, width=7)
    d.arc((7, 18, 92, 103), 225, 315, fill=color, width=7)
    return im


os.makedirs(OUT, exist_ok=True)
glow(make((*MINT, 255)), MINT).save(os.path.join(OUT, "wifi_icon_n.png"))
glow(make((*DIM, 255)), DIM, alpha=60).save(os.path.join(OUT, "wifi_icon_d.png"))
print("wrote", OUT)
