#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TWEAKS_DISCLAIMER_ID=dev
# Single-purpose SCOURGE stick: installs ONLY the AppStandbyClock safety
# disclaimer QML patch (main.qml + both Confirm screens, per GUI base).
#
# It does not touch the home-screen QML, the daemons, the service, wrandr or
# the updater. The patch applies on the NEXT boot; nothing is restarted here.
# Built by scripts/build_disclaimer_stick.sh; restore with
# run_stuff_disclaimer_restore.sh renamed to run_stuff.sh.
set -u
PATH="${TWEAKS_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}"
USB_DIR="${USB_DIR:-/media/usb}"
PAYLOAD="$USB_DIR/payload"
STANDBY_ROOT="${TWEAKS_STANDBY_ROOT:-/app/share/AppStandbyClock}"
RW_DIR="${TWEAKS_RW_DIR:-/rw_data/tweaks}"
LOG="$RW_DIR/disclaimer_install.log"
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

# Audible status through the factory micom audio service (same proven pattern as
# packages/compose.sh). The installer copies its own dbus-send so the call runs
# in the same SMACK context as the working micom client. Beeps are non-fatal:
# 1 beep = done, 3 beeps = failed.
BEEP_DBUS="$RW_DIR/bin/dbus-send"
beep_init() {
    [ -x "$BEEP_DBUS" ] && return 0
    _src=/usr/bin/dbus-send
    [ -x "$_src" ] || _src=/bin/dbus-send
    [ -x "$_src" ] || return 1
    mkdir -p "$RW_DIR/bin" 2>/dev/null || return 1
    rm -f "$BEEP_DBUS" 2>/dev/null || true
    cp -f "$_src" "$BEEP_DBUS" 2>/dev/null || return 1
    chmod 755 "$BEEP_DBUS" 2>/dev/null || true
    chown root:root "$BEEP_DBUS" 2>/dev/null || true
    _label=$(cat /proc/self/attr/current 2>/dev/null | tr -d '\r\n')
    [ -n "$_label" ] && chsmack -e "$_label" "$BEEP_DBUS" 2>/dev/null || true
    [ -x "$BEEP_DBUS" ]
}
beep_once() {
    _b=$BEEP_DBUS
    [ -x "$_b" ] || _b=/usr/bin/dbus-send
    [ -x "$_b" ] || return 0
    "$_b" --system --print-reply --reply-timeout=1000 --dest=com.lge.car.micom \
        /com/lge/car/micom com.lge.car.micom.audio.SetBeepPlay \
        array:byte:0x00 >>"$LOG" 2>&1 && return 0
    sleep 0.3 2>/dev/null || sleep 1
    "$_b" --system --print-reply --reply-timeout=1000 --dest=com.lge.car.micom \
        /com/lge/car/micom com.lge.car.micom.audio.SetBeepPlay \
        array:byte:0x00 >>"$LOG" 2>&1 || true
}
beep_n() {
    _n=$1
    beep_init 2>/dev/null || true
    _i=0
    while [ "$_i" -lt "$_n" ]; do
        beep_once
        _i=$((_i + 1))
        if [ "$_i" -lt "$_n" ]; then
            sleep 0.5 2>/dev/null || sleep 1
        fi
    done
    return 0
}

