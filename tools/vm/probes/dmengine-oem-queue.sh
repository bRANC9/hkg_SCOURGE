#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

# Mode C lab OEM-queue check for the real dmengine.
#
# Starts dmengine with the lab device-info segment and a lab DM tree/config,
# sends the notification through recvDM_01, and proves a lab OEM handler
# receives dmengine event 66 and can send a type-0x36 command. It does not
# implement AppDMClient's FUMO policy or complete an HTTP update.
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
    if [ -f /media/payload/mock-oem-queue ] \
        && [ -f /media/payload/mock-dm-shm ]; then
        PAYLOAD=/media/payload
        break
    fi
    umount /media/payload 2>/dev/null || true
done
[ -n "$PAYLOAD" ] || { echo 'HARNESS_BLOCKED dmengine-oem-queue payload missing'; exit 77; }
trap 'kill "$OEM_PID" "$DM_PID" 2>/dev/null || true; cd /; umount "$PAYLOAD" 2>/dev/null || true' EXIT

cp "$PAYLOAD/mock-dm-shm" /tmp/mock-dm-shm || fail 'oem-shm-copy'
cp "$PAYLOAD/mock-oem-queue" /tmp/mock-oem-queue || fail 'oem-helper-copy'
chmod 0755 /tmp/mock-dm-shm /tmp/mock-oem-queue || fail 'oem-helper-mode'
/tmp/mock-dm-shm >/tmp/mock-dm-shm.out 2>&1 || fail 'oem-shm-create'
grep -q 'MOCK_DM_SHM_READY' /tmp/mock-dm-shm.out || fail 'oem-shm-marker'

cp /etc/passwd.legacy /tmp/passwd || fail 'oem-passwd'
ln -sf /etc/shadow.Global /tmp/shadow || fail 'oem-shadow'
/usr/bin/dbus-daemon --session --fork --print-address >/tmp/dm-session-addr 2>/dev/null \
    || fail 'oem-session-bus'
DBUS_ADDR=$(cat /tmp/dm-session-addr 2>/dev/null)
[ -n "$DBUS_ADDR" ] || fail 'oem-session-address'
export DBUS_SESSION_BUS_ADDRESS="$DBUS_ADDR"

cp /app/share/dmengine/config.txt /tmp/lab-dm-config.txt || fail 'oem-config'
sed -i \
    -e 's#^log_all=.*#log_all=notice#' \
    -e 's#^is_MOFClient_used=.*#is_MOFClient_used=false#' \
    -e 's#^dmtreefile=.*#dmtreefile=/tmp/lab-tree.xml#' \
    -e 's#^MOFServer_host_name=.*#MOFServer_host_name=127.0.0.1#' \
    /tmp/lab-dm-config.txt || fail 'oem-config-edit'
sed -e '/<name>Addr<\/name>/,/<value>/ s#<value>.*</value>#<value>http://127.0.0.1:8002/dm/message</value>#' \
    -e '/<name>AAuthSecret<\/name>/,/<value>/ s#<value>.*</value>#<value>lab-secret</value>#' \
    -e '/<name>AAuthName<\/name>/,/<value>/ s#<value>.*</value>#<value>lab</value>#' \
    /app/share/dmengine/tree.xml > /tmp/lab-tree.xml || fail 'oem-tree-edit'
if grep -q '210.183.168.13' /tmp/lab-tree.xml /tmp/lab-dm-config.txt; then
    fail 'oem-production-address'
fi

rm -f /tmp/mock-oem.log
/tmp/mock-oem-queue 1 /tmp/mock-oem.log 60 >/tmp/mock-oem.out 2>&1 &
OEM_PID=$!
for _ in $(seq 1 20); do
    grep -q 'MOCK_OEM_READY' /tmp/mock-oem.out 2>/dev/null && break
    sleep 0.5
done
grep -q 'MOCK_OEM_READY' /tmp/mock-oem.out || fail 'oem-ready'
grep -q 'MOCK_OEM_SENT code=1' /tmp/mock-oem.log || fail 'oem-command-sent'
echo 'HARNESS_CHECK oem-command-sent PASS'

cd /app/share/dmengine || fail 'oem-cwd'
/app/bin/dmengine config=/tmp/lab-dm-config.txt >/tmp/dmengine-oem.log 2>&1 &
DM_PID=$!
for _ in $(seq 1 60); do
    grep -q 'DevID len ' /tmp/dmengine-oem.log 2>/dev/null && break
    kill -0 "$DM_PID" 2>/dev/null || break
    sleep 0.5
done
grep -q 'DevID len ' /tmp/dmengine-oem.log || {
    tail -40 /tmp/dmengine-oem.log
    fail 'oem-dmengine-start'
}
echo 'HARNESS_CHECK oem-ready PASS'

BUS_NAME=com.lge.hkmc.avn.upgrade.fota.mofInterface
for _ in $(seq 1 40); do
    dbus-send --session --print-reply --dest=org.freedesktop.DBus / \
        org.freedesktop.DBus.ListNames >/tmp/oem-names.out 2>&1 || true
    grep -q "$BUS_NAME" /tmp/oem-names.out 2>/dev/null && break
    sleep 0.5
done
grep -q "$BUS_NAME" /tmp/oem-names.out 2>/dev/null || {
    tail -20 /tmp/oem-names.out
    fail 'oem-mof-bus'
}
echo 'HARNESS_CHECK oem-mof-bus PASS'
dbus-send --session --type=method_call \
    --dest=$BUS_NAME / $BUS_NAME.recvDM_01 \
    'string:msgtype=4&cor=LAB' >/tmp/oem-trigger.out 2>&1
sleep 1

for _ in $(seq 1 30); do
    grep -q 'MOCK_OEM_EVENT code=66' /tmp/mock-oem.log 2>/dev/null && break
    sleep 0.5
done
if ! grep -q 'MOCK_OEM_EVENT code=66' /tmp/mock-oem.log; then
    tail -60 /tmp/mock-oem.log
    tail -40 /tmp/dmengine-oem.log
    fail 'oem-event-66'
fi
echo 'HARNESS_CHECK oem-event-66 PASS'

if grep -q '210.183.168.13' /tmp/dmengine-oem.log /tmp/mock-oem.log; then
    fail 'oem-production-log'
fi
echo 'HARNESS_CHECK oem-no-production PASS'

kill "$DM_PID" "$OEM_PID" 2>/dev/null || true
wait "$DM_PID" "$OEM_PID" 2>/dev/null || true
echo 'HARNESS_CHECK dmengine-oem-queue PASS'
