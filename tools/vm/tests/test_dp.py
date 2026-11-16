# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from harness.dp import (DpComponent, DpError, DpUpdate, HEADER_SIZE,
                        PREAMBLE, SIG_MAGIC, SIG_TRAILER, build_dp, check_dp,
                        parse_dp)


class DpBuildTests(unittest.TestCase):
    def test_single_component_layout(self) -> None:
        data = build_dp([DpComponent('LABCOMP', 'LABID', '1.0', 'LABVENDOR')])
        self.assertEqual(len(data), 108)
        self.assertEqual(data[:len(PREAMBLE)], PREAMBLE)
        self.assertEqual(struct.unpack_from('<I', data, 20)[0], 4)
        self.assertEqual(struct.unpack_from('<I', data, 24)[0], 1)
        self.assertEqual(struct.unpack_from('<I', data, 28)[0], 108)
        self.assertEqual(struct.unpack_from('<I', data, 32)[0], 0)
        self.assertEqual(struct.unpack_from('<I', data, 36)[0], 0)
        self.assertEqual(struct.unpack_from('<I', data, 44)[0], HEADER_SIZE)
        self.assertEqual(struct.unpack_from('<I', data, 48)[0], 80)
        self.assertEqual(data[80:88], b'LABCOMP\0')
        self.assertEqual(data[88:94], b'LABID\0')
        self.assertEqual(data[94:98], b'1.0\0')
        self.assertEqual(data[98:108], b'LABVENDOR\0')

    def test_two_components_share_one_string_table(self) -> None:
        data = build_dp([
            DpComponent('ONE', 'ID1', '1.0', 'LAB'),
            DpComponent('TWO', 'ID2', '2.0', 'LAB', installer_type=1),
        ])
        report = parse_dp(data)
        self.assertTrue(report['ok'], report['problems'])
        self.assertEqual([item['name'] for item in report['components']],
                         ['ONE', 'TWO'])
        self.assertEqual(report['components'][1]['installer_type'], 1)
        strings = b'ONE\x00ID1\x001.0\x00LAB\x00TWO\x00ID2\x002.0\x00LAB\x00'
        self.assertEqual(len(data), 52 + 2 * 28 + len(strings))

    def test_roundtrip_reports_fields(self) -> None:
        data = build_dp([DpComponent('LABCOMP', 'LABID', '8.1', 'LABVENDOR',
                                     installer_type=2)])
        report = parse_dp(data)
        self.assertTrue(report['ok'], report['problems'])
        self.assertEqual(report['version'], 4)
        self.assertEqual(report['size'], 108)
        self.assertEqual(report['components'], [{
            'name': 'LABCOMP', 'id': 'LABID', 'version': '8.1',
            'vendor': 'LABVENDOR', 'installer_type': 2, 'update_count': 0,
            'updates': [],
        }])

    def test_update_payload_layout(self) -> None:
        data = build_dp([DpComponent('LABCOMP', 'LABID', '1.0', 'LABVENDOR',
                                     updates=(DpUpdate(b'LABUPDATE-DATA01'),))])
        self.assertEqual(len(data), 132)
        self.assertEqual(struct.unpack_from('<2I', data, 80), (116, 16))
        self.assertEqual(data[116:132], b'LABUPDATE-DATA01')
        report = parse_dp(data)
        self.assertTrue(report['ok'], report['problems'])
        self.assertEqual(report['components'][0]['update_count'], 1)
        self.assertEqual(report['components'][0]['updates'],
                         [{'offset': 116, 'size': 16}])

    def test_signature_trailer(self) -> None:
        key = b'lab-dp-key'
        body = build_dp([DpComponent('LABCOMP', 'LABID',
                                     vendor='LABVENDOR')])
        data = build_dp([DpComponent('LABCOMP', 'LABID',
                                     vendor='LABVENDOR')], signing_key=key)
        self.assertEqual(len(data), len(body) + SIG_TRAILER)
        self.assertEqual(data[len(body):len(body) + len(SIG_MAGIC)], SIG_MAGIC)
        report = parse_dp(data, key)
        self.assertTrue(report['ok'], report['problems'])
        self.assertEqual(report['body_size'], len(body))
        self.assertEqual(report['signature'],
                         {'present': True, 'algorithm': 'hmac-sha256',
                          'valid': True})

    def test_signature_detects_tampering(self) -> None:
        key = b'lab-dp-key'
        data = bytearray(build_dp([DpComponent('LABCOMP', 'LABID',
                                               updates=(DpUpdate(b'DATA'),))],
                                  signing_key=key))
        data[data.index(b'DATA')] ^= 0x01
        report = parse_dp(bytes(data), key)
        self.assertFalse(report['ok'])
        self.assertIn('signature mismatch', report['problems'])

    def test_signature_without_key_is_unverified(self) -> None:
        data = build_dp([DpComponent('LABCOMP', 'LABID')],
                        signing_key=b'lab-dp-key')
        report = parse_dp(data)
        self.assertTrue(report['ok'], report['problems'])
        self.assertEqual(report['signature']['valid'], None)

    def test_multi_component_and_multi_update_layout(self) -> None:
        components = [
            DpComponent('LABCOMP', 'LABID', '1.0', 'LABVENDOR',
                        installer_type=0,
                        updates=(DpUpdate(b'LABUPDATE-DATA01'),
                                 DpUpdate(b'LABUPDATE-DATA02'))),
            DpComponent('LABCOMP2', 'LABID2', '2.0', 'LABVENDOR2',
                        installer_type=1,
                        updates=(DpUpdate(b'LABUPDATE-DATA03'),)),
        ]
        body = build_dp(components)
        self.assertEqual(len(body), 239)
        self.assertEqual(struct.unpack_from('<2I', body, 24), (2, 239))
        self.assertEqual(struct.unpack_from('<7I', body, 52),
                         (132, 140, 146, 150, 0, 2, 108))
        self.assertEqual(struct.unpack_from('<7I', body, 80),
                         (160, 169, 176, 180, 1, 1, 124))
        self.assertEqual(struct.unpack_from('<6I', body, 108),
                         (191, 16, 207, 16, 223, 16))
        self.assertEqual(body[191:239],
                         b'LABUPDATE-DATA01LABUPDATE-DATA02'
                         b'LABUPDATE-DATA03')
        report = parse_dp(body)
        self.assertTrue(report['ok'], report['problems'])
        self.assertEqual([c['name'] for c in report['components']],
                         ['LABCOMP', 'LABCOMP2'])
        self.assertEqual([len(c['updates']) for c in report['components']],
                         [2, 1])
        self.assertEqual([u['size'] for c in report['components']
                          for u in c['updates']], [16, 16, 16])

    def test_multi_component_signature_trailer(self) -> None:
        components = [
            DpComponent('LABCOMP', 'LABID', '1.0', 'LABVENDOR',
                        installer_type=0,
                        updates=(DpUpdate(b'LABUPDATE-DATA01'),
                                 DpUpdate(b'LABUPDATE-DATA02'))),
            DpComponent('LABCOMP2', 'LABID2', '2.0', 'LABVENDOR2',
                        installer_type=1,
                        updates=(DpUpdate(b'LABUPDATE-DATA03'),)),
        ]
        body = build_dp(components)
        signed = build_dp(components, signing_key=b'lab-dp-key')
        self.assertEqual(len(signed), len(body) + SIG_TRAILER)
        report = parse_dp(signed, b'lab-dp-key')
        self.assertTrue(report['ok'], report['problems'])
        self.assertTrue(report['signature']['valid'])
        self.assertEqual(report['body_size'], 239)

    def test_rejects_empty_component_list(self) -> None:
        with self.assertRaises(DpError):
            build_dp([])

    def test_rejects_invalid_installer_type(self) -> None:
        with self.assertRaises(DpError):
            build_dp([DpComponent('A', 'B', installer_type=9)])

    def test_rejects_empty_update_payload(self) -> None:
        with self.assertRaises(DpError):
            build_dp([DpComponent('A', 'B', updates=(DpUpdate(b''),))])


