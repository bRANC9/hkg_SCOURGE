# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import unittest
from pathlib import Path

from harness.config import load_profile, load_scenario, scenario_command
from harness.results import evaluate

VM = Path(__file__).resolve().parents[1]


class DmengineFumoDownloadUpdateMultiFailureTests(unittest.TestCase):
    def test_scenario_contract(self) -> None:
        scenario = load_scenario('dmengine-fumo-download-update-multi-failure')
        self.assertEqual(scenario['requires'], ['payload'])
        self.assertEqual(scenario['requires_mode'], 'compat')
        self.assertEqual(set(scenario['checks']),
                         {'fumo-ready', 'fumo-http-post', 'fumo-response',
                          'fumo-syncml', 'fumo-trigger', 'fumo-command',
                          'fumo-server-auth', 'fumo-dd', 'fumo-confirm',
                          'fumo-download', 'fumo-multidp', 'fumo-notify',
                          'fumo-dl-complete', 'fumo-state',
                          'fumo-combined-command', 'fumo-update-progressing',
                          'fumo-update-handoff', 'fumo-update-result-sent',
                          'fumo-update-result-accepted',
                          'fumo-update-failure',
                          'fumo-update-result-reported', 'fumo-no-production',
                          'dmengine-fumo-download-update-multi-failure'})
        self.assertIn('result 500', scenario['description'])
        self.assertIn('never enters success state 90',
                      scenario['description'])

    def test_command_selects_failure_result(self) -> None:
        command = scenario_command(
            load_scenario('dmengine-fumo-download-update-multi-failure'),
            load_profile('research'), kernel_target='compat')
        for text in ('export FUMO_MODE=combinedmulti FUMO_RESULT=500',
                     'MOCK_DM_MULTIDP method=GET path=/object.bin '
                     'bytes=291 components=2 updates=3',
                     'fumo-update-failure',
                     'dmengine-fumo-download-update-multi-failure'):
            self.assertIn(text, command)

    def test_checks_fail_closed(self) -> None:
        scenario = load_scenario(
            'dmengine-fumo-download-update-multi-failure')
        text = ''.join(f'HARNESS_CHECK {check} PASS\n'
                       for check in scenario['checks'])
        self.assertEqual(evaluate(text, 0, scenario)['verdict'], 'PASS')
        self.assertEqual(evaluate(
            text.replace('fumo-update-failure PASS',
                         'fumo-update-failure FAIL'),
            0, scenario)['verdict'], 'FAIL')


if __name__ == '__main__':
    unittest.main()
