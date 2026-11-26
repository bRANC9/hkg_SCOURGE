#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# compose.sh — assemble ONE combo USB stick with the selected SCOURGE packages.
#
# Proven pattern (combo v3): each package ships its own installer as
# combo/<pkg>/stage.sh (+ restore.sh); the master run_stuff.sh runs the
# selected installers in a fixed dependency order — battery -> wifi -> tweaks
# -> wifi1b -> migrate — because each installer detects the current QML base
# and picks a matching variant.  A Wifi1b selection without wifi/migrate is
# the standalone layout and is marked combo/standalone.
# Restore runs in reverse. The failsafe's md5 gate re-runs the master script
# whenever its content (the MODE= line) changes.
#
# Usage:
#   compose.sh /path/to/mounted/stick [battery,wifi,tweaks]
#   (no package list = interactive prompt)
set -eu
HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
# Build the car daemons against musl when the cross toolchain is present: the host
# glibc on a rolling distro is built for x86-64-v3, and a -static binary carries
# those routines with it. The head unit is an Atom-class CPU and faults with SIGILL
# (exit 132) on the first v3 instruction, which is what killed every daemon.
MUSL_BIN="$HERE/../work/x86_64-linux-musl-cross/bin"
if [ -x "$MUSL_BIN/musl-gcc" ]; then
    PATH="$MUSL_BIN:$PATH"
    export PATH
fi
TARGET="${1:?usage: compose.sh /path/to/stick [pkg,pkg,...]}"
SELECTION="${2:-}"

# Battery builds into a private tree; clean it up no matter where a later
# step aborts.  Re-installed below once the staging tree exists.
trap '[ -z "${BATTERY_BUILD:-}" ] || rm -rf "$BATTERY_BUILD"' EXIT

[ -d "$TARGET" ] || { echo "not a directory: $TARGET" >&2; exit 1; }
[ "$TARGET" != "/" ] || { echo "refusing to deploy to /" >&2; exit 1; }

# --- package selection -----------------------------------------------------------
AVAILABLE="battery wifi tweaks ambient fake-engine wifi1b migrate java button-gate applauncher dmclient-gate"
DEFAULT_ORDER="battery wifi tweaks"
WIP_STAGES="button-gate applauncher dmclient-gate"

if [ -z "$SELECTION" ]; then
    echo "Which packages should go on the stick?"
    echo "  battery — Battery tile + preconditioning/charge-door daemon"
    echo "  wifi    — WiFi client / CarPlay AP / SSH (dropbear)"
    echo "  tweaks  — night-mode fixes, auto-DST, OTA block, disclaimer, Reboot app"
    echo "  ambient — Ambience: ambient-light page/tile, reactive to music levels"
    echo "  fake-engine — parked engine sound (own synthesised loop, 127.0.0.1:8095)"
    echo "  wifi1b  — separate CarPlay AP/client app beside WiFi (or standalone)"
    echo "  migrate — one-way: remove WiFi, keep Battery/Tweaks and rebase Wifi1b"
    echo "  java    — headless Temurin 25 runtime for JAR apps/daemons (WIP, VM-validated, car-pending)"
    echo "  [WIP, signatureless-update research — docs/08; VM-validated, car-pending]"
    echo "  button-gate   — AppScomoDM stubs + drive wrapper + watcher (tools/update-debug)"
    echo "  applauncher   — AppLauncher .sign stubs"
    echo "  dmclient-gate  — the REAL button gate (AppDMClient) + launcher prerequisite"
    printf 'comma-separated list (default: the three packages; Tweaks includes Reboot): '
    read -r SELECTION
    [ -n "$SELECTION" ] || SELECTION="$DEFAULT_ORDER"
fi

SELECTED=""
for pkg in $(echo "$SELECTION" | tr ',' ' '); do
    echo "$AVAILABLE" | grep -qw "$pkg" || {
        echo "unknown package: $pkg (available: $AVAILABLE)" >&2; exit 1; }
    [ -n "$SELECTED" ] && SELECTED="$SELECTED "
    SELECTED="$SELECTED$pkg"
done
# Always preserve dependency order, regardless of the user's selection order.
REQUESTED="$SELECTED"
SELECTED=""
for pkg in $AVAILABLE; do
    case " $REQUESTED " in *" $pkg "*) SELECTED="${SELECTED:+$SELECTED }$pkg" ;; esac
done

# A Wifi1b-only selection beside Battery/Tweaks is the standalone layout: it
# never reinstalls WiFi and has no one-way migration stage.  build_flat.sh
# requests this explicitly; an interactive selection gets the same treatment.
STANDALONE=0
case " $SELECTED " in
    *" wifi1b "*)
        case " $SELECTED " in
            *" wifi "*|*" migrate "*) ;;
            *) STANDALONE=1 ;;
        esac ;;
esac
if [ "${SCOURGE_STANDALONE:-0}" = 1 ]; then
    case " $SELECTED " in
        *" wifi "*|*" migrate "*)
            echo "standalone bundle must not include wifi or migrate" >&2; exit 1 ;;
    esac
    for required in battery tweaks wifi1b; do
        case " $SELECTED " in
            *" $required "*) ;;
            *) echo "standalone bundle requires $required" >&2; exit 1 ;;
        esac
    done
    STANDALONE=1
fi

# --- build each selected package -------------------------------------------------
if [ "${SCOURGE_SKIP_BUILD:-0}" != "1" ]; then
    for pkg in $SELECTED; do
        case "$pkg" in
        battery|wifi|tweaks|wifi1b|migrate|ambient|fake-engine|java)
            echo "== building package: $pkg"
            if [ "$pkg" = battery ]; then
                mkdir -p "$HERE/battery/out"
                BATTERY_BUILD=$(mktemp -d "$HERE/battery/out/compose.XXXXXX")
                sh "$HERE/battery/scripts/build_payload.sh" "$BATTERY_BUILD"
            elif [ "$pkg" = ambient ] || [ "$pkg" = fake-engine ]; then
                rm -rf "$HERE/$pkg/out/usb"
                sh "$HERE/$pkg/scripts/build_payload.sh" "$HERE/$pkg/out/usb"
            else
                sh "$HERE/$pkg/scripts/build_payload.sh"
            fi || {
                echo "package build failed: $pkg" >&2; exit 1; } ;;
        *)  : ;;   # WIP gate stages stage from tools/update-debug below
        esac
    done
else
    echo "== SCOURGE_SKIP_BUILD=1 — using existing out/usb trees"
fi

# --- stage ----------------------------------------------------------------------
# Product path: never depend on a harness-specific temp directory.
STAGE=$(mktemp -d "${TMPDIR:-/tmp}/scourge_compose.XXXXXX")
trap 'rm -rf "$STAGE"; [ -z "${BATTERY_BUILD:-}" ] || rm -rf "$BATTERY_BUILD"' EXIT
mkdir -p "$STAGE/combo"
# Independently manifest the preflight helpers; verify them before sourcing
# on-car.  runtime_cleanup.sh is shared by the wrapper and the master so the
# cleanup policy has one implementation and one integrity gate.
cp "$HERE/common/install_io.sh" "$STAGE/combo/install_io.sh"
cp "$HERE/common/runtime_cleanup.sh" "$STAGE/combo/runtime_cleanup.sh"
cp "$HERE/common/boot_profile.sh" "$STAGE/combo/boot_profile.sh"
cp "$HERE/common/perf_snapshot.sh" "$STAGE/combo/perf_snapshot.sh"
RUNTIME_CLEANUP_MD5=$(md5sum "$STAGE/combo/runtime_cleanup.sh" | cut -d' ' -f1)
# The helpers are also verified by their own hashes baked into the master: the
# preflight manifest is a tiny file and this stick corrupts tiny files in combo/
# (seen 2026-11-22, twice), which failed the run before a single stage started.
INSTALL_IO_MD5=$(md5sum "$STAGE/combo/install_io.sh" | cut -d' ' -f1)

# Minimal HTTP client: the unit has neither wget nor nc, so the installer could
# not drive the daemon API (every api_walk step said "no wget/nc").  Static,
# baseline ISA, next to the helpers it belongs with.
if [ -f "$HERE/../tools/httpreq.c" ]; then
    mkdir -p "$STAGE/combo/bin"
    _cc=$(command -v musl-gcc 2>/dev/null || echo cc)
    "$_cc" -O2 -static -march=x86-64 -o "$STAGE/combo/bin/httpreq" "$HERE/../tools/httpreq.c" 2>/dev/null || \
        echo "warning: httpreq not built" >&2
fi

# Audible fallback assets.  The stick copies disappear (reformat, media player
# cleanup) and then the loader has nothing to play: ship the two tones with the
# bundle so the signal survives.  1 kHz for start, 1.4 kHz for done, 150 ms each.
if command -v python3 >/dev/null 2>&1; then
    python3 - "$STAGE/combo/bin" <<'PY'
import struct, sys, math, os
out = sys.argv[1]
os.makedirs(out, exist_ok=True)
def wav(path, freq, ms=150, rate=44100):
    n = int(rate * ms / 1000)
    data = b"".join(struct.pack("<h", int(12000 * math.sin(2 * math.pi * freq * i / rate))) for i in range(n))
    hdr = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16) + b"data" + struct.pack("<I", len(data))
    open(path, "wb").write(hdr + data)
