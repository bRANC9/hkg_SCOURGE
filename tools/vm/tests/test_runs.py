# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import json
import os
import shutil
import socket
import tempfile
import unittest
from pathlib import Path

from harness import runs


class RunManagementTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="vm-runs-")
        self.root = Path(self._tmp.name)
        self.runs = self.root / "runs"
        self.runs.mkdir()
        for index in range(5):
            path = self.runs / f"run-{index}"
            path.mkdir()
            (path / "result.json").write_text(json.dumps({
                "label": "smoke" if index % 2 == 0 else "other",
                "verdict": "PASS" if index != 4 else "FAIL",
                "mode": "compat",
                "scenario": {"name": "boot"},
                "keep_overlay": index == 1,
                "screenshot": {},
            }))
            os.utime(path, (1000 + index, 1000 + index))
            (path / "shot.png").write_bytes(b"png")

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_collect_and_filter(self) -> None:
        all_runs = runs.collect_runs(self.root)
        self.assertEqual(len(all_runs), 5)
        self.assertEqual(all_runs[0]["run"], "run-4")  # newest mtime
        self.assertEqual([item["run"] for item in runs.filter_runs(all_runs, label="smoke")],
                         ["run-4", "run-2", "run-0"])
        self.assertEqual([item["run"] for item in runs.filter_runs(all_runs, verdict="FAIL")],
                         ["run-4"])

    def test_prune_is_dry_run_by_default(self) -> None:
        report = runs.prune(self.root, keep=2)
        self.assertFalse(report["apply"])
        self.assertEqual(len(report["candidates"]), 2)
        self.assertTrue((self.runs / "run-0").is_dir())
        self.assertTrue(all((self.runs / f"run-{i}").is_dir() for i in range(5)))

    def test_prune_protects_retained_overlays_and_applies(self) -> None:
        report = runs.prune(self.root, keep=2, apply=True)
        self.assertTrue(report["apply"])
        self.assertIn("run-1", [item["run"] for item in report["protected"]])
        self.assertTrue((self.runs / "run-1").is_dir())
        self.assertFalse((self.runs / "run-0").exists())
        self.assertFalse((self.runs / "run-2").exists())
        self.assertTrue((self.runs / "run-3").is_dir())
        self.assertTrue((self.runs / "run-4").is_dir())

    def test_prune_protects_daemon_and_resultless_dirs(self) -> None:
        daemon_dir = self.runs / "daemon"
        daemon_dir.mkdir()
        (daemon_dir / "control.sock").write_text("")
        regression = self.runs / "regression-123"
        regression.mkdir()
        (regression / "summary.json").write_text("{}")
        os.utime(daemon_dir, (1, 1))
        os.utime(regression, (1, 1))
        report = runs.prune(self.root, keep=1, apply=True)
        self.assertTrue(daemon_dir.is_dir())
        self.assertTrue(regression.is_dir())
        reasons = {item["run"]: item["reason"] for item in report["protected"]}
        self.assertEqual(reasons.get("daemon"), "daemon")
        self.assertEqual(reasons.get("regression-123"), "no-result")

    def test_extract_and_path_traversal(self) -> None:
        metadata = runs.extract(self.root, "run-3", "shot.png", self.root / "out.png")
        self.assertEqual((self.root / "out.png").read_bytes(), b"png")
        self.assertEqual(metadata["size"], 3)
        with self.assertRaises(ValueError):
            runs.extract(self.root, "run-3", "../run-2/result.json", self.root / "bad")
        with self.assertRaises(ValueError):
            runs.extract(self.root, "../run-2", "result.json", self.root / "bad")

    @staticmethod
    def _bind_socket(path: Path) -> None:
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            server.bind(str(path))
        finally:
            server.close()

    def test_long_qmp_path_redirects_to_temp_pointer(self) -> None:
        short = self.runs / "short-run"
        short.mkdir()
        socket_path, temp_dir = runs.allocate_qmp_socket(short)
        self.assertEqual(socket_path, short / "qmp.sock")
        self.assertIsNone(temp_dir)

        base = Path(tempfile.mkdtemp(prefix="vm-long-",
                                     dir=tempfile.gettempdir()))
        long_run = base
        while len(str(long_run / "qmp.sock").encode()) <= 107:
            long_run = long_run / ("x" * 40)
        long_run.mkdir(parents=True)
        temp_dir = None
        try:
            socket_path, temp_dir = runs.allocate_qmp_socket(long_run)
            self.assertIsNotNone(temp_dir)
            self.assertLessEqual(len(str(socket_path).encode()), 107)
            pointer = long_run / "qmp.path"
            self.assertEqual(pointer.read_text().strip(), str(socket_path))
            self.assertEqual(runs.qmp_socket_from_run(long_run), socket_path)
        finally:
            if temp_dir is not None:
                shutil.rmtree(temp_dir, ignore_errors=True)
            shutil.rmtree(base, ignore_errors=True)

    def test_run_qmp_socket_uses_pointer(self) -> None:
        live = self.runs / "run-pointer"
        live.mkdir()
        redirect = self.root / "redirect.sock"
        self._bind_socket(redirect)
        (live / "qmp.path").write_text(str(redirect) + "\n")
        self.assertEqual(runs.run_qmp_socket(self.root, "run-pointer"),
                         redirect)
        self.assertEqual(runs.latest_live(self.root)["qmp"], str(redirect))

    def test_run_qmp_socket_resolution(self) -> None:
        live = self.runs / "run-live"
        live.mkdir()
        self._bind_socket(live / "qmp.sock")
        self.assertEqual(runs.run_qmp_socket(self.root, "run-live"),
                         live / "qmp.sock")
        self.assertEqual(runs.run_qmp_socket(self.root, str(live)),
                         live / "qmp.sock")
        self.assertEqual(runs.run_qmp_socket(self.root, f"runs/{live.name}"),
                         live / "qmp.sock")
        with self.assertRaises(ValueError):
            runs.run_qmp_socket(self.root, "../run-live")
        with self.assertRaisesRegex(ValueError, "QMP socket missing"):
            runs.run_qmp_socket(self.root, "run-1")

    def test_latest_live_prefers_newest_run_then_daemon(self) -> None:
        older = self.runs / "run-live-old"
        older.mkdir()
        self._bind_socket(older / "qmp.sock")
        newer = self.runs / "run-live-new"
        newer.mkdir()
        self._bind_socket(newer / "qmp.sock")
        os.utime(older / "qmp.sock", (100, 100))
        os.utime(newer / "qmp.sock", (200, 200))
        completed = self.runs / "run-complete"
        completed.mkdir()
        self._bind_socket(completed / "qmp.sock")
        (completed / "result.json").write_text("{}")
        os.utime(completed / "qmp.sock", (300, 300))

        target = runs.latest_live(self.root)
        self.assertEqual(target["kind"], "qmp")
        self.assertEqual(Path(target["qmp"]), newer / "qmp.sock")

        daemon_dir = self.runs / "daemon"
        daemon_dir.mkdir()
        self._bind_socket(daemon_dir / "qmp.sock")
        (daemon_dir / "status.json").write_text(
            json.dumps({"state": "ready"}))
        target = runs.latest_live(self.root)
        self.assertEqual(target["kind"], "daemon")
        self.assertEqual(Path(target["directory"]), daemon_dir)


if __name__ == "__main__":
    unittest.main()
