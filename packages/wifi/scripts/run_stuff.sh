#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Ioniq 5 Gen5W_L EU 260128
# WIFI_PAYLOAD_REV=1
# WiFi / CarPlay AP / SSH app for Ioniq 5 Gen5W (260128).
#
# Put this file, payload/, backup/, manifest.md5 and README.txt at the ROOT
# of a FAT/exFAT USB stick.  The initial-root failsafe runs it as root within
# ~8 s of a content change.
#
# Installs:
#   /app/share/AppHomeScreen/qml_eco3.0/HomeScreen/WifiPage.qml
#   /app/share/AppHomeScreen/qml_eco3.0/component/Wifi{Tile,Switch,Button}.qml
#   patched Advanced / Wide / AllMenus / Setup QML
#   /app/share/AppHomeScreen/wifi/{wifi_icon_n,wifi_icon_d}.png
#   /rw_data/wifi/wifi_daemon
#   /rw_data/wifi/regd             (CRDA stand-in: the regdomain on 4.1)
#   /rw_data/wifi/bin/dropbearmulti
#   /etc/systemd/system/wifi-daemon.service
#   /etc/systemd/system/regd.service
#   user car (password electric) for SSH
#
# It backs up the current QML to /rw_data/wifi/backup/<timestamp>/ and to
# the USB backup_current/.  It does not flash firmware and does not touch
# the updater, bootloader or partitions.
set -u

# The unit runs a Korean locale: `date` prints "2026. 11. 23. (월)" and tools
# emit localised text, which lands in combo.log, the probes and the UI popup.
# Pin the C locale so every log line is ASCII.
export LC_ALL=C LANG=C LC_TIME=C 2>/dev/null || true
umask 077
PATH="${WIFI_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}"
USB_DIR="${USB_DIR:-/media/usb}"
STICK_ROOT="${SCOURGE_STICK_DIR:-$USB_DIR}"
# The root bundle passes USB_DIR=<this stage dir> and SCOURGE_STICK_DIR=<stick
# root>.  Files the user drops on the stick (authorized_keys, wifi.txt, static_ip,
# no_ui_restart) live at the ROOT, so they must be read from there - reading them
# from USB_DIR silently found nothing and the flags never applied.
PAYLOAD="$USB_DIR/payload"
BACKUP="$USB_DIR/backup"
QML_ROOT="${WIFI_QML_ROOT:-/app/share/AppHomeScreen}"
QML_DIR="$QML_ROOT/qml_eco3.0"
ASSET_DIR="$QML_ROOT/wifi"
RW_DIR="${WIFI_RW_DIR:-/rw_data/wifi}"
LOG="$RW_DIR/install.log"
OUT="$USB_DIR/run_stuff_result.txt"
SERVICE="${WIFI_SERVICE:-/etc/systemd/system/wifi-daemon.service}"

# Hardware facts measured once: shipped next to the daemon, so the install-time
# probe prints the file instead of re-running the expensive commands
# (firmware strings, iw phy, dbus introspection).  Refresh with
# SCOURGE_HWINFO_REFRESH=1 after a firmware update.
HWFILE="$RW_DIR/hwinfo.txt"
if [ ! -s "$HWFILE" ] && [ -f "$PAYLOAD/facts/hwinfo.txt" ]; then
    cp -f "$PAYLOAD/facts/hwinfo.txt" "$HWFILE" 2>/dev/null || true
fi
# AA cross-check binary: installed next to the daemon, run by the aa probe.
if [ -f "$PAYLOAD/bin/bt_owner.target" ]; then
    cp -f "$PAYLOAD/bin/bt_owner.target" "$RW_DIR/bin/bt_owner.target" 2>/dev/null || true
    chmod 755 "$RW_DIR/bin/bt_owner.target" 2>/dev/null || true
fi
if [ -f "$PAYLOAD/bin/vbtd_api.target" ]; then
    cp -f "$PAYLOAD/bin/vbtd_api.target" "$RW_DIR/bin/vbtd_api.target" 2>/dev/null || true
    chmod 755 "$RW_DIR/bin/vbtd_api.target" 2>/dev/null || true
fi
# AA protocol dumper (on-car measurement without a phone): nav/EV decoders plus the
# discovery parser.  Static binary, argv only, no network and no python on the unit.
if [ -f "$PAYLOAD/bin/aa_dump.target" ]; then
    cp -f "$PAYLOAD/bin/aa_dump.target" "$RW_DIR/bin/aa_dump.target" 2>/dev/null || true
    chmod 755 "$RW_DIR/bin/aa_dump.target" 2>/dev/null || true
fi
wifi_import=not-present

ADV="HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
WIDE="HomeScreen/QHMI_HomeScreenMain_Wide.qml"
ALL="HomeScreen/Screen_HomeScreen_AllMenus.qml"
SETUP="HomeScreen/Screen_HomeScreen_Submenu_Setup.qml"
CUSTOM="component/QCustomizedMenuListAllowEmpty.qml"
THEME="component/common/Q_ThemeImage.qml"

CAR_HASH='$6$ioniq5$/BDNqdHDOkg.wPr3W8PkQx0vbdQWmJBPkU9/.pwGNtL.v/IyXbR20nEEY/CYImn04cI8NlGDKK6A6kPA5noPZ.'

mkdir -p "$RW_DIR" 2>/dev/null
mkdir -p "$RW_DIR/backup" 2>/dev/null
mkdir -p "$RW_DIR/bin" 2>/dev/null
mkdir -p "$RW_DIR/ssh" 2>/dev/null
: > "$LOG" 2>/dev/null

probe_aa() {   # read-only AA/projection facts for the Android Auto work
    _p="$RW_DIR/aa_probe.txt"
    {
        echo "=== SCOURGE AA probe ($(date -u 2>/dev/null))"
        echo "--- wpa_cli status"
        (wpa_cli -i wlan0 status 2>&1 || echo "wpa_cli failed") | head -n 12
        echo "--- wlan0 addr"; ip -4 addr show wlan0 2>&1 | head -n 6
        echo "--- ap bssid (what the bootstrap advertises)"
        _apmac=""
        if command -v wpa_cli >/dev/null 2>&1; then
            _apmac=$(wpa_cli -i wlan0 status 2>/dev/null | sed -n 's/^bssid=//p' | head -n 1)
        fi
        if [ -n "$_apmac" ]; then
            echo "bssid_from=wpa_cli value=$_apmac (this is the AP BSSID)"
        else
            echo "bssid_from=wpa_cli empty (supplicant not in AP mode or no control socket)"
        fi
        echo "wlan0_mac=$(cat /sys/class/net/wlan0/address 2>/dev/null)   <-- WiFi/AP MAC, NOT the BT MAC"
        ifconfig wlan0 2>/dev/null | grep -i HWaddr
        echo "--- bt addresses (for comparison, from sysfs)"
        for _h in /sys/class/bluetooth/*; do
            [ -e "$_h/address" ] || continue
            echo "  $(basename "$_h") address=$(cat "$_h/address" 2>/dev/null)"
        done
        echo "--- our AP config (what the bootstrap must advertise)"
        grep -E '^(carplay_|wifi_mode|mode=|wlan_on|ssh=|psk=|net[0-9]_psk=)' "$RW_DIR/config.conf" 2>/dev/null | redact_psk | head -n 20
        echo "--- bluetooth processes"
        ps 2>/dev/null | grep -i -E 'vbtd|bluetooth' | grep -v grep | head -n 10
        echo "--- /sys/class/bluetooth"; ls /sys/class/bluetooth 2>&1
        echo "--- bluez presence"
        systemctl status bluetooth 2>/dev/null | head -n 3
        ls /usr/libexec/bluetooth 2>/dev/null || echo "no /usr/libexec/bluetooth"
        ls /usr/lib/bluetooth 2>/dev/null | head -n 5
        if command -v bluetoothd >/dev/null 2>&1; then ls -l "$(command -v bluetoothd)"; else echo "no bluetoothd in PATH"; fi
        ps 2>/dev/null | grep -i vbtd | grep -v grep | head -n 3
        echo "--- vendor bt binaries"
        ls /app/bin 2>/dev/null | grep -i -E 'bt|blue' | head -n 10
        echo "--- bluetooth deep (the RFCOMM question)"
        # The AA bootstrap needs an RFCOMM channel.  BlueZ is absent, so the only
        # on-unit hope is the vendor stack: record what it is, what it holds open
        # and whether it talks over D-Bus or its own socket.
        _bt=$(pidof vbtd 2>/dev/null | awk '{print $1}')
        [ -n "$_bt" ] && echo "vbtd pid=$_bt cmdline=$(tr '\0' ' ' < /proc/$_bt/cmdline 2>/dev/null)"
        [ -n "$_bt" ] && ls -l /proc/$_bt/fd 2>/dev/null | awk '{print "  fd", $9, $10, $11}' | head -n 12
        echo "  ttyS0: $(ls -l /dev/ttyS0 2>&1)"
        echo "  patchram: $(ls -l /tmp/bt_patchram.hcd 2>&1)"
        echo "  rfkill: $(rfkill list bluetooth 2>&1 | head -n 4)"
        echo "  hci devs: $(ls /sys/class/bluetooth 2>&1)"
        echo "  dbus services:"; busctl --no-pager list --no-legend 2>/dev/null | grep -iE "blue|bt|vbt" | head -n 8
        # The vendor stack documents its own D-Bus contract; introspection is
        # cheaper than reverse engineering vbtd (14 MB, stripped).  vbtd-51
        # carries com.lge.hkmc.avn.BluetoothService with the pairing surface
        # (BTM_SetPairableMode, AddPairedDevice, CancelPairing, ...), so the
        # method and property names come from the service itself.
        # The vendor BT stack is a CommonAPI adapter on the SESSION bus (env:
        # DBUS_SESSION_BUS_ADDRESS, connections from vbtd and BluetoothHMI), not
        # the system bus: the system-bus introspect came back empty on 2026-11-23.
        # So look at every session-bus name and introspect the candidates.
        echo "  AA enable setting (BluetoothHMI deviceSettings)"
        # The vendor UI has deviceSettings.phoneProjectionSettings.androidAutoSettings
        # .enableAndroidAuto.  Find the store it lives in BEFORE anyone patches a
        # 9 MB Qt binary: grep the candidate setting stores for the key.
        for _sp in /app/data /rw_data/app_data /Data/mnt-c /Data/mnt-wt /app/share/AppSetup/data /app/data/AppSetup; do
            [ -d "${_sp}" ] || continue
            _hit=$(grep -rl "enableAndroidAuto" "${_sp}" 2>/dev/null | head -n 3)
            [ -n "${_hit}" ] && printf "    %s -> %s\n" "${_sp}" "${_hit}"
        done
        _cfg=$(grep -rl "phoneProjectionSettings" /app/data /rw_data/app_data 2>/dev/null | head -n 2)
        if [ -n "${_cfg}" ]; then
            echo "    found in: ${_cfg}"
            for _f in ${_cfg}; do grep -o -m3 "enableAndroidAuto[^,}]*" "${_f}" 2>/dev/null | head -n 3; done
        else
            echo "    key not found in the plain-text stores (QSettings/db or compiled-in)"
        fi
        echo "  --- BT socket ownership (packages/aa bt_owner)"
        if [ -x "$RW_DIR/bin/bt_owner.target" ]; then
            "$RW_DIR/bin/bt_owner.target" 2>&1 | head -n 30
        else
            echo "    bt_owner.target not installed"
        fi
        echo "  --- video decoder path (H.264 for projection)"
        ldd /app/bin/AppProjection 2>/dev/null | grep -iE "ssl|crypto|va|mfx|omx|avcodec|gst|egl|gles|v4l" | head -n 12
        for _l in /usr/lib/libssl.so* /usr/lib/libcrypto.so* /usr/lib/libva.so* /usr/lib/libmfx*.so* \
                  /usr/lib/libOMX*.so* /usr/lib/libavcodec*.so* /usr/lib/libv4l2*.so*; do
            [ -e "$_l" ] && ls -l "$_l" 2>/dev/null | head -n 2
        done
        ls /usr/lib/gstreamer-1.0 2>/dev/null | head -n 10
        (command -v vainfo >/dev/null 2>&1 && vainfo 2>&1 | head -n 12) || echo "    no vainfo"
        echo "  --- USB port role (wired AA / bridge)"
        for _p in /sys/module/dwc3/parameters/*; do
            [ -f "${_p}" ] && printf "    dwc3.%%s=%%s\n" "$(basename "${_p}")" "$(cat "${_p}" 2>/dev/null)"
        done
        echo "    /sys/class/udc: $(ls /sys/class/udc 2>/dev/null | tr "\n" " ")"
        for _u in /sys/class/udc/*; do
            [ -d "${_u}" ] || continue
            printf "    %%s state=%%s function=%%s\n" "$(basename "${_u}")" "$(cat "${_u}/state" 2>/dev/null)" "$(cat "${_u}/function" 2>/dev/null | tr "\n" ",")"
        done
        ls /sys/bus/platform/drivers/dwc3 2>/dev/null | head -n 4
        echo "  --- media service (who holds the decoder)"
        ps -o pid,comm,args 2>/dev/null | grep -iE "media|codec|vaapi|player|projection" | grep -v grep | head -n 10
        # The service's own fd table says HOW it receives the encoded stream
        # (socket, shared memory, dmabuf), which is the missing link for our own
        # video path.  Cheap: one ls per matching pid, capped.
        for _pid in $(ps -o pid,comm 2>/dev/null | grep -iE "media|codec|vaapi|player|projection" | grep -v grep | awk '{print $1}' | head -n 3); do
            echo "    fd table of pid ${_pid} ($(tr '\\0' ' ' < /proc/${_pid}/cmdline 2>/dev/null | cut -c1-50)):"
            ls -l /proc/${_pid}/fd 2>/dev/null | head -n 14
        done
        for _m in /proc/[0-9]*/maps; do
            [ -r "${_m}" ] || continue
            if grep -qiE "libva\.|igfxcmrt|i965_drv|ImageProvider" "${_m}" 2>/dev/null; then
                _pid=${_m#/proc/}; _pid=${_pid%%/*}
                printf "    pid %%s (%%s) maps: %%s\\n" "${_pid}" "$(tr '\\0' ' ' < /proc/${_pid}/cmdline 2>/dev/null | cut -c1-60)" \
                    "$(grep -oiE "libva[a-z0-9_.-]*|igfxcmrt[0-9.]*|i965_drv_video|libImageProvider" "${_m}" 2>/dev/null | sort -u | tr '\\n' ' ')"
            fi
        done | head -n 12
        echo "  --- GPU / DRI"
        ls /dev/dri 2>/dev/null | head -n 6
        for _c in /sys/class/drm/card*/device/driver; do [ -e "${_c}" ] && printf "    %%s -> %%s\\n" "${_c}" "$(basename "$(readlink -f "${_c}")")"; done | head -n 4
        ls /usr/lib/dri 2>/dev/null | head -n 6
        echo "  --- VA-API runtime"
        if command -v vainfo >/dev/null 2>&1; then vainfo 2>&1 | head -n 14; else echo "    no vainfo"; fi
        echo "  --- zero-copy strings (EGL image / dmabuf)"
        strings -a /app/bin/AppProjection 2>/dev/null | grep -iE "eglCreateImage|EGL_EXT_image_dma|eglCreateStream|gralloc|dmabuf|NV12|I420" | sort -u | head -n 10 || true
        echo "  --- VBTConfig full (redacted)"
        for _v in /app/data/vbt/VBTConfig.json /rw_data/app_data/vbt/VBTConfig.json; do
            [ -f "${_v}" ] || continue
            echo "    ## ${_v}"
            sed -E 's/("(psk|passkey|pin|key)[^"]*"[[:space:]]*:[[:space:]]*)"[^"]*"/\1"<redacted>"/Ig' "${_v}" 2>/dev/null | head -n 60
        done
        echo "  session bus names:"
        busctl --user --no-pager list 2>/dev/null | head -n 30
        for _name in org.bluez org.bluez.Adapter.Adapter.v2_0 org.bluez.Adapter.Adapter v2.org.bluez.Adapter com.lge.hkmc.avn.BluetoothService; do
            _out=$(dbus-send --session --print-reply --reply-timeout=2000 --dest="$_name" / org.freedesktop.DBus.Introspectable.Introspect 2>&1)
            case "$_out" in *Interface*|*method*)
                echo "  == $_name"
                printf "%s\n" "$_out" | sed -n "s/.*<\(method\|property\|signal\) name=.\{0,1\}\([A-Za-z0-9_]*\).*/    \1 \2/p" | head -n 20 ;;
            esac
        done
        echo "  com.lge.hkmc.avn.BluetoothService on the system bus (for reference):"
        dbus-send --system --print-reply --reply-timeout=2000 --dest=com.lge.hkmc.avn.BluetoothService / org.freedesktop.DBus.Introspectable.Introspect 2>&1 | head -n 6
        echo "  session bus (CommonAPI adapter):"
        [ -f /tmp/session_appmgr.env ] && sed -n "1,4p" /tmp/session_appmgr.env
        busctl --user --no-pager list --no-legend 2>/dev/null | grep -iE "bt|blue|commonapi" | head -n 8
        echo "  vbtd strings (rfcomm/sdp/uuid):"
        strings -a /app/bin/vbtd 2>/dev/null | grep -iE "^(rfcomm|sdp|uuid|org.bluez|android)" | head -n 8
        echo "  vbt config keys:"; for _f in /app/data/vbt/VBTConfig.json /rw_data/app_data/vbt/VBTConfig.json; do [ -f "$_f" ] && { echo "    $_f"; grep -oE "\"[A-Z_]+\"" "$_f" 2>/dev/null | head -n 12; }; done
        echo "  bt dmesg:"; dmesg 2>/dev/null | grep -iE "hci|btusb|patchram|bluetooth" | tail -n 6
        echo "--- hciconfig"; (command -v hciconfig >/dev/null 2>&1 && hciconfig -a || echo "no hciconfig") 2>&1 | head -n 20
        echo "--- sdptool"; (command -v sdptool >/dev/null 2>&1 && sdptool browse local || echo "no sdptool") 2>&1 | head -n 30
        echo "--- bt services"
        for _u in bluetooth btd vbtd bluetoothd; do printf '%s active=%s\n' "$_u" "$(systemctl is-active "$_u" 2>/dev/null)"; done
        # Which vbtd file the running stack actually is: the AA API probe (dlsym in
        # the live process) needs this inventory to decide whether one file covers it
        # or the family has to be checked (2026-11-24, AA tab).
        echo "--- vbtd inventory"
        ls -l /app/bin/vbtd* 2>/dev/null | head -n 8
        for _v in /app/bin/vbtd*; do
            [ -f "$_v" ] || continue
            printf '%s %s\n' "$(md5sum "$_v" 2>/dev/null | cut -d' ' -f1)" "$_v"
        done
        echo "vbtd_pid=$(pidof vbtd 2>/dev/null)"
        echo "vbtd_cmdline=$(tr '\0' ' ' < /proc/$(pidof vbtd 2>/dev/null | cut -d' ' -f1)/cmdline 2>/dev/null)"
        echo "--- projection binaries"
        ls /app/bin 2>/dev/null | grep -i -E 'projection|auto|carplay' | head -n 12
        (sha256sum /app/bin/AppProjection 2>/dev/null || echo "no /app/bin/AppProjection")
        echo "--- session bus AA names (3 s)"
        ( dbus-monitor --session 2>&1 & _dp=$!; sleep 3; kill -TERM "$_dp" 2>/dev/null ) | grep -m3 -i 'android_auto' || echo "no android_auto on the session bus"
        echo "--- platform"
        uname -a 2>&1; (ldd --version 2>&1 | head -n 1)
        df -h /rw_data 2>&1; ls -la /rw_data 2>&1 | head -n 6
        echo "--- 5 GHz support"
        echo "5ghz_channel_lines=$(iw list 2>/dev/null | grep -c -i '5[0-9][0-9][0-9] MHz')"
        echo "--- ap interface candidates (which iface can host the AP)"
        echo "net_devices=$(ls /sys/class/net 2>/dev/null | tr '\n' ' ')"
        iw dev 2>&1 | head -n 40
        echo "--- wlan0 info"; iw dev wlan0 info 2>&1 | head -n 8
        echo "--- AP mode supported?"
        iw list 2>/dev/null | grep -m1 -A3 -i 'valid interface combinations' | head -n 6
    } >> "$_p" 2>&1
    sync 2>/dev/null || true
}

