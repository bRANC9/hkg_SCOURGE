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
APP_PORT_USB = free_port()
APP_PORT_SU = free_port()
APP_PORT_SU2 = free_port()
APP_PORT_STAGE = free_port()
APP_PORT_TLS = free_port()
APP_PORT_TLS_BAD = free_port()


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
        if self.path.startswith("/old-index/"):
            # a reverse proxy answering http -> https with a redirect
            self.send_response(301)
            self.send_header("Location", self.path.replace("/old-index", "", 1))
            self.send_header("Content-Length", "0")
            self.end_headers()
            return None
        if self.path.startswith("/api/v1/index/"):
            return self._json(200, {"packages": list(self.index.values())})
        if self.path.startswith("/gh/releases/download/"):
            # what GitHub does for release assets: 302 to a short-lived URL
            self.send_response(302)
            self.send_header("Location", self.path.replace("/gh/releases/download", "/artifact", 1))
            self.send_header("Content-Length", "0")
            self.end_headers()
            return None
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
    except (urllib.error.URLError, ConnectionError, TimeoutError) as exc:
        # The daemon is not (yet) listening: callers poll on this.
        return 0, str(exc)


DAEMON_LOG = [None]


def daemon_tail(lines=20):
    path = DAEMON_LOG[0]
    if not path or not os.path.exists(path):
        return "(no daemon log)"
    with open(path, "r", errors="replace") as fh:
        return "".join(fh.readlines()[-lines:])


def wait_job(want, timeout=20.0, port=None):
    deadline = time.time() + timeout
    last = {}
    while time.time() < deadline:
        _status, job = api("/job", port=port or APP_PORT)
        last = job if isinstance(job, dict) else {}
        if last.get("state") == want:
            return last
        if last.get("state") in ("failed", "installed") and want not in ("failed", "installed"):
            return last
        time.sleep(0.2)
    raise AssertionError(f"job never reached {want}: {last}\n--- daemon log ---\n{daemon_tail()}")


def start_daemon(rw, allow_install=True, hub_port=HUB_PORT, port=None, extra_args=None, hub_url=None):
    port = port or APP_PORT
    hub = hub_url or f"http://127.0.0.1:{hub_port}"
    args = [DAEMON, "--rw-dir", rw, "--hub", hub, "--port", str(port)]
    if allow_install:
        args.append("--allow-install")
    args.extend(extra_args or [])
    log_path = os.path.join(rw, "..", "daemon.log")
    log_file = open(log_path, "wb")
    proc = subprocess.Popen(args, stdout=log_file, stderr=subprocess.STDOUT)
    proc.daemon_log = log_path  # type: ignore[attr-defined]
    DAEMON_LOG[0] = log_path
    for _ in range(50):
        if api("/health", port=port)[0] == 200:
            return proc
        if proc.poll() is not None:
            raise AssertionError(f"daemon died: {open(log_path).read()}")
        time.sleep(0.1)
    raise AssertionError(f"daemon did not start: {open(log_path).read()}")


def make_ca_dir(tmp):
    """Self-signed cert for 127.0.0.1 + a hashed CA dir the daemon can trust."""
    cert = os.path.join(tmp, "test-cert.pem")
    key = os.path.join(tmp, "test-key.pem")
    subprocess.run(
        ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
         "-keyout", key, "-out", cert, "-subj", "/CN=127.0.0.1",
         "-addext", "subjectAltName=IP:127.0.0.1",
         "-addext", "basicConstraints=critical,CA:TRUE"],
        check=True, capture_output=True,
    )
    ca = os.path.join(tmp, "ca")
    os.makedirs(ca, exist_ok=True)
    shutil.copyfile(cert, os.path.join(ca, "test-root.pem"))
    subprocess.run(["openssl", "rehash", ca], check=True, capture_output=True)
    return cert, key, ca


