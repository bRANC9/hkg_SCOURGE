#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# FAKE_ENGINE_REV=1 — parked engine sound for Ioniq 5 Gen5W (260128).
#
# Installs /rw_data/fake-engine/{fake_engine_daemon,engine_idle.wav} and
# fake-engine.service, serving 127.0.0.1:8095:
#   POST /sound?action=start|stop     POST /sound?action=volume&value=0..100
#   GET  /status
set -u
PATH=${FAKE_ENGINE_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}
USB_DIR=${USB_DIR:-/media/usb}
PAYLOAD="$USB_DIR/payload"
RW_DIR=${FAKE_ENGINE_RW_DIR:-/rw_data/fake-engine}
SERVICE=${FAKE_ENGINE_SERVICE:-/etc/systemd/system/fake-engine.service}
LOG="$RW_DIR/install.log"
OUT="$USB_DIR/run_stuff_result.txt"
mkdir -p "$RW_DIR" 2>/dev/null || true
: > "$LOG" 2>/dev/null
log(){ echo "$(date -u 2>/dev/null) $*" >> "$LOG" 2>/dev/null; }
fail(){ echo "status=FAIL reason=$1" >> "$OUT"; echo "FAIL: $1" >> "$LOG"; exit 1; }
[ -f "$PAYLOAD/bin/fake_engine_daemon" ] || fail missing_daemon
[ -f "$PAYLOAD/audio/engine_idle.dat" ] || fail missing_asset
( cd "$USB_DIR" && md5sum -c manifest.md5 >/dev/null 2>&1 ) || fail manifest
cp -f "$PAYLOAD/bin/fake_engine_daemon" "$RW_DIR/fake_engine_daemon" || fail daemon_copy
chmod 755 "$RW_DIR/fake_engine_daemon" 2>/dev/null || true
cp -f "$PAYLOAD/audio/engine_idle.dat" "$RW_DIR/engine_idle.wav" || fail asset_copy
for _f in "$PAYLOAD"/audio/engine_*.dat; do
    [ -f "$_f" ] || continue
    cp -f "$_f" "$RW_DIR/$(basename "$_f" .dat).wav" || fail shift_asset_copy
