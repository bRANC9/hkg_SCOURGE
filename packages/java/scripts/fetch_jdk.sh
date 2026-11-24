#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Fetch and verify the pinned Temurin JDK used to build the runtime.
#
# SCOURGE ships no third-party binaries.  The tarball lives in the
# gitignored third_party/ tree and is verified against
# config/third_party.lock before it is extracted or executed.
#
# Usage:
#   sh scripts/fetch_jdk.sh
#   TEMURIN_TARBALL=/path/to/OpenJDK25U-jdk_x64_linux_hotspot_*.tar.gz \
#       sh scripts/fetch_jdk.sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$HERE/lib.sh"

VERSION=$(temurin_version)
BASE_VERSION=$(runtime_version)
URL=$(temurin_url)
WANT_SHA=$(temurin_sha256)
TARBALL_NAME=$(basename "$URL")
CACHE="$JAVA_JDK_STAGE"
TARBALL="$CACHE/$TARBALL_NAME"
JDK_DIR="$CACHE/jdk-$VERSION"

mkdir -p "$CACHE"

sha_ok() {
    [ -f "$1" ] || return 1
    _got=$(sha256sum "$1" | awk '{print $1}')
    [ "$_got" = "$WANT_SHA" ] || {
        echo "ERROR: sha256 mismatch for $1" >&2
        echo "  want $WANT_SHA" >&2
        echo "  got  $_got" >&2
        return 1
    }
}

if [ -n "${TEMURIN_TARBALL:-}" ]; then
    [ -f "$TEMURIN_TARBALL" ] || {
        echo "ERROR: TEMURIN_TARBALL is not a file: $TEMURIN_TARBALL" >&2
        exit 1
    }
    sha_ok "$TEMURIN_TARBALL" || exit 1
    cp -f "$TEMURIN_TARBALL" "$TARBALL"
fi

if ! sha_ok "$TARBALL"; then
    rm -f "$TARBALL"
    echo "== downloading Temurin $VERSION (pinned)"
    echo "   $URL"
    if command -v curl >/dev/null 2>&1; then
        curl -fL --retry 3 --connect-timeout 30 -o "$TARBALL.part" "$URL" || {
            rm -f "$TARBALL.part"
            echo "ERROR: download failed; set TEMURIN_TARBALL=/path/to/$TARBALL_NAME" >&2
            exit 1
        }
    elif command -v wget >/dev/null 2>&1; then
        wget -O "$TARBALL.part" "$URL" || {
            rm -f "$TARBALL.part"
            echo "ERROR: download failed; set TEMURIN_TARBALL=/path/to/$TARBALL_NAME" >&2
            exit 1
        }
    else
        echo "ERROR: neither curl nor wget is available" >&2
        echo "  Set TEMURIN_TARBALL=/path/to/$TARBALL_NAME" >&2
        exit 1
    fi
    mv -f "$TARBALL.part" "$TARBALL"
    sha_ok "$TARBALL" || exit 1
fi

if [ ! -x "$JDK_DIR/bin/jlink" ]; then
    echo "== extracting $TARBALL_NAME"
    rm -rf "$JDK_DIR"
    # Refuse absolute paths and traversal before extracting.
    if tar -tzf "$TARBALL" | grep -Eq '(^/|(^|/)\.\.(/|$))'; then
        echo "ERROR: unsafe path in $TARBALL" >&2
        exit 1
    fi
    mkdir -p "$CACHE/extract.$$"
    tar -xzf "$TARBALL" --no-same-owner -C "$CACHE/extract.$$"
    _top=$(find "$CACHE/extract.$$" -mindepth 1 -maxdepth 1 -type d -name 'jdk-*' | head -1)
    [ -n "$_top" ] || {
        echo "ERROR: tarball did not contain a jdk-* directory" >&2
        rm -rf "$CACHE/extract.$$"
        exit 1
    }
    rm -rf "$JDK_DIR"
    mv "$_top" "$JDK_DIR"
    rm -rf "$CACHE/extract.$$"
fi

[ -x "$JDK_DIR/bin/jlink" ] || {
    echo "ERROR: $JDK_DIR/bin/jlink is missing" >&2
    exit 1
}
grep -q "JAVA_VERSION=\"$BASE_VERSION\"" "$JDK_DIR/release" || {
    echo "ERROR: extracted JDK version does not match $BASE_VERSION" >&2
    grep '^JAVA_VERSION=' "$JDK_DIR/release" >&2 || true
    exit 1
}
ln -sfn "jdk-$VERSION" "$CACHE/current"
echo "JDK_HOME=$JDK_DIR"
echo "JDK_SHA256=$WANT_SHA"
