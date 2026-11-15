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

from harness.dm_queue import (DATA_SIZE, OEM_COMMAND_TYPE, OEM_EVENT_TYPE,
                              QueueError, decode_message, decode_payload,
                              encode_command, encode_event, encode_payload,
                              parse_strace)

VM = Path(__file__).resolve().parents[1]


class DmQueueCodecTests(unittest.TestCase):
    def test_command_roundtrip_and_offsets(self) -> None:
        mtype, data = encode_command(66, 7, -9, 'msgtype=4&cor=LAB')
        self.assertEqual(mtype, OEM_COMMAND_TYPE)
        self.assertEqual(len(data), DATA_SIZE)
        self.assertEqual(struct.unpack_from('<i', data, 0)[0], 66)
        self.assertEqual(struct.unpack_from('<q', data, 8)[0], 7)
        self.assertEqual(struct.unpack_from('<q', data, 16)[0], -9)
        self.assertEqual(data[32:32 + 16], b'msgtype=4&cor=LA')
        decoded = decode_message(mtype, data)
        self.assertEqual(decoded['kind'], 'command')
        self.assertEqual(decoded['code'], 66)
        self.assertEqual(decoded['field1'], 7)
        self.assertEqual(decoded['field2'], -9)
        self.assertEqual(decoded['text'], 'msgtype=4&cor=LAB')

    def test_event_roundtrip(self) -> None:
        mtype, data = encode_event(93)
        self.assertEqual(mtype, OEM_EVENT_TYPE)
        decoded = decode_message(mtype, data)
        self.assertEqual(decoded['kind'], 'event')
        self.assertEqual(decoded['code'], 93)
        self.assertEqual(decoded['text'], '')

    def test_payload_size_and_mtype_validation(self) -> None:
        with self.assertRaisesRegex(QueueError, 'payload must be'):
            decode_payload(b'\0' * 10)
        with self.assertRaisesRegex(QueueError, 'unknown mtype'):
            decode_message(0x99, encode_payload(1))

    def test_field_validation(self) -> None:
        with self.assertRaisesRegex(QueueError, 'outside int32'):
            encode_payload(2 ** 31)
        with self.assertRaisesRegex(QueueError, 'outside int64'):
            encode_payload(1, 2 ** 63)
        with self.assertRaisesRegex(QueueError, 'must be an integer'):
            encode_payload(1, True)
        with self.assertRaisesRegex(QueueError, 'too long'):
            encode_payload(1, text='x' * (DATA_SIZE - 32))
        with self.assertRaisesRegex(QueueError, 'must be a string'):
            encode_payload(1, text=b'bytes')

    def test_cli(self) -> None:
        completed = subprocess.run(
            [sys.executable, '-B', str(VM / 'launch.py'), 'dmengine-queue',
             'encode', '--code', '66', '--field1', '7',
             '--text', 'msgtype=4&cor=LAB'],
            cwd=VM, capture_output=True, text=True, check=False)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        encoded = json.loads(completed.stdout)
        self.assertTrue(encoded['ok'])
        self.assertEqual(encoded['mtype'], OEM_COMMAND_TYPE)
        completed = subprocess.run(
            [sys.executable, '-B', str(VM / 'launch.py'), 'dmengine-queue',
             'decode', '--mtype', '0x36', '--hex', encoded['hex']],
            cwd=VM, capture_output=True, text=True, check=False)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        decoded = json.loads(completed.stdout)
        self.assertEqual(decoded['code'], 66)
        self.assertEqual(decoded['field1'], 7)
        self.assertEqual(decoded['text'], 'msgtype=4&cor=LAB')
        completed = subprocess.run(
            [sys.executable, '-B', str(VM / 'launch.py'), 'dmengine-queue',
             'decode', '--hex', '00'], cwd=VM, capture_output=True,
            text=True, check=False)
        self.assertEqual(completed.returncode, 1)
        self.assertFalse(json.loads(completed.stdout)['ok'])


class QueueTraceTests(unittest.TestCase):
    @staticmethod
    def line(pid: int, call: str, mtype: int, data: bytes) -> str:
        escaped = ''.join(f'\\x{byte:02x}' for byte in data)
        return f'{pid}   {call}(32769, {{{mtype}, "{escaped}"}}, 1048, 0) = 0\n'

    def test_parse_command_and_event_lines(self) -> None:
        _, command = encode_command(66, 7, -9, 'msgtype=4&cor=LAB')
        _, event = encode_event(93)
        messages = parse_strace(self.line(820, 'msgsnd', OEM_COMMAND_TYPE, command)
                                + self.line(991, 'msgrcv', OEM_EVENT_TYPE, event))
        self.assertEqual(len(messages), 2)
        self.assertEqual(messages[0]['kind'], 'command')
        self.assertEqual(messages[0]['code'], 66)
        self.assertEqual(messages[0]['field1'], 7)
        self.assertEqual(messages[0]['field2'], -9)
        self.assertEqual(messages[0]['text'], 'msgtype=4&cor=LAB')
        self.assertTrue(messages[0]['complete'])
        self.assertEqual(messages[1]['kind'], 'event')
        self.assertEqual(messages[1]['code'], 93)

    def test_partial_and_unfinished_lines(self) -> None:
        _, data = encode_event(95, -4, 0)
        partial = data[:300]
        messages = parse_strace(self.line(991, 'msgrcv', OEM_EVENT_TYPE, partial)
                                + '202   msgrcv(32769,  <unfinished ...>\n')
        self.assertEqual(len(messages), 1)
        self.assertFalse(messages[0]['complete'])
        self.assertEqual(messages[0]['raw_len'], 300)
        self.assertEqual(messages[0]['code'], 95)
        self.assertEqual(messages[0]['field1'], -4)

    def test_cli_trace(self) -> None:
        _, data = encode_event(95)
        with tempfile.TemporaryDirectory(prefix='vm-dm-trace-') as tmp:
            path = Path(tmp) / 'dm.strace'
            path.write_text(self.line(991, 'msgrcv', OEM_EVENT_TYPE, data))
            completed = subprocess.run(
                [sys.executable, '-B', str(VM / 'launch.py'),
                 'dmengine-queue', 'trace', '--file', str(path)],
                cwd=VM, capture_output=True, text=True, check=False)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            report = json.loads(completed.stdout)
            self.assertEqual(report['count'], 1)
            self.assertEqual(report['messages'][0]['code'], 95)

if __name__ == '__main__':
    unittest.main()
