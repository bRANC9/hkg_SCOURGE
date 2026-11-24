# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Shared paths and lock helpers for packages/java.
set -eu

JAVA_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
JAVA_REPO=$(CDPATH= cd -- "$JAVA_ROOT/../.." && pwd)
JAVA_LOCK="$JAVA_REPO/config/third_party.lock"
JAVA_JDK_STAGE="$JAVA_REPO/third_party/temurin"
JAVA_OUT="$JAVA_ROOT/out/usb"
JAVA_VM_OUT="$JAVA_ROOT/out/vm"

lock_section() {
    # Print the [temurin-jdk25] section of config/third_party.lock.
    awk '
        /^\[temurin-jdk25\]/ { found = 1; next }
        /^\[/ { found = 0 }
        found { print }
    ' "$JAVA_LOCK"
}

lock_value() {
    # lock_value KEY -> first non-empty value in the Temurin section.
    _section=$(lock_section)
    printf '%s\n' "$_section" |
        sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*//p" |
        head -1
}

temurin_version() {
    _version=$(lock_value version)
    [ -n "$_version" ] || {
        echo "ERROR: no [temurin-jdk25] version in $JAVA_LOCK" >&2
        exit 1
    }
    printf '%s\n' "$_version"
}

temurin_url() {
    _url=$(lock_value url)
    [ -n "$_url" ] || {
        echo "ERROR: no [temurin-jdk25] url in $JAVA_LOCK" >&2
        exit 1
    }
    printf '%s\n' "$_url"
}

temurin_sha256() {
    _sha=$(lock_value sha256)
    [ -n "$_sha" ] || {
        echo "ERROR: no [temurin-jdk25] sha256 in $JAVA_LOCK" >&2
        exit 1
    }
    printf '%s\n' "$_sha"
}

runtime_version() {
    _version=$(temurin_version)
    # The JDK tarball's release file uses the base version (no +build).
    printf '%s\n' "${_version%%+*}"
}
