# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import unittest
from pathlib import Path

from harness.config import load_profile, load_scenario, scenario_command
from harness.results import evaluate

VM = Path(__file__).resolve().parents[1]


class DmclientFotaEventsTests(unittest.TestCase):
    def test_scenario_contract(self) -> None:
        scenario = load_scenario('dmclient-fota-events')
        self.assertEqual(scenario['requires'], ['usb', 'tools'])
        self.assertEqual(set(scenario['checks']),
                         {'dmclient-usb', 'dmclient-standby',
                          'updater-exec', 'updater-alive',
                          'fota-command-28', 'fota-command-29',
                          'fota-command-2a', 'fota-callback'})
        self.assertIn('Not an HTTP update', scenario['description'])
        self.assertIn('dmengine', scenario['description'])

    def test_tools_scripts_contain_callback(self) -> None:
        scenarios = (VM / 'toolsroot/scenarios.sh').read_text()
        phase4 = (VM / 'toolsroot/phase4.sh').read_text()
        self.assertIn('dmclient-fota-events', scenarios)
        self.assertIn('HARNESS_FOTA_EVENTS=1', scenarios)
        self.assertIn('sendEventToDMClientFromAppUpgrade', phase4)
        self.assertIn('/fotaInterface', phase4)
        self.assertIn('fota-command-$code', phase4)
        command = scenario_command(load_scenario('dmclient-fota-events'),
                                   load_profile('research'))
        self.assertIn('dmclient-fota-events', command)

    def test_checks_fail_closed(self) -> None:
        scenario = load_scenario('dmclient-fota-events')
        text = ''.join(f'HARNESS_CHECK {check} PASS\n'
                       for check in scenario['checks'])
        self.assertEqual(evaluate(text, 0, scenario)['verdict'], 'PASS')
        self.assertEqual(evaluate(
            text.replace('fota-command-29 PASS', 'fota-command-29 FAIL'),
            0, scenario)['verdict'], 'FAIL')


if __name__ == '__main__':
    unittest.main()