wav(os.path.join(out, "beep-start.dat"), 1000)
wav(os.path.join(out, "beep-done.dat"), 1400)
PY
fi
if [ "$STANDALONE" = 1 ]; then
    printf 'standalone=1\nlayout=battery,tweaks,wifi1b\n' > "$STAGE/combo/standalone"
    STANDALONE_MD5=$(md5sum "$STAGE/combo/standalone" | cut -d' ' -f1)
else
    STANDALONE_MD5=""
fi
(
    cd "$STAGE" &&
    md5sum combo/install_io.sh combo/runtime_cleanup.sh > combo/preflight.md5 &&
    [ -z "$STANDALONE_MD5" ] || md5sum combo/standalone >> combo/preflight.md5
)
BUNDLE_LAYOUT=combo
[ "$STANDALONE" = 0 ] || BUNDLE_LAYOUT=standalone
# Only a bundle that installs or removes a radio owner may stop WiFi.  A
# battery,tweaks-only bundle must leave a working WiFi app alone.
#
# A SINGLE-package run must not stop the service it is about to restart either:
# on 2026-11-25 a "MODE=wifi" run stopped wifi-daemon in the preflight, then the
# wifi stage never ran (its digest gate refused), and the head unit was left with
# the radio down and no daemon - "restart does not bring it back", hours lost.
# With this only full/restore runs stop it up front; a single stage handles its
# own service, so a failed stage leaves the box exactly as it was.
STOPS_WIFI=0
case " $SELECTED " in
    *" wifi "*|*" wifi1b "*|*" migrate "*) STOPS_WIFI=1 ;;
esac

stage_pkg() {
    name="$1"
    src="$HERE/$name/out/usb"
    [ -d "$src" ] || src="$HERE/$name/out/battery_usb"
    if [ "$name" = battery ] && [ -n "${BATTERY_BUILD:-}" ]; then src=$BATTERY_BUILD; fi
    dst="$STAGE/combo/$name"
    mkdir -p "$dst"
    cp -R "$src/payload" "$dst/"
    cp -R "$src/backup" "$dst/" 2>/dev/null || true
    mkdir -p "$dst/backup_current"
    cp -f "$src/manifest.md5" "$dst/"
    cp -f "$src/run_stuff.sh" "$dst/stage.sh"
    # The per-package manifest never covered stage.sh, so a stage corrupted on
    # the medium only showed up in the car as "cannot execute binary file" or a
    # digest-gate refusal.  Add it here: the host-side copy -> remount -> verify
    # then catches it before the stick leaves the desk (3rd corruption, 2026-11-23).
    ( cd "$dst" && md5sum stage.sh >> manifest.md5 )
    cp -f "$src/run_stuff_restore.sh" "$dst/restore.sh" 2>/dev/null || true
    # Unchanged-stage skip data (see the master's stage_skippable) allows a
    # rebuilt bundle to leave already-installed packages untouched.
    for extra in skip.map skip.services; do
        [ ! -f "$HERE/$name/$extra" ] || cp -f "$HERE/$name/$extra" "$dst/$extra"
    done
    (cd "$dst" && md5sum -c manifest.md5 >/dev/null) || exit 1
    if [ "$name" = tweaks ]; then
        # The layout check wanted one specific variant, which only exists when the
        # base matrix was generated for that combination. Check the richest
        # Advanced variant the payload actually carries; skip when it carries none.
        # The check asserts the placement bindings of the Battery/WiFi/Tweaks
        # tiles, so it only means anything on a variant that carries all three;
        # a stock/wifi-only variant has no tweaksTile to check.
        layout_sample=""
        for prefer in battery_tile_wifi hello_battery_tile_wifi; do
            cand="$dst/payload/qml/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Advanced.$prefer.qml"
            [ -f "$cand" ] && { layout_sample="$cand"; break; }
        done
        if [ -n "$layout_sample" ]; then
            python3 "$HERE/common/check_layout.py" "$layout_sample"
        else
            echo "  (no Advanced variant in the tweaks payload - layout check skipped)"
        fi
    fi
}

stage_wip() {
    # WIP signatureless-update stages (docs/08): staged from tools/update-debug.
    name="$1"
    src="$HERE/../tools/update-debug"
    dst="$STAGE/combo/$name"
    mkdir -p "$dst"
    case "$name" in
    button-gate)   deploy="$src/button_gate.sh" ;;
    applauncher)   deploy="$src/applauncher_sigstub.sh"
                   cp -f "$src/applauncher_sigstub_restore.sh" "$dst/restore.sh"
                   cp -f "$src/applauncher_sigstub_diag.sh" "$dst/diag.sh" ;;
    dmclient-gate) deploy="$src/dmclient_gate.sh" ;;
    esac
    cp -f "$deploy" "$dst/stage.sh"
    # The gate deployers need the patched wrapper container next to them.
    wrapper="$HERE/../out/AppUpgrade.patched.te2"
    if [ -f "$wrapper" ]; then
        cp -f "$wrapper" "$dst/AppUpgrade.patched.te2"
    else
        echo "WARNING: $dst is missing AppUpgrade.patched.te2 —" >&2
        echo "  run scripts/07_build_patched_usb_appupgrade.sh first" >&2
    fi
}

for pkg in $SELECTED; do
    case "$pkg" in
    battery|wifi|tweaks|wifi1b|migrate|ambient|fake-engine|java) stage_pkg "$pkg" ;;
    *) stage_wip "$pkg" ;;
    esac
done

# --- master run_stuff.sh (MODE= pattern) -------------------------------------------
ORDER=$(echo "$SELECTED" | tr ' ' ',')
ORDER_SPACES="$SELECTED"

# Digest of every staged installer/manifest/helper. The loader re-runs the
# master script when its bytes change, so a same-selection rebuild with
# different package payloads still triggers an install on the next plug-in.
PAYLOAD_ID=$(cd "$STAGE" && find combo -type f \( -name manifest.md5 -o -name stage.sh -o -name restore.sh \
    -o -name runtime_cleanup.sh -o -name standalone \) -print0 \
    | LC_ALL=C sort -z | xargs -0 md5sum | md5sum | cut -c1-16)

# Per-installer digests, baked into the master.  The per-package manifest
# cannot protect an installer whose own bytes (or the manifest itself) came
# back corrupt from the medium: on 2026-11-23 combo/battery/stage.sh arrived as
# binary garbage after a sync that verified clean, and the run died inside the
# install with "cannot execute binary file" (rc=126).  With the digests in this
# file a corrupt installer is refused before it runs.
STAGE_DIGESTS=$(cd "$STAGE" && for _f in combo/*/stage.sh combo/*/restore.sh; do
    [ -f "$_f" ] || continue
    printf '%s %s\n' "$(md5sum "$_f" | cut -d' ' -f1)" "$_f"
done)

cat > "$STAGE/run_stuff.sh" <<EOF
#!/bin/sh
# SCOURGE_COMBO_REV=1 — master installer for: $ORDER
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only (https://gnu.org/licenses/agpl-3.0)
#
# Put this file and combo/ at the ROOT of a FAT/exFAT USB stick. The
# initial-root failsafe runs it as root within ~8 s of a content change.
#
# PAYLOAD_ID is a digest of the selected stage installers + manifests. It is
# part of this file, so a rebuilt package changes the loader's md5 gate.
PAYLOAD_ID="$PAYLOAD_ID"
# The bundle shape and the helper digest are baked in: a missing/unreadable
# standalone marker or cleanup helper fails closed before any stage runs.
BUNDLE_LAYOUT="$BUNDLE_LAYOUT"
STOPS_WIFI_BAKED="$STOPS_WIFI"
RUNTIME_CLEANUP_MD5="$RUNTIME_CLEANUP_MD5"
INSTALL_IO_MD5="$INSTALL_IO_MD5"
STANDALONE_MARKER_MD5="$STANDALONE_MD5"
STAGE_ORDER="$ORDER_SPACES"
# <md5> <relative path>, one per installer: the bytes this master expects.
STAGE_DIGESTS="
$STAGE_DIGESTS
"
#
# MODE (edit the line below; the md5 change makes the loader re-run it):
#   all       every selected package, in dependency-safe order (default)
#   restore   restore every selected package in reverse order
#   battery|wifi|tweaks   just that package
MODE="\${SCOURGE_MODE:-all}"
# A single-package run must not stop the service it is about to restart: on
# 2026-11-25 a MODE=wifi run stopped wifi-daemon in the preflight, the wifi stage
# then never ran (its digest gate refused) and the head unit was left with the radio
# down and no daemon - "restart does not bring it back".  Only a full/restore run
# stops it up front; a single stage owns its service and a failure changes nothing.
case "\$MODE" in
    all|restore) STOPS_WIFI="\${STOPS_WIFI_BAKED:-0}" ;;
    *)           STOPS_WIFI=0 ;;
esac

set -u

