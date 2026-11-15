# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import unittest
from pathlib import Path

from harness.config import load_profile, load_scenario, scenario_command
from harness.results import evaluate

VM = Path(__file__).resolve().parents[1]


class DmengineFumoNotifyFailureTests(unittest.TestCase):
    def test_scenario_contract(self) -> None:
        scenario = load_scenario('dmengine-fumo-notify-failure')
        self.assertEqual(scenario['requires'], ['payload'])
        self.assertEqual(scenario['requires_mode'], 'compat')
        self.assertEqual(set(scenario['checks']),
                         {'fumo-ready', 'fumo-http-post', 'fumo-response',
                          'fumo-syncml', 'fumo-trigger', 'fumo-command',
                          'fumo-server-auth', 'fumo-dd', 'fumo-confirm',
                          'fumo-download', 'fumo-notify-fail',
                          'fumo-notify-failure-error',
                          'fumo-notify-no-completion', 'fumo-no-production',
                          'dmengine-fumo-notify-failure'})
        self.assertIn('HTTP 500', scenario['description'])
        self.assertIn('No install or update-success claim', scenario['description'])

    def test_command_selects_notifyfail_mode(self) -> None:
        command = scenario_command(
            load_scenario('dmengine-fumo-notify-failure'),
            load_profile('research'), kernel_target='compat')
        for text in ('export FUMO_MODE=notifyfail',
                     'MOCK_DM_NOTIFY_FAIL path=/notify bytes=11 body=900 Success',
                     'DL session aborted, 0x6308',
                     'fumo result : 503, last error : 25352',
                     'fumo-notify-no-completion'):
            self.assertIn(text, command)

    def test_checks_fail_closed(self) -> None:
        scenario = load_scenario('dmengine-fumo-notify-failure')
        text = ''.join(f'HARNESS_CHECK {check} PASS\n'
                       for check in scenario['checks'])
        self.assertEqual(evaluate(text, 0, scenario)['verdict'], 'PASS')
        self.assertEqual(evaluate(
            text.replace('fumo-notify-failure-error PASS',
                         'fumo-notify-failure-error FAIL'),
            0, scenario)['verdict'], 'FAIL')


if __name__ == '__main__':
    unittest.main()
