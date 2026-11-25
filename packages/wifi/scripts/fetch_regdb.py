#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Fetch the signed wireless regulatory database for the WiFi payload.

Why this exists: cfg80211 on this unit reports "country 00" (world), and a world
regdomain forbids beaconing on every 5 GHz channel, so the 5 GHz AP (which the
wireless Android Auto bootstrap needs) cannot start.  "iw reg set HU" cannot fix
it on its own: the unit has no regulatory.db at all, and without a VALID SIGNED
pair cfg80211 keeps the world regdomain and ignores the ieee80211_regdom hint as
well.

The database is NOT something we may write ourselves: cfg80211 verifies
regulatory.db.p7s against the public key compiled into the kernel, so a hand-made
file is silently ignored.  We ship the upstream-signed pair from the
wireless-regdb release that Debian packages (the -upstream files, not the
-Debian ones: the vendor kernel carries the upstream keys).

Outputs (into build/regdb/):
  regulatory.db       signed binary database (magic 'RGDB')
  regulatory.db.p7s   detached PKCS#7 signature
  version.txt         package version and the sha256 of everything we shipped

Set REGDB_DEB to use a local .deb, or REGDB_REFRESH=1 to force a new download.
"""
from __future__ import annotations

import hashlib
import io
import lzma
import os
import sys
import tarfile
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BUILD = os.path.join(ROOT, "build", "regdb")
LOCK = os.path.join(ROOT, "..", "..", "config", "third_party.lock")
# Pinned, never "latest": a different database changes which channels the AP may
# use, and that has to be a reviewable change.
VERSION = "2026.09.03-1"
URL = ("https://deb.debian.org/debian/pool/main/w/wireless-regdb/"
       f"wireless-regdb_{VERSION}_all.deb")
# sha256 of that .deb, recorded in config/third_party.lock on first fetch.
MAGIC = bytes.fromhex("52474442")  # 'RGDB', struct regdb_file_header


def log(msg: str) -> None:
    print(f"[regdb] {msg}")


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def lock_pin() -> str:
    """The pinned .deb sha256 from third_party.lock, or '' when not recorded."""
    try:
        with open(LOCK, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError:
        return ""
    section = ""
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1]
            continue
        if section != "wireless-regdb":
            continue
        if line.startswith("sha256.") and "=" in line:
            value = line.split("=", 1)[1].strip()
            if len(value) == 64 and all(c in "0123456789abcdef" for c in value):
                return value
    return ""


def download() -> bytes:
    local = os.environ.get("REGDB_DEB")
    if local:
        log(f"using local package {local}")
        with open(local, "rb") as f:
            return f.read()
    log(f"downloading {URL}")
    req = urllib.request.Request(URL, headers={"User-Agent": "scourge-build/1.0"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def ar_members(deb: bytes) -> dict[str, bytes]:
    if deb[:8] != b"!<arch>\n":
        raise SystemExit("not an ar archive")
    members: dict[str, bytes] = {}
    pos = 8
    while pos + 60 <= len(deb):
        header = deb[pos:pos + 60]
        name = header[0:16].decode("utf-8", "replace").strip()
        try:
            size = int(header[48:58].decode("ascii").strip())
        except ValueError:
            raise SystemExit(f"bad ar header at {pos}")
        body = deb[pos + 60:pos + 60 + size]
        members[name.rstrip("/")] = body
        pos += 60 + size + (size & 1)
    return members


def extract(data_tar: bytes) -> dict[str, bytes]:
    opener = lzma.open if data_tar[:6] == b"\xfd7zXZ\x00"[:6] else None
    raw = io.BytesIO(data_tar)
    stream = lzma.LZMAFile(raw) if opener else raw
    wanted = {
        "./usr/lib/firmware/regulatory.db-upstream": "regulatory.db",
        "./usr/lib/firmware/regulatory.db.p7s-upstream": "regulatory.db.p7s",
    }
    out: dict[str, bytes] = {}
    with tarfile.open(fileobj=stream, mode="r|") as tar:
        for member in tar:
            if member.name in wanted:
                handle = tar.extractfile(member)
                if handle:
                    out[wanted[member.name]] = handle.read()
    return out


def main() -> int:
    os.makedirs(BUILD, exist_ok=True)
    db_path = os.path.join(BUILD, "regulatory.db")
    p7s_path = os.path.join(BUILD, "regulatory.db.p7s")
    version_path = os.path.join(BUILD, "version.txt")

    pin = lock_pin()
    if (os.path.exists(db_path) and os.path.exists(p7s_path)
            and not os.environ.get("REGDB_REFRESH")):
        with open(db_path, "rb") as f:
            have = sha256(f.read())
        if not pin or have == pin:
            log(f"using cached {db_path} ({have[:16]})")
            return 0

    deb = download()
    digest = sha256(deb)
    if pin and digest != pin:
        raise SystemExit(f"wireless-regdb {VERSION} sha256 mismatch: "
                         f"pinned {pin}, got {digest}")
    if not pin:
        log(f"NOTE: record this in config/third_party.lock: sha256.{VERSION} = {digest}")

    members = ar_members(deb)
    data_name = next((n for n in members if n.startswith("data.tar")), "")
    if not data_name:
        raise SystemExit("no data.tar member in the package")
    files = extract(members[data_name])
    db = files.get("regulatory.db", b"")
    p7s = files.get("regulatory.db.p7s", b"")
    if db[:4] != MAGIC:
        raise SystemExit("regulatory.db is not a regdb (bad magic)")
    if len(p7s) < 256 or p7s[0] != 0x30:
        raise SystemExit("regulatory.db.p7s is not a DER signature")

    for path, blob in ((db_path, db), (p7s_path, p7s)):
        with open(path, "wb") as f:
            f.write(blob)
        os.chmod(path, 0o644)
    with open(version_path, "w", encoding="utf-8") as f:
        f.write(f"version={VERSION}\ndeb_sha256={digest}\n"
                f"regulatory.db={sha256(db)}\nregulatory.db.p7s={sha256(p7s)}\n")
    log(f"{VERSION}: regulatory.db {len(db)} B {sha256(db)[:16]}, "
        f"signature {len(p7s)} B")
    return 0


if __name__ == "__main__":
    sys.exit(main())