class DpParseTests(unittest.TestCase):
    def setUp(self) -> None:
        self.data = build_dp([DpComponent('LABCOMP', 'LABID')])

    def assert_problem(self, data: bytes, text: str) -> None:
        report = parse_dp(data)
        self.assertFalse(report['ok'])
        self.assertTrue(any(text in problem for problem in report['problems']),
                        report['problems'])

    def test_rejects_short_container(self) -> None:
        self.assert_problem(b'RedBend', 'shorter than')

    def test_rejects_preamble(self) -> None:
        data = bytearray(self.data)
        data[0:2] = b'XX'
        self.assert_problem(bytes(data), 'preamble')

    def test_rejects_unsupported_version(self) -> None:
        data = bytearray(self.data)
        struct.pack_into('<I', data, 20, 3)
        self.assert_problem(bytes(data), 'unsupported DP version')

    def test_rejects_size_mismatch(self) -> None:
        data = bytearray(self.data)
        struct.pack_into('<I', data, 28, len(data) + 1)
        self.assert_problem(bytes(data), 'declared DP size')

    def test_rejects_bad_component_pointer(self) -> None:
        data = bytearray(self.data)
        struct.pack_into('<I', data, 44, 4096)
        self.assert_problem(bytes(data), 'component table is outside')

    def test_rejects_bad_string_offset(self) -> None:
        data = bytearray(self.data)
        struct.pack_into('<I', data, 52, 4096)
        self.assert_problem(bytes(data), 'name offset')

    def test_rejects_update_payload_outside_container(self) -> None:
        data = bytearray(build_dp([DpComponent(
            'LABCOMP', 'LABID', updates=(DpUpdate(b'DATA'),))]))
        struct.pack_into('<I', data, 84, 4096)  # payload size
        self.assert_problem(bytes(data), 'update payload is outside')

    def test_rejects_unterminated_string(self) -> None:
        data = bytearray(self.data)
        data[80:] = b'X' * (len(data) - 80)
        self.assert_problem(bytes(data), 'not NUL-terminated')


