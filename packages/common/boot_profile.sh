#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# boot_profile.sh — one boot-timing snapshot, appended to a TSV that survives
# reboots so a slow boot can be compared with a fast one later.
#
#   sh boot_profile.sh [outdir]        (default /rw_data/boot_profile)
#
# Records: uptime + boot id; every interesting process' start offset computed
# from /proc/<pid>/stat field 22 (starttime / CLK_TCK) — this works without
# systemd-analyze, which the unit may not ship; kernel milestones from dmesg;
# systemd's own view when it exists; our units' ActiveEnterTimestamp.
# Machine-readable line: <outdir>/boot.csv (one per boot).
# Full report: <outdir>/<boot_id>.txt
set -u
OUT=/rw_data/boot_profile
FULL=0
for _a in "$@"; do
    case "$_a" in
        --full) FULL=1 ;;
        -*) ;;
        *) [ -n "$_a" ] && OUT=$_a ;;
    esac
done
mkdir -p "$OUT" 2>/dev/null || { echo "cannot create $OUT" >&2; exit 1; }
CSV="$OUT/boot.csv"
HZ=$(getconf CLK_TCK 2>/dev/null || echo 100)
[ "$HZ" -gt 0 ] 2>/dev/null || HZ=100
UP=$(cut -d. -f1 /proc/uptime 2>/dev/null || echo 0)
BOOT=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null || echo unknown)
# Wall-clock second of the kernel start: a log line's own epoch minus this is its
# boot-relative offset, which is how the disclaimer path below is placed on the
# same timeline as the process start offsets.
BOOT_WALL=$(( $(date +%s) - UP ))
wall_off_ms() { awk -v b="$BOOT_WALL" -v w="$1" 'BEGIN { printf "%d", w / 1000 - b }'; }

# start offset (seconds after boot) of the first process with this name
proc_off() {
    for _d in /proc/[0-9]*; do
        [ -r "$_d/comm" ] || continue
        [ "$(cat "$_d/comm" 2>/dev/null)" = "$1" ] || continue
        _st=$(awk '{print $22}' "$_d/stat" 2>/dev/null)
        [ -n "$_st" ] || continue
        echo $((_st / HZ))
        return 0
    done
    echo ""
}

# kernel milestone second from dmesg for a pattern
dmesg_s() {
    dmesg 2>/dev/null | grep -m1 -E "$1" | sed -n 's/^\[ *\([0-9.]*\)\].*/\1/p' | cut -d. -f1
}

REPORT="$OUT/$BOOT.txt"
{
    echo "=== boot profile $BOOT"
    echo "uptime_s=$UP  measured_at=$(date -u 2>/dev/null)"
    echo
    echo "--- process start offsets (seconds after boot)"
    for name in weston AppHomeScreen AppLauncher connmand vbtd BluetoothHMI wifi_daemon battery_daemon tweaks_daemon ambient_daemon fake_engine_daemon dbus-daemon systemd-udevd; do
        echo "$(proc_off "$name") $name"
    done | sort -n | awk 'NF == 2 { printf "  %6ss  %s\n", $1, $2 }'
    echo
    echo "--- kernel milestones (dmesg)"
    for pat in "Freeing unused kernel memory" "Register interface \[wlan0\]" "dhd_bus_download_firmware" "Reached target Multi-User" "Startup finished"; do
        _v=$(dmesg_s "$pat")
        [ -n "$_v" ] && printf "  %6ss  %s\n" "$_v" "$pat"
    done
    echo
    echo "--- our units"
    for u in wifi-daemon battery-daemon tweaks-daemon ambient-daemon fake-engine; do
        printf "  %-16s active=%-8s since=%s\n" "$u" "$(systemctl is-active "$u.service" 2>/dev/null)" "$(systemctl show -p ActiveEnterTimestamp --value "$u.service" 2>/dev/null)"
    done
    echo
    echo "--- disclaimer path (the gate the driver waits for)"
    _sb=$(proc_off AppStandbyClock)
    printf "  %6ss  AppStandbyClock started\n" "${_sb:--}"
    _sb2=$(proc_off AcceptScreen)
    [ -n "$_sb2" ] && printf "  %6ss  AcceptScreen started\n" "$_sb2"
    for _f in disclaimer_auto disclaimer_timeout_ms; do
        _p="/rw_data/tweaks/$_f"
        if [ -f "$_p" ]; then
            _v=$(tr -d '[:space:]' < "$_p" 2>/dev/null)
            _m=$(stat -c %Y "$_p" 2>/dev/null || echo 0)
            _off=$((_m - BOOT_WALL))
            if [ "$_off" -le 0 ]; then
                _when="pre-boot"
            else
                _when="post-boot+$(printf '%ss' "$_off")"
            fi
            printf "  %s=%s  (written %s)\n" "$_f" "${_v:-empty}" "$_when"
        else
            printf "  %s missing - the QML patch falls back to manual accept\n" "$_f"
        fi
    done
    # The patch logs its own wall clock (see TWEAKS_DISCLAIMER_260128), so the
    # accept can be placed on the boot timeline without trusting journal formats.
    if command -v journalctl >/dev/null 2>&1; then
        journalctl -b --no-pager 2>/dev/null | grep -F "[tweaks] disclaimer" | tail -n 10 |
        while IFS= read -r _line; do
            _ms=$(printf '%s\n' "$_line" | sed -n 's/.*at=\([0-9][0-9]*\).*/\1/p')
            if [ -n "$_ms" ]; then
                printf "  %6ss  %s\n" "$(wall_off_ms "$_ms")" "$(printf '%s' "$_line" | sed 's/.*\[tweaks\] //')"
            else
                printf "  %6s   %s\n" "-" "$(printf '%s' "$_line" | sed 's/.*\[tweaks\] //')"
            fi
        done
    else
        echo "  (no journalctl on this unit)"
    fi
    echo
    echo "--- systemd-analyze (if present)"
    if command -v systemd-analyze >/dev/null 2>&1; then
        systemd-analyze 2>&1 | head -n 4
        echo "  blame (top 10):"; systemd-analyze blame 2>&1 | head -n 10 | sed 's/^/    /'
        echo "  critical-chain:"; systemd-analyze critical-chain 2>&1 | head -n 8 | sed 's/^/    /'
    else
        echo "  (no systemd-analyze on this unit)"
    fi
} > "$REPORT" 2>&1

