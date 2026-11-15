# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import unittest
from pathlib import Path

from harness.config import load_profile, load_scenario, scenario_command
from harness.results import evaluate

VM = Path(__file__).resolve().parents[1]


class DmengineFumoDownloadUpdateTests(unittest.TestCase):
    def test_scenario_contract(self) -> None:
        scenario = load_scenario('dmengine-fumo-download-update')
        self.assertEqual(scenario['requires'], ['payload'])
        self.assertEqual(scenario['requires_mode'], 'compat')
        self.assertEqual(set(scenario['checks']),
                         {'fumo-ready', 'fumo-http-post', 'fumo-response',
                          'fumo-syncml', 'fumo-trigger', 'fumo-command',
                          'fumo-server-auth', 'fumo-dd', 'fumo-confirm',
                          'fumo-download', 'fumo-dl-complete', 'fumo-state',
                          'fumo-combined-command', 'fumo-update-progressing',
                          'fumo-update-handoff', 'fumo-update-result-sent',
                          'fumo-update-result-accepted',
                          'fumo-update-result-reported', 'fumo-no-production',
                          'dmengine-fumo-download-update'})
        self.assertIn('DownloadAndUpdate', scenario['description'])
        self.assertIn('update result 200', scenario['description'])
        self.assertIn('not a physical install', scenario['description'])

    def test_command_selects_combined_mode_and_fills_url(self) -> None:
        command = scenario_command(
            load_scenario('dmengine-fumo-download-update'),
            load_profile('research'), kernel_target='compat')
        for text in ('export FUMO_MODE=combined',
                     'DownloadAndUpdate',
                     'FUMO DownloadAndUpdate executed in this session',
                     'E_VDM_FUMO_STATE_update_progressing',
                     'event to OEM handler = [77]',
                     'strData = [/update//lab-fumo]',
                     'MOCK_OEM_SENT code=20 field1=$RESULT_CODE',
                     'VDMC_FUMO_updateReport] result = [$RESULT_CODE]',
                     'fumo-update-result-sent',
                     'fumo-update-result-reported'):
            self.assertIn(text, command)

    def test_checks_fail_closed(self) -> None:
        scenario = load_scenario('dmengine-fumo-download-update')
        text = ''.join(f'HARNESS_CHECK {check} PASS\n'
                       for check in scenario['checks'])
        self.assertEqual(evaluate(text, 0, scenario)['verdict'], 'PASS')
        self.assertEqual(evaluate(
            text.replace('fumo-update-handoff PASS',
                         'fumo-update-handoff FAIL'),
            0, scenario)['verdict'], 'FAIL')


if __name__ == '__main__':
    unittest.main()
