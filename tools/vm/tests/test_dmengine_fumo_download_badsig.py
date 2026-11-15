# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import unittest
from pathlib import Path

from harness.config import load_profile, load_scenario, scenario_command
from harness.results import evaluate

VM = Path(__file__).resolve().parents[1]


class DmengineFumoDownloadBadSigTests(unittest.TestCase):
    def test_scenario_contract(self) -> None:
        scenario = load_scenario('dmengine-fumo-download-badsig')
        self.assertEqual(scenario['requires'], ['payload'])
        self.assertEqual(scenario['requires_mode'], 'compat')
        self.assertEqual(set(scenario['checks']),
                         {'fumo-ready', 'fumo-http-post', 'fumo-response',
                          'fumo-syncml', 'fumo-trigger', 'fumo-command',
                          'fumo-server-auth', 'fumo-dd', 'fumo-confirm',
                          'fumo-download', 'fumo-notify', 'fumo-badsig',
                          'fumo-dl-complete', 'fumo-state', 'fumo-no-production',
                          'dmengine-fumo-download-badsig'})
        self.assertIn('signature trailer', scenario['description'])
        self.assertIn('does not validate', scenario['description'])

    def test_command_selects_badsig_mode(self) -> None:
        command = scenario_command(
            load_scenario('dmengine-fumo-download-badsig'),
            load_profile('research'), kernel_target='compat')
        for text in ('export FUMO_MODE=badsig',
                     'MOCK_DM_BADSIG path=/object.bin bytes=184',
                     'fumo-badsig'):
            self.assertIn(text, command)

    def test_checks_fail_closed(self) -> None:
        scenario = load_scenario('dmengine-fumo-download-badsig')
        text = ''.join(f'HARNESS_CHECK {check} PASS\n'
                       for check in scenario['checks'])
        self.assertEqual(evaluate(text, 0, scenario)['verdict'], 'PASS')
        self.assertEqual(evaluate(
            text.replace('fumo-badsig PASS', 'fumo-badsig FAIL'),
            0, scenario)['verdict'], 'FAIL')


if __name__ == '__main__':
    unittest.main()
