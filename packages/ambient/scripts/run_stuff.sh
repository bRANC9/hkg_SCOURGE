#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# AMBIENT_PAYLOAD_REV=1
# Ambience ambient-lighting app for Ioniq 5 Gen5W (260128).
#
# Put this file, payload/, manifest.md5 and README.txt at the ROOT of a
# FAT/exFAT USB stick (or let the Store run it from its stage directory; the
# installer only needs USB_DIR).  The initial-root failsafe runs it as root
# within ~8 s of a content change.
#
# Installs:
#   /app/share/AppHomeScreen/qml_eco3.0/HomeScreen/AmbientPage.qml
#   /app/share/AppHomeScreen/qml_eco3.0/component/AmbientTile.qml
#   /app/share/AppHomeScreen/qml_eco3.0/component/AmbientSlider.qml
#   /app/share/AppHomeScreen/ambient/{ambient_icon_n,ambient_icon_d}.png
#   /rw_data/ambient/ambient_daemon        (+ bin/ helpers)
#   /etc/systemd/system/ambient-daemon.service
# and injects the AmbientTile + AmbientPage instances into the live
# QHMI_HomeScreenMain_Advanced.qml (no build-time base is needed).
#
# It does not flash firmware and does not touch the updater, bootloader or
# partitions.
set -u
PATH="${AMBIENT_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}"
USB_DIR="${USB_DIR:-/media/usb}"
PAYLOAD="$USB_DIR/payload"
QML_ROOT="${AMBIENT_QML_ROOT:-/app/share/AppHomeScreen}"
QML_DIR="$QML_ROOT/qml_eco3.0"
ASSET_DIR="$QML_ROOT/ambient"
RW_DIR="${AMBIENT_RW_DIR:-/rw_data/ambient}"
LOG="$RW_DIR/install.log"
OUT="$USB_DIR/run_stuff_result.txt"
SERVICE="${AMBIENT_SERVICE:-/etc/systemd/system/ambient-daemon.service}"
SMACK_SELF="${AMBIENT_SMACK_SELF:-}"

