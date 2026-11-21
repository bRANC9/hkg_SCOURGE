#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TWEAKS_DISCLAIMER_ID=dev
# Restores the AppStandbyClock QML that was present before the disclaimer
# patch: the newest on-car backup if there is one, otherwise the stock
# reference shipped on this stick. Rename to run_stuff.sh and plug it in.
set -u
PATH="${TWEAKS_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}"
USB_DIR="${USB_DIR:-/media/usb}"
PAYLOAD="$USB_DIR/payload"
STANDBY_ROOT="${TWEAKS_STANDBY_ROOT:-/app/share/AppStandbyClock}"
RW_DIR="${TWEAKS_RW_DIR:-/rw_data/tweaks}"
LOG="$RW_DIR/disclaimer_restore.log"
OUT="$USB_DIR/run_stuff_result.txt"
BASES="qml_eco3.0 new_qml qml_genesis"
FILES="main.qml Screen_AppStandbyClock_Confirm.qml Screen_AppStandbyClock_Confirm_Profile.qml"

MD5SUM_BIN=md5sum
command -v "$MD5SUM_BIN" >/dev/null 2>&1 || MD5SUM_BIN=md5sum.coreutils

mkdir -p "$RW_DIR" 2>/dev/null || RW_DIR="$USB_DIR"
: > "$LOG" 2>/dev/null
STATUS_FILE="$RW_DIR/disclaimer_status.txt"
: > "$STATUS_FILE" 2>/dev/null

log() { echo "$(date -u 2>/dev/null) $*" >> "$LOG" 2>/dev/null; }

result() {
    {
        echo "SCOURGE_DISCLAIMER_RESTORE"
        echo "time=$(date -u 2>/dev/null)"
        echo "usb=$USB_DIR"
        echo "uid=$(id -u 2>/dev/null)"
        cat "$STATUS_FILE" 2>/dev/null
        echo "status=$1"
    } > "$OUT" 2>&1
    cp -f "$OUT" "$RW_DIR/result.txt" 2>/dev/null || true
    sync
}

fail() {
    log "FAIL: $*"
    echo "error=$*" >> "$STATUS_FILE" 2>/dev/null
    result FAIL
    exit 1
}

mount -o remount,rw "$USB_DIR" 2>/dev/null || true
log "=== disclaimer restore start usb=$USB_DIR ==="
[ -d "$STANDBY_ROOT" ] || fail "AppStandbyClock missing: $STANDBY_ROOT"
[ -d "$PAYLOAD" ] || fail "payload directory missing: $PAYLOAD"

# ---- pick the source tree ----------------------------------------------
SOURCE=""
if [ -d "$RW_DIR/backup/latest/AppStandbyClock" ]; then
    SOURCE="$RW_DIR/backup/latest/AppStandbyClock"
    echo "source=on-car-backup-latest" >> "$STATUS_FILE"
else
    NEWEST=$(ls -1d "$RW_DIR/backup"/*/AppStandbyClock 2>/dev/null | sort | tail -n 1)
    if [ -n "$NEWEST" ] && [ -d "$NEWEST" ]; then
        SOURCE="$NEWEST"
        echo "source=on-car-backup-newest" >> "$STATUS_FILE"
    elif [ -d "$USB_DIR/backup/AppStandbyClock" ]; then
        SOURCE="$USB_DIR/backup/AppStandbyClock"
        echo "source=stick-stock-reference" >> "$STATUS_FILE"
    fi
fi
[ -n "$SOURCE" ] || fail "no restore source (no on-car backup and no stock reference on the stick)"
log "restore source: $SOURCE"

. "$PAYLOAD/lib/install_io.sh"
io_init || fail "cannot identify root mount"
trap 'io_restore_root || true' EXIT
io_settle_root || fail "root did not become writable"

restore_qml() {
    src="$1"
    dst="$2"
    [ -f "$src" ] || return 0
    label=$(getfattr -n security.SMACK64 --only-values "$dst" 2>/dev/null || true)
    io_mkdir "$(dirname "$dst")" || fail "cannot create dir for $dst"
    io_copy "$src" "$dst" || fail "copy failed: $dst"
    chmod 644 "$dst" 2>/dev/null || true
    chown 0:0 "$dst" 2>/dev/null || true
    [ -n "$label" ] && setfattr -n security.SMACK64 -v "$label" "$dst" 2>/dev/null || true
    want=$("$MD5SUM_BIN" "$src" 2>/dev/null | cut -d' ' -f1)
    got=$("$MD5SUM_BIN" "$dst" 2>/dev/null | cut -d' ' -f1)
    [ -z "$want" ] || [ "$want" = "$got" ] || fail "verify failed: $dst"
}

restored=0
for base in $BASES; do
    [ -d "$SOURCE/$base" ] || continue
    for f in $FILES; do
        [ -f "$SOURCE/$base/$f" ] || continue
        restore_qml "$SOURCE/$base/$f" "$STANDBY_ROOT/$base/$f"
        restored=$((restored + 1))
    done
    if grep -q 'TWEAKS_UNGATED_CONFIRM' "$STANDBY_ROOT/$base/Screen_AppStandbyClock_Confirm.qml" 2>/dev/null; then
        echo "base_${base}=STILL_PATCHED" >> "$STATUS_FILE"
    else
        echo "base_${base}=stock" >> "$STATUS_FILE"
    fi
done
[ "$restored" -gt 0 ] || fail "no file restored from $SOURCE"

# ---- boot message translations (.qm) -----------------------------------
BOOTMSG_BACKUP=$(ls -1d "$RW_DIR"/bootmsg-backup-* 2>/dev/null | sort | tail -n 1)
if [ -n "$BOOTMSG_BACKUP" ] && [ -d "$BOOTMSG_BACKUP" ]; then
    qm_target="${TWEAKS_QM_TARGET:-/app/share/AppStandbyClock/localization/qm}"
    qm_restored=0
    for f in "$BOOTMSG_BACKUP"/translation_*.qm; do
        [ -f "$f" ] || continue
        restore_qml "$f" "$qm_target/$(basename "$f")"
        qm_restored=$((qm_restored + 1))
    done
    echo "bootmsg_restored=$qm_restored" >> "$STATUS_FILE"
    log "restored $qm_restored boot message .qm file(s) from $BOOTMSG_BACKUP"
else
    echo "bootmsg_restored=none" >> "$STATUS_FILE"
fi

rm -rf /root/.cache/qtshadercache 2>/dev/null || true
rm -rf /tmp/qmlcache /tmp/qt-qml-cache 2>/dev/null || true
sync

echo "files=$restored" >> "$STATUS_FILE"
echo "restore=next-boot" >> "$STATUS_FILE"
log "restored $restored file(s); takes effect on the next boot"
result OK
