#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Package the Ambience app for the SCOURGE Hub.
#
# The hub imports a package from a GitHub release: a `<name>.tar.gz` asset
# whose root is this USB payload tree, plus a `<name>.json` sidecar that fills
# the store page and pins the sha256.  This writes all three into dist/.
#
#   sh scripts/package_release.sh 1.0.0 [outdir]
#
# Upload ambient-<version>.tar.gz, its .sha256 and ambient-<version>.json to
# the release, then let the hub sync the repo (see the hub README).
set -eu
PATH=/bin:/sbin:/usr/bin:/usr/sbin

HERE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=${1:-}
[ -n "$VERSION" ] || { echo "usage: sh scripts/package_release.sh <version> [outdir]" >&2; exit 2; }
OUT=${2:-$HERE/dist}
FIRMWARE=${AMBIENT_FIRMWARE:-NE_E.EUR.S5W_L.001.001.260128}
REGION=${AMBIENT_REGION:-EU}
GUI=${AMBIENT_GUI:-qml_eco3.0}
SLUG=${AMBIENT_PKG:-ambient}

case "$VERSION" in
    *[!0-9A-Za-z._-]*) echo "version has unsupported characters: $VERSION" >&2; exit 2 ;;
esac

mkdir -p "$OUT"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/ambient-release.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

echo "== building the payload"
sh "$HERE/scripts/build_payload.sh" "$WORK/payload" >/dev/null

echo "== building the icons"
if python3 -c "import PIL" 2>/dev/null; then
    python3 "$HERE/scripts/make_icons.py" >/dev/null
else
    echo "   Pillow not available; using the committed icons"
fi

NAME="$SLUG-$VERSION"
TARBALL="$OUT/$NAME.tar.gz"

echo "== tarball: $TARBALL"
( cd "$WORK/payload" && find . -type f -print | LC_ALL=C sort | sed 's|^\./||' > "$WORK/files.txt" )
tar -czf "$TARBALL" -C "$WORK/payload" --owner=0 --group=0 \
    --mtime='2026-01-01 00:00:00 UTC' -T "$WORK/files.txt"
SHA=$(sha256sum "$TARBALL" | cut -d' ' -f1)
printf '%s  %s\n' "$SHA" "$NAME.tar.gz" > "$TARBALL.sha256"

SIZE=$(stat -c%s "$TARBALL")
SIDECAR="$OUT/$NAME.json"
cat > "$SIDECAR" <<EOF
{
  "pkg": "$SLUG",
  "version": "$VERSION",
  "firmware": "$FIRMWARE",
  "region": "$REGION",
  "gui": "$GUI",
  "sha256": "$SHA",
  "size": $SIZE,
  "name": "Ambience",
  "short_description": "Ambient lighting with effects, transitions and a music-reactive mode",
  "description": "Takes over ambient lighting control with nine effects (solid, breathe, beat, rainbow, wave, comet, sparkle, cycle), three transition styles, up to four colour zones and an optional music-reactive mode driven from a raw audio tap. Output goes to an aftermarket RGB(W) controller on the LAN (WLED-compatible JSON) or through the micom with a template you supply; nothing is lit until you pick an output. Host-tested only - no car has run it yet.",
  "category": "lighting",
  "homepage": "https://github.com/nickles-lee/hkg_SCOURGE/tree/main/packages/ambient",
  "notes": "First release: effect engine, transitions, presets, music-reactive analysis, hub installer with tile+page injection."
}
EOF

echo
echo "release files:"
echo "  $TARBALL"
echo "  $TARBALL.sha256"
echo "  $SIDECAR"
echo "sha256: $SHA"
