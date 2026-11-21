#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Build the USB payload for the Marketplace agent.
# Usage: sh scripts/build_payload.sh /path/to/mounted/stick
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
TARGET=${1:?usage: build_payload.sh /path/to/stick}

[ -d "$TARGET" ] || { echo "not a directory: $TARGET" >&2; exit 1; }
[ "$TARGET" != "/" ] || { echo "refusing to deploy to /" >&2; exit 1; }

echo "== daemon =="
make -C "$ROOT/src"

STAGE=$(mktemp -d "${TMPDIR:-/tmp}/scourge-store.XXXXXX")
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/payload/bin" "$STAGE/payload/systemd" "$STAGE/backup"

cp "$ROOT/src/store_daemon" "$STAGE/payload/bin/store_daemon"
cat > "$STAGE/payload/systemd/store-daemon.service" <<'EOF'
[Unit]
Description=SCOURGE marketplace agent
After=network.target

[Service]
Type=simple
ExecStart=/rw_data/store/store_daemon --rw-dir /rw_data/store
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

# Optional key + hub; the installer only copies the files that exist.
[ -n "${STORE_HUB:-}" ] && printf '%s\n' "$STORE_HUB" > "$STAGE/payload/hub.txt"
[ -n "${STORE_FIRMWARE:-}" ] && printf '%s\n' "$STORE_FIRMWARE" > "$STAGE/payload/firmware.txt"
[ -n "${STORE_REGION:-}" ] && printf '%s\n' "$STORE_REGION" > "$STAGE/payload/region.txt"
[ -n "${STORE_GUI:-}" ] && printf '%s\n' "$STORE_GUI" > "$STAGE/payload/gui.txt"

cp "$HERE/run_stuff.sh" "$STAGE/run_stuff.sh"
chmod 755 "$STAGE/run_stuff.sh"

cat > "$STAGE/README.txt" <<'EOF'
SCOURGE Marketplace agent (store_daemon)

  Installs /rw_data/store/store_daemon + store-daemon.service and starts the
  daemon on 127.0.0.1:8092. No QML, no firmware, no updater changes.

  Hub URL: set STORE_HUB=... when building the payload, or POST
  /config?hub=http://<hub>:<port> afterwards. Installs are staged only until
  /config?allow_install=1 is set.
EOF

( cd "$STAGE" && find payload -type f -print0 | sort -z | xargs -0 md5sum > manifest.md5 )
ID=$(md5sum "$STAGE/manifest.md5" | cut -d' ' -f1 | cut -c1-16)
sed -i "s/^# TWEAKS_PAYLOAD_ID=.*/# TWEAKS_PAYLOAD_ID=$ID/" "$STAGE/run_stuff.sh"

echo "== write $TARGET =="
cp -R "$STAGE"/. "$TARGET"/
( cd "$TARGET" && md5sum -c manifest.md5 >/dev/null )
echo "store payload ready in $TARGET (id=$ID)"
