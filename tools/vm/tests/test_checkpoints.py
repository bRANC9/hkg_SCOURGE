# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest.mock import patch

from harness.checkpoints import (CheckpointWatcher, normalize_checkpoints,
                                 required_failures, validate_checkpoints)


class FakeSerial:
    def __init__(self) -> None:
        self.log = bytearray()
        self.cond = threading.Condition()
        self.eof = False

    def append(self, text: str) -> None:
        with self.cond:
            self.log += text.encode()


def fake_capture(socket_path: Path, output: Path) -> dict:
    output.write_bytes(b"png")
    return {
        "path": str(output),
        "size": 3,
        "sha256": "a" * 64,
        "width": 1,
        "height": 1,
    }


class CheckpointValidationTests(unittest.TestCase):
    def test_valid_contract(self) -> None:
        self.assertEqual(validate_checkpoints(None), [])
        value = [
            {"name": "a", "after_check": "one"},
            {"name": "b", "after_marker": "READY", "delay": 0.5},
            {"name": "c", "after_seconds": 5, "required": True,
             "nonblank": True},
        ]
        self.assertEqual(validate_checkpoints(value), [])
        self.assertEqual(len(normalize_checkpoints(value)), 3)

    def test_invalid_contract(self) -> None:
        value = [
            {"name": "../x", "after_check": "one"},
            {"name": "a", "after_check": "one", "after_seconds": 1},
            {"name": "a", "after_marker": ""},
            {"name": "b", "after_seconds": -1, "delay": -0.1},
            {"name": "c", "after_check": "one", "required": "yes"},
            {"name": "d", "after_marker": "A\nB"},
        ]
        joined = "\n".join(validate_checkpoints(value))
        for expected in ("name", "exactly one", "duplicated", "non-empty string",
                         "after_seconds", "delay", "required"):
            self.assertIn(expected, joined)
        self.assertIn("at most", "\n".join(validate_checkpoints([{}] * 17)))

    def test_normalize_raises(self) -> None:
        with self.assertRaises(ValueError):
            normalize_checkpoints([{"name": "a"}])


class CheckpointWatcherTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="vm-checkpoints-")
        self.root = Path(self._tmp.name)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _watcher(self, serial, points):
        return CheckpointWatcher(
            serial, self.root / "qmp.sock", self.root / "shots", points,
            capture=fake_capture, pixels=False, poll=0.02)

    def _wait_for(self, path: Path, timeout: float = 2.0) -> bool:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if path.exists():
                return True
            time.sleep(0.02)
        return False

    def test_captures_check_once_even_when_marker_repeats(self) -> None:
        serial = FakeSerial()
        watcher = self._watcher(serial, [
            {"name": "ready", "after_check": "ready"},
        ])
        watcher.start()
        serial.append("HARNESS_CHECK ready PASS\n")
        serial.append("HARNESS_CHECK ready PASS\n")
        output = self.root / "shots" / "ready.png"
        self.assertTrue(self._wait_for(output))
        entries = watcher.finalize()
        self.assertEqual(len(entries), 1)
        self.assertEqual(entries[0]["name"], "ready")
        self.assertIn("after_check=ready", entries[0]["trigger"])
        self.assertIsNone(entries[0].get("error"))
        self.assertEqual(output.read_bytes(), b"png")

    def test_marker_is_an_exact_line(self) -> None:
        serial = FakeSerial()
        watcher = self._watcher(serial, [
            {"name": "ready", "after_marker": "READY"},
        ])
        watcher.start()
        serial.append("NOT READY\nREADY extra\n")
        entries = watcher.finalize()
        self.assertEqual(len(entries), 1)
        self.assertTrue(entries[0].get("missed"))
        self.assertFalse((self.root / "shots" / "ready.png").exists())

    def test_split_marker_and_delay(self) -> None:
        serial = FakeSerial()
        watcher = self._watcher(serial, [
            {"name": "probe", "after_check": "probe", "delay": 0.05},
        ])
        watcher.start()
        serial.append("HARNESS_CHECK pro")
        serial.append("be PASS\n")
        output = self.root / "shots" / "probe.png"
        self.assertTrue(self._wait_for(output))
        entries = watcher.finalize()
        self.assertEqual(len(entries), 1)
        self.assertEqual(entries[0]["name"], "probe")

    def test_finalize_marks_missing_required_checkpoint(self) -> None:
        serial = FakeSerial()
        watcher = self._watcher(serial, [
            {"name": "never", "after_check": "never", "required": True},
            {"name": "late", "after_seconds": 60},
        ])
        watcher.start()
        entries = watcher.finalize()
        self.assertEqual(len(entries), 2)
        for entry in entries:
            self.assertTrue(entry.get("missed"))
        reasons = required_failures(entries)
        self.assertEqual(len(reasons), 1)
        self.assertIn("never", reasons[0])

    def test_required_nonblank_failure(self) -> None:
        serial = FakeSerial()
        watcher = self._watcher(serial, [
            {"name": "blank", "after_seconds": 0, "required": True,
             "nonblank": True},
        ])
        with patch("harness.checkpoints.nonblack_bbox",
                   return_value={"count": 0, "bbox": None, "width": 1,
                                 "height": 1}):
            watcher.start()
            entries = watcher.finalize()
        self.assertEqual(len(entries), 1)
        reasons = required_failures(entries)
        self.assertEqual(len(reasons), 1)
        self.assertIn("blank", reasons[0])

    def test_nonblank_failure_without_required(self) -> None:
        serial = FakeSerial()
        watcher = self._watcher(serial, [
            {"name": "blank", "after_seconds": 0, "nonblank": True},
        ])
        with patch("harness.checkpoints.nonblack_bbox",
                   return_value={"count": 0, "bbox": None, "width": 1,
                                 "height": 1}):
            watcher.start()
            entries = watcher.finalize()
        self.assertEqual(required_failures(entries), ["checkpoint blank is blank"])

    def test_capture_failure_is_recorded(self) -> None:
        serial = FakeSerial()

        def broken(socket_path: Path, output: Path) -> dict:
            raise RuntimeError("no surface")

        watcher = CheckpointWatcher(
            serial, self.root / "qmp.sock", self.root / "shots",
            [{"name": "x", "after_seconds": 0, "required": True}],
            capture=broken, pixels=False, poll=0.02)
        watcher.start()
        entries = watcher.finalize()
        self.assertEqual(len(entries), 1)
        self.assertIn("no surface", entries[0]["error"])
        self.assertIn("failed", required_failures(entries)[0])


if __name__ == "__main__":
    unittest.main()