# The unit has neither wget nor nc; every daemon call goes through the tiny
# httpreq we ship.  Resolve it once: stage dir, combo root, stick root, an
# installed copy.  The 2026-11-23 run logged "no httpreq on the unit" for both
# ssh and the api walk although combo/bin/httpreq was on the stick.
find_httpreq(){
    for _cand in \
        "${USB_DIR}/bin/httpreq" \
        "${USB_DIR}/../bin/httpreq" \
        "${USB_DIR}/../../combo/bin/httpreq" \
        "${STICK_ROOT:-${USB_DIR}}/combo/bin/httpreq" \
        "${COMBO:-${USB_DIR}/..}/bin/httpreq" \
        "${RW_DIR}/bin/httpreq"; do
        [ -f "${_cand}" ] || continue
        [ -x "${_cand}" ] || chmod 755 "${_cand}" 2>/dev/null || true
        if [ -x "${_cand}" ]; then
            mkdir -p "${RW_DIR}/bin" 2>/dev/null || true
            [ "${_cand}" = "${RW_DIR}/bin/httpreq" ] || cp -f "${_cand}" "${RW_DIR}/bin/httpreq" 2>/dev/null || true
            printf '%s\n' "${_cand}"
            return 0
        fi
    done
    return 1
}

# Never leave key material in a probe report: it lands on the stick and gets
# shared.  Redact before anything is printed or copied.
redact_psk(){
    sed -E 's/^(psk|net[0-9]_psk|carplay_psk)=.*/\1=<redacted>/'
}

