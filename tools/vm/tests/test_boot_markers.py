# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from harness.config import load_scenario
from harness.results import evaluate_boot_only
from harness.scenarios import validate_scenario

ESINIT_TEXT = (
    'FDE SUPPORT: No * \r\n'
    '[FDE] ***** use fstab for non FDE\r\n'
)


def valid_scenario(**overrides):
    data = {
        'name': 'sample',
        'description': 'sample scenario',
        'checks': ['one', 'two'],
    }
    data.update(overrides)
    return data


class VendorEsinitTests(unittest.TestCase):
    def test_scenario_contract(self) -> None:
        scenario = load_scenario('vendor-esinit')
        self.assertEqual(scenario['init_override'], '/lib/systemd/esinit')
        self.assertEqual(scenario['boot_marker'],
                         'use fstab for non FDE')
        self.assertEqual(scenario['requires_mode'], 'vendor')
        self.assertEqual(set(scenario['checks']),
                         set(scenario['boot_markers'].values()))
        self.assertIn('esinit', scenario['description'])
        self.assertIn('not stock equivalence', scenario['description'])

    def test_all_markers_pass(self) -> None:
        scenario = load_scenario('vendor-esinit')
        result = evaluate_boot_only(scenario, ESINIT_TEXT)
        self.assertEqual(result['verdict'], 'PASS')
        self.assertEqual(result['rc'], 0)
        self.assertTrue(result['marker_present'])
        self.assertEqual(set(result['checks'].values()), {'PASS'})
        self.assertTrue(all(result['marker_evidence'].values()))
        self.assertEqual(result['failures'], [])

    def test_missing_esinit_marker_fails_closed(self) -> None:
        scenario = load_scenario('vendor-esinit')
        text = ESINIT_TEXT.replace('FDE SUPPORT: No * \r\n', '')
        result = evaluate_boot_only(scenario, text)
        self.assertEqual(result['verdict'], 'FAIL')
        self.assertEqual(result['checks']['esinit-no-fde'], 'FAIL')
        self.assertTrue(any('FDE SUPPORT: No' in failure
                            for failure in result['failures']))

    def test_missing_final_marker_fails_closed(self) -> None:
        scenario = load_scenario('vendor-esinit')
        text = ESINIT_TEXT.replace('use fstab for non FDE', '')
        result = evaluate_boot_only(scenario, text)
        self.assertEqual(result['verdict'], 'FAIL')
        self.assertEqual(result['checks']['esinit-fstab-nonfde'], 'FAIL')
        self.assertFalse(result['marker_present'])

    def test_kernel_panic_fails(self) -> None:
        scenario = load_scenario('vendor-esinit')
        result = evaluate_boot_only(
            scenario, ESINIT_TEXT + 'Kernel panic - not syncing\r\n')
        self.assertEqual(result['verdict'], 'FAIL')
        self.assertIn('kernel panic', result['failures'])

    def test_unmapped_check_fails_closed(self) -> None:
        scenario = dict(load_scenario('vendor-esinit'))
        scenario['checks'] = list(scenario['checks']) + ['unmapped']
        result = evaluate_boot_only(scenario, ESINIT_TEXT)
        self.assertEqual(result['verdict'], 'FAIL')
        self.assertEqual(result['checks']['unmapped'], 'FAIL')
        self.assertTrue(any('unmapped boot checks' in failure
                            for failure in result['failures']))

    def test_legacy_boot_marker_still_gates_checks(self) -> None:
        scenario = {'boot_marker': 'READY', 'checks': ['a', 'b']}
        self.assertEqual(evaluate_boot_only(scenario, 'READY\r\n')['checks'],
                         {'a': 'PASS', 'b': 'PASS'})
        result = evaluate_boot_only(scenario, 'not yet\r\n')
        self.assertEqual(result['verdict'], 'FAIL')
        self.assertEqual(result['checks'], {'a': 'FAIL', 'b': 'FAIL'})
        self.assertEqual(result['marker_evidence'], {})


class BootMarkerContractTests(unittest.TestCase):
    def test_valid_contract(self) -> None:
        with tempfile.TemporaryDirectory(prefix='vm-bootmarkers-') as tmp:
            data = valid_scenario(
                init_override='/lib/systemd/esinit', boot_marker='READY',
                boot_markers={'READY': 'one', 'SPI': 'two'})
            self.assertEqual(validate_scenario('sample', data, Path(tmp)), [])

    def test_unknown_check_and_missing_init_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix='vm-bootmarkers-') as tmp:
            data = valid_scenario(boot_marker='READY',
                                  boot_markers={'READY': 'other'})
            joined = '\n'.join(validate_scenario('sample', data, Path(tmp)))
            self.assertIn('unknown checks', joined)
            self.assertIn('requires init_override', joined)

    def test_empty_or_non_string_markers_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix='vm-bootmarkers-') as tmp:
            for markers in ({}, {'': 'one'}, {'READY': ''},
                            {'READY': 1}, ['READY']):
                data = valid_scenario(init_override='/init', boot_marker='READY',
                                      boot_markers=markers)
                joined = '\n'.join(validate_scenario('sample', data, Path(tmp)))
                self.assertIn('boot_markers', joined, markers)


if __name__ == '__main__':
    unittest.main()
