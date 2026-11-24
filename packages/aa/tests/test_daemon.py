#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Integration test for the AA debug daemon.

Starts the daemon on an ephemeral port, checks the three routes a WiFi page
would read, and verifies that a successful /ap-check is what pins the AP numbers
(the "save"): the state file is written and /saved reports it afterwards.

Builds with warnings as errors, exactly like the engine tests.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
DAEMON = ROOT / "daemon" / "aa_daemon.c"
BUILD_ROOT = Path("/tmp/opencode/aa")

COMMON = [
    "-std=c11",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-pedantic",
    "-Wshadow",
    "-Wstrict-prototypes",
    "-Wmissing-prototypes",
]
SOURCES = [DAEMON, SRC / "aa_ap.c", SRC / "aa_btboot.c", SRC / "aa_proto.c"]


def build() -> Path:
    BUILD_ROOT.mkdir(parents=True, exist_ok=True)
    binary = BUILD_ROOT / "aa_daemon"
    cmd = [os.environ.get("CC", "cc"), *COMMON, "-o", str(binary)]
    cmd.extend(str(path) for path in SOURCES)
    subprocess.run(cmd, check=True, timeout=120)
    return binary


def start(binary: Path, state_path: Path) -> tuple[subprocess.Popen, int]:
    process = subprocess.Popen(
        [str(binary), "--port", "0", "--state", str(state_path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    assert process.stdout is not None
    deadline = time.time() + 10.0
    while time.time() < deadline:
        line = process.stdout.readline()
        if not line:
            if process.poll() is not None:
                raise SystemExit(f"aa_daemon exited early: {process.stderr.read() if process.stderr else ''}")
            continue
        if line.startswith("port="):
            return process, int(line.split("=", 1)[1])
    process.kill()
    raise SystemExit("aa_daemon did not report a port")


def get(port: int, path: str, params: dict[str, str] | None = None) -> tuple[int, dict]:
    url = f"http://127.0.0.1:{port}{path}"
    if params:
        url += "?" + urllib.parse.urlencode(params)
    with urllib.request.urlopen(url, timeout=10) as response:  # noqa: S310 - loopback only
        body = response.read().decode()
        return response.status, json.loads(body)


def main() -> int:
    binary = build()
    with tempfile.TemporaryDirectory() as tmp:
        state_path = Path(tmp) / "aa_state.conf"
        process, port = start(binary, state_path)
        try:
            status, body = get(port, "/status")
            assert status == 200, status
            assert body["service"] == "aa-daemon", body
            assert body["saved"] is False, body
            # Readiness fields the WiFi page renders, and the honest reason.
            assert body["bt_bootstrap"] is False and body["tcp_listener"] is False, body
            assert body["tls"] is False and body["session"] is False, body
            assert body["bt_stack"] == "vendor", body
            assert "bridge board" in body["reason"], body

            status, body = get(port, "/saved")
            assert status == 200 and body["saved"] is False, body

            good = {
                "ssid": "SCOURGE-AA",
                "key": "testpass123",
                "bssid": "DC:A6:32:E7:5A:FE",
                "addr": "10.42.0.1",
                "port": "5288",
            }
            status, body = get(port, "/ap-check", good)
            assert status == 200, status
            assert body["ok"] is True, body
            assert body["announce_ip"] == "10.42.0.1", body
            assert body["announce_port"] == 5288, body
            assert body["security"] == 8 and body["ap_type"] == 1, body
            assert body["saved"] is True, body
            assert state_path.is_file(), "state file was not written"

            status, body = get(port, "/saved")
            assert body["saved"] is True and body["ssid"] == "SCOURGE-AA", body
            assert body["bssid"] == "DC:A6:32:E7:5A:FE", body
            assert body["address"] == "10.42.0.1" and body["port"] == 5288, body

            status, body = get(port, "/status")
            assert body["saved"] is True, body

            # The AP layer reports the radio MAC in lowercase: that must be
            # accepted and canonicalised, because AP mode has no bssid= to read.
            lowercase = dict(good, bssid="28:0f:eb:02:21:8a")
            status, body = get(port, "/ap-check", lowercase)
            assert body["ok"] is True, body
            status, body = get(port, "/saved")
            assert body["bssid"] == "28:0F:EB:02:21:8A", body

            # A malformed MAC is still refused, and the refusal is not a save.
            bad = dict(good, bssid="28:0f:eb:02:21")
            status, body = get(port, "/ap-check", bad)
            assert body["ok"] is False and body["error"] == "field", body

            # A public address is refused too: nothing may announce it.
            bad = dict(good, addr="8.8.8.8")
            status, body = get(port, "/ap-check", bad)
            assert body["ok"] is False and body["error"] == "address", body

            # The saved state survives a refusal.
            status, body = get(port, "/saved")
            assert body["saved"] is True and body["address"] == "10.42.0.1", body

            # Missing fields are a client error, not a save.
            url = f"http://127.0.0.1:{port}/ap-check?ssid=x"
            try:
                urllib.request.urlopen(url, timeout=10)  # noqa: S310 - loopback only
                raise AssertionError("expected 400")
            except urllib.error.HTTPError as error:
                assert error.code == 400, error.code
        finally:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()

        # A restart reads the saved endpoint back.
        process, port = start(binary, state_path)
        try:
            status, body = get(port, "/saved")
            assert body["saved"] is True and body["ssid"] == "SCOURGE-AA", body
        finally:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()

    print("aa daemon: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