def start_tls_hub(port, cert, key):
    import ssl

    server = http.server.ThreadingHTTPServer(("127.0.0.1", port), FakeHub)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert, key)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


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

        # index refresh follows a proxy redirect instead of failing on the 301:
        # point the daemon through a path the fake proxy redirects away from.
        status, body = api(f"/config?hub=http%3A%2F%2F127.0.0.1%3A{HUB_PORT}%2Fold-index", "POST")
        assert status == 200, body

        # index refresh + catalog cache (through the redirect above)
        status, body = api("/refresh", "POST")
        assert status == 200, body
        status, body = api("/catalog")
        assert status == 200 and body["packages"][0]["pkg"] == "demo", body
        # back to the plain base path for the rest of the run
        api(f"/config?hub=http%3A%2F%2F127.0.0.1%3A{HUB_PORT}", "POST")

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

    # ---- USB stick source (offline): /local lists, /install-local installs ----
    tmp_usb = tempfile.mkdtemp(prefix="store-daemon-usb.")
    rw_usb = os.path.join(tmp_usb, "rw")
    os.makedirs(rw_usb)
    stick = os.path.join(tmp_usb, "usb")
    pkgs = os.path.join(stick, "scourge-packages")
    os.makedirs(pkgs)
    usb_marker = os.path.join(tmp_usb, "usb.marker")
    usb_payload = make_payload(usb_marker)
    with open(os.path.join(pkgs, "demo-2.0.tar.gz"), "wb") as fh:
        fh.write(usb_payload)
    with open(os.path.join(pkgs, "demo-2.0.json"), "w") as fh:
        json.dump({"pkg": "demo-usb", "version": "2.0", "firmware": FW}, fh)
    with open(os.path.join(pkgs, "demo-2.0.tar.gz.sha256"), "w") as fh:
        fh.write(hashlib.sha256(usb_payload).hexdigest() + "  demo-2.0.tar.gz\n")
    with open(os.path.join(pkgs, "broken-1.0.tar.gz"), "wb") as fh:
        fh.write(usb_payload)
    with open(os.path.join(pkgs, "broken-1.0.tar.gz.sha256"), "w") as fh:
        fh.write("0" * 64 + "  broken-1.0.tar.gz\n")

    proc_usb = start_daemon(rw_usb, allow_install=True, port=APP_PORT_USB,
                            extra_args=["--local-dir", stick])
    try:
        status, body = api("/local", port=APP_PORT_USB)
        assert status == 200, body
        files = {p["file"]: p for p in body["packages"]}
        assert set(files) == {"demo-2.0.tar.gz", "broken-1.0.tar.gz"}, body
        assert files["demo-2.0.tar.gz"]["meta"]["pkg"] == "demo-usb"
        assert files["demo-2.0.tar.gz"]["sha256_sidecar"] is True

        status, body = api("/install-local?file=demo-2.0.tar.gz", "POST", port=APP_PORT_USB)
        assert status == 200 and body["source"] == "usb", body
        job = wait_job("installed", port=APP_PORT_USB)
        assert job["package"] == "demo-usb", job
        assert os.path.isfile(usb_marker), "USB installer did not run"

        status, body = api("/install-local?file=broken-1.0.tar.gz", "POST", port=APP_PORT_USB)
        assert status == 200, body
        job = wait_job("failed", port=APP_PORT_USB)
        assert "sha256 mismatch" in job["detail"], job

        status, body = api("/install-local?file=../etc/passwd", "POST", port=APP_PORT_USB)
        assert status == 400, body

        # untrusted input never reaches a shell: bad slugs are refused up front
        for bad in ("a$(reboot)", "x;rm%20-rf%20%2F", ".hidden", "with%20space", "ok$IFS", "a%2Fb"):
            status, body = api(f"/install?pkg={bad}", "POST", port=APP_PORT_USB)
            assert status == 400, (bad, body)
            status, body = api(f"/versions?pkg={bad}", port=APP_PORT_USB)
            assert status == 400, (bad, body)
    finally:
        proc_usb.terminate()
        proc_usb.wait(timeout=5)
        shutil.rmtree(tmp_usb, ignore_errors=True)

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
    proc_su = start_daemon(rw_su, allow_install=False, port=APP_PORT_SU)
    try:
        api("/pair?code=123456", "POST", port=APP_PORT_SU)
        api("/refresh", "POST", port=APP_PORT_SU)
        status, body = api("/status", port=APP_PORT_SU)
        assert status == 200, body
        assert body["version"], body
        assert body["self_update"] is True and body["self_version"] == "99.0.0", body

        # without allow_install it only stages - it must never restart itself
        status, body = api("/self-update", "POST", port=APP_PORT_SU)
        assert status == 200 and body["started"] is True, body
        job = wait_job("staged", port=APP_PORT_SU)
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
    proc_old = start_daemon(rw_old, allow_install=False, port=APP_PORT_SU2)
    try:
        api("/refresh", "POST", port=APP_PORT_SU2)
        status, body = api("/status", port=APP_PORT_SU2)
        assert body["self_update"] is False, body
        status, body = api("/self-update", "POST", port=APP_PORT_SU2)
        assert body["started"] is False and "already at" in body["detail"], body
    finally:
        proc_old.terminate()
        proc_old.wait(timeout=5)
        shutil.rmtree(tmp_old, ignore_errors=True)

    # ---- https: the public-store path, via the vendor openssl s_client ----
    tmp_tls = tempfile.mkdtemp(prefix="store-daemon-tls.")
    rw_tls = os.path.join(tmp_tls, "rw")
    os.makedirs(rw_tls)
    cert, key, ca_dir = make_ca_dir(tmp_tls)
    tls_port = free_port()
    try:
        tls_hub = start_tls_hub(tls_port, cert, key)
        FakeHub.index.pop("marketplace", None)
        FakeHub.index["demo"] = {
            "pkg": "demo", "version": "1.0", "release_id": 7,
            "url": f"https://127.0.0.1:{tls_port}/artifact/demo.tar.gz",
            "sha256": hashlib.sha256(FakeHub.payload).hexdigest(),
            "size": len(FakeHub.payload), "installer_sha256": "",
        }
        proc_tls = start_daemon(rw_tls, allow_install=False,
                                hub_url=f"https://127.0.0.1:{tls_port}",
                                port=APP_PORT_TLS,
                                extra_args=["--ca-path", ca_dir])
        try:
            status, body = api("/pair?code=123456", "POST", port=APP_PORT_TLS)
            assert status == 200, f"{body}\n--- daemon log ---\n{daemon_tail()}"
            status, body = api("/refresh", "POST", port=APP_PORT_TLS)
            assert status == 200, body
            status, body = api("/catalog", port=APP_PORT_TLS)
            assert status == 200 and body["packages"][0]["pkg"] == "demo", body

            # GitHub-style asset URL: the artifact lives behind a 302
            FakeHub.index["demo"].update({
                "url": f"https://127.0.0.1:{tls_port}/gh/releases/download/demo.tar.gz",
                "sha256": hashlib.sha256(FakeHub.payload).hexdigest(),
            })
            status, body = api("/refresh", "POST", port=APP_PORT_TLS)
            assert status == 200, body
            status, body = api("/install?pkg=demo", "POST", port=APP_PORT_TLS)
            assert status == 200 and body["started"] is True, body
            job = wait_job("staged", port=APP_PORT_TLS)  # allow_install=0 in this daemon
            assert job["package"] == "demo", job
        finally:
            proc_tls.terminate()
            proc_tls.wait(timeout=5)

        # an untrusted chain must fail, not silently downgrade
        rw_bad = os.path.join(tmp_tls, "rw-bad")
        os.makedirs(rw_bad)
        proc_bad = start_daemon(rw_bad, allow_install=False,
                                hub_url=f"https://127.0.0.1:{tls_port}",
                                port=APP_PORT_TLS_BAD,
                                extra_args=["--ca-path", "/etc/ssl/certs", "--tls-timeout", "5"])
        try:
            status, body = api("/refresh", "POST", port=APP_PORT_TLS_BAD)
            assert status == 502, body
            assert "index fetch failed" in body["detail"], body
        finally:
            proc_bad.terminate()
            proc_bad.wait(timeout=5)
        tls_hub.shutdown()
    finally:
        # put the fake catalogue back on plain http for the remaining scenarios
        FakeHub.index["demo"].update({
            "url": f"http://127.0.0.1:{HUB_PORT}/artifact/demo.tar.gz",
            "sha256": hashlib.sha256(FakeHub.payload).hexdigest(),
        })
        shutil.rmtree(tmp_tls, ignore_errors=True)

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
    proc = start_daemon(rw, allow_install=False, port=APP_PORT_STAGE)
    try:
        api("/pair?code=123456", "POST", port=APP_PORT_STAGE)
        api("/refresh", "POST", port=APP_PORT_STAGE)
        api("/install?pkg=demo", "POST", port=APP_PORT_STAGE)
        job = wait_job("staged", port=APP_PORT_STAGE)
        assert "nothing was executed" in job["detail"], job
        assert not os.path.exists(marker)
        assert os.path.isfile(os.path.join(rw, "stage", "demo", "payload", "demo.qml"))
    finally:
        proc.terminate()
        proc.wait(timeout=5)
        hub.shutdown()
        shutil.rmtree(tmp, ignore_errors=True)

    print("PASS: store daemon (pair, refresh, catalog, install job, usb packages, "
          "tampered artifact, staged-only, self-update)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
