#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# BATTERY_PAYLOAD_REV=1
# Battery preconditioning app for Ioniq 5 Gen5W (260128).
#
# Put this file, payload/, backup/, backup_current/ and manifest.md5 at the
# ROOT of a FAT/exFAT USB stick. The initial-root failsafe runs it as root
# within ~8 s of a content change.
#
# Installs:
#   /app/share/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Advanced.qml
#   /app/share/AppHomeScreen/qml_eco3.0/HomeScreen/BatteryPage.qml
#   /app/share/AppHomeScreen/qml_eco3.0/component/BatterySwitch.qml
#   /app/share/AppHomeScreen/qml_eco3.0/component/BatteryTile.qml
#   /app/share/AppHomeScreen/battery/{battery_icon_n,battery_icon_d}.png
#   /rw_data/battery/battery_daemon
#   /etc/systemd/system/battery-daemon.service
#
# It backs up the current Advanced QML to /rw_data/battery/backup/<timestamp>/
# and to the USB backup_current/. It does not flash firmware and does not touch
# the updater, bootloader, or partitions.
set -u

# The unit runs a Korean locale: `date` prints "2026. 11. 23. (월)" and tools
# emit localised text, which lands in combo.log, the probes and the UI popup.
# Pin the C locale so every log line is ASCII.
export LC_ALL=C LANG=C LC_TIME=C 2>/dev/null || true
PATH="${BATTERY_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}"
USB_DIR="${USB_DIR:-/media/usb}"
PAYLOAD="$USB_DIR/payload"
BACKUP="$USB_DIR/backup"
QML_ROOT="${BATTERY_QML_ROOT:-/app/share/AppHomeScreen}"
QML_DIR="$QML_ROOT/qml_eco3.0"
ASSET_DIR="$QML_ROOT/battery"
RW_DIR="${BATTERY_RW_DIR:-/rw_data/battery}"
LOG="$RW_DIR/install.log"
OUT="$USB_DIR/run_stuff_result.txt"
SERVICE="${BATTERY_SERVICE:-/etc/systemd/system/battery-daemon.service}"
# The daemon and its helpers are stamped with this exact label.  On the car
# this is read from the installer's own /proc/self/attr/current ("_" in the
# normal initial-root/USB flow); BATTERY_SMACK_SELF is a test-only override.
SMACK_SELF="${BATTERY_SMACK_SELF:-}"

ADV="HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
NEW_FILES="HomeScreen/BatteryPage.qml
component/BatterySwitch.qml
component/BatteryTile.qml"

mkdir -p "$RW_DIR" 2>/dev/null
mkdir -p "$RW_DIR/backup" 2>/dev/null
: > "$LOG" 2>/dev/null

log() {
    echo "$(date -u 2>/dev/null) $*" >> "$LOG" 2>/dev/null
}

result() {
    {
        echo "BATTERY_PAYLOAD"
        echo "time=$(date -u 2>/dev/null)"
        echo "usb=$USB_DIR"
        echo "qml_dir=$QML_DIR"
        echo "uid=$(id -u 2>/dev/null)"
        echo "smack_self=${SMACK_SELF:-}"
        echo "$@"
    } > "$OUT" 2>&1
    cp -f "$OUT" "$RW_DIR/result.txt" 2>/dev/null
}

fail() {
    log "FAIL: $*"
    result "status=FAIL" "error=$*"
    exit 1
}

# The privileged side of the app runs in the installer's own SMACK context.
# This is not best-effort: a failed label would silently leave the daemon
# (and every dbus helper it forks) unable to reach App::micommanager.
smack_init() {
    if [ -n "${BATTERY_SMACK_SELF:-}" ]; then
        SMACK_SELF=$BATTERY_SMACK_SELF
        log "SMACK: using override label '$SMACK_SELF'"
        return 0
    fi
    grep -q ' smackfs ' /proc/mounts 2>/dev/null || fail "smackfs is not mounted (SMACK is required)"
    SMACK_SELF=$(cat /proc/self/attr/current 2>/dev/null | tr -d '\r\n')
    [ -n "$SMACK_SELF" ] || fail "cannot read /proc/self/attr/current"
    log "SMACK: installer label '$SMACK_SELF'"
}

