#!/bin/sh
# Stage-3 USB failsafe loader.
#
# Runs as root from initial-root-failsafe.service, which is ordered
# After=local-fs.target and Before=multi-user.target + ApplicationLauncher
# .service, so it comes up BEFORE the UI and keeps running no matter what
# the UI does. Nothing below depends on weston, AppLauncher, D-Bus or
# usb-automount; if the automounter is dead the raw block-device scan
# mounts the stick itself.
#
# Jobs:
#   1. Best-effort bring-up of the USB storage stack (modprobe is a no-op
#      when the modules are already loaded).
#   2. Re-apply the ten updater/launcher stubs (idempotent, survives a
#      stock updater being copied back).
#   3. Poll the HU's USB mounts AND raw block devices for run_stuff.sh and
#      run the first changed one as root (md5-gated so a left-in stick
#      does not re-run every poll).
#
# Contract for run_stuff.sh:
#   USB_DIR  mount directory of the stick (write results back here when
#            the mount is writable)
#   USB_RW   same path; set only for informational use
#   STAGE3   1 (loader generation marker)
#   Execution: /bin/sh as root within ~8 s of a content change.
PATH=/bin:/sbin:/usr/bin:/usr/sbin
STATE=/run/initial_root.lastmd5
LOG=/rw_data/initial_root.log
[ -d /rw_data ] || LOG=/tmp/initial_root.log
TMPMNT=/run/initial_root_usb
GUARD=/etc/initial_root/updater_guard.sh

log() {
    echo "$(date -u 2>/dev/null) $*" >> "$LOG" 2>/dev/null || true
}

# 0. Boot marker: proves the failsafe started, independent of the UI.
{
    echo "stage3 failsafe start $(date -u 2>/dev/null)"
    echo "uptime_sec: $(cut -d. -f1 /proc/uptime 2>/dev/null)"
    echo "uid: $(id -u 2>/dev/null)"
} > /rw_data/initial_root_boot.txt 2>/dev/null || true

# 1. USB stack, best effort (no-op when already loaded).
modprobe ehci-pci 2>/dev/null
modprobe xhci_hcd 2>/dev/null
modprobe usb-storage 2>/dev/null
modprobe sd_mod 2>/dev/null
modprobe texfat 2>/dev/null
modprobe vfat 2>/dev/null
modprobe msdos 2>/dev/null

# 2. Keep the ten updater/launcher stubs in place.
if [ -f "$GUARD" ]; then
    /bin/sh "$GUARD" >> "$LOG" 2>&1 || true
fi

run_if_new() {
    s="$1"
    [ -f "$s" ] || return 1
    m=$(md5sum "$s" 2>/dev/null | cut -d' ' -f1)
    [ -n "$m" ] || return 1
    [ "$m" = "$(cat "$STATE" 2>/dev/null)" ] && return 1
    d=$(dirname "$s")
    log "=== run $s md5=$m usb=$d ==="
    USB_DIR="$d" USB_RW="$d" STAGE3=1 /bin/sh "$s" >> "$LOG" 2>&1
    rc=$?
    log "=== rc=$rc ==="
    echo "$m" > "$STATE" 2>/dev/null || true
    return 0
}

scan_mounts() {
    for s in /media/usb/*/*/run_stuff.sh /media/usb/*/run_stuff.sh \
             /media/usb/run_stuff.sh /media/*/run_stuff.sh \
             /run/media/*/run_stuff.sh /run/media/*/*/run_stuff.sh \
             /mnt/*/run_stuff.sh; do
        run_if_new "$s" && return 0
    done
    return 1
}

try_mount() {
    # Plain mount first (ext4/vfat/ntfs when the type is detectable), then
    # explicit types: the car's exFAT is the Tuxera driver and reports as
    # "texfat", which a type-guessing mount can miss.
    d=$1; m=$2; mode=$3
    mount -o "$mode" "$d" "$m" 2>/dev/null && return 0
    mount -t texfat -o "$mode" "$d" "$m" 2>/dev/null && return 0
    mount -t exfat -o "$mode" "$d" "$m" 2>/dev/null && return 0
    mount -t vfat -o "$mode" "$d" "$m" 2>/dev/null && return 0
    return 1
}

scan_raw() {
    for d in /dev/sd?1 /dev/sd?; do
        [ -b "$d" ] || continue
        grep -q "^$d " /proc/mounts && continue
        mkdir -p "$TMPMNT" 2>/dev/null
        # rw first so run_stuff.sh can write results back to the stick;
        # ro when the filesystem or policy refuses rw.
        try_mount "$d" "$TMPMNT" rw || try_mount "$d" "$TMPMNT" ro || continue
        run_if_new "$TMPMNT/run_stuff.sh"
        r=$?
        umount "$TMPMNT" 2>/dev/null
        [ "$r" = 0 ] && return 0
    done
    return 1
}

log "loader start (stage3)"
while true; do
    scan_mounts || scan_raw
    sleep 8
done
