#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# WIFI_PAYLOAD_RESTORE=1
# Removes the WiFi app and restores the QML that was present before install.
# Rename/copy this file to run_stuff.sh on the USB root, then plug it in.
set -u
umask 077
PATH="${WIFI_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}"
USB_DIR="${USB_DIR:-/media/usb}"
PAYLOAD="$USB_DIR/payload"
QML_ROOT="${WIFI_QML_ROOT:-/app/share/AppHomeScreen}"
QML_DIR="$QML_ROOT/qml_eco3.0"
ASSET_DIR="$QML_ROOT/wifi"
RW_DIR="${WIFI_RW_DIR:-/rw_data/wifi}"
LOG="$RW_DIR/restore.log"
OUT="$USB_DIR/run_stuff_result.txt"
SERVICE="${WIFI_SERVICE:-/etc/systemd/system/wifi-daemon.service}"

ADV="HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
WIDE="HomeScreen/QHMI_HomeScreenMain_Wide.qml"
ALL="HomeScreen/Screen_HomeScreen_AllMenus.qml"
SETUP="HomeScreen/Screen_HomeScreen_Submenu_Setup.qml"
CUSTOM="component/QCustomizedMenuListAllowEmpty.qml"
THEME="component/common/Q_ThemeImage.qml"

mkdir -p "$RW_DIR" 2>/dev/null
: > "$LOG" 2>/dev/null
probe_radio() {   # probe_radio <label> - one bounded report, no guessing later
    _p="$USB_DIR/wifi_probe.txt"
    {
        echo "=== SCOURGE radio probe: ${1:-?} ($(date -u 2>/dev/null))"
        echo "--- uptime"; cut -d' ' -f1 /proc/uptime 2>&1
        echo "--- net devices"; ls /sys/class/net 2>&1
        echo "--- wlan0 link"; ip link show wlan0 2>&1 | head -n 6; ifconfig wlan0 2>&1 | head -n 6
        echo "--- operstate"; cat /sys/class/net/wlan0/operstate 2>&1
        echo "--- iw dev"; iw dev 2>&1 | head -n 16
        echo "--- rfkill"; [ -e /dev/rfkill ] && rfkill list 2>&1 | head -n 12 || echo "no /dev/rfkill in this kernel"
        echo "--- modules"; lsmod 2>/dev/null | grep -E '^(bcmdhd|dhd|brcmfmac|brcmutil|cfg80211|bluetooth|btusb|hci)' | head -n 12
        echo "--- wifi firmware load (dmesg)"; dmesg 2>/dev/null | grep -iE 'register interface|firmware version = wl|dhd_bus_download' | tail -n 6
        echo "--- time to wlan0"; dmesg 2>/dev/null | sed -n 's/^\[ *\([0-9.]*\)\].*Register interface \[wlan0\].*/registered_at=\1s/p' | tail -n 1
        echo "--- bluetooth state"; (hciconfig -a 2>/dev/null || true) | head -n 8
        echo "--- bluetooth connections"; (hcitool con 2>/dev/null || true) | head -n 6
        echo "--- bluetooth nodes"; ls /sys/class/bluetooth 2>&1; hciconfig -a 2>&1 | head -n 8
        echo "--- dmesg (net/bt)"; dmesg 2>/dev/null | grep -iE 'brcmfmac|wlan|bluetooth|hci0|firmware' | tail -n 12
        echo "--- services"
        for _u in connman supplicantmanager.service supplicantmanager.path wifi-daemon.service; do
            printf '%s active=%s enabled=%s\n' "$_u" "$(systemctl is-active "$_u" 2>/dev/null)" "$(systemctl is-enabled "$_u" 2>/dev/null)"
        done
        echo "--- connman technologies"; connmanctl technologies 2>&1 | head -n 12
        echo "--- connman state"; connmanctl state 2>&1 | head -n 4
        echo "--- our daemon pid"; pidof wifi_daemon 2>&1
        systemctl show -p MainPID -p ExecMainStatus wifi-daemon.service 2>&1
        echo "--- status.json"; head -c 400 "$RW_DIR/status.json" 2>&1; echo
        echo "--- daemon.log tail"; tail -n 12 "$RW_DIR/daemon.log" 2>&1
        echo "--- journal"; journalctl -u wifi-daemon.service -n 15 --no-pager 2>&1 | tail -n 15
        echo "--- smack of binary"; getfattr -n security.SMACK64 --only-values "$RW_DIR/wifi_daemon" 2>&1
        echo "--- listening ports"; (ss -ltn 2>/dev/null || netstat -ltn 2>/dev/null) | grep -E ':(809[0-9]|2222)' 2>&1
        echo
    } >> "$_p" 2>&1
    sync 2>/dev/null || true
}

