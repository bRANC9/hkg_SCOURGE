#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Add the Marketplace tile + page to the home-screen QML.

Same contract as the other packages: read the owner's base `../base/<variant>`,
insert one tile into the allocator and one full-screen page, marker
`STORE_PAYLOAD_260128`. The page itself is static (artifacts/qml/...), only the
tile placement is per-variant.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BASE = os.path.join(ROOT, "base")
OUT = os.path.join(ROOT, "artifacts", "qml", "AppHomeScreen", "qml_eco3.0")
MARK = "// STORE_PAYLOAD_260128\n"
ADV = "HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
VARIANTS = [
    "stock", "hello", "battery", "battery_hello",
    "wifi", "hello_wifi", "battery_wifi", "hello_battery_wifi",
    "battery_tile", "battery_tile_hello", "battery_tile_wifi", "hello_battery_tile_wifi",
]

# Tile ordinal: Battery=1, WiFi=2, Tweaks=3, Reboot=4 -> Marketplace=5.
EMPTY_INDEX = 5


def read(path):
    with open(path, "r", encoding="utf-8") as fh:
        return fh.read()


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text)


def add_marker(text):
    if MARK in text:
        return text
    if text.startswith("//"):
        first, rest = text.split("\n", 1)
        return first + "\n" + MARK + rest
    return MARK + text


TILE = """            StoreTile {
                id: storeTile
                property int emptyIdx: root.firstEmptyIndex(menuPage.pageModel, %d)
                visible: (isAllMenuScreen || listType === AppHome_Enum.E_LIST_SETUP_MAIN) && !customizeMode && index === menuList.currentIndex && emptyIdx >= 0
                x: menuPage.x + (emptyIdx %% k_nbrOfColumns) * k_itemWidth
                y: menuPage.y + Math.floor(emptyIdx / k_nbrOfColumns) * k_itemHeight
                width: k_itemWidth
                height: k_itemHeight
                z: 7
                onClicked: root.storeClicked()
            }
""" % EMPTY_INDEX


def insert_after_item(text, marker):
    """Insert TILE right after the QML item whose first line contains `marker`.

    Brace-counted (string literals are skipped) so nested items are copied
    correctly. Returns None when the marker is absent.
    """
    at = text.find(marker)
    if at < 0:
        return None
    brace = text.find("{", at)
    if brace < 0:
        return None
    depth = 0
    i = brace
    while i < len(text):
        ch = text[i]
        if ch == '"':
            i += 1
            while i < len(text) and text[i] != '"':
                i += 2 if text[i] == "\\" else 1
        elif ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                end = text.find("\n", i)
                end = len(text) if end < 0 else end + 1
                return text[:end] + TILE + text[end:]
        i += 1
    return None


ANCHORS = [
    "            RebootTile {\n                id: rebootTile",
    "            TweaksTile {\n                id: tweaksTile",
    "            QMenuPageAllowEmpty{\n                id: menuPage",
    "            QMenuPageAllowEmpty {\n                id: menuPage",
]


def patch_advanced(variant):
    text = add_marker(read(os.path.join(BASE, variant, ADV)))
    if "signal storeClicked()" not in text:
        if "signal tweaksClicked()" in text:
            text = text.replace("    signal tweaksClicked()\n",
                                "    signal tweaksClicked()\n    signal storeClicked()\n", 1)
        else:
            text = text.replace("    id: root\n", "    id: root\n    signal storeClicked()\n", 1)
    if "StoreTile" not in text:
        for anchor in ANCHORS:
            patched = insert_after_item(text, anchor)
            if patched is not None:
                text = patched
                break
        else:
            raise ValueError(f"no tile anchor in {variant}/{ADV}")
    return text


def main():
    if not os.path.isdir(BASE):
        print(f"base QML not found ({BASE}) - skipping the home-screen patch", file=sys.stderr)
        return 0
    for variant in VARIANTS:
        src = os.path.join(BASE, variant, ADV)
        if not os.path.exists(src):
            continue
        try:
            patched = patch_advanced(variant)
        except ValueError as exc:
            # A base without the shared allocator (a plain stock home screen)
            # cannot take the tile. Ship no variant for it: the installer then
            # reports "page installed, tile skipped" instead of risking a
            # half-patched home screen.
            print(f"skip {variant}: {exc}", file=sys.stderr)
            continue
        write(os.path.join(OUT, "HomeScreen", f"QHMI_HomeScreenMain_Advanced.{variant}.qml"), patched)
    # The page and the tile are committed in artifacts/ and need no generation.
    print("store artifacts written to", OUT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