# Audio source probe: the reactive mode needs a capture stream (S16LE mono on
# stdout via --audio-cmd, or a FIFO via --audio-file).  Nothing on this unit has
# been verified as capturable yet, so record what exists before promising music
# reactivity: a loopback (snd-aloop), a monitor PCM, or arecord itself.
audio_probe(){
    _o="$RW_DIR/audio_probe.txt"
    {
        echo "=== SCOURGE audio probe ($(date -u 2>/dev/null))"
        echo "--- cards"; cat /proc/asound/cards 2>&1 | head -n 12
        echo "--- pcm devices"; ls /dev/snd 2>&1 | head -n 16
        echo "--- pcm list"; cat /proc/asound/pcm 2>&1 | head -n 12
        echo "--- snd modules"; lsmod 2>/dev/null | grep -E "^snd|aloop|loopback" | head -n 10
        echo "--- capture tools"; for _t in arecord aplay parec pacat sox; do printf "%s=%s\n" "$_t" "$(command -v "$_t" 2>/dev/null || echo none)"; done
        echo "--- capture devices (arecord -l)"; (arecord -l 2>&1 || true) | head -n 8
        echo "--- loopback module present?"; (modinfo snd-aloop >/dev/null 2>&1 && echo yes || echo "no snd-aloop module")
        # The beep path proves playback works (aplay on beep-start.dat), so the
        # card exists; the question for reactive mode is whether the same card
        # exposes a CAPTURE PCM.  /proc/asound/devices lists p/c nodes and a short
        # arecord run answers "may we read it" without guessing.
        echo "--- playback devices (aplay -l)"; (aplay -l 2>&1 || true) | head -n 10
        echo "--- aplay -L (names)"; (aplay -L 2>&1 || true) | head -n 12
        echo "--- /proc/asound/devices"; cat /proc/asound/devices 2>&1 | head -n 16
        echo "--- capture test (1 s, raw to /dev/null)"
        if command -v arecord >/dev/null 2>&1; then
            ( arecord -q -d 1 -f S16_LE -r 16000 -c 1 -t raw /dev/null >/tmp/arec.out 2>&1; echo "rc=$?" >>/tmp/arec.out ) & _ap=$!
            _i=0; while [ "$_i" -lt 5 ] && kill -0 "$_ap" 2>/dev/null; do sleep 1; _i=$((_i + 1)); done
            kill "$_ap" 2>/dev/null; wait "$_ap" 2>/dev/null
            head -n 4 /tmp/arec.out 2>/dev/null
        else
            echo "no arecord on this unit"
        fi
        # The external sound generator (VESS) is its own module with its own DSP
        # firmware; the head unit only carries a volume level and a visibility flag
        # (AppSetup: VESS_VolLevel_RValue, APP_SETTING_VESS_VISIBLE) stored in the
        # setup database.  Record what this trim actually has, so "can we play our
        # own audio there" is answered by evidence, not by hope.
        echo "--- VESS / ANC settings present?"
        for _db in /app/data/AppSetup/setup_database.db /app/share/AppSetup/data/setup_database.db; do
            [ -f "$_db" ] || continue
            echo "  $_db"
            grep -a -o -iE "VESS[A-Za-z_]*|APP_SETTING_VESS_VISIBLE|activeNoiseControl" "$_db" 2>/dev/null | sort -u | head -n 10
        done
        echo "--- vehicle_settings.json (VESS/ANC keys)"
        grep -o -iE "\"[A-Za-z_]*([Vv]ess|AVAS|NoiseControl)[A-Za-z_]*\"[[:space:]]*:[[:space:]]*[^,}]*" /app/share/AppSetup/data/vehicle_settings.json 2>/dev/null | head -n 10
        echo "--- VESS updater present?"; ls -l /app/bin/vess_fw_updater* 2>/dev/null | head -n 4
        echo "--- VESS image staging dir"; ls /update/extInfoUpdate 2>/dev/null | grep -i vess | head -n 6
        echo "--- daemon audio config"; curl -s --max-time 3 http://127.0.0.1:8094/status 2>/dev/null | head -c 400 || echo "(daemon http not reachable)"
    } > "$_o" 2>&1
}
ADV="HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
NEW_FILES="HomeScreen/AmbientPage.qml
component/AmbientTile.qml
component/AmbientSlider.qml"

mkdir -p "$RW_DIR" 2>/dev/null
mkdir -p "$RW_DIR/backup" 2>/dev/null
: > "$LOG" 2>/dev/null

log() {
    echo "$(date -u 2>/dev/null) $*" >> "$LOG" 2>/dev/null
}

