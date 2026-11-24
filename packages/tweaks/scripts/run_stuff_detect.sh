#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TWEAKS_DETECT=1
# Read-only inventory: what is installed on this head unit right now.
#
# Copy this file to run_stuff.sh on the USB root and plug the stick in. Nothing
# on the unit is modified: the only write is <stick>/inventory.txt (plus a copy
# under /rw_data/tweaks for the on-car logs). No daemon, service or QML is
# touched, no CAN frame is sent.
set -u
PATH="${TWEAKS_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}"
USB_DIR="${USB_DIR:-/media/usb}"
QML_ROOT="${TWEAKS_QML_ROOT:-/app/share/AppHomeScreen}"
QML_DIR="$QML_ROOT/qml_eco3.0"
STANDBY_ROOT="${TWEAKS_STANDBY_ROOT:-/app/share/AppStandbyClock}"
QM_DIR="${TWEAKS_QM_DIR:-$STANDBY_ROOT/localization/qm}"
RW_DIR="${TWEAKS_RW_DIR:-/rw_data/tweaks}"
OUT="$USB_DIR/inventory.txt"
BASES="qml_eco3.0 new_qml qml_genesis"
FILES="main.qml Screen_AppStandbyClock_Confirm.qml Screen_AppStandbyClock_Confirm_Profile.qml"

MD5=md5sum
command -v "$MD5" >/dev/null 2>&1 || MD5=md5sum.coreutils
SHA=sha256sum
command -v "$SHA" >/dev/null 2>&1 || SHA=sha256sum.coreutils

mount -o remount,rw "$USB_DIR" 2>/dev/null || true
mkdir -p "$RW_DIR" 2>/dev/null || RW_DIR="$USB_DIR"

md5() { "$MD5" "$1" 2>/dev/null | cut -d' ' -f1; }
marks() {
    grep -o 'HELLO_PAYLOAD_260128\|BATTERY_PAYLOAD_260128\|WIFI_PAYLOAD_260128\|WIFI1B_PAYLOAD_260128\|TWEAKS_PAYLOAD_260128\|REBOOT_PAYLOAD_260128' "$1" 2>/dev/null |
        sort -u | tr '\n' ','
}
svc() { systemctl is-active "$1" 2>/dev/null || echo unknown; }

ADV="$QML_DIR/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"

