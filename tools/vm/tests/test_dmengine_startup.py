# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import unittest
from pathlib import Path

from harness.config import load_profile, load_scenario, scenario_command
from harness.results import evaluate

VM = Path(__file__).resolve().parents[1]


class DmengineStartupTests(unittest.TestCase):
    def test_scenario_contract(self) -> None:
        scenario = load_scenario('dmengine-startup')
        self.assertEqual(scenario['requires'], ['payload'])
        self.assertEqual(scenario['requires_mode'], 'compat')
        self.assertEqual(set(scenario['checks']),
                         {'dmengine-ready', 'dmengine-tree',
                          'dmengine-device-info', 'dmengine-scomo-plugin',
                          'dmengine-no-production', 'dmengine-startup'})
        self.assertIn('does not complete a DM session',
                      scenario['description'])

    def test_command_uses_lab_device_info_and_rewrites_tree(self) -> None:
        command = scenario_command(
            load_scenario('dmengine-startup'), load_profile('research'),
            kernel_target='compat')
        for text in ('mock-dm-shm', 'MOCK_DM_SHM_READY',
                     'http://127.0.0.1:8002/dm/message',
                     'is_MOFClient_used=false',
                     'FUMO Plugin Init',
                     'SCOMO Plugin Init',
                     'dmengine-no-production'):
            self.assertIn(text, command)
        source = (VM / 'guest/mock_dm_shm.c').read_text()
        self.assertIn('qipc_sharedmemory_DMCDEVINFO', source)
        self.assertIn('VIN:LABVIN0000000000_AVN', source)

    def test_checks_fail_closed(self) -> None:
        scenario = load_scenario('dmengine-startup')
        text = ''.join(f'HARNESS_CHECK {check} PASS\n'
                       for check in scenario['checks'])
        self.assertEqual(evaluate(text, 0, scenario)['verdict'], 'PASS')
        self.assertEqual(evaluate(
            text.replace('dmengine-device-info PASS',
                         'dmengine-device-info FAIL'),
            0, scenario)['verdict'], 'FAIL')


if __name__ == '__main__':
    unittest.main()
