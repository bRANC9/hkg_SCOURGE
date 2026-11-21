#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Build a single-purpose USB stick that installs ONLY the AppStandbyClock
# safety-disclaimer QML patch (main.qml + both Confirm screens, per GUI base).
#
# Why a separate stick: the full Tweaks payload needs the owner stock QML
# bases (packages/*/base, gitignored) to patch the home screen. This stick
# needs none of that - it ships the generated AppStandbyClock files plus the
# stock originals for the restore path.
#
# Usage: sh scripts/build_disclaimer_stick.sh /path/to/mounted/stick
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
TARGET="${1:?usage: build_disclaimer_stick.sh /path/to/mounted/stick}"
ROOTFS="${SCOURGE_ROOTFS:-$ROOT/../../work/rootfs}"
STANDBY_SRC="$ROOTFS/app/share/AppStandbyClock"
BASES="qml_eco3.0 new_qml qml_genesis"
FILES="main.qml Screen_AppStandbyClock_Confirm.qml Screen_AppStandbyClock_Confirm_Profile.qml"

[ -d "$TARGET" ] || { echo "not a directory: $TARGET" >&2; exit 1; }
[ "$TARGET" != "/" ] || { echo "refusing to deploy to /" >&2; exit 1; }
[ -d "$STANDBY_SRC" ] || { echo "missing stock AppStandbyClock: $STANDBY_SRC" >&2; exit 1; }

echo "== AppStandbyClock artifacts =="
SCOURGE_ROOTFS="$ROOTFS" python3 - "$ROOT" <<'PY'
import importlib.util
import os
import sys

root = sys.argv[1]
spec = importlib.util.spec_from_file_location(
    "build_artifacts", os.path.join(root, "scripts", "build_artifacts.py"))
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
mod.build_standbyclock()
PY

echo "== boot message .qm =="
LOCALIZATION="$ROOTFS/app/share/AppStandbyClock/localization"
[ -d "$LOCALIZATION/qm" ] || { echo "missing stock localization: $LOCALIZATION" >&2; exit 1; }
BOOTMSG_ID=""

echo "== stage =="
STAGE=$(mktemp -d "${TMPDIR:-/tmp}/scourge_disclaimer.XXXXXX")
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/payload/qml/AppStandbyClock" "$STAGE/payload/lib" \
         "$STAGE/payload/bootmsg" "$STAGE/payload/tools" "$STAGE/backup/AppStandbyClock"
cp -R "$ROOT/artifacts/qml/AppStandbyClock/." "$STAGE/payload/qml/AppStandbyClock/"
cp "$ROOT/../common/install_io.sh" "$STAGE/payload/lib/"

# Boot-message translations: the same patcher the flashed image uses, run in
# message-only mode against the stock localization tree. Installed on the car
# by the proven tools/deploy_boot_messages.sh (hash+size verified, backed up,
# atomic rename, rollback), shipped verbatim on this stick.
python3 "$ROOT/../../scripts/03_patch_payload.py" \
    --stock-qm-dir "$LOCALIZATION" \
    --messages "$ROOT/../../config/messages.json" \
    --out "$STAGE/payload/bootmsg" >/dev/null
( cd "$STAGE/payload/bootmsg" && sha256sum translation_*.qm > MANIFEST.sha256 )
BOOTMSG_ID=$(ls "$STAGE/payload/bootmsg"/translation_*.qm | wc -l)
[ "$BOOTMSG_ID" -gt 0 ] || { echo "no boot message .qm staged" >&2; exit 1; }
cp "$ROOT/../../tools/deploy_boot_messages.sh" "$STAGE/payload/tools/"
chmod 755 "$STAGE/payload/tools/deploy_boot_messages.sh"
echo "staged $BOOTMSG_ID boot-message .qm file(s)"

# Stock reference for the restore path: identical firmware, so these bytes are
# what a stock 260128 unit carries.
for base in $BASES; do
    for f in $FILES; do
        src="$STANDBY_SRC/$base/$f"
        [ -f "$src" ] || continue
        mkdir -p "$STAGE/backup/AppStandbyClock/$base"
        cp -f "$src" "$STAGE/backup/AppStandbyClock/$base/$f"
    done
done

cp "$HERE/run_stuff_disclaimer.sh" "$STAGE/run_stuff.sh"
cp "$HERE/run_stuff_disclaimer_restore.sh" "$STAGE/run_stuff_restore.sh"
chmod 755 "$STAGE/run_stuff.sh" "$STAGE/run_stuff_restore.sh"

cat > "$STAGE/README.txt" <<'EOF'
SCOURGE - AppStandbyClock safety-disclaimer patch only

What it does
  Replaces the AppStandbyClock QML that draws the boot safety disclaimer:
  main.qml (touch guard) and both Confirm screens (ungated Confirm button +
  queued click delivery) for every GUI base present on the unit.
  Installs the patched boot-message translations (localization/qm/*.qm) with
  the proven deploy_boot_messages.sh: hash and size verified against the live
  files first, backed up, atomic rename, rollback on any failure.
  Everything takes effect on the NEXT boot.

What it does NOT do
  No home-screen QML, no daemon, no service, no wrandr, no updater, no
  firmware. Nothing is restarted.

Install
  Plug the stick in; the initial-root failsafe runs run_stuff.sh as root.
  Result: run_stuff_result.txt on the stick (status=OK, disclaimer=next-boot).
  Audible feedback through the factory micom beeper: 1 beep = finished,
  3 beeps = failed (both non-fatal; the result file is authoritative).

Restore
  Copy run_stuff_restore.sh over run_stuff.sh on the stick and plug it in.
  It restores the newest on-car backup, else the stock reference shipped in
  backup/AppStandbyClock.

Diagnostics
  /rw_data/tweaks/disclaimer_install.log, /rw_data/tweaks/disclaimer_status.txt
EOF

( cd "$STAGE" && find payload -type f -print0 | sort -z | xargs -0 md5sum > manifest.md5 )
DISCLAIMER_ID=$(md5sum "$STAGE/manifest.md5" | cut -d' ' -f1 | cut -c1-16)
for f in "$STAGE/run_stuff.sh" "$STAGE/run_stuff_restore.sh"; do
    sed -i "s/^# TWEAKS_DISCLAIMER_ID=.*/# TWEAKS_DISCLAIMER_ID=$DISCLAIMER_ID/" "$f"
done

echo "== write $TARGET =="
cp -R "$STAGE"/. "$TARGET"/
( cd "$TARGET" && md5sum -c manifest.md5 >/dev/null )
echo "disclaimer stick ready: $TARGET (id=$DISCLAIMER_ID)"