{
echo "SCOURGE_INVENTORY"
echo "time=$(date -u 2>/dev/null)"
echo "kernel=$(uname -r 2>/dev/null) $(uname -m 2>/dev/null)"
echo "uid=$(id -u 2>/dev/null)"
echo "installer_label=$(cat /proc/self/attr/current 2>/dev/null)"
echo

echo "== home screen QML =="
for f in HomeScreen/QHMI_HomeScreenMain_Advanced.qml \
         HomeScreen/QHMI_HomeScreenMain_Wide.qml \
         HomeScreen/Screen_HomeScreen_AllMenus.qml \
         HomeScreen/Screen_HomeScreen_Submenu_Setup.qml \
         component/QCustomizedMenuListAllowEmpty.qml; do
    p="$QML_DIR/$f"
    if [ -f "$p" ]; then
        printf '%-46s md5=%s markers=%s\n' "$f" "$(md5 "$p")" "$(marks "$p")"
    else
        printf '%-46s MISSING\n' "$f"
    fi
done
if [ -f "$ADV" ]; then
    echo "tiles=$(grep -o 'id: *tweaksTile\|id: *rebootTile\|id: *batteryTile1\|id: *wifiTile\|id: *wifi1bTile' "$ADV" 2>/dev/null | sed 's/id: *//' | sort -u | tr '\n' ',')"
else
    echo "tiles=unknown (no Advanced.qml)"
fi
echo

echo "== packages =="
printf '%-8s %-46s %-22s %s\n' pkg daemon service version
printf '%-8s %-46s %-22s %s\n' tweaks "$RW_DIR/tweaks_daemon" "$(svc tweaks-daemon.service)" \
    "$(sed -n 's/.*"build":"\([^"]*\)".*/\1/p' "$RW_DIR/status.json" 2>/dev/null | head -1)"
printf '%-8s %-46s %-22s %s\n' battery /rw_data/battery/battery_daemon "$(svc battery-daemon.service)" \
    "$(/rw_data/battery/battery_daemon --version 2>/dev/null | head -1)"
printf '%-8s %-46s %-22s %s\n' wifi /rw_data/wifi/wifi_daemon "$(svc wifi-daemon.service)" \
    "$(/rw_data/wifi/wifi_daemon --version 2>/dev/null | head -1)"
printf '%-8s %-46s %-22s %s\n' wifi1b /rw_data/wifi1b/wifi1b_daemon "$(svc wifi1b-daemon.service)" \
    "$(/rw_data/wifi1b/wifi1b_daemon --version 2>/dev/null | head -1)"
for d in "$RW_DIR" /rw_data/battery /rw_data/wifi /rw_data/wifi1b; do
    [ -d "$d" ] || continue
    printf 'state %-16s %s\n' "$(basename "$d")" \
        "$(ls "$d" 2>/dev/null | tr '\n' ' ' | cut -c1-120)"
done
echo

echo "== AppStandbyClock (boot disclaimer) =="
disclaimer_patched=0
for base in $BASES; do
    for f in $FILES; do
        p="$STANDBY_ROOT/$base/$f"
        if [ -f "$p" ]; then
            printf '%-46s %s\n' "$base/$f" \
                "$(grep -q 'TWEAKS_DISCLAIMER_260128' "$p" && echo patched || echo stock)$(grep -q 'TWEAKS_UNGATED_CONFIRM' "$p" && echo '+ungated')"
            grep -q 'TWEAKS_DISCLAIMER_260128' "$p" && disclaimer_patched=1
        else
            printf '%-46s MISSING\n' "$base/$f"
        fi
    done
done
echo "disclaimer_auto=$(cat "$RW_DIR/disclaimer_auto" 2>/dev/null || echo unset)"
echo "disclaimer_timeout_ms=$(cat "$RW_DIR/disclaimer_timeout_ms" 2>/dev/null || echo unset)"
echo

echo "== boot message (localization .qm) =="
bootmsg=unknown
if [ -f "$QM_DIR/translation_en-gb.qm" ]; then
    live=$("$SHA" "$QM_DIR/translation_en-gb.qm" 2>/dev/null | cut -d' ' -f1)
    echo "live_translation_en-gb.qm sha256=$live"
    want=$(sed -n 's/^\([0-9a-f]*\)  translation_en-gb\.qm$/\1/p' \
        "$USB_DIR/payload/bootmsg/MANIFEST.sha256" 2>/dev/null | head -1)
    if [ -n "$want" ]; then
        [ "$want" = "$live" ] && bootmsg=ours || bootmsg=stock
        echo "staged_in_this_stick=$want -> $bootmsg"
    else
        echo "(no staged manifest on this stick; cannot classify)"
    fi
else
    echo "translation_en-gb.qm MISSING"
fi
echo

echo "== verdict =="
verdict() {
    _name=$1
    _marker=$2
    _path=$3
    _state=absent
    if [ -f "$ADV" ] && grep -q "$_marker" "$ADV" 2>/dev/null; then _state=installed; fi
    [ -e "$_path" ] && _state=installed
    echo "$_name=$_state"
}
verdict hello HELLO_PAYLOAD_260128 "$QML_DIR/component/HelloTile.qml"
verdict battery BATTERY_PAYLOAD_260128 /rw_data/battery/battery_daemon
verdict wifi WIFI_PAYLOAD_260128 /rw_data/wifi/wifi_daemon
verdict tweaks TWEAKS_PAYLOAD_260128 "$RW_DIR/tweaks_daemon"
verdict reboot REBOOT_PAYLOAD_260128 "$QML_DIR/component/RebootTile.qml"
verdict wifi1b WIFI1B_PAYLOAD_260128 /rw_data/wifi1b/wifi1b_daemon
[ "$disclaimer_patched" = 1 ] && echo "disclaimer=patched" || echo "disclaimer=stock"
echo "bootmsg=$bootmsg"
echo "status=OK"
} > "$OUT" 2>&1

cp -f "$OUT" "$RW_DIR/inventory.txt" 2>/dev/null || true
echo "wrote $OUT"
exit 0
