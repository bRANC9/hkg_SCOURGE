#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Install-time QML injection for the Ambience app.
#
#   sh qml_inject.sh --file /app/share/.../QHMI_HomeScreenMain_Advanced.qml \
#                    --anchor menuPage --block /path/to/AmbientTile.instance.qml
#   sh qml_inject.sh --file ... --block /path/to/AmbientPage.instance.qml --append
#   sh qml_inject.sh --file ... --revert
#
# Safety: brace-matched insertion, a backup next to the file, a balance check
# before and after, SMACK64 preserved, an explicit --revert.
#
# NO AWK.  The unit's awk (vendor glibc gawk) aborts with
#   *** Error in 'awk': free(): invalid pointer
# on the live home screen - that crash, not a brace mismatch, is why the ambient
# stage reported "anchor not found or block unbalanced" (2026-11-23).  Brace
# counting and the insertion are therefore pure shell + sed.
set -eu
FILE=""
ANCHOR="menuPage"
BLOCK=""
REVERT=0
DRY=0
APPEND=0
BACKUP_DIR="${QML_INJECT_BACKUP:-/rw_data/ambient/qml-backup}"

while [ $# -gt 0 ]; do
    case "$1" in
        --file) FILE=$2; shift 2 ;;
        --anchor) ANCHOR=$2; shift 2 ;;
        --block) BLOCK=$2; shift 2 ;;
        --revert) REVERT=1; shift ;;
        --dry-run) DRY=1; shift ;;
        --append) APPEND=1; shift ;;
        --backup-dir) BACKUP_DIR=$2; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

[ -n "$FILE" ] || { echo "usage: qml_inject.sh --file <Advanced.qml> [--anchor id] (--block <qml> | --revert) [--append] [--dry-run]" >&2; exit 2; }
[ -f "$FILE" ] || { echo "no such file: $FILE" >&2; exit 2; }

mkdir -p "$BACKUP_DIR" 2>/dev/null || true
BACKUP="$BACKUP_DIR/$(basename "$FILE")"

if [ "$REVERT" = 1 ]; then
    [ -f "$BACKUP" ] || { echo "no backup to revert to: $BACKUP" >&2; exit 3; }
    label=$(getfattr -n security.SMACK64 --only-values "$FILE" 2>/dev/null || true)
    cp -f "$BACKUP" "$FILE"
    [ -n "$label" ] && setfattr -n security.SMACK64 -v "$label" "$FILE" 2>/dev/null || true
    echo "reverted $FILE from $BACKUP"
    exit 0
fi

[ -n "$BLOCK" ] || { echo "--block is required (or --revert)" >&2; exit 2; }
[ -f "$BLOCK" ] || { echo "no such block: $BLOCK" >&2; exit 2; }

braces() {
    _stripped=${TMPDIR:-/tmp}/qmlbraces.$$
    sed -e 's://.*$::' -e 's:"[^"]*"::g' "$1" > "$_stripped" 2>/dev/null || cp -f "$1" "$_stripped"
    _o=0
    _c=0
    while IFS= read -r _l || [ -n "$_l" ]; do
        _t=${_l//[^\{]/}
        _o=$((_o + ${#_t}))
        _t=${_l//[^\}]/}
        _c=$((_c + ${#_t}))
    done < "$_stripped"
    rm -f "$_stripped"
    echo $((_o - _c))
}

line_delta() {
    _l=$1
    _l=${_l%%//*}
    case "$_l" in
        *'"'*'"'*) _l=$(printf '%s\n' "$_l" | sed 's/"[^"]*"//g') ;;
    esac
    _o=${_l//[^\{]/}
    _c=${_l//[^\}]/}
    echo $(( ${#_o} - ${#_c} ))
}

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

if [ "$APPEND" = 1 ]; then
    last_brace=$(grep -n '}' "$FILE" | tail -n 1 | cut -d: -f1)
    if [ -z "$last_brace" ] || [ "$last_brace" -le 0 ]; then
        rm -f "$TMP"; echo "no closing brace in $FILE" >&2; exit 3
    fi
    head -n $((last_brace - 1)) "$FILE" > "$TMP" || { rm -f "$TMP"; echo "append failed" >&2; exit 3; }
    cat "$BLOCK_CLEAN" >> "$TMP"
    tail -n +"$last_brace" "$FILE" >> "$TMP"
else
    anchor_line=$(grep -n "id: $ANCHOR" "$FILE" | head -n 1 | cut -d: -f1)
    [ -n "$anchor_line" ] || { rm -f "$TMP"; echo "anchor not found: id: $ANCHOR" >&2; exit 3; }
    depth=0
    close_line=0
    n=0
    while IFS= read -r line || [ -n "$line" ]; do
        n=$((n + 1))
        [ "$n" -ge "$anchor_line" ] || continue
        depth=$((depth + $(line_delta "$line")))
        if [ "$n" -gt "$anchor_line" ] && [ "$depth" -le 0 ]; then
            close_line=$n
            break
        fi
    done < "$FILE"
    if [ "$close_line" = 0 ]; then
        rm -f "$TMP"
        echo "anchor item never closes: id: $ANCHOR (line $anchor_line)" >&2
        exit 3
    fi
    head -n "$close_line" "$FILE" > "$TMP" || { rm -f "$TMP"; echo "insert failed" >&2; exit 3; }
    cat "$BLOCK_CLEAN" >> "$TMP"
    tail -n +$((close_line + 1)) "$FILE" >> "$TMP"
fi

after=$(braces "$TMP")
if [ "$after" -ne $((before + block_depth)) ]; then
    rm -f "$TMP"
    echo "brace balance check failed (before=$before block=$block_depth after=$after) - refusing to write" >&2
    exit 4
fi

if [ "$DRY" = 1 ]; then
    echo "would insert $(basename "$BLOCK") into $FILE (depth $before -> $after)"
    rm -f "$TMP"
    exit 0
fi

label=$(getfattr -n security.SMACK64 --only-values "$FILE" 2>/dev/null || true)
[ -f "$BACKUP" ] || cp -f "$FILE" "$BACKUP"
cat "$TMP" > "$FILE"
rm -f "$TMP"
[ -n "$label" ] && setfattr -n security.SMACK64 -v "$label" "$FILE" 2>/dev/null || true
echo "injected $(basename "$BLOCK") into $FILE (backup: $BACKUP)"
