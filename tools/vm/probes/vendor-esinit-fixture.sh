#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

# Preparation for vendor-esinit-fixture.
#
# The captured esinit blocks in modprobe fan: lge-fan's probe sleeps in
# __driver_attach without micom/thermal hardware. This lab fixture preserves
# the real kmod entry as /sbin/modprobe.real and installs a wrapper that skips
# only that one gate; every other modprobe call is delegated unchanged.
#
# This is not the vendor initramfs and not stock boot equivalence: it bypasses
# a hardware gate rather than emulating it.
set -eu
export PATH=/bin:/sbin:/usr/bin:/usr/sbin

# The missing vendor initramfs normally creates the /var skeleton and the
# messagebus identity. Reuse the same lab layout as vendor-systemd-dbus.
mkdir -p /rw_data/var /etc/tmpfiles.d /var/volatile/log /var/volatile/tmp \
         /var/log /var/cache /var/lib /var/spool /run/lock
for link in /rw_data/var/run /rw_data/var/lock; do
    if [ -d "$link" ] && [ ! -L "$link" ]; then
        rm -rf "$link" || { echo "P28_PREP_FAIL cleanup $link"; exit 1; }
    fi
done
ln -sfn /run /rw_data/var/run || { echo 'P28_PREP_FAIL run-link'; exit 1; }
ln -sfn /run/lock /rw_data/var/lock || { echo 'P28_PREP_FAIL lock-link'; exit 1; }
if [ ! -f /tmp/passwd ]; then
    cp /etc/passwd.legacy /tmp/passwd || { echo 'P28_PREP_FAIL passwd'; exit 1; }
fi
grep -q '^messagebus:' /tmp/passwd 2>/dev/null || \
    echo 'messagebus:x:998:998:messagebus:/run/dbus:/bin/false' >> /tmp/passwd
printf 'd /run/dbus 0755 - - -\n' > /etc/tmpfiles.d/zz-p28-dbus.conf || {
    echo 'P28_PREP_FAIL tmpfiles'
    exit 1
}

# /etc/fstab is a symlink to /tmp/fstab; esinit points it at the vendor eMMC
# fstab, whose missing partitions make systemd-remount-fs fail into emergency
# mode. This fixture supplies an empty regular fstab instead.
rm -f /etc/fstab
: > /etc/fstab || { echo 'P28_PREP_FAIL fstab'; exit 1; }

if [ ! -e /sbin/modprobe.real ] && [ ! -L /sbin/modprobe.real ]; then
    mv /sbin/modprobe /sbin/modprobe.real || {
        echo 'P28_PREP_FAIL preserve-modprobe'
        exit 1
    }
fi
cat > /sbin/modprobe <<'WRAPPER'
#!/bin/sh
[ "$1" = fan ] && exit 0
exec /sbin/modprobe.real "$@"
WRAPPER
chmod 0755 /sbin/modprobe || {
    echo 'P28_PREP_FAIL wrapper-mode'
    exit 1
}
[ -e /sbin/modprobe.real ] || {
    echo 'P28_PREP_FAIL wrapper-target'
    exit 1
}
sync
echo 'P28_PREP_OK'
