#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

# Vendor-kernel device-model regression. This asserts the QEMU substrate the
# harness relies on, not the captured BXT hardware.
set -u
export PATH=/bin:/sbin:/usr/bin:/usr/sbin

fail() {
    echo "HARNESS_FAIL $1"
    exit 1
}

PCI=$(lspci -nn 2>/dev/null) || fail 'device-lspci'
BLOCK=$(lsblk -o NAME,SIZE,TYPE,MOUNTPOINT 2>/dev/null) || fail 'device-lsblk'
printf '%s\n' "$PCI" >/tmp/device-model-pci.txt
printf '%s\n' "$BLOCK" >/tmp/device-model-block.txt

if ! printf '%s\n' "$PCI" | grep -q '8086:29c0'; then
    fail 'device-host'
fi
echo 'HARNESS_CHECK device-host PASS'

if ! printf '%s\n' "$PCI" | grep -q '1b36:0007'; then
    fail 'device-sdhci'
fi
echo 'HARNESS_CHECK device-sdhci PASS'

if ! printf '%s\n' "$PCI" | grep -q '8086:2918' \
    || ! printf '%s\n' "$PCI" | grep -q '8086:2922'; then
    fail 'device-lpc-sata'
fi
echo 'HARNESS_CHECK device-lpc-sata PASS'

if ! printf '%s\n' "$BLOCK" | grep -q 'mmcblk0.*disk.* /'; then
    fail 'device-rootfs'
fi
echo 'HARNESS_CHECK device-rootfs PASS'

if printf '%s\n' "$PCI" | grep -q '1af4:'; then
    fail 'device-no-virtio'
fi
echo 'HARNESS_CHECK device-no-virtio PASS'
echo 'HARNESS_CHECK device-model PASS'
