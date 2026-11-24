#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Assemble the fake-engine payload.  Usage: sh scripts/build_payload.sh [outdir]
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT=${1:-$HERE/out/usb}
mkdir -p "$OUT/payload/bin" "$OUT/payload/systemd" "$OUT/payload/audio" "$OUT/docs" "$OUT/payload/assets/fake-engine" \
         "$OUT/payload/lib" "$OUT/payload/qml" \
         "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/HomeScreen" \
         "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/component"
export PATH="$HERE/../../../work/x86_64-linux-musl-cross/bin:/usr/bin:/bin:/usr/sbin:/sbin"
CC=$(command -v musl-gcc || echo cc)
"$CC" -O2 -Wall -Wextra -Werror -march=x86-64 -mtune=generic -static -o "$OUT/payload/bin/fake_engine_daemon" "$HERE/src/fake_engine_daemon.c"
cp -f "$HERE/artifacts/systemd/fake-engine.service" "$OUT/payload/systemd/"
python3 "$HERE/scripts/make_engine.py" "$OUT/payload/audio/engine_idle.wav" 4 600 >/dev/null
python3 "$HERE/scripts/make_icons.py" >/dev/null 2>&1 || echo "warning: icons not regenerated" >&2
mkdir -p "$OUT/payload/assets/fake-engine"
cp -f "$HERE/artifacts/assets/fake-engine/"*.png "$OUT/payload/assets/fake-engine/" 2>/dev/null || true
python3 "$HERE/scripts/make_engine.py" "$OUT/payload/audio" --bundle >/dev/null
# The HU's USB media player ("player1") auto-scans the stick and started playing
# these on insertion (2026-11-24).  Ship them as .dat (the same trick as the
# beep tones): the installer renames them back to .wav under /rw_data.
for _f in "$OUT"/payload/audio/*.wav; do
    [ -f "$_f" ] || continue
    mv -f "$_f" "${_f%.wav}.dat"
done
cp -f "$HERE/artifacts/qml/AppHomeScreen/qml_eco3.0/HomeScreen/FakeEnginePage.qml" \
    "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/HomeScreen/"
cp -f "$HERE/artifacts/qml/AppHomeScreen/qml_eco3.0/component/FakeEngineTile.qml" \
    "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/component/"
cp -f "$HERE/scripts/tile.instance.qml" "$OUT/payload/qml/FakeEngineTile.instance.qml"
cp -f "$HERE/scripts/tile.fallback.qml" "$OUT/payload/qml/FakeEngineTile.fallback.qml"
cp -f "$HERE/scripts/page.instance.qml" "$OUT/payload/qml/FakeEnginePage.instance.qml"
# The injection helper is the ambient one: brace-matched insertion with backup
# and --revert.  Copied (not linked) so this package stands alone.
if [ -f "$HERE/../ambient/scripts/qml_inject.sh" ]; then
    cp -f "$HERE/../ambient/scripts/qml_inject.sh" "$OUT/payload/lib/"
fi
# Root-mount helper.  /etc and /app live on the read-only rootfs, and this stage
# runs after the other packages restored their own read-only state: without an
# explicit remount the service copy died with "Read-only file system" and the
# whole combo reported FAIL after four OK stages (2026-11-23).
if [ -f "$HERE/../common/install_io.sh" ]; then
    cp -f "$HERE/../common/install_io.sh" "$OUT/payload/lib/"
fi
cp -f "$HERE/scripts/run_stuff.sh" "$OUT/run_stuff.sh"
cp -f "$HERE/scripts/run_stuff_restore.sh" "$OUT/run_stuff_restore.sh"
cp -f "$HERE/docs/README_STICK.txt" "$OUT/docs/" 2>/dev/null || true
( cd "$OUT" && find payload -type f | LC_ALL=C sort | xargs md5sum > manifest.md5 )
( cd "$OUT" && md5sum -c manifest.md5 >/dev/null )
echo "payload built at: $OUT"