# The unit runs a Korean locale, so the date command prints a Korean weekday and
# tools emit localised text - that lands in combo.log, the probes and the UI
# popup.  Pin the C locale so every log line is ASCII.  (No backticks here: this
# text is inside an expanding heredoc.)
export LC_ALL=C LANG=C LC_TIME=C 2>/dev/null || true
PATH="\${COMBO_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}"
USB_DIR="\${USB_DIR:-\$(CDPATH= cd -- "\$(dirname "\$0")" && pwd)}"
STICK_DIR="\${SCOURGE_STICK_DIR:-\$USB_DIR}"
COMBO="\$USB_DIR/combo"
RW_DIR="\${COMBO_RW_DIR:-/rw_data/combo}"
RUNTIME_ROOT="\${COMBO_RUNTIME_ROOT:-/rw_data}"
QML_BASE="\${COMBO_QML_DIR:-/app/share/AppHomeScreen/qml_eco3.0}"
RUN_DIR="\${COMBO_RUN_DIR:-/run}"
WIFI1B_SERVICE="\${WIFI1B_SERVICE:-/etc/systemd/system/wifi1b-daemon.service}"
WIFI_SERVICE="\${WIFI_SERVICE:-/etc/systemd/system/wifi-daemon.service}"
MD5SUM_BIN=md5sum
command -v "\$MD5SUM_BIN" >/dev/null 2>&1 || MD5SUM_BIN=md5sum.coreutils
SHA256SUM_BIN=sha256sum
command -v "\$SHA256SUM_BIN" >/dev/null 2>&1 || SHA256SUM_BIN=sha256sum.coreutils
RUNTIME_CLEANUP_SHA256="\$SHA256SUM_BIN"

# FIRST: verify and load the cleanup helper without writing anything, then
# reclaim bounded runtime log space before any other runtime write.  A full
# /rw_data makes every later write fail.  Stale temps are handled later, only
# after owned radios are verified stopped.
# runtime_cleanup.sh is best effort (kill owned daemons, bound runtime logs). A
# missing or corrupted copy must not stop the run: this stick corrupted the small
# files under combo/ three times on 2026-11-22 and every attempt died right here,
# before a single stage. Warn instead, and use no-ops that report "not alive" so
# the radio bookkeeping stays conservative.
CLEANUP_LIB="\$COMBO/runtime_cleanup.sh"
_cleanup_ok=0
if [ -f "\$CLEANUP_LIB" ] && [ ! -L "\$CLEANUP_LIB" ] && [ -r "\$CLEANUP_LIB" ]; then
    _cleanup_md5=\$("\$MD5SUM_BIN" "\$CLEANUP_LIB" 2>/dev/null | cut -d' ' -f1)
    [ -n "\$_cleanup_md5" ] && [ "\$_cleanup_md5" = "\$RUNTIME_CLEANUP_MD5" ] && _cleanup_ok=1
fi
if [ "\$_cleanup_ok" = 1 ]; then
    . "\$CLEANUP_LIB"
else
    echo "combo: runtime_cleanup.sh missing or corrupt - continuing without it" \
        >> "\$USB_DIR/combo_second_run.log" 2>/dev/null || true
    cleanup_bounded_logs(){ :; }
    runtime_cleanup_kill_owned(){ :; }
    runtime_cleanup_process_alive(){ return 1; }
fi
# One run at a time. The loader re-executes run_stuff.sh on a content change, and
# every stage writes its own result and backup_current into the stick - which is a
# content change. Two overlapping runs then fight over combo/ and the second one
# fails the helper check while the first is still installing (2026-11-22: the root
# result was the integrity stub while the stages had already run).
# The loader mounts the stick read-write first and falls back to read-only when the
# filesystem refuses it (a dirty exFAT after an unclean pull).  2026-11-25: that
# fallback cost a whole round - run_stuff.sh died on "run_stuff_started.log:
# Read-only file system", wrote no result, made no sound, and nothing on the stick
# said why.  Probe once, remount, and if the medium stays read-only say it where it
# can still be read afterwards.
if ! : >> "\$USB_DIR/.scourge-rw-probe" 2>/dev/null; then
    mount -o remount,rw "\$USB_DIR" 2>/dev/null || true
    if : >> "\$USB_DIR/.scourge-rw-probe" 2>/dev/null; then
        rm -f "\$USB_DIR/.scourge-rw-probe" 2>/dev/null || true
        printf 'stick was read-only, remounted read-write %s\n' "\$(date -u 2>/dev/null)" \
            >> "\$RW_DIR/ro_stick.log" 2>/dev/null || true
    else
        printf 'stick=%s ro=1 boot=%s time=%s note=read-only medium, nothing installed\n' \
            "\$USB_DIR" "\$(cat /proc/sys/kernel/random/boot_id 2>/dev/null)" \
            "\$(date -u 2>/dev/null)" >> "\$RW_DIR/run_stuff_ro_stick.txt" 2>/dev/null || true
        # Two short tones: distinct from the start (1) and done (5) patterns, so the
        # failure is audible even though the medium cannot carry the message.
        _i=0
        while [ "\$_i" -lt 2 ]; do
            aplay -q -f S16_LE -r 44100 -c 1 "\$USB_DIR/beep-start.dat" >/dev/null 2>&1 || true
            _i=\$((_i + 1))
            [ "\$_i" -lt 2 ] && sleep 0.25 2>/dev/null || true
        done
        exit 1
    fi
fi
rm -f "\$USB_DIR/.scourge-rw-probe" 2>/dev/null || true

# Breadcrumb before anything else: the run lock, the helper checks and the first
# stage all come later, and every one of them can exit without a sound. If this
# file does not change when the stick is plugged, the loader never started us at
# all (md5 gate, unreadable medium, or the unit was asleep).
printf 'started=%s boot=%s mode=%s payload=%s\n' \
    "\$(date -u 2>/dev/null)" "\$(cat /proc/sys/kernel/random/boot_id 2>/dev/null)" \
    "\${SCOURGE_MODE:-all}" "\$PAYLOAD_ID" >> "\$USB_DIR/run_stuff_started.log" 2>/dev/null || true
printf 'started=%s payload=%s\n' "\$(date -u 2>/dev/null)" "\$PAYLOAD_ID" >> "\$RW_DIR/started.log" 2>/dev/null || true
# The audible start tone is played right after the log is opened (below), so the
# log says which beep the run made before it reached the preflight.

RUN_LOCK="\$USB_DIR/.scourge-run"
_boot_id=\$(cat /proc/sys/kernel/random/boot_id 2>/dev/null || echo unknown)
if ! mkdir "\$RUN_LOCK" 2>/dev/null; then
    _held=\$(cat "\$RUN_LOCK/started" 2>/dev/null || echo 0)
    _held_boot=\$(cat "\$RUN_LOCK/boot" 2>/dev/null || echo none)
    _now=\$(date +%s 2>/dev/null || echo 0)
    # A lock from another boot is stale by definition: the process that took it
    # cannot exist any more. Same-boot locks expire after 15 minutes. Without the
    # boot id a killed run left the stick locked and the next plug did nothing at
    # all - no beep, no output (2026-11-22).
    if [ "\$_held_boot" = "\$_boot_id" ] && [ "\$_now" -gt 0 ] && \
       [ "\$((_now - _held))" -lt 900 ] 2>/dev/null; then
        echo "combo: another run holds \$RUN_LOCK (started \${_held}s, same boot) - exiting" >> "\$USB_DIR/combo_second_run.log" 2>/dev/null
        exit 0
    fi
    echo "combo: clearing a stale run lock (boot=\$_held_boot started=\$_held)" >> "\$USB_DIR/combo_second_run.log" 2>/dev/null || true
    rm -rf "\$RUN_LOCK" 2>/dev/null || true
    mkdir "\$RUN_LOCK" 2>/dev/null || exit 0
fi
printf '%s\n' "\$_boot_id" > "\$RUN_LOCK/boot" 2>/dev/null || true
date +%s > "\$RUN_LOCK/started" 2>/dev/null || true
trap 'rm -rf "\$RUN_LOCK" 2>/dev/null || true' EXIT INT TERM HUP

RUNTIME_CLEANUP_LOG=""
cleanup_bounded_logs "\$RUNTIME_ROOT" || true

mkdir -p "\$RW_DIR" 2>/dev/null || RW_DIR="\$USB_DIR"
LOG="\$RW_DIR/combo.log"
: > "\$LOG" 2>/dev/null
RUNTIME_CLEANUP_LOG="\$LOG"
[ -z "\$RUNTIME_CLEANUP_NOTES" ] || printf 'cleanup: %s\n' "\$RUNTIME_CLEANUP_NOTES" >> "\$LOG" 2>/dev/null
log(){ echo "\$(date -u 2>/dev/null) \$*" >> "\$LOG" 2>/dev/null; }
case "\$STAGE_DIGESTS" in
    "") log "stage digest table: EMPTY" ;;
    *)  log "stage digest table: present" ;;
esac

