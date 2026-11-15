# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import unittest
from pathlib import Path

from harness.config import load_profile, load_scenario, scenario_command
from harness.results import evaluate

VM = Path(__file__).resolve().parents[1]


class DmengineFumoUpdateTests(unittest.TestCase):
    def test_scenario_contract(self) -> None:
        scenario = load_scenario('dmengine-fumo-update')
        self.assertEqual(scenario['requires'], ['payload'])
        self.assertEqual(scenario['requires_mode'], 'compat')
        self.assertEqual(set(scenario['checks']),
                         {'fumo-ready', 'fumo-http-post', 'fumo-response',
                          'fumo-syncml', 'fumo-trigger', 'fumo-command',
                          'fumo-server-auth', 'fumo-dd', 'fumo-confirm',
                          'fumo-download', 'fumo-dl-complete', 'fumo-state',
                          'fumo-update-request', 'fumo-update-rejected',
                          'fumo-update-no-completion', 'fumo-no-production',
                          'dmengine-fumo-update'})
        self.assertIn('install request', scenario['description'])
        self.assertIn('no install is claimed', scenario['description'])

    def test_command_selects_update_mode(self) -> None:
        command = scenario_command(
            load_scenario('dmengine-fumo-update'), load_profile('research'),
            kernel_target='compat')
        for text in ('export FUMO_MODE=update',
                     '/tmp/mock-dm 8002 /tmp/mock-dm.log "$MODE"',
                     'MOCK_DM_UPDATE_EXEC',
                     'targetURI=./ManagedObjects/FUMO/Update',
                     'FUMO_Plugin_onSessionStateChange(DM): Error=\\[24593\\]',
                     'DM session aborted, 0x6011',
                     'fumo-update-request', 'fumo-update-rejected'):
            self.assertIn(text, command)

    def test_checks_fail_closed(self) -> None:
        scenario = load_scenario('dmengine-fumo-update')
        text = ''.join(f'HARNESS_CHECK {check} PASS\n'
                       for check in scenario['checks'])
        self.assertEqual(evaluate(text, 0, scenario)['verdict'], 'PASS')
        self.assertEqual(evaluate(
            text.replace('fumo-update-request PASS',
                         'fumo-update-request FAIL'),
            0, scenario)['verdict'], 'FAIL')


if __name__ == '__main__':
    unittest.main()