log() { echo "$(date -u 2>/dev/null) $*" >> "$LOG" 2>/dev/null; }
result() {
    { echo "WIFI_RESTORE"; echo "time=$(date -u 2>/dev/null)"; echo "usb=$USB_DIR"; echo "$@"; } > "$OUT" 2>&1
    cp -f "$OUT" "$RW_DIR/restore-result.txt" 2>/dev/null
}
fail() { log "FAIL: $*"; result "status=FAIL" "error=$*"; exit 1; }

was_ro=0
cleanup_ro() {
    if [ "${IO_ACTIVE:-0}" = 1 ]; then io_restore_root || true; fi
}
trap cleanup_ro EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP

mount -o remount,rw "$USB_DIR" 2>/dev/null || true
[ -f "$USB_DIR/manifest.md5" ] || fail "manifest.md5 missing"
MD5SUM_BIN=md5sum
command -v md5sum >/dev/null 2>&1 || MD5SUM_BIN=md5sum.coreutils
if ! ( cd "$USB_DIR" && "$MD5SUM_BIN" -c manifest.md5 ) > "$RW_DIR/restore-manifest.log" 2>&1; then
    fail "manifest check failed"
fi

LATEST="$RW_DIR/backup/latest"
[ -d "$LATEST" ] || fail "no backup/latest; cannot restore"
[ ! -f /rw_data/wifi1b/wifi1b_daemon ] || fail "restore Wifi1b first; preserving its shared radio and QML"

. "$PAYLOAD/lib/install_io.sh"
io_init && io_settle_root || fail "root filesystem is not stably writable"

# Stop the service before synchronous cleanup: two independent controllers
# must never race to re-enable the radio while restore is removing files.
if command -v systemctl >/dev/null 2>&1; then
    systemctl stop wifi-daemon.service 2>/dev/null || true
    systemctl disable wifi-daemon.service 2>/dev/null || true
fi
if [ -x "$RW_DIR/wifi_daemon" ]; then
    # Best effort: a daemon that cannot take the radio down must not stop the
    # rest of the restore (QML, OEM unmask, connman). The 2026-11-22 run failed
    # here and left the unit with no radio owner at all.
    "$RW_DIR/wifi_daemon" --mode off >> "$LOG" 2>&1 || \
        log "warning: wifi_daemon --mode off failed; continuing with the restore"
fi
for p in /proc/[0-9]*; do
    executable=$(readlink "$p/exe" 2>/dev/null || true)
    case "$executable" in
        "$RW_DIR/wifi_daemon"|"$RW_DIR/wifi_daemon (deleted)"|\
        "$RW_DIR/bin/dropbearmulti"|"$RW_DIR/bin/dropbearmulti (deleted)")
            kill -TERM "${p#/proc/}" 2>/dev/null || true ;;
    esac
done
# Remove only our namespaced chains. Legacy untagged INPUT rules cannot be
# distinguished from vendor/owner rules and are deliberately not guessed at.
for family in iptables ip6tables; do
    IPT=$(command -v "$family" 2>/dev/null || true)
    [ ! -x "$RW_DIR/bin/$family" ] || IPT="$RW_DIR/bin/$family"
    [ -n "$IPT" ] || continue
    for chain in WIFI_APP_SSH WIFI_APP_CP; do
        for attempt in 1 2 3 4; do
            "$IPT" -D INPUT -i wlan0 -j "$chain" 2>/dev/null || break
        done
        for attempt in 1 2 3 4; do
            "$IPT" -D OUTPUT -o wlan0 -j "$chain" 2>/dev/null || break
        done
        for attempt in 1 2 3 4; do
            "$IPT" -D OUTPUT -o wlan0 -j "$chain" 2>/dev/null || break
        done
        "$IPT" -F "$chain" 2>/dev/null || true
        "$IPT" -X "$chain" 2>/dev/null || true
    done
done