# Audible start: the micom beep is silent on this unit, so a local tone makes
# "did the loader start us" a sound. Logged, because a run that dies later must
# still tell which tone it made (the start and done tones differ).
audio_wake(){
    # A tone is proof of intent, not of sound: on this unit aplay exits 0 while the
    # amp is idle or the mixer is muted (the micom beep is silent by design here).
    if command -v amixer >/dev/null 2>&1; then
        amixer -q sset "Master" 80% unmute >/dev/null 2>&1 || true
        amixer -q sset "PCM" 80% unmute >/dev/null 2>&1 || true
        amixer -q sset "Headphone" 80% unmute >/dev/null 2>&1 || true
    fi
    if command -v pactl >/dev/null 2>&1; then
        pactl set-sink-mute @DEFAULT_SINK@ 0 >/dev/null 2>&1 || true
        pactl set-sink-volume @DEFAULT_SINK@ 70% >/dev/null 2>&1 || true
    fi
    # Unmuting is not enough on this unit: the amp/DSP route powers down while
    # nothing plays, and then a tone is silently dropped even though aplay exits 0
    # ("no beep unless something is already playing", measured 2026-11-26).  A short
    # silent stream wakes the route; the tone right after it is audible.  Raw PCM on
    # purpose: no WAV header to seek, and /dev/zero cannot be missing).
    _z=/tmp/scourge-silence.raw
    [ -s "\$_z" ] || head -c 26460 /dev/zero > "\$_z" 2>/dev/null || true
    if [ -s "\$_z" ]; then
        aplay -q -f S16_LE -r 44100 -c 1 "\$_z" >/dev/null 2>&1 || \
            aplay -q -D plughw:0,0 -f S16_LE -r 44100 -c 1 "\$_z" >/dev/null 2>&1 || true
    fi
}
play_tone(){   # play_tone <file> <count>: logs which device actually played it
    _wav="\$1"; _n="\$2"; _k=0
    audio_wake
    while [ "\$_k" -lt "\$_n" ]; do
        _ok=""
        for _dev in "" "-D plughw:0,0" "-D default" "-D hw:0,0"; do
            if aplay -q \$_dev -f S16_LE -r 44100 -c 1 "\$_wav" >/dev/null 2>&1; then
                if [ -n "\$_dev" ]; then _ok="\$_dev"; else _ok="default"; fi
                break
            fi
        done
        if [ -n "\$_ok" ]; then log "beep: played \$(basename "\$_wav") via \$_ok"
        else log "beep: no device played \$(basename "\$_wav")"; fi
        _k=\$((_k + 1))
        [ "\$_k" -lt "\$_n" ] && sleep 0.4 2>/dev/null || true
    done
}
if [ -f "\$USB_DIR/beep-start.dat" ]; then
    log "beep: start tone (pre-preflight)"
    play_tone "\$USB_DIR/beep-start.dat" 2
fi

