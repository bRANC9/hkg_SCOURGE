#!/bin/sh
# Pin the compiler: the host glibc is x86-64-v3 and any -static glibc build
# faults on the head unit (Atom-class CPU). musl-gcc keeps the baseline ISA.
MUSL_CC="$(command -v musl-gcc 2>/dev/null || true)"

# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Ioniq 5 Gen5W_L EU 260128
# Build a complete USB payload tree for the WiFi app.
# Usage: sh scripts/build_payload.sh [output_dir]
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
OUT=${1:-"$ROOT/out/usb"}
DROPBEAR="${SCOURGE_DROPBEAR:-$ROOT/bin/dropbearmulti}"
SCOURGE_ROOTFS="${SCOURGE_ROOTFS:-$ROOT/../../work/rootfs}"
export SCOURGE_ROOTFS
ROOTFS="$SCOURGE_ROOTFS/app/share/AppHomeScreen/qml_eco3.0"

cd "$ROOT"

echo "== icons =="
python3 scripts/make_icons.py

echo "== QML artifacts =="
python3 scripts/build_artifacts.py

echo "== build daemon =="
make -C src CC="${MUSL_CC:-cc}"

echo "== assemble $OUT =="
rm -rf "$OUT"
mkdir -p "$OUT/payload/facts" "$OUT/payload/bin" "$OUT/payload/assets/wifi" "$OUT/payload/assets/hello" "$OUT/payload/assets/battery" "$OUT/payload/systemd" \
         "$OUT/payload/ref" "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/HomeScreen" \
         "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/component/common" \
         "$OUT/backup/AppHomeScreen/qml_eco3.0/HomeScreen" \
         "$OUT/backup/AppHomeScreen/qml_eco3.0/component/common"
# AA cross-check: the BT socket-ownership probe from packages/aa, so the
# install-time probe can report who owns SDP / RFCOMM 8 on this unit.
if [ -x "$HERE/../../aa/bt_owner.target" ]; then
    cp -f "$HERE/../../aa/bt_owner.target" "$OUT/payload/bin/bt_owner.target"
else
    echo "note: packages/aa/bt_owner.target not built (make -C packages/aa target-bt-owner)" >&2
fi
# AA cross-check: does the vendor Bluetooth stack *export* the SDP/RFCOMM calls
# the bootstrap needs?  The probe reads the running daemon's .dynsym and writes
# /rw_data/aa/bt_api.json, which the status JSON carries as sdp_api/rfcomm_api.
if [ -x "$HERE/../../aa/vbtd_api.target" ]; then
    cp -f "$HERE/../../aa/vbtd_api.target" "$OUT/payload/bin/vbtd_api.target"
else
    echo "note: packages/aa/vbtd_api.target not built (make -C packages/aa target-vbtd-api)" >&2
fi
mkdir -p "$OUT/payload/lib" "$OUT/payload/firmware"
# Signed regulatory database: without it cfg80211 stays on "country 00" (world),
# which forbids beaconing on every 5 GHz channel, and the 5 GHz AP - the only band
# wireless Android Auto accepts - cannot start.  Fetched, never hand-made: the
# signature is verified against the key compiled into the kernel.
if python3 scripts/fetch_regdb.py; then
    cp -f build/regdb/regulatory.db build/regdb/regulatory.db.p7s "$OUT/payload/firmware/" \
        && cp -f build/regdb/version.txt "$OUT/payload/firmware/regdb-version.txt"
else
    echo "WARNING: regulatory.db not available; the 5 GHz AP stays blocked (2.4 GHz is unaffected)" >&2
fi
[ -f "$HERE/artifacts/facts/hwinfo.txt" ] && cp -f "$HERE/artifacts/facts/hwinfo.txt" "$OUT/payload/facts/hwinfo.txt" || true
cp "$ROOT/../common/install_io.sh" "$OUT/payload/lib/"

