#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Java 25 runtime package test. Runs only inside the disposable VM.
#
# The payload stick contains install/ (the real out/usb tree) and
# selftest.jar.  The probe installs through run_stuff.sh, exercises the
# runtime, then restores.  Security/policy claims are not made here.
set -u
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
PAYLOAD=
RW=/rw_data/java-vmtest
APPS=/rw_data/java-apps-vmtest
DPID=

cleanup() {
    if [ -n "${DPID:-}" ]; then
        kill -TERM "$DPID" 2>/dev/null || true
        wait "$DPID" 2>/dev/null || true
    fi
    cd / 2>/dev/null || true
    umount /media/payload 2>/dev/null || true
    rm -rf "$APPS" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

fail() {
    echo "HARNESS_FAIL $1${2:+ ($2)}"
    exit 1
}

show_install_logs() {
    for file in "$RW/result.txt" "$RW/install.log" "$RW/version-check.log" \
                "$RW/sha256-check.log" "$PAYLOAD/install/run_stuff_result.txt"; do
        [ -f "$file" ] || continue
        echo "=== $file ==="
        tail -40 "$file" 2>/dev/null || true
    done
}

modprobe ehci-pci 2>/dev/null || true
modprobe usb-storage 2>/dev/null || true
modprobe sd_mod 2>/dev/null || true
sleep 2
ip link set lo up || fail java-loopback-up

mkdir -p /media/payload
attempt=0
while [ "$attempt" -lt 20 ]; do
    for dev in /dev/sd?; do
        [ -b "$dev" ] || continue
        mount -o rw "$dev" /media/payload 2>/dev/null || continue
        if [ -f /media/payload/selftest.jar ] && [ -d /media/payload/install/payload/java ]; then
            PAYLOAD=/media/payload
            break 2
        fi
        umount /media/payload 2>/dev/null || true
    done
    sleep 0.5
    attempt=$((attempt + 1))
done
[ -n "$PAYLOAD" ] || { echo 'HARNESS_BLOCKED java payload stick missing'; exit 77; }

rm -rf "$RW" "$APPS"

echo '== path guards'
# These must fail before any marker/write.  /rw_data/ and .. canonicalize to
# /rw_data; the app path must not be the runtime path either.
if USB_DIR="$PAYLOAD/install" JAVA_RW_DIR=/rw_data/ JAVA_APPS_DIR=/tmp/java-apps-guard \
        sh "$PAYLOAD/install/run_stuff.sh" >/tmp/java-guard1.log 2>&1; then
    fail java-install 'installer accepted JAVA_RW_DIR=/rw_data/'
fi
if USB_DIR="$PAYLOAD/install" JAVA_RW_DIR="$RW/.." JAVA_APPS_DIR=/tmp/java-apps-guard \
        sh "$PAYLOAD/install/run_stuff.sh" >/tmp/java-guard2.log 2>&1; then
    fail java-install 'installer accepted JAVA_RW_DIR=<parent>/..'
fi
if USB_DIR="$PAYLOAD/install" JAVA_RW_DIR="$APPS/" JAVA_APPS_DIR="$APPS" \
        sh "$PAYLOAD/install/run_stuff.sh" >/tmp/java-guard3.log 2>&1; then
    fail java-install 'installer accepted JAVA_RW_DIR == JAVA_APPS_DIR'
fi
[ ! -e /rw_data/.scourge-java ] || fail java-install 'guard wrote /rw_data marker'
[ ! -e "$APPS/.scourge-java" ] || fail java-install 'guard wrote app-dir marker'
echo 'JAVA_PATH_GUARD_OK'

echo '== install through the real package installer'
if ! USB_DIR="$PAYLOAD/install" JAVA_RW_DIR="$RW" JAVA_APPS_DIR="$APPS" \
        sh "$PAYLOAD/install/run_stuff.sh" > /tmp/java-install.log 2>&1; then
    cat /tmp/java-install.log 2>/dev/null
    show_install_logs
    fail java-install 'run_stuff.sh failed'
fi
grep -q '^status=OK$' "$RW/result.txt" || {
    cat /tmp/java-install.log 2>/dev/null
    show_install_logs
    fail java-install 'installer did not report status=OK'
}
[ -L "$RW/current" ] || fail java-install 'current symlink missing'
[ -x "$RW/current/bin/java" ] || fail java-install 'installed java is not executable'
[ -f "$RW/.scourge-java" ] || fail java-install 'restore marker missing'
[ -d "$APPS" ] || fail java-install 'app directory missing'
printf 'demo\n' > "$APPS/vmtest.jar" || fail java-install 'cannot write app marker'
echo 'HARNESS_CHECK java-install PASS'

echo '== version'
"$RW/current/bin/java" -version > /tmp/java-version.log 2>&1 || {
    cat /tmp/java-version.log 2>/dev/null
    fail java-version 'java -version failed'
}
grep -q 'openjdk version "25.0.5.1"' /tmp/java-version.log || {
    cat /tmp/java-version.log 2>/dev/null
    fail java-version 'unexpected java version'
}
grep -q 'Temurin-25.0.5.1+2' /tmp/java-version.log || {
    cat /tmp/java-version.log 2>/dev/null
    fail java-version 'not the Temurin build'
}
echo 'HARNESS_CHECK java-version PASS'

echo '== self-test'
if ! "$RW/current/bin/java" -Xmx128m -jar "$PAYLOAD/selftest.jar" selftest \
        > /tmp/java-selftest.log 2>&1; then
    cat /tmp/java-selftest.log 2>/dev/null
    fail java-selftest 'selftest exited non-zero'
fi
grep -q '^JAVA_SELFTEST_OK$' /tmp/java-selftest.log || {
    cat /tmp/java-selftest.log 2>/dev/null
    fail java-selftest 'selftest marker missing'
}
echo 'HARNESS_CHECK java-selftest PASS'

echo '== HTTP daemon lifecycle'
rm -f /tmp/java-daemon.ready /tmp/java-daemon.shutdown
"$RW/current/bin/java" -Xmx128m -jar "$PAYLOAD/selftest.jar" daemon \
    --ready /tmp/java-daemon.ready --shutdown /tmp/java-daemon.shutdown \
    > /tmp/java-daemon.log 2>&1 &
DPID=$!
i=0
while [ "$i" -lt 60 ]; do
    [ -f /tmp/java-daemon.ready ] && break
    kill -0 "$DPID" 2>/dev/null || break
    sleep 0.5
    i=$((i + 1))
done
[ -f /tmp/java-daemon.ready ] || {
    cat /tmp/java-daemon.log 2>/dev/null
    fail java-daemon 'daemon did not become ready'
}
PORT=$(sed -n 's/^port=//p' /tmp/java-daemon.ready | head -1)
[ -n "$PORT" ] || fail java-daemon 'ready file has no port'
"$RW/current/bin/java" -jar "$PAYLOAD/selftest.jar" client "$PORT" \
    > /tmp/java-client.log 2>&1 || {
    cat /tmp/java-client.log 2>/dev/null
    cat /tmp/java-daemon.log 2>/dev/null
    fail java-daemon 'client request failed'
}
grep -q '^JAVA_CLIENT_OK$' /tmp/java-client.log || fail java-daemon 'client marker missing'

# Diagnostics are useful but not part of the runtime contract; record rather
# than gate on attach behaviour across kernels.
if "$RW/current/bin/jcmd" "$DPID" VM.version > /tmp/java-jcmd.log 2>&1; then
    echo 'JAVA_JCMD_OK'
else
    echo 'JAVA_JCMD_UNAVAILABLE'
    cat /tmp/java-jcmd.log 2>/dev/null || true
fi

kill -TERM "$DPID" 2>/dev/null || true
wait "$DPID" 2>/dev/null || true
DPID=
i=0
while [ "$i" -lt 20 ]; do
    [ -f /tmp/java-daemon.shutdown ] && break
    sleep 0.25
    i=$((i + 1))
done
[ -f /tmp/java-daemon.shutdown ] || fail java-daemon 'shutdown hook did not run'
echo 'HARNESS_CHECK java-daemon PASS'

echo '== reinstall and prune'
if ! USB_DIR="$PAYLOAD/install" JAVA_RW_DIR="$RW" JAVA_APPS_DIR="$APPS" \
        sh "$PAYLOAD/install/run_stuff.sh" > /tmp/java-install2.log 2>&1; then
    cat /tmp/java-install2.log 2>/dev/null
    fail java-idempotent 'second install failed'
fi
grep -q '^status=OK$' "$RW/result.txt" || {
    show_install_logs
    fail java-idempotent 'second install did not report OK'
}
COUNT=$(find "$RW" -maxdepth 1 -type d -name 'runtime-*' | wc -l | tr -d ' ')
[ "$COUNT" = "1" ] || fail java-idempotent "expected one runtime, found $COUNT"
[ -L "$RW/current" ] || fail java-idempotent 'current symlink lost'
echo 'HARNESS_CHECK java-idempotent PASS'

echo '== restore path guards'
for guard in /rw_data/ /rw_data/. "$RW/.." "$APPS/"; do
    if USB_DIR="$PAYLOAD/install" JAVA_RW_DIR="$guard" JAVA_APPS_DIR="$APPS" \
            sh "$PAYLOAD/install/run_stuff_restore.sh" >/tmp/java-restore-guard.log 2>&1; then
        fail java-restore "restore accepted JAVA_RW_DIR=$guard"
    fi
    [ -d "$RW" ] || fail java-restore "guard $guard removed the runtime"
    [ -f "$APPS/vmtest.jar" ] || fail java-restore "guard $guard removed the app JAR"
done
echo 'JAVA_RESTORE_GUARD_OK'

echo '== restore'
if ! USB_DIR="$PAYLOAD/install" JAVA_RW_DIR="$RW" JAVA_APPS_DIR="$APPS" \
        sh "$PAYLOAD/install/run_stuff_restore.sh" > /tmp/java-restore.log 2>&1; then
    cat /tmp/java-restore.log 2>/dev/null
    cat /tmp/java_restore_result.txt 2>/dev/null
    fail java-restore 'restore failed'
fi
grep -q '^status=OK$' /tmp/java_restore_result.txt 2>/dev/null || {
    cat /tmp/java-restore.log 2>/dev/null
    cat /tmp/java_restore_result.txt 2>/dev/null
    fail java-restore 'restore did not report OK'
}
[ ! -e "$RW" ] || fail java-restore "restore left $RW"
[ -f "$APPS/vmtest.jar" ] || fail java-restore 'restore removed the app JAR'
echo 'HARNESS_CHECK java-restore PASS'

echo 'HARNESS_CHECK java-runtime PASS'
exit 0
