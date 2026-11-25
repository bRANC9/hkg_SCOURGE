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
# The cross toolchain lives at <repo>/work/, i.e. TWO levels up from here - the
# old path had three and silently found nothing, so the build fell back to the
# host cc.  That links a static glibc built for x86-64-v3: the head unit's
# Atom-class CPU has SSE4.2 but no AVX, and the binary died with
# "trap invalid opcode ip:44f5c5 ... vpxor" (SIGILL) at every start - the fake
# engine never ran in the car (2026-11-25, found over ssh).
export PATH="$HERE/../../work/x86_64-linux-musl-cross/bin:/usr/bin:/bin:/usr/sbin:/sbin"
CC=$(command -v musl-gcc || true)
if [ -z "$CC" ]; then
    echo "no musl-gcc: a host -static build is AVX-tainted and SIGILLs on the unit." >&2
    echo "fetch the cross toolchain (scripts/00_toctou.sh / work/x86_64-linux-musl-cross)." >&2
    exit 1
fi
"$CC" -O2 -Wall -Wextra -Werror -march=x86-64 -mtune=generic -static -o "$OUT/payload/bin/fake_engine_daemon" "$HERE/src/fake_engine_daemon.c"
# The unit's CPU has no AVX.  Anything VEX-encoded in the binary (VEX prefix in
# objdump's text: vpxor/vmovdqa/vzeroupper...) means the wrong libc was linked and
# the daemon will SIGILL, so fail the build instead of shipping it.
if objdump -d "$OUT/payload/bin/fake_engine_daemon" 2>/dev/null | grep -qE '\bv(pxor|movdq|zero|movdqu)'; then
    echo "AVX instructions in fake_engine_daemon: the unit has none (SSE4.2 max);" >&2
    echo "build it with the musl cross toolchain, not the host cc." >&2
    exit 1
fi
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
# tile.fallback.qml is no longer shipped: it was the floating top-right icon
# (removed 2026-11-25).  The stage strips an existing one with --remove-marker.
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
