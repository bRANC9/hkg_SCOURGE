# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import json
import os
import subprocess
import sys
import unittest
from pathlib import Path

from harness.mof import MofError, build_notification, decode_notification

VM = Path(__file__).resolve().parents[1]
VIN = 'VIN:LABVIN00000000000_AVN'
PREVIOUS = 'NE_E.EUR.S5W_L.001.001.250226'
NEXT = 'NE_E.EUR.S5W_L.001.001.250604'


class MofNotificationTests(unittest.TestCase):
    def test_encode_decode_roundtrip(self) -> None:
        uri = build_notification(VIN, PREVIOUS, NEXT, '54728')
        self.assertIn('/dm/message?msgtype=4&cor=', uri)
        decoded = decode_notification(uri)
        self.assertEqual(decoded['msgtype'], 4)
        self.assertEqual(decoded['vin'], VIN)
        self.assertEqual(decoded['previous'], PREVIOUS)
        self.assertEqual(decoded['next'], NEXT)
        self.assertEqual(decoded['update_id'], '54728')
        self.assertEqual(decoded['client'], 'CLIENT')
        self.assertTrue(decoded['host_present'])
        self.assertNotIn('127.0.0.1', json.dumps(decoded))

    def test_bad_path_type_and_base64(self) -> None:
        uri = build_notification(VIN, PREVIOUS, NEXT, '54728')
        for bad in (uri.replace('/dm/message', '/other'),
                    uri.replace('msgtype=4', 'msgtype=3'),
                    uri.replace('cor=', 'cor=not-base64!')):
            with self.subTest(bad=bad[:60]):
                with self.assertRaises(MofError):
                    decode_notification(bad)

    def test_bad_lab_fields(self) -> None:
        for vin, previous, next_version, update_id in (
                ('VIN:short_AVN', PREVIOUS, NEXT, '54728'),
                (VIN, 'bad', NEXT, '54728'),
                (VIN, PREVIOUS, 'bad', '54728'),
                (VIN, PREVIOUS, NEXT, 'not-numeric')):
            with self.subTest(vin=vin, previous=previous):
                with self.assertRaises(MofError):
                    build_notification(vin, previous, next_version, update_id)
        with self.assertRaises(MofError):
            build_notification(VIN, PREVIOUS, NEXT, '54728',
                               host='127.0.0.1/path')

    def test_cli(self) -> None:
        completed = subprocess.run(
            [sys.executable, '-B', str(VM / 'launch.py'), 'mof-notify',
             '--encode'], cwd=VM, capture_output=True, text=True, check=False)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        uri = json.loads(completed.stdout)['uri']
        completed = subprocess.run(
            [sys.executable, '-B', str(VM / 'launch.py'), 'mof-notify', uri],
            cwd=VM, capture_output=True, text=True, check=False)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        decoded = json.loads(completed.stdout)
        self.assertTrue(decoded['ok'])
        self.assertEqual(decoded['vin'], VIN)
        self.assertNotIn('127.0.0.1', completed.stdout)
        completed = subprocess.run(
            [sys.executable, '-B', str(VM / 'launch.py'), 'mof-notify',
             'http://example.invalid/not-dm'], cwd=VM, capture_output=True,
            text=True, check=False)
        self.assertEqual(completed.returncode, 1)
        self.assertFalse(json.loads(completed.stdout)['ok'])

    @unittest.skipUnless(os.environ.get('VM_REAL_MOF') == '1',
                         'set VM_REAL_MOF=1 for the captured notification')
    def test_real_captured_notification(self) -> None:
        path = VM / 'captures/car-20261115/analysis/update/fota/.notify_uri'
        if not path.is_file():
            self.skipTest('captured notification unavailable')
        decoded = decode_notification(path.read_text().strip())
        self.assertEqual(decoded['msgtype'], 4)
        self.assertRegex(decoded['vin'], r'^VIN:[A-Z0-9]{17}_AVN$')
        self.assertRegex(decoded['previous'],
                         r'^[A-Z0-9_]+\.[A-Z0-9_]+\.[A-Z0-9_]+\.\d{3}\.\d{3}\.\d{6}$')
        self.assertTrue(decoded['update_id'].isdigit())
        encoded = json.dumps(decoded)
        for sensitive in ('210.183', 'hmota', 'px-hash', 'ccs-hkmc'):
            self.assertNotIn(sensitive, encoded)


if __name__ == '__main__':
    unittest.main()
