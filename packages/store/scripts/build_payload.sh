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

echo "== icons + QML =="
python3 "$HERE/make_icons.py"
python3 "$HERE/build_artifacts.py"

STAGE=$(mktemp -d "${TMPDIR:-/tmp}/scourge-store.XXXXXX")
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/payload/bin" "$STAGE/payload/systemd" "$STAGE/backup" \
         "$STAGE/payload/assets/store" \
         "$STAGE/payload/qml/AppHomeScreen/qml_eco3.0/HomeScreen" \
         "$STAGE/payload/qml/AppHomeScreen/qml_eco3.0/component"

cp "$ROOT/src/store_daemon" "$STAGE/payload/bin/store_daemon"
cp "$ROOT"/artifacts/assets/store/*.png "$STAGE/payload/assets/store/"
cp "$ROOT"/artifacts/qml/AppHomeScreen/qml_eco3.0/HomeScreen/StorePage.qml \
   "$STAGE/payload/qml/AppHomeScreen/qml_eco3.0/HomeScreen/"
cp "$ROOT"/artifacts/qml/AppHomeScreen/qml_eco3.0/component/StoreTile.qml \
   "$STAGE/payload/qml/AppHomeScreen/qml_eco3.0/component/"
if ls "$ROOT"/artifacts/qml/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Advanced.*.qml >/dev/null 2>&1; then
    cp "$ROOT"/artifacts/qml/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Advanced.*.qml \
       "$STAGE/payload/qml/AppHomeScreen/qml_eco3.0/HomeScreen/"
fi
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

# The stick also carries a local catalogue: /scourge-packages on the USB root is
# what the Marketplace lists in its "USB packages" view (/local in the daemon).
mkdir -p "$STAGE/scourge-packages"
( cd "$STAGE" && python3 - "$STAGE" <<'PY'
import hashlib
import json
import os
import sys
import tarfile

stage = sys.argv[1]
out = os.path.join(stage, "scourge-packages")
name = "marketplace-0.2.0"
target = os.path.join(out, name + ".tar.gz")
with tarfile.open(target, "w:gz") as archive:
    for root, _dirs, files in os.walk(os.path.join(stage, "payload")):
        for f in sorted(files):
            full = os.path.join(root, f)
            archive.add(full, os.path.join("payload", os.path.relpath(full, os.path.join(stage, "payload"))))
    archive.add(os.path.join(stage, "run_stuff.sh"), "run_stuff.sh")
sha = hashlib.sha256(open(target, "rb").read()).hexdigest()
with open(os.path.join(out, name + ".json"), "w") as fh:
    json.dump({"pkg": "marketplace", "version": "0.2.0", "sha256": sha,
               "notes": "SCOURGE Marketplace app payload"}, fh, indent=2)
with open(os.path.join(out, name + ".tar.gz.sha256"), "w") as fh:
    fh.write(f"{sha}  {name}.tar.gz\n")
print("local catalogue:", name)
PY
)

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
