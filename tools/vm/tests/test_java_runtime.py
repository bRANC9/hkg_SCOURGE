# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import unittest
from pathlib import Path

from harness.config import SCENARIO_NAMES, load_profile, load_scenario, scenario_command

VM = Path(__file__).resolve().parents[1]


class JavaRuntimeScenarioTests(unittest.TestCase):
    def test_scenario_is_registered_and_requires_payload(self) -> None:
        self.assertIn("java-runtime", SCENARIO_NAMES)
        scenario = load_scenario("java-runtime")
        self.assertEqual(scenario["requires"], ["payload"])
        self.assertNotIn("requires_mode", scenario)

    def test_probe_emits_every_required_check(self) -> None:
        scenario = load_scenario("java-runtime")
        command = scenario_command(scenario, load_profile("research"))
        self.assertIn("JAVA_SELFTEST_OK", command)
        self.assertIn("HARNESS_BLOCKED java payload stick missing", command)
        for check in scenario["checks"]:
            self.assertIn(f"HARNESS_CHECK {check} PASS", command)

    def test_probe_uses_the_real_installer_and_restore(self) -> None:
        command = scenario_command(load_scenario("java-runtime"),
                                   load_profile("research"))
        self.assertIn("install/run_stuff.sh", command)
        self.assertIn("install/run_stuff_restore.sh", command)
        self.assertIn("JAVA_RW_DIR", command)
        self.assertIn("JAVA_APPS_DIR", command)
        self.assertIn("vmtest.jar", command)
        self.assertIn("jcmd", command)

    def test_probe_is_not_used_as_a_policy_claim(self) -> None:
        scenario = load_scenario("java-runtime")
        text = scenario_command(scenario, load_profile("research")).lower()
        self.assertNotIn("smack policy", text)
        self.assertNotIn("car-validated", text)


if __name__ == "__main__":
    unittest.main()