probe_radio() {   # probe_radio <label> - one bounded report, no guessing later
    # Write to the STICK ROOT: the stage dir is wiped by the next bundle copy, and
    # that is exactly how the report for a failed run disappeared.
    # Written to /rw_data, NOT to the stick: every write to the stick changes its
    # content digest, which re-triggers the loader and made the run repeat (heard
    # as a regular, heartbeat-like beep).  The files are copied out at the very end.
# The unit has no `timeout` (coreutils) and no busybox applet for it either:
# every "timeout 25 iw ... scan" printed "timeout: command not found" and the
# scan never ran (2026-11-23, wifi_probe showed 0 frequencies for that reason,
# not because the spectrum was empty).  Bound it in shell instead.
run_bounded(){
    _limit=$1; shift
    "$@" & _p=$!
    _i=0
    while [ "$_i" -lt "$_limit" ] && kill -0 "$_p" 2>/dev/null; do
        sleep 1; _i=$((_i + 1))
    done
    if kill -0 "$_p" 2>/dev/null; then
        kill "$_p" 2>/dev/null
        echo "(bounded out after ${_limit}s)"
    fi
    wait "$_p" 2>/dev/null
}
    # probe_radio <label> [fast]: fast skips the scans, 20-25 s each
    _mode=${2:-full}
    _p="$RW_DIR/wifi_probe.txt"
    {
        echo "=== SCOURGE radio probe: ${1:-?} ($(date -u 2>/dev/null))"
        echo "--- uptime"; cut -d' ' -f1 /proc/uptime 2>&1
        echo "--- net devices"; ls /sys/class/net 2>&1
        echo "--- wlan0 link"; ip link show wlan0 2>&1 | head -n 6; ifconfig wlan0 2>&1 | head -n 6
        echo "--- operstate"; cat /sys/class/net/wlan0/operstate 2>&1
        echo "--- iw dev"; iw dev 2>&1 | head -n 16
        # Band support, regulatory domain and what the radio actually sees.  The
        # imported list carries a 5 GHz SSID (the phone hotspot is 5 GHz only), and
        # an association that fails on one band looks exactly like a dead
        # supplicant in our own log - this is what tells the two apart.
        if [ -s "$HWFILE" ] && [ "${SCOURGE_HWINFO_REFRESH:-0}" != 1 ]; then
            echo "--- hardware facts (cached, no re-probe)"
            cat "$HWFILE" 2>/dev/null
        else
        echo "--- iw phy (bands)"
        ( iw phy phy#0 info 2>&1 || iw phy0 info 2>&1 ) | grep -E "Band|Frequency|MHz \[|Channel" | head -n 30
        echo "--- regdomain"; iw reg get 2>&1 | head -n 12
        # The dhd/bcmdhd driver keeps its OWN country in the firmware (nvram
        # ccode/regrev) and does not publish it to cfg80211: "iw reg get" shows
        # country 00 even though nvram says ccode=GB, so the world regdomain is
        # NOT proof that 5 GHz is unusable.  Record the real source of truth.
        echo "--- nvram country (the firmware side)"
        for _n in /lib/firmware/bcmdhd/nvram.txt /lib/firmware/nvram.txt; do [ -f "$_n" ] && { echo "  $_n"; grep -n -iE "^(ccode|regrev)=" "$_n" | head -4; }; done
        echo "--- dmesg country lines"; dmesg 2>/dev/null | grep -iE "country|regdomain|ccode" | tail -n 8
        echo "--- broadcom wl tool"; (command -v wl >/dev/null 2>&1 && wl country 2>&1 | head -2) || echo "  no wl tool"
        [ "${_mode}" = fast ] || { echo "--- scan frequencies seen"; run_bounded 8 iw dev wlan0 scan 2>&1 | grep "freq:" | head -n 20 || true; }
        echo "--- bcmdhd module params"; for _p in /sys/module/bcmdhd/parameters/*; do [ -f "$_p" ] && printf "  %s=%s\n" "$(basename "$_p")" "$(cat "$_p" 2>/dev/null)"; done
        # A world regdomain (country 00, DFS-UNSET) leaves every 5 GHz channel
        # PASSIVE-SCAN: the station may listen there but must not associate, and an
        # AP on channel 36 cannot start at all.  Proven 2026-11-23: only 2.4 GHz is
        # active here.  Try a country without persisting it, report, then restore.
        fi
        echo "--- regdomain trial (HU)"
        [ "${_mode}" = fast ] || iw reg set HU 2>&1 | head -n 2
        iw reg get 2>&1 | sed -n "1,12p"
        echo "--- 5 GHz visibility after the trial"
        [ "${_mode}" = fast ] || run_bounded 8 iw dev wlan0 scan 2>&1 | grep -E "^(BSS|SSID|freq)" | head -n 40
        [ "${_mode}" = fast ] || iw reg set 00 2>&1 | head -n 2
        [ "${_mode}" = fast ] || { echo "--- scan errors (unfiltered, 3 lines)"; run_bounded 6 iw dev wlan0 scan 2>&1 | head -n 3; }
        echo "--- rfkill"; [ -e /dev/rfkill ] && rfkill list 2>&1 | head -n 12 || echo "no /dev/rfkill in this kernel"
        echo "--- modules"; lsmod 2>/dev/null | grep -E '^(bcmdhd|dhd|brcmfmac|brcmutil|cfg80211|bluetooth|btusb|hci)' | head -n 12
        echo "--- kernel module inventory (dmesg)"; dmesg 2>/dev/null | grep -m2 "Modules linked in" | cut -c1-600
        echo "--- wifi firmware load (dmesg)"; dmesg 2>/dev/null | grep -iE 'register interface|firmware version = wl|dhd_bus_download' | tail -n 6
        echo "--- module load timing"
        dmesg 2>/dev/null | grep -iE 'bcmdhd|dhdpcie' | sed -n 's/^\[ *\([0-9.]*\)\].*/  \1s/p' | head -n 6
        echo "--- who loads bcmdhd"; grep -rl 'bcmdhd' /etc/modules /etc/modules-load.d /etc/modprobe.d 2>/dev/null | head -n 5
        echo "--- time to wlan0"; dmesg 2>/dev/null | sed -n 's/^\[ *\([0-9.]*\)\].*Register interface \[wlan0\].*/registered_at=\1s/p' | tail -n 1
        echo "--- bluetooth state"; (hciconfig -a 2>/dev/null || true) | head -n 8
        echo "--- bluetooth connections"; (hcitool con 2>/dev/null || true) | head -n 6
        echo "--- bluetooth nodes"; ls /sys/class/bluetooth 2>&1; hciconfig -a 2>&1 | head -n 8
        echo "--- dmesg (net/bt)"; dmesg 2>/dev/null | grep -iE 'brcmfmac|wlan|bluetooth|hci0|firmware' | tail -n 12
        echo "--- services"
        for _u in connman supplicantmanager.service supplicantmanager.path wifi-daemon.service regd.service; do
            printf '%s active=%s enabled=%s\n' "$_u" "$(systemctl is-active "$_u" 2>/dev/null)" "$(systemctl is-enabled "$_u" 2>/dev/null)"
        done
        echo "--- connman technologies"; connmanctl technologies 2>&1 | head -n 12
        echo "--- connman state"; connmanctl state 2>&1 | head -n 4
        # Reverse-engineering ConnMan on this unit: what config it shipped with,
        # who owns the wpa_supplicant D-Bus name, and whether a scan ever surfaces
        # a service (the OEM association path needs a service + the agent flow).
        echo "--- connman main.conf + drop-ins"
        for _cf in /etc/connman/main.conf /etc/connman/*.conf /run/connman/*.conf; do
            [ -f "$_cf" ] || continue
            echo "[$_cf]"; sed -n '1,80p' "$_cf" 2>/dev/null | grep -vE '^\s*#|^\s*$'
        done
        echo "--- connmand version"; connmand --version 2>&1 | head -n 2
        echo "--- wpa_supplicant D-Bus owner"; dbus-send --system --print-reply --reply-timeout=2000 --dest=org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus.GetNameOwner string:fi.w1.wpa_supplicant1 2>&1 | head -n 4
        echo "--- connman scan wifi + services (10 s)"; connmanctl scan wifi 2>&1 | head -n 3; sleep 10; connmanctl services 2>&1 | head -n 20
        echo "--- our daemon pid"; pidof wifi_daemon 2>&1
        systemctl show -p MainPID -p ExecMainStatus wifi-daemon.service 2>&1
        echo "--- status.json"; head -c 400 "$RW_DIR/status.json" 2>&1; echo
        echo "--- dhcp clients"; (command -v udhcpc dhcpcd dhclient 2>/dev/null || echo none); command -v busybox 2>/dev/null
        echo "--- wlan0 ip"; ip -4 addr show wlan0 2>&1 | grep -E "inet |state " | head -n 3
        echo "--- dhcp/static lines"
        grep -iE "dhcp:|static|iw link SSID" "$RW_DIR/daemon.log" 2>/dev/null | tail -n 20
        echo "--- daemon.log tail"; tail -n 60 "$RW_DIR/daemon.log" 2>&1
        echo "--- journal"; journalctl -u wifi-daemon.service -n 15 --no-pager 2>&1 | tail -n 15
        echo "--- smack of binary"; getfattr -n security.SMACK64 --only-values "$RW_DIR/wifi_daemon" 2>&1
        echo "--- listening ports"; (ss -ltn 2>/dev/null || netstat -ltn 2>/dev/null) | grep -E ':(809[0-9]|2222)' 2>&1
        echo
    } >> "$_p" 2>&1
    sync 2>/dev/null || true
}

log() {
    echo "$(date -u 2>/dev/null) $*" >> "$LOG" 2>/dev/null
}

result() {
    {
        echo "WIFI_PAYLOAD"
        echo "time=$(date -u 2>/dev/null)"
        echo "usb=$USB_DIR"
        echo "qml_dir=$QML_DIR"
        echo "uid=$(id -u 2>/dev/null)"
        echo "$@"
    } > "$OUT" 2>&1
    cp -f "$OUT" "$RW_DIR/run_stuff_result.txt" 2>/dev/null || true
    cp -f "$OUT" "$RW_DIR/result.txt" 2>/dev/null
}

fail() {
    log "FAIL: $*"
    result "status=FAIL" "error=$*"
    exit 1
}

# Always put / back read-only, even when a later step fails halfway through
# the install; otherwise a failed run leaves the rootfs writable.
cleanup_ro() {
    if [ "${transaction_started:-0}" = 1 ] && [ "${install_complete:-0}" != 1 ]; then
        log "rolling back incomplete install"
        systemctl stop wifi-daemon.service >> "$LOG" 2>&1 || true
        if [ "${mdnsd_changed:-0}" = 1 ]; then
            systemctl stop carplay_mdnsd.service >> "$LOG" 2>&1 || true
        fi
        while IFS='|' read -r item existed destination; do
            [ -n "$destination" ] || continue
            if [ "$existed" = 1 ]; then
                io_restore_entry "$BACKUP_HU/transaction/$item" "$destination" || log "ROLLBACK FAILED: $destination (backup retained)"
                for attr in SMACK64 SMACK64EXEC; do
                    value=$(cat "$BACKUP_HU/transaction/$item.$attr" 2>/dev/null || true)
                    [ -z "$value" ] || setfattr -n "security.$attr" -v "$value" "$destination" 2>/dev/null || true
                done
            else
                io_remove "$destination" || log "ROLLBACK FAILED: remove $destination"
            fi
        done < "$BACKUP_HU/transaction/files"
        systemctl daemon-reload >> "$LOG" 2>&1 || true
        if [ "${mdnsd_changed:-0}" = 1 ] && [ "${mdnsd_was_active:-0}" = 1 ]; then
            systemctl restart carplay_mdnsd.service >> "$LOG" 2>&1 || true
        fi
        for unit in supplicantmanager.path supplicantmanager.service; do
            case "$unit" in *.path) prior=${old_oem_path:-unknown};; *) prior=${old_oem_service:-unknown};; esac
            case "$prior" in masked|masked-runtime) systemctl mask "$unit" >> "$LOG" 2>&1 || true;;
                unknown) :;; *) systemctl unmask "$unit" >> "$LOG" 2>&1 || true;; esac
        done
        [ "${daemon_was_enabled:-0}" = 1 ] || systemctl disable wifi-daemon.service >> "$LOG" 2>&1 || true
        [ "${daemon_was_active:-0}" != 1 ] || systemctl start wifi-daemon.service >> "$LOG" 2>&1 || true
    fi
    if [ "${IO_ACTIVE:-0}" = 1 ]; then io_restore_root || true; fi
}
trap cleanup_ro EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP

remember() {
    destination=$1
    transaction_index=$((transaction_index + 1))
    existed=0
    if [ -e "$destination" ] || [ -L "$destination" ]; then
        if ! cp -pP "$destination" "$BACKUP_HU/transaction/$transaction_index" 2>>"$LOG"; then
            if [ "${2:-required}" = optional ]; then
                log "WARNING: optional icon backup failed; keeping existing icon: $destination"
                return 1
            fi
            fail "transaction backup failed: $destination"
        fi
        existed=1
        for attr in SMACK64 SMACK64EXEC; do
            getfattr -n "security.$attr" --only-values "$destination" > "$BACKUP_HU/transaction/$transaction_index.$attr" 2>/dev/null || true
        done
    fi
    echo "$transaction_index|$existed|$destination" >> "$BACKUP_HU/transaction/files"
}

mount -o remount,rw "$USB_DIR" 2>/dev/null || true
log "=== wifi payload start usb=$USB_DIR ==="

# The firmware ships md5sum only as a busybox applet; the coreutils binary
# is /usr/bin/md5sum.coreutils.  Resolve once, before any use.
MD5SUM_BIN=md5sum
command -v md5sum >/dev/null 2>&1 || MD5SUM_BIN=md5sum.coreutils

[ -d "$PAYLOAD" ] || fail "payload directory missing: $PAYLOAD"
[ -d "$BACKUP" ] || fail "backup directory missing: $BACKUP"
[ -f "$USB_DIR/manifest.md5" ] || fail "manifest.md5 missing"
[ -d "$QML_DIR" ] || fail "expected QML directory missing: $QML_DIR"

if ! ( cd "$USB_DIR" && "$MD5SUM_BIN" -c manifest.md5 ) > "$RW_DIR/manifest-check.log" 2>&1; then
    log "manifest check failed"
    fail "manifest check failed (see /rw_data/wifi/manifest-check.log)"
fi
log "manifest OK"

# ---- pick QML variants -------------------------------------------------
cur_adv="$QML_DIR/$ADV"
cur_wide="$QML_DIR/$WIDE"
cur_all="$QML_DIR/$ALL"
cur_setup="$QML_DIR/$SETUP"
cur_custom="$QML_DIR/$CUSTOM"
cur_theme="$QML_DIR/$THEME"
[ -f "$cur_adv" ] || fail "target missing: $cur_adv"
[ -f "$cur_wide" ] || fail "target missing: $cur_wide"
[ -f "$cur_all" ] || fail "target missing: $cur_all"
[ -f "$cur_setup" ] || fail "target missing: $cur_setup"
[ -f "$cur_custom" ] || fail "target missing: $cur_custom"

md5() { "$MD5SUM_BIN" "$1" 2>/dev/null | cut -d' ' -f1; }

ref_md5() {
    cat "$PAYLOAD/ref/$1.md5" 2>/dev/null | cut -d' ' -f1
}

adv_variant=wifi
has_hello=0
has_battery=0
has_battery_tile=0
has_wifi=0
grep -q 'HELLO_PAYLOAD_260128' "$cur_adv" 2>/dev/null && has_hello=1
grep -q 'BATTERY_PAYLOAD_260128' "$cur_adv" 2>/dev/null && has_battery=1
grep -q 'id: batteryTile1' "$cur_adv" 2>/dev/null && has_battery_tile=1
grep -q 'WIFI_PAYLOAD_260128' "$cur_adv" 2>/dev/null && has_wifi=1

if [ "$has_battery" = 1 ]; then
    if [ "$has_battery_tile" = 1 ]; then
        if [ "$has_hello" = 1 ]; then
            adv_variant=hello_battery_tile_wifi
        else
            adv_variant=battery_tile_wifi
        fi
    else
        if [ "$has_hello" = 1 ]; then
            adv_variant=hello_battery_wifi
        else
            adv_variant=battery_wifi
        fi
    fi
elif [ "$has_hello" = 1 ]; then
    adv_variant=hello_wifi
elif [ "$has_wifi" = 1 ]; then
    adv_variant=wifi
else
    cur_md5=$(md5 "$cur_adv")
    if [ "$cur_md5" = "$(ref_md5 stock_advanced)" ]; then
        adv_variant=wifi
    else
        fail "unknown Advanced QML (md5=$cur_md5)"
    fi
fi

wide_variant=wifi
if grep -q 'WIFI_PAYLOAD_260128' "$cur_wide" 2>/dev/null; then
    if grep -q 'HELLO_PAYLOAD_260128' "$cur_wide" 2>/dev/null; then
        wide_variant=hello_wifi
    fi
elif grep -q 'HELLO_PAYLOAD_260128' "$cur_wide" 2>/dev/null; then
    wide_variant=hello_wifi
else
    cur_md5=$(md5 "$cur_wide")
    if [ "$cur_md5" = "$(ref_md5 stock_wide)" ]; then
        wide_variant=wifi
    else
        fail "unknown Wide QML (md5=$cur_md5)"
    fi
fi

all_variant=wifi
if grep -q 'WIFI_PAYLOAD_260128' "$cur_all" 2>/dev/null; then
    if grep -q 'HELLO_PAYLOAD_260128' "$cur_all" 2>/dev/null; then
        all_variant=hello_wifi
    fi
elif grep -q 'HELLO_PAYLOAD_260128' "$cur_all" 2>/dev/null; then
    all_variant=hello_wifi
else
    cur_md5=$(md5 "$cur_all")
    if [ "$cur_md5" = "$(ref_md5 stock_allmenus)" ]; then
        all_variant=wifi
    else
        fail "unknown AllMenus QML (md5=$cur_md5)"
    fi
fi

setup_variant=wifi
if grep -q 'WIFI_PAYLOAD_260128' "$cur_setup" 2>/dev/null; then
    if grep -q 'HELLO_PAYLOAD_260128' "$cur_setup" 2>/dev/null; then
        setup_variant=hello_wifi
    fi
elif grep -q 'HELLO_PAYLOAD_260128' "$cur_setup" 2>/dev/null; then
    setup_variant=hello_wifi
else
    cur_md5=$(md5 "$cur_setup")
    if [ "$cur_md5" = "$(ref_md5 stock_setup)" ]; then
        setup_variant=wifi
    else
        fail "unknown Setup QML (md5=$cur_md5)"
    fi
fi

custom_variant=wifi
if grep -q 'WIFI_PAYLOAD_260128' "$cur_custom" 2>/dev/null; then
    if grep -q 'HELLO_PAYLOAD_260128' "$cur_custom" 2>/dev/null; then
        custom_variant=hello_wifi
    fi
elif grep -q 'HELLO_PAYLOAD_260128' "$cur_custom" 2>/dev/null; then
    custom_variant=hello_wifi
else
    cur_md5=$(md5 "$cur_custom")
    if [ "$cur_md5" = "$(ref_md5 stock_custom)" ]; then
        custom_variant=wifi
    else
        fail "unknown QCustomizedMenuListAllowEmpty QML (md5=$cur_md5)"
    fi
fi

log "variants: adv=$adv_variant wide=$wide_variant all=$all_variant setup=$setup_variant custom=$custom_variant"

# The page and its components are this package's own artifacts and must be
# there. The home-screen VARIANTS are per installed-package combination, and a
# build only carries the ones somebody had a base for: a missing variant must not
# fail the stage (that would leave the daemon uninstalled), it ships the page and
# reports the tile as skipped, exactly like the store installer does.
for selected in HomeScreen/WifiPage.qml component/WifiTile.qml component/WifiSwitch.qml component/WifiButton.qml; do
    [ -f "$PAYLOAD/qml/AppHomeScreen/qml_eco3.0/$selected" ] || fail "selected QML missing: $selected"
done
variant_missing=""
for selected in \
    "HomeScreen/QHMI_HomeScreenMain_Advanced.$adv_variant.qml" \
    "HomeScreen/QHMI_HomeScreenMain_Wide.$wide_variant.qml" \
    "HomeScreen/Screen_HomeScreen_AllMenus.$all_variant.qml" \
    "HomeScreen/Screen_HomeScreen_Submenu_Setup.$setup_variant.qml" \
    "component/QCustomizedMenuListAllowEmpty.$custom_variant.qml"; do
    [ -f "$PAYLOAD/qml/AppHomeScreen/qml_eco3.0/$selected" ] || variant_missing="$variant_missing $selected"
done
[ -z "$variant_missing" ] || log "WARNING: no home-screen variant for this unit, tile skipped:$variant_missing"
[ -s "$PAYLOAD/build-id.txt" ] || fail "build-id.txt missing"
BUILD_ID=$(cat "$PAYLOAD/build-id.txt")

if [ "${WIFI_DRY_RUN:-0}" = "1" ]; then
    result "status=DRY_RUN" "variant=$adv_variant/$wide_variant/$all_variant/$setup_variant/$custom_variant"
    exit 0
fi

# ---- backup (retention, then snapshot) ----------------------------------
# The loader re-runs this stick on every power-on (/run is tmpfs, so its md5
# gate resets), and each run writes a full transaction snapshot (~2-3 MB).
# Unbounded, that history filled /rw_data to 97% (58 snapshots = 101 MB), so
# keep the restore point plus the two newest snapshots and prune the rest
# before allocating a new one.
prune_backups() {
    _b="$1"
    [ -d "$_b" ] || return 0
    # readlink -f prints the target of a dangling symlink, so require that it
    # resolves to an existing directory before treating it as the restore point.
    _lat=$(readlink -f "$_b/latest" 2>/dev/null)
    [ -n "$_lat" ] && [ -d "$_lat" ] || _lat=""
    _n=0
    _freed=0
    for _d in $(ls -d "$_b"/20* 2>/dev/null | sort -r); do
        [ -d "$_d" ] || continue
        case "$_d" in
            "$_b"/[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]T[0-9][0-9][0-9][0-9][0-9][0-9]Z*) ;;
            *) continue ;;
        esac
        _n=$((_n + 1))
        if [ "$_n" -le 2 ]; then continue; fi
        # _d keeps the caller's $RW_DIR spelling while _lat is canonical, so a
        # trailing slash or a symlinked parent must not hide the restore point.
        if [ -n "$_lat" ] && [ "$(readlink -f "$_d" 2>/dev/null)" = "$_lat" ]; then
            continue
        fi
        _kb=$(du -sk "$_d" 2>/dev/null | cut -f1)
        rm -rf "$_d" || fail "cannot prune $_d"
        _freed=$((_freed + ${_kb:-0}))
    done
    if [ -n "$_lat" ] && [ ! -d "$_lat" ]; then
        fail "restore point $_lat disappeared while pruning"
    fi
    [ "$_freed" -gt 0 ] && log "prune: freed ${_freed} KB (kept restore point + 2 snapshots)"
    return 0
}

STAMP=$(date -u +%Y%m%dT%H%M%SZ 2>/dev/null)
[ -n "$STAMP" ] || STAMP=manual
prune_backups "$RW_DIR/backup"
BACKUP_HU="$RW_DIR/backup/$STAMP"
suffix=0
while ! mkdir -m 700 "$BACKUP_HU" 2>/dev/null; do
    [ -e "$BACKUP_HU" ] || [ -L "$BACKUP_HU" ] || fail "cannot create backup directory"
    suffix=$((suffix + 1))
    [ "$suffix" -le 100 ] || fail "too many backup timestamp collisions"
    BACKUP_HU="$RW_DIR/backup/$STAMP-$suffix"
done
mkdir -p "$BACKUP_HU/HomeScreen" "$BACKUP_HU/component/common" 2>/dev/null || fail "cannot create $BACKUP_HU"

for f in "$ADV" "$WIDE" "$ALL" "$SETUP" "$CUSTOM" "$THEME"; do
    if [ -f "$QML_DIR/$f" ]; then
        mkdir -p "$BACKUP_HU/$(dirname "$f")" 2>/dev/null
        cp -f "$QML_DIR/$f" "$BACKUP_HU/$f" || fail "backup failed: $f"
        mkdir -p "$USB_DIR/backup_current/AppHomeScreen/qml_eco3.0/$(dirname "$f")" 2>/dev/null
        cp -f "$QML_DIR/$f" "$USB_DIR/backup_current/AppHomeScreen/qml_eco3.0/$f" 2>/dev/null || true
    fi
done
# Keep the first pre-install tree as the restore point.
new_restore_point=0
grep -q 'WIFI_PAYLOAD_260128' "$cur_adv" 2>/dev/null || new_restore_point=1
log "HU backup: $BACKUP_HU"

# ---- rootfs rw ---------------------------------------------------------
. "$PAYLOAD/lib/install_io.sh"
io_capture_qml "$QML_DIR" "$USB_DIR/qml_before" || log "WARNING: QML before-snapshot unavailable"
io_init && io_settle_root || fail "root filesystem is not stably writable"

mkdir -p "$BACKUP_HU/transaction" "$RW_DIR/backup/accounts" || fail "cannot create transaction backup"
transaction_index=0
: > "$BACKUP_HU/transaction/files"
for destination in "$RW_DIR/wifi_daemon" "$RW_DIR/bin/dropbearmulti" "$SERVICE" "$RW_DIR/status.json" \
    "$RW_DIR/config.conf" "$RW_DIR/wifi.txt" "$RW_DIR/ssh/authorized_keys" \
    "$RW_DIR/home/.ssh/authorized_keys" /etc/passwd /etc/passwd.legacy /etc/passwd.secure /etc/shadow /etc/shadow.Global \
    /etc/systemd/system/carplay_mdnsd.service.d/50-scourge-restart.conf; do
    remember "$destination"
    if [ -L "$destination" ]; then
        target=$(readlink -f "$destination" 2>/dev/null || true)
        [ -z "$target" ] || [ ! -f "$target" ] || remember "$target"
    fi
done
for f in passwd passwd.legacy passwd.secure shadow shadow.Global; do remember "$RW_DIR/backup/accounts/$f"; done
for f in "$ADV" "$WIDE" "$ALL" "$SETUP" "$CUSTOM" "$THEME" \
    HomeScreen/WifiPage.qml component/WifiTile.qml component/WifiSwitch.qml component/WifiButton.qml \
    component/HelloTile.qml component/BatteryTile.qml; do remember "$QML_DIR/$f"; done
daemon_was_active=0
systemctl is-active --quiet wifi-daemon.service 2>/dev/null && daemon_was_active=1
daemon_was_enabled=0
systemctl is-enabled --quiet wifi-daemon.service 2>/dev/null && daemon_was_enabled=1
if [ "$daemon_was_active" = 1 ]; then
    systemctl stop wifi-daemon.service >> "$LOG" 2>&1 || fail "cannot stop existing daemon safely"
fi
transaction_started=1

# ---- SSH user ----------------------------------------------------------
# /etc/passwd is a symlink to /tmp/passwd on this firmware.  esinit
# rewrites /tmp/passwd from /etc/passwd.legacy or /etc/passwd.secure on
# every boot, so the user must live in those templates too.
add_car_passwd() {
    f="$1"
    [ -f "$f" ] || return 0
    grep -q '^car:' "$f" 2>/dev/null && return 0
    entry='car:x:0:0:Car User:/rw_data/wifi/home:/bin/sh'
    io_append_line "$f" "$entry" || fail "cannot add account: $f"
    printf '%s\n' "$entry" > "$RW_DIR/backup/accounts/$(basename "$f")" || fail "cannot record account ownership"
    chmod 600 "$RW_DIR/backup/accounts/$(basename "$f")" 2>/dev/null || true
    log "added user car to $f"
}
add_car_shadow() {
    f="$1"
    [ -f "$f" ] || return 0
    grep -q '^car:' "$f" 2>/dev/null && return 0
    entry="car:$CAR_HASH:19000:0:99999:7:::"
    io_append_line "$f" "$entry" || fail "cannot add shadow: $f"
    printf '%s\n' "$entry" > "$RW_DIR/backup/accounts/$(basename "$f")" || fail "cannot record shadow ownership"
    chmod 600 "$RW_DIR/backup/accounts/$(basename "$f")" 2>/dev/null || true
    log "added car shadow to $f"
}

if [ ! -e /etc/passwd ] && [ -f /etc/passwd.legacy ]; then
    cp -f /etc/passwd.legacy /etc/passwd 2>/dev/null || true
    chmod 644 /etc/passwd 2>/dev/null || true
fi
if [ ! -e /etc/shadow ] && [ -f /etc/shadow.Global ]; then
    cp -f /etc/shadow.Global /etc/shadow 2>/dev/null || true
fi

add_car_passwd /etc/passwd
add_car_passwd /etc/passwd.legacy
add_car_passwd /etc/passwd.secure
add_car_shadow /etc/shadow.Global
add_car_shadow /etc/shadow

mkdir -p "$RW_DIR/home/.ssh" 2>/dev/null
chmod 700 "$RW_DIR/home" "$RW_DIR/home/.ssh" 2>/dev/null || true
chown 0:0 "$RW_DIR/home" "$RW_DIR/home/.ssh" 2>/dev/null || true
# /etc/shells is /tmp/shells; it is recreated at boot.  Add /bin/sh for
# this boot only; dropbear does not require it, but some login paths do.
if [ -f /tmp/shells ]; then
    grep -qx '/bin/sh' /tmp/shells 2>/dev/null || echo '/bin/sh' >> /tmp/shells
else
    printf '/bin/sh\n' > /tmp/shells 2>/dev/null || true
    chmod 644 /tmp/shells 2>/dev/null || true
fi

# ---- assets / icons ----------------------------------------------------
# WiFi assets plus the refreshed Hello/Battery tile icons.  The latter are
# installed only when their app is present; the directories are harmless.
for app in wifi hello battery; do
    src="$PAYLOAD/assets/$app"
    [ -d "$src" ] || continue
    dst="$QML_ROOT/$app"
    for f in "$src/"*; do
        [ -f "$f" ] || continue
        if remember "$dst/$(basename "$f")" optional; then
            io_asset "$f" "$dst/$(basename "$f")"
        else
            ASSET_WARNINGS=$((ASSET_WARNINGS + 1))
        fi
    done
    chmod 644 "$dst/"* 2>/dev/null || true
done

# ---- daemon / dropbear / service --------------------------------------
io_copy "$PAYLOAD/bin/wifi_daemon" "$RW_DIR/wifi_daemon" || fail "daemon copy failed"
chmod 755 "$RW_DIR/wifi_daemon" 2>/dev/null || true
chown 0:0 "$RW_DIR/wifi_daemon" 2>/dev/null || true
# Verify the requested label; do not claim it establishes effective runtime
# access. Vendor executables can carry their own exec labels. A failed label
# gate aborts and rolls back instead of silently reporting a successful install.
set_exec_label() {
    binary=$1
    if grep -q ' smackfs ' /proc/mounts 2>/dev/null; then
        command -v chsmack >/dev/null 2>&1 || fail "chsmack missing"
        chsmack -e _ "$binary" >> "$LOG" 2>&1 || fail "exec label failed: $binary"
        actual=$(getfattr -n security.SMACK64EXEC --only-values "$binary" 2>/dev/null || true)
        if [ -z "$actual" ]; then
            log "WARNING: cannot read back SMACK64EXEC for $binary; trusting chsmack"
        elif [ "$actual" != _ ]; then
            fail "exec label readback failed: $binary"
        fi
    fi
}
set_exec_label "$RW_DIR/wifi_daemon"

if [ -f "$PAYLOAD/bin/dropbearmulti" ]; then
    cp -f "$PAYLOAD/bin/dropbearmulti" "$RW_DIR/bin/dropbearmulti" || fail "dropbear copy failed"
    chmod 755 "$RW_DIR/bin/dropbearmulti" 2>/dev/null || true
    chown 0:0 "$RW_DIR/bin/dropbearmulti" 2>/dev/null || true
    set_exec_label "$RW_DIR/bin/dropbearmulti"
    ln -sf dropbearmulti "$RW_DIR/bin/dropbear" 2>/dev/null || true
    ln -sf dropbearmulti "$RW_DIR/bin/dropbearkey" 2>/dev/null || true
fi

io_copy "$PAYLOAD/systemd/wifi-daemon.service" "$SERVICE" || fail "service copy failed"
chmod 644 "$SERVICE" 2>/dev/null || true
chown 0:0 "$SERVICE" 2>/dev/null || true

# ---- carplay_mdnsd restart drop-in -------------------------------------
# /app/bin/mdnsd is a *launcher*: it forks the real responder and exits 0
# immediately (2026-11-25, checked over ssh: the launcher returns 0 at once while
# UDP 5353 is served by its detached child).  The stock unit therefore runs with
# RemainAfterExit=true, which is the only correct semantic for a launcher.
#
# The first version of this drop-in set RemainAfterExit=no + Restart=always, so
# systemd saw a clean exit and restarted the launcher every 2 s (start-limit
# hold-off made it look like 10 s) - and every restart forked ANOTHER responder:
# the unit had four 5353 sockets and a permanent churn.  Restart=on-failure keeps
# the recovery the drop-in was added for (a crash still restarts) without
# relaunching a launcher that is supposed to exit.
MDNSD_UNIT_DIR="/etc/systemd/system/carplay_mdnsd.service.d"
if [ -d /etc/systemd/system ] && command -v systemctl >/dev/null 2>&1; then
    if io_mkdir "$MDNSD_UNIT_DIR"; then
        cat > "$RW_DIR/mdnsd-dropin.conf" <<'EOF'
# SCOURGE_MDNSD_RESTART=1
[Service]
Restart=on-failure
RestartSec=2
EOF
        # Always (re)write: an older drop-in with Restart=always is the churn above.
        io_copy "$RW_DIR/mdnsd-dropin.conf" "$MDNSD_UNIT_DIR/50-scourge-restart.conf" || fail "mdnsd drop-in copy failed"
        chmod 644 "$MDNSD_UNIT_DIR/50-scourge-restart.conf" 2>/dev/null || true
        log "installed carplay_mdnsd restart drop-in (on-failure: the launcher exits 0 by design)"
    else
        log "WARNING: cannot create $MDNSD_UNIT_DIR (mdnsd will not auto-restart)"
    fi
fi

# ---- regulatory domain: the 5 GHz AP depends on it ----------------------
# cfg80211 has no regulatory.db on this unit (/lib/firmware/regulatory.* absent)
# and the driver's own country never reaches cfg80211: after
# "wl -i wlan0 country GB" the driver reports GB while "iw reg get" still says
# country 00, so every 5 GHz channel stays PASSIVE-SCAN at 0.0 dBm.  Measured on
# the car 2026-11-25: the static AP on ch36 applied its address, started the
# supplicant and then never beaconed ("regdom after set = '00'", AP interface up
# with an empty SSID).  Without a signed regdb the only knob is the cfg80211
# module parameter, which is read at load time - so this takes effect on the next
# boot.  AA's wireless bootstrap needs the 5 GHz AP; SSH does not (2.4 GHz is
# allowed as active here).
REG_DROPIN="/etc/modprobe.d/90-scourge-regdom.conf"
# HU is the owner's country (ETSI, same 5 GHz ranges as the vendor's ccode=GB).
REG_DOMAIN="${SCOURGE_REGDOM:-HU}"
# ---- signed regulatory database -----------------------------------------
# Kernel 4.1.27 on this unit: cfg80211 only learned to read the signed
# regulatory.db in 4.15, so the pair below is INERT here - 4.1 asks CRDA (a
# userspace daemon this image does not have) and the module parameter alone never
# moves it off "country 00" (world), which forbids 5 GHz beacons.  Measured
# 2026-11-25: ieee80211_regdom read as HU, "iw reg get" still country 00, while
# "wl -i wlan0 country HU" did stick in the vendor firmware (DE -> HU/HUNGARY).
# The pair is shipped anyway: it costs nothing and any newer kernel uses it.
# The stage logs all three answers so the bundle never has to guess.
FW_DIR="/lib/firmware"
if [ -f "$PAYLOAD/firmware/regulatory.db" ] && [ -f "$PAYLOAD/firmware/regulatory.db.p7s" ]; then
    for _f in regulatory.db regulatory.db.p7s; do
        if [ -f "$FW_DIR/$_f" ] && [ ! -f "$FW_DIR/$_f.scourge-bak" ]; then
            if io_copy "$FW_DIR/$_f" "$RW_DIR/$_f.stock"; then
                io_copy "$RW_DIR/$_f.stock" "$FW_DIR/$_f.scourge-bak" || true
            fi
        fi
        io_copy "$PAYLOAD/firmware/$_f" "$FW_DIR/$_f" || log "WARNING: cannot install $FW_DIR/$_f"
    done
    # Magic is the cheap proof that a database landed, not an error page.
    if [ "$(head -c 4 "$FW_DIR/regulatory.db" 2>/dev/null)" = "RGDB" ]; then
        log "regulatory.db installed (signed pair; cfg80211 only reads it from 4.15 on)"
    else
        log "WARNING: $FW_DIR/regulatory.db has no RGDB magic - 5 GHz stays blocked"
    fi
    io_copy "$PAYLOAD/firmware/regdb-version.txt" "$RW_DIR/regdb-version.txt" || true
else
    log "WARNING: payload carries no regulatory.db - cfg80211 stays on country 00 (no 5 GHz AP)"
fi
if [ -d /etc/modprobe.d ] && command -v modprobe >/dev/null 2>&1; then
    if io_mkdir /etc/modprobe.d 2>/dev/null; then
        printf '# SCOURGE_REGDOM=1\noptions cfg80211 ieee80211_regdom=%s\n' "$REG_DOMAIN" \
            > "$RW_DIR/regdom.conf"
        if io_copy "$RW_DIR/regdom.conf" "$REG_DROPIN"; then
            chmod 644 "$REG_DROPIN" 2>/dev/null || true
            log "regulatory domain drop-in installed: $REG_DOMAIN (applies on the next boot)"
        else
            log "WARNING: cannot install $REG_DROPIN - the 5 GHz AP stays blocked"
        fi
    else
        log "WARNING: cannot create /etc/modprobe.d - the 5 GHz AP stays blocked"
    fi
fi
# Apply the hint now (cfg80211 also reads the module parameter at load time) and
# report what the radio really says: 00 means passive scan, i.e. no 5 GHz AP.
iw reg set "$REG_DOMAIN" 2>/dev/null && log "iw reg set $REG_DOMAIN applied" \
    || log "WARNING: iw reg set $REG_DOMAIN failed"
_regdom_now=$(iw reg get 2>/dev/null | sed -n 's/^country \([^:]*\).*/\1/p' | head -n 1)
[ -n "$_regdom_now" ] || _regdom_now=unknown
log "regdomain now: $_regdom_now (00 = world = 5 GHz passive scan)"

# ---- the CRDA stand-in: a regdomain that actually sticks -----------------
# The lines above are the measurement; on 4.1 nothing moves cfg80211 off
# country 00 unless a userspace daemon answers the CRDA hint, and this image has
# none.  regd does that from the signed database installed above and then reads
# the regdomain back, so this stage records a verdict instead of a hope.
REGD_BIN="$RW_DIR/regd"
REGD_UNIT="/etc/systemd/system/regd.service"
regd_status="not-installed"
if [ -f "$PAYLOAD/bin/regd" ]; then
    io_copy "$PAYLOAD/bin/regd" "$REGD_BIN" || fail "regd copy failed"
    chmod 755 "$REGD_BIN" 2>/dev/null || true
    chown 0:0 "$REGD_BIN" 2>/dev/null || true
    set_exec_label "$REGD_BIN"
    # regd searches this copy first: the tool then works even in a mount state
    # where /lib/firmware cannot be read.
    if [ -f "$FW_DIR/regulatory.db" ]; then
        cp -f "$FW_DIR/regulatory.db" "$RW_DIR/regulatory.db" 2>/dev/null || true
    fi
    regd_rc=0
    regd_out=$("$REGD_BIN" --once --country "$REG_DOMAIN" --timeout 4000 \
        --status-file "$RW_DIR/regd.json" 2>&1) || regd_rc=$?
    printf '%s\n' "$regd_out" >> "$LOG"
    log "regd --once rc=$regd_rc: $(printf '%s' "$regd_out" | tr '\n' ' ')"
    case "$regd_rc" in
        0) regd_status="applied" ;;
        2) regd_status="no-database" ;;
        3) regd_status="country-unknown" ;;
        5) regd_status="set-reg-rejected" ;;
        6) regd_status="driver-managed" ;;
        *) regd_status="rc=$regd_rc" ;;
    esac
    if [ -f "$PAYLOAD/systemd/regd.service" ] && [ -d /etc/systemd/system ] &&
       command -v systemctl >/dev/null 2>&1; then
        if io_copy "$PAYLOAD/systemd/regd.service" "$REGD_UNIT"; then
            chmod 644 "$REGD_UNIT" 2>/dev/null || true
            if systemctl enable regd.service >> "$LOG" 2>&1; then
                log "regd.service enabled (re-applies the regdomain on every boot)"
            else
                log "WARNING: cannot enable regd.service (the regdomain resets on reboot)"
            fi
        else
            log "WARNING: cannot install $REGD_UNIT (the regdomain resets on reboot)"
        fi
    fi
