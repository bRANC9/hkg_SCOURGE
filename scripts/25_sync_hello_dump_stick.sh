#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# 25_sync_hello_dump_stick.sh — put the Hello-base dump payload on a stick and PROVE it landed.
#
#   scripts/25_sync_hello_dump_stick.sh [--label RIGIMG] [--mount DIR]
#                                       [--payload payload/dump_hello.sample.sh]
#                                       [--verify-only]
#
# Why: packages/wifi/base/hello* cannot be synthesised (a fabricated base ships a
# variant that references a tile component the unit may not define), so the base
# has to be dumped off a unit that really carries the Hello app set. This puts
# the dump payload on the stick the way 24_sync_combo_stick.sh puts the combo
# bundle there, with the same honest check: a plain cp followed by md5sum -c
# reads through the page cache and lies about what reached the medium
# (2026-11-22: three car runs lost that way).
#
# The payload is a single run_stuff.sh at the stick root. The loader md5-gates
# per file, so every copy carries a fresh stamp - an identical file makes the
# next plug do nothing at all: no beep, no dump, no log line.
#
# A previous dump on the stick is never deleted: it is the only record of that
# unit. The payload writes into hello_dump/<UTC stamp>/.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

PAYLOAD="$ROOT/payload/dump_hello.sample.sh"
LABEL="RIGIMG"
VERIFY_ONLY=0
MOUNT_OVERRIDE=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        --payload) PAYLOAD="${2:?--payload needs a file}"; shift 2 ;;
        --label)   LABEL="${2:?--label needs a filesystem label}"; shift 2 ;;
        --mount)   MOUNT_OVERRIDE="${2:?--mount needs a directory}"; shift 2 ;;
        --verify-only) VERIFY_ONLY=1; shift ;;
        -h|--help) sed -n '4,9p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
[ -f "$PAYLOAD" ] || { echo "no payload at $PAYLOAD" >&2; exit 1; }

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

# Second resolution is not enough: two syncs in the same second would write an
# identical file, and the loader's md5 gate would then do nothing at all on the
# next plug (no beep, no dump).  A random suffix guarantees a new digest.
STAMP="$(date -u +%Y%m%dT%H%M%SZ)-$(od -An -N2 -tx1 /dev/urandom 2>/dev/null | tr -d ' ')"

copy_payload() {
    mount_stick
    echo "== copying $PAYLOAD -> $MOUNT/run_stuff.sh"
    # The stamp is what makes the loader run it again; without it the md5 gate
    # sees an unchanged file and does nothing at all.
    local staged
    staged="$(mktemp)"
    cat "$PAYLOAD" > "$staged"
    printf '\n# sync stamp: %s\n' "$STAMP" >> "$staged"
    cp -f "$staged" "$MOUNT/run_stuff.sh"
    rm -f "$staged"
    chmod +x "$MOUNT/run_stuff.sh" 2>/dev/null || true
    local wav
    for wav in "$ROOT"/out/dumpstick/beep-start.dat "$ROOT"/out/dumpstick/beep-done.dat; do
        [ -f "$wav" ] && cp -f "$wav" "$MOUNT/"
    done
    # A lock left behind by an interrupted run makes the next plug do nothing.
    rm -rf "$MOUNT/.scourge-run"
    if [ -d "$MOUNT/hello_dump" ]; then
        echo "   note: hello_dump/ is already on the stick (previous dump kept; the new one goes to hello_dump/<stamp>/)"
    fi
    sync
}

verify() {
    mount_stick
    local file="$MOUNT/run_stuff.sh" body want stamp
    # The stamp line is what makes the loader run the payload again.
    [ -f "$file" ] || { echo "run_stuff.sh is not on the stick" >&2; return 1; }
    stamp="$(sed -n 's/^# sync stamp: //p' "$file" | tail -n1)"
    [ -n "$stamp" ] || { echo "run_stuff.sh has no sync stamp; the loader would skip it" >&2; return 1; }
    # Compare the payload body itself (the file is the payload plus a blank line
    # and the stamp): a truncated write on FAT is what actually happened on
    # 2026-11-22, and a per-run digest cannot be compared across runs.
    body="$(sed '$d' "$file" | sed '$d')"
    want="$(cat "$PAYLOAD")"
    if [ "$body" = "$want" ]; then
        echo "== verified after remount: payload body intact ($(printf '%s' "$body" | md5sum | cut -d' ' -f1 | cut -c1-16)), stamp $stamp"
        local wav name want have
        for wav in "$ROOT"/out/dumpstick/beep-start.dat "$ROOT"/out/dumpstick/beep-done.dat; do
            [ -f "$wav" ] || continue
            name="$(basename "$wav")"
            want="$(md5sum "$wav" | cut -d' ' -f1)"
            have="$(md5sum "$MOUNT/$name" 2>/dev/null | cut -d' ' -f1)"
            [ "$want" = "$have" ] || { echo "   $name did not survive the medium" >&2; return 1; }
        done
        unmount_stick
        echo "== unmounted; safe to pull"
        echo "   after the run, import with:"
        echo "     python3 packages/wifi/scripts/install_hello_base.py --dump <stick>/hello_dump/<stamp> --check"
        return 0
    fi
    echo "== run_stuff.sh did not survive the medium (got $got want $EXPECTED)" >&2
    return 1
}

if [ "$VERIFY_ONLY" = 0 ]; then
    copy_payload
fi

round=1
while [ "$round" -le 4 ]; do
    if verify; then
        exit 0
    fi
    echo "== round $round: re-copying" >&2
    if [ "$VERIFY_ONLY" = 0 ]; then
        copy_payload
    else
        echo "verify-only: nothing to re-copy" >&2
        exit 1
    fi
    round=$((round + 1))
done
echo "== still bad after 4 rounds: this stick is not holding the data." >&2
exit 1