# Restore QML from the pre-install backup.
for f in "$ADV" "$WIDE" "$ALL" "$SETUP" "$CUSTOM" "$THEME"; do
    if [ -f "$LATEST/$f" ]; then
        label=$(getfattr -n security.SMACK64 --only-values "$QML_DIR/$f" 2>/dev/null || true)
        mkdir -p "$QML_DIR/$(dirname "$f")" 2>/dev/null
        io_copy "$LATEST/$f" "$QML_DIR/$f" || fail "restore failed: $f"
        chmod 644 "$QML_DIR/$f" 2>/dev/null || true
        chown 0:0 "$QML_DIR/$f" 2>/dev/null || true
        if [ -n "$label" ]; then
            # Restore is a recovery path: onlycap can make security-xattr
            # writes fail from the '_' installer context.  The restored file
            # already carries the inherited '_' floor label (readable by all),
            # so warn and continue instead of stranding the user.
            if setfattr -n security.SMACK64 -v "$label" "$QML_DIR/$f" 2>/dev/null; then
                actual=$(getfattr -n security.SMACK64 --only-values "$QML_DIR/$f" 2>/dev/null || true)
                [ "$actual" = "$label" ] || log "WARNING: SMACK64 readback mismatch on $f; expected $label, got '$actual'"
            else
                actual=$(getfattr -n security.SMACK64 --only-values "$QML_DIR/$f" 2>/dev/null || true)
                log "WARNING: cannot set SMACK64=$label on $f (onlycap?); keeping label '$actual'"
            fi
        fi
        log "restored $f"
    fi
done

# Remove new files/assets.
for f in HomeScreen/WifiPage.qml component/WifiTile.qml component/WifiSwitch.qml component/WifiButton.qml; do
    if cmp -s "$PAYLOAD/qml/AppHomeScreen/qml_eco3.0/$f" "$QML_DIR/$f"; then
        rm -f "$QML_DIR/$f" 2>/dev/null || true
    else
        log "kept changed/unknown component: $f"
    fi
done
for source in "$PAYLOAD/assets/wifi/"*; do
    [ -f "$source" ] || continue
    target="$ASSET_DIR/$(basename "$source")"
    if cmp -s "$source" "$target"; then rm -f "$target" 2>/dev/null || true; fi
done
rmdir "$ASSET_DIR" 2>/dev/null || true
rm -f "$SERVICE" 2>/dev/null || true

# Restore only our flag, from the immutable baseline for this exact path.
# Never pick a timestamp backup belonging to another file or replace newer
# unrelated Bluetooth settings. Legacy ambiguous backups are left alone.
vbt_changed=0
restore_vbt() {
    identity=$1
    vbt=$2
    bak="$RW_DIR/backup/vbt-$identity.original"
    applied="$RW_DIR/backup/vbt-$identity.applied"
    [ -f "$vbt" ] && [ -f "$bak" ] && [ -f "$applied" ] || return 0
    original=$(sed -n 's/.*"ENABLE_WIRELESS_CARPLAY"[[:space:]]*:[[:space:]]*\(true\|false\).*/\1/p' "$bak" | head -1)
    tmp="$vbt.wifi-restore.$$"
    cp -p "$vbt" "$tmp" || fail "cannot prepare VBT restore: $vbt"
    if [ -n "$original" ]; then
        sed "s/\(\"ENABLE_WIRELESS_CARPLAY\"[[:space:]]*:[[:space:]]*\)\(true\|false\)/\1$original/" "$vbt" > "$tmp" || fail "cannot restore VBT flag"
    elif cmp -s "$vbt" "$applied"; then
        cat "$bak" > "$tmp" || fail "cannot restore absent VBT flag"
    else
        rm -f "$tmp"
        fail "VBT baseline lacked flag and file changed independently; manual restore required: $vbt"
    fi
    label=$(getfattr -n security.SMACK64 --only-values "$vbt" 2>/dev/null || true)
    if [ -n "$label" ]; then
        setfattr -n security.SMACK64 -v "$label" "$tmp" 2>/dev/null \
            || log "WARNING: cannot set SMACK64=$label on VBT restore (onlycap?); keeping inherited label"
    fi
    mv -f "$tmp" "$vbt" || fail "VBT restore failed: $vbt"
    vbt_changed=1
}
restore_vbt app /app/data/vbt/VBTConfig.json
restore_vbt rw /rw_data/app_data/vbt/VBTConfig.json
[ "$vbt_changed" != 1 ] || systemctl restart vbtd.service >> "$LOG" 2>&1 || fail "vbtd restart failed"

