#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Offline install/restore test for packages/java (no car, no VM required).
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/.." && pwd)
SRC="$ROOT/out/usb"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/java-installer-test.XXXXXX")
trap 'rm -rf "$TMP"' EXIT INT TERM

[ -d "$SRC/payload/java" ] || {
    echo "ERROR: build out/usb first: sh scripts/build_payload.sh" >&2
    exit 1
}
cp -a "$SRC" "$TMP/usb"
mkdir -p "$TMP/rw"
APPS="$TMP/apps"

run_install() {
    USB_DIR="$TMP/usb" JAVA_RW_DIR="$TMP/rw" JAVA_APPS_DIR="$APPS" \
        JAVA_SKIP_SMACK=1 sh "$TMP/usb/run_stuff.sh"
}

check_status() {
    file="$1"
    grep -q '^status=OK$' "$file" || {
        echo "expected status=OK in $file" >&2
        cat "$file" >&2 || true
        exit 1
    }
}

run_install
check_status "$TMP/rw/result.txt"
check_status "$TMP/usb/run_stuff_result.txt"
[ -L "$TMP/rw/current" ] || { echo "current symlink missing" >&2; exit 1; }
[ -f "$TMP/rw/.scourge-java" ] || { echo "restore marker missing" >&2; exit 1; }
[ -d "$APPS" ] || { echo "app directory missing" >&2; exit 1; }
"$TMP/rw/current/bin/java" -version >/dev/null 2>&1 || {
    echo "installed java -version failed" >&2; exit 1; }

# Reinstall is idempotent and prunes an obsolete runtime directory.
mkdir -p "$TMP/rw/runtime-0.0.0"
printf 'demo\n' > "$APPS/demo.jar"
run_install
[ ! -e "$TMP/rw/runtime-0.0.0" ] || { echo "old runtime was not pruned" >&2; exit 1; }
[ -L "$TMP/rw/current" ] || { echo "current symlink lost on reinstall" >&2; exit 1; }
[ -f "$APPS/demo.jar" ] || { echo "app JAR lost on reinstall" >&2; exit 1; }

# Free-space failure is explicit and leaves no partial current link.
mkdir -p "$TMP/rw2"
set +e
USB_DIR="$TMP/usb" JAVA_RW_DIR="$TMP/rw2" JAVA_APPS_DIR="$APPS" \
    JAVA_SKIP_SMACK=1 JAVA_MIN_FREE_KB=999999999 \
    sh "$TMP/usb/run_stuff.sh" >/dev/null 2>&1
rc=$?
set -e
[ "$rc" -ne 0 ] || { echo "free-space failure did not fail" >&2; exit 1; }
grep -q '^status=FAIL$' "$TMP/rw2/result.txt" || {
    echo "free-space failure did not write status=FAIL" >&2; exit 1; }
[ ! -e "$TMP/rw2/current" ] || { echo "free-space failure left current" >&2; exit 1; }

# Restore removes the runtime, preserves the app JAR, and refuses an
# unmarked directory even if the caller points JAVA_RW_DIR at it.
USB_DIR="$TMP/usb" JAVA_RW_DIR="$TMP/rw" JAVA_APPS_DIR="$APPS" \
    sh "$TMP/usb/run_stuff_restore.sh" >/dev/null 2>&1
[ ! -e "$TMP/rw" ] || { echo "restore did not remove $TMP/rw" >&2; exit 1; }
[ -f "$APPS/demo.jar" ] || { echo "restore removed the app JAR" >&2; exit 1; }
grep -q '^status=OK$' "$TMP/usb/run_stuff_result.txt" || {
    echo "restore did not write status=OK" >&2; exit 1; }

mkdir -p "$TMP/not-java"
printf 'keep\n' > "$TMP/not-java/keep"
set +e
USB_DIR="$TMP/usb" JAVA_RW_DIR="$TMP/not-java" sh "$TMP/usb/run_stuff_restore.sh" \
    >/dev/null 2>&1
rc=$?
set -e
[ "$rc" -ne 0 ] || { echo "restore accepted an unmarked directory" >&2; exit 1; }
[ -f "$TMP/not-java/keep" ] || { echo "restore deleted unmarked data" >&2; exit 1; }

# Canonicalization/overlap guards: trailing slash and .. must not let the
# installer put the runtime on top of the app directory.
mkdir -p "$TMP/guard/rw_data/java" "$TMP/guard/rw_data/java-apps"
for rw in "$TMP/guard/rw_data/" "$TMP/guard/rw_data/java/.." \
          "$TMP/guard/rw_data/java-apps/"; do
    set +e
    USB_DIR="$TMP/usb" JAVA_RW_DIR="$rw" \
        JAVA_APPS_DIR="$TMP/guard/rw_data/java-apps" \
        sh "$TMP/usb/run_stuff.sh" >/dev/null 2>&1
    rc=$?
    set -e
    [ "$rc" -ne 0 ] || { echo "installer accepted overlapping path $rw" >&2; exit 1; }
done
[ ! -e "$TMP/guard/rw_data/.scourge-java" ] || {
    echo "refused install left a marker" >&2; exit 1; }
[ ! -e "$TMP/guard/rw_data/java-apps/.scourge-java" ] || {
    echo "refused install wrote into the app directory" >&2; exit 1; }

echo "INSTALLER_TEST_OK"
