#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Host tests for the Hello-base dump path: payload -> dump -> importer.

Why these exist: packages/wifi/base/hello* decides whether a hello-family QML
variant is built, and that variant is installed on the car. The synthesised route
was closed (build_artifacts._have() skips a base carrying a SYNTHESIZED marker),
so the only honest source is a dump off a unit that really ships the Hello tile.
These tests prove, on temp files only:
  * the payload copies exactly the files the builder reads, with a manifest,
  * a unit without the Hello tile yields verdict=absent instead of a fake base,
  * the importer refuses a damaged dump and a dump that cannot prove the tile,
  * a dumped base is accepted by the builder while a synthesised one never is.

Run:  python3 -m unittest discover -s tests -v
"""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PAYLOAD = ROOT / "payload" / "dump_hello.sample.sh"
IMPORTER = ROOT / "packages" / "wifi" / "scripts" / "install_hello_base.py"
BUILD = ROOT / "packages" / "wifi" / "scripts" / "build_artifacts.py"
ADV = "HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
BASE_FILES = (
    ADV,
    "HomeScreen/QHMI_HomeScreenMain_Wide.qml",
    "HomeScreen/Screen_HomeScreen_AllMenus.qml",
    "HomeScreen/Screen_HomeScreen_Submenu_Setup.qml",
    "component/QCustomizedMenuListAllowEmpty.qml",
)

ADV_WITH_TILE = """// fixture: the Hello app set
Item {
    id: helloTile1
    HelloTile { id: helloTile }
    Item { id: batteryPage }
    BatteryTile { id: batteryTile1 }
}
"""
ADV_STOCK = """// fixture: stock app set, no Hello tile
Item { id: homeRoot }
"""


def write_tree(qml: Path, advanced: str) -> None:
    (qml / "HomeScreen").mkdir(parents=True, exist_ok=True)
    (qml / "component").mkdir(parents=True, exist_ok=True)
    (qml / ADV).write_text(advanced)
    (qml / "HomeScreen/QHMI_HomeScreenMain_Wide.qml").write_text("// wide\n")
    (qml / "HomeScreen/Screen_HomeScreen_AllMenus.qml").write_text("// menus\n")
    (qml / "HomeScreen/Screen_HomeScreen_Submenu_Setup.qml").write_text("// submenu\n")
    (qml / "component/QCustomizedMenuListAllowEmpty.qml").write_text("// allow empty\n")
    (qml / "component/HelloTile.qml").write_text("// HelloTile component\n")


def run_payload(app_root: Path, usb: Path) -> subprocess.CompletedProcess:
    # SCOURGE_BEEP=0: the BEL fallback is loud on a laptop that routes the console
    # bell to the speakers, and these tests have nothing to listen for.
    env = dict(os.environ, APP_ROOT=str(app_root), USB_DIR=str(usb), SCOURGE_BEEP="0")
    return subprocess.run(["/bin/sh", str(PAYLOAD)], env=env, capture_output=True, text=True, timeout=120)


def dump_dir(usb: Path) -> Path:
    latest = (usb / "hello_dump" / "latest.txt").read_text().strip()
    return usb / "hello_dump" / latest


def run_importer(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run([os.sys.executable, str(IMPORTER), *args],
                          capture_output=True, text=True, timeout=60)


class HelloDumpPayloadTests(unittest.TestCase):
    def test_complete_dump_copies_the_builder_files_and_a_manifest(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_tree(root / "unit/share/AppHomeScreen/qml_eco3.0", ADV_WITH_TILE)
            usb = root / "stick"
            usb.mkdir()
            done = run_payload(root / "unit", usb)
            self.assertEqual(done.returncode, 0, done.stderr)
            dump = dump_dir(usb)
            report = (dump / "report.txt").read_text()
            self.assertIn("verdict=complete", report)
            self.assertIn("helloTile1=1", report)
            self.assertIn("root=qml_eco3.0", report)
            # A page-based battery base is recorded, not guessed.
            self.assertIn("battery_pages= HomeScreen/QHMI_HomeScreenMain_Advanced.qml", report)
            copied = dump / "roots/qml_eco3.0"
            for rel in BASE_FILES:
                self.assertTrue((copied / rel).is_file(), rel)
            self.assertTrue((copied / "component/HelloTile.qml").is_file())
            self.assertTrue((dump / "manifest.md5").is_file())
            self.assertTrue((usb / "hello_dump_result.txt").is_file())
            checked = run_importer("--dump", str(dump), "--check")
            self.assertEqual(checked.returncode, 0, checked.stderr + checked.stdout)

    def test_a_unit_without_the_hello_tile_reports_absent(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_tree(root / "unit/share/AppHomeScreen/qml_eco3.0", ADV_STOCK)
            usb = root / "stick"
            usb.mkdir()
            done = run_payload(root / "unit", usb)
            self.assertEqual(done.returncode, 0, done.stderr)
            dump = dump_dir(usb)
            report = (dump / "report.txt").read_text()
            self.assertIn("verdict=absent", report)
            self.assertIn("hello_tile=0", report)
            refused = run_importer("--dump", str(dump))
            self.assertEqual(refused.returncode, 2, refused.stdout)
            self.assertIn("no usable Hello base", refused.stderr)

    def test_a_unit_without_qml_roots_reports_absent(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            usb = root / "stick"
            usb.mkdir()
            done = run_payload(root / "unit", usb)
            self.assertEqual(done.returncode, 0, done.stderr)
            self.assertIn("roots=0", (dump_dir(usb) / "report.txt").read_text())


class HelloBaseImporterTests(unittest.TestCase):
    def _dump(self, root: Path, advanced: str = ADV_WITH_TILE) -> Path:
        write_tree(root / "unit/share/AppHomeScreen/qml_eco3.0", advanced)
        usb = root / "stick"
        usb.mkdir()
        self.assertEqual(run_payload(root / "unit", usb).returncode, 0)
        return dump_dir(usb)

    def test_installs_a_dumped_base_with_provenance(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            dump = self._dump(root)
            base = root / "base"
            done = run_importer("--dump", str(dump), "--base-dir", str(base))
            self.assertEqual(done.returncode, 0, done.stderr)
            for rel in BASE_FILES:
                self.assertTrue((base / "hello" / rel).is_file(), rel)
            marker = (base / "hello/.dumped").read_text()
            self.assertIn("source_root=qml_eco3.0", marker)
            self.assertNotIn("SYNTHESIZED", marker)

    def test_refuses_a_damaged_dump(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            dump = self._dump(root)
            victim = dump / "roots/qml_eco3.0" / ADV
            victim.write_text(victim.read_text() + "// tampered\n")
            done = run_importer("--dump", str(dump), "--base-dir", str(root / "base"))
            self.assertEqual(done.returncode, 3, done.stdout)
            self.assertIn("md5 mismatch", done.stderr)

    def test_battery_hello_requires_a_page_based_battery(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            no_page = ADV_WITH_TILE.replace("    Item { id: batteryPage }\n", "")
            dump = self._dump(root, advanced=no_page)
            done = run_importer("--dump", str(dump), "--as", "battery_hello",
                               "--base-dir", str(root / "base"))
            self.assertEqual(done.returncode, 2, done.stdout)
            self.assertIn("page-based battery", done.stderr)

    def test_battery_tile_hello_lands_under_the_name_the_builder_reads(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            dump = self._dump(root)
            base = root / "base"
            done = run_importer("--dump", str(dump), "--as", "battery_tile_hello",
                               "--base-dir", str(base))
            self.assertEqual(done.returncode, 0, done.stderr)
            self.assertTrue((base / "battery_tile_hello" / ADV).is_file())

    def test_existing_base_needs_force(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            dump = self._dump(root)
            base = root / "base"
            self.assertEqual(run_importer("--dump", str(dump), "--base-dir", str(base)).returncode, 0)
            again = run_importer("--dump", str(dump), "--base-dir", str(base))
            self.assertEqual(again.returncode, 2, again.stdout)
            self.assertIn("already exists", again.stderr)
            forced = run_importer("--dump", str(dump), "--base-dir", str(base), "--force")
            self.assertEqual(forced.returncode, 0, forced.stderr)


class BuilderBaseGateTests(unittest.TestCase):
    """_have() must accept a dumped base and refuse a synthesised one."""

    def _module(self):
        spec = importlib.util.spec_from_file_location("wifi_build_artifacts", BUILD)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def test_dumped_base_is_used_and_synthesised_is_skipped(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp) / "base"
            write_tree(base / "hello", ADV_WITH_TILE)
            module = self._module()
            module.BASE = str(base)
            self.assertTrue(module._have("hello", "HomeScreen", "QHMI_HomeScreenMain_Advanced.qml"))
            (base / "hello" / "SYNTHESIZED").write_text("fixture for the hello-family variants, not a dumped base.\n")
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertFalse(module._have("hello", "HomeScreen", "QHMI_HomeScreenMain_Advanced.qml"))


if __name__ == "__main__":
    unittest.main()
