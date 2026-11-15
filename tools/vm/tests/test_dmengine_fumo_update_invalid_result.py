# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import unittest
from pathlib import Path

from harness.config import load_profile, load_scenario, scenario_command
from harness.results import evaluate

VM = Path(__file__).resolve().parents[1]


class DmengineFumoUpdateInvalidResultTests(unittest.TestCase):
    def test_scenario_contract(self) -> None:
        scenario = load_scenario('dmengine-fumo-update-invalid-result')
        self.assertEqual(scenario['requires'], ['payload'])
        self.assertEqual(scenario['requires_mode'], 'compat')
        self.assertEqual(set(scenario['checks']),
                         {'fumo-ready', 'fumo-http-post', 'fumo-response',
                          'fumo-syncml', 'fumo-trigger', 'fumo-command',
                          'fumo-server-auth', 'fumo-dd', 'fumo-confirm',
                          'fumo-download', 'fumo-dl-complete', 'fumo-state',
                          'fumo-combined-command', 'fumo-update-progressing',
                          'fumo-update-handoff', 'fumo-update-result-sent',
                          'fumo-update-result-accepted',
                          'fumo-update-invalid-remap', 'fumo-update-failure',
                          'fumo-update-result-reported', 'fumo-no-production',
                          'dmengine-fumo-update-invalid-result'})
        self.assertIn('result 0', scenario['description'])
        self.assertIn('no physical install', scenario['description'])

    def test_command_selects_invalid_result(self) -> None:
        command = scenario_command(
            load_scenario('dmengine-fumo-update-invalid-result'),
            load_profile('research'), kernel_target='compat')
        for text in ('export FUMO_MODE=combined FUMO_RESULT=0 FUMO_INVALID_RESULT=1',
                     'validateResultCode - invalid result code',
                     'using UNDEFINED(409)',
                     'fumo-update-invalid-remap'):
            self.assertIn(text, command)

    def test_checks_fail_closed(self) -> None:
        scenario = load_scenario('dmengine-fumo-update-invalid-result')
        text = ''.join(f'HARNESS_CHECK {check} PASS\n'
                       for check in scenario['checks'])
        self.assertEqual(evaluate(text, 0, scenario)['verdict'], 'PASS')
        self.assertEqual(evaluate(
            text.replace('fumo-update-invalid-remap PASS',
                         'fumo-update-invalid-remap FAIL'),
            0, scenario)['verdict'], 'FAIL')


if __name__ == '__main__':
    unittest.main()