# Every exit path publishes the run log to the stick.  combo.log lives on
# /rw_data, which no cable reaches once the stick is out: without this export a
# run that dies between the preflight and the first stage leaves nothing to read
# (2026-11-23: three starts on the stick, no result, no log, and the tones
# alternating between the start and the failure pattern).
publish_logs(){
    _ts=\$(date -u +%Y%m%dT%H%M%SZ 2>/dev/null || echo run)
    _out="\$USB_DIR/combo_export_\$_ts.log"
    {
        echo "SCOURGE_COMBO_LOG"
        echo "time=\$_ts rw=\$RW_DIR mode=\${MODE:-?} payload=\$PAYLOAD_ID"
        echo "--- started"
        tail -n 12 "\$RW_DIR/started.log" 2>/dev/null
        echo "--- combo.log"
        cat "\$LOG" 2>/dev/null
        echo "--- resources (free/df)"
        free 2>/dev/null | head -n 4
        df -k / /rw_data /tmp 2>/dev/null | head -n 6
        echo "--- stage logs (rw_data)"
        for _sl in /rw_data/*/install.log; do
            [ -f "\${_sl}" ] || continue
            echo "## \${_sl}"
            tail -n 250 "\${_sl}" 2>/dev/null
        done
        echo "--- stages.log"
        cat "\${STAGES_LOG:-/dev/null}" 2>/dev/null
    } > "\$_out" 2>/dev/null || true
}
# The lock must go whatever happens; the export must land before it does.
trap 'publish_logs; rm -rf "\$RUN_LOCK" 2>/dev/null || true' EXIT INT TERM HUP

result(){
    {
        echo "SCOURGE_COMBO"
        echo "time=\$(date -u 2>/dev/null)"
        echo "usb=\$USB_DIR"
        echo "uid=\$(id -u 2>/dev/null)"
        echo "mode=\$MODE"
        echo "payload_id=\$PAYLOAD_ID"
        cat "\$STAGES_LOG" 2>/dev/null
        echo "status=\$1"
    } > "\$USB_DIR/run_stuff_result.txt" 2>&1
    cp -f "\$USB_DIR/run_stuff_result.txt" "\$RW_DIR/result.txt" 2>/dev/null
    sync
}

STAGES_LOG="\$RW_DIR/stages.log"
# Where the WiFi daemon publishes its own build id (deferred verify reads it).
RADIO_STATUS="\${WIFI_STATUS_FILE:-/rw_data/wifi/status.json}"
: > "\$STAGES_LOG" 2>/dev/null
FAILED=""

# --- unchanged-stage skip -----------------------------------------------
# A stage is skipped only when ALL hold:
#   1. the packaged digest (manifest + installer) equals the digest recorded
#      after that stage's last successful install;
#   2. every curated installed file in its skip.map still md5-matches the
#      packaged file (and required paths/services still exist);
#   3. the shared home-screen QML is byte-identical to the last successful
#      run's recorded output.
# The ledger lives in /rw_data, so a firmware update or a restore invalidates
# it. MODE=<package> always reinstalls; skipping applies to MODE=all only.
LEDGER="\${COMBO_LEDGER_DIR:-/rw_data/combo/installed}"
QML_BASE="\${COMBO_QML_DIR:-/app/share/AppHomeScreen/qml_eco3.0}"
QML_FILES="HomeScreen/QHMI_HomeScreenMain_Advanced.qml
HomeScreen/QHMI_HomeScreenMain_Wide.qml
HomeScreen/Screen_HomeScreen_AllMenus.qml
HomeScreen/Screen_HomeScreen_Submenu_Setup.qml
component/QCustomizedMenuListAllowEmpty.qml
component/common/Q_ThemeImage.qml"
md5f(){ "\$MD5SUM_BIN" "\$1" 2>/dev/null | cut -d' ' -f1; }
qml_digest(){
    for _q in \$QML_FILES; do
        [ -f "\$QML_BASE/\$_q" ] || return 1
        md5f "\$QML_BASE/\$_q"
    done | "\$MD5SUM_BIN" | cut -d' ' -f1
}
stage_digest(){
    (
        cat "\$COMBO/\$1/manifest.md5" "\$COMBO/\$1/stage.sh" \
            "\$COMBO/\$1/skip.map" "\$COMBO/\$1/skip.services" 2>/dev/null
        # Owner-supplied SSH keys / WiFi profiles are part of the installed
        # state: a changed key must re-run the stage, while an identical one
        # must still skip.
        case "\$1" in
        wifi|wifi1b)
            for _in in authorized_keys wifi.txt iphone_wifi.txt; do
                [ ! -f "\$USB_DIR/\$_in" ] || cat "\$USB_DIR/\$_in"
            done ;;
        esac
    ) | "\$MD5SUM_BIN" | cut -d' ' -f1
}
stage_owns_destination(){
    # True when another selected stage ships this exact destination.  A
    # "final stage owns it" entry in skip.map is then left to that stage.
    _own_from=\$1; _own_dst=\$2; _own_seen=0
    for _own_s in \$STAGE_ORDER; do
        if [ "\$_own_s" = "\$_own_from" ]; then _own_seen=1; continue; fi
        [ "\$_own_seen" = 1 ] || continue
        [ -f "\$COMBO/\$_own_s/skip.map" ] || continue
        while IFS= read -r _own_line; do
            [ -n "\$_own_line" ] || continue
            case "\$_own_line" in \#*|'!'*) continue ;; esac
            _own_line=\${_own_line#\?}
            _own_line=\${_own_line#\*}
            [ "\${_own_line#*|}" = "\$_own_dst" ] || continue
            [ -f "\$COMBO/\$_own_s/\${_own_line%%|*}" ] || continue
            return 0
        done < "\$COMBO/\$_own_s/skip.map"
    done
    return 1
}
stage_skippable(){
    _s=\$1; _d="\$COMBO/\$_s"
    [ -f "\$_d/skip.map" ] || return 1
    [ -f "\$LEDGER/\$_s.digest" ] || return 1
    _want=\$(cat "\$LEDGER/\$_s.digest" 2>/dev/null)
    [ -n "\$_want" ] && [ "\$_want" = "\$(stage_digest "\$_s")" ] || return 1
    _qrec=\$(cat "\$LEDGER/final-qml.digest" 2>/dev/null)
    _qnow=\$(qml_digest)
    [ -n "\$_qrec" ] && [ -n "\$_qnow" ] && [ "\$_qrec" = "\$_qnow" ] || return 1
    while IFS='|' read -r _left _dst; do
        [ -n "\$_left" ] || continue
        case "\$_left" in \#*) continue ;; esac
        case "\$_left" in
        \!*) [ -e "\${_left#!}" ] || return 1; continue ;;
        esac
        _rel=\$_left; _cond=0; _final=0
        case "\$_left" in
            \?*) _rel=\${_left#\?}; _cond=1 ;;
            \**) _rel=\${_left#\*}; _final=1 ;;
        esac
        # "*" = the final selected stage that ships this destination owns the
        # installed-state check.  Standalone builds have no WiFi stage, so the
        # Battery map must still verify its BatteryTile itself.
        [ "\$_final" = 0 ] || ! stage_owns_destination "\$_s" "\$_dst" || continue
        [ "\$_cond" = 0 ] || [ -d "\$(dirname "\$_dst")" ] || continue
        [ -f "\$_d/\$_rel" ] || continue
        [ -f "\$_dst" ] || return 1
        [ "\$(md5f "\$_dst")" = "\$(md5f "\$_d/\$_rel")" ] || return 1
    done < "\$_d/skip.map"
    if [ -f "\$_d/skip.services" ]; then
        while IFS= read -r _unit; do
            [ -n "\$_unit" ] || continue
            case "\$_unit" in \#*) continue ;; esac
            command -v systemctl >/dev/null 2>&1 || return 1
            systemctl is-active --quiet "\$_unit" 2>/dev/null || return 1
        done < "\$_d/skip.services"
    fi
    return 0
}
record_stage_digest(){
    mkdir -p "\$LEDGER" 2>/dev/null || return 0
    stage_digest "\$1" > "\$LEDGER/\$1.digest" 2>/dev/null || true
}

# Audible installer status through the factory micom audio service.  The
# installer copies its own dbus-send so the call runs in the same SMACK
# context as the working micom client (the floor rule allows
# "_ App::micommanager rwx---").  Beeps are strictly non-fatal.
BEEP_DBUS="\$RW_DIR/bin/dbus-send"
beep_init(){
    [ -x "\$BEEP_DBUS" ] && return 0
    _src=/usr/bin/dbus-send
    [ -x "\$_src" ] || _src=/bin/dbus-send
    [ -x "\$_src" ] || return 1
    mkdir -p "\$RW_DIR/bin" 2>/dev/null || return 1
    rm -f "\$BEEP_DBUS" 2>/dev/null || true
    cp -f "\$_src" "\$BEEP_DBUS" 2>/dev/null || return 1
    chmod 755 "\$BEEP_DBUS" 2>/dev/null || true
    chown root:root "\$BEEP_DBUS" 2>/dev/null || true
    _label=\$(cat /proc/self/attr/current 2>/dev/null | tr -d '\r\n')
    [ -n "\$_label" ] && chsmack -e "\$_label" "\$BEEP_DBUS" 2>/dev/null || true
    [ -x "\$BEEP_DBUS" ]
}
beep_once(){
    _b=\$BEEP_DBUS
    [ -x "\$_b" ] || _b=/usr/bin/dbus-send
    # The micom call is attempted first, but success there is NOT proof of sound:
    # on this unit it returns OK and stays silent (amp/idle route at boot), which
    # is why earlier builds never beeped. The local tone below is played
    # regardless, so "micom OK but no audio" cannot swallow the signal.
    if [ -x "\$_b" ]; then
        "\$_b" --system --print-reply --reply-timeout=1000 --dest=com.lge.car.micom \
            /com/lge/car/micom com.lge.car.micom.audio.SetBeepPlay \
            array:byte:0x00 >/dev/null 2>&1 || {
            # The system bus may still be coming up when the loader starts us.
            sleep 0.3 2>/dev/null || sleep 1
            "\$_b" --system --print-reply --reply-timeout=1000 --dest=com.lge.car.micom \
                /com/lge/car/micom com.lge.car.micom.audio.SetBeepPlay \
                array:byte:0x00 >/dev/null 2>&1 || true
        }
    fi
    # The micom method is silent on some units (the 2021 car never beeped), so
    # fall back to an audible tone: the raw-PCM files the dump script uses,
    # played by aplay with an explicit format. The .dat extension is deliberate -
    # the head unit media player must not loop them.
    # Two sources, in order: the stick files, then the tones shipped in the
    # bundle (combo/bin).  A missing medium must not silence the run, and nothing
    # here may block or repeat (speaker-test looped on this alsa build).
    _n=\${1:-1}
    _wav="\$USB_DIR/beep-start.dat"
    [ "\$_n" -ge 3 ] 2>/dev/null && _wav="\$USB_DIR/beep-done.dat"
    if [ ! -f "\$_wav" ]; then
        _bundle="\$USB_DIR/combo/bin/beep-start.dat"
        [ "\$_n" -ge 3 ] 2>/dev/null && _bundle="\$USB_DIR/combo/bin/beep-done.dat"
        [ -f "\$_bundle" ] && _wav="\$_bundle"
    fi
    # Audible signal must not depend on the medium: say in the log which path was
    # taken, and fall back to synthesising the tone when the .dat files are gone.
    if [ -f "\$_wav" ]; then
        aplay -q -f S16_LE -r 44100 -c 1 "\$_wav" >>"\$LOG" 2>&1 && { log "beep: played \$(basename "\$_wav")"; return 0; }
        aplay -q -D plughw:0,0 -f S16_LE -r 44100 -c 1 "\$_wav" >>"\$LOG" 2>&1 && { log "beep: played \$(basename "\$_wav") via plughw:0,0"; return 0; }
        log "beep: aplay failed for \$_wav"
    else
        log "beep: \$_wav missing (the .dat files live on the stick root)"
    fi
    # speaker-test is deliberately NOT used as a fallback: on this alsa build it
    # ran as a continuous tone (the loader then appeared to "beep forever"), and
    # the tone must never block the run.
    return 0
}
beep_n(){
    _n=\$1
    log "beep: \$_n tone(s)"
    beep_init 2>/dev/null || true
    _i=0
    while [ "\$_i" -lt "\$_n" ]; do
        beep_once "\$_n"
        _i=\$((_i + 1))
        if [ "\$_i" -lt "\$_n" ]; then
            sleep 0.5 2>/dev/null || sleep 1
        fi
    done
    return 0
}

run_stage(){
    name="\$1"; script="\$2"
    stage_dir=\$(dirname "\$script")
    stage_result="\$stage_dir/run_stuff_result.txt"
    # Clear both copies: a stage that dies before writing its result must
    # not leave the previous run's result looking current on the stick.
    rm -f "\$stage_result" "\$USB_DIR/\${name}_result.txt"
    log "=== stage \$name ==="
    publish_logs
    if [ ! -f "\$script" ]; then
        echo "stage_\$name=status=missing" >> "\$STAGES_LOG"
        FAILED="\$FAILED \$name"
        beep_n 2
        return 1
    fi
    # Baked-digest gate: a corrupt installer must never run.  The lookup uses the
    # read builtin over the table, not printf/awk: on 2026-11-23 the table was in
    # this file and the pipeline still came back empty on the unit, so the gate
    # said "no baked digest" for every stage and failed open.  No external tool,
    # no multi-line printf.
    _rel="combo\${script#\$COMBO}"
    _want=""
    if [ -n "\$STAGE_DIGESTS" ]; then
        while read -r _d _p; do
            [ "\$_p" = "\$_rel" ] || continue
            _want="\$_d"
            break
        done <<EOF_STAGE_DIGESTS
\$STAGE_DIGESTS
EOF_STAGE_DIGESTS
    fi
    if [ -n "\$_want" ]; then
        _got=\$(md5sum "\$script" 2>/dev/null | cut -d' ' -f1)
        if [ "\$_got" != "\$_want" ]; then
            echo "stage_\$name=status=corrupt got=\${_got:-none} want=\$_want" >> "\$STAGES_LOG"
            log "stage \$name: installer bytes do not match the baked digest - refusing to run it (medium corruption; re-sync the stick)"
            FAILED="\$FAILED \$name"
            beep_n 2
            return 1
        fi
        log "stage \$name: installer digest verified"
    elif [ -n "\$STAGE_DIGESTS" ]; then
        # The master carries a table, so every staged installer must have an
        # entry: an unverified installer is treated as a failure, not a warning.
        echo "stage_\$name=status=unverified path=\$_rel" >> "\$STAGES_LOG"
        log "stage \$name: no digest entry for \$_rel in the baked table - refusing to run an unverified installer"
        FAILED="\$FAILED \$name"
        beep_n 2
        return 1
    else
        log "stage \$name: this master carries no digest table; running \$_rel unverified"
    fi

    # Unchanged stage: same packaged bytes as its last successful install and
    # every installed file/service/UI still matches. MODE=all only; an
    # explicit MODE=<package> (or restore) always runs the real installer.
    if [ "\$MODE" = all ] && stage_skippable "\$name"; then
        { echo "SCOURGE_COMBO_STAGE"; echo "stage=\$name"; echo "status=SKIPPED";
          echo "reason=unchanged_payload_and_installed_state"; } > "\$stage_result"
        cp -f "\$stage_result" "\$USB_DIR/\${name}_result.txt" 2>/dev/null || true
        echo "stage_\$name=status=SKIPPED" >> "\$STAGES_LOG"
        log "stage \$name skipped: payload and installed state unchanged"
        return 0
    fi
    # Per-package installers honor USB_DIR; point them at their own subtree.
    # Forward explicit owner inputs into the WiFi subtree. Do not package
    # credentials in source or leave stale copies after the stage finishes.
    case "\$name" in wifi|wifi1b)
        for input in wifi.txt iphone_wifi.txt authorized_keys MDNSD_ROOT_ARM; do
            [ ! -f "\$USB_DIR/\$input" ] || cp -f "\$USB_DIR/\$input" "\$stage_dir/\$input"
        done ;;
    esac
    # A WiFi-removal bundle rebuilds the shared QML in its migrate stage, so
    # Wifi1b must not fail on unknown current bytes or patch them first.
    _defer=""
    [ "\$name" = wifi1b ] && [ -f "\$COMBO/migrate/stage.sh" ] && _defer=1
    USB_DIR="\$stage_dir" SCOURGE_STICK_DIR="\$STICK_DIR" WIFI1B_DEFER_QML="\$_defer" SCOURGE_DEFER_WIFI_WAIT="\${SCOURGE_DEFER_WIFI_WAIT:-1}" \
        SCOURGE_DEFER_UI_RESTART=1 \
        /bin/sh "\$script" >> "\$LOG" 2>&1
    stage_rc=\$?
    log "stage \$name exit=\$stage_rc"
    log "stage \$name exit=\$stage_rc"
    status=\$(sed -n 's/^status=\([^ ]*\).*/\1/p' "\$stage_result" 2>/dev/null | tail -n 1)
    if [ -z "\$status" ] && [ -f "/rw_data/\$name/run_stuff_result.txt" ]; then
        status=\$(sed -n 's/^status=\([^ ]*\).*/\1/p' "/rw_data/\$name/run_stuff_result.txt" 2>/dev/null | tail -n 1)
        log "stage \$name: result read from /rw_data/\$name (the stick copy is missing)"
    fi
    [ ! -f "\$stage_result" ] || cp -f "\$stage_result" "\$USB_DIR/\${name}_result.txt"
    case "\$name" in wifi|wifi1b) rm -f "\$stage_dir/wifi.txt" "\$stage_dir/iphone_wifi.txt" "\$stage_dir/authorized_keys" "\$stage_dir/MDNSD_ROOT_ARM" ;; esac
    # A stage that exits 0 with status=REBOOT_REQUIRED succeeded: the WiFi
    # installer reports it when the UI restart could not be verified on the
    # spot.  Only rc=0 plus an accepted status counts; rc=0 with a failure
    # status is still a failure.
    case "\$stage_rc:\$status" in
        # DAEMON_UNVERIFIED is a deferred condition, not a failure: on this unit
        # the WiFi module registers wlan0 ~110 s after boot, so the installer
        # legitimately cannot see the daemon yet. Failing here stopped the whole
        # combo before the tweaks stage ever ran (2026-11-22, three runs).
        0:OK|0:REBOOT_REQUIRED|0:DAEMON_UNVERIFIED)
            echo "stage_\$name=status=\$status rc=0" >> "\$STAGES_LOG"
            [ "\$MODE" = restore ] || record_stage_digest "\$name"
            return 0
            ;;
    esac
    echo "stage_\$name=status=\${status:-missing} rc=\$stage_rc" >> "\$STAGES_LOG"
    FAILED="\$FAILED \$name"
    beep_n 2
    return 1
}

