#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

# TARGET: Ioniq 5 Gen5W_L, 260128 (EU)
# Research persistence-admin bring-up for Mode C compat. The vendor rootfs
# ships /usr/bin/pers_admin_svc; start it on a guest system bus the app's
# PersistentManager can reach. No OEM binary is modified; this is a lab
# service fixture, not a stock vehicle claim.
set -u
export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/app/bin:/usr/bin/qt5
export LD_LIBRARY_PATH=/app/lib:/usr/lib

mkdir -p /run/dbus /tmp/PAS
cp /etc/passwd.legacy /tmp/passwd 2>/dev/null || true

dbus-daemon --system --address=unix:path=/run/dbus/system_bus_socket \
    --nopidfile --fork --print-address 2>/dev/null || {
    echo 'HARNESS_FAIL dbus-daemon did not start'
    exit 1
}

export DBUS_SYSTEM_BUS_ADDRESS=unix:path=/run/dbus/system_bus_socket
export PERS_CLIENT_DBUS_ADDRESS=unix:path=/run/dbus/system_bus_socket

/usr/bin/pers_admin_svc >/tmp/pers-admin.out 2>&1 &
sleep 20

# ListNames prints each name as `string "name"`; anchor the quotes so the
# `admin` assertion cannot be satisfied by `admin_svc` alone.
own_name() {
    dbus-send --system --dest=org.freedesktop.DBus --type=method_call \
        --print-reply /org/freedesktop/DBus org.freedesktop.DBus.ListNames 2>/dev/null \
        | grep -aqF "string \"$1\""
}

if ! own_name org.genivi.persistence.admin; then
    echo 'HARNESS_FAIL persistence admin name not owned'
    tail -30 /tmp/pers-admin.out
    exit 1
fi

if ! own_name org.genivi.persistence.admin_svc; then
    echo 'HARNESS_FAIL persistence admin_svc name not owned'
    exit 1
fi

echo 'HARNESS_CHECK stock-pers PASS'
exit 0
