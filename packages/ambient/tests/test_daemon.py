#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Host tests for ambient_daemon: build it, run it, drive the HTTP API.

No car, no rootfs, no CAN: the daemon is started on a free loopback port with
a temporary state directory and the `file` backend.
"""
import json
import math
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
BIN = SRC / "ambient_daemon"
PCM_RATE = 16000


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def build_daemon() -> None:
    if BIN.exists() and BIN.stat().st_mtime >= (SRC / "ambient_daemon.c").stat().st_mtime:
        return
    result = subprocess.run(["make", "-C", str(SRC)], capture_output=True, text=True)
    if result.returncode != 0:
        raise unittest.SkipTest(f"cannot build ambient_daemon: {result.stderr[-500:]}")


def tone(freq: float, seconds: float, amplitude: float = 0.6) -> bytes:
    data = bytearray()
    for i in range(int(PCM_RATE * seconds)):
        v = int(amplitude * 32767 * math.sin(2 * math.pi * freq * (i / PCM_RATE)))
        data += struct.pack("<h", v)
    return bytes(data)


class Daemon:
    def __init__(self, state_dir: Path, extra=()):
        self.state_dir = state_dir
        self.port = free_port()
        self.frame_file = state_dir / "frames.jsonl"
        args = [
            str(BIN),
            "--state-dir", str(state_dir),
            "--port", str(self.port),
            "--backend", "file",
            "--target", str(self.frame_file),
            "--no-session-monitor",
            "--quiet",
            *extra,
        ]
        self.proc = subprocess.Popen(args, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL)
        self._wait_ready()

    def _wait_ready(self) -> None:
        deadline = time.time() + 10
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise AssertionError("daemon exited during startup")
            try:
                self.get("/health")
                return
            except OSError:
                time.sleep(0.05)
        raise AssertionError("daemon did not answer /health")

    def url(self, path: str) -> str:
        return f"http://127.0.0.1:{self.port}{path}"

    def get(self, path: str):
        with urllib.request.urlopen(self.url(path), timeout=5) as r:
            return json.loads(r.read())

    def post(self, path: str):
        req = urllib.request.Request(self.url(path), data=b"", method="POST")
        with urllib.request.urlopen(req, timeout=5) as r:
            return json.loads(r.read())

    def post_error(self, path: str) -> tuple[int, dict]:
        req = urllib.request.Request(self.url(path), data=b"", method="POST")
        try:
            with urllib.request.urlopen(req, timeout=5) as r:
                return r.status, json.loads(r.read())
        except urllib.error.HTTPError as exc:
            with exc:
                return exc.code, json.loads(exc.read() or b"{}")

    def close(self) -> None:
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)


class DaemonTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        build_daemon()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="ambient-test-")
        self.dir = Path(self.tmp.name)
        self.daemon = Daemon(self.dir)

    def tearDown(self):
        self.daemon.close()
        self.tmp.cleanup()

    # ---- API contract ---------------------------------------------------
    def test_health_and_status_shape(self):
        self.assertTrue(self.daemon.get("/health")["ok"])
        status = self.daemon.get("/status")
        for key in ("power", "effect", "color", "color_hsv", "brightness",
                    "transition_style", "reactive", "backend", "zone_colors",
                    "presets", "home_seq", "audio"):
            self.assertIn(key, status)
        self.assertEqual(status["backend"], "file")
        self.assertTrue(status["zone_colors"])

    def test_effect_list_and_preset_defaults(self):
        effects = [e["name"] for e in self.daemon.get("/effects")["effects"]]
        self.assertEqual(effects, ["off", "solid", "breathe", "pulse",
                                   "rainbow", "wave", "comet", "sparkle", "cycle"])
        presets = [p["name"] for p in self.daemon.get("/presets")["presets"]]
        self.assertIn("Relax", presets)
        self.assertIn("Party", presets)

    # ---- rendering ------------------------------------------------------
    def test_solid_colour_and_brightness(self):
        self.daemon.post("/transition?ms=0&style=cut")
        self.daemon.post("/effect?name=solid")
        self.daemon.post("/color?hex=ff0000")
        self.daemon.post("/brightness?value=100")
        frame = self.daemon.get("/frame")
        self.assertEqual(frame["zones"][0], [255, 0, 0])
        self.daemon.post("/brightness?value=50")
        frame = self.daemon.get("/frame")
        self.assertAlmostEqual(frame["zones"][0][0], 128, delta=3)

    def test_hsv_colour_conversion(self):
        self.daemon.post("/color?h=0.333&s=1&v=1")
        rgb = self.daemon.get("/status")["color"]
        self.assertGreaterEqual(rgb[1], 250)
        self.assertLessEqual(rgb[0], 5)
        self.assertLessEqual(rgb[2], 5)

    def test_off_and_power(self):
        self.daemon.post("/effect?name=solid")
        self.daemon.post("/color?hex=3366ff")
        self.daemon.post("/transition?ms=0&style=cut")
        self.daemon.post("/off")
        self.assertEqual(self.daemon.get("/frame")["zones"], [[0, 0, 0]] * 4)
        self.daemon.post("/on")
        self.assertTrue(self.daemon.get("/status")["power"])

    def test_effects_produce_frames(self):
        for name in ("breathe", "rainbow", "wave", "comet", "sparkle", "cycle"):
            self.daemon.post(f"/effect?name={name}")
            first = self.daemon.get("/frame")["zones"]
            time.sleep(0.12)
            second = self.daemon.get("/frame")["zones"]
            self.assertNotEqual(first, second, f"{name} is static")
        self.daemon.post("/effect?name=rainbow")
        zones = self.daemon.get("/frame")["zones"]
        self.assertEqual(len(set(map(tuple, zones))), 4, "zones are not offset")

    # ---- transitions ----------------------------------------------------
    def test_crossfade_blends_and_finishes(self):
        self.daemon.post("/effect?name=solid")
        self.daemon.post("/color?hex=ff0000")
        self.daemon.post("/brightness?value=100")
        self.daemon.post("/transition?ms=0&style=cut")
        self.daemon.get("/frame")
        self.daemon.post("/transition?ms=1200&style=crossfade")
        self.daemon.post("/color?hex=0000ff")
        self.daemon.get("/frame")
        mid = self.daemon.get("/frame")["zones"][0]
        self.assertTrue(self.daemon.get("/frame")["transition"])
        self.assertLess(mid[2], 255)
        time.sleep(1.4)
        end = self.daemon.get("/frame")
        self.assertFalse(end["transition"])
        self.assertEqual(end["zones"][0], [0, 0, 255])

    def test_black_transition_passes_through_dark(self):
        self.daemon.post("/effect?name=solid")
        self.daemon.post("/color?hex=ffffff")
        self.daemon.post("/brightness?value=100")
        self.daemon.post("/transition?ms=1000&style=black")
        self.daemon.post("/color?hex=ffffff")
        time.sleep(0.45)
        dark = self.daemon.get("/frame")["zones"][0]
        self.assertLess(max(dark), 90)

    # ---- audio ----------------------------------------------------------
    def test_audio_levels_and_reactive_modulation(self):
        pcm = self.dir / "pcm.raw"
        pcm.write_bytes(tone(60, 1.0, 0.0))
        self.daemon.close()
        self.daemon = Daemon(self.dir, ["--audio-file", str(pcm)])
        self.daemon.post("/effect?name=solid")
        self.daemon.post("/color?hex=ffffff")
        self.daemon.post("/brightness?value=100")
        self.daemon.post("/transition?ms=0&style=cut")
        silent = self.daemon.get("/status")["audio"]
        self.assertTrue(silent["enabled"])
        self.assertLess(silent["level"], 0.05)

        with open(pcm, "ab") as fh:
            fh.write(tone(60, 1.0, 0.7))
        time.sleep(0.5)
        loud = self.daemon.get("/status")["audio"]
        self.assertGreater(loud["level"], 0.25)
        self.assertGreater(loud["bass"], loud["high"])

        self.daemon.post("/reactive?enabled=1&gain=110")
        time.sleep(0.3)
        bright = self.daemon.get("/frame")["zones"][0][0]
        with open(pcm, "ab") as fh:
            fh.write(tone(60, 2.0, 0.0))
        time.sleep(0.6)
        dim = self.daemon.get("/frame")["zones"][0][0]
        self.assertGreater(bright, dim)

    def test_beat_counter_reacts_to_kicks(self):
        pcm = self.dir / "kicks.raw"
        pcm.write_bytes(b"")
        self.daemon.close()
        self.daemon = Daemon(self.dir, ["--audio-file", str(pcm)])
        payload = bytearray()
        for _ in range(12):
            payload += tone(50, 0.05, 0.9)
            payload += bytes(2 * int(PCM_RATE * 0.20))
        with open(pcm, "ab") as fh:
            fh.write(bytes(payload))
        time.sleep(1.0)
        self.assertGreater(self.daemon.get("/status")["audio"]["beats"], 2)

    # ---- presets / persistence -----------------------------------------
    def test_preset_save_apply_and_persist(self):
        self.daemon.post("/effect?name=comet")
        self.daemon.post("/color?hex=00ff88")
        self.daemon.post("/brightness?value=55")
        self.daemon.post("/preset?name=Mine&action=save")
        names = [p["name"] for p in self.daemon.get("/presets")["presets"]]
        self.assertIn("Mine", names)

        self.daemon.post("/effect?name=solid")
        self.daemon.post("/preset?name=Mine")
        status = self.daemon.get("/status")
        self.assertEqual(status["effect"], "comet")
        self.assertEqual(status["brightness"], 55)

        self.daemon.close()
        self.daemon = Daemon(self.dir)
        self.assertEqual(self.daemon.get("/status")["effect"], "comet")
        names = [p["name"] for p in self.daemon.get("/presets")["presets"]]
        self.assertIn("Mine", names)

    def test_preset_delete(self):
        self.daemon.post("/preset?name=Gone&action=save")
        self.daemon.post("/preset?name=Gone&action=delete")
        names = [p["name"] for p in self.daemon.get("/presets")["presets"]]
        self.assertNotIn("Gone", names)

    # ---- backend --------------------------------------------------------
    def test_file_backend_writes_frames(self):
        self.daemon.post("/effect?name=solid")
        self.daemon.post("/color?hex=123456")
        self.daemon.post("/brightness?value=100")
        self.daemon.post("/transition?ms=0&style=cut")
        time.sleep(0.5)
        lines = self.daemon.frame_file.read_text().strip().splitlines()
        self.assertTrue(lines)
        entry = json.loads(lines[-1])
        self.assertEqual(entry["z"][0], [0x12, 0x34, 0x56])

    def test_unchanged_frames_are_not_resent(self):
        self.daemon.post("/effect?name=solid")
        self.daemon.post("/color?hex=ffffff")
        self.daemon.post("/transition?ms=0&style=cut")
        time.sleep(0.4)
        sent = self.daemon.get("/status")["frames_sent"]
        time.sleep(0.4)
        self.assertEqual(self.daemon.get("/status")["frames_sent"], sent)

    def test_wled_backend_posts_segments(self):
        import http.server
        import threading

        seen = []

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                seen.append(json.loads(self.rfile.read(length) or b"{}"))
                self.send_response(200)
                self.send_header("Content-Length", "0")
                self.end_headers()

            def log_message(self, *args):
                pass

        server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        port = server.server_address[1]
        try:
            self.daemon.close()
            self.daemon = Daemon(self.dir, ["--backend", "wled",
                                            "--target", "127.0.0.1",
                                            "--target-port", str(port)])
            self.daemon.post("/effect?name=solid")
            self.daemon.post("/color?hex=102030")
            self.daemon.post("/brightness?value=100")
            self.daemon.post("/transition?ms=0&style=cut")
            deadline = time.time() + 5
            while time.time() < deadline:
                if seen and seen[-1].get("bri") == 255:
                    break
                time.sleep(0.05)
        finally:
            server.shutdown()
            server.server_close()
        self.assertTrue(seen, "no WLED request was sent")
        body = seen[-1]
        self.assertTrue(body["on"])
        self.assertEqual(body["bri"], 255)
        self.assertEqual(body["seg"][0]["col"][0], [0x10, 0x20, 0x30])
        self.assertEqual(len(body["seg"]), 4)
        self.assertEqual(self.daemon.get("/status")["output_errors"], 0)

    def test_wled_backend_reports_an_unreachable_target(self):
        self.daemon.close()
        port = free_port()
        self.daemon = Daemon(self.dir, ["--backend", "wled",
                                        "--target", "127.0.0.1",
                                        "--target-port", str(port)])
        self.daemon.post("/effect?name=solid")
        self.daemon.post("/transition?ms=0&style=cut")
        time.sleep(0.6)
        status = self.daemon.get("/status")
        self.assertGreater(status["output_errors"], 0)
        self.assertTrue(status["last_error"])

    # ---- validation -----------------------------------------------------
    def test_validation_errors(self):
        cases = [
            "/effect?name=nope",
            "/effect",
            "/brightness?value=500",
            "/brightness",
            "/color?r=300&g=0&b=0",
            "/color?r=1",
            "/transition?ms=99999",
            "/transition?ms=100&style=nope",
            "/reactive?enabled=7",
            "/preset?name=Missing",
            "/preset",
            "/config?backend=nope",
            "/config?fps=0",
            "/config?output_hz=0",
            "/config?target_port=0",
        ]
        for path in cases:
            code, body = self.daemon.post_error(path)
            self.assertEqual(code, 400, f"{path} -> {code}")
            self.assertFalse(body.get("ok"))
            self.assertTrue(body.get("error"))

    def test_capture_endpoint_reports_state_and_serves_the_file(self):
        # The page's "Capture frames" button POSTs /capture and then polls /status
        # and /capture.txt.  The old handler filtered on an interface that does not
        # exist on this unit (com.lge.car.micom.can), so the file stayed empty and
        # the button looked broken; the endpoint must at least report honestly.
        self.assertTrue(self.daemon.get("/status")["capture"] in ("idle", "running", "done"))
        status = self.daemon.get("/status")
        self.assertIn("capture_events", status)
        self.assertIn("capture_light_hits", status)
        body = self.daemon.post("/capture")
        self.assertTrue(body["ok"])
        self.assertEqual(body["capture"], "running")
        self.assertEqual(body["toggle_in_s"], 8)
        # The script is written next to the state, and the read-back endpoint
        # answers even before the capture window finished.
        served = self.daemon.get("/capture.txt")
        self.assertTrue(served["ok"])
        self.assertEqual(served["file"], "hw_capture.txt")
        import pathlib
        script = pathlib.Path(self.dir) / "hw_capture.sh"
        self.assertTrue(script.is_file(), "capture script was not written")
        text = script.read_text()
        self.assertIn("com[.]lge[.]car[.]micom", text)
        self.assertNotIn("micom.can", text)   # the interface that never existed

    def test_unknown_route(self):
        with self.assertRaises(urllib.error.HTTPError) as ctx:
            self.daemon.get("/nope")
        with ctx.exception as exc:
            self.assertEqual(exc.code, 404)


class ValidationPolicyTests(unittest.TestCase):
    """Colour/template helpers have no HTTP surface of their own."""

    def test_status_json_is_valid(self):
        build_daemon()
        with tempfile.TemporaryDirectory(prefix="ambient-oneshot-") as tmp:
            out = subprocess.run(
                [str(BIN), "--state-dir", tmp, "--status", "--quiet"],
                capture_output=True, text=True, timeout=15)
            self.assertEqual(out.returncode, 0)
            data = json.loads(out.stdout)
            self.assertTrue(data["ok"])
            self.assertIn("effect", data)


if __name__ == "__main__":
    unittest.main(verbosity=2)