done
chmod 644 "$RW_DIR/engine_idle.wav" 2>/dev/null || true
# Everything from here writes to the read-only rootfs.  install_io.sh owns the
# remount/verify/restore contract the other packages use; a plain cp here failed
# with "Read-only file system" on every run after the root had been restored ro.
[ -f "$PAYLOAD/lib/install_io.sh" ] || fail io_helper_missing
. "$PAYLOAD/lib/install_io.sh"
io_init || fail io_init
trap 'io_restore_root || true' EXIT
io_settle_root || fail root_read_only
io_copy "$PAYLOAD/systemd/fake-engine.service" "$SERVICE" || fail service_copy
ASSET_DIR="${FAKE_ENGINE_ASSET_DIR:-/app/share/AppHomeScreen}/fake-engine"
for _i in "$PAYLOAD"/assets/fake-engine/*.png; do
    [ -f "$_i" ] || continue
    io_asset "$_i" "$ASSET_DIR/$(basename "$_i")"
done
QML_ROOT=${FAKE_ENGINE_QML_ROOT:-/app/share/AppHomeScreen}
QML_DIR="$QML_ROOT/qml_eco3.0"
ADV="HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
NEW_FILES="HomeScreen/FakeEnginePage.qml component/FakeEngineTile.qml"
if [ -d "$QML_DIR" ]; then
    for _f in $NEW_FILES; do
        _src="$PAYLOAD/qml/AppHomeScreen/qml_eco3.0/$_f"
        [ -f "$_src" ] || fail "missing_qml:$_f"
        io_mkdir "$QML_DIR/$(dirname "$_f")" || fail "qml_mkdir:$_f"
        io_copy "$_src" "$QML_DIR/$_f" || fail "qml_copy:$_f"
    done
    _cur="$QML_DIR/$ADV"
    if [ -f "$_cur" ] && [ -f "$PAYLOAD/lib/qml_inject.sh" ]; then
        if grep -q "FAKE_ENGINE_TILE_V2" "$_cur" 2>/dev/null; then
            echo "qml: already injected" >> "$LOG"
            # The floating corner instance is gone from this payload; strip it from
            # units that were installed before 2026-11-25 so its top-right icon
            # disappears without a full restore.
            sh "$PAYLOAD/lib/qml_inject.sh" --file "$_cur" \
                --remove-marker FAKE_ENGINE_TILE_V2_FALLBACK >> "$LOG" 2>&1 \
                && echo "qml: corner icon removed" >> "$LOG" || true
        else
            # The tile has to behave like the battery/wifi/tweaks tiles: one
            # instance per grid page, placed by the SCOURGE allocator
            # (root.firstEmptySlot) and visible only on the page that got the free
            # cell.  The old single instance anchored at menuPage read
            # firstEmptyIndex(menuPage.pageModel, 6) and menuList.currentIndex -
            # neither exists in the live file, so the binding threw and the app
            # never appeared on the home screen (2026-11-23).
            fe_slot=0
            _max=$(grep -o 'firstEmptySlot([0-9][0-9]*)' "$_cur" 2>/dev/null | tr -dc '0-9\n' | sort -n | tail -n 1)
            case "$_max" in ''|*[!0-9]*) fe_slot=0 ;; *) fe_slot=$((_max + 1)) ;; esac
            echo "qml: slot=$fe_slot" >> "$LOG"
            fe_pages=0
            if grep -q 'function firstEmptySlot' "$_cur" 2>/dev/null; then
                for _p in 1 2 3; do
                    _menu=menuPage
                    [ "$_p" = 2 ] && _menu=menuPage2
                    [ "$_p" = 3 ] && _menu=menuPage3
                    grep -q "id: $_menu" "$_cur" 2>/dev/null || continue
                    sed -e "s/@PAGE0@/$((_p - 1))/g" -e "s/@PAGE@/$_p/g" \
                        -e "s/@MENU@/$_menu/g" -e "s/@ORD@/$fe_slot/g" \
                        "$PAYLOAD/qml/FakeEngineTile.instance.qml" > "$RW_DIR/tile.block.$_p.qml" 2>/dev/null \
                        || { echo "qml: cannot stage tile block $_menu" >> "$LOG"; continue; }
                    if sh "$PAYLOAD/lib/qml_inject.sh" --file "$_cur" --anchor "$_menu" \
                        --block "$RW_DIR/tile.block.$_p.qml" >> "$LOG" 2>&1; then
                        fe_pages=$((fe_pages + 1))
                    else
                        echo "qml: tile block for $_menu failed" >> "$LOG"
                    fi
                done
            fi
            echo "qml: tile injected on $fe_pages page(s)" >> "$LOG"
            # No floating corner instance any more: it drew a permanent icon in the
            # top-right of every page ("a fake engine lebegő jobb felső ikon", 2026-11-25)
            # and duplicated the grid tile.  The grid tile is the only entry point;
            # an already injected corner block is removed below.
            sh "$PAYLOAD/lib/qml_inject.sh" --file "$_cur" \
                --remove-marker FAKE_ENGINE_TILE_V2_FALLBACK >> "$LOG" 2>&1 || true
            sh "$PAYLOAD/lib/qml_inject.sh" --file "$_cur" --append \
                --block "$PAYLOAD/qml/FakeEnginePage.instance.qml" >> "$LOG" 2>&1 \
                && echo "qml: page injected" >> "$LOG" || echo "qml: page injection failed" >> "$LOG"
            fe_inject="pages=$fe_pages slot=$fe_slot"
        fi
    fi
fi

systemctl daemon-reload >/dev/null 2>&1 || true
systemctl enable fake-engine.service >/dev/null 2>&1 || true
systemctl restart fake-engine.service >/dev/null 2>&1 || true
sleep 1
{
  echo "FAKE_ENGINE_PAYLOAD"
  echo "time=$(date -u 2>/dev/null)"
  echo "usb=$USB_DIR"
  echo "status=OK daemon=$RW_DIR/fake_engine_daemon asset=$RW_DIR/engine_idle.wav service=fake-engine.service inject=${fe_inject:-already} asset_warnings=${ASSET_WARNINGS:-0} root=${IO_ORIGINAL_OPTIONS:-?}"
  echo "hint=curl -X POST http://127.0.0.1:8095/sound?action=start"
} > "$OUT"
echo "status=OK" >> "$LOG"
exit 0
