#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# run_stuff.sh - SCOURGE head-unit dump: the Hello-app base matrix.
#
# Why a dump and not a synthesis: packages/wifi/base/hello* decides whether the
# WiFi builder emits a hello-family variant.  A synthesised base produces a
# variant that references a tile component the unit's OEM app set may not
# define, and the installer would carry that variant to the car (2026-11-24).
# Only a unit that really ships the Hello tile can supply the base, so this
# payload copies exactly the files the builder reads, plus the evidence that
# they are what they claim to be.
#
# Put this file at the ROOT of a FAT/exFAT stick and plug it in: the initial-root
# failsafe runs it as root within ~8 s (md5-gated, so every sync must change it).
# Read-only on the unit: everything is written under $USB_DIR/hello_dump/<stamp>.
#
#   sh run_stuff.sh                                   # on the unit
#   APP_ROOT=/tmp/unit USB_DIR=/tmp/stick sh run_stuff.sh    # host fixture/tests
#
# Audible verdict: three long beeps = complete base found, two short ones = no
# Hello tile on this unit (nothing to import).  Beeps are strictly non-fatal and
# SCOURGE_BEEP=0 silences them completely - on a host that routes the console
# bell to the speakers the fallback is far louder than on the unit.
set -u

APP_ROOT="${APP_ROOT:-/app}"
MEDIA_ROOT="${MEDIA_ROOT:-/media}"
USB_DIR="${USB_DIR:-/media/usb}"

# Over SSH the tones are pointless - nobody is standing at the stick - and they
# come out of the head unit's speakers while the car may be moving.  So the
# default is "beep only when the failsafe ran me"; SCOURGE_BEEP=1/0 overrides.
if [ -n "${SCOURGE_BEEP:-}" ]; then
    BEEP_ENABLED="$SCOURGE_BEEP"
elif [ -n "${SSH_CONNECTION:-}${SSH_CLIENT:-}" ]; then
    BEEP_ENABLED=0
else
    BEEP_ENABLED=1
fi

# The loader mounts the stick where it likes (/media/usb, /media/sda1, ...).  An
# interactive run has to find it: look for the stick by its own files instead of
# guessing the mount point.
if [ ! -d "$USB_DIR" ] && [ -d "$MEDIA_ROOT" ]; then
    for cand in "$MEDIA_ROOT"/usb "$MEDIA_ROOT"/*; do
        [ -d "$cand" ] || continue
        if [ -f "$cand/run_stuff.sh" ] || [ -f "$cand/beep-start.dat" ]; then
            USB_DIR="$cand"
            break
        fi
    done
fi
OUT="$USB_DIR/hello_dump"
mkdir -p "$OUT" 2>/dev/null || true
mount -o remount,rw "$USB_DIR" 2>/dev/null || true

STAMP="$(LC_ALL=C date -u +%Y%m%dT%H%M%SZ 2>/dev/null || echo unknown)"
[ -n "$STAMP" ] || STAMP=unknown
RUN="$OUT/$STAMP"
mkdir -p "$RUN" 2>/dev/null || true
REPORT="$RUN/report.txt"
: > "$REPORT" 2>/dev/null || true

# --- beep ---------------------------------------------------------------------
# The tones are RAW PCM (.dat), not .wav: the head unit's media player scans the
# stick and loops any audio file it recognises at the root.
BEEP_METHOD="none"
BEEP_LOG=""
play_once() {
    wav="$1"
    for cmd in \
        "aplay -q -f S16_LE -r 44100 -c 1 $wav" \
        "aplay -q -D plughw:0,0 -f S16_LE -r 44100 -c 1 $wav" \
        "pacat --format=s16le --rate=44100 --channels=1 $wav"
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

shout() {
    [ "$BEEP_ENABLED" = 0 ] && return 0
    BEEP_METHOD="bel"
    for dev in /dev/console /dev/tty1 /dev/tty2; do
        printf '\a' > "$dev" 2>/dev/null || true
    done
    sleep 0.4
}

beep() {
    wav="$1"; count="${2:-1}"; gap="${3:-0.35}"
    if [ "$BEEP_ENABLED" = 0 ]; then
        BEEP_METHOD="disabled"
        return 0
    fi
    i=0
    while [ "$i" -lt "$count" ]; do
        if [ -f "$wav" ]; then
            play_once "$wav" || shout
        else
            shout
        fi
        i=$((i + 1))
        [ "$i" -lt "$count" ] && sleep "$gap"
    done
}

beep "$USB_DIR/beep-start.dat" 1

# --- what this is -------------------------------------------------------------
{
    echo "SCOURGE_HELLO_DUMP"
    echo "stamp=$STAMP"
    echo "time=$(date -u 2>/dev/null)"
    echo "usb=$USB_DIR"
    echo "app_root=$APP_ROOT"
    echo "beep_enabled=$BEEP_ENABLED"
    if [ -n "${SSH_CONNECTION:-}" ]; then
        echo "started_by=ssh"
    else
        echo "started_by=failsafe"
    fi
    echo "uid=$(id -u 2>/dev/null)"
    echo "uname=$(uname -a 2>/dev/null)"
} >> "$REPORT" 2>&1

# --- the QML roots that could carry the Hello app set -------------------------
ROOTS=""
for d in "$APP_ROOT"/share/AppHomeScreen/qml_* "$APP_ROOT"/share/*/qml_*; do
    [ -d "$d" ] || continue
    case " $ROOTS " in
        *" $d "*) continue ;;
    esac
    ROOTS="$ROOTS $d"