else
    log "WARNING: payload carries no regd; cfg80211 stays on country 00 (no 5 GHz AP)"
fi
log "regdomain after regd: $(iw reg get 2>/dev/null | sed -n 's/^country \([^:]*\).*/\1/p' | head -n 1) ($regd_status)"

# ---- client-mode network tools: our own DHCP client and the mDNS advert ----
# This image has no DHCP client at all (measured 2026-11-26: no udhcpc/dhcpcd/
# dhclient, and the busybox has no udhcpc applet), so client mode was stuck on the
# static address.  dhcp_client is the first entry of dhcp_acquire()'s chain; the
# daemon starts mdns_adv itself once the SSH port is really bound.
NET_TOOLS_INSTALLED=""
for _t in dhcp_client mdns_adv; do
    if [ -f "$PAYLOAD/bin/$_t" ]; then
        io_copy "$PAYLOAD/bin/$_t" "$RW_DIR/bin/$_t" || fail "$_t copy failed"
        chmod 755 "$RW_DIR/bin/$_t" 2>/dev/null || true
        chown 0:0 "$RW_DIR/bin/$_t" 2>/dev/null || true
        set_exec_label "$RW_DIR/bin/$_t"
        NET_TOOLS_INSTALLED="$NET_TOOLS_INSTALLED $_t"
    else
        log "WARNING: payload carries no $_t"
    fi
