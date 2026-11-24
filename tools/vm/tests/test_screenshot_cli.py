# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import argparse
import base64
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path

VM = Path(__file__).resolve().parents[1]

PNG_1X1 = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAFgwJ/lK3Q6wAAAABJRU5ErkJggg=="
)


class FakeQmpServer(threading.Thread):
    """Accept QMP connections until closed; write a 1x1 PNG per screendump."""

    def __init__(self, path: Path) -> None:
        super().__init__(daemon=True)
        self.path = Path(path)
        self.stop_event = threading.Event()
        self.ready = threading.Event()
        self.error: BaseException | None = None
        self.requests = 0

    def run(self) -> None:
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            server.bind(str(self.path))
            server.listen(4)
            server.settimeout(0.05)
            self.ready.set()
            while not self.stop_event.is_set():
                try:
                    conn, _ = server.accept()
                except socket.timeout:
                    continue
                handle = conn.makefile("rwb", buffering=0)
                try:
                    handle.write(b'{"QMP": {"version": {"qemu": {"major": 11}}}}\n')
                    while True:
                        line = handle.readline()
                        if not line:
                            break
                        request = json.loads(line)
                        if request.get("execute") == "screendump":
                            output = Path(request["arguments"]["filename"])
                            output.write_bytes(PNG_1X1)
                            self.requests += 1
                        handle.write(b'{"return": {}}\n')
                finally:
                    handle.close()
                    conn.close()
        except BaseException as exc:  # noqa: BLE001
            self.error = exc
            self.ready.set()
        finally:
            server.close()
            self.path.unlink(missing_ok=True)

    def close(self) -> None:
        self.stop_event.set()
        self.join(2)


class ScreenshotCliTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="vm-screenshot-cli-")
        self.root = Path(self._tmp.name)
        self.sock = self.root / "qmp.sock"
        self.server = FakeQmpServer(self.sock)
        self.server.start()
        self.assertTrue(self.server.ready.wait(2))
        if self.server.error is not None:
            self.fail(f"fake QMP server failed: {self.server.error}")

    def tearDown(self) -> None:
        self.server.close()
        self._tmp.cleanup()

    def cli(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, "-B", str(VM / "launch.py"), "screenshot", *args],
            cwd=VM, capture_output=True, text=True, timeout=30)

    def test_single_capture_to_explicit_path(self) -> None:
        output = self.root / "shot.png"
        completed = self.cli("--qmp", str(self.sock), "--output", str(output),
                             "--no-pixels", "--timeout", "5")
        self.assertEqual(completed.returncode, 0, completed.stderr)
        payload = json.loads(completed.stdout)
        self.assertTrue(payload["ok"])
        self.assertEqual(Path(payload["path"]), output)
        self.assertEqual(payload["size"], len(PNG_1X1))
        self.assertEqual((payload["width"], payload["height"]), (1, 1))
        self.assertEqual(output.read_bytes(), PNG_1X1)
        self.assertEqual(self.server.requests, 1)

    def test_sequence_capture_names_each_frame(self) -> None:
        output = self.root / "burst.png"
        completed = self.cli("--qmp", str(self.sock), "--output", str(output),
                             "--shots", "2", "--interval", "0.01",
                             "--no-pixels", "--timeout", "5")
        self.assertEqual(completed.returncode, 0, completed.stderr)
        payload = json.loads(completed.stdout)
        self.assertTrue(payload["ok"])
        self.assertEqual(len(payload["captures"]), 2)
        first = self.root / "burst-000.png"
        second = self.root / "burst-001.png"
        self.assertEqual(Path(payload["captures"][0]["path"]), first)
        self.assertEqual(Path(payload["captures"][1]["path"]), second)
        self.assertTrue(first.is_file())
        self.assertTrue(second.is_file())
        self.assertEqual(self.server.requests, 2)

    def test_default_output_lives_beside_qmp_socket(self) -> None:
        completed = self.cli("--qmp", str(self.sock), "--no-pixels",
                             "--timeout", "5")
        self.assertEqual(completed.returncode, 0, completed.stderr)
        payload = json.loads(completed.stdout)
        path = Path(payload["path"])
        self.assertEqual(path.parent, self.root)
        self.assertTrue(path.name.startswith("live-screenshot-"))
        self.assertTrue(path.is_file())

    def test_run_target_resolution_and_missing_socket(self) -> None:
        run_name = f"shot{os.getpid()}"
        run_dir = VM / "runs" / run_name
        run_dir.mkdir(parents=True)
        try:
            run_sock = run_dir / "qmp.sock"
            server = FakeQmpServer(run_sock)
            server.start()
            self.assertTrue(server.ready.wait(2))
            if server.error is not None:
                self.fail(f"fake QMP server failed: {server.error}")
            try:
                completed = self.cli("--run", run_name, "--no-pixels",
                                     "--timeout", "5")
                self.assertEqual(completed.returncode, 0, completed.stderr)
                payload = json.loads(completed.stdout)
                self.assertEqual(Path(payload["path"]).parent, run_dir)
            finally:
                server.close()
        finally:
            for path in run_dir.glob("*"):
                path.unlink(missing_ok=True)
            run_dir.rmdir()

        missing = self.cli("--run", "does-not-exist", "--no-pixels")
        self.assertEqual(missing.returncode, 1)
        payload = json.loads(missing.stdout)
        self.assertFalse(payload["ok"])
        self.assertIn("QMP socket missing", payload["error"])

    def test_invalid_sequence_arguments(self) -> None:
        shots = self.cli("--qmp", str(self.sock), "--shots", "0")
        self.assertEqual(shots.returncode, 2)
        self.assertIn("--shots must be positive", shots.stderr)
        interval = self.cli("--qmp", str(self.sock), "--interval", "-1")
        self.assertEqual(interval.returncode, 2)
        self.assertIn("--interval must be", interval.stderr)


class InputCliTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="vm-input-cli-")
        self.root = Path(self._tmp.name)
        self.sock = self.root / "qmp.sock"
        self.server = FakeQmpServer(self.sock)
        self.server.start()
        self.assertTrue(self.server.ready.wait(2))
        if self.server.error is not None:
            self.fail(f"fake QMP server failed: {self.server.error}")

    def tearDown(self) -> None:
        self.server.close()
        self._tmp.cleanup()

    def cli(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, "-B", str(VM / "launch.py"), "input",
             "--qmp", str(self.sock), *args],
            cwd=VM, capture_output=True, text=True, timeout=30)

    def test_click(self) -> None:
        completed = self.cli("--click", "640,360", "--width", "1280",
                             "--height", "720")
        self.assertEqual(completed.returncode, 0, completed.stderr)
        payload = json.loads(completed.stdout)
        self.assertTrue(payload["ok"])
        self.assertEqual((payload["x"], payload["y"]), (640, 360))

    def test_drag(self) -> None:
        completed = self.cli("--drag", "900,360,400,360", "--width", "1280",
                             "--height", "720", "--button", "left")
        self.assertEqual(completed.returncode, 0, completed.stderr)
        payload = json.loads(completed.stdout)
        self.assertTrue(payload["ok"])
        self.assertEqual((payload["x1"], payload["y1"],
                          payload["x2"], payload["y2"]),
                         (900, 360, 400, 360))
        self.assertGreaterEqual(payload["steps"], 1)

    def test_requires_one_input_mode(self) -> None:
        completed = self.cli()
        self.assertEqual(completed.returncode, 2)
        self.assertIn("exactly one of", completed.stderr)

    def test_drag_out_of_bounds(self) -> None:
        completed = self.cli("--drag", "1000,1000,2000,1000",
                             "--width", "1280", "--height", "720")
        self.assertEqual(completed.returncode, 2)
        self.assertIn("drag point outside", completed.stderr)


class DisplaySizeTests(unittest.TestCase):
    def test_parse_display_size(self) -> None:
        sys.path.insert(0, str(VM))
        import launch

        self.assertEqual(launch.parse_display_size("1920x720"), (1920, 720))
        self.assertEqual(launch.parse_display_size("1920X720"), (1920, 720))
        for value in ("bad", "100x100", "1920", "1920x", "4000x720"):
            with self.assertRaises(argparse.ArgumentTypeError):
                launch.parse_display_size(value)


if __name__ == "__main__":
    unittest.main()
