# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import unittest

from harness.config import load_profile, load_scenario, scenario_command
from harness.results import evaluate


class DeviceModelTests(unittest.TestCase):
    def test_scenario_contract(self) -> None:
        scenario = load_scenario('device-model')
        self.assertEqual(scenario['requires'], [])
        self.assertEqual(scenario['requires_mode'], 'vendor')
        self.assertEqual(set(scenario['checks']),
                         {'device-host', 'device-sdhci', 'device-lpc-sata',
                          'device-rootfs', 'device-no-virtio', 'device-model'})
        self.assertIn('not the captured BXT hardware',
                      scenario['description'])

    def test_command_asserts_qemu_substrate(self) -> None:
        command = scenario_command(
            load_scenario('device-model'), load_profile('research'))
        for text in ('8086:29c0', '1b36:0007', '8086:2918',
                     '8086:2922', 'mmcblk0', '1af4:', 'device-model'):
            self.assertIn(text, command)

    def test_checks(self) -> None:
        scenario = load_scenario('device-model')
        text = ''.join(f'HARNESS_CHECK {check} PASS\n'
                       for check in scenario['checks'])
        self.assertEqual(evaluate(text, 0, scenario)['verdict'], 'PASS')
        self.assertEqual(evaluate(
            text.replace('device-no-virtio PASS', 'device-no-virtio FAIL'),
            0, scenario)['verdict'], 'FAIL')


if __name__ == '__main__':
    unittest.main()