result() {
    {
        echo "AMBIENT_PAYLOAD"
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

# The privileged side of the app runs in the installer's own SMACK context
# (the floor label "_" in the normal USB/Store flow).
smack_init() {
    if [ -n "${AMBIENT_SMACK_SELF:-}" ]; then
        SMACK_SELF=$AMBIENT_SMACK_SELF
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

mount -o remount,rw "$USB_DIR" 2>/dev/null || true

log "=== ambient payload start usb=$USB_DIR ==="
smack_init

[ -d "$PAYLOAD" ] || fail "payload directory missing: $PAYLOAD"
[ -f "$USB_DIR/manifest.md5" ] || fail "manifest.md5 missing"
[ -d "$QML_DIR" ] || fail "expected QML directory missing: $QML_DIR (not qml_eco3.0?)"

if ! ( cd "$USB_DIR" && md5sum -c manifest.md5 ) > "$RW_DIR/manifest-check.log" 2>&1; then
    log "manifest check failed"
    fail "manifest check failed (see $RW_DIR/manifest-check.log)"
fi
log "manifest OK"

cur="$QML_DIR/$ADV"
[ -f "$cur" ] || fail "target file missing: $cur"

# Back up the Advanced file once per install (restore point for the injector).
STAMP=$(date -u +%Y%m%dT%H%M%SZ 2>/dev/null)
[ -n "$STAMP" ] || STAMP=manual
BACKUP_HU="$RW_DIR/backup/$STAMP"
mkdir -p "$BACKUP_HU/HomeScreen" 2>/dev/null || fail "cannot create $BACKUP_HU"
cp -f "$cur" "$BACKUP_HU/$ADV" || fail "backup failed: $ADV"
[ -f "$RW_DIR/backup/latest/$ADV" ] || ln -sfn "$BACKUP_HU" "$RW_DIR/backup/latest" 2>/dev/null || true
log "HU backup: $BACKUP_HU"

. "$PAYLOAD/lib/install_io.sh"
io_init || fail "cannot identify root mount"
trap 'io_restore_root || true' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP
io_settle_root || fail "root filesystem is not stably writable"

# Assets.
for f in "$PAYLOAD/assets/ambient/"*; do
    [ -f "$f" ] || continue
    io_asset "$f" "$ASSET_DIR/$(basename "$f")"
done
chmod 644 "$ASSET_DIR/"* 2>/dev/null || true

# Daemon.  Remove the old file first: cp -f onto an existing inode keeps its
# extended attributes, including a stale SMACK64EXEC from an older install.
mkdir -p "$RW_DIR" 2>/dev/null || fail "cannot create $RW_DIR"
rm -f "$RW_DIR/ambient_daemon" 2>/dev/null || true
cp -f "$PAYLOAD/bin/ambient_daemon" "$RW_DIR/ambient_daemon" || fail "daemon copy failed"
chmod 755 "$RW_DIR/ambient_daemon" 2>/dev/null || true
chown root:root "$RW_DIR/ambient_daemon" 2>/dev/null || true
set_exec_label "$RW_DIR/ambient_daemon"

# Helpers.  The vendor micom only accepts CAN sends from a short process
# whitelist (AppHomeScreen, TmsRemoteApp, micommanager_userspace, CANService),
# so the vendor dbus-send is copied under the whitelisted name and a wrapper
# keeps it alive for the micom's /proc/<pid>/exe check.  This is only used
# when the driver selects the micom output backend; nothing is sent by
# default.
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
# Micom CAN-send helper (see the Battery package docs/MICOM_WHITELIST.md).
#   micom-send <array>
# --print-reply keeps this process alive while the micom resolves
# /proc/<pid>/exe. A missing reply is normal and still counts as queued.
out=\$("$HELPER_DIR/CANService" --system --type=method_call --print-reply \\
    --reply-timeout=500 --dest=com.lge.car.micom /Can \\
    com.lge.car.micom.can.SetCANMicomRecv "\$1" 2>&1)
rc=\$?
[ \$rc -eq 0 ] && exit 0
case "\$out" in
    *NoReply*|*"Did not receive a reply"*) exit 0 ;;
esac
printf '%s\n' "\$out" >&2
exit \$rc
EOF
chmod 755 "$HELPER_DIR/micom-send" 2>/dev/null || true
chown root:root "$HELPER_DIR/micom-send" 2>/dev/null || true
set_exec_label "$HELPER_DIR/micom-send"

# Session-bus Home/Star monitor (not a micom sender, not whitelisted).
_src=$(command -v dbus-monitor 2>/dev/null || true)
[ -n "$_src" ] && [ -x "$_src" ] || fail "helper not found: dbus-monitor"
rm -f "$HELPER_DIR/dbus-monitor" 2>/dev/null || true
cp -f "$_src" "$HELPER_DIR/dbus-monitor" || fail "helper copy failed: dbus-monitor"
chmod 755 "$HELPER_DIR/dbus-monitor" 2>/dev/null || true
chown root:root "$HELPER_DIR/dbus-monitor" 2>/dev/null || true
set_exec_label "$HELPER_DIR/dbus-monitor"

printf '%s\n' "$SMACK_SELF" > "$RW_DIR/smack_expected" || fail "cannot write smack_expected"
chmod 644 "$RW_DIR/smack_expected" 2>/dev/null || true
chown root:root "$RW_DIR/smack_expected" 2>/dev/null || true
rm -f "$RW_DIR/status.json" 2>/dev/null || true

# systemd unit.
io_copy "$PAYLOAD/systemd/ambient-daemon.service" "$SERVICE" || fail "service copy failed"
chmod 644 "$SERVICE" 2>/dev/null || true
chown root:root "$SERVICE" 2>/dev/null || true

# QML files.
installed=""
for f in $NEW_FILES; do
    src="$PAYLOAD/qml/AppHomeScreen/qml_eco3.0/$f"
    dst="$QML_DIR/$f"
    [ -f "$src" ] || fail "payload file missing: $src"
    mkdir -p "$QML_DIR/$(dirname "$f")" 2>/dev/null || fail "cannot create dir for $f"
    io_copy "$src" "$dst" "$f" || fail "copy failed: $f (see $LOG)"
    chmod 644 "$dst" 2>/dev/null || true
    chown root:root "$dst" 2>/dev/null || true
    installed="$installed $f"
done
log "installed:$installed"

# Inject the tile and the page instance into the live home screen.  A base
# with the SCOURGE allocator gets both; anything else still gets the page, so
# the app is reachable even without a tile slot.
inject_state="skipped"
ambient_skip=0
if grep -q 'AMBIENT_TILE_V2' "$cur" 2>/dev/null; then
    inject_state="already"
else
    # An earlier build injected a tile whose visible binding read
    # menuList.currentIndex and index - names that do not exist in this file.
    # The binding threw, so the tile kept its default visible=true on every page
    # and its position came from a model index nobody re-checked (2026-11-23: the
    # tile showed on the widget page with its right half off the screen edge).
    # That block is reverted from its own backup before the new one goes in; when
    # the revert cannot be proven, the old block is left alone rather than doubled.
    if grep -q 'AMBIENT_PAYLOAD_260128' "$cur" 2>/dev/null; then
        if sh "$PAYLOAD/lib/qml_inject.sh" --file "$cur" --revert \
                --backup-dir "$RW_DIR/qml-backup" >> "$LOG" 2>&1 \
           && ! grep -q 'AMBIENT_PAYLOAD_260128' "$cur" 2>/dev/null; then
            log "inject: reverted the previous ambient block (v1) before reinstalling"
        else
            inject_state="old-block-kept"
            log "inject: an old ambient block is present and the revert failed - leaving the file untouched"
        fi
    fi
    label=$(getfattr -n security.SMACK64 --only-values "$cur" 2>/dev/null || true)
    # Which custom slot is ours: the battery/wifi/tweaks/reboot tiles take 0..3, so
    # the ambient tile takes the next ordinal, read from the live file instead of
    # being hardcoded.  The same number is used by every page instance.
    if [ "$inject_state" = "skipped" ]; then
        _max=$(grep -o 'firstEmptySlot([0-9][0-9]*)' "$cur" 2>/dev/null | tr -dc '0-9\n' | sort -n | tail -n 1)
        case "$_max" in ''|*[!0-9]*) ambient_skip=0 ;; *) ambient_skip=$((_max + 1)) ;; esac
        log "inject: slot=$ambient_skip (ordinals already used by the SCOURGE tiles)"
    fi
    if [ "$inject_state" != "skipped" ]; then
        :
    elif grep -q 'function firstEmptySlot' "$cur" 2>/dev/null; then
        # One instance per grid page, exactly like the stock custom tiles: the
        # allocator decides which page gets the free cell, and only that page's
        # instance is visible.  A single instance anchored at menuPage left the
        # app invisible whenever the free cell landed on a later page.
        ambient_pages=0
        for _p in 1 2 3; do
            _menu=menuPage
            [ "$_p" = 2 ] && _menu=menuPage2
            [ "$_p" = 3 ] && _menu=menuPage3
            grep -q "id: $_menu" "$cur" 2>/dev/null || continue
            sed -e "s/@PAGE0@/$((_p - 1))/g" -e "s/@PAGE@/$_p/g" \
                -e "s/@MENU@/$_menu/g" -e "s/@ORD@/$ambient_skip/g" \
                "$PAYLOAD/qml/AmbientTile.instance.qml" > "$RW_DIR/tile.block.$_p.qml" 2>/dev/null \
                || fail "cannot stage the tile block for $_menu"
            # The injector keeps one backup; only the first call may write it, or
            # --revert would restore an already-injected file.
            if [ "$_p" = 1 ]; then _bdir="$RW_DIR/qml-backup"; else _bdir="$RW_DIR/qml-backup-steps"; fi
            if sh "$PAYLOAD/lib/qml_inject.sh" --file "$cur" --anchor "$_menu" \
                    --block "$RW_DIR/tile.block.$_p.qml" --backup-dir "$_bdir" >> "$LOG" 2>&1; then
                ambient_pages=$((ambient_pages + 1))
            else
                log "inject: tile block for $_menu failed"
            fi
        done
        if [ "$ambient_pages" -gt 0 ]; then
            if sh "$PAYLOAD/lib/qml_inject.sh" --file "$cur" --append \
                    --block "$PAYLOAD/qml/AmbientPage.instance.qml" \
                    --backup-dir "$RW_DIR/qml-backup-steps" >> "$LOG" 2>&1; then
                inject_state="tile+page (pages=$ambient_pages slot=$ambient_skip)"
            else
                inject_state="tile-only (pages=$ambient_pages slot=$ambient_skip)"
            fi
        else
            inject_state="failed"
        fi
    else
        if sh "$PAYLOAD/lib/qml_inject.sh" --file "$cur" --append \
                --block "$PAYLOAD/qml/AmbientPage.instance.qml" \
                --backup-dir "$RW_DIR/qml-backup" >> "$LOG" 2>&1; then
            inject_state="page-only (no tile allocator)"
        else
            inject_state="failed"
        fi
    fi
    # Last resort: the page still gets injected, so the app is reachable even when
    # no tile slot could be allocated (a full grid, or a base without the
    # allocator).  Nothing here may leave the live home screen unpatched silently.
    if [ "$inject_state" = "failed" ]; then
        if sh "$PAYLOAD/lib/qml_inject.sh" --file "$cur" --append \
            --block "$PAYLOAD/qml/AmbientPage.instance.qml" \
            --backup-dir "$RW_DIR/qml-backup" >> "$LOG" 2>&1; then
            inject_state="page-only (no free tile slot)"
        fi
    fi
    [ -z "$label" ] || setfattr -n security.SMACK64 -v "$label" "$cur" 2>/dev/null || true
