# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from harness.fumo import (FumoError, FumoMachine, MockDmEndpoint, Package,
                          State, make_package, run_exchange, run_script,
                          version_key)

VM = Path(__file__).resolve().parents[1]


class FumoPackageTests(unittest.TestCase):
    def test_state_values(self) -> None:
        self.assertEqual([int(state) for state in State],
                         [10, 20, 30, 40])

    def test_package_build_and_validation(self) -> None:
        package = Package.build('mango-rootfs', '8',
                                'https://update.example/pkg.bin', b'payload')
        self.assertEqual(package.size, len(b'payload'))
        self.assertEqual(len(package.digest), 64)
        package.validate()
        for url in ('ftp://update.example/pkg', 'https://user:pw@example/pkg',
                    'https://example/pkg#frag', 'not-a-url'):
            with self.subTest(url=url):
                with self.assertRaises(FumoError):
                    Package.build('pkg', '8', url, b'x')
        with self.assertRaises(FumoError):
            Package('bad name', '8', 'https://example/pkg', b'x', 1,
                    '0' * 64)
        with self.assertRaises(FumoError):
            Package('pkg', 'v8', 'https://example/pkg', b'x', 1, '0' * 64)

    def test_version_key(self) -> None:
        self.assertGreater(version_key('1.10'), version_key('1.9'))
        self.assertEqual(version_key('8'), (8,))
        with self.assertRaises(FumoError):
            version_key('8-beta')


class FumoMachineTests(unittest.TestCase):
    def setUp(self) -> None:
        self.package = make_package('mango-rootfs', '8')

    def test_full_exchange(self) -> None:
        machine = FumoMachine()
        machine.begin_download(self.package)
        self.assertEqual(machine.state, State.DOWNLOADING)
        for offset in range(0, len(self.package.data), 3):
            machine.append_download(self.package.data[offset:offset + 3])
        machine.complete_download()
        self.assertEqual(machine.state, State.DOWNLOADED)
        machine.begin_install()
        self.assertEqual(machine.state, State.UPDATING)
        machine.complete_install()
        self.assertEqual(machine.state, State.IDLE)
        self.assertEqual(machine.installed_version, '8')
        self.assertIsNone(machine.package)
        actions = [event['action'] for event in machine.events]
        self.assertEqual(actions[0], 'begin-download')
        self.assertEqual(actions[-1], 'install-complete')
        self.assertEqual(machine.status()['state'], 10)

    def test_invalid_transition_fails_closed(self) -> None:
        machine = FumoMachine()
        with self.assertRaises(FumoError):
            machine.complete_download()
        self.assertEqual(machine.state, State.IDLE)
        self.assertEqual(machine.events[-1]['result'], 'error')

    def test_download_size_mismatch_fails(self) -> None:
        machine = FumoMachine()
        machine.begin_download(self.package)
        machine.append_download(self.package.data[:-1])
        with self.assertRaisesRegex(FumoError, 'size'):
            machine.complete_download()
        self.assertEqual(machine.state, State.DOWNLOADING)

    def test_download_overflow_fails(self) -> None:
        machine = FumoMachine()
        machine.begin_download(self.package)
        with self.assertRaisesRegex(FumoError, 'exceeds'):
            machine.append_download(self.package.data + b'!')
        self.assertEqual(machine.state, State.DOWNLOADING)

    def test_download_failure_returns_to_idle_and_retries(self) -> None:
        machine = FumoMachine()
        machine.begin_download(self.package)
        machine.append_download(b'partial')
        machine.fail_download('network')
        self.assertEqual(machine.state, State.IDLE)
        self.assertIsNone(machine.package)
        machine.begin_download(self.package)
        machine.append_download(self.package.data)
        machine.complete_download()
        self.assertEqual(machine.state, State.DOWNLOADED)

    def test_install_failure_stays_downloaded_and_retries(self) -> None:
        machine = FumoMachine()
        machine.begin_download(self.package)
        machine.append_download(self.package.data)
        machine.complete_download()
        machine.begin_install()
        machine.fail_install('power')
        self.assertEqual(machine.state, State.DOWNLOADED)
        self.assertEqual(machine.package, self.package)
        machine.begin_install()
        machine.complete_install()
        self.assertEqual(machine.installed_version, '8')

    def test_reset_clears_active_package(self) -> None:
        machine = FumoMachine('7')
        machine.begin_download(self.package)
        machine.reset()
        self.assertEqual(machine.state, State.IDLE)
        self.assertIsNone(machine.package)
        self.assertEqual(machine.installed_version, '7')

    def test_downgrade_is_rejected(self) -> None:
        machine = FumoMachine('8')
        with self.assertRaisesRegex(FumoError, 'not newer'):
            machine.begin_download(make_package('mango-rootfs', '8'))
        self.assertEqual(machine.state, State.IDLE)


