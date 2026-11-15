# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import socket
import subprocess
import tempfile
import time
import unittest
import urllib.request
from pathlib import Path

from harness.dp import DpComponent, DpUpdate, build_dp, parse_dp

VM = Path(__file__).resolve().parents[1]
MOCK_SERVER = VM / 'payload/tweaks-vm/mock-dm-server'


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(('127.0.0.1', 0))
        return int(sock.getsockname()[1])


@unittest.skipUnless(MOCK_SERVER.is_file(),
                     'mock-dm-server build artifact is not present')
class MockDmServerDpTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.log = Path(self.tmp.name) / 'mock-dm.log'
        self.port = free_port()
        self.server = subprocess.Popen(
            [str(MOCK_SERVER), str(self.port), str(self.log), 'download'],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if self.server.poll() is not None:
                output = self.server.stdout.read() if self.server.stdout else ''
                self.fail(f'mock server exited early: {output}')
            try:
                with socket.create_connection(('127.0.0.1', self.port), 0.2):
                    return
            except OSError:
                time.sleep(0.05)
        self.fail('mock server did not start')

    def tearDown(self) -> None:
        self.server.terminate()
        try:
            self.server.wait(timeout=1)
        except subprocess.TimeoutExpired:
            self.server.kill()
            self.server.wait(timeout=1)
        if self.server.stdout:
            self.server.stdout.close()
        self.tmp.cleanup()

    def get(self, path: str) -> tuple[bytes, str]:
        with urllib.request.urlopen(
                f'http://127.0.0.1:{self.port}{path}', timeout=3) as response:
            return response.read(), response.headers.get('Content-Type', '')

    def test_served_dp_matches_python_builder(self) -> None:
        data, content_type = self.get('/object.bin')
        self.assertEqual(content_type, 'application/octet-stream')
        expected = build_dp([DpComponent(
            'LABCOMP', 'LABID', '1.0', 'LABVENDOR',
            updates=(DpUpdate(b'LABUPDATE-DATA01'),))],
            signing_key=b'lab-dp-key')
        self.assertEqual(data, expected)
        report = parse_dp(data, b'lab-dp-key')
        self.assertTrue(report['ok'], report['problems'])
        self.assertTrue(report['signature']['valid'])
        self.assertEqual(report['body_size'], 132)
        self.assertEqual(report['components'][0]['name'], 'LABCOMP')

    def test_download_descriptor_points_at_the_dp(self) -> None:
        data, content_type = self.get('/package.bin')
        text = data.decode('ascii')
        self.assertEqual(content_type, 'application/vnd.oma.dd+xml')
        self.assertIn('<size>184</size>', text)
        self.assertIn(
            '<objectURI>http://127.0.0.1:8002/object.bin</objectURI>', text)
        self.assertIn('<installNotifyURI>', text)
        log = self.log.read_text()
        self.assertIn('MOCK_DM_DD method=GET path=/package.bin', log)



@unittest.skipUnless(MOCK_SERVER.is_file(),
                     'mock-dm-server build artifact is not present')
class MockDmServerBadSignatureTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.log = Path(self.tmp.name) / 'mock-dm.log'
        self.port = free_port()
        self.server = subprocess.Popen(
            [str(MOCK_SERVER), str(self.port), str(self.log), 'badsig'],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if self.server.poll() is not None:
                output = self.server.stdout.read() if self.server.stdout else ''
                self.fail(f'mock server exited early: {output}')
            try:
                with socket.create_connection(('127.0.0.1', self.port), 0.2):
                    return
            except OSError:
                time.sleep(0.05)
        self.fail('mock server did not start')

    def tearDown(self) -> None:
        self.server.terminate()
        try:
            self.server.wait(timeout=1)
        except subprocess.TimeoutExpired:
            self.server.kill()
            self.server.wait(timeout=1)
        if self.server.stdout:
            self.server.stdout.close()
        self.tmp.cleanup()

    def test_bad_signature_rejected_by_validator(self) -> None:
        with urllib.request.urlopen(
                f'http://127.0.0.1:{self.port}/object.bin', timeout=3) as response:
            data = response.read()
        report = parse_dp(data, b'lab-dp-key')
        self.assertFalse(report['ok'])
        self.assertIn('signature mismatch', report['problems'])
        self.assertTrue(parse_dp(data)['ok'])
        self.assertIn('MOCK_DM_BADSIG path=/object.bin bytes=184',
                      self.log.read_text())


if __name__ == '__main__':
    unittest.main()
