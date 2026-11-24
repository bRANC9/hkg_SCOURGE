#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

# TARGET: Ioniq 5 Gen5W_L, 260128 (EU)
# Research input bring-up for Mode C compat. The vendor rootfs ships no
# udevadm/systemd-udevd, so the QEMU tablet/keyboard are untagged. Write the
# /run/udev/data tags and verify Weston creates the devices. No OEM binary is
# modified; this is a lab input fixture, not stock input fidelity.
set -u
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
mkdir -p /run/udev/data /run/user/root
chmod 700 /run/user/root
export XDG_RUNTIME_DIR=/run/user/root

tag() {
    dev=$1
    prop=$2
    [ -e "/sys/class/input/$dev/dev" ] || return 0
    major_minor=$(cat "/sys/class/input/$dev/dev")
    minor=${major_minor#*:}
    {
        printf 'E:ID_INPUT=1\n'
        printf 'E:ID_SEAT=seat0\n'
        printf 'E:%s=1\n' "$prop"
        printf 'G:seat\n'
    } > "/run/udev/data/c13:$minor"
}

tag event0 ID_INPUT_KEY
tag event1 ID_INPUT_KEYBOARD
tag event2 ID_INPUT_MOUSE
tag event3 ID_INPUT_KEYBOARD

printf '[core]\nshell=fullscreen-shell.so\n' >/tmp/weston-input.ini
/usr/bin/weston --backend=drm-backend.so --use-pixman --tty=1 \
    --config=/tmp/weston-input.ini --socket=wayland-input --idle-time=0 --debug \
    </dev/tty1 >/tmp/weston-input.log 2>&1 &
sleep 6

if ! pidof weston >/dev/null 2>&1; then
    echo 'HARNESS_FAIL weston did not start'
    tail -40 /tmp/weston-input.log
    exit 1
fi

if ! grep -q "QEMU Virtio Tablet.*is a pointer caps" /tmp/weston-input.log; then
    echo 'HARNESS_FAIL tablet not tagged as pointer'
    grep -a 'QEMU Virtio Tablet' /tmp/weston-input.log | head -4
    exit 1
fi

if ! grep -q "QEMU Virtio Keyboard.*is a keyboard" /tmp/weston-input.log; then
    echo 'HARNESS_FAIL keyboard not tagged'
    grep -a 'QEMU Virtio Keyboard' /tmp/weston-input.log | head -4
    exit 1
fi

grep -aE 'QEMU Virtio (Tablet|Keyboard)' /tmp/weston-input.log | head -4
echo 'HARNESS_CHECK stock-input PASS'
exit 0
