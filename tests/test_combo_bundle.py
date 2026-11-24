#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Host tests for the combo bundle: the layer that kept breaking.

Every failure that reached the car on 2026-11-23 was catchable here:
 * a stage script that does not parse under /bin/sh,
 * a stage that uses a variable before assigning it (the scripts run with set -u,
   so that kills the stage silently and the master reports status=missing),
 * a corrupt stage.sh on the medium (the baked digest table must match),
 * the QML injector breaking on a base file that carries marker comments,
 * the boot-profile / perf collectors failing to produce their files.

Run:  python3 -m unittest discover -s tests -v
No car, no rootfs, no stick: everything works on temp files.
"""
import hashlib
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
COMPOSE = ROOT / "packages" / "compose.sh"
INJECT = ROOT / "packages" / "ambient" / "scripts" / "qml_inject.sh"
BOOT = ROOT / "packages" / "common" / "boot_profile.sh"
PERF = ROOT / "packages" / "common" / "perf_snapshot.sh"
STAGED = ROOT / "out" / "firstround"

DOLLAR = "$"


def shell_ok(path):
    done = subprocess.run(["/bin/sh", "-n", str(path)],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return done.returncode == 0, done.stderr.decode("utf-8", "replace")


def shipped_scripts():
    found = []
    for script in sorted((ROOT / "packages").glob("*/scripts/*.sh")):
        found.append(script)
    for script in sorted((ROOT / "packages" / "common").glob("*.sh")):
        found.append(script)
    return found


class ShellSyntax(unittest.TestCase):
    def test_every_shipped_script_parses(self):
        broken = []
        for script in shipped_scripts():
            ok, err = shell_ok(script)
            if not ok:
                first = err.strip().splitlines()[0] if err.strip() else "?"
                broken.append("%s: %s" % (script.relative_to(ROOT), first))
        self.assertEqual(broken, [], "scripts that do not parse:\n  " + "\n  ".join(broken))

    def test_compose_parses(self):
        ok, err = shell_ok(COMPOSE)
        self.assertTrue(ok, err)


class EarlyVariableUse(unittest.TestCase):
    """A var used before its assignment kills a set -u stage without a message.

    Assignments are plain VAR=, indented assignments, and "for VAR in".
    References are matched with word boundaries so $Q inside $QML_DIR is not
    mistaken for a reference to Q (the first version of this test did exactly
    that and drowned the real signal in noise).
    """

    ASSIGN = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]*)=")
    FOR_IN = re.compile(r"^\s*for\s+([A-Za-z_][A-Za-z0-9_]*)\s+in\b")

    FUNC = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*\s*\(\)\s*\{?")
    MAX_TOP_LINES = 400

    def check(self, path):
        all_lines = path.read_text(errors="replace").splitlines()
        # Only the top-level prologue is checked: after the first function
        # definition the data flows at call time, and a lexical check there
        # reports loop/read variables as false positives (which is exactly what
        # buried the two real bugs this test exists for).
        cut = len(all_lines)
        for i, line in enumerate(all_lines):
            if self.FUNC.match(line):
                cut = i
                break
        cut = min(cut, self.MAX_TOP_LINES)
        lines = all_lines[:cut]
        first = {}
        for i, line in enumerate(lines):
            for rx in (self.ASSIGN, self.FOR_IN):
                m = rx.match(line)
                if m:
                    first.setdefault(m.group(1), i)
        problems = []
        for i, line in enumerate(lines):
            if line.lstrip().startswith("#"):
                continue
            for var, at in first.items():
                if i >= at:
                    continue
                plain = re.compile(r"(?<![A-Za-z0-9_])" + re.escape(DOLLAR) + re.escape(var) + r"(?![A-Za-z0-9_])")
                braced = DOLLAR + "{" + var
                if not plain.search(line) and braced not in line:
                    continue
                if (braced + ":-") in line or (braced + "-") in line:
                    continue
                problems.append("%s:%d uses %s%s before line %d"
                                % (path.name, i + 1, DOLLAR, var, at + 1))
        return problems

    def test_no_early_use_in_stage_scripts(self):
        problems = []
        for script in sorted((ROOT / "packages").glob("*/scripts/run_stuff.sh")):
            problems.extend(self.check(script))
        self.assertEqual(problems, [], "\n  ".join(["early variable use:"] + problems))


class StagedBundle(unittest.TestCase):
    @unittest.skipUnless((STAGED / "run_stuff.sh").is_file(), "no staged bundle in out/firstround")
    def test_master_parses(self):
        ok, err = shell_ok(STAGED / "run_stuff.sh")
        self.assertTrue(ok, err)

    @unittest.skipUnless((STAGED / "run_stuff.sh").is_file(), "no staged bundle in out/firstround")
    def test_baked_digests_match_the_files(self):
        text = (STAGED / "run_stuff.sh").read_text(errors="replace")
        block = re.search(r'^STAGE_DIGESTS="\n(.*?)\n"$', text, re.S | re.M)
        self.assertIsNotNone(block, "STAGE_DIGESTS block missing from the master")
        entries = [l for l in block.group(1).splitlines() if l.strip()]
        self.assertGreater(len(entries), 0, "empty digest table")
        for line in entries:
            want, rel = line.split(None, 1)
            target = STAGED / rel
            self.assertTrue(target.is_file(), "digest for a missing file: " + rel)
            got = hashlib.md5(target.read_bytes()).hexdigest()
            self.assertEqual(got, want, "corrupt stage on disk: " + rel)

    @unittest.skipUnless((STAGED / "combo").is_dir(), "no staged combo tree")
    def test_every_stage_writes_a_result(self):
        for stage in sorted((STAGED / "combo").glob("*/stage.sh")):
            body = stage.read_text(errors="replace")
            self.assertIn("run_stuff_result", body,
                          stage.parent.name + "/stage.sh never writes run_stuff_result.txt")

    @unittest.skipUnless((STAGED / "combo").is_dir(), "no staged combo tree")
    def test_helpers_are_manifested(self):
        preflight = STAGED / "combo" / "preflight.md5"
        self.assertTrue(preflight.is_file(), "combo/preflight.md5 missing")
        done = subprocess.run(["md5sum", "-c", "combo/preflight.md5"], cwd=str(STAGED),
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.assertEqual(done.returncode, 0, done.stdout.decode("utf-8", "replace"))


class QmlInjector(unittest.TestCase):
    BASE = (
        "import QtQuick 2.0\n"
        "Item {\n"
        "    id: root\n"
        '    // WiFi marker: { "state": "client" }  // deliberately unbalanced\n'
        "    Item {\n"
        "        id: menuPage\n"
        "        Item { id: inner }\n"
        "    }\n"
        "}\n"
    )

    def run_inject(self, workdir, args):
        env = dict(os.environ)
        env["QML_INJECT_BACKUP"] = str(Path(workdir) / "backup")
        return subprocess.run(["/bin/sh", str(INJECT)] + args, cwd=str(workdir), env=env,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def test_anchor_append_and_revert(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            target = work / "Advanced.qml"
            target.write_text(self.BASE)
            block = work / "tile.qml"
            block.write_text("    Item {\n        id: injectedTile\n    }\n")

            done = self.run_inject(work, ["--file", str(target), "--anchor", "menuPage",
                                          "--block", str(block)])
            self.assertEqual(done.returncode, 0, done.stderr.decode())
            self.assertIn("injectedTile", target.read_text())

            done = self.run_inject(work, ["--file", str(target), "--append", "--block", str(block)])
            self.assertEqual(done.returncode, 0, done.stderr.decode())
            self.assertEqual(target.read_text().count("injectedTile"), 2)

            done = self.run_inject(work, ["--file", str(target), "--revert"])
            self.assertEqual(done.returncode, 0, done.stderr.decode())
            self.assertEqual(target.read_text(), self.BASE)

    def test_dry_run_does_not_write(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            target = work / "Advanced.qml"
            target.write_text(self.BASE)
            block = work / "page.qml"
            block.write_text("    Item { id: injectedPage }\n")
            done = self.run_inject(work, ["--file", str(target), "--append",
                                          "--block", str(block), "--dry-run"])
            self.assertEqual(done.returncode, 0, done.stderr.decode())
            self.assertEqual(target.read_text(), self.BASE)

    def test_missing_anchor_fails_closed(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            target = work / "Advanced.qml"
            target.write_text(self.BASE)
            block = work / "tile.qml"
            block.write_text("    Item { id: x }\n")
            done = self.run_inject(work, ["--file", str(target), "--anchor", "noSuchId",
                                          "--block", str(block)])
            self.assertNotEqual(done.returncode, 0)
            self.assertEqual(target.read_text(), self.BASE)


class Collectors(unittest.TestCase):
    def test_boot_profile_writes_report_and_csv(self):
        with tempfile.TemporaryDirectory() as tmp:
            done = subprocess.run(["/bin/sh", str(BOOT), tmp],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.assertEqual(done.returncode, 0, done.stderr.decode())
            out = Path(tmp)
            csv = out / "boot.csv"
            self.assertTrue(csv.is_file(), "boot.csv missing")
            line = csv.read_text().strip()
            self.assertIn("boot=", line)
            self.assertIn("uptime=", line)
            self.assertEqual(len(list(out.glob("*.txt"))), 1, "expected one report file")

    def test_perf_snapshot_writes_report_and_csv(self):
        with tempfile.TemporaryDirectory() as tmp:
            done = subprocess.run(["/bin/sh", str(PERF), "1", tmp],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.assertEqual(done.returncode, 0, done.stderr.decode())
            out = Path(tmp)
            self.assertTrue((out / "perf.txt").is_file(), "perf.txt missing")
            csv = out / "perf.csv"
            self.assertTrue(csv.is_file(), "perf.csv missing")
            line = csv.read_text().strip()
            for key in ("uptime=", "load=", "top_cpu=", "nav_cpu="):
                self.assertIn(key, line)


if __name__ == "__main__":
    unittest.main(verbosity=2)
