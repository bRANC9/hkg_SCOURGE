# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import unittest
from pathlib import Path

from harness.config import load_profile, load_scenario, scenario_command
from harness.results import evaluate

VM = Path(__file__).resolve().parents[1]


class DmengineFumoDownloadMissingTests(unittest.TestCase):
    def test_scenario_contract(self) -> None:
        scenario = load_scenario('dmengine-fumo-download-missing')
        self.assertEqual(scenario['requires'], ['payload'])
        self.assertEqual(scenario['requires_mode'], 'compat')
        self.assertEqual(set(scenario['checks']),
                         {'fumo-ready', 'fumo-http-post', 'fumo-response',
                          'fumo-syncml', 'fumo-trigger', 'fumo-command',
                          'fumo-server-auth', 'fumo-dd', 'fumo-confirm',
                          'fumo-missing-request', 'fumo-missing-error',
                          'fumo-missing-no-download', 'fumo-no-production',
                          'dmengine-fumo-download-missing'})
        self.assertIn('returns 404', scenario['description'])
        self.assertIn('No install or update-success claim', scenario['description'])

    def test_command_selects_missing_mode(self) -> None:
        command = scenario_command(
            load_scenario('dmengine-fumo-download-missing'),
            load_profile('research'), kernel_target='compat')
        for text in ('export FUMO_MODE=missing',
                     '/tmp/mock-dm 8002 /tmp/mock-dm.log "$MODE"',
                     'MOCK_DM_MISSING method=GET path=/object.bin',
                     'DL session aborted, 0x6306',
                     'fumo result : 407, last error : 25350',
                     'fumo-missing-no-download'):
            self.assertIn(text, command)

    def test_checks_fail_closed(self) -> None:
        scenario = load_scenario('dmengine-fumo-download-missing')
        text = ''.join(f'HARNESS_CHECK {check} PASS\n'
                       for check in scenario['checks'])
        self.assertEqual(evaluate(text, 0, scenario)['verdict'], 'PASS')
        self.assertEqual(evaluate(
            text.replace('fumo-missing-error PASS', 'fumo-missing-error FAIL'),
            0, scenario)['verdict'], 'FAIL')


if __name__ == '__main__':
    unittest.main()