done
echo "roots=$(echo "$ROOTS" | wc -w | tr -d ' ')" >> "$REPORT"

# --- the files packages/wifi/scripts/build_artifacts.py reads ------------------
# BASE_FILES: the five the builder needs; HelloTile.qml is copied for reference
# because the generated variant references that component.
BASE_FILES="HomeScreen/QHMI_HomeScreenMain_Advanced.qml HomeScreen/QHMI_HomeScreenMain_Wide.qml HomeScreen/Screen_HomeScreen_AllMenus.qml HomeScreen/Screen_HomeScreen_Submenu_Setup.qml component/QCustomizedMenuListAllowEmpty.qml"
OPTIONAL_FILES="component/HelloTile.qml"

VERDICT="absent"
for d in $ROOTS; do
    name="$(basename "$d")"
    dest="$RUN/roots/$name"
    have=0
    missing=""
    for rel in $BASE_FILES; do
        src="$d/$rel"
        if [ -f "$src" ]; then
            mkdir -p "$dest/$(dirname "$rel")" 2>/dev/null || true
            cp -f "$src" "$dest/$rel" 2>/dev/null || true
            have=$((have + 1))
        else
            missing="$missing $rel"
        fi
    done
    for rel in $OPTIONAL_FILES; do
        [ -f "$d/$rel" ] || continue
        mkdir -p "$dest/$(dirname "$rel")" 2>/dev/null || true
        cp -f "$d/$rel" "$dest/$rel" 2>/dev/null || true
    done

    # A page-based battery base is what the hello_battery_wifi variant needs;
    # record the candidates instead of guessing which one is meant.
    pages=""
    for f in "$d"/HomeScreen/*.qml "$d"/component/*.qml; do
        [ -f "$f" ] || continue
        grep -q 'id:[[:space:]]*batteryPage' "$f" 2>/dev/null || continue
        rel="${f#$d/}"
        pages="$pages $rel"
        mkdir -p "$dest/_battery_pages/$(dirname "$rel")" 2>/dev/null || true
        cp -f "$f" "$dest/_battery_pages/$rel" 2>/dev/null || true
    done

    # Variant markers the installers stamp into the Advanced page.
    adv="$d/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
    markers=""
    for m in HELLO_PAYLOAD_260128 WIFI_PAYLOAD_260128 BATTERY_PAYLOAD_260128 BATTERY_TILE_PAYLOAD_260128; do
        grep -q "$m" "$adv" 2>/dev/null && markers="$markers $m"
    done
    hello_tile=0
    helloTile1=0
    if [ -f "$adv" ]; then
        grep -q 'HelloTile' "$adv" 2>/dev/null && hello_tile=1
        grep -q 'helloTile1' "$adv" 2>/dev/null && helloTile1=1
    fi

    {
        echo "root=$name"
        echo "  files_copied=$have/5"
        echo "  missing=$missing"
        echo "  hello_tile=$hello_tile"
        echo "  helloTile1=$helloTile1"
        echo "  markers=$markers"
        echo "  battery_pages=$pages"
    } >> "$REPORT" 2>&1

    # Complete: the five base files and the Hello tile proven in the Advanced page.
    if [ "$have" -eq 5 ] && [ "$hello_tile" -eq 1 ] && [ "$VERDICT" != "complete" ]; then
        VERDICT="complete"
        BEST_ROOT="$name"
    fi
done

[ -n "${BEST_ROOT:-}" ] && echo "best_root=$BEST_ROOT" >> "$REPORT"
echo "verdict=$VERDICT" >> "$REPORT"

# --- audible verdict ----------------------------------------------------------
# Before the manifest: report.txt is hashed there, so nothing may be appended to
# it afterwards (a stale hash made the importer reject a perfectly good dump).
if [ "$VERDICT" = "complete" ]; then
    sleep 0.3
    beep "$USB_DIR/beep-done.dat" 3 0.35
else
    sleep 0.3
    beep "$USB_DIR/beep-start.dat" 2 0.25
fi
echo "beep_method=${BEEP_METHOD:-none}" >> "$REPORT" 2>/dev/null || true
echo "beep_attempts=${BEEP_LOG}" >> "$REPORT" 2>/dev/null || true

# --- manifest the importer verifies (written last: it covers every file) ------
( cd "$RUN" 2>/dev/null && find . -type f ! -name manifest.md5 2>/dev/null | sort \
    | xargs -r md5sum > manifest.md5 2>/dev/null ) || true
files="$(cd "$RUN" 2>/dev/null && find . -type f 2>/dev/null | wc -l | tr -d ' ')"
echo "manifest_files=${files:-0}"

printf '%s\n' "$STAMP" > "$OUT/latest.txt" 2>/dev/null || true
cp -f "$REPORT" "$USB_DIR/hello_dump_result.txt" 2>/dev/null || true
sync
exit 0
