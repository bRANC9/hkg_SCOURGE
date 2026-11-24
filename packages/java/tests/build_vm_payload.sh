#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Build the ext4 payload directory used by the tools/vm java-runtime scenario.
#
# Layout:
#   out/vm/install/       copy of out/usb (the real installer + runtime)
#   out/vm/selftest.jar   JavaSelfTest selftest/daemon/client modes
#   out/vm/README.txt
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/.." && pwd)
REPO=$(CDPATH= cd -- "$ROOT/../.." && pwd)
OUT="$ROOT/out/vm"

[ -d "$ROOT/out/usb/payload/java" ] || {
    echo "ERROR: build out/usb first: sh scripts/build_payload.sh" >&2
    exit 1
}
sh "$ROOT/scripts/fetch_jdk.sh" >/dev/null
JDK_HOME=$(readlink -f "$REPO/third_party/temurin/current")

rm -rf "$OUT"
mkdir -p "$OUT/classes"
"$JDK_HOME/bin/javac" -d "$OUT/classes" "$ROOT/tests/src/JavaSelfTest.java"
"$JDK_HOME/bin/jar" --create --file "$OUT/selftest.jar" \
    --main-class JavaSelfTest -C "$OUT/classes" .
rm -rf "$OUT/classes"

cp -a "$ROOT/out/usb" "$OUT/install"
cat > "$OUT/README.txt" <<'EOF'
SCOURGE java-runtime VM payload.

install/ is the exact USB tree produced by packages/java/scripts/build_payload.sh.
selftest.jar is the first-party JavaSelfTest harness (selftest/daemon/client).
The VM probe installs install/ with the real run_stuff.sh, verifies the runtime,
runs the self-test and daemon tests, then restores.
EOF

echo "VM payload: $OUT"
du -sh "$OUT"
