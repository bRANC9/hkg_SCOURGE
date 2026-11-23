#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Interactive emulation of a head-unit page through Qt's VNC platform plugin.
# The real QML renders into a real window (so taps and clicks work), and any VNC
# client - desktop or phone - can drive it at head-unit resolution.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
PAGE=${1:-$ROOT/packages/wifi/artifacts/qml/AppHomeScreen/qml_eco3.0/HomeScreen/WifiPage.qml}
GEOM=${2:-1280x720}
PORT=${VNC_PORT:-5906}
JAIL=${QMLPREVIEW_JAIL:-/tmp/qmlpreview-jail}
BUILD=${QMLVNC_BUILD:-/tmp/qmlvnc-build}

# The jail (page copy + vendor stubs) is the same one render.sh builds.
sh "$HERE/render.sh" "$PAGE" >/dev/null 2>&1 || true
mkdir -p "$BUILD"
if [ ! -x "$BUILD/qmlvnc" ]; then
    command -v qmake6 >/dev/null 2>&1 || { echo "qmake6 is required (Qt 6)" >&2; exit 1; }
    ( cd "$BUILD" && qmake6 "$HERE/qmlvnc.pro" >/dev/null && make -s ) || { echo "build failed" >&2; exit 1; }
fi

echo "page: $PAGE"
echo "connect a VNC client to 127.0.0.1:$PORT  (geometry $GEOM)"
echo "  Linux: vncviewer 127.0.0.1:$PORT   ·   macOS: Finder > Go > Connect to Server > vnc://127.0.0.1:$PORT"
echo "  phone: any VNC app, address 127.0.0.1:$PORT (or the host IP)"
QT_QPA_PLATFORM="vnc:size=$GEOM:port=$PORT" QT_QUICK_BACKEND=software \
    "$BUILD/qmlvnc" "$JAIL/HomeScreen/$(basename "$PAGE")" \
    "${GEOM%x*}" "${GEOM#*x}" "$JAIL"