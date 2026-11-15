# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import unittest
from pathlib import Path

from harness.config import load_profile, load_scenario, scenario_command
from harness.results import evaluate

VM = Path(__file__).resolve().parents[1]


class DmengineOemQueueTests(unittest.TestCase):
    def test_scenario_contract(self) -> None:
        scenario = load_scenario('dmengine-oem-queue')
        self.assertEqual(scenario['requires'], ['payload'])
        self.assertEqual(scenario['requires_mode'], 'compat')
        self.assertEqual(set(scenario['checks']),
                         {'oem-command-sent', 'oem-ready', 'oem-mof-bus',
                          'oem-event-66',
                          'oem-no-production', 'dmengine-oem-queue'})
        self.assertIn('does not implement AppDMClient',
                      scenario['description'])

    def test_command_uses_queue_and_trigger(self) -> None:
        command = scenario_command(
            load_scenario('dmengine-oem-queue'), load_profile('research'),
            kernel_target='compat')
        for text in ('mock-oem-queue', 'mock-dm-shm',
                     '/tmp/mock-oem-queue 1', 'recvDM_01',
                     'MOCK_OEM_SENT code=1', 'MOCK_OEM_EVENT code=66',
                     'oem-mof-bus',
                     'oem-no-production'):
            self.assertIn(text, command)

    def test_checks(self) -> None:
        scenario = load_scenario('dmengine-oem-queue')
        text = ''.join(f'HARNESS_CHECK {check} PASS\n'
                       for check in scenario['checks'])
        self.assertEqual(evaluate(text, 0, scenario)['verdict'], 'PASS')
        self.assertEqual(evaluate(
            text.replace('oem-event-66 PASS', 'oem-event-66 FAIL'),
            0, scenario)['verdict'], 'FAIL')


if __name__ == '__main__':
    unittest.main()
