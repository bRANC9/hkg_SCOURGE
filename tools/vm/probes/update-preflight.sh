#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

# Update-media preflight in the vendor rootfs. The default mode mounts the
# stick read-only. UPDATE_PREFLIGHT_NEGATIVE uses the disposable overlay in
# read-write mode, corrupts the package map/manifest, proves the checks reject
# it, then restores the files. It never launches AppUpgrade, decrypts the
# package or stages an update.
set -u
export PATH=/bin:/sbin:/usr/bin:/usr/sbin

fail() {
    echo "HARNESS_FAIL $1"
    exit 1
}

modprobe ehci-pci 2>/dev/null || true
modprobe usb-storage 2>/dev/null || true
modprobe sd_mod 2>/dev/null || true
sleep 2
mkdir -p /media/usb
MOUNT_MODE=ro
[ "${UPDATE_PREFLIGHT_NEGATIVE:-0}" = "1" ] && MOUNT_MODE=rw
MEDIA=
for dev in /dev/sd?; do
    [ -b "$dev" ] || continue
    mount -o "$MOUNT_MODE" "$dev" /media/usb 2>/dev/null || continue
    if [ -f /media/usb/IONIQ5_EU.ver ] \
        && [ -f /media/usb/MANIFEST.sha256 ] \
        && [ -f /media/usb/.lge.upgrade.xml ]; then
        MEDIA=/media/usb
        break
    fi
    umount /media/usb 2>/dev/null || true
done
[ -n "$MEDIA" ] || { echo 'HARNESS_BLOCKED update-preflight stick missing'; exit 77; }
trap 'cd /; umount "$MEDIA" 2>/dev/null || true' EXIT

cd "$MEDIA" || fail 'preflight-cd'
[ -s IONIQ5_EU.ver ] || fail 'preflight-ver-empty'
[ -s MANIFEST.sha256 ] || fail 'preflight-manifest-empty'
[ -s .lge.upgrade.xml ] || fail 'preflight-package-empty'
# The package blob is encrypted/binary, not plain XML.
FIRST=$(od -An -tx1 -N1 .lge.upgrade.xml | tr -d ' \n')
[ "$FIRST" != "3c" ] || fail 'preflight-package-plaintext'
grep -q "IONIQ5_EU.ver" MANIFEST.sha256 || fail 'preflight-manifest-ver'
echo 'HARNESS_CHECK preflight-media PASS'