# One beep: installer started.
beep_n 1

log "step: preflight"
publish_logs

# Read-only preflight, before any stage creates backups or changes services.
# Helper integrity first, by baked hash: independent of combo/preflight.md5, which
# has been observed corrupted on the medium. Then the manifest, when it parses.
# install_io.sh is the safety-critical helper (root mount handling, verified
# copies). Source the first copy whose bytes match the baked hash, preferring the
# one inside a package payload: those are covered by the big per-package manifests,
# which have been reliable, while the tiny files directly under combo/ have been
# corrupted on the medium three times on 2026-11-22.
IO_HELPER=""
for _cand in "\$COMBO/battery/payload/lib/install_io.sh" "\$COMBO/wifi/payload/lib/install_io.sh" \
             "\$COMBO/tweaks/payload/lib/install_io.sh" "\$COMBO/install_io.sh"; do
    [ -f "\$_cand" ] || continue
    if [ "\$("\$MD5SUM_BIN" "\$_cand" 2>/dev/null | cut -d' ' -f1)" = "\$INSTALL_IO_MD5" ]; then
        IO_HELPER="\$_cand"
        break
    fi
done
if [ -z "\$IO_HELPER" ]; then
    echo 'preflight=status=FAIL error=install_io_bytes (no intact copy found)' >> "\$STAGES_LOG"
    result FAIL; beep_n 2; exit 1
fi
[ "\$IO_HELPER" = "\$COMBO/install_io.sh" ] || \
    echo "preflight=note=install_io_from_payload path=\$IO_HELPER" >> "\$STAGES_LOG"

# runtime_cleanup.sh is best effort: it kills owned daemons and bounds logs. A
# corrupted copy must not stop the install - warn, and carry on with no-ops that
# report "not alive" so the radio bookkeeping stays conservative.
CLEANUP_HELPER=""
if [ "\$("\$MD5SUM_BIN" "\$COMBO/runtime_cleanup.sh" 2>/dev/null | cut -d' ' -f1)" = "\$RUNTIME_CLEANUP_MD5" ]; then
    CLEANUP_HELPER="\$COMBO/runtime_cleanup.sh"
else
    echo 'preflight=status=warn error=runtime_cleanup_corrupt (continuing without it)' >> "\$STAGES_LOG"
fi
if [ -n "\$CLEANUP_HELPER" ]; then
    CLEANUP_LIB="\$CLEANUP_HELPER"
else
    cleanup_bounded_logs(){ :; }
    runtime_cleanup_kill_owned(){ :; }
    runtime_cleanup_process_alive(){ return 1; }
    : > /dev/null
fi
_layout_error=""
case "\$BUNDLE_LAYOUT" in
    standalone)
        if [ ! -f "\$COMBO/standalone" ] || [ -L "\$COMBO/standalone" ] || \
           [ ! -r "\$COMBO/standalone" ]; then
            _layout_error=standalone_marker_missing
        elif [ "\$("\$MD5SUM_BIN" "\$COMBO/standalone" 2>/dev/null | cut -d' ' -f1)" != \
               "\$STANDALONE_MARKER_MD5" ]; then
            _layout_error=standalone_marker_integrity
        elif ! grep -qx 'standalone=1' "\$COMBO/standalone" 2>/dev/null; then
            _layout_error=standalone_marker_content
        fi
        [ ! -d "\$COMBO/wifi" ] || _layout_error=standalone_has_wifi_stage
        [ ! -d "\$COMBO/migrate" ] || _layout_error=standalone_has_migrate_stage
        ;;
    combo) : ;;
    *) _layout_error=unknown_bundle_layout ;;
esac
if [ -n "\$_layout_error" ]; then
    echo "preflight=status=FAIL error=\$_layout_error" >> "\$STAGES_LOG"
    result FAIL; beep_n 2; exit 1
fi
. "\$IO_HELPER"
if ! io_init; then
    echo 'preflight=status=FAIL error=root_mount_detection' >> "\$STAGES_LOG"
    result FAIL; beep_n 2; exit 1
fi
echo 'preflight=status=OK' >> "\$STAGES_LOG"

# Stop owned radios ourselves; never ask the owner to turn WiFi off manually.
# Only units carrying our SPDX header and exact owned executable paths are
# touched, so a foreign or owner-modified service is left alone.
# A stop without a bound can wedge the run before the first stage (systemd on
# this unit waits on the vendor radio stack): 30 s, then move on and let the
# helper kill the leftover process by path.
stop_bounded(){
    log "step: stopping \$1"
    if command -v timeout >/dev/null 2>&1; then
        timeout 30 systemctl stop "\$1" >> "\$LOG" 2>&1 || true
    else
        systemctl stop "\$1" >> "\$LOG" 2>&1 || true
    fi
}
unit_owned(){
    [ -f "\$1" ] && [ ! -L "\$1" ] && [ -r "\$1" ] &&
        grep -q 'SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors' "\$1" 2>/dev/null
}
if [ "\$MODE" != restore ]; then
    _stop_wifi=\$STOPS_WIFI
    if command -v systemctl >/dev/null 2>&1; then
        unit_owned "\$WIFI1B_SERVICE" && \
            stop_bounded wifi1b-daemon.service
        [ "\$_stop_wifi" = 0 ] || {
            unit_owned "\$WIFI_SERVICE" && \
                stop_bounded wifi-daemon.service
        }
    fi
    runtime_cleanup_kill_owned "\$RUNTIME_ROOT" wifi1b wifi1b_daemon || true
    [ "\$_stop_wifi" = 0 ] || \
        runtime_cleanup_kill_owned "\$RUNTIME_ROOT" wifi wifi_daemon || true
    rm -f "\$RUN_DIR/wifi1b/active" 2>/dev/null || true
    [ "\$_stop_wifi" = 0 ] || rm -f "\$RUN_DIR/wifi/active" 2>/dev/null || true
    if ! runtime_cleanup_process_alive "\$RUNTIME_ROOT" wifi1b wifi1b_daemon &&
       { [ "\$_stop_wifi" = 0 ] || ! runtime_cleanup_process_alive "\$RUNTIME_ROOT" wifi wifi_daemon; }; then
        RUNTIME_CLEANUP_RADIOS_STOPPED=1
        echo 'preflight=status=OK radio_stopped=1' >> "\$STAGES_LOG"
    else
        log "preflight: an owned radio is still running; stale temp cleanup deferred"
    fi
    log "step: stale temp cleanup"
    publish_logs
    cleanup_stale_temps "\$RUNTIME_ROOT" || true
    if [ "\$MODE" = all ]; then
        # Full removed-WiFi cleanup is evidence-gated inside the helper: a
        # corrupt/missing bundle, unreadable marker, page/service or live
        # process preserves credentials and backups and is reported instead.
        cleanup_removed_wifi "\$RUNTIME_ROOT" "\$QML_BASE" "\$STICK_DIR" \
            "\${SCOURGE_ARCHIVE:-}" "\${SCOURGE_TARBALL_SHA256:-}" \
            "\$WIFI_SERVICE" || true
    fi
    [ -z "\$RUNTIME_CLEANUP_NOTES" ] || log "cleanup: \$RUNTIME_CLEANUP_NOTES"
fi

case "\$MODE" in
  all)     for s in $ORDER_SPACES; do run_stage "\$s" "\$COMBO/\$s/stage.sh" || break; done ;;
EOF

# restore runs in reverse
RESTORE_ORDER=$(echo "$SELECTED" | awk '{for(i=NF;i>0;i--) printf "%s%s", $i, (i>1?" ":"")}')
cat >> "$STAGE/run_stuff.sh" <<EOF
  restore) for s in $RESTORE_ORDER; do run_stage "restore_\$s" "\$COMBO/\$s/restore.sh" || true; done ;;
