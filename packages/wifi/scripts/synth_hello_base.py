#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Synthesise the two missing Hello-family QML bases for the WiFi builder.

Why this exists
---------------
`build_artifacts.py` reads its inputs from `../base/<variant>`, and
`build_artifacts._have()` skips a variant when its base is missing.  Only the
bases somebody dumped or synthesised exist on a given machine: this one has
`stock` and `battery_tile`, but not `hello`/`battery_tile_hello`, so the
hello-family variants (and the tests that exercise them) were skipped, and
`tests/test_tiles.py` plus the installer's `hello_battery_tile` case failed on
a missing file rather than on a real defect.

What "hello" means here: the OEM app set that ships the Hello demo tile.  No
rootfs on this machine carries it (`qml_eco3.0`, `qml_new2.0` and
`qml_genesis` all have zero `HelloTile` blocks), so the base cannot be dumped
locally.  This script therefore *synthesises* it the way the builder itself
would place a tile: copy the base that exists and insert one `HelloTile` per
page menu, using `build_artifacts.hello_tile_block`.

Synthesised bases are marked with a `SYNTHESIZED` file and are NOT a substitute
for a dump: a variant generated from them targets units whose OEM app set has
the Hello tile, and the generated QML will reference a `HelloTile` component
that a stock unit does not define.  Treat the result as a test fixture; if a
real Hello base is ever dumped, drop it in and the marker disappears.

Usage:
    python3 packages/wifi/scripts/synth_hello_base.py [--force]
"""

from __future__ import annotations

import argparse
import importlib.util
from pathlib import Path
import shutil
import sys

SCRIPT = Path(__file__).resolve()
SCRIPTS = SCRIPT.parent
WIFI = SCRIPTS.parent
BUILD = SCRIPTS / "build_artifacts.py"
BASE = WIFI / "base"

# variant -> the base it is derived from
VARIANTS = {
    "hello": "stock",
    "battery_tile_hello": "battery_tile",
}
# Deliberately NOT synthesised: "battery_hello".  The hello_battery_wifi variant
# reads battery_hello/HomeScreen/<A>.hello.qml, and a *page-based* Battery base
# (a batteryPage block) is what that file must contain.  No such base exists on
# this machine, and deriving it from battery_tile produces a variant that carries
# BatteryTile *and* a battery page - contradictory QML that would be installed on
# a unit.  tests/test_tiles.py reads hello_battery_wifi, so it stays red here
# until somebody dumps the page-based Battery base; inventing one is worse.

MARKER = "SYNTHESIZED"
MARKER_TEXT = (
    "Synthesised from {source} by packages/wifi/scripts/synth_hello_base.py.\n"
    "No OEM dump with the Hello tile exists on this machine; this tree is a test\n"
    "fixture for the hello-family variants, not a dumped base.\n"
)


def load_builder():
    """Import build_artifacts.py without running its main()."""
    spec = importlib.util.spec_from_file_location("wifi_build_artifacts", BUILD)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def insert_hello(text: str, builder) -> tuple[str, int]:
    """Insert one HelloTile per page menu, right after the page block opens."""
    inserted = 0
    for page, menu, hello_id, page_index in builder.PAGE_MENUS:
        if f"id: {hello_id}" in text:
            continue
        page_marker = f"                QMenuPageNoDrag{{\n                    id: {menu}"
        if page_marker not in text:
            print(f"  no page marker for {menu}; skipped", file=sys.stderr)
            continue
        block = builder.hello_tile_block(menu, hello_id, page_index,
                                         "onClicked: helloPopup.visible = true")
        position = text.find(page_marker)
        end = builder.find_block_end(text, position)
        text = text[:end] + block + text[end:]
        inserted += 1
    return text, inserted


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--force", action="store_true",
                        help="replace an existing non-synthesised base")
    args = parser.parse_args()

    builder = load_builder()
    advanced = "HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
    problems = 0

    for variant, source in VARIANTS.items():
        source_dir = BASE / source
        target_dir = BASE / variant
        if not source_dir.is_dir():
            print(f"skip {variant}: source base {source_dir} is missing", file=sys.stderr)
            problems += 1
            continue
        if target_dir.exists():
            if not (target_dir / MARKER).is_file() and not args.force:
                print(f"skip {variant}: {target_dir} exists and was not synthesised "
                      f"(pass --force to replace)", file=sys.stderr)
                problems += 1
                continue
            shutil.rmtree(target_dir)
        shutil.copytree(source_dir, target_dir)

        target = target_dir / advanced
        if not target.is_file():
            print(f"skip {variant}: no {advanced} in {source_dir}", file=sys.stderr)
            problems += 1
            continue
        text, inserted = insert_hello(target.read_text(encoding="utf-8"), builder)
        target.write_text(text, encoding="utf-8")
        (target_dir / MARKER).write_text(MARKER_TEXT.format(source=source), encoding="utf-8")
        print(f"{variant}: {inserted} HelloTile block(s) inserted from {source}")

    return 0 if problems == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