# ---- mdnsd patch + carplay_mdnsd drop-in -------------------------------
# Undo the optional mdnsd "continue as root" patch (restore the stock
# binary from the installer backup) and remove the Restart=always drop-in
# so the vendor unit behaves like stock again.
MDNSD_BIN=/app/bin/mdnsd
MDNSD_STOCK_MD5=f63a65c4c2b0c011ae8ed17557b45c1b
MDNSD_PATCHED_MD5=f847373d7c5a6906453ba921bb790d22
MDNSD_BAK="$RW_DIR/backup/mdnsd.stock"
if [ -f "$MDNSD_BAK" ] && [ -f "$MDNSD_BIN" ]; then
    if [ "$("$MD5SUM_BIN" "$MDNSD_BIN" 2>/dev/null | cut -d' ' -f1)" != "$MDNSD_STOCK_MD5" ]; then
        [ "$("$MD5SUM_BIN" "$MDNSD_BAK" | cut -d' ' -f1)" = "$MDNSD_STOCK_MD5" ] || fail "mdnsd stock backup hash mismatch"
        [ "$("$MD5SUM_BIN" "$MDNSD_BIN" | cut -d' ' -f1)" = "$MDNSD_PATCHED_MD5" ] || fail "mdnsd changed independently; refusing restore"
        systemctl stop carplay_mdnsd.service >> "$LOG" 2>&1 || fail "cannot stop mdnsd for restore"
        if io_copy "$MDNSD_BAK" "$MDNSD_BIN"; then
            if [ "$("$MD5SUM_BIN" "$MDNSD_BIN" 2>/dev/null | cut -d' ' -f1)" = "$MDNSD_STOCK_MD5" ]; then
                log "restored stock mdnsd from backup"
                systemctl restart carplay_mdnsd.service >> "$LOG" 2>&1 || true
            else
                log "WARNING: mdnsd restore verification failed"
            fi
        else
            log "WARNING: cannot copy mdnsd backup over $MDNSD_BIN"
        fi
    fi
fi
if grep -q '^# SCOURGE_REGDOM=1$' /etc/modprobe.d/90-scourge-regdom.conf 2>/dev/null; then
    if io_remove /etc/modprobe.d/90-scourge-regdom.conf; then
        log "removed the regulatory domain drop-in (5 GHz AP hint)"
    else
        log "WARNING: cannot remove /etc/modprobe.d/90-scourge-regdom.conf"
    fi
fi
# The regulatory database is stock-free: either restore what was there or drop
# the pair we installed, never leave a half state behind.
for _f in regulatory.db regulatory.db.p7s; do
    if [ -f "/lib/firmware/$_f.scourge-bak" ]; then
        io_copy "/lib/firmware/$_f.scourge-bak" "/lib/firmware/$_f" && \
            io_remove "/lib/firmware/$_f.scourge-bak" && log "restored the stock /lib/firmware/$_f"
    elif [ -f "/lib/firmware/$_f" ]; then
        io_remove "/lib/firmware/$_f" && log "removed the installed /lib/firmware/$_f"
    fi
done
io_remove "$RW_DIR/regdb-version.txt" 2>/dev/null || true

if grep -q '^# SCOURGE_MDNSD_RESTART=1$' /etc/systemd/system/carplay_mdnsd.service.d/50-scourge-restart.conf 2>/dev/null; then
    if io_remove /etc/systemd/system/carplay_mdnsd.service.d/50-scourge-restart.conf; then
        rmdir /etc/systemd/system/carplay_mdnsd.service.d 2>/dev/null || true
        systemctl daemon-reload >> "$LOG" 2>&1 || true
        log "removed carplay_mdnsd restart drop-in"
    else
        log "WARNING: cannot remove carplay_mdnsd drop-in"
    fi
fi

# Remove only car: entries this installer created (recorded at install
# time).  An pre-existing car account from earlier owner tooling is kept.
remove_car() {
    f="$1"
    [ -f "$f" ] || return 0
    marker="$RW_DIR/backup/accounts/$(basename "$f")"
    [ -f "$marker" ] || return 0     # legacy/global ownership is ambiguous
    expected=$(cat "$marker")
    current=$(grep '^car:' "$f" 2>/dev/null || true)
    [ "$current" = "$expected" ] || { log "kept independently changed account in $f"; return 0; }
    tmp="/tmp/wifi-car-remove.$$"
    grep -v '^car:' "$f" > "$tmp" 2>/dev/null || true
    cat "$tmp" > "$f" 2>/dev/null || true
    rm -f "$tmp"
    rm -f "$marker"
    case "$f" in
        *shadow*) chmod 600 "$f" 2>/dev/null || true ;;
        *) chmod 644 "$f" 2>/dev/null || true ;;
    esac
}
remove_car /etc/passwd
remove_car /etc/passwd.legacy
remove_car /etc/passwd.secure
remove_car /etc/shadow
remove_car /etc/shadow.Global
if [ -L /home/car ]; then
    rm -f /home/car 2>/dev/null || true
fi

