#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Run the java-runtime scenario in Mode V (vendor kernel) and, when the
# compat initramfs is prepared, Mode C.  VM backing images are gitignored and
# must be prepared per docs/14; this script never copies or creates them.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/.." && pwd)
REPO=$(CDPATH= cd -- "$ROOT/../.." && pwd)
VM="$REPO/tools/vm"
PAYLOAD="$ROOT/out/vm"

[ -d "$PAYLOAD/install/payload/java" ] || {
    echo "ERROR: VM payload missing; run sh tests/build_vm_payload.sh" >&2
    exit 1
}
[ -f "$VM/bzImage" ] || {
    echo "ERROR: $VM/bzImage missing; prepare the vendor kernel per docs/14" >&2
    exit 1
}
[ -f "$VM/rootfs.img" ] || {
    echo "ERROR: $VM/rootfs.img missing; prepare the decrypted rootfs per docs/14" >&2
    exit 1
}

set +e
echo "== Mode V: vendor 4.1.27 kernel"
python3 -B "$VM/launch.py" run \
    --kernel vendor --profile research --scenario java-runtime \
    --payload "$PAYLOAD" --timeout 420 --label java-runtime-vendor
VENDOR_RC=$?

if [ -f "$VM/compat/initramfs-linux.img" ]; then
    echo "== Mode C: compat kernel"
    python3 -B "$VM/launch.py" run \
        --kernel compat --profile research --scenario java-runtime \
        --payload "$PAYLOAD" --timeout 420 --label java-runtime-compat
    COMPAT_RC=$?
else
    echo "SKIP Mode C: $VM/compat/initramfs-linux.img not prepared"
    COMPAT_RC=0
fi
set -e

[ "$VENDOR_RC" = 0 ] || exit "$VENDOR_RC"
[ "$COMPAT_RC" = 0 ] || exit "$COMPAT_RC"
echo "JAVA_VM_TEST_OK"