LINE="boot=$BOOT uptime=$UP"
add() { _v=$2; [ -n "$_v" ] && LINE="$LINE $1=$_v"; return 0; }
add wlan0_reg "$(dmesg_s 'Register interface \[wlan0\]')"
add firmware "$(dmesg_s 'dhd_bus_download_firmware')"
add multi_user "$(dmesg_s 'Reached target Multi-User')"
add weston "$(proc_off weston)"
add home "$(proc_off AppHomeScreen)"
add launcher "$(proc_off AppLauncher)"
add connman "$(proc_off connmand)"
add vbtd "$(proc_off vbtd)"
add bthmi "$(proc_off BluetoothHMI)"
add wifi_daemon "$(proc_off wifi_daemon)"
add battery_daemon "$(proc_off battery_daemon)"
add tweaks_daemon "$(proc_off tweaks_daemon)"
add ambient_daemon "$(proc_off ambient_daemon)"
add fake_engine_daemon "$(proc_off fake_engine_daemon)"
add standby "$(proc_off AppStandbyClock)"
add disclaimer_auto "$(tr -d '[:space:]' < /rw_data/tweaks/disclaimer_auto 2>/dev/null)"
add disclaimer_ms "$(tr -d '[:space:]' < /rw_data/tweaks/disclaimer_timeout_ms 2>/dev/null)"
if [ -f /rw_data/tweaks/disclaimer_auto ]; then
    _dam=$(stat -c %Y /rw_data/tweaks/disclaimer_auto 2>/dev/null || echo 0)
    if [ $((_dam - BOOT_WALL)) -le 0 ]; then add disclaimer_auto_boot pre
    else add disclaimer_auto_boot post; fi
fi
# The accept itself, from the patch's own wall clock (see TWEAKS_DISCLAIMER_260128).
if command -v journalctl >/dev/null 2>&1; then
    _acc=$(journalctl -b --no-pager 2>/dev/null | grep -F "[tweaks] disclaimer: confirm sent at=" |
           tail -n 1 | sed -n 's/.*at=\([0-9][0-9]*\).*/\1/p')
    [ -n "$_acc" ] && add disclaimer_accept "$(wall_off_ms "$_acc")"
fi
add measured_at "$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null)"
if [ -f "$CSV" ]; then
    grep -v "^boot=$BOOT " "$CSV" > "$CSV.tmp" 2>/dev/null || : > "$CSV.tmp"
    mv -f "$CSV.tmp" "$CSV" 2>/dev/null || true
fi
printf '%s\n' "$LINE" >> "$CSV"

# --full: the whole boot, not just the offsets.  Bounded so a long-lived unit
# cannot fill the partition; the artifacts land next to the report and the combo
# copies them to the stick.
if [ "$FULL" = 1 ]; then
    FULLDIR="$OUT/full_$BOOT"
    mkdir -p "$FULLDIR" 2>/dev/null || FULLDIR="$OUT"
    if command -v dmesg >/dev/null 2>&1; then
        dmesg 2>/dev/null | tail -n 4000 > "$FULLDIR/dmesg.txt" 2>/dev/null || true
    fi
    if command -v journalctl >/dev/null 2>&1; then
        journalctl -b --no-pager -o short-monotonic 2>/dev/null | tail -n 4000 > "$FULLDIR/journal.txt" 2>/dev/null || true
    fi
    if command -v systemctl >/dev/null 2>&1; then
        systemctl list-units --type=service --all --no-pager --no-legend 2>/dev/null |
            awk '{print $1, $4}' | sort > "$FULLDIR/units.txt" 2>/dev/null || true
        systemctl show --property=Id,ActiveEnterTimestamp,ExecMainStartTimestamp --no-pager 2>/dev/null |
            grep -E "^(Id|ActiveEnterTimestamp|ExecMainStartTimestamp)=" |
            paste - - - 2>/dev/null | sort -t= -k3 > "$FULLDIR/unit_times.txt" 2>/dev/null || true
    fi
    echo "boot_profile: full artifacts in $FULLDIR"
fi
echo "boot_profile: $REPORT"
echo "boot_profile: $CSV ($(wc -l < "$CSV" 2>/dev/null || echo 1) boot(s) recorded)"
