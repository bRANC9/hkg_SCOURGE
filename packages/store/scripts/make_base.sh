#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Populate packages/store/base/<variant> so build_artifacts.py can add the tile.
#
# The base is the owner's own home-screen QML - never committed (.gitignore).
# Two sources:
#
#   # from a vendor/extracted rootfs (stock unit)
#   SCOURGE_ROOTFS=/path/to/rootfs sh scripts/make_base.sh
#
#   # from the car's current file (already carrying Battery/WiFi/Tweaks tiles)
#   sh scripts/make_base.sh --from-dump /path/to/QHMI_HomeScreenMain_Advanced.qml
#
# The variant name is derived from the payload markers in the file, matching the
# names build_artifacts.py emits (stock, hello, battery, battery_hello,
# battery_tile_wifi, ...).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
BASE="$ROOT/base"
REL="HomeScreen/QHMI_HomeScreenMain_Advanced.qml"

dump=""
rootfs="${SCOURGE_ROOTFS:-$ROOT/../../work/rootfs}"
if [ "${1:-}" = "--from-dump" ]; then
    dump=${2:?usage: make_base.sh --from-dump <Advanced.qml>}
fi

if [ -n "$dump" ]; then
    [ -f "$dump" ] || { echo "no such file: $dump" >&2; exit 1; }
    src="$dump"
else
    src="$rootfs/app/share/AppHomeScreen/qml_eco3.0/$REL"
    [ -f "$src" ] || {
        echo "home-screen QML not found: $src" >&2
        echo "set SCOURGE_ROOTFS to an extracted rootfs, or use --from-dump <file>" >&2
        exit 2
    }
fi

variant=""
for marker in HELLO_PAYLOAD_260128 WIFI_PAYLOAD_260128 BATTERY_TILE_PAYLOAD_260128 \
              BATTERY_PAYLOAD_260128 TWEAKS_PAYLOAD_260128 STORE_PAYLOAD_260128; do
    grep -q "$marker" "$src" 2>/dev/null || continue
    [ "$marker" = "STORE_PAYLOAD_260128" ] && continue
    prefix=$(echo "$marker" | cut -d_ -f1 | tr 'A-Z' 'a-z')
    case "$prefix" in
        battery) [ "${variant#*battery}" != "$variant" ] && continue ;;
    esac
    variant="${variant}${prefix}_"
done
variant="${variant%_}"
[ -n "$variant" ] || variant="stock"

mkdir -p "$BASE/$variant/$(dirname "$REL")"
cp -f "$src" "$BASE/$variant/$REL"
chmod 644 "$BASE/$variant/$REL" 2>/dev/null || true

echo "base/$variant written from $src"
echo "variant=$variant"
echo
echo "Now build the payload:"
echo "  sh scripts/build_payload.sh /run/media/<stick>"
echo "The tile patch needs only this file; the installer picks the variant that"
echo "matches the marker set on the unit at install time."