cp src/wifi_daemon "$OUT/payload/bin/wifi_daemon"
# CRDA stand-in: 4.1 asks userspace for the regdomain and this image has no
# CRDA, so nothing ever moves cfg80211 off country 00 (world).
cp src/regd "$OUT/payload/bin/regd"
cp artifacts/assets/wifi/*.png "$OUT/payload/assets/wifi/"
# The battery/hello assets are NOT installed by this stage (the battery stage
# owns them), so shipping stale blue copies here only confused the audit.  Drop
# them (2026-11-24: unified mint/glow set).
cp artifacts/systemd/wifi-daemon.service "$OUT/payload/systemd/"
cp artifacts/systemd/regd.service "$OUT/payload/systemd/"
cp artifacts/qml/AppHomeScreen/qml_eco3.0/HomeScreen/WifiPage.qml "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/HomeScreen/"
cp artifacts/qml/AppHomeScreen/qml_eco3.0/component/WifiTile.qml "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/component/"
cp artifacts/qml/AppHomeScreen/qml_eco3.0/component/WifiSwitch.qml "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/component/"
cp artifacts/qml/AppHomeScreen/qml_eco3.0/component/WifiButton.qml "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/component/"
cp artifacts/qml/AppHomeScreen/qml_eco3.0/component/HelloTile.qml "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/component/" 2>/dev/null || true
cp artifacts/qml/AppHomeScreen/qml_eco3.0/component/BatteryTile.qml "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/component/" 2>/dev/null || true
cp artifacts/qml/AppHomeScreen/qml_eco3.0/component/common/Q_ThemeImage.qml "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/component/common/"
cp artifacts/qml/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Advanced.*.qml "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/HomeScreen/"
cp artifacts/qml/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Wide.*.qml "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/HomeScreen/"
cp artifacts/qml/AppHomeScreen/qml_eco3.0/HomeScreen/Screen_HomeScreen_AllMenus.*.qml "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/HomeScreen/"
cp artifacts/qml/AppHomeScreen/qml_eco3.0/HomeScreen/Screen_HomeScreen_Submenu_Setup.*.qml "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/HomeScreen/"
cp artifacts/qml/AppHomeScreen/qml_eco3.0/component/QCustomizedMenuListAllowEmpty.*.qml "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/component/"

if [ ! -f "$DROPBEAR" ]; then
    sh scripts/build_dropbear.sh
fi
cp "$DROPBEAR" "$OUT/payload/bin/dropbearmulti"
# Pre-patched vendor mdnsd (setuid NOPed so it continues as root; see
# scripts/run_stuff.sh "mdnsd continue as root" section).  Applied on the
# car only when the MDNSD_ROOT_ARM marker is present.
python3 scripts/patch_mdnsd.py "$SCOURGE_ROOTFS/app/bin/mdnsd" "$OUT/payload/bin/mdnsd.rooted"
chmod 755 "$OUT/payload/bin/wifi_daemon" "$OUT/payload/bin/regd" "$OUT/payload/bin/dropbearmulti" "$OUT/payload/bin/mdnsd.rooted"

cp wifi.txt.sample "$OUT/payload/wifi.txt.sample"
cp wifi.txt.sample "$OUT/README-wifi.txt"

# Reference hashes used by the installer to recognise stock/Hello bases.
md5_of() { md5sum "$1" | cut -d' ' -f1; }
echo "$(md5_of base/stock/HomeScreen/QHMI_HomeScreenMain_Advanced.qml)" > "$OUT/payload/ref/stock_advanced.md5"
echo "$(md5_of base/hello/HomeScreen/QHMI_HomeScreenMain_Advanced.qml)" > "$OUT/payload/ref/hello_advanced.md5"
echo "$(md5_of base/stock/HomeScreen/QHMI_HomeScreenMain_Wide.qml)" > "$OUT/payload/ref/stock_wide.md5"
echo "$(md5_of base/hello/HomeScreen/QHMI_HomeScreenMain_Wide.qml)" > "$OUT/payload/ref/hello_wide.md5"
echo "$(md5_of base/stock/HomeScreen/Screen_HomeScreen_AllMenus.qml)" > "$OUT/payload/ref/stock_allmenus.md5"
echo "$(md5_of base/hello/HomeScreen/Screen_HomeScreen_AllMenus.qml)" > "$OUT/payload/ref/hello_allmenus.md5"
echo "$(md5_of base/stock/HomeScreen/Screen_HomeScreen_Submenu_Setup.qml)" > "$OUT/payload/ref/stock_setup.md5"
echo "$(md5_of base/hello/HomeScreen/Screen_HomeScreen_Submenu_Setup.qml)" > "$OUT/payload/ref/hello_setup.md5"
echo "$(md5_of base/stock/component/QCustomizedMenuListAllowEmpty.qml)" > "$OUT/payload/ref/stock_custom.md5"
echo "$(md5_of base/hello/component/QCustomizedMenuListAllowEmpty.qml)" > "$OUT/payload/ref/hello_custom.md5"
echo "$(md5_of "$ROOTFS/component/common/Q_ThemeImage.qml")" > "$OUT/payload/ref/stock_theme.md5"

# Reference originals for the USB backup directory (not used by the installer,
# but useful for manual recovery/diffing).
cp base/stock/HomeScreen/QHMI_HomeScreenMain_Advanced.qml "$OUT/backup/AppHomeScreen/qml_eco3.0/HomeScreen/"
cp base/stock/HomeScreen/QHMI_HomeScreenMain_Wide.qml "$OUT/backup/AppHomeScreen/qml_eco3.0/HomeScreen/"
cp base/stock/HomeScreen/Screen_HomeScreen_AllMenus.qml "$OUT/backup/AppHomeScreen/qml_eco3.0/HomeScreen/"
cp base/stock/HomeScreen/Screen_HomeScreen_Submenu_Setup.qml "$OUT/backup/AppHomeScreen/qml_eco3.0/HomeScreen/"
cp base/stock/component/QCustomizedMenuListAllowEmpty.qml "$OUT/backup/AppHomeScreen/qml_eco3.0/component/"
cp "$ROOTFS/component/common/Q_ThemeImage.qml" "$OUT/backup/AppHomeScreen/qml_eco3.0/component/common/"

cp scripts/run_stuff.sh "$OUT/run_stuff.sh"
cp scripts/run_stuff_restore.sh "$OUT/run_stuff_restore.sh"
cp scripts/run_stuff_diag.sh "$OUT/run_stuff_diag.sh"
cp scripts/run_stuff_apdiag.sh "$OUT/run_stuff_apdiag.sh"
chmod 755 "$OUT/run_stuff.sh" "$OUT/run_stuff_restore.sh" "$OUT/run_stuff_diag.sh" "$OUT/run_stuff_apdiag.sh"

cat > "$OUT/README.txt" <<'EOF'
Ioniq 5 Gen5W WiFi / CarPlay / SSH app — USB payload

Install:
  1. Copy the whole contents of this USB tree to a FAT/exFAT stick.
  2. (Optional) Put your own wifi.txt at the stick root.
     (Optional) Put authorized_keys at the stick root.
  3. Plug the stick into the head unit; the initial_root failsafe runs
     run_stuff.sh as root within ~8 seconds.
  4. Check run_stuff_result.txt on the stick.

The app adds a WiFi tile to the home screen / All Menus / Setup and a
full-screen WiFi page.  Credentials are imported from wifi.txt with the
"Import USB" button.  SSH user is car / electric (change it afterwards).

Restore:
  Copy run_stuff_restore.sh over run_stuff.sh on the stick and plug it in.

Diagnostics:
  Copy run_stuff_diag.sh over run_stuff.sh on the stick and plug it in;
   it writes wifi_diag.txt to the stick root (read-only).
  Optional run_stuff_apdiag.sh records a bounded, read-only AP/D-Bus header
  trace. It never enables or restarts WLAN. No message bodies are captured.

This does not flash firmware and does not touch the updater, bootloader or
partitions.
EOF

# Package identity is written before the manifest so it is gated too. The ZIP
# is regenerated only after its tree (including the manifest) is complete.
python3 scripts/package_release.py "$OUT" --metadata-only
# Manifest covers payload/ only; the loader md5-gates run_stuff.sh itself.
( cd "$OUT" && find payload -type f -print0 | sort -z | xargs -0 md5sum > manifest.md5 )
python3 scripts/package_release.py "$OUT"

echo "== done =="
echo "USB tree: $OUT"
echo "manifest entries: $(wc -l < "$OUT/manifest.md5")"
