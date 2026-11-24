#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

# TARGET: Ioniq 5 Gen5W_L, 260128 (EU)
# Build the research ivi_input focus helper against the guest's libwayland
# client and libdl from the owner's staged rootfs. OEM libraries stay in the
# gitignored cache and are never committed.
set -eu
cd "$(dirname "$0")/.."

ROOTFS=${1:-}
if [ -z "$ROOTFS" ]; then
    echo 'usage: sh probes/ivi-focus-build.sh /path/to/rootfs.img' >&2
    echo "the rootfs image must be the owner's staged decrypt (input/, ignored)" >&2
    exit 1
fi
if [ ! -f "$ROOTFS" ]; then
    echo "rootfs image not found: $ROOTFS" >&2
    exit 1
fi

# ivi-layer-build.sh extracts the same libwayland-client/libdl cache.
sh probes/ivi-layer-build.sh "$ROOTFS" >/dev/null
OUT=.cache/ivi-layer

gcc -O2 -fno-builtin -fno-tree-loop-distribute-patterns \
    -fno-stack-protector -fno-pie -no-pie -nostdlib -nostartfiles \
    -Wl,-e,_start -Wl,--dynamic-linker=/lib/ld-2.25.so \
    -Wl,--allow-shlib-undefined \
    -o "$OUT/ivi-focus" probes/ivi-focus.c \
    "$OUT/libwayland-client.so.0.3.0" "$OUT/libdl-2.25.so"

echo "built $OUT/ivi-focus"
echo "copy it to the guest share, then run with LD_LIBRARY_PATH=/app/lib:/usr/lib"
echo "usage: $OUT/ivi-focus [surface-id]   (default 11000, the launcher surface id)"