done
[ -n "$NET_TOOLS_INSTALLED" ] && log "network tools installed:$NET_TOOLS_INSTALLED"
# A stale lease or advertiser from the previous payload must not survive.
rm -f "$RW_DIR/dhcp.lease" "$RW_DIR/mdns_adv.pid" 2>/dev/null || true

# ---- optional mdnsd "continue as root" patch ----------------------------
# Experimental, disarmed by default: multicast EPERM was observed but its
# cause is not established. NOPing setuid retains root; it does not take
# the getpwnam-failure logging branch. The patch is applied by copying a
# pre-patched binary from the payload (md5-verified), NOT by byte-patching
# on the car: cp -f over the existing file keeps its inode and therefore
# the vendor SMACK64EXEC label (App::projection).
#
# Opt-in: create the marker file MDNSD_ROOT_ARM at the stick root.
# Guarded: the on-car binary must match the stock md5 (or already be the
# patched md5) and the payload copy must match the patched md5; anything
# else aborts without touching the vendor binary.  The stock binary is
# backed up and run_stuff_restore.sh restores it.
MDNSD_BIN=/app/bin/mdnsd
MDNSD_STOCK_MD5=f63a65c4c2b0c011ae8ed17557b45c1b
MDNSD_PATCHED_MD5=f847373d7c5a6906453ba921bb790d22
mdnsd_patch=not-attempted
md5_of() { "$MD5SUM_BIN" "$1" 2>/dev/null | cut -d' ' -f1; }
if [ -f "$USB_DIR/MDNSD_ROOT_ARM" ]; then
    if [ ! -f "$MDNSD_BIN" ]; then
        mdnsd_patch=missing-binary
        log "mdnsd patch: $MDNSD_BIN not found"
    elif [ ! -f "$PAYLOAD/bin/mdnsd.rooted" ]; then
        mdnsd_patch=payload-missing
        log "mdnsd patch: $PAYLOAD/bin/mdnsd.rooted missing"
    else
        _cur=$(md5_of "$MDNSD_BIN")
        if [ "$_cur" = "$MDNSD_PATCHED_MD5" ]; then
            mdnsd_patch=already-patched
            log "mdnsd patch: already applied"
        elif [ "$_cur" != "$MDNSD_STOCK_MD5" ]; then
            mdnsd_patch=md5-mismatch
            log "mdnsd patch: unexpected md5 for $MDNSD_BIN ($_cur); refusing"
        elif [ "$(md5_of "$PAYLOAD/bin/mdnsd.rooted")" != "$MDNSD_PATCHED_MD5" ]; then
            mdnsd_patch=payload-corrupt
            log "mdnsd patch: payload mdnsd.rooted md5 mismatch; refusing"
        else
            _bak="$RW_DIR/backup/mdnsd.stock"
            remember "$MDNSD_BIN"
            if { [ -f "$_bak" ] && [ "$(md5_of "$_bak")" = "$MDNSD_STOCK_MD5" ]; } ||
               { [ ! -e "$_bak" ] && cp -p "$MDNSD_BIN" "$_bak" && [ "$(md5_of "$_bak")" = "$MDNSD_STOCK_MD5" ]; }; then
                mdnsd_was_active=0
                systemctl is-active --quiet carplay_mdnsd.service && mdnsd_was_active=1
                mdnsd_changed=1
                systemctl stop carplay_mdnsd.service >> "$LOG" 2>&1 || fail "cannot stop mdnsd for experimental patch"
                if io_copy "$PAYLOAD/bin/mdnsd.rooted" "$MDNSD_BIN"; then
                    if [ "$(md5_of "$MDNSD_BIN")" = "$MDNSD_PATCHED_MD5" ]; then
                        mdnsd_patch=ok
                        log "mdnsd patch: applied (backup: $_bak)"
                        systemctl restart carplay_mdnsd.service >> "$LOG" 2>&1 || true
                    else
                        mdnsd_patch=verify-failed
                        io_copy "$_bak" "$MDNSD_BIN" || fail "cannot restore stock mdnsd"
                        log "mdnsd patch: verification failed; stock restored"
                    fi
                else
                    mdnsd_patch=copy-failed
                    log "mdnsd patch: copy over $MDNSD_BIN failed"
                fi
            else
                mdnsd_patch=backup-failed
                log "mdnsd patch: cannot back up $MDNSD_BIN"
            fi
        fi
    fi
fi
case "$mdnsd_patch" in not-attempted|ok|already-patched) : ;; *) fail "mdnsd experiment refused: $mdnsd_patch" ;; esac

# ---- default config / import ------------------------------------------
if [ ! -f "$RW_DIR/config.conf" ]; then
    RAND=$(dd if=/dev/urandom bs=16 count=1 2>/dev/null | "$MD5SUM_BIN" | cut -c1-16)
    cat > "$RW_DIR/config.conf" <<EOF
config_version=2
mode=off
wlan_on=0
ssid=
psk=
carplay_ssid=IONIQ5-CarPlay
carplay_psk=$RAND
carplay_band=5
carplay_channel=36
carplay_hidden=0
carplay_max_sta=2
carplay_manufacturer=Hyundai
carplay_model=Ioniq 5
carplay_name=Ioniq 5
ssh=0
ssh_port=2222
EOF
    chmod 600 "$RW_DIR/config.conf" 2>/dev/null || true
fi
# Force the SSH switch from what the stick asked for, whether or not the config
# already existed: set_key keeps the rest of the file intact.
if [ "${ssh_wanted:-0}" != 0 ] && [ -f "$RW_DIR/config.conf" ]; then
    if grep -q '^ssh=' "$RW_DIR/config.conf" 2>/dev/null; then
        sed -i "s/^ssh=.*/ssh=1/" "$RW_DIR/config.conf" 2>/dev/null || true
    else
        printf 'ssh=1\n' >> "$RW_DIR/config.conf" 2>/dev/null || true
    fi
    log "config ssh=1 (ssh_wanted=$ssh_wanted)"
fi

ssh_wanted=0
_ac_src=""
[ -f "$STICK_ROOT/authorized_keys" ] && _ac_src="$STICK_ROOT/authorized_keys"
[ -z "$_ac_src" ] && [ -f "$USB_DIR/authorized_keys" ] && _ac_src="$USB_DIR/authorized_keys"
if [ -n "$_ac_src" ]; then
    cp -f "$_ac_src" "$RW_DIR/ssh/authorized_keys" 2>/dev/null || true
    chmod 600 "$RW_DIR/ssh/authorized_keys" 2>/dev/null || true
    cp -f "$_ac_src" "$RW_DIR/home/.ssh/authorized_keys" 2>/dev/null || true
    chmod 600 "$RW_DIR/home/.ssh/authorized_keys" 2>/dev/null || true
    # Key-only login, so enabling it here exposes nothing that the stick did not
    # already carry - and it is the only way to debug the unit when the WiFi page
    # itself refuses to open (2026-11-22: the page is exactly what we needed to
    # reach the switch through). ssh=1 is written into config.conf below.
    ssh_wanted=1
    log "authorized_keys imported; SSH requested (key-only)"
fi
# The password account (car/electric) stays off unless asked for. ssh=1 with keys
# present means key-only; the daemon enables password auth separately.
if [ -f "$USB_DIR/ssh_enable" ]; then
    ssh_wanted=1
    [ ! -f "$USB_DIR/ssh_password_ok" ] || ssh_wanted=2
    log "ssh_enable on the stick: SSH requested (password auth: $([ "$ssh_wanted" = 2 ] && echo yes || echo no))"
fi
# Static address for the case where the unit associates but has no DHCP client
# at all (2026-11-22: "dhcpcd: command not found" on the head unit).  Put a file
# named static_ip on the stick, e.g. "192.0.2.60/24", optionally static_gw.
for _k in static_ip static_gw; do
[ -f "$STICK_ROOT/$_k" ] || continue
_v=$(tr -d " \t\r\n" < "$STICK_ROOT/$_k" 2>/dev/null | head -c 64)
[ -n "$_v" ] || continue
if grep -q "^$_k=" "$RW_DIR/config.conf" 2>/dev/null; then
    sed -i "s|^$_k=.*|$_k=$_v|" "$RW_DIR/config.conf" 2>/dev/null || true
else
    printf "%s=%s\n" "$_k" "$_v" >> "$RW_DIR/config.conf" 2>/dev/null || true
fi
log "config $_k=$_v"
    # The daemon reads these as plain files (it rewrites config.conf from its own
    # struct, which used to drop the keys), so keep both copies.
    printf "%s\n" "$_v" > "$RW_DIR/$_k" 2>/dev/null || true