fi
log "inject=$inject_state"
# Layout evidence.  Where the tile resolves is decided by the live file's own
# grid constants, and those differ per SKU: the 23:52 run left a tile hanging
# off the edge and nothing on the stick could say which constant did it.  Copy
# the constants and the injected block next to the run outputs, so the next
# placement is computed from numbers instead of guessed from a photo.
layout_report() {
    _dir=""
    for _c in "${SCOURGE_STICK_DIR:-}" "${USB_DIR:-}" "$RW_DIR"; do
        [ -n "$_c" ] || continue
        [ -d "$_c" ] || continue
        [ -w "$_c" ] || continue
        _dir="$_c"; break
    done
    _o="${_dir:-/tmp}/ambient_layout.txt"
    {
        echo "=== ambient layout $(date -u 2>/dev/null)"
        echo "file=$cur"
        echo "inject=$inject_state slot=$ambient_skip"
        echo "--- grid constants"
        grep -nE "property int k_itemWidth|property int k_itemHeight|property int k_nbrOfColumns|property int k_nbrOfRows|property int k_menuPageWidth|property bool isWideScreen|SCREEN_WIDTH|DEVICE_WIDTH" "$cur" 2>/dev/null | head -n 20
        echo "--- custom tile ordinals"
        grep -o "firstEmptySlot([0-9][0-9]*)" "$cur" 2>/dev/null | sort -u
        echo "--- ambient tile block"
        sed -n "/AMBIENT_TILE_V2/,/onClicked: ambientPage.open()/p" "$cur" 2>/dev/null | head -n 40
        echo "--- app stdout candidates"
        echo "--- injected page ids (open() targets)"
        grep -nE "id: (ambientPage|fakeEnginePage|batteryOverlay|wifiPage|tweaksPage|rebootPage)" "$cur" 2>/dev/null
        ls -l /rw_data/apphome* 2>/dev/null | head -n 6
    } > "$_o" 2>&1
    sync 2>/dev/null || true
    log "layout report: $_o"
}
# Why a daemon that "started" is not running.  On 2026-11-24 the installer
# reported daemon=systemd-ok while no ambient_daemon process existed and nothing
# on the stick said why, so the unit's own view is collected next to the run
# outputs.
# ---- factory ambient-lighting discovery ------------------------------------
# The factory ambient controller is not documented for this trim.  Interior
# ambient on HMG is normally a body-bus/LIN function bridged by the micom, so the
# only honest way to find it is to listen to what the micom already relays: the
# body-CAN and M-CAN broadcasts.  Toggle any light (dome/map/cluster theme) while
# this window is open and the diff between runs is the candidate frame.
hw_probe() {
    _hdir=""
    for _c in "${SCOURGE_STICK_DIR:-}" "${USB_DIR:-}" "$RW_DIR"; do
        [ -n "$_c" ] || continue
        [ -d "$_c" ] && [ -w "$_c" ] || continue
        _hdir="$_c"; break
    done
    _ho="${_hdir:-/tmp}/ambient_hw.txt"
    {
        echo "=== ambient hw probe $(date -u 2>/dev/null)"
        echo "--- settings keys (ambient/light/theme)"
        grep -o -iE '"[A-Za-z_]*(ambient|mood|light|illum|theme)[A-Za-z_]*"[[:space:]]*:[[:space:]]*[^,}]*' /app/share/AppSetup/data/vehicle_settings.json 2>/dev/null | head -n 20
        # A light COMMAND would be a method call, not a signal: the 2026-11-25 capture
        # (15 s of toggling) showed only ihu speed/ETC telemetry and radio station
        # updates, and no light-ish signal at all.  So introspect the service first -
        # what can we even send - and then watch method calls as well as signals.
        echo "--- micom method surface (Introspect)"
        if command -v dbus-send >/dev/null 2>&1; then
            _intro="$RW_DIR/.micom_introspect.txt"
            : > "$_intro" 2>/dev/null || _intro=/tmp/micom_introspect.txt
            for _p in / /Ihu /Radio /Light /Ambient /Interior /Theme /Setting; do
                dbus-send --system --print-reply --dest=com.lge.car.micom "$_p" \
                    org.freedesktop.DBus.Introspectable.Introspect 2>/dev/null \
                    | sed "s|^|[$_p] |" >> "$_intro" 2>/dev/null || true
            done
            echo "  paths that answered: $(grep -c 'interface name' "$_intro" 2>/dev/null)"
            echo "  interfaces: $(grep -oE 'interface name="[^"]+"' "$_intro" 2>/dev/null | sort -u | sed 's/interface name=//' | tr '\n' ' ')"
            echo "  methods (name only): $(grep -oE '<method name="[^"]+"' "$_intro" 2>/dev/null | sed 's/<method name=//' | sort -u | tr '\n' ' ' | cut -c1-1200)"
            echo "  light-ish methods:"
            grep -oE '<method name="[^"]*"' "$_intro" 2>/dev/null \
                | grep -iE 'light|ambient|illum|theme|mood|dim|led|color' | sed 's/^/    /' | head -n 20
            echo "  child nodes: $(grep -oE '<node name="[^"]+"' "$_intro" 2>/dev/null | sed 's/<node name=//' | sort -u | tr '\n' ' ')"
        else
            echo "no dbus-send"
        fi
        echo "--- phase 1: idle baseline, signals + method calls (8 s)"
        if command -v dbus-monitor >/dev/null 2>&1; then
            ( dbus-monitor --system "type='signal',sender='com.lge.car.micom'" 2>&1 \
                  & _dp=$!; sleep 8; kill -TERM "$_dp" 2>/dev/null; wait "$_dp" 2>/dev/null ) | head -n 80
            echo "--- phase 2: TOGGLE A LIGHT NOW (dome/map/cluster theme), 15 s"
            # Monitors write to files, not into a pipeline: killing a pipeline pid
            # leaves the monitor alive and it keeps holding the medium.
            dbus-monitor --system "type='signal',sender='com.lge.car.micom'" > /tmp/amb_sig.txt 2>&1 &
            _mp1=$!
            dbus-monitor --system "type='method_call'" > /tmp/amb_call.txt 2>&1 &
            _mp2=$!
            dbus-monitor --session "type='method_call'" > /tmp/amb_sess.txt 2>&1 &
            _mp3=$!
            sleep 15
            kill -TERM "$_mp1" "$_mp2" "$_mp3" 2>/dev/null || true
            wait 2>/dev/null || true
            echo "  captured: signals=$(grep -c 'member=' /tmp/amb_sig.txt 2>/dev/null) system_calls=$(grep -c 'member=' /tmp/amb_call.txt 2>/dev/null) session_calls=$(grep -c 'member=' /tmp/amb_sess.txt 2>/dev/null)"
            echo "  --- phase 2 micom signals"
            head -n 80 /tmp/amb_sig.txt 2>/dev/null
            echo "  --- phase 2 system method calls mentioning light/Ambient/micom/Scomo/mcan"
            grep -iE 'light|ambient|illum|theme|mood|micom|Scomo|mcan' /tmp/amb_call.txt 2>/dev/null | head -n 60
            echo "  --- phase 2 session method calls mentioning light/ambient/theme"
            grep -iE 'light|ambient|illum|theme|mood' /tmp/amb_sess.txt 2>/dev/null | head -n 40
            echo "  --- every distinct system method member seen (top 20 by count)"
            grep -oE 'member=[A-Za-z0-9_]+' /tmp/amb_call.txt 2>/dev/null | sort | uniq -c | sort -rn | head -n 20
            rm -f /tmp/amb_sig.txt /tmp/amb_call.txt /tmp/amb_sess.txt 2>/dev/null || true
        else
            echo "no dbus-monitor"
        fi
    } > "$_ho" 2>&1
    sync 2>/dev/null || true
    log "hw probe: $_ho"
}
hw_probe

daemon_diag() {
    _ddir=""
    for _c in "${SCOURGE_STICK_DIR:-}" "${USB_DIR:-}" "$RW_DIR"; do
        [ -n "$_c" ] || continue
        [ -d "$_c" ] || continue
        [ -w "$_c" ] || continue
        _ddir="$_c"; break
    done
    _do="${_ddir:-/tmp}/ambient_daemon.txt"
    {
        echo "=== ambient daemon diag $(date -u 2>/dev/null)"
        echo "is_active=$(systemctl is-active ambient-daemon 2>&1)"
        echo "is_enabled=$(systemctl is-enabled ambient-daemon 2>&1)"
        echo "--- systemctl status"
        systemctl status ambient-daemon --no-pager 2>&1 | head -n 24
        echo "--- process"
        pidof ambient_daemon 2>/dev/null || echo "no ambient_daemon process"
        echo "--- unit"
        grep -E '^(ExecStart|User|Restart|Type)=' /etc/systemd/system/ambient-daemon.service 2>/dev/null
        echo "--- daemon log (tail)"
        tail -n 30 "$RW_DIR/daemon.log" 2>/dev/null
        echo "--- status.json"
        head -c 600 "$RW_DIR/status.json" 2>/dev/null
        echo
        echo "--- port 8094"
        (ss -ltn 2>/dev/null || netstat -ltn 2>/dev/null) | grep 8094 || echo "no 8094 listener"
    } > "$_do" 2>&1
    sync 2>/dev/null || true
    log "daemon diag: $_do"
}
daemon_diag
layout_report
if [ "$inject_state" = "failed" ]; then
    _tail=$(tail -n 4 "$LOG" 2>/dev/null | tr "\n" " " | cut -c1-300)
    result "status=FAIL" "error=QML injection failed" "inject_tail=$_tail"
    exit 1
fi

# Clear QML caches.
rm -rf /root/.cache/qtshadercache 2>/dev/null || true
rm -rf /root/.cache/AppHomeScreen 2>/dev/null || true
rm -rf /tmp/qmlcache 2>/dev/null || true
rm -rf /tmp/qt-qml-cache 2>/dev/null || true

sync

# Start the daemon now and enable it for future boots.  This must happen while
# / is still rw because `systemctl enable` writes a wants symlink.
daemon_status="not-started"
if command -v systemctl >/dev/null 2>&1; then
    # systemd keeps the loaded unit, and a changed file only produces
    # "Warning: ambient-daemon.service changed on disk" while the OLD ExecStart
    # keeps failing (2026-11-24: the --status-file fix went to disk, the service
    # kept running the old command line).  Reload, and prove the loaded unit is
    # the one on disk before starting.
    systemctl daemon-reload >> "$LOG" 2>&1 || log "WARNING: systemctl daemon-reload failed"
    _loaded=$(systemctl show -p FragmentPath --value ambient-daemon.service 2>/dev/null)
    log "unit loaded from ${_loaded:-unknown} (file $SERVICE)"
    if systemctl enable ambient-daemon.service >> "$LOG" 2>&1; then
        if systemctl restart ambient-daemon.service >> "$LOG" 2>&1; then
            daemon_status="systemd-ok"
        else
            daemon_status="systemd-restart-failed"
        fi
    else
        daemon_status="systemd-enable-failed"
    fi
fi
start_daemon_manual() {
    ( "$RW_DIR/ambient_daemon" --port 8094 --state-dir "$RW_DIR" \
        --status-file "$RW_DIR/status.json" \
        --micom-send "$RW_DIR/bin/micom-send" \
        --dbus-monitor "$RW_DIR/bin/dbus-monitor" \
        >> "$RW_DIR/daemon.log" 2>&1 & ) 2>/dev/null || true
    sleep 1
}
if [ "$daemon_status" != "systemd-ok" ]; then
    if [ -x "$RW_DIR/ambient_daemon" ] && ! pidof ambient_daemon >/dev/null 2>&1; then
        start_daemon_manual
        pidof ambient_daemon >/dev/null 2>&1 && daemon_status="$daemon_status+manual"
    fi
fi

# Freshness gate: a failed restart can leave the OLD daemon running.
payload_daemon_md5=$(md5sum "$PAYLOAD/bin/ambient_daemon" 2>/dev/null | cut -d' ' -f1)
daemon_check_fresh() {
    _found=no-daemon
    for p in /proc/[0-9]*; do
        [ -r "$p/comm" ] || continue
        if [ "$(cat "$p/comm" 2>/dev/null)" = "ambient_daemon" ]; then
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
daemon_fresh=unknown
if [ -n "$payload_daemon_md5" ] && command -v md5sum >/dev/null 2>&1; then
    daemon_fresh=$(daemon_check_fresh)
    if [ "$daemon_fresh" = "stale" ]; then
        log "running daemon is stale; forcing restart"
        for p in /proc/[0-9]*; do
            [ -r "$p/comm" ] || continue
            if [ "$(cat "$p/comm" 2>/dev/null)" = "ambient_daemon" ]; then
                kill -TERM "${p#/proc/}" 2>/dev/null || true
            fi
        done
        sleep 1
        command -v systemctl >/dev/null 2>&1 && systemctl restart ambient-daemon.service >> "$LOG" 2>&1 || true
        pidof ambient_daemon >/dev/null 2>&1 || start_daemon_manual
        daemon_fresh=$(daemon_check_fresh)
    fi
fi
log "daemon=$daemon_status fresh=$daemon_fresh"

daemon_smack="${AMBIENT_DAEMON_SMACK:-}"
if [ -z "$daemon_smack" ]; then
    for p in /proc/[0-9]*; do
        [ -r "$p/comm" ] || continue
        if [ "$(cat "$p/comm" 2>/dev/null)" = "ambient_daemon" ]; then
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
result "status=$final_status" "installed=$installed" "inject=$inject_state" \
       "backup=$BACKUP_HU" "daemon=$daemon_status" "daemon_fresh=$daemon_fresh" \
       "daemon_smack=${daemon_smack:-none}" "smack_check=$smack_check" \
       "restart=$restart" "tile_slot=$ambient_skip" "asset_warnings=$ASSET_WARNINGS" "log=$LOG"
log "=== ambient payload done ==="
[ "$final_status" = "OK" ] || exit 1
exit 0
