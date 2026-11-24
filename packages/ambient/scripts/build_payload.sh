#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Assemble a USB payload tree (or a Store stage directory) for the Ambience app.
#
# Usage: sh scripts/build_payload.sh /path/to/output
set -eu
# Keep the cross toolchain reachable: pinning PATH here hid the repo musl-gcc, so
# `command -v musl-gcc` fell back to the host cc and the shipped daemon was a
# glibc x86-64-v3 static binary that dies with SIGILL on the head unit (2026-11-24:
# systemctl restart returned 0, no process, nothing listening on 8094).
PATH=/bin:/sbin:/usr/bin:/usr/sbin

HERE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT=${1:-}
if [ -z "$OUT" ]; then
    echo "usage: sh scripts/build_payload.sh /path/to/output" >&2
    exit 2
fi
if [ -e "$OUT" ] && [ ! -d "$OUT" ]; then
    echo "output exists and is not a directory: $OUT" >&2
    exit 1
fi
mkdir -p "$OUT"

if [ -e "$OUT/payload" ] || [ -e "$OUT/run_stuff.sh" ]; then
    echo "refusing to overwrite an existing payload in: $OUT" >&2
    exit 1
fi

# Build the daemon with musl, like every other SCOURGE binary.  The host glibc is
# x86-64-v3 and a -static glibc build carries those routines with it: the head
# unit is an Atom-class CPU and dies with SIGILL on the first v3 instruction.
# That is exactly what happened to this daemon - systemctl restart returned 0, the
# process vanished immediately and nothing was listening on 8094 (2026-11-24).
CROSS="$HERE/../../work/x86_64-linux-musl-cross/bin/musl-gcc"
MUSL_CC=""
[ -x "$CROSS" ] && MUSL_CC="$CROSS"
[ -n "$MUSL_CC" ] || MUSL_CC="$(command -v musl-gcc 2>/dev/null || true)"
[ -n "$MUSL_CC" ] || {
    echo "ERROR: musl-gcc not found (looked in $CROSS and PATH)." >&2
    echo "       Refusing to ship a host-glibc daemon: it faults with SIGILL on the Atom." >&2
    exit 1
}
echo "== daemon compiler: $MUSL_CC"
# Force the link: `make` happily reused a binary built by the host gcc, which is
# how a glibc x86-64-v3 daemon kept shipping after the compiler was switched.
rm -f "$HERE/src/ambient_daemon"
( cd "$HERE/src" && make CC="$MUSL_CC" CFLAGS="-O2 -Wall -Wextra -Werror -march=x86-64 -mtune=generic" )
if [ -x "$HERE/src/ambient_daemon" ]; then
    _sz=$(wc -c < "$HERE/src/ambient_daemon")
    # A musl static build of this daemon is ~4x smaller than the glibc one; the
    # size is the cheap tell that the intended toolchain produced the binary.
    [ "$_sz" -lt 600000 ] || echo "WARNING: ambient_daemon is $_sz bytes; a musl build is far smaller - check the toolchain" >&2
fi

mkdir -p "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/component" \
         "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/HomeScreen" \
         "$OUT/payload/assets/ambient" "$OUT/payload/bin" \
         "$OUT/payload/systemd" "$OUT/payload/lib" "$OUT/docs"

cp "$HERE/../common/install_io.sh" "$OUT/payload/lib/"
cp "$HERE/scripts/qml_inject.sh" "$OUT/payload/lib/"

cp -a "$HERE/artifacts/qml/AppHomeScreen/qml_eco3.0/HomeScreen/AmbientPage.qml" \
      "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/HomeScreen/"
cp -a "$HERE/artifacts/qml/AppHomeScreen/qml_eco3.0/component/AmbientTile.qml" \
      "$HERE/artifacts/qml/AppHomeScreen/qml_eco3.0/component/AmbientSlider.qml" \
      "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/component/"
cp -a "$HERE/artifacts/assets/ambient" "$OUT/payload/assets/"
cp -a "$HERE/artifacts/systemd/ambient-daemon.service" "$OUT/payload/systemd/"
cp -a "$HERE/src/ambient_daemon" "$OUT/payload/bin/ambient_daemon"
cp -a "$HERE/docs/README_STICK.txt" "$OUT/README.txt"
cp -a "$HERE/docs/README_STICK.txt" "$OUT/docs/README_STICK.txt"

# Instance snippets the installer injects into the live home screen.
cp -a "$HERE/scripts/tile.instance.qml" "$OUT/payload/qml/AmbientTile.instance.qml"
cp -a "$HERE/scripts/page.instance.qml" "$OUT/payload/qml/AmbientPage.instance.qml"

cp -a "$HERE/scripts/run_stuff.sh" "$OUT/run_stuff.sh"
cp -a "$HERE/scripts/run_stuff_restore.sh" "$OUT/run_stuff_restore.sh"

(
    cd "$OUT"
    find payload -type f -print | sort | while IFS= read -r f; do md5sum "$f"; done > manifest.md5
    md5sum -c manifest.md5 >/dev/null
)

echo "payload built at: $OUT"
echo "copy run_stuff.sh, payload/, manifest.md5 and README.txt to the stick root."
echo "restore: copy run_stuff_restore.sh over run_stuff.sh and plug the stick in."
