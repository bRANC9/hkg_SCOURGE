# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Codec for the vendor dmengine <-> OEM handler SysV message queue.

The captured queue uses key 0xd54. dmengine sends OEM events with mtype 0x48
and waits for commands with mtype 0x36. The 1048-byte command payload starts
with an int32 event code, two 64-bit fields at offsets 8 and 16, and a string
at offset 32. This module encodes/decodes that layout; it does not implement
the AppDMClient command semantics.
"""
from __future__ import annotations

import re
import struct

QUEUE_KEY = 0xD54
OEM_EVENT_TYPE = 0x48
OEM_COMMAND_TYPE = 0x36
DATA_SIZE = 1048
TEXT_OFFSET = 32
MAX_TEXT = DATA_SIZE - TEXT_OFFSET


class QueueError(ValueError):
    """Malformed queue message or field."""


def decode_payload(data: bytes) -> dict:
    if len(data) != DATA_SIZE:
        raise QueueError(f'payload must be {DATA_SIZE} bytes, got {len(data)}')
    code = struct.unpack_from('<i', data, 0)[0]
    field1 = struct.unpack_from('<q', data, 8)[0]
    field2 = struct.unpack_from('<q', data, 16)[0]
    raw = data[TEXT_OFFSET:]
    end = raw.find(b'\0')
    if end < 0:
        end = len(raw)
    text = raw[:end].decode('utf-8', errors='replace')
    return {'code': code, 'field1': field1, 'field2': field2,
            'text': text, 'text_offset': TEXT_OFFSET}


def encode_payload(code: int, field1: int = 0, field2: int = 0,
                   text: str = '') -> bytes:
    for label, value in (('code', code), ('field1', field1),
                         ('field2', field2)):
        if isinstance(value, bool) or not isinstance(value, int):
            raise QueueError(f'{label} must be an integer')
    if not -(2 ** 31) <= code < 2 ** 31:
        raise QueueError('code is outside int32')
    if not -(2 ** 63) <= field1 < 2 ** 63 \
            or not -(2 ** 63) <= field2 < 2 ** 63:
        raise QueueError('field is outside int64')
    if not isinstance(text, str):
        raise QueueError('text must be a string')
    encoded = text.encode('utf-8')
    if len(encoded) >= MAX_TEXT:
        raise QueueError(f'text is too long (max {MAX_TEXT - 1} bytes)')
    data = bytearray(DATA_SIZE)
    struct.pack_into('<i', data, 0, code)
    struct.pack_into('<q', data, 8, field1)
    struct.pack_into('<q', data, 16, field2)
    data[TEXT_OFFSET:TEXT_OFFSET + len(encoded)] = encoded
    return bytes(data)


def encode_command(code: int, field1: int = 0, field2: int = 0,
                   text: str = '') -> tuple[int, bytes]:
    return OEM_COMMAND_TYPE, encode_payload(code, field1, field2, text)


def encode_event(code: int, field1: int = 0, field2: int = 0,
                 text: str = '') -> tuple[int, bytes]:
    return OEM_EVENT_TYPE, encode_payload(code, field1, field2, text)


_STRACE = re.compile(
    r'(?m)^\s*(?P<pid>\d+)\s+(?P<call>msgsnd|msgrcv)\('
    r'(?P<qid>\d+),.*?\{\s*(?P<mtype>\d+),\s*"'
    r'(?P<data>(?:\\x[0-9a-fA-F]{2}|\\.|[^"\\])*)"', re.S)


def _unescape(value: str) -> bytes:
    out = bytearray()
    index = 0
    while index < len(value):
        if value.startswith('\\x', index) and index + 3 < len(value):
            out.append(int(value[index + 2:index + 4], 16))
            index += 4
        elif value[index] == '\\' and index + 1 < len(value):
            out.append({'n': 10, 't': 9, 'r': 13, '\\': 92, '"': 34}
                       .get(value[index + 1], ord(value[index + 1])))
            index += 2
        else:
            out.append(ord(value[index]))
            index += 1
    return bytes(out)


def parse_strace(text: str) -> list[dict]:
    """Parse complete msgsnd/msgrcv message lines from an strace -xx log."""
    messages = []
    for match in _STRACE.finditer(text):
        data = _unescape(match.group('data'))
        mtype = int(match.group('mtype'))
        if mtype not in (OEM_COMMAND_TYPE, OEM_EVENT_TYPE):
            continue
        message = {'pid': int(match.group('pid')),
                   'call': match.group('call'),
                   'queue': int(match.group('qid')),
                   'mtype': mtype,
                   'kind': ('command' if mtype == OEM_COMMAND_TYPE
                            else 'event'),
                   'raw_len': len(data),
                   'complete': len(data) == DATA_SIZE}
        if len(data) >= 24:
            message.update({'code': struct.unpack_from('<i', data, 0)[0],
                            'field1': struct.unpack_from('<q', data, 8)[0],
                            'field2': struct.unpack_from('<q', data, 16)[0]})
            raw = data[TEXT_OFFSET:]
            end = raw.find(b'\0')
            if end < 0:
                end = len(raw)
            message['text'] = raw[:end].decode('utf-8', errors='replace')
        messages.append(message)
    return messages


def decode_message(mtype: int, data: bytes) -> dict:
    if mtype not in (OEM_COMMAND_TYPE, OEM_EVENT_TYPE):
        raise QueueError(f'unknown mtype: {mtype:#x}')
    return {'mtype': mtype,
            'kind': 'command' if mtype == OEM_COMMAND_TYPE else 'event',
            **decode_payload(data)}
