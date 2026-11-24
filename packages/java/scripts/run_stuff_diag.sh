#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Ioniq 5 Gen5W_L EU 260128
# Read-only Java runtime diagnostic.  Writes java_diag.txt to the stick root.
set -u
PATH="${JAVA_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}"
USB_DIR="${USB_DIR:-/media/usb}"
RW_DIR="${JAVA_RW_DIR:-/rw_data/java}"
APPS_DIR="${JAVA_APPS_DIR:-/rw_data/java-apps}"
OUT="$USB_DIR/java_diag.txt"
JAVA="$RW_DIR/current/bin/java"

{
    echo "JAVA_DIAG"
    echo "time=$(date -u 2>/dev/null)"
    echo "uid=$(id -u 2>/dev/null)"
    echo "usb=$USB_DIR"
    echo "rw=$RW_DIR"
    echo "apps=$APPS_DIR"
    echo "smack_self=$(cat /proc/self/attr/current 2>/dev/null | tr -d '\r\n')"
    echo
    echo "== paths =="
    ls -ld "$RW_DIR" "$RW_DIR/current" "$JAVA" "$APPS_DIR" 2>&1
    readlink -f "$RW_DIR/current" 2>/dev/null || true
    echo
    echo "== space =="
    df -Pk "$RW_DIR" 2>&1
    du -sk "$RW_DIR" 2>/dev/null || true
    echo
    echo "== smack =="
    for f in "$JAVA" "$RW_DIR/current/bin/jcmd"; do
        [ -e "$f" ] || continue
        echo "$f: $(getfattr -n security.SMACK64EXEC --only-values "$f" 2>/dev/null || echo none)"
    done
    echo
    echo "== version =="
    if [ -x "$JAVA" ]; then
        "$JAVA" -version 2>&1
        echo
        echo "== modules =="
        "$JAVA" --list-modules 2>&1
        echo
        echo "== jcmd =="
        "$RW_DIR/current/bin/jcmd" -l 2>&1 || true
    else
        echo "not installed"
    fi
    echo
    echo "status=OK"
} > "$OUT" 2>&1
cp -f "$OUT" /tmp/java_diag.txt 2>/dev/null || true
exit 0