class DpFileTests(unittest.TestCase):
    def test_check_dp_file(self) -> None:
        data = build_dp([DpComponent('LABCOMP', 'LABID')])
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'package.dp'
            path.write_bytes(data)
            report = check_dp(path)
        self.assertTrue(report['ok'], report['problems'])
        self.assertEqual(report['source'], str(path))
        self.assertEqual(report['components'][0]['name'], 'LABCOMP')

    def test_check_dp_with_key(self) -> None:
        data = build_dp([DpComponent('LABCOMP', 'LABID')],
                        signing_key=b'lab-dp-key')
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'package.dp'
            path.write_bytes(data)
            report = check_dp(path, b'lab-dp-key')
        self.assertTrue(report['ok'], report['problems'])
        self.assertTrue(report['signature']['valid'])

    def test_check_dp_missing_file(self) -> None:
        report = check_dp(Path('/nonexistent/lab.dp'))
        self.assertFalse(report['ok'])
        self.assertTrue(report['problems'])

    def test_launch_cli_check_dp(self) -> None:
        data = build_dp([DpComponent('LABCOMP', 'LABID')])
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'package.dp'
            path.write_bytes(data)
            result = subprocess.run(
                [sys.executable, '-B', str(Path(__file__).resolve().parents[1] /
                                           'launch.py'),
                 'check-dp', str(path)],
                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertTrue(report['ok'])
        self.assertEqual(report['components'][0]['id'], 'LABID')


if __name__ == '__main__':
    unittest.main()
