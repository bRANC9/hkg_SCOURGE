# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from harness.update_state import check_update_state
from tests.test_dm_tree import tree_xml

VM = Path(__file__).resolve().parents[1]
VERSION = '5W.XXX.S5W_L.001.001.260128'
SCHEDULED = 'NE_E.EUR.S5W_L.001.001.260128'


def make_state(root: Path) -> None:
    (root / 'fota/dme').mkdir(parents=True)
    (root / 'usb/bin').mkdir(parents=True)
    (root / 'updateVersionInfo').write_text(VERSION)
    (root / 'FumoUsbUpdateTimeVersion').write_text(
        f'20261107125619^{SCHEDULED}^{SCHEDULED}')
    (root / 'scheduledUpdateVersion').write_text(SCHEDULED)
    (root / 'scheduledUpdateDate').write_text('2026.11.08')
    (root / 'fota/install_param.txt').write_text(
        'ui:user-interaction/targetVersion:NE_E.XXX.S5W_L.001.001.250604/'
        'nextCountFumo:1/nextCountScomo:-1/installTime:99999999/'
        'execCommand:0000')
    (root / 'fota/dm_state.txt').write_text('0')
    (root / 'usb/bin/.lge.upgrade.xml').write_bytes(b'\x0f\x09\x21\x8f' + b'\0' * 32)
    (root / 'package.tar.sign').write_text('SIGN_VALUE')


class UpdateStateTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix='vm-update-state-')
        self.root = Path(self._tmp.name) / 'update'
        self.root.mkdir()
        make_state(self.root)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_valid_state(self) -> None:
        report = check_update_state(self.root)
        self.assertTrue(report['ok'], report['problems'])
        self.assertEqual(report['version'], VERSION)
        self.assertEqual(report['fumo_time']['timestamp'], '20261107125619')
        self.assertEqual(report['fumo_time']['current'], SCHEDULED)
        self.assertEqual(report['install']['nextCountFumo'], 1)
        self.assertEqual(report['install']['nextCountScomo'], -1)
        self.assertEqual(report['dm_state'], '0')
        self.assertIsNone(report['fumo_state'])
        self.assertEqual(report['package_blob'], 'present')
        self.assertEqual(report['scheduled_date'], '2026.11.08')

    def test_missing_required_file(self) -> None:
        (self.root / 'fota/dm_state.txt').unlink()
        report = check_update_state(self.root)
        self.assertFalse(report['ok'])
        self.assertIn('fota/dm_state.txt is missing',
                      '\n'.join(report['problems']))

    def test_bad_version_and_timestamp(self) -> None:
        (self.root / 'updateVersionInfo').write_text('not-a-version')
        (self.root / 'FumoUsbUpdateTimeVersion').write_text(
            f'bad^{SCHEDULED}^{SCHEDULED}')
        report = check_update_state(self.root)
        joined = '\n'.join(report['problems'])
        self.assertFalse(report['ok'])
        self.assertIn('updateVersionInfo has an unexpected format', joined)
        self.assertIn('timestamp is not 14 digits', joined)

    def test_install_param_missing_key_and_dm_state(self) -> None:
        (self.root / 'fota/install_param.txt').write_text(
            'ui:user-interaction/targetVersion:'
            'NE_E.XXX.S5W_L.001.001.250604/nextCountFumo:1')
        (self.root / 'fota/dm_state.txt').write_text('2')
        report = check_update_state(self.root)
        joined = '\n'.join(report['problems'])
        self.assertFalse(report['ok'])
        self.assertIn('install_param is missing nextCountScomo', joined)
        self.assertIn('dm_state.txt must be 0 or 1', joined)

    def test_scheduled_version_mismatch(self) -> None:
        (self.root / 'scheduledUpdateVersion').write_text(
            'NE_E.EUR.S5W_L.001.001.250604')
        report = check_update_state(self.root)
        self.assertFalse(report['ok'])
        self.assertIn('FUMO current version does not match',
                      '\n'.join(report['problems']))

    def test_tree_state_consistency(self) -> None:
        tree = self.root / 'fota/dme/tree.xml'
        tree.write_text(tree_xml(state='20'))
        (self.root / 'fota/dm_state.txt').write_text('1')
        report = check_update_state(self.root)
        self.assertTrue(report['ok'], report['problems'])
        self.assertEqual(report['fumo_state'], 20)
        self.assertTrue(report['state_consistent'])
        (self.root / 'fota/dm_state.txt').write_text('0')
        report = check_update_state(self.root)
        self.assertTrue(report['ok'])
        self.assertFalse(report['state_consistent'])
        self.assertTrue(any('disagree' in warning for warning in report['warnings']))

    def test_plaintext_package_blob_is_rejected(self) -> None:
        (self.root / 'usb/bin/.lge.upgrade.xml').write_text('<?xml version="1.0"?>')
        report = check_update_state(self.root)
        self.assertFalse(report['ok'])
        self.assertIn('plaintext XML', '\n'.join(report['problems']))

    def test_cli(self) -> None:
        completed = subprocess.run(
            [sys.executable, '-B', str(VM / 'launch.py'),
             'check-update-state', str(self.root)],
            cwd=VM, capture_output=True, text=True, check=False)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertTrue(json.loads(completed.stdout)['ok'])
        (self.root / 'updateVersionInfo').write_text('bad')
        completed = subprocess.run(
            [sys.executable, '-B', str(VM / 'launch.py'),
             'check-update-state', str(self.root)],
            cwd=VM, capture_output=True, text=True, check=False)
        self.assertEqual(completed.returncode, 1)
        self.assertFalse(json.loads(completed.stdout)['ok'])

    @unittest.skipUnless(shutil.which('mke2fs'), 'mke2fs unavailable')
    def test_ext4_image_roundtrip(self) -> None:
        image = Path(self._tmp.name) / 'update.img'
        completed = subprocess.run(
            ['mke2fs', '-q', '-t', 'ext4', '-d', str(self.root),
             str(image), '16M'], capture_output=True, text=True)
        if completed.returncode != 0:
            self.skipTest(f'mke2fs -d unavailable: {completed.stderr[:120]}')
        report = check_update_state(image)
        self.assertTrue(report['ok'], report['problems'])
        self.assertEqual(report['mode'], 'image')

    @unittest.skipUnless(os.environ.get('VM_REAL_UPDATE_STATE') == '1',
                         'set VM_REAL_UPDATE_STATE=1 for the captured /update')
    def test_real_captured_state(self) -> None:
        root = VM / 'captures/car-20261115/analysis/update'
        if not root.is_dir():
            self.skipTest('captured /update unavailable')
        report = check_update_state(root)
        self.assertTrue(report['ok'], report['problems'])
        self.assertEqual(report['version'], VERSION)
        self.assertEqual(report['fumo_state'], 10)
        self.assertEqual(report['dm_state'], '0')
        encoded = json.dumps(report)
        for sensitive in ('210.183', 'hmota', 'px-hash', 'ccs-hkmc'):
            self.assertNotIn(sensitive, encoded)


if __name__ == '__main__':
    unittest.main()