set_exec_label() {
    _bin=$1
    command -v chsmack >/dev/null 2>&1 || fail "chsmack missing (SMACK is required)"
    chsmack -e "$SMACK_SELF" "$_bin" >> "$LOG" 2>&1 || fail "exec label failed: $_bin"
    _actual=$(getfattr -n security.SMACK64EXEC --only-values "$_bin" 2>/dev/null || true)
    if [ -z "$_actual" ]; then
        log "WARNING: cannot read back SMACK64EXEC for $_bin; inherited process label must be verified"
    elif [ "$_actual" != "$SMACK_SELF" ]; then
        fail "exec label readback failed: $_bin (expected '$SMACK_SELF', got '$_actual')"
    fi
    log "exec label readback: $_bin = ${_actual:-unverified}"
}

# Copy a QML file with verified, retried bytes (see install_io.sh io_copy).
# A reinstall can meet an old file created by a previous payload with a
# different label; io_copy keeps the destination inode (and its SMACK
# attributes) and never unlinks to force a retry.
copy_qml_file() {
    io_copy "$1" "$2"
}

mount -o remount,rw "$USB_DIR" 2>/dev/null || true

log "=== battery payload start usb=$USB_DIR ==="
smack_init

[ -d "$PAYLOAD" ] || fail "payload directory missing: $PAYLOAD"
[ -d "$BACKUP" ] || fail "backup directory missing: $BACKUP"
[ -f "$USB_DIR/manifest.md5" ] || fail "manifest.md5 missing"
[ -d "$QML_DIR" ] || fail "expected QML directory missing: $QML_DIR (not qml_eco3.0?)"

if ! ( cd "$USB_DIR" && md5sum -c manifest.md5 ) > "$RW_DIR/manifest-check.log" 2>&1; then
    log "manifest check failed"
    fail "manifest check failed (see /rw_data/battery/manifest-check.log)"
fi
log "manifest OK"

# Pick the Advanced variant that matches what is on the car.
cur="$QML_DIR/$ADV"
ref="$BACKUP/AppHomeScreen/qml_eco3.0/$ADV"
[ -f "$cur" ] || fail "target file missing: $cur"
cur_md5=$(md5sum "$cur" | cut -d' ' -f1)
ref_md5=$(md5sum "$ref" | cut -d' ' -f1)

variant=plain
already=0
hello_mismatch=0
if grep -q 'BATTERY_PAYLOAD_260128' "$cur" 2>/dev/null; then
    already=1
    if grep -q 'HELLO_PAYLOAD_260128' "$cur" 2>/dev/null && \
       grep -q 'WIFI_PAYLOAD_260128' "$cur" 2>/dev/null; then
        variant=hello_wifi
    elif grep -q 'WIFI_PAYLOAD_260128' "$cur" 2>/dev/null; then
        variant=wifi
    elif grep -q 'HELLO_PAYLOAD_260128' "$cur" 2>/dev/null; then
        variant=hello
    fi
elif grep -q 'WIFI_PAYLOAD_260128' "$cur" 2>/dev/null; then
    if grep -q 'HELLO_PAYLOAD_260128' "$cur" 2>/dev/null; then
        variant=hello_wifi
    else
        variant=wifi
    fi
elif grep -q 'HELLO_PAYLOAD_260128' "$cur" 2>/dev/null; then
    variant=hello
elif [ "$cur_md5" = "$ref_md5" ]; then
    variant=plain
else
    fail "unknown QML version for $ADV (current=$cur_md5 ref=$ref_md5)"
fi
# The Hello base check only means something on a FIRST install over a
# Hello-patched file.  On reinstall the current file is battery+hello patched
# and can never match the pre-battery base, so the warning would fire on
# every reinstall.
if [ "$variant" = "hello" ] && [ "$already" = 0 ]; then
    hello_base_md5=$(cat "$PAYLOAD/hello_base.md5" 2>/dev/null | cut -d' ' -f1)
    if [ -n "$hello_base_md5" ] && [ "$cur_md5" != "$hello_base_md5" ]; then
        hello_mismatch=1
        log "WARNING: Hello QML differs from the known artifact; installing anyway"
    fi
fi
log "variant=$variant already_patched=$already hello_mismatch=$hello_mismatch"

