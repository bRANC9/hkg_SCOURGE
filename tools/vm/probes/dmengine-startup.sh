#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

# Mode C startup probe for the real vendor dmengine.
#
# A lab helper creates the DMCDEVINFO SysV segment that dmengine attaches to,
# and the captured tree/config are rewritten to lab values with no production
# address. The probe proves the engine reads the tree, accepts device info and
# initializes the FUMO plugin. It does not complete a DM session.
set -u
export PATH=/bin:/sbin:/usr/bin:/usr/sbin

fail() {
    echo "HARNESS_FAIL $1"
    exit 1
}

modprobe ehci-pci 2>/dev/null || true
modprobe usb-storage 2>/dev/null || true
modprobe sd_mod 2>/dev/null || true
sleep 2
mkdir -p /media/payload
PAYLOAD=
for dev in /dev/sd?; do
    [ -b "$dev" ] || continue
    mount -o ro "$dev" /media/payload 2>/dev/null || continue
    if [ -f /media/payload/mock-dm-shm ]; then
        PAYLOAD=/media/payload
        break
    fi
    umount /media/payload 2>/dev/null || true
done
[ -n "$PAYLOAD" ] || { echo 'HARNESS_BLOCKED dmengine-startup payload missing'; exit 77; }
trap 'kill "$DM_PID" 2>/dev/null || true; cd /; umount "$PAYLOAD" 2>/dev/null || true' EXIT

cp "$PAYLOAD/mock-dm-shm" /tmp/mock-dm-shm || fail 'dmengine-shm-copy'
chmod 0755 /tmp/mock-dm-shm || fail 'dmengine-shm-mode'
/tmp/mock-dm-shm >/tmp/mock-dm-shm.out 2>&1 || fail 'dmengine-shm-create'
grep -q 'MOCK_DM_SHM_READY' /tmp/mock-dm-shm.out || fail 'dmengine-shm-marker'

cp /etc/passwd.legacy /tmp/passwd || fail 'dmengine-passwd'
ln -sf /etc/shadow.Global /tmp/shadow || fail 'dmengine-shadow'
/usr/bin/dbus-daemon --session --fork --print-address >/tmp/dm-session-addr 2>/tmp/dm-session.err \
    || fail 'dmengine-session-bus'
DBUS_ADDR=$(cat /tmp/dm-session-addr 2>/dev/null)
[ -n "$DBUS_ADDR" ] || fail 'dmengine-session-address'
export DBUS_SESSION_BUS_ADDRESS="$DBUS_ADDR"

cp /app/share/dmengine/config.txt /tmp/lab-dm-config.txt || fail 'dmengine-config'
sed -i \
    -e 's#^log_all=.*#log_all=notice#' \
    -e 's#^is_MOFClient_used=.*#is_MOFClient_used=false#' \
    -e 's#^dmtreefile=.*#dmtreefile=/tmp/lab-tree.xml#' \
    -e 's#^MOFServer_host_name=.*#MOFServer_host_name=127.0.0.1#' \
    /tmp/lab-dm-config.txt || fail 'dmengine-config-edit'

sed -e '/<name>Addr<\/name>/,/<value>/ s#<value>.*</value>#<value>http://127.0.0.1:8002/dm/message</value>#' \
    -e '/<name>AAuthSecret<\/name>/,/<value>/ s#<value>.*</value>#<value>lab-secret</value>#' \
    -e '/<name>AAuthName<\/name>/,/<value>/ s#<value>.*</value>#<value>lab</value>#' \
    /app/share/dmengine/tree.xml > /tmp/lab-tree.xml || fail 'dmengine-tree-edit'

if grep -q '210.183.168.13' /tmp/lab-tree.xml /tmp/lab-dm-config.txt; then
    fail 'dmengine-production-address'
fi
echo 'HARNESS_CHECK dmengine-ready PASS'

cd /app/share/dmengine || fail 'dmengine-cwd'
/app/bin/dmengine config=/tmp/lab-dm-config.txt >/tmp/dmengine-startup.log 2>&1 &
DM_PID=$!

for _ in $(seq 1 60); do
    if grep -q 'FUMO Plugin Init' /tmp/dmengine-startup.log 2>/dev/null \
        && grep -q '\[dmc_getDeviceInfo\] Start' /tmp/dmengine-startup.log 2>/dev/null \
        && grep -q 'DevID len ' /tmp/dmengine-startup.log 2>/dev/null \
        && grep -q 'SCOMO Plugin Init' /tmp/dmengine-startup.log 2>/dev/null; then
        break
    fi
    kill -0 "$DM_PID" 2>/dev/null || break
    sleep 0.5
done

if ! grep -q 'FUMO Plugin Init' /tmp/dmengine-startup.log; then
    tail -40 /tmp/dmengine-startup.log
    fail 'dmengine-tree'
fi
echo 'HARNESS_CHECK dmengine-tree PASS'

if ! grep -q '\[dmc_getDeviceInfo\] Start' /tmp/dmengine-startup.log \
    || ! grep -q 'DevID len ' /tmp/dmengine-startup.log; then
    tail -40 /tmp/dmengine-startup.log
    fail 'dmengine-device-info'
fi
echo 'HARNESS_CHECK dmengine-device-info PASS'

if ! grep -q 'SCOMO Plugin Init' /tmp/dmengine-startup.log; then
    tail -40 /tmp/dmengine-startup.log
    fail 'dmengine-scomo-plugin'
fi
echo 'HARNESS_CHECK dmengine-scomo-plugin PASS'

if grep -q '210.183.168.13' /tmp/dmengine-startup.log; then
    fail 'dmengine-production-log'
fi
echo 'HARNESS_CHECK dmengine-no-production PASS'

kill "$DM_PID" 2>/dev/null || true
wait "$DM_PID" 2>/dev/null || true
echo 'HARNESS_CHECK dmengine-startup PASS'