# Keep backups; remove runtime pieces and secrets.
rm -f "$RW_DIR/wifi_daemon" "$RW_DIR/status.json" "$RW_DIR/daemon.log" \
      "$RW_DIR/config.conf" "$RW_DIR/wifi.txt" "$RW_DIR/home.json" \
       "$RW_DIR/apply.result" "$RW_DIR/bt.state" "$RW_DIR/ssh.bound" \
       "$RW_DIR/ap.station-ip" \
      "$RW_DIR/wpa_supplicant.conf" "$RW_DIR/wpa_supplicant.log" \
      "$RW_DIR/wpa_supplicant.pid" "$RW_DIR/connman.agent" "$RW_DIR/scan.json" 2>/dev/null || true
rm -f "$RW_DIR/bin/dropbearmulti" "$RW_DIR/bin/dropbear" "$RW_DIR/bin/dropbearkey" \
      "$RW_DIR/bin/iptables" "$RW_DIR/bin/ip6tables" \
      "$RW_DIR/ssh/authorized_keys" "$RW_DIR/ssh/dropbear.pid" \
      "$RW_DIR/ssh/dropbear_ed25519_host_key" "$RW_DIR/ssh/dropbear_rsa_host_key" \
      "$RW_DIR/home/.ssh/authorized_keys" 2>/dev/null || true
rmdir "$RW_DIR/bin" "$RW_DIR/ssh" "$RW_DIR/home/.ssh" "$RW_DIR/home" 2>/dev/null || true

# Restore pre-existing private keys and icon assets rather than deleting
# unrelated owner tooling's files. First-install transaction paths are exact.
if [ -f "$LATEST/transaction/files" ]; then
    while IFS='|' read -r item existed destination; do
        [ "$existed" = 1 ] || continue
        case "$destination" in
            "$RW_DIR/ssh/authorized_keys"|"$RW_DIR/home/.ssh/authorized_keys"|"$QML_ROOT/wifi/"*|"$QML_ROOT/hello/"*|"$QML_ROOT/battery/"*)
                io_mkdir "$(dirname "$destination")" || fail "cannot restore original file directory"
                io_restore_entry "$LATEST/transaction/$item" "$destination" || fail "cannot restore original owner file: $destination"
                ;;
        esac
    done < "$LATEST/transaction/files"
fi

if command -v systemctl >/dev/null 2>&1; then
    # The installer masked the OEM WiFi manager so it could not respawn and
    # fight for the control socket.  Put the stock state back.
    for unit in supplicantmanager.path supplicantmanager.service; do
        state=$(cat "$RW_DIR/backup/$unit.state" 2>/dev/null || true)
        if [ -n "$state" ] && [ "$state" != masked ] && [ "$state" != masked-runtime ]; then
            systemctl unmask "$unit" 2>/dev/null || fail "cannot restore OEM unit mask: $unit"
        else
            log "preserved pre-existing/unknown mask state: $unit"
        fi
    done
    systemctl daemon-reload 2>/dev/null || true
    state=$(cat "$RW_DIR/backup/supplicantmanager.path.state" 2>/dev/null || true)
    case "$state" in enabled|static|indirect) systemctl start supplicantmanager.path 2>/dev/null || true;; esac
    # Unmasking the units is not enough: the installer also ran
    # "connmanctl disable wifi", so without this the OEM manager is unmasked but
    # its radio is still off - the head unit then behaves as if it had no WiFi
    # (and no Bluetooth, which shares the module) at all.
    if command -v connmanctl >/dev/null 2>&1; then
        connmanctl enable wifi >> "$LOG" 2>&1 || log "warning: connmanctl enable wifi failed"
        log "connman wifi re-enabled after restore"
        probe_radio "restore: after connman re-enable"
    fi
# RUN_DIR is not always set (the stage runs under `set -u` from the bundle and
    # from the test harness), and an unset var aborted the whole restore.
    rm -f "${RUN_DIR:-$RW_DIR}/wifi/active" 2>/dev/null || true
fi
sync

# Restart AppHomeScreen.
restart="not-attempted"
old_home_pid=$(pidof AppHomeScreen 2>/dev/null || true)
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

io_restore_root || fail "cannot restore root mount state"

sleep 3
new_home_pid=$(pidof AppHomeScreen 2>/dev/null || true)
status=OK
if [ -z "$new_home_pid" ] || [ "$new_home_pid" = "$old_home_pid" ]; then
    status=REBOOT_REQUIRED
fi
result "status=$status" "restored_from=$LATEST" "restart=$restart" "log=$LOG"
log "=== wifi restore done ==="
exit 0
