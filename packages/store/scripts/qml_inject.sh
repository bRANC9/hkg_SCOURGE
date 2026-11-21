#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Install-time QML injection for packages delivered through the Store.
#
# The Store itself is built like any other package (variant files from the
# owner's base). Anything it installs afterwards has no base and no variant, so
# its installer injects its tile into the live home screen instead:
#
#   sh qml_inject.sh --file /app/share/.../QHMI_HomeScreenMain_Advanced.qml \
#                    --anchor menuPage --block /path/to/MyTile.qml [--dry-run]
#   sh qml_inject.sh --file ... --revert
#
# Safety: brace-matched insertion (no regex surgery), backup next to the file and
# on the stick, a brace-balance check before and after, SMACK64 preserved, and
# an explicit --revert. The anchor is the `id: <anchor>` of the item the tile is
# inserted after, which is the same allocator the build-time variants use.
set -eu
FILE=""
ANCHOR="menuPage"
BLOCK=""
REVERT=0
DRY=0
BACKUP_DIR="${QML_INJECT_BACKUP:-/rw_data/store/qml-backup}"

while [ $# -gt 0 ]; do
    case "$1" in
        --file) FILE=$2; shift 2 ;;
        --anchor) ANCHOR=$2; shift 2 ;;
        --block) BLOCK=$2; shift 2 ;;
        --backup-dir) BACKUP_DIR=$2; shift 2 ;;
        --revert) REVERT=1; shift ;;
        --dry-run) DRY=1; shift ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

[ -n "$FILE" ] || { echo "usage: qml_inject.sh --file <Advanced.qml> [--anchor id] (--block <tile.qml> | --revert) [--dry-run]" >&2; exit 2; }
[ -f "$FILE" ] || { echo "no such QML file: $FILE" >&2; exit 2; }

BASE=$(basename "$FILE")
BACKUP="$BACKUP_DIR/$BASE"

if [ "$REVERT" = 1 ]; then
    [ -f "$BACKUP" ] || { echo "no backup to revert to: $BACKUP" >&2; exit 1; }
    if [ "$DRY" = 1 ]; then
        echo "would restore $FILE from $BACKUP"
        exit 0
    fi
    label=$(getfattr -n security.SMACK64 --only-values "$FILE" 2>/dev/null || true)
    cp -f "$BACKUP" "$FILE"
    [ -n "$label" ] && setfattr -n security.SMACK64 -v "$label" "$FILE" 2>/dev/null || true
    echo "reverted $FILE from $BACKUP"
    exit 0
fi

[ -n "$BLOCK" ] || { echo "--block is required (or --revert)" >&2; exit 2; }
[ -f "$BLOCK" ] || { echo "no such block: $BLOCK" >&2; exit 2; }

braces() {
    # net { } depth of a file, ignoring nothing but CRs
    awk '{ for (i = 1; i <= length($0); i++) { c = substr($0, i, 1); if (c == "{") d++; else if (c == "}") d-- } } END { print d + 0 }' "$1"
}

# A block injected in the middle of a file may not carry imports (they are only
# legal at the top). Strip them, and require the target to already import what
# the block needs - the home screen imports ../component, so a tile that lives
# there resolves like any other component.
BLOCK_CLEAN=$(mktemp "${TMPDIR:-/tmp}/qmlblock.XXXXXX")
grep -v '^[[:space:]]*import[[:space:]]' "$BLOCK" > "$BLOCK_CLEAN" || true
block_imports=$(grep -c '^[[:space:]]*import[[:space:]]' "$BLOCK" 2>/dev/null || true)
[ -n "$block_imports" ] || block_imports=0
if [ "$block_imports" -gt 0 ] && ! grep -q '^[[:space:]]*import[[:space:]]*"\.\./component"' "$FILE"; then
    echo "warning: block had $block_imports import line(s); the target does not import ../component" >&2
fi

before=$(braces "$FILE")
block_depth=$(braces "$BLOCK_CLEAN")

TMP=$(mktemp "${TMPDIR:-/tmp}/qmlinject.XXXXXX")
awk -v anchor="id: $ANCHOR" -v block="$BLOCK_CLEAN" '
BEGIN { started = 0; done = 0; depth = 0; inblock = 0; block_head = 0 }
{
    line = $0
    if (inblock) {
        # copy the block verbatim, tracking its own depth
        print line
        for (i = 1; i <= length(line); i++) {
            c = substr(line, i, 1)
            if (c == "{") depth++
            else if (c == "}") depth--
        }
        if (depth <= 0) {
            inblock = 0
            done = 1
        }
        next
    }
    print line
    if (done) next
    if (!started && index(line, anchor) > 0) {
        started = 1
        pending_open = 1
        next
    }
    if (started && !inblock) {
        for (i = 1; i <= length(line); i++) {
            c = substr(line, i, 1)
            if (c == "{") depth++
            else if (c == "}") depth--
        }
        if (depth <= 0) {
            # the anchor item just closed: emit the block here
            inblock = 1
            depth = 0
            while ((getline l < block) > 0) {
                print l
                for (i = 1; i <= length(l); i++) {
                    c = substr(l, i, 1)
                    if (c == "{") depth++
                    else if (c == "}") depth--
                }
            }
            close(block)
            inblock = 0
            done = 1
        }
    }
}
END { if (!done) exit 3 }
' "$FILE" > "$TMP" || { rm -f "$TMP"; echo "anchor not found or block unbalanced: id: $ANCHOR" >&2; exit 3; }

after=$(braces "$TMP")
if ! awk -v a="$after" -v b="$before" -v c="$block_depth" 'BEGIN { exit !(a + 0 == b + 0 + c + 0) }'; then
    rm -f "$TMP"
    echo "brace balance check failed (before=$before block=$block_depth after=$after) - refusing to write" >&2
    exit 4
fi

if [ "$DRY" = 1 ]; then
    echo "would insert $(basename "$BLOCK") after 'id: $ANCHOR' in $FILE (depth $before -> $after)"
    rm -f "$TMP"
    exit 0
fi

mkdir -p "$BACKUP_DIR"
[ -f "$BACKUP" ] || cp -f "$FILE" "$BACKUP"
label=$(getfattr -n security.SMACK64 --only-values "$FILE" 2>/dev/null || true)
cat "$TMP" > "$FILE"
rm -f "$TMP"
chmod 644 "$FILE" 2>/dev/null || true
[ -n "$label" ] && setfattr -n security.SMACK64 -v "$label" "$FILE" 2>/dev/null || true
rm -rf /root/.cache/qtshadercache /tmp/qmlcache /tmp/qt-qml-cache 2>/dev/null || true

echo "injected $(basename "$BLOCK") into $FILE (backup: $BACKUP)"