result() {
    {
        echo "SCOURGE_DISCLAIMER"
        echo "time=$(date -u 2>/dev/null)"
        echo "usb=$USB_DIR"
        echo "uid=$(id -u 2>/dev/null)"
        echo "payload_id=$(sed -n 's/^# TWEAKS_DISCLAIMER_ID=//p' "$0" 2>/dev/null)"
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
    beep_n 3
    exit 1
}

# ---- preflight ----------------------------------------------------------
mount -o remount,rw "$USB_DIR" 2>/dev/null || true
log "=== disclaimer payload start usb=$USB_DIR ==="
[ -d "$PAYLOAD" ] || fail "payload directory missing: $PAYLOAD"
[ -f "$USB_DIR/manifest.md5" ] || fail "manifest.md5 missing"
[ -d "$STANDBY_ROOT" ] || fail "AppStandbyClock missing: $STANDBY_ROOT"
if ! ( cd "$USB_DIR" && "$MD5SUM_BIN" -c manifest.md5 ) > "$RW_DIR/manifest-check.log" 2>&1; then
    fail "manifest check failed (see /rw_data/tweaks/manifest-check.log)"
fi

# ---- backup -------------------------------------------------------------
STAMP=$(date -u +%Y%m%dT%H%M%SZ 2>/dev/null)
[ -n "$STAMP" ] || STAMP=manual
BACKUP_HU="$RW_DIR/backup/$STAMP"
[ -e "$BACKUP_HU" ] && BACKUP_HU="$BACKUP_HU.$$"
bases_found=""
for base in $BASES; do
    [ -d "$STANDBY_ROOT/$base" ] || continue
    bases_found="$bases_found $base"
    for f in $FILES; do
        src="$STANDBY_ROOT/$base/$f"
        [ -f "$src" ] || continue
        mkdir -p "$BACKUP_HU/AppStandbyClock/$base" 2>/dev/null || fail "cannot create $BACKUP_HU"
        cp -f "$src" "$BACKUP_HU/AppStandbyClock/$base/$f" || fail "backup failed: $base/$f"
        mkdir -p "$USB_DIR/backup_current/AppStandbyClock/$base" 2>/dev/null
        cp -f "$src" "$USB_DIR/backup_current/AppStandbyClock/$base/$f" 2>/dev/null || true
    done
done
[ -n "$bases_found" ] || fail "no AppStandbyClock GUI base found under $STANDBY_ROOT"
ln -sfn "$BACKUP_HU" "$RW_DIR/backup/latest" 2>/dev/null || true
log "HU backup: $BACKUP_HU (bases:$bases_found)"

# ---- rootfs rw ----------------------------------------------------------
. "$PAYLOAD/lib/install_io.sh"
io_init || fail "cannot identify root mount"
trap 'io_restore_root || true' EXIT
io_settle_root || fail "root did not become writable"

install_qml() {
    src="$1"
    dst="$2"
    [ -f "$src" ] || fail "payload QML missing: $src"
    label=$(getfattr -n security.SMACK64 --only-values "$dst" 2>/dev/null || true)
    io_mkdir "$(dirname "$dst")" || fail "cannot create dir for $dst"
    io_copy "$src" "$dst" || fail "copy failed: $dst"
    chmod 644 "$dst" 2>/dev/null || true
    chown 0:0 "$dst" 2>/dev/null || true
    [ -n "$label" ] && setfattr -n security.SMACK64 -v "$label" "$dst" 2>/dev/null || true
}

# ---- install ------------------------------------------------------------
installed=0
for base in $bases_found; do
    for f in $FILES; do
        src="$PAYLOAD/qml/AppStandbyClock/$base/$f"
        [ -f "$src" ] || continue
        install_qml "$src" "$STANDBY_ROOT/$base/$f"
        installed=$((installed + 1))
    done
    if grep -q 'TWEAKS_UNGATED_CONFIRM' "$STANDBY_ROOT/$base/Screen_AppStandbyClock_Confirm.qml" 2>/dev/null; then
        echo "base_${base}=installed" >> "$STATUS_FILE"
    else
        echo "base_${base}=UNVERIFIED" >> "$STATUS_FILE"
    fi
done
[ "$installed" -gt 0 ] || fail "nothing installed"

# ---- boot message translations (.qm) -----------------------------------
BOOTMSG_DIR="$PAYLOAD/bootmsg"
BOOTMSG_DEPLOY="$PAYLOAD/tools/deploy_boot_messages.sh"
if [ -d "$BOOTMSG_DIR" ] && [ -f "$BOOTMSG_DEPLOY" ]; then
    qm_count=$(ls "$BOOTMSG_DIR"/translation_*.qm 2>/dev/null | wc -l)
    if [ "$qm_count" -gt 0 ]; then
        log "boot messages: installing $qm_count .qm file(s)"
        if STAGE="$BOOTMSG_DIR" \
           TARGET="${TWEAKS_QM_TARGET:-/app/share/AppStandbyClock/localization/qm}" \
           BACKUP_ROOT="${TWEAKS_BOOTMSG_BACKUP:-$RW_DIR}" \
           EXPECTED_COUNT="$qm_count" \
           sh "$BOOTMSG_DEPLOY" >> "$LOG" 2>&1; then
            echo "bootmsg=installed" >> "$STATUS_FILE"
            log "boot messages installed; new text appears after the next boot"
        else
            echo "bootmsg=FAILED" >> "$STATUS_FILE"
            log "WARNING: boot message install failed (rolled back); the QML patch stays installed"
        fi
    fi
else
    echo "bootmsg=absent" >> "$STATUS_FILE"
fi

# ---- QML caches ---------------------------------------------------------
rm -rf /root/.cache/qtshadercache 2>/dev/null || true
rm -rf /tmp/qmlcache /tmp/qt-qml-cache 2>/dev/null || true
sync

log "installed $installed file(s); takes effect on the next boot"
echo "files=$installed" >> "$STATUS_FILE"
echo "bases=$(echo $bases_found | tr ' ' ',')" >> "$STATUS_FILE"
echo "backup=$BACKUP_HU" >> "$STATUS_FILE"
echo "disclaimer=next-boot" >> "$STATUS_FILE"
result OK
beep_n 1
