# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import unittest
from pathlib import Path

from harness.config import load_profile, load_scenario, scenario_command
from harness.results import evaluate

VM = Path(__file__).resolve().parents[1]


class DmengineFumoResumeRestartTests(unittest.TestCase):
    def test_scenario_contract(self) -> None:
        scenario = load_scenario('dmengine-fumo-resume-restart')
        self.assertEqual(scenario['requires'], ['payload'])
        self.assertEqual(scenario['requires_mode'], 'compat')
        self.assertEqual(set(scenario['checks']),
                         {'fumo-resume-restart-phase1-ready',
                          'fumo-resume-partial', 'fumo-resume-restart-state',
                          'fumo-resume-restart-server',
                          'fumo-resume-restart-phase2-ready',
                          'fumo-resume-request', 'fumo-resume-complete',
                          'fumo-resume-notify', 'fumo-no-production',
                          'dmengine-fumo-resume-restart'})
        self.assertIn('restarts the engine', scenario['description'])
        self.assertIn('No install', scenario['description'])

    def test_command_uses_restart_probe(self) -> None:
        command = scenario_command(
            load_scenario('dmengine-fumo-resume-restart'),
            load_profile('research'), kernel_target='compat')
        for text in ('MODE=partial', 'MOCK_DM_PARTIAL',
                     '/tmp/mock-dm 8002 /tmp/mock-dm.log resume',
                     'dlresume.dat', 'MOCK_DM_RESUME',
                     'fumo-resume-complete'):
            self.assertIn(text, command)

    def test_checks_fail_closed(self) -> None:
        scenario = load_scenario('dmengine-fumo-resume-restart')
        text = ''.join(f'HARNESS_CHECK {check} PASS\n'
                       for check in scenario['checks'])
        self.assertEqual(evaluate(text, 0, scenario)['verdict'], 'PASS')
        self.assertEqual(evaluate(
            text.replace('fumo-resume-request PASS',
                         'fumo-resume-request FAIL'),
            0, scenario)['verdict'], 'FAIL')


if __name__ == '__main__':
    unittest.main()
