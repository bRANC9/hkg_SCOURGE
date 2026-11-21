#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Host-side test for store_daemon against a fake hub.

Covers the whole Marketplace logic without a car: register/pair, index refresh,
catalog cache, install job (download -> sha256 -> tar -> run_stuff.sh -> report),
tampered artifact rejection, and the allow_install=0 staging path.
"""
import hashlib
import http.server
import io
import json
import os
import shutil
import socket
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import urllib.error
import urllib.request

DAEMON = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "store_daemon")
FW = "NE_E.EUR.S5W_L.001.001.260128"


def free_port():
    """Ask the kernel for an unused loopback port (no fixed-port flakes)."""
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


HUB_PORT = free_port()
APP_PORT = free_port()


def make_payload(marker_path, body=b"#!/bin/sh\necho installed > \"$MARKER\"\n"):
    """A tiny payload: run_stuff.sh writes MARKER so we can prove it ran."""
    script = body.replace(b"$MARKER", marker_path.encode())
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w:gz") as tar:
        for name, data in (("run_stuff.sh", script), ("payload/demo.qml", b"Item {}\n")):
            info = tarfile.TarInfo(name)
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
    return buffer.getvalue()


class FakeHub(http.server.BaseHTTPRequestHandler):
    index = {}
    payload = b""
    reports = []
    pair_status = 200

    def log_message(self, *args):
        pass

    def _json(self, code, body):
        raw = json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self):
        if self.path.startswith("/api/v1/index/"):
            return self._json(200, {"packages": list(self.index.values())})
        if self.path.startswith("/artifact/"):
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(self.payload)))
            self.end_headers()
            self.wfile.write(self.payload)
            return None
        return self._json(404, {"detail": "nope"})

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        body = json.loads(self.rfile.read(length) or b"{}")
        if self.path.startswith("/api/v1/device/register"):
            return self._json(200, {
                "install_id": "11111111-2222-3333-4444-555555555555",
                "pairing_code": "123456",
                "pair_url": "http://hub/pair/?t=tok",
                "qr_url": "http://hub/qr.png",
                "expires_in": 1800,
            })
        if self.path.startswith("/api/v1/device/pair"):
            if FakeHub.pair_status != 200:
                return self._json(FakeHub.pair_status, {"detail": "unit is already claimed by another account"})
            ok = body.get("code") == "123456" or body.get("token") == "tok"
            if not ok:
                return self._json(400, {"detail": "invalid or expired pairing code"})
            return self._json(200, {"install_id": "11111111-2222-3333-4444-555555555555",
                                    "device_token": "device-token-abc", "claimed": True})
        if self.path.startswith("/api/v1/device/report"):
            FakeHub.reports.append({"auth": self.headers.get("Authorization"), "body": body})
            return self._json(200, {"status": "recorded"})
        return self._json(404, {"detail": "nope"})


def api(path, method="GET", port=None):
    port = port or APP_PORT
    req = urllib.request.Request(f"http://127.0.0.1:{port}{path}", method=method)
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            raw = r.read().decode()
            return r.status, (json.loads(raw) if raw.strip().startswith(("{", "[")) else raw)
    except urllib.error.HTTPError as exc:
        raw = exc.read().decode()
        try:
            return exc.code, json.loads(raw)
        except json.JSONDecodeError:
            return exc.code, raw


def wait_job(want, timeout=20.0):
    deadline = time.time() + timeout
    last = {}
    while time.time() < deadline:
        _status, job = api("/job")
        last = job if isinstance(job, dict) else {}
        if last.get("state") == want:
            return last
        if last.get("state") in ("failed", "installed") and want not in ("failed", "installed"):
            return last
        time.sleep(0.2)
    raise AssertionError(f"job never reached {want}: {last}")


def start_daemon(rw, allow_install=True, hub_port=HUB_PORT, port=APP_PORT):
    args = [DAEMON, "--rw-dir", rw, "--hub", f"http://127.0.0.1:{hub_port}", "--port", str(port)]
    if allow_install:
        args.append("--allow-install")
    proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    for _ in range(50):
        if api("/health", port=port)[0] == 200:
            return proc
        if proc.poll() is not None:
            raise AssertionError(f"daemon died: {proc.stdout.read().decode()}")
        time.sleep(0.1)
    raise AssertionError("daemon did not start")


def main():
    tmp = tempfile.mkdtemp(prefix="store-daemon-test.")
    rw = os.path.join(tmp, "rw")
    os.makedirs(rw)
    marker = os.path.join(tmp, "installed.marker")
    payload = make_payload(marker)
    FakeHub.payload = payload
    FakeHub.reports = []
    FakeHub.pair_status = 200
    FakeHub.index = {
        "demo": {
            "pkg": "demo",
            "version": "1.0",
            "release_id": 7,
            "url": f"http://127.0.0.1:{HUB_PORT}/artifact/demo.tar.gz",
            "sha256": hashlib.sha256(payload).hexdigest(),
            "size": len(payload),
            "installer_sha256": hashlib.sha256(b'#!/bin/sh\necho installed > "$MARKER"\n'.replace(b"$MARKER", marker.encode())).hexdigest(),
        }
    }

    hub = http.server.ThreadingHTTPServer(("127.0.0.1", HUB_PORT), FakeHub)
    threading.Thread(target=hub.serve_forever, daemon=True).start()
    proc = start_daemon(rw)
    try:
        status, body = api("/status")
        assert status == 200 and body["paired"] is False, body
        assert body["firmware"] == FW, body

        # pairing against the fake hub
        status, body = api("/pair?code=123456", "POST")
        assert status == 200 and "paired" in body["detail"], body
        status, body = api("/status")
        assert body["paired"] is True and body["install_id"].startswith("11111111"), body

        # wrong code and 409 from the hub are surfaced, not swallowed
        FakeHub.pair_status = 409
        status, body = api("/pair?code=000000", "POST")
        assert status == 409 and "claimed" in body["detail"], body
        FakeHub.pair_status = 200

        # index refresh + catalog cache
        status, body = api("/refresh", "POST")
        assert status == 200, body
        status, body = api("/catalog")
        assert status == 200 and body["packages"][0]["pkg"] == "demo", body

        # install job: download -> sha256 -> tar -> run_stuff.sh -> report
        status, body = api("/install?pkg=demo", "POST")
        assert status == 200 and body["started"] is True, body
        job = wait_job("installed")
        assert job["percent"] == 100, job
        assert os.path.isfile(marker), "installer did not run"
        status, body = api("/status")
        assert any(entry["package"] == "demo" for entry in body["ledger"]["installed"]), body
        assert FakeHub.reports and FakeHub.reports[0]["auth"] == "Device device-token-abc", FakeHub.reports
        assert FakeHub.reports[0]["body"]["release"] == 7, FakeHub.reports

        # tampered artifact is refused before anything runs
        os.unlink(marker)
        FakeHub.index["demo"]["sha256"] = "0" * 64
        api("/refresh", "POST")
        api("/install?pkg=demo", "POST")
        job = wait_job("failed")
        assert "sha256 mismatch" in job["detail"], job
        assert not os.path.exists(marker)
    finally:
        proc.terminate()
        proc.wait(timeout=5)
        shutil.rmtree(tmp, ignore_errors=True)

    # self-update: the app itself is a catalogue package ("marketplace")
    tmp_su = tempfile.mkdtemp(prefix="store-daemon-selfupdate.")
    rw_su = os.path.join(tmp_su, "rw")
    os.makedirs(rw_su)
    payload_su = make_payload(os.path.join(tmp_su, "self.marker"))
    FakeHub.payload = payload_su
    FakeHub.index["marketplace"] = {
        "pkg": "marketplace",
        "version": "99.0.0",
        "release_id": 99,
        "url": f"http://127.0.0.1:{HUB_PORT}/artifact/marketplace.tar.gz",
        "sha256": hashlib.sha256(payload_su).hexdigest(),
        "size": len(payload_su),
        "installer_sha256": "",
    }
    proc_su = start_daemon(rw_su, allow_install=False)
    try:
        api("/pair?code=123456", "POST")
        api("/refresh", "POST")
        status, body = api("/status")
        assert status == 200, body
        assert body["version"], body
        assert body["self_update"] is True and body["self_version"] == "99.0.0", body

        # without allow_install it only stages - it must never restart itself
        status, body = api("/self-update", "POST")
        assert status == 200 and body["started"] is True, body
        job = wait_job("staged")
        assert job["package"] == "marketplace", job
        assert os.path.isfile(os.path.join(rw_su, "stage", "marketplace", "payload", "demo.qml"))
        # staging must not have queued a report (nothing was installed)
        assert not os.path.exists(os.path.join(rw_su, "report.pending.json"))
    finally:
        proc_su.terminate()
        proc_su.wait(timeout=5)
        shutil.rmtree(tmp_su, ignore_errors=True)

    # same index, older version in the catalogue -> no update offered
    FakeHub.index["marketplace"]["version"] = "0.0.1"
    tmp_old = tempfile.mkdtemp(prefix="store-daemon-noupdate.")
    rw_old = os.path.join(tmp_old, "rw")
    os.makedirs(rw_old)
    proc_old = start_daemon(rw_old, allow_install=False)
    try:
        api("/refresh", "POST")
        status, body = api("/status")
        assert body["self_update"] is False, body
        status, body = api("/self-update", "POST")
        assert body["started"] is False and "already at" in body["detail"], body
    finally:
        proc_old.terminate()
        proc_old.wait(timeout=5)
        shutil.rmtree(tmp_old, ignore_errors=True)

    # allow_install=0: staged but never executed
    tmp = tempfile.mkdtemp(prefix="store-daemon-stage.")
    rw = os.path.join(tmp, "rw")
    os.makedirs(rw)
    marker = os.path.join(tmp, "never.marker")
    payload = make_payload(marker)
    FakeHub.payload = payload
    FakeHub.index["demo"].update({
        "sha256": hashlib.sha256(payload).hexdigest(),
        "installer_sha256": "",
    })
    proc = start_daemon(rw, allow_install=False)
    try:
        api("/pair?code=123456", "POST")
        api("/refresh", "POST")
        api("/install?pkg=demo", "POST")
        job = wait_job("staged")
        assert "nothing was executed" in job["detail"], job
        assert not os.path.exists(marker)
        assert os.path.isfile(os.path.join(rw, "stage", "demo", "payload", "demo.qml"))
    finally:
        proc.terminate()
        proc.wait(timeout=5)
        hub.shutdown()
        shutil.rmtree(tmp, ignore_errors=True)

    print("PASS: store daemon (pair, refresh, catalog, install job, tampered artifact, staged-only)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