done
# Import before the service starts.  Import is best-effort: the daemon may
# not be able to persist config in the installer context, but an existing
# /rw_data/wifi/config.conf is still usable and the UI can import again later.
# The user's network list lives at the STICK ROOT (that is where the docs and the
# UI tell them to put it); the stage dir copy is only a fallback.  Reading the
# wrong one meant edits to wifi.txt were ignored and stale networks survived.
_wifi_src=""
[ -f "$STICK_ROOT/wifi.txt" ] && _wifi_src="$STICK_ROOT/wifi.txt"
[ -z "$_wifi_src" ] && [ -f "$USB_DIR/wifi.txt" ] && _wifi_src="$USB_DIR/wifi.txt"
if [ -n "$_wifi_src" ]; then
    log "importing network list from $_wifi_src"
    cp -f "$_wifi_src" "$RW_DIR/wifi.txt" 2>/dev/null || true
    chmod 600 "$RW_DIR/wifi.txt" 2>/dev/null || true
    if [ -x "$RW_DIR/wifi_daemon" ]; then
        if "$RW_DIR/wifi_daemon" --import-file "$RW_DIR/wifi.txt" >> "$LOG" 2>&1; then
            wifi_import=OK
        else
            # The import runs before the radio exists (~110 s after boot), and the
            # daemon refuses to persist a client profile when it cannot see wlan0.
            # The file is in /rw_data/wifi/wifi.txt either way, and the master
            # retries the import at the end of the run once the daemon is up
            # (wifi_import_late=), so this is deferred, not failed.
            wifi_import=deferred
            log "WARNING: wifi.txt import deferred (daemon not ready); master retries after the stages"
        fi
    else
        wifi_import=no-daemon
    fi
fi

# wifi.txt only carries credentials: the client mode and the radio switch are
# config keys, and the shipped default is mode=off / wlan_on=0. Importing a
# network without flipping them leaves wlan0 DOWN with a perfectly healthy daemon
# (2026-11-22: daemon_verify=ok, wifi_import=OK, wlan0 state DOWN, no address).
if [ -f "$RW_DIR/wifi.txt" ] && [ -f "$RW_DIR/config.conf" ]; then
    for _kv in mode=client wlan_on=1 boot_off=0; do
        _k=${_kv%%=*}; _v=${_kv#*=}
        if grep -q "^$_k=" "$RW_DIR/config.conf" 2>/dev/null; then
            sed -i "s/^$_k=.*/$_k=$_v/" "$RW_DIR/config.conf" 2>/dev/null || true
        else
            printf '%s\n' "$_kv" >> "$RW_DIR/config.conf" 2>/dev/null || true
        fi
    done
    chmod 600 "$RW_DIR/config.conf" 2>/dev/null || true
  # A dropped network survives in config.conf (net2..net4): the import only adds,
  # it never deletes, so an old hotspot stayed in the retry list and shared the
  # little airtime a weak AP leaves (2026-11-24: -87 dBm, two configured nets).
  # Put "wifi_primary_only" on the stick root to keep the first network and drop
  # the rest.  Must run AFTER the import, or the import puts them back.
  if [ -f "$STICK_ROOT/wifi_primary_only" ] || [ -f "$USB_DIR/wifi_primary_only" ]; then
      for _k in net2_ssid net2_psk net3_ssid net3_psk net4_ssid net4_psk; do
          sed -i "s/^$_k=.*/$_k=/" "$RW_DIR/config.conf" 2>/dev/null || true
      done
      sed -i "s/^net_count=.*/net_count=1/" "$RW_DIR/config.conf" 2>/dev/null || true
      log "wifi_primary_only: extra client networks cleared (kept '$(sed -n 's/^ssid=//p' "$RW_DIR/config.conf" | head -n1)')"
      # The master retries `--import-file /rw_data/wifi/wifi.txt` after the stages,
      # so trimming only config.conf is undone by the next import.  Reduce the file
      # itself to the first network as well.
      if [ -f "$RW_DIR/wifi.txt" ]; then
          _trim="$RW_DIR/wifi.txt.trim"
          : > "$_trim" 2>/dev/null
          _pairs=0
          while IFS= read -r _l || [ -n "$_l" ]; do
              case "$_l" in
                  ssid=*|ssid2=*|ssid3=*|ssid4=*) _pairs=$((_pairs + 1)) ;;
              esac
              if [ "$_pairs" -le 1 ]; then
                  printf '%s\n' "$_l" >> "$_trim"
              else
                  case "$_l" in
                      ssid[0-9]*=*|psk[0-9]*=*) : ;;
                      *) printf '%s\n' "$_l" >> "$_trim" ;;
                  esac
              fi
          done < "$RW_DIR/wifi.txt"
          mv -f "$_trim" "$RW_DIR/wifi.txt" 2>/dev/null && chmod 600 "$RW_DIR/wifi.txt" 2>/dev/null
          log "wifi_primary_only: wifi.txt reduced to the first network ($(grep -c '^ssid' "$RW_DIR/wifi.txt" 2>/dev/null) ssid line)"
      fi
      wifi_nets_trimmed=1
  fi
    log "config forced for client mode: mode=client wlan_on=1 boot_off=0"
fi
# ConnMan stays the association/DHCP owner in this app, and its WiFi technology
# ships POWERED OFF: with it off, wlan0 comes up but never associates and never
# gets an address (2026-11-22 probe: "Powered = False", wlan0 UP, no IPv4, no ssh).
# The stock pair is ConnMan + supplicantmanager: ConnMan powers the technology and
# asks that manager for a supplicant. Our installer masks the manager so it cannot
# fight for the control socket, but then ConnMan has nothing to power the radio
# with - the probe shows "Powered = False" and wlan0 up with no association.
# Unmask it for this boot only (the restore puts the masked state back) and start
# the path unit, then let ConnMan take over.
if command -v systemctl >/dev/null 2>&1; then
    for _u in supplicantmanager.service supplicantmanager.path; do
        systemctl unmask --runtime "$_u" >> "$LOG" 2>&1 || true
    done
    systemctl start supplicantmanager.path >> "$LOG" 2>&1 || \
        systemctl start supplicantmanager.service >> "$LOG" 2>&1 || true
    log "supplicantmanager unmasked (runtime) and started for this boot"
fi
if command -v connmanctl >/dev/null 2>&1; then
    _en=$(connmanctl enable wifi 2>&1)
    case "$_en" in
        ""|Enabled*|*ready*) log "connmanctl enable wifi: ok ($_en)" ;;
        *"doesn't exist"*|*"Unknown method"*)
            # This unit's ConnMan has no SetProperty on the wifi technology, so the
            # call cannot succeed no matter how often it is retried.  The restart
            # plus 3 s sleep bought nothing (every run since 2026-11-24 logged the
            # same error twice); say it once and move on.
            log "connmanctl enable wifi: unsupported on this ConnMan ('$_en'); no retry"
            ;;
          *)
            # ConnMan refuses to power the technology when its own supplicant never
            # came up (typical right after the installer masked the OEM manager).
            # One restart makes ConnMan spawn its supplicant again.
            log "connmanctl enable wifi: '$_en' - restarting connman and retrying"
            systemctl restart connman >> "$LOG" 2>&1 || true
            sleep 3
            _en2=$(connmanctl enable wifi 2>&1)
            log "connmanctl enable wifi (retry): $_en2"
            ;;
    esac
    log "connman wifi technology: $(connmanctl technologies 2>/dev/null | grep -A2 'technology/wifi' | tr '\n' ' ')"
fi

# ---- QML ---------------------------------------------------------------
install_qml() {
    src="$1"
    dst="$2"
    ref="$3"
    [ -f "$src" ] || fail "payload QML missing: $src"
    label=$(getfattr -n security.SMACK64 --only-values "$ref" 2>/dev/null || true)
    io_mkdir "$(dirname "$dst")" || fail "cannot create dir for $dst"
    io_copy "$src" "$dst" || fail "copy failed: $dst"
    chmod 644 "$dst" 2>/dev/null || true
    chown 0:0 "$dst" 2>/dev/null || true
    if [ -n "$label" ]; then
        # onlycap=Privileged rejects security-xattr writes from the '_' USB
        # installer context (setfattr -> EPERM), which used to abort the whole
        # install and leave the previous WifiPage.qml in place.  A file created
        # by this process already carries the '_' floor label, and every reader
        # (including AppHomeScreen) may open floor-labelled files, so keep the
        # inherited label and continue with a warning instead of rolling back.
        if setfattr -n security.SMACK64 -v "$label" "$dst" 2>/dev/null; then
            actual=$(getfattr -n security.SMACK64 --only-values "$dst" 2>/dev/null || true)
            [ "$actual" = "$label" ] || fail "QML label readback failed: $dst"
        else
            actual=$(getfattr -n security.SMACK64 --only-values "$dst" 2>/dev/null || true)
            if [ "$actual" = "$label" ]; then
                # cp -f kept the destination inode, so the copy already
                # carries exactly the target label; nothing to set.
                :
            else
                case "$actual" in
                    ""|_) log "WARNING: cannot set SMACK64=$label on $dst (onlycap?); keeping inherited floor label" ;;
                    *) fail "QML label failed: $dst (unexpected inherited label '$actual')" ;;
                esac
            fi
        fi
    fi
}

Q="$PAYLOAD/qml/AppHomeScreen/qml_eco3.0"
install_variant() {   # install_variant <src> <dst> <label>
    [ -f "$1" ] || return 0
    install_qml "$1" "$2" "$3"
}
install_variant "$Q/HomeScreen/QHMI_HomeScreenMain_Advanced.$adv_variant.qml" "$cur_adv" "$cur_adv"
install_variant "$Q/HomeScreen/QHMI_HomeScreenMain_Wide.$wide_variant.qml" "$cur_wide" "$cur_wide"
install_variant "$Q/HomeScreen/Screen_HomeScreen_AllMenus.$all_variant.qml" "$cur_all" "$cur_all"
install_variant "$Q/HomeScreen/Screen_HomeScreen_Submenu_Setup.$setup_variant.qml" "$cur_setup" "$cur_setup"
install_variant "$Q/component/QCustomizedMenuListAllowEmpty.$custom_variant.qml" "$cur_custom" "$cur_custom"
if ! grep -q 'indexOf("file://")' "$cur_theme" 2>/dev/null; then
    install_qml "$Q/component/common/Q_ThemeImage.qml" "$cur_theme" "$cur_theme"
fi

for f in HomeScreen/WifiPage.qml component/WifiTile.qml component/WifiSwitch.qml component/WifiButton.qml component/HelloTile.qml component/BatteryTile.qml; do
    [ -f "$Q/$f" ] || continue
    install_qml "$Q/$f" "$QML_DIR/$f" "$cur_adv"
done

# Clear QML caches (including the Qt5 QML disk cache, which may hold
# compiled bytecode of the previous WifiPage).
rm -rf /root/.cache/qtshadercache 2>/dev/null || true
rm -rf /root/.cache/AppHomeScreen 2>/dev/null || true
rm -rf /root/.cache/qmlcache 2>/dev/null || true
rm -rf /tmp/qmlcache 2>/dev/null || true
rm -rf /tmp/qt-qml-cache 2>/dev/null || true
sync

sync

# ---- stop OEM WiFi race / start daemon --------------------------------
oem_mask=not-attempted
if command -v systemctl >/dev/null 2>&1; then
    systemctl stop supplicantmanager.path 2>/dev/null || true
    systemctl stop supplicantmanager.service 2>/dev/null || true
    # supplicantmanager is Restart=always behind a path watcher; stopping it
    # only makes it respawn and steal /var/run/wpa_supplicant/wlan0.
    #
    # But it is ALSO what ConnMan asks for a supplicant: with it masked, ConnMan
    # has no wifi technology at all (connmanctl technologies lists cellular
    # only), so it can neither power the radio, nor associate, nor run its
    # built-in DHCP - and "no DHCP client exists on this unit" is a dead end.
    # Client mode therefore hands wifi back to the vendor pair and only keeps it
    # away in AP mode, where our own supplicant owns wlan0.
    _want_mode=$(sed -n 's/^mode=//p' "$RW_DIR/config.conf" 2>/dev/null | head -n 1)
    if [ "$_want_mode" = "client" ]; then
        for unit in supplicantmanager.path supplicantmanager.service; do
            systemctl unmask "$unit" >> "$LOG" 2>&1 || true
            systemctl enable "$unit" >> "$LOG" 2>&1 || true
        done
        systemctl start supplicantmanager.path >> "$LOG" 2>&1 || \
            systemctl start supplicantmanager.service >> "$LOG" 2>&1 || true
        systemctl daemon-reload >> "$LOG" 2>&1 || true
        log "client mode: supplicantmanager left enabled (ConnMan owns wifi + DHCP); want_mode=$_want_mode"
        oem_mask=ok
    else
    oem_mask=ok
    for unit in supplicantmanager.path supplicantmanager.service; do
        state="$RW_DIR/backup/$unit.state"
        remember "$state"
        prior=$(systemctl is-enabled "$unit" 2>/dev/null || true)
        case "$unit" in *.path) old_oem_path=$prior;; *) old_oem_service=$prior;; esac
        if [ ! -f "$state" ]; then printf '%s\n' "$prior" > "$state"; fi
        systemctl mask "$unit" >> "$LOG" 2>&1 || oem_mask=partial
    done
    rm -f /tmp/no_unece 2>/dev/null || true
    systemctl daemon-reload >> "$LOG" 2>&1 || true
fi
fi   # client mode keeps supplicantmanager, AP mode masks it
[ "$oem_mask" = ok ] || fail "OEM manager masking failed: $oem_mask"

# ConnMan's wifi plugin stays active even with the technology Powered=False, and
# it calls Interface.Disconnect over D-Bus on any supplicant it can reach: the
# supplicant log shows CONNECTED to the AP immediately followed by
# "wpas_dbus_handler_disconnect" and reason=3 locally_generated=1, in a loop.
# Our supplicant no longer publishes on D-Bus (no -u), and the technology is
# switched off here too.  Cellular is untouched - it is the unit's only uplink.
if command -v connmanctl >/dev/null 2>&1; then
    # Keep the technology POWERED: client mode hands the interface to ConnMan
    # (its agent flow does the association and the DHCP lease), so Powered=False
    # made every apply fail with "WiFi technology did not power on".  What we
    # must prevent is ConnMan taking over OUR supplicant, and that is done in the
    # daemon: client mode no longer starts one.
    connmanctl enable wifi >> "$LOG" 2>&1 || log "connmanctl enable wifi failed"
    log "connman wifi powered=$(connmanctl technologies 2>/dev/null | \
        sed -n '/\/technology\/wifi/,/Powered/p' | sed -n 's/.*Powered = //p' | head -n 1)"
fi

# Belt and braces for the current boot: the unit's RuntimeDirectory covers future
# starts, but the directory is on tmpfs and may be gone right now.
mkdir -p /var/run/wpa_supplicant 2>/dev/null && chmod 755 /var/run/wpa_supplicant 2>/dev/null
log "ctrl dir /var/run/wpa_supplicant: $(ls -ld /var/run/wpa_supplicant 2>&1)"

daemon_status="not-started"
rm -f "$RW_DIR/status.json" 2>/dev/null || fail "cannot clear old daemon status"
if command -v systemctl >/dev/null 2>&1; then
    if systemctl enable wifi-daemon.service >> "$LOG" 2>&1; then
        if systemctl restart wifi-daemon.service >> "$LOG" 2>&1; then
            daemon_status="systemd-ok"
        else
            daemon_status="systemd-restart-failed"
        fi
    else
        daemon_status="systemd-enable-failed"
    fi
