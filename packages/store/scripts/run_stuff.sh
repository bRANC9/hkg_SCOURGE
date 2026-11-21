#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TWEAKS_PAYLOAD_REV=1
# TWEAKS_PAYLOAD_ID=dev
# SCOURGE Marketplace agent: installs store_daemon + the service on the unit.
#
# Installs:
#   /rw_data/store/store_daemon
#   /etc/systemd/system/store-daemon.service
#   /rw_data/store/{hub,firmware,region,gui} (only when provided on the stick)
# Starts the daemon on 127.0.0.1:8092. It does not touch QML, wrandr or the
# updater; the Marketplace UI is a separate QML page that talks to :8092.
set -u
PATH="${TWEAKS_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}"
USB_DIR="${USB_DIR:-/media/usb}"
PAYLOAD="$USB_DIR/payload"
BACKUP="$USB_DIR/backup"
RW_DIR="${STORE_RW_DIR:-/rw_data/store}"
SERVICE="${STORE_SERVICE:-/etc/systemd/system/store-daemon.service}"
DAEMON="$RW_DIR/store_daemon"
LOG="$RW_DIR/install.log"
OUT="$USB_DIR/run_stuff_result.txt"
SMACK_SELF="${STORE_SMACK_SELF:-}"

mkdir -p "$RW_DIR" 2>/dev/null || { echo "cannot create $RW_DIR" >&2; exit 1; }
: > "$LOG" 2>/dev/null

log() { echo "$(date -u 2>/dev/null) $*" >> "$LOG" 2>/dev/null; }
result() {
    { echo "SCOURGE_STORE"; echo "time=$(date -u 2>/dev/null)"; echo "uid=$(id -u 2>/dev/null)";
      echo "usb=$USB_DIR"; for line in "$@"; do echo "$line"; done; } > "$OUT" 2>&1
    cp -f "$OUT" "$RW_DIR/result.txt" 2>/dev/null || true
    sync
}
fail() { log "FAIL: $*"; result "status=FAIL" "error=$*"; echo "FAIL: $*" >&2; exit 1; }

mount -o remount,rw "$USB_DIR" 2>/dev/null || true
[ -d "$PAYLOAD" ] || fail "payload directory missing"
[ -f "$PAYLOAD/bin/store_daemon" ] || fail "store_daemon missing from the payload"
[ -d "$BACKUP" ] || mkdir -p "$BACKUP" 2>/dev/null || fail "no backup dir"

# ---- daemon ------------------------------------------------------------
if [ -e "$DAEMON" ]; then
    cp -f "$DAEMON" "$RW_DIR/store_daemon.prev" 2>/dev/null || true
fi
rm -f "$DAEMON" 2>/dev/null || true
cp -f "$PAYLOAD/bin/store_daemon" "$DAEMON" || fail "daemon copy failed"
chmod 755 "$DAEMON" 2>/dev/null || true
chown 0:0 "$DAEMON" 2>/dev/null || true

# The daemon must run on the installer's SMACK label (the vendor floor label
# `_` on the normal USB flow), the same way the other package daemons do.
if [ -n "${STORE_SMACK_SELF:-}" ]; then
    SMACK_SELF="$STORE_SMACK_SELF"
elif [ -r /proc/self/attr/current ]; then
    SMACK_SELF=$(cat /proc/self/attr/current 2>/dev/null | tr -d '\r\n')
fi
smack_state="absent"
if [ -n "$SMACK_SELF" ] && command -v chsmack >/dev/null 2>&1; then
    if chsmack -e "$SMACK_SELF" "$DAEMON" >> "$LOG" 2>&1; then
        smack_state="$SMACK_SELF"
    else
        fail "cannot stamp the SMACK exec label"
    fi
fi

# ---- key + hub ---------------------------------------------------------
for key in hub firmware region gui; do
    src="$PAYLOAD/$key.txt"
    [ -f "$src" ] || continue
    cp -f "$src" "$RW_DIR/$key" || fail "cannot write $RW_DIR/$key"
    chmod 600 "$RW_DIR/$key" 2>/dev/null || true
done

# ---- service -----------------------------------------------------------
[ -f "$PAYLOAD/systemd/store-daemon.service" ] || fail "service unit missing"
cp -f "$PAYLOAD/systemd/store-daemon.service" "$SERVICE" || fail "service copy failed"
chmod 644 "$SERVICE" 2>/dev/null || true
chown 0:0 "$SERVICE" 2>/dev/null || true

started="not-started"
if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload >> "$LOG" 2>&1 || true
    if systemctl enable store-daemon.service >> "$LOG" 2>&1 &&
       systemctl restart store-daemon.service >> "$LOG" 2>&1; then
        started="systemd"
    fi
fi
if [ "$started" = "not-started" ]; then
    ( "$DAEMON" --rw-dir "$RW_DIR" >> "$RW_DIR/daemon.log" 2>&1 & ) 2>/dev/null && started="manual"
fi

healthy="no"
i=0
while [ "$i" -lt 20 ]; do
    if command -v wget >/dev/null 2>&1; then
        wget -q -O - http://127.0.0.1:8092/health >/dev/null 2>&1 && healthy="yes" && break
    elif [ -e /proc/net/tcp ]; then
        healthy="unverified"
        break
    fi
    i=$((i + 1))
    sleep 0.2 2>/dev/null || sleep 1
done

log "installed daemon, service=$started, health=$healthy, smack=$smack_state"
result "status=OK" "daemon=$started" "health=$healthy" "smack=$smack_state" \
       "hub=$(cat "$RW_DIR/hub" 2>/dev/null || echo unset)" \
       "listen=127.0.0.1:8092" "log=$LOG"