validate_ver() {
    local file=$1
    local header product entries present path filename ident check size flag
    local rel full actual
    header=$(head -n 1 "$file") || return 1
    case "$header" in
        +\|*) ;;
        *) return 1 ;;
    esac
    [ "$(printf '%s' "$header" | awk -F'|' '{print NF}')" = 7 ] || return 1
    product=$(printf '%s' "$header" | awk -F'|' '{print $5}')
    [ -n "$product" ] || return 1
    if ! tail -n +2 "$file" | awk -F'|' 'NF != 6 {exit 1}'; then
        return 1
    fi
    tail -n +2 "$file" > /tmp/preflight.entries || return 1
    entries=0
    present=0
    while IFS='|' read -r path filename ident check size flag; do
        [ -n "$path" ] || return 1
        case "$path" in
            *..*|/*) return 1 ;;
            "$product"|"$product"\\*) ;;
            *) return 1 ;;
        esac
        case "$filename" in
            ''|.|..|*/*|*\\*) return 1 ;;
        esac
        case "$size" in
            ''|*[!0-9]*) return 1 ;;
        esac
        entries=$((entries + 1))
        rel=${path#"$product"}
        rel=${rel#\\}
        rel=${rel//\\//}
        if [ -n "$rel" ]; then
            full="$rel/$filename"
        else
            full="$filename"
        fi
        if [ -f "$full" ]; then
            actual=$(wc -c < "$full" | tr -d ' ')
            [ "$actual" = "$size" ] || return 1
            present=$((present + 1))
        fi
    done < /tmp/preflight.entries
    [ "$entries" -gt 0 ] || return 1
    [ "$present" -gt 0 ] || return 1
    return 0
}

tr -d '\r' < IONIQ5_EU.ver > /tmp/preflight.ver || fail 'preflight-ver-copy'
validate_ver /tmp/preflight.ver || fail 'preflight-ver'
echo 'HARNESS_CHECK preflight-ver PASS'


tr -d '\r' < MANIFEST.sha256 > /tmp/preflight.manifest || fail 'preflight-manifest-copy'
if ! awk 'NF != 2 || length($1) != 64 || $1 !~ /^[0-9a-f]+$/ {exit 1}' \
    /tmp/preflight.manifest; then
    fail 'preflight-manifest-format'
fi
if ! /bin/busybox sha256sum -c /tmp/preflight.manifest >/tmp/preflight-hashes.log 2>&1; then
    tail -20 /tmp/preflight-hashes.log
    fail 'preflight-manifest-hash'
fi
echo 'HARNESS_CHECK preflight-manifest PASS'

PAYLOAD=HU/images/mango-rootfs.tar.gz
[ -s "$PAYLOAD" ] || fail 'preflight-payload-missing'
# The real package payload is encrypted/opaque despite the .tar.gz name.
HEAD=$(od -An -tx1 -N2 "$PAYLOAD" | tr -d ' \n')
[ "$HEAD" != "1f8b" ] || fail 'preflight-payload-plain-gzip'
USTAR=$(od -An -tx1 -j 257 -N 5 "$PAYLOAD" | tr -d ' \n')
[ "$USTAR" != "7573746172" ] || fail 'preflight-payload-plain-tar'
echo 'HARNESS_CHECK preflight-payload PASS'

# Nothing may have launched the updater or staged an update.
if ps -ef | grep -E 'AppUpgrade|dmengine' | grep -v grep >/tmp/preflight-processes.log 2>&1; then
    cat /tmp/preflight-processes.log
    fail 'preflight-updater-running'
fi
[ ! -e /tmp/AppUpgrade ] || fail 'preflight-updater-staged'

if [ "${UPDATE_PREFLIGHT_NEGATIVE:-0}" = "1" ]; then
    cp IONIQ5_EU.ver /tmp/negative-ver.bak || fail 'negative-backup-ver'
    cp MANIFEST.sha256 /tmp/negative-manifest.bak || fail 'negative-backup-manifest'

    awk -F'|' 'BEGIN{OFS="|"} /mango-rootfs/ {$5=$5+1} {print}' \
        /tmp/negative-ver.bak > /tmp/negative-size.ver
    if validate_ver /tmp/negative-size.ver; then
        fail 'negative-size-not-detected'
    fi
    echo 'HARNESS_CHECK negative-size-detected PASS'

    { cat /tmp/negative-ver.bak; \
      echo 'IONIQ5_EU\..\evil|payload|1|1|1|1'; } > /tmp/negative-path.ver
    if validate_ver /tmp/negative-path.ver; then
        fail 'negative-path-not-detected'
    fi
    echo 'HARNESS_CHECK negative-path-detected PASS'

    awk '{ if (NR==1) { h=$1; c=substr(h,1,1); r=(c=="0"?"1":"0");
                        $1=r substr(h,2) } print }' \
        /tmp/negative-manifest.bak > /tmp/negative-manifest.sha256
    if /bin/busybox sha256sum -c /tmp/negative-manifest.sha256 \
        >/tmp/negative-manifest.log 2>&1; then
        fail 'negative-manifest-not-detected'
    fi
    echo 'HARNESS_CHECK negative-manifest-detected PASS'

    cp /tmp/negative-ver.bak IONIQ5_EU.ver || fail 'negative-restore-ver'
    cp /tmp/negative-manifest.bak MANIFEST.sha256 || fail 'negative-restore-manifest'
    sync
    if ! /bin/busybox sha256sum -c MANIFEST.sha256 >/tmp/negative-restore.log 2>&1; then
        tail -20 /tmp/negative-restore.log
        fail 'negative-restore-hash'
    fi
    validate_ver /tmp/preflight.ver || fail 'negative-restore-size'
    echo 'HARNESS_CHECK negative-restored PASS'
fi

echo 'HARNESS_CHECK preflight-no-exec PASS'
