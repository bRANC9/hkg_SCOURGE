# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import unittest
from pathlib import Path

from harness.config import load_profile, load_scenario, scenario_command
from harness.results import evaluate

VM = Path(__file__).resolve().parents[1]


class DmengineFumoResumeTests(unittest.TestCase):
    def test_scenario_contract(self) -> None:
        scenario = load_scenario('dmengine-fumo-resume')
        self.assertEqual(scenario['requires'], ['payload'])
        self.assertEqual(scenario['requires_mode'], 'compat')
        self.assertEqual(set(scenario['checks']),
                         {'fumo-resume-ready', 'fumo-resume-partial',
                          'fumo-resume-request', 'fumo-resume-complete',
                          'fumo-resume-notify', 'fumo-no-production',
                          'dmengine-fumo-resume'})
        self.assertIn('Range: bytes=64-', scenario['description'])
        self.assertIn('No install', scenario['description'])

    def test_command_uses_resume_probe(self) -> None:
        command = scenario_command(
            load_scenario('dmengine-fumo-resume'), load_profile('research'),
            kernel_target='compat')
        for text in ('MODE=resume', 'MOCK_DM_PARTIAL',
                     'MOCK_DM_RESUME method=GET path=/object.bin offset=64',
                     'MOCK_DM_OBJECT_HEADERS range=Range: bytes=64-',
                     'fumo-resume-complete'):
            self.assertIn(text, command)

    def test_checks_fail_closed(self) -> None:
        scenario = load_scenario('dmengine-fumo-resume')
        text = ''.join(f'HARNESS_CHECK {check} PASS\n'
                       for check in scenario['checks'])
        self.assertEqual(evaluate(text, 0, scenario)['verdict'], 'PASS')
        self.assertEqual(evaluate(
            text.replace('fumo-resume-request PASS',
                         'fumo-resume-request FAIL'),
            0, scenario)['verdict'], 'FAIL')


if __name__ == '__main__':
    unittest.main()
