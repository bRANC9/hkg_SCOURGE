#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Build the Temurin 25 jlink runtime and the USB package tree.
#
# Usage:
#   sh scripts/build_payload.sh
#   JAVA_EXTRA_MODULES=jdk.compiler,jdk.jartool sh scripts/build_payload.sh
#
# Output: packages/java/out/usb/{payload,manifest.md5,run_stuff.sh,...}
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$HERE/lib.sh"

OUT="${JAVA_OUT:-$JAVA_ROOT/out/usb}"
BUILD="$JAVA_ROOT/build"
VERSION=$(temurin_version)
BASE_VERSION=$(runtime_version)
URL=$(temurin_url)
WANT_SHA=$(temurin_sha256)
MODULES=$(sed -e '/^[[:space:]]*#/d' -e '/^[[:space:]]*$/d' "$JAVA_ROOT/config/modules.txt" | paste -sd, -)
if [ -n "${JAVA_EXTRA_MODULES:-}" ]; then
    MODULES="$MODULES,$JAVA_EXTRA_MODULES"
fi

echo "== fetch pinned JDK"
sh "$HERE/fetch_jdk.sh"
JDK_HOME=$(readlink -f "$JAVA_JDK_STAGE/current")
[ -x "$JDK_HOME/bin/jlink" ] || {
    echo "ERROR: JDK_HOME is not usable: $JDK_HOME" >&2
    exit 1
}

echo "== jlink $VERSION"
rm -rf "$BUILD/runtime"
mkdir -p "$BUILD"
"$JDK_HOME/bin/jlink" \
    --add-modules "$MODULES" \
    --strip-debug \
    --no-man-pages \
    --no-header-files \
    --compress=zip-6 \
    --output "$BUILD/runtime"

echo "== assemble $OUT"
rm -rf "$OUT"
mkdir -p "$OUT/payload/java" "$OUT/backup" "$OUT/backup_current"
printf '%s\n' "$VERSION" > "$OUT/payload/java/runtime.version"
RUNTIME="$OUT/payload/java/runtime-$VERSION"
mkdir -p "$RUNTIME"
# jlink emits legal/ symlinks.  exFAT cannot store symlinks, so materialize
# them now; the USB tree must copy cleanly with plain cp -R.
cp -RLp "$BUILD/runtime/." "$RUNTIME/"
cp -f "$JDK_HOME/NOTICE" "$RUNTIME/NOTICE" 2>/dev/null || true

cat > "$RUNTIME/SCOURGE-RUNTIME.txt" <<EOF
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
runtime=temurin-jdk25
version=$VERSION
base_version=$BASE_VERSION
upstream=$URL
upstream_sha256=$WANT_SHA
modules=$MODULES
jlink_from=temurin-jdk25-$VERSION
built=$(date -u +%Y-%m-%dT%H:%M:%SZ)
EOF
printf '%s\n' "$VERSION" > "$RUNTIME/runtime.version"

# The example unit is documentation, not an installed unit.
mkdir -p "$RUNTIME/examples"
cp -f "$JAVA_ROOT/artifacts/systemd/java-app@.service" "$RUNTIME/examples/"

chmod 755 "$RUNTIME/bin/"* 2>/dev/null || true

echo "== runtime gate"
SCOURGE_ROOTFS="${SCOURGE_ROOTFS:-$JAVA_REPO/work/rootfs}" \
    python3 "$HERE/check_runtime.py" "$RUNTIME"

echo "== runtime checksums"
SUMS_TMP="$OUT/payload/java/SHA256SUMS.tmp.$$"
(
    cd "$RUNTIME"
    find . -type f ! -name 'SHA256SUMS*' -print0 | sort -z | \
        xargs -0 sha256sum > "$SUMS_TMP"
)
mv "$SUMS_TMP" "$RUNTIME/SHA256SUMS"
( cd "$RUNTIME" && sha256sum -c SHA256SUMS >/dev/null )

cat > "$OUT/README.txt" <<EOF
Ioniq 5 Gen5W Java 25 runtime - USB payload

Install:
  1. Copy the whole contents of this USB tree to a FAT/exFAT stick.
     Use cp -R or rsync -r; the runtime is materialized and has no symlinks.
  2. Plug the stick into the head unit; the initial_root failsafe runs
     run_stuff.sh as root within ~8 seconds.
  3. Check run_stuff_result.txt on the stick.

What it installs:
  /rw_data/java/runtime-$VERSION/
  /rw_data/java/current -> runtime-$VERSION
  /rw_data/java/env.sh
  /rw_data/java-apps/          (empty app directory; preserved by restore)

Usage:
  /rw_data/java/current/bin/java -jar /rw_data/java-apps/app.jar
  . /rw_data/java/env.sh

This is a headless runtime.  The vendor rootfs has no X11 libraries, so
AWT/Swing GUI applications are not supported; headless AWT/ImageIO is.
The default build has no javac.

A systemd unit example is installed at:
  /rw_data/java/current/examples/java-app@.service
Copy/edit it for your app; the package does not install or enable units.
Put the JAR at /rw_data/java-apps/<name>.jar.

Restore:
  Copy run_stuff_restore.sh over the loader filename on the stick and plug
  it in.  It removes /rw_data/java and preserves /rw_data/java-apps.

This does not flash firmware and does not touch the updater, bootloader,
/app, /etc or any partition.  Runtime install is WIP: VM-validated, car
validation pending.
EOF

cp -f "$HERE/run_stuff.sh" "$OUT/run_stuff.sh"
cp -f "$HERE/run_stuff_restore.sh" "$OUT/run_stuff_restore.sh"
cp -f "$HERE/run_stuff_diag.sh" "$OUT/run_stuff_diag.sh"
chmod 755 "$OUT"/run_stuff*.sh

(
    cd "$OUT"
    find payload -type f -print0 | sort -z | xargs -0 md5sum > manifest.md5
    md5sum -c manifest.md5 >/dev/null
)

echo "== done"
echo "USB tree: $OUT"
echo "runtime:  $RUNTIME ($(du -sh "$RUNTIME" | awk '{print $1}'))"
echo "entries:  $(wc -l < "$OUT/manifest.md5")"
