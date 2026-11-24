#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Ioniq 5 Gen5W_L EU 260128
# Remove the Java runtime installed by run_stuff.sh.
#
# This removes /rw_data/java (or JAVA_RW_DIR) and deliberately preserves
# /rw_data/java-apps (or JAVA_APPS_DIR), where the example unit expects user
# JARs.  It touches no /app, /etc, updater or bootloader content.
set -u
PATH="${JAVA_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}"
USB_DIR="${USB_DIR:-/media/usb}"

# Canonicalize before any check or delete.  A literal-string guard is bypassed
# by /rw_data/, /rw_data/. and /rw_data/java/..; readlink -f resolves those
# (and symlinks), and every failure is a hard stop.
canon_dir() {
    _input=$1
    [ -n "$_input" ] || return 1
    case "$_input" in
        /*) ;;
        *) return 1 ;;
    esac
    _trim=$_input
    while [ "$_trim" != "/" ] && [ "${_trim%/}" != "$_trim" ]; do
        _trim=${_trim%/}
    done
    _canon=$(readlink -f -- "$_trim" 2>/dev/null) || return 1
    [ -n "$_canon" ] || return 1
    printf '%s\n' "$_canon"
}

RW_DIR=$(canon_dir "${JAVA_RW_DIR:-/rw_data/java}") || {
    echo "cannot canonicalize JAVA_RW_DIR: ${JAVA_RW_DIR:-/rw_data/java}" >&2
    exit 1
}
APPS_DIR=$(canon_dir "${JAVA_APPS_DIR:-/rw_data/java-apps}") || {
    echo "cannot canonicalize JAVA_APPS_DIR: ${JAVA_APPS_DIR:-/rw_data/java-apps}" >&2
    exit 1
}

OUT="$USB_DIR/run_stuff_result.txt"
LOCAL_RESULT="/tmp/java_restore_result.txt"

result() {
    {
        echo "JAVA_RESTORE"
        echo "time=$(date -u 2>/dev/null)"
        echo "usb=$USB_DIR"
        echo "rw=$RW_DIR"
        echo "apps=$APPS_DIR"
        _line=
        for _line in "$@"; do
            echo "$_line"
        done
    } > "$LOCAL_RESULT" 2>&1 || true
    cp -f "$LOCAL_RESULT" "$OUT" 2>/dev/null || true
}

STATUS=OK
ERROR=""
if [ -z "$RW_DIR" ] || [ "$RW_DIR" = "/" ] || [ "$RW_DIR" = "/rw_data" ]; then
    STATUS=FAIL
    ERROR="refusing to remove '$RW_DIR'"
fi

if [ "$STATUS" = OK ]; then
    case "$APPS_DIR/" in
        "$RW_DIR/"*)
            STATUS=FAIL
            ERROR="JAVA_APPS_DIR must be outside JAVA_RW_DIR"
            ;;
    esac
fi
if [ "$STATUS" = OK ]; then
    case "$RW_DIR/" in
        "$APPS_DIR/"*)
            STATUS=FAIL
            ERROR="JAVA_RW_DIR must be outside JAVA_APPS_DIR"
            ;;
    esac
fi

if [ "$STATUS" = OK ] && [ -e "$RW_DIR" ]; then
    if [ ! -f "$RW_DIR/.scourge-java" ]; then
        STATUS=FAIL
        ERROR="refusing to remove '$RW_DIR': missing .scourge-java marker"
    elif [ "$(cat "$RW_DIR/.scourge-java" 2>/dev/null)" != \
           "SCOURGE_JAVA_RUNTIME" ]; then
        STATUS=FAIL
        ERROR="refusing to remove '$RW_DIR': invalid .scourge-java marker"
    fi
fi

if [ "$STATUS" = OK ] && [ -e "$RW_DIR" ]; then
    rm -rf "$RW_DIR" || {
        STATUS=FAIL
        ERROR="cannot remove $RW_DIR"
    }
fi

sync 2>/dev/null || true
if [ "$STATUS" = OK ]; then
    result "status=OK" "restored=1" "removed=$RW_DIR" "apps_kept=$APPS_DIR"
    exit 0
fi
result "status=FAIL" "error=$ERROR"
exit 1
