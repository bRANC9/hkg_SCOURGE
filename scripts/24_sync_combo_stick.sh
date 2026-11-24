#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# 24_sync_combo_stick.sh — put a built combo bundle on a stick and PROVE it landed.
#
#   scripts/24_sync_combo_stick.sh [--payload out/firstround] [--label RIGIMG]
#                                  [--standby] [--verify-only]
#
# Why this exists: a plain `cp` followed by `md5sum -c` lies. The verification
# reads through the page cache, so bytes that never reached the medium look fine.
# On 2026-11-22 that cost three car runs: the helpers in combo/ and the tiny
# combo/preflight.md5 arrived corrupted, and the unit failed closed before any
# stage ran ("cleanup_helper_integrity"). The only honest check is copy -> sync ->
# unmount -> remount -> verify, and re-copy whatever came back wrong.
#
# The verification here mirrors what the unit actually gates on:
#   * each package's manifest.md5 (payload + backup)
#   * the two combo helpers, against the hashes baked into the master
#   * the root manifest.md5, when present
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

PAYLOAD="$ROOT/out/firstround"
LABEL="RIGIMG"
STANDBY=1
VERIFY_ONLY=0
MOUNT_OVERRIDE=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        --payload) PAYLOAD="${2:?--payload needs a directory}"; shift 2 ;;
        --label)   LABEL="${2:?--label needs a filesystem label}"; shift 2 ;;
        --standby) STANDBY=0; shift ;;
        --verify-only) VERIFY_ONLY=1; shift ;;
        --mount)   MOUNT_OVERRIDE="${2:?--mount needs a directory}"; shift 2 ;;
        -h|--help) sed -n '4,8p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
[ -d "$PAYLOAD" ] || { echo "no payload at $PAYLOAD (run packages/compose.sh first)" >&2; exit 1; }
[ -f "$PAYLOAD/run_stuff.sh" ] || { echo "$PAYLOAD/run_stuff.sh missing" >&2; exit 1; }

udev() { lsblk -pnro NAME,LABEL 2>/dev/null | awk -v l="$LABEL" '$2==l {print $1; exit}'; }

MOUNT=""
mount_stick() {
    local dev
    if [ -n "$MOUNT_OVERRIDE" ]; then
        MOUNT="$MOUNT_OVERRIDE"
        [ -d "$MOUNT" ] || { echo "no such directory: $MOUNT" >&2; exit 1; }
        return 0
    fi
    dev="$(udev)"
    [ -n "$dev" ] || { echo "no device carrying the label $LABEL is attached" >&2; exit 1; }
    MOUNT="$(lsblk -pnro MOUNTPOINT "$dev" 2>/dev/null | head -n1)"
    if [ -z "$MOUNT" ]; then
        udisksctl mount -b "$dev" >/dev/null
        MOUNT="$(lsblk -pnro MOUNTPOINT "$dev" 2>/dev/null | head -n1)"
    fi
    [ -n "$MOUNT" ] || { echo "could not mount $dev" >&2; exit 1; }
}
unmount_stick() {
    local dev
    [ -z "$MOUNT_OVERRIDE" ] || return 0
    dev="$(udev)"
    sync
    udisksctl unmount -b "$dev" >/dev/null 2>&1 || true
}
remount_stick() {
    [ -z "$MOUNT_OVERRIDE" ] || return 0
    unmount_stick
    sleep 1
    MOUNT=""
    mount_stick
}

copy_payload() {
    mount_stick
    echo "== copying $PAYLOAD -> $MOUNT"
    # Update in place, writing only the files whose bytes differ.  Every sync used
    # to delete and rewrite all ~770 files (~12 MB of tiny writes) on FAT: that
    # dominated the wall time and left the flash hot (2026-11-23).  Correctness is
    # unchanged - the per-package manifests are still verified after the remount,
    # so a file that was skipped but is wrong fails the check and is re-copied.
    local copied=0 kept=0 dropped=0
    if [ -d "$MOUNT/combo" ]; then
        while IFS= read -r -d '' f; do
            src="$PAYLOAD/combo/$f"
            dst="$MOUNT/combo/$f"
            if [ -f "$dst" ] && [ "$(stat -c %s "$src")" = "$(stat -c %s "$dst")" ] \
               && cmp -s "$src" "$dst"; then
                kept=$((kept + 1))
                continue
            fi
            mkdir -p "$(dirname "$dst")"
            cp -f "$src" "$dst" || return 1
            copied=$((copied + 1))
        done < <( cd "$PAYLOAD/combo" && find . -type f -print0 )
        # A stage the payload no longer ships must not stay behind: it would still
        # be executable (the master runs whatever combo/<name>/stage.sh exists).
        while IFS= read -r -d '' f; do
            [ -f "$PAYLOAD/combo/$f" ] || { rm -f "$MOUNT/combo/$f"; dropped=$((dropped + 1)); }
        done < <( cd "$MOUNT/combo" && find . -type f -print0 )
        echo "   in-place: $copied written, $kept unchanged, $dropped removed"
    else
        cp -R "$PAYLOAD/combo" "$MOUNT/"
        echo "   fresh copy of combo/"
    fi
    cp -f "$PAYLOAD/run_stuff.sh" "$MOUNT/run_stuff.sh"
    # The loader md5-gates per file: an unchanged run_stuff.sh makes the next plug
    # do NOTHING at all - no beep, no result, no log line. Every copy therefore
    # carries a fresh stamp so the master always differs from the last run's.
    printf '\n# sync stamp: %s\n' "$(date -u +%Y%m%dT%H%M%SZ)" >> "$MOUNT/run_stuff.sh"
    [ -f "$PAYLOAD/README.txt" ] && cp -f "$PAYLOAD/README.txt" "$MOUNT/README.combo.txt"
    for wav in "$ROOT"/out/dumpstick/beep-start.dat "$ROOT"/out/dumpstick/beep-done.dat; do
        [ -f "$wav" ] && cp -f "$wav" "$MOUNT/"
    done
    [ "$STANDBY" = 0 ] || : > "$MOUNT/combo/tweaks/no-standby"
    # A lock left behind by an interrupted run makes the next plug do nothing at
    # all (the master exits silently). The payload just replaced everything, so no
    # run can legitimately hold it here.
    rm -rf "$MOUNT/.scourge-run"
    chmod +x "$MOUNT/run_stuff.sh"
    sync
}

