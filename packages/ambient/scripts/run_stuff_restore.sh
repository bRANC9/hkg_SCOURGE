#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# AMBIENT_RESTORE_REV=1
# Remove the Ambience app (reverse of run_stuff.sh).
#
# Copy this file over run_stuff.sh on the stick and plug it in.  The injection
# is reverted from the injector's backup, the page/tile/slider files and the
# icons are removed, the service is disabled and the daemon files are deleted.
# The configuration under /rw_data/ambient survives (config.conf, presets.conf)
# so a reinstall keeps the driver's settings; backups stay too.
set -u
PATH="${AMBIENT_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}"
USB_DIR="${USB_DIR:-/media/usb}"
QML_ROOT="${AMBIENT_QML_ROOT:-/app/share/AppHomeScreen}"
QML_DIR="$QML_ROOT/qml_eco3.0"
ASSET_DIR="$QML_ROOT/ambient"
RW_DIR="${AMBIENT_RW_DIR:-/rw_data/ambient}"
LOG="$RW_DIR/restore.log"
OUT="$USB_DIR/run_stuff_result.txt"
SERVICE="${AMBIENT_SERVICE:-/etc/systemd/system/ambient-daemon.service}"
ADV="HomeScreen/QHMI_HomeScreenMain_Advanced.qml"

mkdir -p "$RW_DIR" 2>/dev/null
: > "$LOG" 2>/dev/null

log() { echo "$(date -u 2>/dev/null) $*" >> "$LOG" 2>/dev/null; }
result() {
    {
        echo "AMBIENT_RESTORE"
        echo "time=$(date -u 2>/dev/null)"
        echo "$@"
    } > "$OUT" 2>&1
    cp -f "$OUT" "$RW_DIR/restore_result.txt" 2>/dev/null
}
fail() { log "FAIL: $*"; result "status=FAIL" "error=$*"; exit 1; }

log "=== ambient restore start ==="

# Stop the service before touching its files.
if command -v systemctl >/dev/null 2>&1; then
    systemctl disable --now ambient-daemon.service >> "$LOG" 2>&1 || true
fi
for p in /proc/[0-9]*; do
    [ -r "$p/comm" ] || continue
    if [ "$(cat "$p/comm" 2>/dev/null)" = "ambient_daemon" ]; then
        kill -TERM "${p#/proc/}" 2>/dev/null || true
    fi
done
sleep 1

cur="$QML_DIR/$ADV"
inject_state="skipped"
if [ -f "$RW_DIR/qml-backup/QHMI_HomeScreenMain_Advanced.qml" ] && [ -f "$cur" ]; then
    if sh "$USB_DIR/payload/lib/qml_inject.sh" --file "$cur" --revert \
            --backup-dir "$RW_DIR/qml-backup" >> "$LOG" 2>&1; then
        inject_state="reverted"
    else
        inject_state="failed"
    fi
fi
log "inject=$inject_state"

for f in "HomeScreen/AmbientPage.qml" "component/AmbientTile.qml" \
         "component/AmbientSlider.qml"; do
    rm -f "$QML_DIR/$f" 2>/dev/null || true
done
rm -f "$ASSET_DIR/ambient_icon_n.png" "$ASSET_DIR/ambient_icon_d.png" 2>/dev/null || true
rmdir "$ASSET_DIR" 2>/dev/null || true

rm -f "$SERVICE" 2>/dev/null || true
rm -f "$RW_DIR/ambient_daemon" 2>/dev/null || true
rm -rf "$RW_DIR/bin" 2>/dev/null || true
rm -f "$RW_DIR/status.json" "$RW_DIR/inject.block.qml" "$RW_DIR/smack_expected" 2>/dev/null || true

if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload >> "$LOG" 2>&1 || true
fi
rm -rf /root/.cache/qtshadercache /tmp/qmlcache /tmp/qt-qml-cache 2>/dev/null || true
sync

# Bring the home screen back with the restored QML.
restart="not-attempted"
launcher_pid=""
for p in /proc/[0-9]*; do
    [ -r "$p/comm" ] || continue
    if [ "$(cat "$p/comm" 2>/dev/null)" = "AppLauncher" ]; then
        launcher_pid="${p#/proc/}"
        break
    fi
done
if [ -n "$launcher_pid" ] && [ -r "/proc/$launcher_pid/environ" ]; then
    for var in DBUS_SESSION_BUS_ADDRESS XDG_RUNTIME_DIR WAYLAND_DISPLAY QT_QPA_PLATFORM; do
        val=$(tr '\0' '\n' < "/proc/$launcher_pid/environ" 2>/dev/null | grep "^$var=" | head -n 1 | cut -d= -f2-)
        [ -n "$val" ] && export "$var=$val"
    done
fi
if command -v dbus-send >/dev/null 2>&1; then
    dbus-send --session --type=method_call --dest=com.lge.avn.applauncher /Request \
        com.lge.avn.applauncher.request.terminate int32:11 >> "$LOG" 2>&1 || true
    sleep 2
    if dbus-send --session --type=method_call --dest=com.lge.avn.applauncher /Request \
        com.lge.avn.applauncher.request.launch int32:11 \
        string:AppHomeScreen string:/app/bin/AppHomeScreen >> "$LOG" 2>&1; then
        restart="dbus-ok"
    else
        restart="dbus-launch-failed"
    fi
fi
log "restart=$restart"

status=OK
[ "$inject_state" = "failed" ] && status=PARTIAL
result "status=$status" "inject=$inject_state" "restart=$restart" \
       "config=kept" "log=$LOG"
log "=== ambient restore done ==="
exit 0
