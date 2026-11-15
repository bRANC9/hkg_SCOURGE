#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

# Mode C lab FUMO HTTP exchange.
#
# Creates the DMCDEVINFO segment and a lab /update state, points the captured
# tree at a loopback mock DM endpoint, starts the real dmengine and sends the
# OEM session-start command (code 0, field1=1). The engine must open a real
# HTTP session and POST WBXML SyncML to the mock endpoint. The mock returns an
# empty 200, so this proves the client attempt, not a completed update.
set -u
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
OEM_PID=
DM_PID=
SERVER_PID=
LOG=/log_data/AppUpgrade/fota/vdm.log

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
    if [ -f /media/payload/mock-dm-server ] \
        && [ -f /media/payload/mock-dm-shm ] \
        && [ -f /media/payload/mock-oem-queue ]; then
        PAYLOAD=/media/payload
        break
    fi
    umount /media/payload 2>/dev/null || true
done
[ -n "$PAYLOAD" ] || { echo 'HARNESS_BLOCKED dmengine-fumo-http payload missing'; exit 77; }
trap 'kill "$OEM_PID" "$DM_PID" "$SERVER_PID" 2>/dev/null || true; cd /; umount "$PAYLOAD" 2>/dev/null || true' EXIT

cp "$PAYLOAD/mock-dm-server" /tmp/mock-dm || fail 'fumo-server-copy'
cp "$PAYLOAD/mock-dm-shm" /tmp/mock-dm-shm || fail 'fumo-shm-copy'
cp "$PAYLOAD/mock-oem-queue" /tmp/mock-oem || fail 'fumo-oem-copy'
chmod 0755 /tmp/mock-dm /tmp/mock-dm-shm /tmp/mock-oem || fail 'fumo-helper-mode'

ip link set lo up || fail 'fumo-loopback-up'
if ! ip addr show lo 2>/dev/null | grep -q '127.0.0.1'; then
    fail 'fumo-loopback-address'
fi

/tmp/mock-dm-shm >/tmp/mock-dm-shm.out 2>&1 || fail 'fumo-shm-create'
grep -q 'MOCK_DM_SHM_READY' /tmp/mock-dm-shm.out || fail 'fumo-shm-marker'

cp /etc/passwd.legacy /tmp/passwd || fail 'fumo-passwd'
ln -sf /etc/shadow.Global /tmp/shadow || fail 'fumo-shadow'

mkdir -p /update/fota/dme /update/usb/bin
cp /app/share/dmengine/config.txt /tmp/lab-dm-config.txt || fail 'fumo-config'
sed -i \
    -e 's#^log_all=.*#log_all=notice#' \
    -e 's#^is_MOFClient_used=.*#is_MOFClient_used=false#' \
    -e 's#^dmtreefile=.*#dmtreefile=tree.xml#' \
    -e 's#^MOFServer_host_name=.*#MOFServer_host_name=127.0.0.1#' \
    -e '$awbxml=false' \
    /tmp/lab-dm-config.txt || fail 'fumo-config-edit'
sed -e '/<name>Addr<\/name>/,/<value>/ s#<value>.*</value>#<value>http://127.0.0.1:8002/dm/message</value>#' \
    -e '/<name>AAuthSecret<\/name>/,/<value>/ s#<value>.*</value>#<value>lab-secret</value>#' \
    -e '/<name>AAuthName<\/name>/,/<value>/ s#<value>.*</value>#<value>lab</value>#' \
    -e '/<name>PkgURL<\/name>/,/<value>/ s#<value>.*</value>#<value>http://127.0.0.1:8002/package.bin</value>#' \
    -e '/<name>MinServerAuth<\/name>/,/<value>/ s#<value>.*</value>#<value>none</value>#' \
    /app/share/dmengine/tree.xml > /tmp/lab-tree.xml || fail 'fumo-tree-edit'
cp /tmp/lab-tree.xml /update/fota/dme/tree.xml || fail 'fumo-tree-copy'
cp /app/share/dmengine/version.txt /update/fota/dme/version.txt 2>/dev/null
printf '0' > /update/fota/dm_state.txt || fail 'fumo-dm-state'
printf '20261107125619^NE_E.EUR.S5W_L.001.001.250226^NE_E.EUR.S5W_L.001.001.250604' \
    > /update/FumoUsbUpdateTimeVersion || fail 'fumo-time-file'
printf 'ui:user-interaction/targetVersion:NE_E.EUR.S5W_L.001.001.250604/nextCountFumo:1/nextCountScomo:-1/installTime:99999999/execCommand:0000' \
    > /update/fota/install_param.txt || fail 'fumo-install-param'
printf 'SIGN_VALUE' > /update/package.tar.sign || fail 'fumo-sign'
printf '\017\011\041\217lab-blob' > /update/usb/bin/.lge.upgrade.xml || fail 'fumo-blob'

if grep -q '210.183.168.13' /tmp/lab-tree.xml /tmp/lab-dm-config.txt; then
    fail 'fumo-production-address'
fi

: > /tmp/mock-dm.log
/tmp/mock-dm 8002 /tmp/mock-dm.log >/tmp/mock-dm.out 2>&1 &
SERVER_PID=$!
server_wait=0
while [ "$server_wait" -lt 20 ]; do
    grep -q 'MOCK_DM_READY' /tmp/mock-dm.out 2>/dev/null && break
    sleep 1
    server_wait=$((server_wait + 1))
done
grep -q 'MOCK_DM_READY' /tmp/mock-dm.out || fail 'fumo-server-ready'

