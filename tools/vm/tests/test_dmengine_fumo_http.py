# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import unittest
from pathlib import Path

from harness.config import load_profile, load_scenario, scenario_command
from harness.results import evaluate

VM = Path(__file__).resolve().parents[1]


class DmengineFumoHttpTests(unittest.TestCase):
    def test_scenario_contract(self) -> None:
        scenario = load_scenario('dmengine-fumo-http')
        self.assertEqual(scenario['requires'], ['payload'])
        self.assertEqual(scenario['requires_mode'], 'compat')
        self.assertEqual(set(scenario['checks']),
                         {'fumo-ready', 'fumo-http-post', 'fumo-syncml',
                          'fumo-response', 'fumo-trigger',
                          'fumo-session-complete', 'fumo-no-production',
                          'dmengine-fumo-http'})
        self.assertIn('no update-success claim', scenario['description'])
        self.assertIn('SCOMO session state 1 with lastError 0',
                      scenario['description'])

    def test_command_uses_loopback_and_session_trigger(self) -> None:
        command = scenario_command(
            load_scenario('dmengine-fumo-http'), load_profile('research'),
            kernel_target='compat')
        for text in ('mock-dm-server', 'mock-dm-shm', 'mock-oem-queue',
                     'ip link set lo up', '127.0.0.1:8002/dm/message',
                     '/tmp/mock-oem 0', 'content_type=application/vnd.syncml.dm+wbxml',
                     'body_prefix=02', 'Triggering DM session',
                     'onSessionStateChange', 'MOCK DM REQUEST',
                     'VDM TRIGGER', 'fumo-no-production'):
            self.assertIn(text, command)

    def test_helper_and_build_wiring(self) -> None:
        source = (VM / 'guest/mock_dm_server.c').read_text()
        self.assertIn('INADDR_LOOPBACK', source)
        self.assertIn('MOCK_DM_REQUEST', source)
        build = (VM / 'build_research_tools.sh').read_text()
        self.assertIn('mock-dm-server', build)
        self.assertIn('payload/tweaks-vm/mock-dm-server', build)

    def test_checks_fail_closed(self) -> None:
        scenario = load_scenario('dmengine-fumo-http')
        text = ''.join(f'HARNESS_CHECK {check} PASS\n'
                       for check in scenario['checks'])
        self.assertEqual(evaluate(text, 0, scenario)['verdict'], 'PASS')
        self.assertEqual(evaluate(
            text.replace('fumo-http-post PASS', 'fumo-http-post FAIL'),
            0, scenario)['verdict'], 'FAIL')


if __name__ == '__main__':
    unittest.main()