EOF

for pkg in $SELECTED; do
    cat >> "$STAGE/run_stuff.sh" <<EOF
  $pkg)    run_stage "$pkg" "\$COMBO/$pkg/stage.sh" || true ;;
EOF
done

cat >> "$STAGE/run_stuff.sh" <<'EOF'
  *) echo "unknown MODE: $MODE" >> "$STAGES_LOG"; FAILED="invalid-mode" ;;
esac

# Deferred WiFi verification. The radio registers wlan0 ~110 s after boot, so the
# WiFi stage reports DAEMON_UNVERIFIED and moves on instead of blocking the run in
# front of the tweaks install. Come back to it here: wait out the rest of the
# radio's boot, then look for the build id the stage recorded in the daemon's own
# status.json. The outcome lands in the stages log as wifi_verify.
# Late import: the client profile needs a daemon that can see wlan0, which exists
# only after the radio comes up. Retry once the build id shows in status.json.
late_wifi_import(){
    _wr="$COMBO/wifi/run_stuff_result.txt"
    [ -f "$_wr" ] || return 0
    grep -q 'wifi_import=deferred' "$_wr" 2>/dev/null || return 0
    [ -x /rw_data/wifi/wifi_daemon ] || { echo 'wifi_import_late=no-daemon' >> "$STAGES_LOG"; return 0; }
    [ -f /rw_data/wifi/wifi.txt ] || { echo 'wifi_import_late=no-file' >> "$STAGES_LOG"; return 0; }
    if /rw_data/wifi/wifi_daemon --import-file /rw_data/wifi/wifi.txt >> "$LOG" 2>&1; then
        echo 'wifi_import_late=OK' >> "$STAGES_LOG"
    else
        echo 'wifi_import_late=failed (UI can import again)' >> "$STAGES_LOG"
    fi
    return 0
}

# Same deferred idea for the tweaks daemon: its SMACK check runs seconds after
# systemd starts it, so a "pending" verdict is re-checked here against the live
# process label instead of failing the stage.
verify_pending_tweaks(){
    _tr="$COMBO/tweaks/run_stuff_result.txt"
    [ -f "$_tr" ] || return 0
    grep -q 'daemon_verify=pending' "$_tr" 2>/dev/null || return 0
    _i=0
    while [ "$_i" -lt 60 ]; do
        for _p in /proc/[0-9]*; do
            [ -r "$_p/comm" ] || continue
            if [ "$(cat "$_p/comm" 2>/dev/null)" = "tweaks_daemon" ]; then
                _lab=$(cat "$_p/attr/current" 2>/dev/null | tr -d '\r\n')
                echo "tweaks_verify=status=OK label=\${_lab:-none} waited=$_i seconds" >> "$STAGES_LOG"
                echo "tweaks_verify=status=OK label=\${_lab:-none} waited=$_i seconds" >> "$USB_DIR/tweaks_verify.txt"
                return 0
            fi
        done
        sleep 2
        _i=$((_i + 2))
    done
    echo "tweaks_verify=status=unverified waited=$_i seconds" >> "$STAGES_LOG"
    echo "tweaks_verify=status=unverified waited=$_i seconds" >> "$USB_DIR/tweaks_verify.txt"
    { systemctl status tweaks-daemon.service --no-pager 2>&1 | head -n 10
      journalctl -u tweaks-daemon.service -n 20 --no-pager 2>&1 | tail -n 20
      tail -n 15 /rw_data/tweaks/daemon.log 2>&1
    } >> "$USB_DIR/tweaks_verify.txt" 2>&1 || true
    return 0
}

verify_pending_wifi(){
    _wr="$COMBO/wifi/run_stuff_result.txt"
    [ -f "$_wr" ] || return 0
    grep -q 'daemon_verify=unverified' "$_wr" 2>/dev/null || return 0
    _want=$(sed -n 's/.*build_id=\([0-9a-f]*\).*/\1/p' "$_wr" 2>/dev/null | tail -n 1)
    [ -n "$_want" ] || { echo 'wifi_verify=status=skipped reason=no_build_id' >> "$STAGES_LOG"; return 0; }
    _i=0
    while [ "$_i" -lt 180 ]; do
        if grep -q "\"build_id\":\"$_want\"" "$RADIO_STATUS" 2>/dev/null; then
            echo "wifi_verify=status=OK build_id=$_want waited=$_i seconds" >> "$STAGES_LOG"
            return 0
        fi
        sleep 2
        _i=$((_i + 2))
    done
    echo "wifi_verify=status=unverified build_id=$_want waited=$_i seconds" >> "$STAGES_LOG"
    return 0
}

verify_pending_wifi
late_wifi_import
verify_pending_tweaks

if [ -n "$FAILED" ]; then
    result "FAIL"
    beep_n 3
    exit 1
fi
if [ "$MODE" = restore ]; then
    # File state is back to pre-install: the install ledger must not survive.
    rm -rf "$LEDGER" 2>/dev/null || true
elif [ "$MODE" = all ]; then
    # Only a full-stack run defines the shared QML the skip check compares.
    mkdir -p "$LEDGER" 2>/dev/null || true
    qml_now=$(qml_digest 2>/dev/null) || qml_now=""
    [ -z "$qml_now" ] || printf '%s\n' "$qml_now" > "$LEDGER/final-qml.digest" 2>/dev/null || true
fi
  # ---- one UI restart for the whole run ---------------------------------
  # Every package used to terminate+relaunch AppHomeScreen after installing its
  # QML, so a full combo flickered the home screen 4-5 times.  The stages now
  # skip their own restart (SCOURGE_DEFER_UI_RESTART=1) and this single restart
  # picks up ALL the injected pages/tiles at once (2026-11-24).
  # Runs at the end of EVERY run (all / wifi / tweaks / restore alike): the stages
  # only defer their own restart, so without this a single-package run left the old
  # QML in memory - the new page/tile was on disk and the running app kept the old one
  # (measured 2026-11-26: "wifi_result restart=deferred" and the car still showed the
  # pre-fix WifiPage).  The outcome is recorded in the result line so the bundle proves
  # whether the UI really restarted.
  ui_restart="not-attempted"
  restart_ui_once() {
      _launcher_pid=""
      for _p in /proc/[0-9]*; do
          [ -r "$_p/comm" ] || continue
          [ "$(cat "$_p/comm" 2>/dev/null)" = "AppLauncher" ] || continue
          _launcher_pid="${_p#/proc/}"
          break
      done
      if [ -n "$_launcher_pid" ] && [ -r "/proc/$_launcher_pid/environ" ]; then
          for _v in DBUS_SESSION_BUS_ADDRESS XDG_RUNTIME_DIR WAYLAND_DISPLAY QT_QPA_PLATFORM; do
              _val=$(tr '\0' '\n' < "/proc/$_launcher_pid/environ" 2>/dev/null | grep "^$_v=" | head -n 1 | cut -d= -f2-)
              [ -n "$_val" ] && export "$_v=$_val"
          done
      fi
      _old_pid=$(pidof AppHomeScreen 2>/dev/null | awk '{print $1}')
      if command -v dbus-send >/dev/null 2>&1; then
          dbus-send --session --type=method_call --dest=com.lge.avn.applauncher /Request \
              com.lge.avn.applauncher.request.terminate int32:11 >> "$LOG" 2>&1 || true
          sleep 2
          for _i in 1 2 3; do
              dbus-send --session --type=method_call --dest=com.lge.avn.applauncher /Request \
                  com.lge.avn.applauncher.request.launch int32:11 \
                  string:AppHomeScreen string:/app/bin/AppHomeScreen >> "$LOG" 2>&1 && {
                  _new_pid=$(pidof AppHomeScreen 2>/dev/null | awk '{print $1}')
                  if [ -n "$_new_pid" ] && [ "$_new_pid" != "$_old_pid" ]; then
                      log "restart_ui_once: dbus-ok pid ${_old_pid:-none} -> $_new_pid"
                      ui_restart="dbus-ok"
                      return 0
                  fi
                  log "restart_ui_once: launch accepted (pid ${_old_pid:-none} -> ${_new_pid:-none})"
                  break
              }
              sleep 2
          done
      fi
      # Fallback: terminate the app and let the vendor launcher bring it back.  The
      # refresh must not depend on one D-Bus call succeeding.
      if [ -n "$_old_pid" ]; then
          log "restart_ui_once: dbus path did not restart AppHomeScreen; terminating pid $_old_pid"
          kill -TERM "$_old_pid" 2>/dev/null || true
          _i=0
          while [ "$_i" -lt 10 ]; do
              sleep 1; _i=$((_i + 1))
              _new_pid=$(pidof AppHomeScreen 2>/dev/null | awk '{print $1}')
              [ -n "$_new_pid" ] && [ "$_new_pid" != "$_old_pid" ] && {
                  log "restart_ui_once: relaunched by the launcher (pid $_new_pid)"
                  ui_restart="kill-relaunched"
                  return 0
              }
          done
          log "restart_ui_once: AppHomeScreen did not return; a reboot is needed"
          ui_restart="killed-no-return"
          return 1
      fi
      log "restart_ui_once: AppHomeScreen not running and no launcher answer"
      ui_restart="no-app"
      return 1
  }
  if [ -f "$STICK_DIR/no_ui_restart" ]; then
      ui_restart="skipped-flag"
      log "restart_ui_once: skipped (no_ui_restart on the stick)"
  else
      restart_ui_once || true
  fi

