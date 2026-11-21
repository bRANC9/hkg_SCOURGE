#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Generate the Marketplace tile icons (day + night) as 99x99 PNGs.

Same shapes and palette as the other tiles: an outlined shopping bag with a
download arrow, filled for the normal icon, dimmer for the pressed/night one.
"""
import os

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "artifacts", "assets", "store")
SIZE = 99
SCALE = 4


def draw(color, accent, width):
    img = Image.new("RGBA", (SIZE * SCALE, SIZE * SCALE), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    s = SCALE
    # bag body
    d.rounded_rectangle([18 * s, 32 * s, 81 * s, 85 * s], radius=8 * s,
                        outline=color, width=width * s)
    # handle
    d.arc([30 * s, 14 * s, 69 * s, 52 * s], start=180, end=360,
          fill=color, width=width * s)
    # download arrow inside the bag
    d.line([49 * s, 46 * s, 49 * s, 68 * s], fill=accent, width=width * s)
    d.polygon([(41 * s, 60 * s), (57 * s, 60 * s), (49 * s, 73 * s)], fill=accent)
    return img.resize((SIZE, SIZE), Image.LANCZOS)


def main():
    os.makedirs(OUT, exist_ok=True)
    draw((255, 255, 255, 255), (143, 203, 155, 255), 4).save(
        os.path.join(OUT, "store_icon_n.png"))
    draw((168, 178, 188, 255), (110, 160, 120, 255), 4).save(
        os.path.join(OUT, "store_icon_d.png"))
    print("icons written to", OUT)


if __name__ == "__main__":
    main()
