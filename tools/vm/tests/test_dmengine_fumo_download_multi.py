# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import unittest
from pathlib import Path

from harness.config import load_profile, load_scenario, scenario_command
from harness.results import evaluate

VM = Path(__file__).resolve().parents[1]


class DmengineFumoDownloadMultiTests(unittest.TestCase):
    def test_scenario_contract(self) -> None:
        scenario = load_scenario('dmengine-fumo-download-multi')
        self.assertEqual(scenario['requires'], ['payload'])
        self.assertEqual(scenario['requires_mode'], 'compat')
        self.assertEqual(set(scenario['checks']),
                         {'fumo-ready', 'fumo-http-post', 'fumo-response',
                          'fumo-syncml', 'fumo-trigger', 'fumo-command',
                          'fumo-server-auth', 'fumo-dd', 'fumo-confirm',
                          'fumo-download', 'fumo-multidp', 'fumo-notify',
                          'fumo-dl-complete', 'fumo-state',
                          'fumo-no-production',
                          'dmengine-fumo-download-multi'})
        self.assertIn('two components', scenario['description'])
        self.assertIn('no install is claimed', scenario['description'])

    def test_command_selects_multidp_mode(self) -> None:
        command = scenario_command(
            load_scenario('dmengine-fumo-download-multi'),
            load_profile('research'), kernel_target='compat')
        for text in ('export FUMO_MODE=multidp',
                     'multidp|combinedmulti) EXPECTED_OBJECT=291',
                     'MOCK_DM_MULTIDP method=GET path=/object.bin '
                     'bytes=291 components=2 updates=3',
                     'fumo-multidp'):
            self.assertIn(text, command)

    def test_checks_fail_closed(self) -> None:
        scenario = load_scenario('dmengine-fumo-download-multi')
        text = ''.join(f'HARNESS_CHECK {check} PASS\n'
                       for check in scenario['checks'])
        self.assertEqual(evaluate(text, 0, scenario)['verdict'], 'PASS')
        self.assertEqual(evaluate(
            text.replace('fumo-multidp PASS', 'fumo-multidp FAIL'),
            0, scenario)['verdict'], 'FAIL')


if __name__ == '__main__':
    unittest.main()