# Back up the file we are about to replace (original or Hello-patched).
STAMP=$(date -u +%Y%m%dT%H%M%SZ 2>/dev/null)
[ -n "$STAMP" ] || STAMP=manual
BACKUP_HU="$RW_DIR/backup/$STAMP"
mkdir -p "$BACKUP_HU/HomeScreen" 2>/dev/null || fail "cannot create $BACKUP_HU"
cp -f "$cur" "$BACKUP_HU/$ADV" || fail "backup failed: $ADV"
# Keep the first pre-install file as the restore point. On reinstall, the
# current file is already patched, so do not repoint latest/backup_current.
if [ "$already" = 0 ]; then
    ln -sfn "$BACKUP_HU" "$RW_DIR/backup/latest" 2>/dev/null || true
    mkdir -p "$USB_DIR/backup_current/AppHomeScreen/qml_eco3.0/HomeScreen" 2>/dev/null || true
    cp -f "$cur" "$USB_DIR/backup_current/AppHomeScreen/qml_eco3.0/$ADV" 2>/dev/null || true
fi
log "HU backup: $BACKUP_HU"

# The car's I-CAN is not always can0: on this unit all of can0/can1/can2 exist and
# every one of them was DOWN (2026-11-22 radio probe), and the daemon opens its
# interface itself. Pick the first interface that has a CAN parent and bring it up
# at 500 kbit/s, so "installed but no data" cannot be blamed on a downed link.
detect_can() {
    for _c in "${BATTERY_CAN_IFACE:-}" can0 can1 can2; do
        [ -n "$_c" ] || continue
        [ -e "/sys/class/net/$_c" ] || continue
        echo "$_c"
        return 0
    done
    echo can0
}
collect_dlt() {
    # The head unit's Qt messages do not go to stderr: they go to DLT, on the
    # separate /log_data partition. Copy the newest log's tail (plus the lines
    # that name a page or a QML error) so "Battery/Wifi page does not open" can be
    # read from the stick instead of guessed. Runs at the START of the bundle, so
    # the previous session's taps are already in the file.
    _dir=""
    for _c in "${SCOURGE_STICK_DIR:-}" "${USB_DIR:-}" "/rw_data"; do
        [ -n "$_c" ] || continue
        [ -d "$_c" ] && [ -w "$_c" ] || continue
        _dir="$_c"; break
    done
    [ -n "$_dir" ] || return 0
    _out="$_dir/dlt_tail.txt"
    {
        echo "=== /log_data (newest first)"; ls -lt /log_data 2>/dev/null | head -n 12
        echo "=== recent files"; find /log_data -type f -newermt '-3 hours' 2>/dev/null | head -n 12
    } > "$_out" 2>&1
    # Every process logs through the same DLT daemon, so the page that failed is
    # not necessarily in the newest file: take the three newest and filter all of
    # them for page/QML lines, then keep bounded tails of each.
    # Scan every log touched in the last six hours, not just the newest: on this
    # unit the home-screen context is not in the newest offline trace (that one is
    # BluetoothManager noise), so picking files by mtime missed the UI completely.
    _files=$(find /log_data -type f -newermt '-6 hours' -size -20M 2>/dev/null | head -n 40)
    _qmlout="$_dir/qml_errors.txt"
    : > "$_qmlout" 2>/dev/null || true
    # Any stderr we captured from the UI in an earlier run, if it exists.  This
    # must come AFTER the truncation above, or the copies are wiped.
    for _c in /rw_data/apphome.err /rw_data/apphome.out /rw_data/applauncher.err \
              /rw_data/*homescreen*.log; do
        [ -f "$_c" ] || continue
        { echo "=== $_c (last 200 lines)"; tail -n 200 "$_c"; } >> "$_qmlout" 2>&1
    done
    for _f in $_files; do
        _hit=$(strings "$_f" 2>/dev/null | grep -inE 'BatteryPage|WifiPage|TweaksPage|is not a type|Cannot assign|Unable to assign|ReferenceError|TypeError|AppHomeScreen|qmlEngine' | tail -n 20)
        [ -n "$_hit" ] || continue
        { echo "=== $_f"; printf '%s\n' "$_hit"; } >> "$_qmlout" 2>&1
    done
    sync 2>/dev/null || true
    log "qml errors: $_qmlout ($(wc -l < "$_qmlout" 2>/dev/null) lines)"
    if [ -n "$_files" ]; then
        echo "=== qml/page matches (all of: $_files)" >> "$_out"
        for _f in $_files; do
            echo "--- $_f" >> "$_out"
            tail -c 300000 "$_f" 2>/dev/null | strings | \
                grep -iE 'batterypage|wifipage|tweakspage|apphomescreen|qml|is not a type|cannot assign|unable to assign|referenceerror|typeerror|wifi page|battery page' | \
                tail -n 60 >> "$_out" 2>&1
        done
        for _f in $_files; do
            echo "=== tail of $_f (last 120 KB)" >> "$_out"
            tail -c 120000 "$_f" >> "$_out" 2>/dev/null
        done
    else
        echo "=== no .dlt file found under /log_data" >> "$_out"
    fi
    sync 2>/dev/null || true
    log "dlt tail: $_out"
}

collect_dlt

CAN_IFACE=$(detect_can)
_candir=""
for _c in "${SCOURGE_STICK_DIR:-}" "${USB_DIR:-}" "/rw_data"; do
    [ -n "$_c" ] || continue
    [ -d "$_c" ] && [ -w "$_c" ] || continue
    _candir="$_c"; break
done
[ -n "$_candir" ] || _candir="$RW_DIR"
if [ -e "/sys/class/net/$CAN_IFACE" ]; then
    _canlog="$_candir/can_diag.txt"
    {
        echo "=== ip tooling"
        ip -V 2>&1 | head -n 2
        ls -l "$(command -v ip 2>/dev/null)" 2>&1
        echo "=== before"
        ip -d link show "$CAN_IFACE" 2>&1 | head -n 6
    } > "$_canlog" 2>&1
    ip link set "$CAN_IFACE" up type can bitrate 500000 >> "$_canlog" 2>&1 || \
        ip link set "$CAN_IFACE" up >> "$_canlog" 2>&1 || true
    _st=$(cat "/sys/class/net/$CAN_IFACE/operstate" 2>/dev/null || echo unknown)
    if [ "$_st" != "up" ]; then
        # iproute2 wants the bitrate configured while the link is down; the
        # one-shot form above silently fails on this vendor kernel, which left
        # can0 DOWN and the daemon reading nothing.
        ip link set "$CAN_IFACE" down >> "$_canlog" 2>&1 || true
        ip link set "$CAN_IFACE" type can bitrate 500000 restart-ms 100 >> "$_canlog" 2>&1 || \
            ip link set "$CAN_IFACE" type can bitrate 500000 >> "$_canlog" 2>&1 || true
        ip link set "$CAN_IFACE" up >> "$_canlog" 2>&1 || true
        sleep 1
        _st=$(cat "/sys/class/net/$CAN_IFACE/operstate" 2>/dev/null || echo unknown)
    fi
    {
        echo "=== after (operstate=$_st)"
        ip -d link show "$CAN_IFACE" 2>&1 | head -n 8
        ip -o link 2>/dev/null | grep -i can
        # Which driver owns it, and does the driver advertise a clock parameter at
        # all: "clock 0" in ip -d output is the reason every bitrate write fails, so
        # record the module, its parameters and the sysfs driver link.
        echo "=== driver"
        readlink -f "/sys/class/net/$CAN_IFACE/device/driver" 2>&1
        lsmod 2>/dev/null | grep -iE "can|ifx|mcp|flex" | head -n 5
        _mod=$(basename "$(readlink -f "/sys/class/net/$CAN_IFACE/device/driver" 2>/dev/null)" 2>/dev/null)
        [ -n "$_mod" ] && ls /sys/module/"$_mod"/parameters/ 2>/dev/null | head -n 10
        [ -n "$_mod" ] && for _p in /sys/module/"$_mod"/parameters/*; do [ -e "$_p" ] && echo "  param $(basename "$_p")=$(cat "$_p" 2>/dev/null)"; done
        echo "=== vendor can tooling"
        ls /app/bin 2>/dev/null | grep -iE "can|com" | head -n 10
        (command -v canconfig candump cansend 2>/dev/null || echo "no can-utils")
        systemctl list-units --type=service 2>/dev/null | grep -iE "can|micom" | head -n 5
        echo "=== dmesg can"
        dmesg 2>/dev/null | grep -i -E "can|mcp|ifx|flex" | tail -n 8
    } >> "$_canlog" 2>&1
    log "can iface=$CAN_IFACE operstate=$_st diag=$_canlog"
    # BMS precondition state (0x2AD) and BMS_04 (0x25D) are M-CAN: not on this
    # unit's can0 (I-CAN), so the daemon's can0 socket never sees them and
    # precond_confirmed stays false.  The micom broadcasts M-CAN frames on the
    # system bus as com.lge.car.micom.can.NotifyMCANEachMsg; a short bounded
    # capture puts the actual bytes next to the run, which is what settles the
    # decode instead of guessing from a dead can0 filter (2026-11-24).
    if command -v dbus-monitor >/dev/null 2>&1; then
        {
            echo "=== M-CAN/ICAN micom D-Bus capture (8 s)"
            ( dbus-monitor --system "type='signal',sender='com.lge.car.micom'" 2>&1 \
                  & _dp=$!; sleep 8; kill -TERM "$_dp" 2>/dev/null; wait "$_dp" 2>/dev/null ) | head -n 40
        } >> "$_canlog" 2>&1
        log "mcan dbus capture appended to $_canlog"
    else
        log "no dbus-monitor; M-CAN capture skipped"
    fi
fi


# Remount the rootfs rw while installing.
. "$PAYLOAD/lib/install_io.sh"
io_init || fail "cannot identify root mount"
trap 'io_restore_root || true' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP
io_settle_root || fail "root filesystem is not stably writable"

# Assets.
for f in "$PAYLOAD/assets/battery/"*; do
    [ -f "$f" ] || continue
    io_asset "$f" "$ASSET_DIR/$(basename "$f")"
done
chmod 644 "$ASSET_DIR/"* 2>/dev/null || true

# Daemon.  Remove the old file first: cp -f onto an existing inode keeps its
# extended attributes, including a stale SMACK64EXEC from an older install.
mkdir -p "$RW_DIR" 2>/dev/null || fail "cannot create $RW_DIR"
rm -f "$RW_DIR/battery_daemon" 2>/dev/null || true
cp -f "$PAYLOAD/bin/battery_daemon" "$RW_DIR/battery_daemon" || fail "daemon copy failed"
chmod 755 "$RW_DIR/battery_daemon" 2>/dev/null || true
chown root:root "$RW_DIR/battery_daemon" 2>/dev/null || true
set_exec_label "$RW_DIR/battery_daemon"

# Micom helper.  The vendor micom only accepts CAN sends from a small set of
# process names (AppHomeScreen, TmsRemoteApp, micommanager_userspace,
# CANService); see docs/MICOM_WHITELIST.md.  The vendor dbus-send binary is
# copied under the whitelisted name, and a tiny wrapper keeps it alive for
# the micom's /proc/<pid>/exe check.
HELPER_DIR="$RW_DIR/bin"
mkdir -p "$HELPER_DIR" 2>/dev/null || fail "cannot create $HELPER_DIR"
_src=$(command -v dbus-send 2>/dev/null || true)
[ -n "$_src" ] && [ -x "$_src" ] || fail "helper not found: dbus-send"
rm -f "$HELPER_DIR/CANService" 2>/dev/null || true
cp -f "$_src" "$HELPER_DIR/CANService" || fail "helper copy failed: CANService"
chmod 755 "$HELPER_DIR/CANService" 2>/dev/null || true
chown root:root "$HELPER_DIR/CANService" 2>/dev/null || true
set_exec_label "$HELPER_DIR/CANService"

cat > "$HELPER_DIR/micom-send" <<EOF
#!/bin/sh
# Micom CAN-send helper. The vendor service whitelists the sender's
# executable basename (CANService); see docs/MICOM_WHITELIST.md.
#   micom-send <array>                        raw SetCANMicomRecv (legacy)
#   micom-send raw <array>                    explicit raw form
#   micom-send fixed <idhi> <idlo> <start> <size> <data>
#                                             SetCANFixedMicomRecv
# --print-reply keeps this process alive while the micom resolves
# /proc/<pid>/exe. A missing reply is normal (factory senders use
# no-reply) and still counts as queued. A real transport error (unknown
# service/method, access denied) exits non-zero so callers can tell
# "frame queued" from "the micom was never reached".
send() {
    method=\$1
    shift
    out=\$("$HELPER_DIR/CANService" --system --type=method_call --print-reply \\
        --reply-timeout=500 --dest=com.lge.car.micom /Can \\
        "\$method" "\$@" 2>&1)
    rc=\$?
    [ \$rc -eq 0 ] && exit 0
    case "\$out" in
        *NoReply*|*"Did not receive a reply"*) exit 0 ;;
    esac
    printf '%s\n' "\$out" >&2
    exit \$rc
}
case "\$1" in
fixed)
    send com.lge.car.micom.can.SetCANFixedMicomRecv \\
        "array:byte:\$2,\$3" "int32:\$4" "int32:\$5" "int32:\$6"
    ;;
raw)
    shift
    ;;
esac
send com.lge.car.micom.can.SetCANMicomRecv "\$1"
EOF
chmod 755 "$HELPER_DIR/micom-send" 2>/dev/null || true
chown root:root "$HELPER_DIR/micom-send" 2>/dev/null || true
set_exec_label "$HELPER_DIR/micom-send"

# Session-bus Home/Star monitor.  This is not a micom sender and is not
# subject to the whitelist.
_src=$(command -v dbus-monitor 2>/dev/null || true)
[ -n "$_src" ] && [ -x "$_src" ] || fail "helper not found: dbus-monitor"
rm -f "$HELPER_DIR/dbus-monitor" 2>/dev/null || true
cp -f "$_src" "$HELPER_DIR/dbus-monitor" || fail "helper copy failed: dbus-monitor"
chmod 755 "$HELPER_DIR/dbus-monitor" 2>/dev/null || true
chown root:root "$HELPER_DIR/dbus-monitor" 2>/dev/null || true
set_exec_label "$HELPER_DIR/dbus-monitor"
# Remove the old dbus-send copy; the daemon no longer uses it for micom.
rm -f "$HELPER_DIR/dbus-send" 2>/dev/null || true

# Expected label for the daemon's self-check, and fresh runtime files so an
# old labeled status/log cannot block the new process or the QML file path.
printf '%s\n' "$SMACK_SELF" > "$RW_DIR/smack_expected" || fail "cannot write smack_expected"
chmod 644 "$RW_DIR/smack_expected" 2>/dev/null || true
chown root:root "$RW_DIR/smack_expected" 2>/dev/null || true
rm -f "$RW_DIR/status.json" "$RW_DIR/daemon.log" 2>/dev/null || true

# systemd unit.
io_copy "$PAYLOAD/systemd/battery-daemon.service" "$SERVICE" || fail "service copy failed"
chmod 644 "$SERVICE" 2>/dev/null || true
chown root:root "$SERVICE" 2>/dev/null || true

# QML.
installed=""
src="$PAYLOAD/qml/AppHomeScreen/qml_eco3.0/$ADV"
adv_base="${ADV%.qml}"
case "$variant" in
    hello)
        src="$PAYLOAD/qml/AppHomeScreen/qml_eco3.0/${adv_base}.hello.qml"
        ;;
    wifi)
        src="$PAYLOAD/qml/AppHomeScreen/qml_eco3.0/${adv_base}.wifi.qml"
        ;;
    hello_wifi)
        src="$PAYLOAD/qml/AppHomeScreen/qml_eco3.0/${adv_base}.hello_wifi.qml"
        ;;
esac
[ -f "$src" ] || fail "payload variant missing: $src"
label=$(getfattr -n security.SMACK64 --only-values "$cur" 2>/dev/null || true)
copy_qml_file "$src" "$cur" "$ADV" || fail "copy failed: $ADV (see $LOG)"
chmod 644 "$cur" 2>/dev/null || true
chown root:root "$cur" 2>/dev/null || true
[ -n "$label" ] && setfattr -n security.SMACK64 -v "$label" "$cur" 2>/dev/null || true
installed="$installed $ADV($variant)"

for f in $NEW_FILES; do
    src="$PAYLOAD/qml/AppHomeScreen/qml_eco3.0/$f"
    dst="$QML_DIR/$f"
    [ -f "$src" ] || fail "payload file missing: $src"
    mkdir -p "$QML_DIR/$(dirname "$f")" 2>/dev/null || fail "cannot create dir for $f"
    copy_qml_file "$src" "$dst" "$f" || fail "copy failed: $f (see $LOG)"
    chmod 644 "$dst" 2>/dev/null || true
    chown root:root "$dst" 2>/dev/null || true
    installed="$installed $f"
done
log "installed:$installed"

# Clear QML caches.
rm -rf /root/.cache/qtshadercache 2>/dev/null || true
rm -rf /root/.cache/AppHomeScreen 2>/dev/null || true
rm -rf /tmp/qmlcache 2>/dev/null || true
rm -rf /tmp/qt-qml-cache 2>/dev/null || true

sync

# Start the daemon now and enable it for future boots. This must happen while
# / is still rw because `systemctl enable` writes a wants symlink.
daemon_status="not-started"
if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload >> "$LOG" 2>&1 || true
    if systemctl enable battery-daemon.service >> "$LOG" 2>&1; then
        if systemctl restart battery-daemon.service >> "$LOG" 2>&1; then
            daemon_status="systemd-ok"
        else
            daemon_status="systemd-restart-failed"
        fi
    else
        daemon_status="systemd-enable-failed"
    fi
fi
if [ "$daemon_status" != "systemd-ok" ]; then
    # Fallback: run it detached.
    if [ -x "$RW_DIR/battery_daemon" ]; then
        if ! pidof battery_daemon >/dev/null 2>&1; then
            ( "$RW_DIR/battery_daemon" --iface "$CAN_IFACE" --port 8088 \
                --status-file "$RW_DIR/status.json" \
                --micom-send "$RW_DIR/bin/micom-send" --micom-bin "$RW_DIR/bin/CANService" \
                --dbus-monitor "$RW_DIR/bin/dbus-monitor" \
                >> "$RW_DIR/daemon.log" 2>&1 & ) 2>/dev/null || true
            sleep 1
        fi
        if pidof battery_daemon >/dev/null 2>&1; then
            daemon_status="$daemon_status+manual"
        fi
    fi
fi

# Freshness check: a failed systemctl restart used to leave the OLD daemon
# running (new UI, old logic).  Verify the RUNNING binary matches the payload
# and force a restart if it does not.
payload_daemon_md5=$(md5sum "$PAYLOAD/bin/battery_daemon" 2>/dev/null | cut -d' ' -f1)
daemon_fresh=unknown
daemon_check_fresh() {
    _found=no-daemon
    for p in /proc/[0-9]*; do
        [ -r "$p/comm" ] || continue
        if [ "$(cat "$p/comm" 2>/dev/null)" = "battery_daemon" ]; then
            _run_md5=$(md5sum "$p/exe" 2>/dev/null | cut -d' ' -f1)
            if [ "$_run_md5" != "$payload_daemon_md5" ]; then
                _found=stale
            else
                _found=yes
            fi
        fi
    done
    echo "$_found"
}
if [ -n "$payload_daemon_md5" ] && command -v md5sum >/dev/null 2>&1; then
    daemon_fresh=$(daemon_check_fresh)
    if [ "$daemon_fresh" = "stale" ]; then
        log "running daemon is stale; forcing restart"
        for p in /proc/[0-9]*; do
            [ -r "$p/comm" ] || continue
            if [ "$(cat "$p/comm" 2>/dev/null)" = "battery_daemon" ]; then
                kill -TERM "${p#/proc/}" 2>/dev/null || true
            fi
        done
        sleep 1
        if command -v systemctl >/dev/null 2>&1; then
            systemctl restart battery-daemon.service >> "$LOG" 2>&1 || true
        fi
        if ! pidof battery_daemon >/dev/null 2>&1 && [ -x "$RW_DIR/battery_daemon" ]; then
            ( "$RW_DIR/battery_daemon" --iface can0 --port 8088 \
                --status-file "$RW_DIR/status.json" \
                --micom-send "$RW_DIR/bin/micom-send" --micom-bin "$RW_DIR/bin/CANService" \
                --dbus-monitor "$RW_DIR/bin/dbus-monitor" \
                >> "$RW_DIR/daemon.log" 2>&1 & ) 2>/dev/null || true
            sleep 1
        fi
        daemon_fresh=$(daemon_check_fresh)
        if [ "$daemon_fresh" = "stale" ]; then
            daemon_status="$daemon_status STALE-DAEMON-RUNNING"
        fi
    fi
fi
log "daemon=$daemon_status fresh=$daemon_fresh"

# The unit can report a successful restart and still have the process exit a
# moment later (Type=simple counts the job, not the process).  With journald
# absent there is no other record, so capture one foreground run.
if ! pidof battery_daemon >/dev/null 2>&1; then
    log "battery_daemon is not running; collecting daemon diagnostics"
    _ddir=""
    for _c in "${SCOURGE_STICK_DIR:-}" "${USB_DIR:-}" "/rw_data"; do
        [ -n "$_c" ] || continue
        [ -d "$_c" ] && [ -w "$_c" ] || continue
        _ddir="$_c"; break
    done
    [ -n "$_ddir" ] || _ddir="$RW_DIR"
    {
        echo "=== systemctl status"
        systemctl status --no-pager -l battery-daemon.service 2>&1 | head -n 30
        echo "=== systemctl show"
        systemctl show --no-pager -p Type -p ExecMainCode -p ExecMainStatus -p Result -p NRestarts -p ExecStart 2>&1 | head -n 12
        echo "=== unit file"
        sed -n '1,40p' /etc/systemd/system/battery-daemon.service 2>/dev/null
        echo "=== binary"
        ls -l "$RW_DIR/battery_daemon" 2>&1
        ls -Z "$RW_DIR/battery_daemon" 2>/dev/null | head -n 2
        echo "=== can interfaces"
        ip -o link 2>/dev/null | grep -i can
        echo "=== foreground run (3 s)"
        # busybox has no `timeout`, so start it, wait, then kill it.
        ( "$RW_DIR/battery_daemon" --iface "$CAN_IFACE" --port 8088 \
            --status-file "$RW_DIR/status.json" \
            --micom-send "$RW_DIR/bin/micom-send" --micom-bin "$RW_DIR/bin/CANService" \
            --dbus-monitor "$RW_DIR/bin/dbus-monitor" 2>&1 &
          _dp=$!; sleep 3; kill -TERM "$_dp" 2>/dev/null ) | head -n 40
        echo "=== install.log tail"
        tail -n 20 "$RW_DIR/install.log" 2>/dev/null
        echo "=== daemon.log tail"
        tail -n 20 "$RW_DIR/daemon.log" 2>/dev/null
    } > "$_ddir/daemon_diag.txt" 2>&1
    daemon_status="$daemon_status no-process"
fi

# Prove the process actually runs in the installer's context.  The file label
# gate above already guarantees the exec transition; this catches a systemd /
# shell path that bypassed it.  BATTERY_DAEMON_SMACK is a test-only override.
daemon_smack="${BATTERY_DAEMON_SMACK:-}"
if [ -z "$daemon_smack" ]; then
    for p in /proc/[0-9]*; do
        [ -r "$p/comm" ] || continue
        if [ "$(cat "$p/comm" 2>/dev/null)" = "battery_daemon" ]; then
            daemon_smack=$(cat "$p/attr/current" 2>/dev/null | tr -d '\r\n')
            break
        fi
    done
fi
smack_check=unknown
if [ -n "$daemon_smack" ]; then
    if [ "$daemon_smack" = "$SMACK_SELF" ]; then
        smack_check=ok
    else
        smack_check=mismatch
        log "SMACK MISMATCH: daemon='$daemon_smack' expected='$SMACK_SELF'"
    fi
fi
log "smack self=$SMACK_SELF daemon=${daemon_smack:-none} check=$smack_check"

io_restore_root || fail "cannot restore root mount state"

# Restart AppHomeScreen through AppLauncher (app id 11).
restart="not-attempted"
if [ "${SCOURGE_DEFER_UI_RESTART:-0}" = 1 ]; then
    # The master restarts AppHomeScreen once after every stage has installed,
    # instead of one restart per package (4-5 UI flickers per run, 2026-11-24).
    restart="deferred"
else
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
        if [ -n "$val" ]; then
            export "$var=$val"
        fi
    done
fi

if command -v dbus-send >/dev/null 2>&1; then
    dbus-send --session --type=method_call --dest=com.lge.avn.applauncher /Request \
        com.lge.avn.applauncher.request.terminate int32:11 >> "$LOG" 2>&1 || true
    sleep 2
    restart="dbus-launch-failed"
    for i in 1 2 3; do
        if dbus-send --session --type=method_call --dest=com.lge.avn.applauncher /Request \
            com.lge.avn.applauncher.request.launch int32:11 \
            string:AppHomeScreen string:/app/bin/AppHomeScreen >> "$LOG" 2>&1; then
            restart="dbus-ok"
            break
        fi
        sleep 2
    done
else
    restart="dbus-send-missing"
fi

if [ "$restart" != "dbus-ok" ]; then
    for p in /proc/[0-9]*; do
        [ -r "$p/comm" ] || continue
        if [ "$(cat "$p/comm" 2>/dev/null)" = "AppHomeScreen" ]; then
            kill -TERM "${p#/proc/}" 2>/dev/null || true
        fi
    done
    restart="$restart (reboot required if UI does not return)"
fi

fi
log "restart=$restart"
sync

final_status=OK
[ "$smack_check" = "mismatch" ] && final_status=FAIL
result "status=$final_status" "already_patched=$already" "variant=$variant" \
       "hello_mismatch=$hello_mismatch" \
       "backup=$BACKUP_HU" "installed=$installed" "daemon=$daemon_status" \
       "daemon_fresh=$daemon_fresh" \
       "daemon_smack=${daemon_smack:-none}" \
       "smack_check=$smack_check" "micom=CANService" \
       "restart=$restart" "asset_warnings=$ASSET_WARNINGS" "log=$LOG"
log "=== battery payload done ==="
[ "$final_status" = "OK" ] || exit 1
exit 0
