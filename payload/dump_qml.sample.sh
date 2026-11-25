#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# run_stuff.sh - SCOURGE head-unit dump: copy the unit's own QML tree and a
# system inventory back to the stick, with a beep at the start and a longer one
# at the end so it is audible from outside the car.
#
# Put this file at the ROOT of a FAT/exFAT stick and plug it in: the initial-root
# failsafe runs it as root within ~8 s (md5-gated, so editing it re-runs it).
# Read-only apart from its own dump/ directory: nothing on the unit is modified.
set -u
USB_DIR="${USB_DIR:-/media/usb}"
QML_BASE="/app/share/AppHomeScreen/qml_eco3.0"
# SCOURGE_BEEP=0 silences every tone: the console-bell fallback is loud on a host
# that routes it to the speakers, and a host run has nothing to listen for.
BEEP_ENABLED="${SCOURGE_BEEP:-1}"
DUMP="$USB_DIR/dump"
LOG="$DUMP/dump.txt"

mkdir -p "$DUMP" 2>/dev/null
mount -o remount,rw "$USB_DIR" 2>/dev/null || true

# --- beep ---------------------------------------------------------------------
# The unit is not guaranteed to have an audio player, so try them in order and
# fall back to a BEL on the console. The first method that works is logged.
BEEP_METHOD="none"
BEEP_LOG=""

play_once() {   # play_once <raw pcm> -> 0 on success, keeps the attempts
    # The tones are RAW PCM (.dat), not .wav: the head unit's media player scans
    # the stick and loops any audio file it recognises at the root. An unknown
    # extension keeps it out, and aplay is told the format explicitly.
    wav="$1"
    for cmd in \
        "aplay -q -f S16_LE -r 44100 -c 1 $wav" \
        "aplay -q -D default -f S16_LE -r 44100 -c 1 $wav" \
        "aplay -q -D plughw:0,0 -f S16_LE -r 44100 -c 1 $wav" \
        "pacat --format=s16le --rate=44100 --channels=1 $wav" \
        "gst-launch-1.0 -q filesrc location=$wav ! rawaudioparse ! alsasink"
    do
        command -v "${cmd%% *}" >/dev/null 2>&1 || continue
        if $cmd >/dev/null 2>&1; then
            BEEP_METHOD="${cmd%% *}"
            BEEP_LOG="${BEEP_LOG}${cmd} rc=0; "
            return 0
        fi
        BEEP_LOG="${BEEP_LOG}${cmd} rc=1; "
    done
    return 1
}

beep() {   # beep <wav> <count> [gap]
    wav="$1"; count="${2:-1}"; gap="${3:-0.3}"
    [ "$BEEP_ENABLED" = 0 ] && { BEEP_METHOD="disabled"; return 0; }
    i=0
    while [ "$i" -lt "$count" ]; do
        if ! play_once "$wav"; then
            # Nothing on the box can play it: shout at every console we can open.
            BEEP_METHOD="bel"
            for dev in /dev/console /dev/tty1 /dev/tty2; do
                printf '\a' > "$dev" 2>/dev/null || true
            done
            sleep 0.4
        fi
        i=$((i + 1))
        [ "$i" -lt "$count" ] && sleep "$gap"
    done
}

beep "$USB_DIR/beep-start.dat" 1

# --- what this is -------------------------------------------------------------
{
    echo "SCOURGE_DUMP"
    echo "time=$(date -u 2>/dev/null)"
    echo "usb=$USB_DIR"
    echo "uid=$(id -u 2>/dev/null)"
    echo "uname=$(uname -a 2>/dev/null)"
    echo "qml_base=$QML_BASE"
} > "$LOG" 2>&1

# --- the QML tree, whole ------------------------------------------------------
mkdir -p "$DUMP/qml"
if [ -d "$QML_BASE" ]; then
    ( cd "$QML_BASE" && tar -cf - . 2>/dev/null ) | ( cd "$DUMP/qml" && tar -xf - 2>/dev/null )
    echo "qml_files=$(find "$DUMP/qml" -type f 2>/dev/null | wc -l)" >> "$LOG"
else
    echo "absent $QML_BASE" >> "$LOG"
fi

