#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Render a head-unit QML page offscreen at a real resolution, with mock context
# properties and mock scan rows, and report every field whose text overflows its
# box.  Nothing here touches the car or the stick.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
PAGE=${1:-$ROOT/packages/wifi/artifacts/qml/AppHomeScreen/qml_eco3.0/HomeScreen/WifiPage.qml}
OUTDIR=${2:-$ROOT/out/preview}
JAIL=${QMLPREVIEW_JAIL:-/tmp/qmlpreview-jail}
BUILD=${QMLPREVIEW_BUILD:-/tmp/qmlpreview-build}
mkdir -p "$OUTDIR" "$BUILD" "$JAIL/HomeScreen" "$JAIL/component/common" "$JAIL/imports/HMIEVENTS" "$JAIL/imports/AppHome_Enum"

cp -f "$PAGE" "$JAIL/HomeScreen/$(basename "$PAGE")"
# The two vendor modules are enum/event only and the page uses no type from them;
# dropping the imports in the JAIL copy is what makes the page loadable off-car.
sed -i "/^import HMIEVENTS/d;/^import AppHome_Enum/d" "$JAIL/HomeScreen/$(basename "$PAGE")"
# The page asks QText from "../component": the stock one is a Text variant, and a
# plain Text behaves identically for measuring and painting.
{ printf "import QtQuick 2.0\nText {\n"; sed -n "s/^\\(property\\|signal\\) /    \\1 /p" "$HERE/stub-props.txt" 2>/dev/null | grep -v "property var text:" ; printf "}\n"; } > "$JAIL/component/QText.qml"
# Every other vendor tile the page instantiates is only a container here; a bare
# Item keeps the geometry (and therefore the measuring) honest.
# Stub every vendor type listed in stubs.txt inside the jail (never in the repo).
if [ -f "$HERE/stubs.txt" ]; then
    while IFS= read -r stub; do
        case "$stub" in ""|\#*) continue;; esac
        # Always regenerate: the stub lists grow as pages are added, and a stale
        # stub would fail the load with the property error we just fixed.
        { printf "import QtQuick 2.0\nItem {\n"; sed -n "s/^\\(property\\|signal\\) /    \\1 /p" "$HERE/stub-props.txt" 2>/dev/null; printf "\n    width: 190\n    height: 44\n    MouseArea {\n        anchors.fill: parent\n        onClicked: if (parent.clicked) parent.clicked()\n    }\n}\n"; } > "$JAIL/component/$stub.qml"
    done < "$HERE/stubs.txt"
fi
# Vendor QML modules the page imports by name; both are enum/event only and the
# page uses no type from them, so empty modules are enough.
printf "module HMIEVENTS\n" > "$JAIL/imports/HMIEVENTS/qmldir"
printf "module AppHome_Enum\n" > "$JAIL/imports/AppHome_Enum/qmldir"

# Qt 6 prints the grab reliably on the offscreen platform; Qt 5.15 returns a null
# image there (the window is never exposed), so prefer qmake6 when present.
QMAKE=qmake
command -v qmake6 >/dev/null 2>&1 && QMAKE=qmake6
if [ ! -x "$BUILD/qmlpreview" ]; then
    ( cd "$BUILD" && "$QMAKE" "$HERE/qmlpreview.pro" >/dev/null && make -s ) || { echo "build failed" >&2; exit 1; }
fi

for res in 1280x720 1920x720; do
    w=${res%x*}; h=${res#*x}
    name=$(basename "$PAGE" .qml)-$res.png
    QT_QUICK_BACKEND=software QT_QPA_PLATFORM=offscreen "$BUILD/qmlpreview" "$JAIL/HomeScreen/$(basename "$PAGE")" \
        "$w" "$h" "$OUTDIR/$name" "$JAIL" 2> "$OUTDIR/$name.log" || {
        echo "render failed for $res" >&2; cat "$OUTDIR/$name.log" >&2; exit 1; }
    echo "$name: $(tail -n 1 "$OUTDIR/$name.log")"
done