/usr/bin/dbus-daemon --session --fork --print-address >/tmp/dm-session-addr 2>/dev/null \
    || fail 'fumo-session-bus'
DBUS_ADDR=$(cat /tmp/dm-session-addr 2>/dev/null)
[ -n "$DBUS_ADDR" ] || fail 'fumo-session-address'
export DBUS_SESSION_BUS_ADDRESS="$DBUS_ADDR"

# dmengine's DLT logger waits for a daemon; phase4 starts one for the same
# reason.
/usr/bin/dlt-daemon -d >/tmp/dlt.err 2>&1 || true
sleep 1

cd /app/share/dmengine || fail 'fumo-cwd'
/app/bin/dmengine config=/tmp/lab-dm-config.txt >/tmp/dmengine-fumo.out 2>&1 &
DM_PID=$!
echo "DMENGINE_PID $DM_PID"
engine_wait=0
while [ "$engine_wait" -lt 90 ]; do
    dbus-send --session --print-reply --dest=org.freedesktop.DBus / \
        org.freedesktop.DBus.ListNames >/tmp/fumo-names.out 2>/dev/null || true
    grep -q 'com.lge.hkmc.avn.upgrade.fota.mofInterface' /tmp/fumo-names.out 2>/dev/null && break
    sleep 1
    engine_wait=$((engine_wait + 1))
done
if ! grep -q 'com.lge.hkmc.avn.upgrade.fota.mofInterface' /tmp/fumo-names.out 2>/dev/null; then
    tail -40 /tmp/dmengine-fumo.out
    fail 'fumo-ready'
fi
sleep 5
echo 'HARNESS_CHECK fumo-ready PASS'
LOG_MARK=$(wc -c < "$LOG" 2>/dev/null || echo 0)

/tmp/mock-oem 0 /tmp/mock-oem.log 10 1 0 >/tmp/mock-oem.out 2>&1 &
OEM_PID=$!

request_wait=0
while [ "$request_wait" -lt 60 ]; do
    grep -q 'MOCK_DM_REQUEST' /tmp/mock-dm.log 2>/dev/null && break
    sleep 1
    request_wait=$((request_wait + 1))
done
if ! grep -q 'MOCK_DM_REQUEST method=POST path=/dm/message' /tmp/mock-dm.log 2>/dev/null; then
    tail -40 /tmp/mock-dm.log 2>/dev/null
    tail -40 /tmp/dmengine-fumo.out
    fail 'fumo-http-post'
fi
echo 'HARNESS_CHECK fumo-http-post PASS'

if ! grep -q 'MOCK_DM_RESPONSE status=200 syncml=1 session=' /tmp/mock-dm.log; then
    tail -20 /tmp/mock-dm.log
    fail 'fumo-response'
fi
echo 'HARNESS_CHECK fumo-response PASS'

if ! grep -q 'content_type=application/vnd.syncml.dm+wbxml' /tmp/mock-dm.log; then
    tail -20 /tmp/mock-dm.log
    fail 'fumo-syncml-type'
fi
LENGTH=$(sed -n 's/.*content_length=\([0-9]*\).*/\1/p' /tmp/mock-dm.log | head -1)
if [ -z "$LENGTH" ] || [ "$LENGTH" -le 0 ]; then
    tail -20 /tmp/mock-dm.log
    fail 'fumo-syncml-length'
fi
if ! grep -q 'body_prefix=02' /tmp/mock-dm.log; then
    tail -20 /tmp/mock-dm.log
    fail 'fumo-syncml-wbxml'
fi
echo 'HARNESS_CHECK fumo-syncml PASS'

sleep 3
tail -c +$((LOG_MARK + 1)) "$LOG" > /tmp/vdm-delta.log 2>/dev/null
if ! grep -q '\[dmc_startFumoSession\] sesstionType = \[0\]' /tmp/vdm-delta.log \
    || ! grep -q 'Triggering DM session' /tmp/vdm-delta.log \
    || ! grep -q 'Performing HTTP POST' /tmp/vdm-delta.log; then
    tail -60 /tmp/vdm-delta.log
    fail 'fumo-trigger'
fi
echo 'HARNESS_CHECK fumo-trigger PASS'

if ! grep -q 'SCOMO_Plugin_onSessionStateChange inType=\[1\], inState=\[1\], inLastError=\[0\]' /tmp/vdm-delta.log; then
    grep -aE 'Session State Notify|onSessionStateChange|DM session aborted|lastError' /tmp/vdm-delta.log | tail -20
    fail 'fumo-session-complete'
fi
if grep -q 'DM session aborted' /tmp/vdm-delta.log; then
    grep -aE 'DM session aborted|lastError' /tmp/vdm-delta.log | tail -10
    fail 'fumo-session-aborted'
fi
echo 'HARNESS_CHECK fumo-session-complete PASS'

if grep -q '210.183.168.13' /tmp/vdm-delta.log /tmp/mock-dm.log; then
    fail 'fumo-production-log'
fi
echo 'HARNESS_CHECK fumo-no-production PASS'

echo '=== MOCK DM REQUEST ==='
head -5 /tmp/mock-dm.log 2>/dev/null
echo '=== VDM TRIGGER ==='
grep -aE 'dmc_startFumoSession|Triggering DM session|Open connection|Performing HTTP POST|onSessionStateChange' \
    /tmp/vdm-delta.log 2>/dev/null | head -20
kill "$DM_PID" "$OEM_PID" "$SERVER_PID" 2>/dev/null || true
sleep 1
kill -9 "$DM_PID" "$OEM_PID" "$SERVER_PID" 2>/dev/null || true
echo 'HARNESS_CHECK dmengine-fumo-http PASS'