# --- inventory: what else is on the unit (names + sizes, no bulk copies) ------
mkdir -p "$DUMP/inventory"
for dir in /app/bin /app/lib /app/share /etc/systemd/system /etc/init.d /rw_data /usr/lib; do
    [ -d "$dir" ] || continue
    out="$DUMP/inventory${dir//\//_}.txt"
    find "$dir" -maxdepth 3 -type f -printf '%s %p\n' 2>/dev/null | sort -k2 > "$out"
done
# -size -1M would match only empty files (find rounds up), so the cutoff is in
# bytes. Bounded: /app carries ~39k small files (~500 MB), which is 1-2 minutes of
# hashing on the unit's eMMC - the inventory is worth it, an unbounded run is not.
find /app -type f -size -1048576c -printf '%p\n' 2>/dev/null | head -n 60000 \
    | while read -r f; do
        md5sum "$f" 2>/dev/null
    done > "$DUMP/inventory/app-small.md5" 2>/dev/null

# --- system state -------------------------------------------------------------
mkdir -p "$DUMP/system"
{
    echo "--- ip"; ip addr 2>/dev/null
    echo "--- route"; ip route 2>/dev/null
    echo "--- cmdline"; cat /proc/cmdline 2>/dev/null
    echo "--- mounts"; mount 2>/dev/null | head -40
    echo "--- df"; df -h 2>/dev/null
    echo "--- services"; ls /etc/systemd/system/*.service 2>/dev/null
    echo "--- persistence"; ls -l /usr/lib/libpersistence_client_library.so* 2>/dev/null
    echo "--- wpa"; wpa_cli -p /var/run/wpa_supplicant -i wlan0 status 2>/dev/null
} > "$DUMP/system/state.txt" 2>&1

# --- payload markers: the variant name the installers derive ------------------
ADV="$DUMP/qml/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
if [ -f "$ADV" ]; then
    {
        echo "--- markers"
        v=""
        for m in HELLO_PAYLOAD_260128 WIFI_PAYLOAD_260128 BATTERY_PAYLOAD_260128 \
                 BATTERY_TILE_PAYLOAD_260128 TWEAKS_PAYLOAD_260128 STORE_PAYLOAD_260128; do
            if grep -q "$m" "$ADV" 2>/dev/null; then
                echo "$m"
                [ "$m" = "STORE_PAYLOAD_260128" ] && continue
                v="$v$(echo "$m" | cut -d_ -f1 | tr 'A-Z' 'a-z')_"
            fi
        done
        echo "--- variant"
        echo "${v%_}"
    } >> "$LOG" 2>&1
fi

# --- state backup: what a package restore would need to put back ------------
# Not a partition image (the OEM rootfs stays untouchable and 1-2 GB away), but
# the unit's own writable state: daemon configs and logs, the installers' own
# backups, the shipped home-screen tree and the service units. A few tens of MB.
mkdir -p "$DUMP/state"
{
    echo "--- rw_data"
    find /rw_data -type f -size -16M -printf '%p\n' 2>/dev/null | head -n 20000 \
        | tar -czf "$DUMP/state/rw_data.tgz" -T - 2>/dev/null
    echo "rw_data_tgz=$(stat -c%s "$DUMP/state/rw_data.tgz" 2>/dev/null || echo 0)"
    echo "--- systemd units"
    tar -czf "$DUMP/state/systemd.tgz" -C / etc/systemd/system 2>/dev/null
    echo "systemd_tgz=$(stat -c%s "$DUMP/state/systemd.tgz" 2>/dev/null || echo 0)"
    echo "--- app tree outside the QML dump (names only)"
    find /app -maxdepth 2 -type d -printf '%p\n' 2>/dev/null
} >> "$LOG" 2>&1

( cd "$DUMP" && find . -type f 2>/dev/null | sort | xargs -r md5sum > manifest.md5 2>/dev/null )
# The start beep already told us whether audio works at all; the end one runs
# after the sync so a slow flush cannot swallow it, and its outcome is logged
# (attempted commands + rc) so a silent run is diagnosable from the stick.
echo "beep_method=${BEEP_METHOD:-none}" >> "$LOG"
sleep 0.3
beep "$USB_DIR/beep-done.dat" 3 0.35
echo "beep_end_method=${BEEP_METHOD:-none}" >> "$LOG"
echo "beep_attempts=${BEEP_LOG}" >> "$LOG"
cp -f "$LOG" "$USB_DIR/dump_result.txt" 2>/dev/null
sync
exit 0