class MockEndpointTests(unittest.TestCase):
    def test_auth_and_not_found(self) -> None:
        package = make_package('mango-rootfs', '8')
        endpoint = MockDmEndpoint('token', {package.name: package})
        with self.assertRaisesRegex(FumoError, 'authentication'):
            endpoint.query('mango-rootfs', 'wrong')
        with self.assertRaisesRegex(FumoError, 'not found'):
            endpoint.query('missing', 'token')
        self.assertEqual(endpoint.queries[-1]['result'], 'not-found')
        self.assertEqual(endpoint.query('mango-rootfs', 'token'), package)

    def test_endpoint_rejects_downgrade(self) -> None:
        package = make_package('mango-rootfs', '7')
        endpoint = MockDmEndpoint('token', {package.name: package},
                                  installed_version='8')
        with self.assertRaisesRegex(FumoError, 'not newer'):
            endpoint.query('mango-rootfs', 'token')
        self.assertEqual(endpoint.queries[-1]['result'], 'not-newer')


class ExchangeReportTests(unittest.TestCase):
    def test_canonical_exchange(self) -> None:
        report = run_exchange()
        self.assertTrue(report['ok'])
        self.assertEqual(report['installed_before'], None)
        self.assertEqual(report['installed_after'], '8')
        self.assertEqual(report['final_state'], 10)
        self.assertEqual(report['queries'][0]['result'], 'ok')

    def test_injected_download_failure_is_recoverable(self) -> None:
        report = run_exchange(fail_stage='download')
        self.assertTrue(report['ok'])
        self.assertEqual(report['failed_stage'], 'download')
        self.assertEqual(report['final_state'], 10)
        self.assertIsNone(report['installed_after'])
        self.assertEqual(report['events'][-1]['action'], 'download-failed')

    def test_injected_install_failure_stays_downloaded(self) -> None:
        report = run_exchange(fail_stage='install')
        self.assertTrue(report['ok'])
        self.assertEqual(report['failed_stage'], 'install')
        self.assertEqual(report['final_state'], 30)
        self.assertEqual(report['events'][-1]['action'], 'install-failed')

    def test_bad_chunk_size_and_downgrade_fail(self) -> None:
        with self.assertRaises(FumoError):
            run_exchange(chunk_size=0)
        report = run_exchange(installed='8', package_version='8')
        self.assertFalse(report['ok'])
        self.assertIn('not newer', report['error'])

    def test_cli(self) -> None:
        completed = subprocess.run(
            [sys.executable, '-B', str(VM / 'launch.py'), 'fumo-exchange'],
            cwd=VM, capture_output=True, text=True, check=False)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertTrue(json.loads(completed.stdout)['ok'])
        completed = subprocess.run(
            [sys.executable, '-B', str(VM / 'launch.py'), 'fumo-exchange',
             '--fail', 'download'], cwd=VM, capture_output=True, text=True,
            check=False)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(json.loads(completed.stdout)['final_state'], 10)


class FumoScriptTests(unittest.TestCase):
    def test_full_script(self) -> None:
        report = run_script([{'op': 'download'}, {'op': 'install'}])
        self.assertTrue(report['ok'])
        self.assertEqual(report['installed_after'], '8')
        self.assertEqual(report['final_state'], 10)
        self.assertEqual([step['ok'] for step in report['steps']],
                         [True, True])
        self.assertEqual(report['steps'][0]['from'], 10)
        self.assertEqual(report['steps'][0]['to'], 30)

    def test_failure_then_retry_script(self) -> None:
        report = run_script([
            {'op': 'begin-download'},
            {'op': 'fail-download', 'reason': 'network'},
            {'op': 'download'},
            {'op': 'install'},
        ])
        self.assertTrue(report['ok'])
        self.assertEqual(report['steps'][1]['to'], 10)
        self.assertEqual(report['steps'][3]['to'], 10)
        self.assertEqual(report['installed_after'], '8')

    def test_invalid_operation_stops_fail_closed(self) -> None:
        report = run_script([{'op': 'download'}, {'op': 'bogus'}])
        self.assertFalse(report['ok'])
        self.assertEqual(report['final_state'], 30)
        self.assertEqual(report['steps'][-1]['ok'], False)
        self.assertIn('unknown op', report['error'])

    def test_bad_append_hex_fails_closed(self) -> None:
        report = run_script([{'op': 'begin-download'},
                             {'op': 'append', 'data': 'not-hex'}])
        self.assertFalse(report['ok'])
        self.assertEqual(report['final_state'], 20)

    def test_cli_script(self) -> None:
        with tempfile.TemporaryDirectory(prefix='vm-fumo-script-') as tmp:
            path = Path(tmp) / 'script.json'
            path.write_text(json.dumps([{'op': 'download'},
                                        {'op': 'install'}]))
            completed = subprocess.run(
                [sys.executable, '-B', str(VM / 'launch.py'),
                 'fumo-exchange', '--script', str(path)],
                cwd=VM, capture_output=True, text=True, check=False)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            report = json.loads(completed.stdout)
            self.assertTrue(report['ok'])
            self.assertEqual(report['final_state'], 10)
            path.write_text(json.dumps([{'op': 'bogus'}]))
            completed = subprocess.run(
                [sys.executable, '-B', str(VM / 'launch.py'),
                 'fumo-exchange', '--script', str(path)],
                cwd=VM, capture_output=True, text=True, check=False)
            self.assertEqual(completed.returncode, 1)
            self.assertFalse(json.loads(completed.stdout)['ok'])

if __name__ == '__main__':
    unittest.main()