verify() {   # prints the bad entries, one per line, as "<pkg>/<path>" or "combo/<path>"
    local dev out bad=""
    dev="$(udev)"
    MOUNT="$(lsblk -pnro MOUNTPOINT "$dev" 2>/dev/null | head -n1)"
    [ -n "$MOUNT" ] || return 1
    for pkg in battery wifi tweaks migrate ambient wifi1b; do
        [ -d "$MOUNT/combo/$pkg" ] || continue
        out="$( cd "$MOUNT/combo/$pkg" && md5sum -c manifest.md5 2>/dev/null | grep -v ': OK$' | sed 's/: FAILED$//' || true )"
        [ -z "$out" ] || bad="$bad $(printf '%s\n' "$out" | sed "s|^|$pkg/|")"
    done
    local want_io want_rc got_io got_rc
    want_io="$(sed -n 's/^INSTALL_IO_MD5="\(.*\)"/\1/p' "$MOUNT/run_stuff.sh" | head -n1)"
    want_rc="$(sed -n 's/^RUNTIME_CLEANUP_MD5="\(.*\)"/\1/p' "$MOUNT/run_stuff.sh" | head -n1)"
    got_io="$(md5sum "$MOUNT/combo/install_io.sh" 2>/dev/null | cut -d' ' -f1)"
    got_rc="$(md5sum "$MOUNT/combo/runtime_cleanup.sh" 2>/dev/null | cut -d' ' -f1)"
    [ -z "$want_io" ] || [ "$want_io" = "$got_io" ] || bad="$bad combo/install_io.sh"
    [ -z "$want_rc" ] || [ "$want_rc" = "$got_rc" ] || bad="$bad combo/runtime_cleanup.sh"
    # The beep files are not in any manifest, and a missing or corrupted tone is
    # silent rather than loud: check them by their local hashes.
    for wav in "$ROOT"/out/dumpstick/beep-start.dat "$ROOT"/out/dumpstick/beep-done.dat; do
        [ -f "$wav" ] || continue
        name="$(basename "$wav")"
        want="$(md5sum "$wav" | cut -d' ' -f1)"
        got="$(md5sum "$MOUNT/$name" 2>/dev/null | cut -d' ' -f1)"
        [ "$want" = "$got" ] || bad="$bad $name"
    done
    if [ -f "$MOUNT/combo/preflight.md5" ]; then
        out="$( cd "$MOUNT" && md5sum -c combo/preflight.md5 2>/dev/null | grep -v ': OK$' | sed 's/: FAILED$//' || true )"
        [ -z "$out" ] || bad="$bad $(printf '%s\n' "$out" | sed 's|^|  |')"
    fi
    printf '%s' "$bad"
}

recopy() {   # recopy <pkg/path ...>
    local f pkg rel
    for f in "$@"; do
        case "$f" in
            combo/*) cp -f "$PAYLOAD/$f" "$MOUNT/$f" ;;
            beep-*.dat) cp -f "$ROOT/out/dumpstick/$f" "$MOUNT/$f" ;;
            *) pkg="${f%%/*}"; rel="${f#*/}"
               mkdir -p "$MOUNT/combo/$pkg/$(dirname "$rel")"
               cp -f "$PAYLOAD/combo/$pkg/$rel" "$MOUNT/combo/$pkg/$rel" ;;
        esac
    done
    sync
}

if [ "$VERIFY_ONLY" = 0 ]; then
    copy_payload
fi

round=1
while [ "$round" -le 6 ]; do
    remount_stick
    bad="$(verify)"
    bad="$(echo "$bad" | tr ' ' '\n' | sed '/^$/d')"
    if [ -z "$bad" ]; then
        echo "== verified after remount: clean (round $round)"
        echo "   master $(md5sum "$MOUNT/run_stuff.sh" | cut -c1-16)  mode $(grep -m1 -o 'SCOURGE_MODE:-[a-z]*' "$MOUNT/run_stuff.sh" | cut -d: -f2)"
        unmount_stick
        echo "== unmounted; safe to pull"
        exit 0
    fi
    echo "== round $round: $(echo "$bad" | wc -l) file(s) did not survive the medium, re-copying"
    echo "$bad" | sed 's/^/   /'
    recopy $bad
    round=$((round + 1))
done

echo "== still bad after 6 rounds: this stick is not holding the data." >&2
echo "   Pull it, run --verify-only, and use another stick if it stays bad." >&2
unmount_stick
exit 1
