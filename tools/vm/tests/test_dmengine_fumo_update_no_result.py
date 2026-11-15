# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import unittest
from pathlib import Path

from harness.config import load_profile, load_scenario, scenario_command
from harness.results import evaluate

VM = Path(__file__).resolve().parents[1]


class DmengineFumoUpdateNoResultTests(unittest.TestCase):
    def test_scenario_contract(self) -> None:
        scenario = load_scenario('dmengine-fumo-update-no-result')
        self.assertEqual(scenario['requires'], ['payload'])
        self.assertEqual(scenario['requires_mode'], 'compat')
        self.assertEqual(set(scenario['checks']),
                         {'fumo-ready', 'fumo-http-post', 'fumo-response',
                          'fumo-syncml', 'fumo-trigger', 'fumo-command',
                          'fumo-server-auth', 'fumo-dd', 'fumo-confirm',
                          'fumo-download', 'fumo-notify', 'fumo-dl-complete',
                          'fumo-state',
                          'fumo-combined-command', 'fumo-update-progressing',
                          'fumo-update-handoff', 'fumo-update-no-result',
                          'fumo-no-production',
                          'dmengine-fumo-update-no-result'})
        self.assertIn('sends no update result', scenario['description'])
        self.assertIn('no physical install', scenario['description'])

    def test_command_skips_result(self) -> None:
        command = scenario_command(
            load_scenario('dmengine-fumo-update-no-result'),
            load_profile('research'), kernel_target='compat')
        for text in ('export FUMO_MODE=combined FUMO_SKIP_RESULT=1',
                     'SKIP_RESULT=${FUMO_SKIP_RESULT:-0}',
                     'fumo-update-no-result',
                     'VDMC_FUMO_updateReport'):
            self.assertIn(text, command)

    def test_checks_fail_closed(self) -> None:
        scenario = load_scenario('dmengine-fumo-update-no-result')
        text = ''.join(f'HARNESS_CHECK {check} PASS\n'
                       for check in scenario['checks'])
        self.assertEqual(evaluate(text, 0, scenario)['verdict'], 'PASS')
        self.assertEqual(evaluate(
            text.replace('fumo-update-no-result PASS',
                         'fumo-update-no-result FAIL'),
            0, scenario)['verdict'], 'FAIL')


if __name__ == '__main__':
    unittest.main()
