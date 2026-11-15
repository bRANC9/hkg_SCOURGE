# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from harness.capture import check_capture, parse_sums

VM = Path(__file__).resolve().parents[1]


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def make_capture(root: Path) -> dict[str, bytes]:
    for name in ('archives', 'partitions', 'runtime', 'meta', 'critical'):
        (root / name).mkdir(parents=True, exist_ok=True)
    files = {
        'archives/rootfs.tar': b'rootfs',
        'critical/critical.tar': b'critical',
        'partitions/p1-boot-a.img': b'partition',
        'runtime/dmesg.txt': b'dmesg',
    }
    for rel, data in files.items():
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    (root / 'meta/SOURCE.txt').write_text('synthetic capture\n')
    (root / 'meta/SHA256SUMS').write_text(''.join(
        f'{digest(data)}  ./{rel}\n' for rel, data in files.items()))
    return files


class CaptureTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix='vm-capture-')
        self.root = Path(self._tmp.name) / 'capture'
        self.files = make_capture(self.root)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_valid_capture(self) -> None:
        report = check_capture(self.root)
        self.assertTrue(report['ok'], report['problems'])
        self.assertEqual(report['archives'], ['rootfs.tar'])
        self.assertEqual(report['partitions'], ['p1-boot-a.img'])
        self.assertEqual(report['hash_entries'], 4)
        self.assertEqual(report['hashes_verified'], 0)
        self.assertEqual(report['unlisted'], [])
        self.assertEqual(report['warnings'], [])

    def test_missing_required_file(self) -> None:
        (self.root / 'archives/rootfs.tar').unlink()
        report = check_capture(self.root)
        self.assertFalse(report['ok'])
        self.assertIn('missing required file: archives/rootfs.tar',
                      '\n'.join(report['problems']))

    def test_hash_verification(self) -> None:
        (self.root / 'critical/critical.tar').write_bytes(b'changed')
        self.assertTrue(check_capture(self.root)['ok'])
        report = check_capture(self.root, verify_hashes=True)
        self.assertFalse(report['ok'])
        self.assertIn('critical/critical.tar', report['mismatched'])
        self.assertEqual(report['hashes_verified'], 4)

    def test_missing_hash_target(self) -> None:
        (self.root / 'runtime/dmesg.txt').unlink()
        report = check_capture(self.root)
        self.assertFalse(report['ok'])
        self.assertIn('runtime/dmesg.txt', report['missing'])

    def test_malformed_sums(self) -> None:
        with (self.root / 'meta/SHA256SUMS').open('a') as handle:
            handle.write('not-a-sha  ./somewhere\n')
        entries, problems = parse_sums(self.root / 'meta/SHA256SUMS')
        self.assertEqual(len(entries), 4)
        self.assertTrue(problems)
        report = check_capture(self.root)
        self.assertFalse(report['ok'])
        self.assertTrue(report['problems'])

    def test_navi_archive_and_warning(self) -> None:
        (self.root / 'archives/navi2-nonmap.tar').write_bytes(b'navi-nonmap')
        (self.root / 'archives/navi-map.tar').write_bytes(b'map')
        (self.root / 'meta/SHA256SUMS.navi2').write_text(
            f'{digest(b"navi-nonmap")}  ./archives/navi2-nonmap.tar\n'
            f'{digest(b"map")}  ./archives/navi-map.tar\n')
        report = check_capture(self.root, verify_hashes=True)
        self.assertTrue(report['ok'], report['problems'])
        self.assertEqual(report['hash_entries'], 6)
        self.assertEqual(report['hashes_verified'], 6)
        self.assertTrue(any('navi-map.tar' in warning
                            for warning in report['warnings']))
        strict = check_capture(self.root, verify_hashes=True, reject_maps=True)
        self.assertFalse(strict['ok'])
        self.assertTrue(any('rejected map data' in problem
                            for problem in strict['problems']))

    def test_absolute_sums_path_is_capture_relative(self) -> None:
        (self.root / 'archives/navi2-nonmap.tar').write_bytes(b'navi-nonmap')
        (self.root / 'meta/SHA256SUMS.navi2').write_text(
            f'{digest(b"navi-nonmap")}  /tmp/car-usb/car-dump-20261115/'
            'archives/navi2-nonmap.tar\n')
        report = check_capture(self.root, verify_hashes=True)
        self.assertTrue(report['ok'], report['problems'])
        self.assertEqual(report['missing'], [])
        self.assertEqual(report['hashes_verified'], 5)

    def test_unlisted_archive_is_rejected(self) -> None:
        (self.root / 'archives/extra.tar').write_bytes(b'extra')
        report = check_capture(self.root)
        self.assertFalse(report['ok'])
        self.assertEqual(report['unlisted'], ['archives/extra.tar'])
        self.assertIn('unlisted artifact: archives/extra.tar',
                      '\n'.join(report['problems']))

    def test_hash_target_cannot_escape_root(self) -> None:
        outside = self.root.parent / 'outside.txt'
        outside.write_bytes(b'outside')
        with (self.root / 'meta/SHA256SUMS').open('a') as handle:
            handle.write(f'{digest(b"outside")}  ../outside.txt\n')
        report = check_capture(self.root)
        self.assertFalse(report['ok'])
        self.assertTrue(any('escapes capture root' in problem
                            for problem in report['problems']))

    def test_missing_root(self) -> None:
        report = check_capture(self.root / 'missing')
        self.assertFalse(report['ok'])
        self.assertIn('not a directory', report['problems'][0])

    def test_cli(self) -> None:
        completed = subprocess.run(
            [sys.executable, '-B', str(VM / 'launch.py'), 'check-capture',
             str(self.root)], cwd=VM, capture_output=True, text=True,
            check=False)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertTrue(json.loads(completed.stdout)['ok'])
        completed = subprocess.run(
            [sys.executable, '-B', str(VM / 'launch.py'), 'check-capture',
             str(self.root), '--verify-hashes'], cwd=VM,
            capture_output=True, text=True, check=False)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        (self.root / 'meta/SHA256SUMS').write_text('bad\n')
        completed = subprocess.run(
            [sys.executable, '-B', str(VM / 'launch.py'), 'check-capture',
             str(self.root)], cwd=VM, capture_output=True, text=True,
            check=False)
        self.assertEqual(completed.returncode, 1)
        self.assertFalse(json.loads(completed.stdout)['ok'])


if __name__ == '__main__':
    unittest.main()