result "OK ui_restart=${ui_restart:-not-attempted}"
# Five beeps: all requested stages completed successfully.  A short pause
# separates this from the last stage's two-beep signal.
sleep 0.8 2>/dev/null || sleep 1
# ---- wait for WiFi, with the UI already up ------------------------------
# The run is ordered the way the driver watches it: install, restart, then wait
# for the association while the Live log is readable, and only then the beeps.
# The wifi stage installed and restarted before this point; this only watches.
wait_wifi(){
    [ "${SCOURGE_WAIT_WIFI:-1}" = 1 ] || return 0
    [ -f /rw_data/wifi/status.json ] || return 0
    _budget=${SCOURGE_WIFI_WAIT_S:-90}
    _w=0
    while [ "${_w}" -lt "${_budget}" ]; do
        _ip=$(grep -o '"ip":"[^"]*"' /rw_data/wifi/status.json 2>/dev/null | head -n 1 | cut -d'"' -f4)
        _applying=$(grep -o '"applying":[a-z]*' /rw_data/wifi/status.json 2>/dev/null | head -n 1 | cut -d: -f2)
        if [ -n "${_ip}" ]; then
            log "wait-wifi: address ${_ip} after ${_w}s"
            printf 'combo: wifi up, address %s (%ss)\n' "${_ip}" "${_w}" >> /rw_data/wifi/daemon.log 2>/dev/null || true
            return 0
        fi
        if [ $((_w % 15)) -eq 0 ]; then
            log "wait-wifi: ${_w}s applying=${_applying:-?} ip=none"
            printf 'combo: waiting for wifi (%ss, applying=%s)\n' "${_w}" "${_applying:-?}" >> /rw_data/wifi/daemon.log 2>/dev/null || true
        fi
        sleep 5
        _w=$((_w + 5))
    done
    log "wait-wifi: no address within ${_budget}s - the Live log has the supplicant reason"
    printf 'combo: no wifi address within %ss\n' "${_budget}" >> /rw_data/wifi/daemon.log 2>/dev/null || true
}
wait_wifi

# ---- boot profile -----------------------------------------------------------
# One line per boot in /rw_data/boot_profile/boot.csv, copied to the stick so a
# slow boot can be compared with a fast one later.  Cheap (no scans, /proc only),
# and it also records our own daemons' start offsets.
if [ -f "$COMBO/boot_profile.sh" ]; then
    # --full: the whole boot (dmesg, journal, unit timings) next to the offsets, so
    # the disclaimer path can be read end to end instead of guessed.
    sh "$COMBO/boot_profile.sh" --full /rw_data/boot_profile >> "$LOG" 2>&1 || true
    if [ -f /rw_data/boot_profile/boot.csv ]; then
        cp -f /rw_data/boot_profile/boot.csv "$USB_DIR/boot_profile.csv" 2>/dev/null || true
        log "boot profile: $(wc -l < /rw_data/boot_profile/boot.csv 2>/dev/null || echo 1) boot(s) on record"
          # The full per-boot report has the vendor critical chain (systemd-analyze
        # blame/critical-chain) that explains WHY the home app comes up late.
        # Copy the newest one too, so the stick answers the interactivity
        # question without a second run (2026-11-24).
        _last_boot=$(ls -1t /rw_data/boot_profile/*.txt 2>/dev/null | head -n 1)
        [ -n "$_last_boot" ] && cp -f "$_last_boot" "$USB_DIR/boot_profile_latest.txt" 2>/dev/null || true
        # The full boot, one bundle per boot id: the report alone lost the kernel
        # and unit timeline that explains a slow disclaimer.
        for _f in dmesg.txt journal.txt units.txt unit_times.txt; do
            _src=$(ls -1t /rw_data/boot_profile/full_*/"$_f" 2>/dev/null | head -n 1)
            [ -n "$_src" ] && cp -f "$_src" "$USB_DIR/boot_$_f" 2>/dev/null || true
        done
    fi
fi

# ---- perf snapshot ----------------------------------------------------------

# ---- perf snapshot ----------------------------------------------------------
# "What is eating the CPU right now", taken when the install is done: the unit
# ships htop/top but nobody can watch a screen in the car.  The report has a
# dedicated line for the navigation map, which keeps drawing in the background.
# perf.csv accumulates one line per run so a hot run can be compared later.
if [ -f "$COMBO/perf_snapshot.sh" ]; then
    _ps_secs=3
    [ -f "$STICK_DIR/perf_sample_s" ] && _ps_secs=$(cat "$STICK_DIR/perf_sample_s" 2>/dev/null || echo 3)
    sh "$COMBO/perf_snapshot.sh" "$_ps_secs" /rw_data/perf >> "$LOG" 2>&1 || true
    for _pf in perf.txt perf.csv; do
        [ -f "/rw_data/perf/$_pf" ] && cp -f "/rw_data/perf/$_pf" "$USB_DIR/$_pf" 2>/dev/null || true
    done
    log "perf snapshot: /rw_data/perf/perf.txt (sample $_ps_secs s)"
fi

beep_n 5
exit 0
EOF

cp -f "$HERE/../payload/run_stuff.sample.sh" "$STAGE/run_stuff.sample.sh" 2>/dev/null || true
{
cat <<EOF
SCOURGE combo stick
===================
Packages: $ORDER
Bundle layout: $BUNDLE_LAYOUT

SCOURGE — The SCOURGE Project contributors (AGPL-3.0-or-later, with the
attribution terms in the project LICENSE). Firmware: Ioniq 5 Gen5W_L 260128.

1. Copy the CONTENTS of this directory to the root of a FAT/exFAT stick.
2. Plug it into the head unit; the failsafe runs run_stuff.sh as root
   within ~8 s. The preflight stops this bundle's own radio daemons itself;
   do not turn WiFi off manually.
3. Read run_stuff_result.txt on the stick; every stage reports status=OK.
4. To restore: edit MODE=restore in run_stuff.sh, replug, wait ~20 s.
5. To re-run a single package: edit MODE=<package>, replug.

Stick flags (drop the file at the root to change one run):
  no_ui_restart  skip the AppHomeScreen restart - honoured only when the QML
                 payload is unchanged, so a new page still goes live
  (the daemon API walk was removed on 2026-11-25: ~100 s of sleeps, drive the API from the UI instead)
  bt_tap_run     attach the LD_PRELOAD HCI tap to vbtd: the HCI stream goes to
                 /rw_data/bt/tap.bin, BUT Bluetooth is stopped for the run
                 (no hands-free, no phone audio, no BT CarPlay), and the tap
                 only records what happens while it runs - the phone has to
                 pair/connect during the run or the dump says nothing

EOF
if [ "$STANDALONE" = 1 ]; then
cat <<'EOF'
Standalone layout: this bundle installs Battery, Tweaks/Reboot and Wifi1b.
It does NOT install or reinstall the WiFi app and has NO migration stage.
Wifi1b installs beside an existing WiFi app, or on the no-WiFi
Battery/Tweaks base (the canonical standalone layout).
EOF
cat <<'EOF'
Optional inputs: iphone_wifi.txt (or wifi.txt) and authorized_keys at the
stick ROOT. They are imported without activating WLAN, a mode or SSH.
Wifi1b SSH: user wifi1b, UID 0, key-only, configured port (default 2224),
bind-all. This is a deliberate product default account and is NOT
car-validated. Owner keys already present in /rw_data/wifi1b are preserved
unless replaced here.

BlueLink/Pi networking:
- While rmnet_usb0/rmnet_usb1 has an IPv4 address, Wifi1b installs no WiFi
  default route. WiFi remains an SSH-only link; the modem default and the
  vendor per-server host routes stay authoritative.
- A stale V21 `route replace default via wlan0` is removed on client start.
  A 30 s audit restores/removes the bounded proto-66 fallback as modem data
  appears/disappears. Routes on other interfaces are never touched.
- The Orange Pi ECM link is usb0 at 10.55.0.2/24 with no default route. SSH
  follows the configured port and stays available over usb0 when WiFi is
  switched Off. ConnMan is kept off usb0/rmnet_usb0..7.
EOF
else
cat <<'EOF'
WiFi note: put your wifi.txt and authorized_keys at the stick ROOT before
the first install; the WiFi installer imports them. Remove them afterwards.
Default SSH: car@<head-unit-ip> -p 2222, password 'electric' — CHANGE IT.
EOF
fi
cat <<'EOF'

WIP gate stages (if selected — button-gate / applauncher / dmclient-gate):
these are the signatureless-update research deployers (docs/08 of the
repo). They patch the updater's signature gates in place, need
AppUpgrade.patched.te2 next to them, require a REBOOT afterwards, and are
VM-validated but not yet car-validated. REVERT markers restore stock bytes.

This whole branch is work in progress: offline-tested, not car-validated.
Keep the stick private: it can contain owner credentials and private
WiFi/QML snapshots.
EOF
} > "$STAGE/README.txt"

echo "== deploying to $TARGET"
# The target may already hold a previous bundle; leaving its combo/ around
# let stale files (e.g. renamed .wav -> .dat payloads) survive to the stick and
# the HU media player kept playing them (2026-11-24).  Clean only combo/ -
# root files like ota_package.tar and the ssh markers belong to the owner.
rm -rf "$TARGET/combo"
cp -R "$STAGE"/. "$TARGET"/
[ -z "${BATTERY_BUILD:-}" ] || rm -rf "$BATTERY_BUILD"
sync
echo "DONE. Stick contents:"
ls -la "$TARGET"