fi
log "daemon=$daemon_status"
[ "$daemon_status" = systemd-ok ] || fail "daemon did not start through systemd: $daemon_status"

io_restore_root || fail "cannot restore root mount state"
# ---- optional Bluetooth tap --------------------------------------------
# Exporting the vendor BT stack for local analysis needs an LD_PRELOAD shim on
# vbtd, and that stops Bluetooth while it runs (no HFP, no media, no wireless
# CarPlay pairing).  So the stage only INSTALLS the shim and prints the exact
# command; it starts nothing unless the stick also carries bt_tap_run.
_bttap_src="$STICK_ROOT/combo/bt/libbttap.so"
[ -f "$_bttap_src" ] || _bttap_src="$STICK_ROOT/libbttap.so"
if [ -f "$_bttap_src" ]; then
    mkdir -p "$RW_DIR/bt" 2>/dev/null
    cp -f "$_bttap_src" "$RW_DIR/bt/libbttap.so" 2>/dev/null || true
    chmod 755 "$RW_DIR/bt/libbttap.so" 2>/dev/null || true
    log "bt tap shim installed: $RW_DIR/bt/libbttap.so"
    if [ -f "$STICK_ROOT/bt_tap_run" ]; then
        _btpid=$(pidof vbtd 2>/dev/null | awk '{print $1}')
        if [ -n "$_btpid" ]; then
            tr '\0' ' ' < "/proc/$_btpid/cmdline" > "$RW_DIR/bt/vbtd_cmdline.txt" 2>/dev/null || true
            log "bt tap: vbtd cmdline saved; stopping vbtd for the tap (Bluetooth is off until it restarts)"
            kill -TERM "$_btpid" 2>/dev/null || true
            sleep 2
            BT_TAP_OUT="$RW_DIR/bt/tap.bin" BT_TAP_DEV=/dev/ttyS0 \
                LD_PRELOAD="$RW_DIR/bt/libbttap.so" \
                /app/bin/vbtd -d /dev/ttyS0 -p /tmp/bt_patchram.hcd \
                >> "$RW_DIR/bt/vbtd_tapped.log" 2>&1 &
            sleep 1
            log "bt tap: vbtd restarted under the shim ($(pidof vbtd 2>/dev/null))"
        else
            log "bt tap: vbtd not running; shim installed but not attached"
        fi
    else
        log "bt tap: shim only - add bt_tap_run to the stick to attach it (stops Bluetooth)"
    fi
fi
# ---- address fallback ---------------------------------------------------
# The association is ours (our supplicant holds the link), but nothing hands out
# a lease on this unit.  Record which clients exist and, when every one is
# missing or fails, apply the static address from config.conf (fed from the
# static_ip / static_gw files on the stick) so a successful association is
# enough to reach the unit.
_dhcp_bins=$(command -v udhcpc dhcpcd dhclient 2>/dev/null | tr '\n' ' ')
log "dhcp clients: ${_dhcp_bins:-none}busybox=$(command -v busybox 2>/dev/null)"
_static_ip=$(sed -n 's/^static_ip=//p' "$RW_DIR/config.conf" 2>/dev/null | head -n 1)
# Iteration speed matters more than patience: with no DHCP client and no static
# address there is nothing to wait for, so the 90 s report wait below is skipped
# too (it burned three minutes per run for an address that could not arrive).
_have_dhcp=1
[ -n "$_dhcp_bins" ] || [ -x /bin/busybox ] || _have_dhcp=0
if [ "$_have_dhcp" = 0 ] && [ -z "$_static_ip" ]; then
    log "no dhcp client and no static_ip: skipping the address wait"
else
    _ipwait=0
    while [ "$_ipwait" -lt 30 ]; do
        ip -4 addr show wlan0 2>/dev/null | grep -q "inet " && break
        _ipwait=$((_ipwait + 1))
        sleep 1
    done
fi
_static_gw=$(sed -n 's/^static_gw=//p' "$RW_DIR/config.conf" 2>/dev/null | head -n 1)
if ! ip -4 addr show wlan0 2>/dev/null | grep -q "inet " && [ -n "$_static_ip" ]; then
    case "$_static_ip" in */*) _cidr=$_static_ip ;; *) _cidr=$_static_ip/24 ;; esac
    ip addr flush dev wlan0 >> "$LOG" 2>&1 || true
    if ip addr add "$_cidr" dev wlan0 >> "$LOG" 2>&1; then
        [ -n "$_static_gw" ] && ip route replace default via "$_static_gw" dev wlan0 >> "$LOG" 2>&1 || true
        log "static address applied: $_cidr gw=${_static_gw:-none}"
    else
        log "static address FAILED: $_cidr"
    fi
fi
log "wlan0 addr: $(ip -4 addr show wlan0 2>/dev/null | grep "inet " | tr '\n' ' ')"

# ---- restart AppHomeScreen --------------------------------------------
io_capture_qml "$QML_DIR" "$USB_DIR/qml_after" || log "WARNING: QML after-snapshot unavailable"
restart="not-attempted"
# The manifest paths are needed by the deferred path too (they are copied into
# last_manifest.md5 after the daemon check).  Declaring them inside the else
# branch below meant that with SCOURGE_DEFER_UI_RESTART=1 - the combo master's
# normal mode since 2026-11-24 - the stage died on `set -u` at the `cp` far
# below, rolled itself back and reported rc=1 after a clean install (on-car
# 2026-11-24 20:28, "line 1501: _cur_manifest: unbound variable").
_prev_manifest="$RW_DIR/last_manifest.md5"
_cur_manifest="$PAYLOAD/manifest.md5"
_qml_changed=0
if [ "${SCOURGE_DEFER_UI_RESTART:-0}" = 1 ]; then
    # The master restarts AppHomeScreen once after every stage has installed,
    # instead of one restart per package (4-5 UI flickers per run, 2026-11-24).
    restart="deferred"
else
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
# Did the payload change since the last run?  The manifest covers every
# installed file (payload + stage.sh), so a digest comparison is the honest
# signal for "the UI must restart".  Listing individual QML paths broke once:
# NEW_FILES was used before it was defined and set -u killed the whole stage.
if [ -f "$_prev_manifest" ] && [ -f "$_cur_manifest" ]; then
    cmp -s "$_prev_manifest" "$_cur_manifest" || _qml_changed=1
fi
if [ "$_qml_changed" = 1 ] && [ -f "${SCOURGE_STICK_DIR:-$USB_DIR}/no_ui_restart" ]; then
    log "no_ui_restart ignored: the payload changed, the UI must restart"
fi
if [ -f "${SCOURGE_STICK_DIR:-$USB_DIR}/no_ui_restart" ] && [ "$_qml_changed" = 0 ]; then
    # Iterating on the daemon only? The UI restart costs ~10 s and the QML is
    # unchanged, so let the stick say so.
    restart="skipped-flag verified=1"
    log "UI restart skipped: no_ui_restart on the stick"
elif command -v dbus-send >/dev/null 2>&1; then
    dbus-send --session --type=method_call --reply-timeout=3000 --dest=com.lge.avn.applauncher /Request \
        com.lge.avn.applauncher.request.terminate int32:11 >> "$LOG" 2>&1 || true
    sleep 2
    restart="dbus-launch-failed"
    for i in 1 2 3; do
        if dbus-send --session --type=method_call --reply-timeout=3000 --dest=com.lge.avn.applauncher /Request \
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
case "$restart" in
    skipped-flag*|dbus-ok) : ;;
    *)
        for p in /proc/[0-9]*; do
            [ -r "$p/comm" ] || continue
            if [ "$(cat "$p/comm" 2>/dev/null)" = "AppHomeScreen" ]; then
                kill -TERM "${p#/proc/}" 2>/dev/null || true
            fi
        done
        restart="$restart (killed AppHomeScreen; reboot if UI does not return)"
        ;;
esac
# The new QML is only live once AppHomeScreen has actually restarted.  The
# daemon updates via systemd, but the UI process keeps the old QML in
# memory, so verify and tell the user when a reboot is required.
case "$restart" in
    skipped-flag*) : ;;
    *)
        sleep 3
        new_home_pid=$(pidof AppHomeScreen 2>/dev/null || true)
        if [ -n "$new_home_pid" ] && [ "$new_home_pid" != "$old_home_pid" ]; then
            restart="$restart verified=1"
        else
            restart="$restart verified=0 REBOOT_REQUIRED"
        fi
        ;;
esac
fi
log "restart=$restart"
sync

# A running PID alone is not proof of the deployed daemon version. The daemon has
# to come up, write status.json with THIS build id, and its /proc/PID/exe has to
# be the payload binary. Waiting long is cheap; failing the stage after the files
# are already installed is not: it stopped the combo before tweaks and left the
# unit half-verified (see the 2026-11-22 run, "build identity did not match").
# The radio itself is the bottleneck: bcmdhd registers wlan0 ~110 s after boot
# (2026-11-22 probe, dmesg "Register interface [wlan0]"), and the unit's
# ExecStartPre waits for it too. So verify over three minutes, not thirty
# seconds - but give up 20 s after the interface appeared and still nothing,
# because that is a real failure and not a timing one.
# Load the radio ourselves first: modprobe is idempotent and takes ~1 s, while
# waiting for the OEM to load it costs ~110 s of boot time.  The unit file does
# the same in ExecStartPre, so this only moves the moment it happens.
if [ ! -e /sys/class/net/wlan0 ]; then
    modprobe bcmdhd >> "$LOG" 2>&1 || log "modprobe bcmdhd failed (continuing)"
    _i=0
    while [ ! -e /sys/class/net/wlan0 ] && [ "$_i" -lt 20 ]; do
        sleep 1; _i=$((_i + 1))
    done
    log "wlan0 after modprobe: $([ -e /sys/class/net/wlan0 ] && echo present || echo absent) after ${_i}s"
fi

# The master can own the long wait: install + daemon + UI restart happen
# here, then the run waits for the association at the very end, so the
# driver can watch the Live log instead of a dark screen.
DEFER_WAIT=${SCOURGE_DEFER_WIFI_WAIT:-0}
_verify_budget=30
[ "${DEFER_WAIT}" = 1 ] && _verify_budget=20
verified_build=0
_i=0
_first_seen=""
while [ "$_i" -lt 30 ]; do
    if systemctl is-active --quiet wifi-daemon.service 2>/dev/null && \
       grep -q "\"build_id\":\"$BUILD_ID\"" "$RW_DIR/status.json" 2>/dev/null; then
        verified_build=1
        break
    fi
    [ "$_i" = 0 ] && systemctl start wifi-daemon.service >> "$LOG" 2>&1 || true
    if [ -e /sys/class/net/wlan0 ]; then
        [ -n "$_first_seen" ] || _first_seen=$_i
        [ "$((_i - _first_seen))" -ge 20 ] && break
    fi
    sleep 1
    _i=$((_i + 1))
done
main_pid=$(systemctl show -p MainPID wifi-daemon.service 2>/dev/null | sed -n 's/^MainPID=//p')
if [ "$verified_build" != 1 ]; then
    daemon_verify=unverified
    probe_radio "install: daemon did not verify" fast
    log "WARNING: wifi daemon did not report build_id=$BUILD_ID within 30 s"
    { systemctl status wifi-daemon.service --no-pager 2>&1 | head -n 12
      echo '--- journal'
      journalctl -u wifi-daemon.service -n 12 --no-pager 2>&1 | tail -n 12
    } >> "$LOG" 2>&1 || true
elif [ "$(md5 "/proc/$main_pid/exe")" != "$(md5 "$PAYLOAD/bin/wifi_daemon")" ]; then
    daemon_verify=bytes-mismatch
    log "WARNING: running daemon bytes differ from the payload ($main_pid)"
else
    daemon_verify=ok
fi
if [ "$new_restore_point" = 1 ]; then ln -sfn "$BACKUP_HU" "$RW_DIR/backup/latest" || fail "cannot record restore point"; fi
[ -n "${_cur_manifest:-}" ] && cp -f "$_cur_manifest" "$_prev_manifest" 2>/dev/null || true
install_complete=1
final_status=OK
case "$restart" in *verified=0*) final_status=REBOOT_REQUIRED;; esac
# A daemon that did not verify does not undo the install: the files, the page and
# the tile are in place, and a reboot usually brings the service up. Report it
# instead of aborting, so the next stage can still run.
case "$daemon_verify" in
    ok|"") : ;;
    *) final_status="DAEMON_UNVERIFIED"; daemon_status="$daemon_status+$daemon_verify" ;;
esac

wifi_net_report() {
    # One short, greppable file on the STICK ROOT: what it associated with, what
    # address it got, and whether ssh is listening. No guessing from probe dumps.
    _sf="$RW_DIR/status.json"
    # Write where the owner will look: the stick root, then the stage dir, then
    # /rw_data. Pick a writable candidate instead of trusting the environment (the
    # 16:09 run produced no report at all).
    _dir=""
    for _c in "${SCOURGE_STICK_DIR:-}" "${USB_DIR:-}" "$RW_DIR"; do
        [ -n "$_c" ] || continue
        [ -d "$_c" ] || continue
        [ -w "$_c" ] || continue
        _dir="$_c"; break
    done
    _out="${_dir:-/tmp}/wifi_net.txt"
    _field() {   # _field <key>: quoted or bare JSON value
        sed -n "s/.*\"$1\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\".*/\1/p" "$_sf" 2>/dev/null | head -n1
    }
    _num() {
        sed -n "s/.*\"$1\"[[:space:]]*:[[:space:]]*\(-\?[0-9][0-9]*\).*/\1/p" "$_sf" 2>/dev/null | head -n1
    }
    _bool() {   # unquoted true/false JSON value (booleans are not strings)
        sed -n "s/.*\"$1\"[[:space:]]*:[[:space:]]*\(true\|false\).*/\1/p" "$_sf" 2>/dev/null | head -n1
    }
    # Say so immediately: the file exists from the first second, so "no end beep
    # yet" can be told apart from "still waiting for an address" without guessing.
    printf '=== SCOURGE wifi network (waiting for an address, started %s)\n' "$(date -u 2>/dev/null)" > "$_out"
    sync 2>/dev/null || true
    _i=0
    _off=0
    # Shorter than the old 45x2 s: either the association comes up quickly or it
    # does not, and every extra second here is a second per iteration.
    [ "${_have_dhcp:-1}" = 0 ] && [ -z "${_static_ip:-}" ] && _i=45
    while [ "$_i" -lt 30 ]; do
        # No point waiting for an address while ConnMan's wifi technology is off.
        if connmanctl technologies 2>/dev/null | grep -A2 'technology/wifi' | grep -q 'Powered = False'; then
            _off=$((_off + 2))
            [ "$_off" -lt 16 ] || break
        else
            _off=0
        fi
        _ip=$(_field ip); [ -n "$_ip" ] || _ip=$(ip -4 -o addr show wlan0 2>/dev/null | awk '{print $4}' | head -n1)
        [ -n "$_ip" ] && break
        sleep 1; _i=$((_i + 1))
    done
    # The daemon reports only what it can see itself, and on this unit it saw
    # nothing: status.json carried ip="" ssid="" signal=0 while wlan0 really had
    # 192.0.2.60/24 (23:52 run).  The report therefore falls back to the kernel
    # view, so a working association is never published as "no address".
    _ssid=$(_field ssid)
    _bssid=$(_field bssid)
    _state=$(_field state)
    _sig=$(_num signal)
    if [ -z "$_ssid" ] || [ "${_sig:-0}" = 0 ]; then
        _link=$(iw dev wlan0 link 2>/dev/null)
        [ -n "$_ssid" ] || _ssid=$(printf '%s\n' "$_link" | sed -n 's/^[[:space:]]*SSID[[:space:]]*:[[:space:]]*//p' | head -n1)
        [ "${_sig:-0}" != 0 ] || _sig=$(printf '%s\n' "$_link" | sed -n 's/^[[:space:]]*signal:[[:space:]]*\(-\?[0-9][0-9]*\).*/\1/p' | head -n1)
    fi
    {
        echo "=== SCOURGE wifi network ($(date -u 2>/dev/null))"
        echo "ssid=${_ssid:-none}"
        echo "bssid=${_bssid:-none}"
        echo "state=${_state:-none}"
        echo "ip=${_ip:-none}"
        echo "signal_dbm=${_sig:-0}"
        # The daemon states why SSH is not up ("SSH deferred: ...", "SSH started on
        # port 2222", "dropbear binary missing"): the next run then answers the
        # question instead of leaving it to a guess.
        echo "daemon_msg=$(_field message)"
        # Is the daemon's picture of the radio current?  A stale status.json is what
        # made "Applying mode..." coexist with a live 192.0.2.60 address
        # (2026-11-24); the age and the flag make that readable from the stick.
        _upd=$(_num updated_ms)
        _now=$(date +%s 2>/dev/null || echo 0)
        _age=unknown
        case "$_upd" in ''|*[!0-9]*) : ;; *) [ "$_now" -gt 0 ] && _age=$((_now - _upd / 1000)) ;; esac
        echo "observations_fresh=$(_bool observations_fresh)  status_age_s=$_age"
        echo "applying=$(_bool applying)"
        echo "ssh_active=$(_bool ssh_active)  ssh_enabled=$(_bool ssh_enabled)  ssh_port=$(_num ssh_port)"
        echo "gateway=$(ip route 2>/dev/null | awk '/^default/{print $3; exit}')"
        echo "--- wlan0"
        (ip -4 -o addr show wlan0 2>/dev/null || ifconfig wlan0 2>/dev/null | head -n 4)
        echo "--- wpa_cli"
        wpa_cli -i wlan0 status 2>/dev/null | sed -n '1,8p'
        echo "--- connman"; connmanctl technologies 2>/dev/null | grep -A2 'technology/wifi'
        echo "--- credentials in config.conf (psk hidden)"
        grep -E '^(mode|wlan_on|boot_off|ssid|ssid2|ssid3|static_ip|static_gw)=' "$RW_DIR/config.conf" 2>/dev/null
        echo "--- wifi.txt"; [ -f "$RW_DIR/wifi.txt" ] && echo "present ssid_lines=$(grep -cE '^ssid' "$RW_DIR/wifi.txt" 2>/dev/null)" || echo absent
        echo "--- wpa_cli"; wpa_cli -i wlan0 status 2>&1 | sed -n '1,6p'
        echo "--- supplicant procs"; pidof wpa_supplicant 2>/dev/null; ls -l /var/run/wpa_supplicant 2>/dev/null | head -n 4
        echo "--- listeners"
        (ss -ltn 2>/dev/null || netstat -ltn 2>/dev/null) | grep -E ':(2222|8090)' || echo "no 2222/8090 listener"
    } > "$_out" 2>&1
    sync 2>/dev/null || true
    log "wifi network report written: $_out"
}

