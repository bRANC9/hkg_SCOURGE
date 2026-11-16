# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import unittest
from pathlib import Path

from harness.config import load_profile, load_scenario, scenario_command
from harness.results import evaluate

VM = Path(__file__).resolve().parents[1]


class DmengineFumoResumeNoEtagTests(unittest.TestCase):
    def test_scenario_contract(self) -> None:
        scenario = load_scenario('dmengine-fumo-resume-noetag')
        self.assertEqual(scenario['requires'], ['payload'])
        self.assertEqual(scenario['requires_mode'], 'compat')
        self.assertEqual(set(scenario['checks']),
                         {'fumo-noetag-ready', 'fumo-noetag-partial',
                          'fumo-noetag-recorded', 'fumo-noetag-fallback',
                          'fumo-noetag-complete', 'fumo-noetag-notify',
                          'fumo-no-production',
                          'dmengine-fumo-resume-noetag'})
        self.assertIn('omits the ETag', scenario['description'])
        self.assertIn('No install', scenario['description'])

    def test_command_uses_noetag_probe(self) -> None:
        command = scenario_command(
            load_scenario('dmengine-fumo-resume-noetag'),
            load_profile('research'), kernel_target='compat')
        for text in ('MODE=noetag', 'MOCK_DM_NOETAG_PARTIAL',
                     'MOCK_DM_NOETAG_FULL method=GET path=/object.bin '
                     'bytes=184 range=none',
                     'DL resume: got offset=64, eTag=<\\[null\\]>',
                     'fumo-noetag-fallback'):
            self.assertIn(text, command)

    def test_checks_fail_closed(self) -> None:
        scenario = load_scenario('dmengine-fumo-resume-noetag')
        text = ''.join(f'HARNESS_CHECK {check} PASS\n'
                       for check in scenario['checks'])
        self.assertEqual(evaluate(text, 0, scenario)['verdict'], 'PASS')
        self.assertEqual(evaluate(
            text.replace('fumo-noetag-fallback PASS',
                         'fumo-noetag-fallback FAIL'),
            0, scenario)['verdict'], 'FAIL')


if __name__ == '__main__':
    unittest.main()
