#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Remove the fake engine sound again.
set -u
USB_DIR=${USB_DIR:-/media/usb}
OUT="$USB_DIR/run_stuff_result.txt"
systemctl stop fake-engine.service >/dev/null 2>&1 || true
systemctl disable fake-engine.service >/dev/null 2>&1 || true
rm -f /etc/systemd/system/fake-engine.service 2>/dev/null || true
systemctl daemon-reload >/dev/null 2>&1 || true
pkill -f fake_engine_daemon 2>/dev/null || true
# Revert the home-screen injection and remove our two QML files.
if [ -f "$USB_DIR/payload/lib/qml_inject.sh" ]; then
    sh "$USB_DIR/payload/lib/qml_inject.sh" --file /app/share/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Advanced.qml --revert >/dev/null 2>&1 || true
fi
rm -f /app/share/AppHomeScreen/qml_eco3.0/HomeScreen/FakeEnginePage.qml 2>/dev/null || true
rm -f /app/share/AppHomeScreen/qml_eco3.0/component/FakeEngineTile.qml 2>/dev/null || true
rm -rf /app/share/AppHomeScreen/fake-engine 2>/dev/null || true
{ echo "FAKE_ENGINE_RESTORE"; echo "status=OK removed"; } > "$OUT"
exit 0
