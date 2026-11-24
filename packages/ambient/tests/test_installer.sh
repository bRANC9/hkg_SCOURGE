#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Host test for the Ambience payload: build it, run the real installer against
# a fake rootfs with stubbed system commands, then run the restore.
#
#   sh tests/test_installer.sh
set -eu
PATH=/bin:/sbin:/usr/bin:/usr/sbin

HERE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/ambient-installer.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

pass() { echo "ok - $*"; }

# ---- fixture rootfs ---------------------------------------------------------
ROOTFS="$WORK/rootfs"
QML="$ROOTFS/app/share/AppHomeScreen/qml_eco3.0"
RW="$WORK/rw"
BIN="$WORK/bin"
mkdir -p "$QML/HomeScreen" "$QML/component/common" "$ROOTFS/etc/systemd/system" "$RW" "$BIN"

cat > "$QML/HomeScreen/QHMI_HomeScreenMain_Advanced.qml" <<'QMLBASE'
// fixture home screen (allocator + one page item)
import QtQuick 2.3
import "../component"

QRootScreen {
    id: root
    property int k_nbrOfColumns: 5
    property int k_itemWidth: 200
    function firstEmptySlot(customIndex) { return { page: 0, index: customIndex } }
    function firstEmptyIndex(pageModel, skip) { return skip }
    Item {
        id: content
        QMenuPageAllowEmpty {
            id: menuPage
            pageModel: null
        }
    }
}
QMLBASE
cp "$QML/HomeScreen/QHMI_HomeScreenMain_Advanced.qml" "$WORK/original.qml"

# ---- command stubs ----------------------------------------------------------
cat > "$BIN/chsmack" <<'EOF'
#!/bin/sh
# chsmack -e LABEL FILE
exit 0
EOF
cat > "$BIN/getfattr" <<'EOF'
#!/bin/sh
# getfattr -n security.SMACK64EXEC --only-values FILE -> the override label
for last; do :; done
printf '%s\n' "${AMBIENT_SMACK_SELF:-_}"
exit 0
EOF
cat > "$BIN/setfattr" <<'EOF'
#!/bin/sh
exit 0
EOF
cat > "$BIN/systemctl" <<'EOF'
#!/bin/sh
echo "systemctl $*" >> "$STUB_LOG"
exit 0
EOF
cat > "$BIN/pidof" <<'EOF'
#!/bin/sh
# Nothing is really started in this test.
exit 1
EOF
cat > "$BIN/dbus-send" <<'EOF'
#!/bin/sh
case "$*" in
    *terminate*) exit 0 ;;
    *launch*) exit 0 ;;