# Always leave a radio snapshot on the stick, success or not: the working case
# is exactly when we need the unit's address to ssh in afterwards.
wifi_net_report

# ---- start SSH as soon as there is an address ---------------------------
# The daemon starts dropbear itself once wlan0 has an IPv4, but a gate drift left
# SSH deferred through several runs.  Ask it explicitly once an address exists,
# then report what is actually listening.
# The stick is FAT: every file lands 0644 there, so the old [ -x ] test was false
# on the unit and the kick was skipped on every run ("ssh request skipped: no
# httpreq on the unit", 23:50).  Stage a copy under /rw_data, where chmod works.
httpreq_bin() {
    [ -n "${_HTTPREQ_CACHED:-}" ] && { printf '%s' "$_HTTPREQ_CACHED"; return 0; }
    for _c in "$STICK_ROOT/combo/bin/httpreq" "$USB_DIR/../bin/httpreq" "$USB_DIR/bin/httpreq"; do
        [ -f "$_c" ] || continue
        mkdir -p "$RW_DIR/bin" 2>/dev/null || true
        cp -f "$_c" "$RW_DIR/bin/httpreq" 2>/dev/null || continue
        chmod 755 "$RW_DIR/bin/httpreq" 2>/dev/null || continue
        _HTTPREQ_CACHED="$RW_DIR/bin/httpreq"
        printf '%s' "$_HTTPREQ_CACHED"
        return 0
    done
    return 1
}
_wip=$(ip -4 -o addr show wlan0 2>/dev/null | awk '{print $4}' | head -n1)
if [ -n "$_wip" ]; then
    _rq=$(httpreq_bin || true)
    if [ -n "$_rq" ]; then
        "$_rq" POST 127.0.0.1 8090 "/ssh?enabled=1&port=2222" >> "$LOG" 2>&1 || log "ssh request failed (httpreq)"
    else
        log "ssh request skipped: no httpreq on the unit"
    fi
    sleep 3
    _l=$( (ss -ltn 2>/dev/null || netstat -ltn 2>/dev/null) | grep 2222 | head -n1 )
    log "ssh: wlan0=$_wip listener=${_l:-none}"
    # Last resort, because SSH is the only way to iterate without another stick
    # round: a worker stuck in "Applying mode..." keeps applying=true forever and
    # the daemon never reaches ssh_start (2026-11-24 20:36 and 20:54 snapshots).
    # The started copy keeps its OWN pid file on purpose (dropbear-stage.pid, not
    # dropbear.pid): the daemon's apply cycle calls stop_all() -> ssh_stop(), and
    # with 10 supplicant restarts and 34 apply cycles in one log window
    # (2026-11-24) it also tore down the link it had just built (reason=3,
    # locally_generated=1). An SSH session that survives that churn is worth more
    # than a tidy pid file; disable SSH on the page to get rid of it.
    if [ -z "$_l" ]; then
        _db=""
        for _c in "$RW_DIR/bin/dropbear" "$RW_DIR/bin/dropbearmulti"; do
            [ -f "$_c" ] && { _db="$_c"; break; }
        done
        if [ -n "$_db" ]; then
            chmod 755 "$_db" 2>/dev/null || true
            mkdir -p "$RW_DIR/ssh" 2>/dev/null || true
            _ed="$RW_DIR/ssh/dropbear_ed25519_host_key"
            _rsa="$RW_DIR/ssh/dropbear_rsa_host_key"
            _dkey="$RW_DIR/bin/dropbearkey"
            if [ -x "$_dkey" ]; then
                [ -f "$_ed" ] || "$_dkey" -t ed25519 -f "$_ed" >> "$LOG" 2>&1 || true
                [ -f "$_rsa" ] || "$_dkey" -t rsa -s 2048 -f "$_rsa" >> "$LOG" 2>&1 || true
            fi
            if [ -f "$_ed" ] && [ -f "$_rsa" ]; then
                "$_db" -r "$_ed" -r "$_rsa" -p "$_wip:2222" -P "$RW_DIR/ssh/dropbear-stage.pid" \
                    >> "$RW_DIR/ssh.log" 2>&1 &
            else
                "$_db" -R -p "$_wip:2222" -P "$RW_DIR/ssh/dropbear-stage.pid" \
                    >> "$RW_DIR/ssh.log" 2>&1 &
            fi
            sleep 2
            _l=$( (ss -ltn 2>/dev/null || netstat -ltn 2>/dev/null) | grep 2222 | head -n1 )
            log "ssh: direct dropbear start listener=${_l:-none}"
        else
            log "ssh: no dropbear binary under $RW_DIR/bin; cannot start it directly"
        fi
    fi
else
    log "ssh: no wlan0 address yet; dropbear stays down"
fi


# ---- API walk: removed 2026-11-25 -----------------------------------------
_HTTPRQ=$(httpreq_bin || true)
_api() {
    [ -n "$_HTTPRQ" ] || { echo "{\"error\":\"no httpreq\"}"; return 1; }
    "$_HTTPRQ" POST 127.0.0.1 8090 "$1" 2>/dev/null | head -c 400
}
_apistatus() {
    [ -n "$_HTTPRQ" ] || { echo "{\"error\":\"no httpreq\"}"; return 1; }
    "$_HTTPRQ" GET 127.0.0.1 8090 /status 2>/dev/null | head -c 1200
}
# The one-round API walk and its api_walk.txt collection were removed here
# (2026-11-25): ~100 s of deliberate sleeps inside the wifi stage, only a
# convenience for the AA work.  The same calls can be made one at a time from the
# UI or over ssh.

# ---- vendor BT API measurement (AA package prober) ------------------------
# The AA package has no installer of its own, so this stage is the only writer of
# /rw_data/aa/bt_api.json (agreed with the AA tab, 2026-11-24).  The probe target
# is the RUNNING object (/proc/<pid>/exe): the shim resolves the symbols with
# dlsym(RTLD_DEFAULT) inside that process, so "does the loaded object export it"
# is the question, not "which file has the string".
bt_api_probe() {
    mkdir -p /rw_data/aa 2>/dev/null || true
    _prober=""
    # The AA prober ships inside the wifi payload (combo/wifi/payload/bin) and the
    # stage installs it to $RW_DIR/bin a few lines into this script, so those are
    # the real locations; combo/aa is for a standalone AA bundle that does not
    # exist yet, and $USB_DIR/bin never did (it cost two runs of
    # "vbtd_api not staged, bt_api.json not written", 2026-11-24 20:50).
    for _c in "$PAYLOAD/bin/vbtd_api.target" "$RW_DIR/bin/vbtd_api.target" \
              "$STICK_ROOT/combo/aa/vbtd_api.target" "$USB_DIR/../aa/vbtd_api.target"; do
        [ -f "$_c" ] && { _prober="$_c"; break; }
    done
    if [ -z "$_prober" ]; then
        log "vbtd_api not staged, bt_api.json not written"
        rm -f /rw_data/aa/bt_api.json 2>/dev/null || true
        return 0
    fi
    _pid=$(pidof vbtd 2>/dev/null | cut -d' ' -f1)
    _exe=""
    [ -n "$_pid" ] && _exe=$(readlink -f "/proc/$_pid/exe" 2>/dev/null)
    [ -n "$_exe" ] || _exe=/app/bin/vbtd
    mkdir -p "$RW_DIR/bin" 2>/dev/null || true
    if ! cp -f "$_prober" "$RW_DIR/bin/vbtd_api" 2>/dev/null || ! chmod 755 "$RW_DIR/bin/vbtd_api" 2>/dev/null; then
        log "vbtd_api: cannot stage the prober; bt_api.json not written"
        return 0
    fi
    # The running object first; the CLI versions are the fallback when the pid is
    # gone or /proc is unreadable.  The prober exits 1 when a symbol is missing but
    # still writes a complete report, so only a non-zero *without* output is fatal.
    _measured=""
    for _cand in "$_exe" /app/bin/vbtd /app/bin/vbtd-51 /app/bin/vbtd-63; do
        [ -n "$_cand" ] || continue
        [ -f "$_cand" ] || continue
        if "$RW_DIR/bin/vbtd_api" "$_cand" > /rw_data/aa/bt_api.json 2>>"$LOG"; then
            log "bt_api: measured $_cand (pid ${_pid:-none}, every AA symbol exported)"
        else
            log "bt_api: measured $_cand (pid ${_pid:-none}, symbols missing, see bt_api.json)"
        fi
        [ -s /rw_data/aa/bt_api.json ] || { rm -f /rw_data/aa/bt_api.json 2>/dev/null || true; continue; }
        _measured="$_cand"
        break
    done
    if [ -z "$_measured" ]; then
        log "bt_api: no vendor binary could be measured"
        return 1
    fi
    # Same reason as bt_tap.bin: the owner reads the measurement from the stick.
    cp -f /rw_data/aa/bt_api.json "${SCOURGE_STICK_DIR:-$USB_DIR}/bt_api.json" 2>/dev/null || true
    return 0
}
bt_api_probe || true

log "step: end-of-install probe"
probe_radio "end of install (daemon_verify=${daemon_verify:-unknown})"
log "step: probe done"
probe_aa

# Publish the collected reports at the very end, in one go: anything written to
# the stick earlier restarts the loader (md5 gate), and each restart beeps.
_stick="${SCOURGE_STICK_DIR:-$USB_DIR}"
if [ -w "$_stick" ]; then
    for _f in wifi_probe.txt aa_probe.txt; do
        [ -f "$RW_DIR/$_f" ] && cp -f "$RW_DIR/$_f" "$_stick/$_f" 2>/dev/null || true
    done
    # The Bluetooth tap is only produced when the stick carried `bt_tap_run`, and
    # it is the evidence for whether the vendor stack's RFCOMM layer is alive at
    # all (tools/bttap/hci_decode.py on the host). It used to stay on the unit,
    # so getting it home needed SSH or dd; it travels with the reports instead.
    if [ -f "$RW_DIR/bt/tap.bin" ]; then
        if cp -f "$RW_DIR/bt/tap.bin" "$_stick/bt_tap.bin" 2>/dev/null; then
            log "bt tap published: bt_tap.bin"
        else
            log "bt tap: copy to the stick failed, it stays at $RW_DIR/bt/tap.bin"
        fi
    fi
    sync 2>/dev/null || true
fi

result "status=$final_status" "variant=$adv_variant/$wide_variant/$all_variant/$setup_variant/$custom_variant" \
    "tile_skipped=${variant_missing:+1}${variant_missing:-0}" "nets_trimmed=${wifi_nets_trimmed:-0}" \
       "backup=$BACKUP_HU" "daemon=$daemon_status" "daemon_verify=$daemon_verify" \
       "oem_mask=$oem_mask" "regd=$regd_status" \
       "wifi_import=$wifi_import" "restart=$restart" "build_id=$BUILD_ID" \
       \
       "mdnsd_patch=$mdnsd_patch" "asset_warnings=$ASSET_WARNINGS" "log=$LOG"
log "=== wifi payload done ==="
exit 0
