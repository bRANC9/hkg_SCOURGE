# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import unittest
from pathlib import Path

from harness.config import load_profile, load_scenario, scenario_command
from harness.results import evaluate

VM = Path(__file__).resolve().parents[1]


class DmengineFumoResumeMultiTests(unittest.TestCase):
    def test_scenario_contract(self) -> None:
        scenario = load_scenario('dmengine-fumo-resume-multi')
        self.assertEqual(scenario['requires'], ['payload'])
        self.assertEqual(scenario['requires_mode'], 'compat')
        self.assertEqual(set(scenario['checks']),
                         {'fumo-multi-resume-ready',
                          'fumo-multi-resume-partial',
                          'fumo-multi-resume-request',
                          'fumo-multi-resume-complete',
                          'fumo-multi-resume-notify', 'fumo-no-production',
                          'dmengine-fumo-resume-multi'})
        self.assertIn('291-byte', scenario['description'])
        self.assertIn('Range: bytes=64-', scenario['description'])
        self.assertIn('no install', scenario['description'])

    def test_command_uses_multi_resume_probe(self) -> None:
        command = scenario_command(
            load_scenario('dmengine-fumo-resume-multi'),
            load_profile('research'), kernel_target='compat')
        for text in ('MODE=multiresume',
                     'MOCK_DM_PARTIAL method=GET path=/object.bin '
                     'bytes=64 total=291',
                     'MOCK_DM_RESUME method=GET path=/object.bin '
                     'offset=64 bytes=227 total=291',
                     'MOCK_DM_DD method=GET path=/package.bin bytes=403 '
                     'object=/object.bin object_bytes=291',
                     'fumo-multi-resume-complete'):
            self.assertIn(text, command)

    def test_checks_fail_closed(self) -> None:
        scenario = load_scenario('dmengine-fumo-resume-multi')
        text = ''.join(f'HARNESS_CHECK {check} PASS\n'
                       for check in scenario['checks'])
        self.assertEqual(evaluate(text, 0, scenario)['verdict'], 'PASS')
        self.assertEqual(evaluate(
            text.replace('fumo-multi-resume-request PASS',
                         'fumo-multi-resume-request FAIL'),
            0, scenario)['verdict'], 'FAIL')


if __name__ == '__main__':
    unittest.main()