esac
exit 0
EOF
cat > "$BIN/dbus-monitor" <<'EOF'
#!/bin/sh
exit 0
EOF
cat > "$BIN/mount" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod 755 "$BIN"/*

STUB_LOG="$WORK/stubs.log"
: > "$STUB_LOG"
export STUB_LOG

# A fake mount table: the fixture root is reported rw at the install root.
printf 'fake %s ext4 rw,relatime 0 0\n' "$ROOTFS" > "$WORK/mounts"

# ---- build ------------------------------------------------------------------
STAGE="$WORK/usb"
sh "$HERE/scripts/build_payload.sh" "$STAGE" > "$WORK/build.log" 2>&1 \
    || { cat "$WORK/build.log"; fail "payload build"; }
pass "payload built"
[ -f "$STAGE/manifest.md5" ] || fail "no manifest"
[ -x "$STAGE/payload/bin/ambient_daemon" ] || fail "daemon binary missing"
( cd "$STAGE" && md5sum -c manifest.md5 >/dev/null ) || fail "manifest does not verify"
pass "manifest verifies"

# ---- install ----------------------------------------------------------------
env \
    PATH="$BIN:/usr/bin:/bin" \
    USB_DIR="$STAGE" \
    AMBIENT_PATH="$BIN:/usr/bin:/bin" \
    AMBIENT_QML_ROOT="$ROOTFS/app/share/AppHomeScreen" \
    AMBIENT_RW_DIR="$RW" \
    AMBIENT_SERVICE="$ROOTFS/etc/systemd/system/ambient-daemon.service" \
    AMBIENT_SMACK_SELF="_" \
    INSTALL_IO_ROOT="$ROOTFS" \
    INSTALL_IO_MOUNTS="$WORK/mounts" \
    sh "$STAGE/run_stuff.sh" > "$WORK/install.out" 2>&1 \
    || { cat "$WORK/install.out"; cat "$RW/install.log" 2>/dev/null; fail "installer exited non-zero"; }

RESULT="$STAGE/run_stuff_result.txt"
[ -f "$RESULT" ] || fail "no result file"
grep -q 'status=OK' "$RESULT" || { cat "$RESULT"; fail "status not OK"; }
grep -q 'inject=tile+page' "$RESULT" || { cat "$RESULT"; fail "tile not injected"; }
if grep -q 'smack_check=mismatch' "$RESULT"; then cat "$RESULT"; fail "smack mismatch"; fi
pass "installer result: status=OK inject=tile+page"

for f in HomeScreen/AmbientPage.qml component/AmbientTile.qml component/AmbientSlider.qml; do
    [ -f "$QML/$f" ] || fail "missing installed file: $f"
done
[ -f "$ROOTFS/app/share/AppHomeScreen/ambient/ambient_icon_n.png" ] || fail "icon missing"
[ -f "$RW/ambient_daemon" ] || fail "daemon not installed"
[ -x "$RW/bin/micom-send" ] || fail "micom helper missing"
[ -f "$ROOTFS/etc/systemd/system/ambient-daemon.service" ] || fail "service unit missing"
pass "files installed"

ADV="$QML/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
grep -q 'AMBIENT_PAYLOAD_260128' "$ADV" || fail "marker not injected"
grep -q 'AmbientTile {' "$ADV" || fail "tile instance missing"
grep -q 'AmbientPage {' "$ADV" || fail "page instance missing"
[ -f "$RW/qml-backup/QHMI_HomeScreenMain_Advanced.qml" ] || fail "no injector backup"
pass "tile + page injected"

# Brace balance must be unchanged by the injection.
opens=$(awk '{ for (i = 1; i <= length($0); i++) { c = substr($0, i, 1); if (c == "{") o++ } } END { print o + 0 }' "$ADV")
closes=$(awk '{ for (i = 1; i <= length($0); i++) { c = substr($0, i, 1); if (c == "}") o++ } } END { print o + 0 }' "$ADV")
[ "$opens" = "$closes" ] || fail "braces unbalanced after injection ($opens/$closes)"
pass "braces balanced ($opens pairs)"

# ---- reinstall is idempotent ------------------------------------------------
cp -f "$ADV" "$WORK/after-first.qml"
env \
    PATH="$BIN:/usr/bin:/bin" USB_DIR="$STAGE" \
    AMBIENT_PATH="$BIN:/usr/bin:/bin" \
    AMBIENT_QML_ROOT="$ROOTFS/app/share/AppHomeScreen" \
    AMBIENT_RW_DIR="$RW" \
    AMBIENT_SERVICE="$ROOTFS/etc/systemd/system/ambient-daemon.service" \
    AMBIENT_SMACK_SELF="_" INSTALL_IO_ROOT="$ROOTFS" \
    INSTALL_IO_MOUNTS="$WORK/mounts" \
    sh "$STAGE/run_stuff.sh" > "$WORK/install2.out" 2>&1 \
    || { cat "$WORK/install2.out"; fail "second install failed"; }
grep -q 'inject=already' "$STAGE/run_stuff_result.txt" \
    || { cat "$STAGE/run_stuff_result.txt"; fail "reinstall re-injected"; }
cmp -s "$ADV" "$WORK/after-first.qml" || fail "reinstall changed the injected file"
pass "reinstall is idempotent"

# ---- page-only fallback (no allocator in the base) --------------------------
PLAIN="$WORK/plain"
mkdir -p "$PLAIN/HomeScreen"
cat > "$PLAIN/HomeScreen/QHMI_HomeScreenMain_Advanced.qml" <<'QMLPLAIN'
import QtQuick 2.3
QRootScreen {
    id: root
    Item {
        id: content
        Text { text: "stock" }
    }
}
QMLPLAIN
sh "$STAGE/payload/lib/qml_inject.sh" --file "$PLAIN/HomeScreen/QHMI_HomeScreenMain_Advanced.qml" \
    --append --block "$STAGE/payload/qml/AmbientPage.instance.qml" \
    --backup-dir "$PLAIN/backup" > "$WORK/plain.log" 2>&1 \
    || { cat "$WORK/plain.log"; fail "append injection failed"; }
grep -q 'AmbientPage {' "$PLAIN/HomeScreen/QHMI_HomeScreenMain_Advanced.qml" \
    || fail "page not appended"
po=$(awk '{ for (i = 1; i <= length($0); i++) { c = substr($0, i, 1); if (c == "{") o++ } } END { print o + 0 }' "$PLAIN/HomeScreen/QHMI_HomeScreenMain_Advanced.qml")
pc=$(awk '{ for (i = 1; i <= length($0); i++) { c = substr($0, i, 1); if (c == "}") o++ } } END { print o + 0 }' "$PLAIN/HomeScreen/QHMI_HomeScreenMain_Advanced.qml")
[ "$po" = "$pc" ] || fail "append broke braces ($po/$pc)"
# The block must close after the root item's last line, not outside it.
tail -n 2 "$PLAIN/HomeScreen/QHMI_HomeScreenMain_Advanced.qml" | grep -q '^}' \
    || fail "appended block is outside the root item"
pass "page-only append fallback"

# ---- restore ----------------------------------------------------------------
cp -f "$STAGE/run_stuff_restore.sh" "$WORK/restore.sh"
env \
    PATH="$BIN:/usr/bin:/bin" USB_DIR="$STAGE" \
    AMBIENT_PATH="$BIN:/usr/bin:/bin" \
    AMBIENT_QML_ROOT="$ROOTFS/app/share/AppHomeScreen" \
    AMBIENT_RW_DIR="$RW" \
    AMBIENT_SERVICE="$ROOTFS/etc/systemd/system/ambient-daemon.service" \
    sh "$WORK/restore.sh" > "$WORK/restore.out" 2>&1 \
    || { cat "$WORK/restore.out"; cat "$RW/restore.log" 2>/dev/null; fail "restore failed"; }
cmp -s "$ADV" "$WORK/original.qml" || { diff "$WORK/original.qml" "$ADV" | head -20; fail "restore did not restore the original"; }
[ ! -f "$QML/HomeScreen/AmbientPage.qml" ] || fail "restore left the page"
[ ! -f "$RW/ambient_daemon" ] || fail "restore left the daemon"
[ ! -f "$ROOTFS/etc/systemd/system/ambient-daemon.service" ] || fail "restore left the unit"
[ -f "$RW/config.conf" ] || true
pass "restore round-trip"

echo "all installer checks passed"
