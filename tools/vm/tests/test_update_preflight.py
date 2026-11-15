# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import unittest

from harness.config import load_profile, load_scenario, scenario_command
from harness.results import evaluate


class UpdatePreflightTests(unittest.TestCase):
    def test_scenario_contract(self) -> None:
        scenario = load_scenario('update-preflight')
        self.assertEqual(scenario['requires'], ['usb'])
        self.assertNotIn('requires_mode', scenario)
        self.assertEqual(set(scenario['checks']),
                         {'preflight-media', 'preflight-ver',
                          'preflight-manifest', 'preflight-payload',
                          'preflight-no-exec'})
        self.assertIn('read-only', scenario['description'])
        self.assertIn('never launches AppUpgrade', scenario['description'])

    def test_command_is_read_only_and_validates_media(self) -> None:
        command = scenario_command(
            load_scenario('update-preflight'), load_profile('research'))
        for text in ('MOUNT_MODE=ro', 'mount -o "$MOUNT_MODE"',
                     'IONIQ5_EU.ver', 'MANIFEST.sha256',
                     '.lge.upgrade.xml', 'sha256sum -c',
                     'HU/images/mango-rootfs.tar.gz', '1f8b',
                     'AppUpgrade', 'preflight-media',
                     'preflight-ver', 'preflight-manifest',
                     'preflight-payload', 'preflight-no-exec'):
            self.assertIn(text, command)
        self.assertNotIn('mount -o rw', command)

    def test_checks(self) -> None:
        scenario = load_scenario('update-preflight')
        text = ''.join(f'HARNESS_CHECK {check} PASS\n'
                       for check in scenario['checks'])
        self.assertEqual(evaluate(text, 0, scenario)['verdict'], 'PASS')
        self.assertEqual(
            evaluate(text.replace('preflight-payload PASS',
                                  'preflight-payload FAIL'),
                     0, scenario)['verdict'], 'FAIL')


class UpdatePreflightNegativeTests(unittest.TestCase):
    def test_scenario_contract(self) -> None:
        scenario = load_scenario('update-preflight-negative')
        self.assertEqual(scenario['requires'], ['usb'])
        self.assertEqual(set(scenario['checks']),
                         {'preflight-media', 'preflight-ver',
                          'preflight-manifest', 'preflight-payload',
                          'negative-size-detected',
                          'negative-path-detected',
                          'negative-manifest-detected',
                          'negative-restored', 'preflight-no-exec'})
        self.assertIn('disposable USB overlay', scenario['description'])
        self.assertIn('never launches AppUpgrade', scenario['description'])

    def test_command_sets_negative_mode(self) -> None:
        command = scenario_command(
            load_scenario('update-preflight-negative'),
            load_profile('research'))
        self.assertTrue(command.startswith(
            'export UPDATE_PREFLIGHT_NEGATIVE=1\n'))
        for text in ('mount -o "$MOUNT_MODE"', 'negative-size.ver',
                     'negative-path.ver', 'negative-manifest.sha256',
                     'negative-restored'):
            self.assertIn(text, command)
        positive = scenario_command(
            load_scenario('update-preflight'), load_profile('research'))
        self.assertFalse(positive.startswith(
            'export UPDATE_PREFLIGHT_NEGATIVE=1'))

    def test_checks(self) -> None:
        scenario = load_scenario('update-preflight-negative')
        text = ''.join(f'HARNESS_CHECK {check} PASS\n'
                       for check in scenario['checks'])
        self.assertEqual(evaluate(text, 0, scenario)['verdict'], 'PASS')
        self.assertEqual(
            evaluate(text.replace('negative-restored PASS',
                                  'negative-restored FAIL'),
                     0, scenario)['verdict'], 'FAIL')

if __name__ == '__main__':
    unittest.main()
