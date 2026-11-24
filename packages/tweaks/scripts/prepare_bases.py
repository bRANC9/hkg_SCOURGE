#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Gen5W_L EU 260128
"""Stage generated Tweaks inputs from canonical sibling packages, not app forks."""
import os
from pathlib import Path
import shutil
import tempfile

root = Path(__file__).resolve().parents[1]
wifi = root.parent / 'wifi'
dest = root / 'base'
if dest.exists() and not (dest / '.scourge-generated').is_file():
    raise SystemExit('Existing unowned tweaks/base: preserve it; move it aside explicitly before rebuilding')
with tempfile.TemporaryDirectory(prefix='tweaks-base-', dir=root / 'out') as d:
    stage = Path(d) / 'base'
    # Only the variants somebody dumped or synthesised exist locally; a missing
    # base is skipped, not fatal - the installer then reports "tile skipped" for
    # that combination instead of the package failing to build at all.
    for variant in ('stock', 'hello', 'battery', 'battery_hello', 'battery_tile', 'battery_tile_hello'):
        src = wifi / 'base' / variant
        if not src.is_dir():
            print(f'skip base {variant}: {src} missing')
            continue
        shutil.copytree(src, stage / variant)
    advanced = 'HomeScreen/QHMI_HomeScreenMain_Advanced'
    if (stage / 'battery_hello' / f'{advanced}.hello.qml').is_file():
        shutil.copyfile(stage / 'battery_hello' / f'{advanced}.hello.qml',
                        stage / 'battery_hello' / f'{advanced}.qml')
    source = wifi / 'artifacts/qml/AppHomeScreen/qml_eco3.0'
    for variant in ('wifi', 'hello_wifi', 'battery_wifi', 'hello_battery_wifi', 'battery_tile_wifi', 'hello_battery_tile_wifi'):
        base_from = wifi / 'base' / ('hello' if variant.startswith('hello') else 'stock')
        if not base_from.is_dir() or not (source / f'HomeScreen/QHMI_HomeScreenMain_Advanced.{variant}.qml').is_file():
            print(f'skip wifi variant {variant}: base or artifact missing')
            continue
        base = stage / variant
        shutil.copytree(base_from, base)
        for name in ('HomeScreen/QHMI_HomeScreenMain_Advanced',):
            shutil.copyfile(source / f'{name}.{variant}.qml', base / f'{name}.qml')
        short = 'hello_wifi' if variant.startswith('hello') else 'wifi'
        for name in ('HomeScreen/QHMI_HomeScreenMain_Wide', 'HomeScreen/Screen_HomeScreen_AllMenus',
                     'HomeScreen/Screen_HomeScreen_Submenu_Setup', 'component/QCustomizedMenuListAllowEmpty'):
            shutil.copyfile(source / f'{name}.{short}.qml', base / f'{name}.qml')
    firmware = os.environ.get('SCOURGE_ROOTFS') or os.environ.get('IONIQ_ROOTFS') or root.parents[1] / 'work/rootfs'
    theme = Path(firmware) / 'app/share/AppHomeScreen/qml_eco3.0/component/common/Q_ThemeImage.qml'
    for variant in ('stock', 'hello'):
        target = stage / variant / 'component/common/Q_ThemeImage.qml'
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(theme, target)
    (stage / '.scourge-generated').write_text('Canonical generated inputs; no owner edits\n')
    # Only this builder's previously marked generated tree may be replaced.
    if dest.exists():
        shutil.rmtree(dest)
    shutil.move(stage, dest)
