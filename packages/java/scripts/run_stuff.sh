#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Ioniq 5 Gen5W_L EU 260128
# JAVA_PAYLOAD_REV=1
# Install the headless Temurin 25 runtime under /rw_data/java.
#
# This package deliberately touches only /rw_data.  It installs no systemd
# unit, no QML, no /app or /etc file, and no updater/bootloader content.
set -u
umask 022
PATH="${JAVA_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}"
USB_DIR="${USB_DIR:-/media/usb}"
PAYLOAD="$USB_DIR/payload"

# Canonicalize before any check or write.  A literal-string guard is bypassed
# by /rw_data/, /rw_data/. and /rw_data/java/..; readlink -f resolves those
# (and symlinks), and every failure is a hard stop.
canon_dir() {
    _input=$1
    [ -n "$_input" ] || return 1
    case "$_input" in
        /*) ;;
        *) return 1 ;;
    esac
    _trim=$_input
    while [ "$_trim" != "/" ] && [ "${_trim%/}" != "$_trim" ]; do
        _trim=${_trim%/}
    done
    _canon=$(readlink -f -- "$_trim" 2>/dev/null) || return 1
    [ -n "$_canon" ] || return 1
    printf '%s\n' "$_canon"
}

RW_DIR=$(canon_dir "${JAVA_RW_DIR:-/rw_data/java}") || {
    echo "cannot canonicalize JAVA_RW_DIR: ${JAVA_RW_DIR:-/rw_data/java}" >&2
    exit 1
}
APPS_DIR=$(canon_dir "${JAVA_APPS_DIR:-/rw_data/java-apps}") || {
    echo "cannot canonicalize JAVA_APPS_DIR: ${JAVA_APPS_DIR:-/rw_data/java-apps}" >&2
    exit 1
}

case "$RW_DIR" in
    /|/rw_data) echo "refusing unsafe JAVA_RW_DIR: $RW_DIR" >&2; exit 1 ;;
esac
case "$APPS_DIR/" in
    "$RW_DIR/"*) echo "JAVA_APPS_DIR must be outside JAVA_RW_DIR" >&2; exit 1 ;;
esac
case "$RW_DIR/" in
    "$APPS_DIR/"*) echo "JAVA_RW_DIR must be outside JAVA_APPS_DIR" >&2; exit 1 ;;
esac

LOG="$RW_DIR/install.log"
OUT="$USB_DIR/run_stuff_result.txt"
SMACK_SELF=""
JAVA_SMACK="unavailable"

mkdir -p "$RW_DIR" 2>/dev/null || {
    echo "cannot create $RW_DIR" >&2
    exit 1
}
mkdir -p "$APPS_DIR" 2>/dev/null || {
    echo "cannot create $APPS_DIR" >&2
    exit 1
}
: > "$LOG" 2>/dev/null || true

log() {
    echo "$(date -u 2>/dev/null) $*" >> "$LOG" 2>/dev/null
}

result() {
    _result="$RW_DIR/result.txt"
    {
        echo "JAVA_PAYLOAD"
        echo "time=$(date -u 2>/dev/null)"
        echo "usb=$USB_DIR"
        echo "rw=$RW_DIR"
        echo "uid=$(id -u 2>/dev/null)"
        _line=
        for _line in "$@"; do
            echo "$_line"
        done
    } > "$_result" 2>&1 || {
        echo "cannot write $_result" >&2
        return 1
    }
    cp -f "$_result" "$OUT" 2>/dev/null || \
        log "WARNING: cannot copy result to $OUT (stick may be read-only)"
    return 0
}

fail() {
    log "FAIL: $*"
    result "status=FAIL" "error=$*"
    exit 1
}

log "install start: uid=$(id -u 2>/dev/null) usb=$USB_DIR rw=$RW_DIR apps=$APPS_DIR"
[ -f "$USB_DIR/manifest.md5" ] || fail "manifest.md5 missing"
( cd "$USB_DIR" && md5sum -c manifest.md5 ) > "$RW_DIR/manifest-check.log" 2>&1 || \
    fail "manifest check failed (see $RW_DIR/manifest-check.log)"

[ -f "$PAYLOAD/java/runtime.version" ] || fail "payload/java/runtime.version missing"
VERSION=$(cat "$PAYLOAD/java/runtime.version" 2>/dev/null | tr -d '\r\n')
[ -n "$VERSION" ] || fail "empty runtime.version"
SRC="$PAYLOAD/java/runtime-$VERSION"
[ -d "$SRC" ] || fail "runtime payload missing: $SRC"
[ -f "$SRC/SHA256SUMS" ] || fail "runtime SHA256SUMS missing"

# Remove leftovers from an interrupted previous install before measuring free
# space, so an ~80 MiB failed copy cannot trigger a false ENOSPC.
rm -rf "$RW_DIR"/runtime-*.tmp.* 2>/dev/null || true

PAYLOAD_KB=$(du -sk "$SRC" 2>/dev/null | awk '{print $1}')
[ -n "${PAYLOAD_KB:-}" ] || fail "cannot measure payload size"
MIN_KB="${JAVA_MIN_FREE_KB:-$((PAYLOAD_KB + 16384))}"
AVAIL_KB=$(df -Pk "$RW_DIR" 2>/dev/null | awk 'NR == 2 {print $4}')
[ -n "${AVAIL_KB:-}" ] || fail "cannot read free space for $RW_DIR"
[ "$AVAIL_KB" -ge "$MIN_KB" ] || \
    fail "insufficient space in $RW_DIR: need ${MIN_KB} KiB, have ${AVAIL_KB} KiB"

# Claim the directory only after the read-only gates above.  Restore requires
# this marker (and the canonical path checks) before it will delete anything.
printf 'SCOURGE_JAVA_RUNTIME\n' > "$RW_DIR/.scourge-java" || \
    fail "cannot write $RW_DIR/.scourge-java"

TMP="$RW_DIR/runtime-$VERSION.tmp.$$"
rm -rf "$TMP" 2>/dev/null || true
log "copying $SRC -> $TMP"
cp -Rp "$SRC/." "$TMP" || fail "runtime copy failed"
# exFAT/texfat can expose synthetic writable modes and may drop the execute
# bit; harden the copy before it becomes current.
chmod -R go-w "$TMP" || fail "cannot harden runtime permissions"
chmod 755 "$TMP/bin/"* 2>/dev/null || fail "cannot restore runtime executable modes"

( cd "$TMP" && sha256sum -c SHA256SUMS ) > "$RW_DIR/sha256-check.log" 2>&1 || \
    fail "runtime SHA256SUMS verification failed (see $RW_DIR/sha256-check.log)"

"$TMP/bin/java" -version > "$RW_DIR/version-check.log" 2>&1 || \
    fail "installed java -version failed (see $RW_DIR/version-check.log)"
grep -q 'openjdk version "25' "$RW_DIR/version-check.log" || \
    fail "unexpected java -version output (see $RW_DIR/version-check.log)"

DEST="$RW_DIR/runtime-$VERSION"
rm -rf "$DEST" 2>/dev/null || fail "cannot replace $DEST"
mv "$TMP" "$DEST" || fail "cannot finalize $DEST"
ln -sfn "runtime-$VERSION" "$RW_DIR/current" || fail "cannot update current symlink"

cat > "$RW_DIR/env.sh" <<EOF
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Generated by the SCOURGE Java runtime installer.
export JAVA_HOME="$RW_DIR/current"
export PATH="$RW_DIR/current/bin:\$PATH"
EOF
chmod 644 "$RW_DIR/env.sh" 2>/dev/null || true

# Keep one runtime.  An older package can be reinstalled for rollback; the
# current + SHA256SUMS make that a normal, verified operation.
for old in "$RW_DIR"/runtime-*; do
    [ -d "$old" ] || continue
    [ "$old" = "$DEST" ] && continue
    log "pruning old runtime: $old"
    rm -rf "$old" 2>/dev/null || log "WARNING: could not remove $old"
done

if [ "${JAVA_SKIP_SMACK:-0}" = "1" ]; then
    JAVA_SMACK="skipped"
else
    if command -v chsmack >/dev/null 2>&1 && [ -d /sys/fs/smackfs ]; then
        SMACK_SELF=$(cat /proc/self/attr/current 2>/dev/null | tr -d '\r\n')
        if [ -n "$SMACK_SELF" ]; then
            log "SMACK: applying '$SMACK_SELF' to bin/java"
            if chsmack -e "$SMACK_SELF" "$DEST/bin/java" >> "$LOG" 2>&1; then
                JAVA_SMACK=$(getfattr -n security.SMACK64EXEC --only-values \
                    "$DEST/bin/java" 2>/dev/null || true)
                [ -n "$JAVA_SMACK" ] || JAVA_SMACK="unreadable"
                [ "$JAVA_SMACK" = "$SMACK_SELF" ] || \
                    log "WARNING: SMACK readback '$JAVA_SMACK' != '$SMACK_SELF'"
            else
                JAVA_SMACK="set-failed"
                log "WARNING: chsmack failed for $DEST/bin/java"
            fi
        else
            JAVA_SMACK="unknown-self"
            log "WARNING: cannot read /proc/self/attr/current"
        fi
    else
        JAVA_SMACK="unavailable"
        log "SMACK: chsmack or smackfs unavailable; leaving the label unchanged"
    fi
fi

JAVA_VERSION=$("$RW_DIR/current/bin/java" -version 2>&1 | sed -n '1p')
sync 2>/dev/null || true
log "install complete: version=$VERSION smack=$JAVA_SMACK"
result \
    "status=OK" \
    "version=$VERSION" \
    "runtime=$DEST" \
    "current=$RW_DIR/current" \
    "java=$RW_DIR/current/bin/java" \
    "apps=$APPS_DIR" \
    "java_version=$JAVA_VERSION" \
    "smack=$JAVA_SMACK" \
    "free_kb=$AVAIL_KB" \
    "manifest=OK" \
    "sha256=OK"
exit 0
