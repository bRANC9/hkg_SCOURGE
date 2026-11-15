# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""RedBend EF delivery-package container helpers.

The vendor dmengine downloads a RedBend EF DP object after resolving an OMA
Download Descriptor.  The container starts with a 20-byte
``RedBendEFDPackage`` preamble and a 32-bit header.  This module builds and
validates the version-4 layout observed in the engine:

* 20-byte preamble
* u32 version (4)
* u32 component count
* u32 DP size
* u32 delta-table offset
* u32 scripts-table offset
* u32 reserved
* u32 component-table offset
* u32 string-table offset

Each component entry is 28 bytes: four absolute string offsets, the installer
type, an update count and an update-table offset.  Each update entry is a u32
payload offset and a u32 payload size; the payload bytes follow the string
table.  Strings are NUL-terminated ASCII.  The builder is deliberately strict:
it produces a structural lab fixture, not a signed or installable firmware
package.
"""
from __future__ import annotations

import hashlib
import hmac
import struct
from dataclasses import dataclass
from pathlib import Path

PREAMBLE = b'RedBendEFDPackage\0\0\0'
VERSION = 4
HEADER_SIZE = 52
ENTRY_SIZE = 28
MAX_COMPONENTS = 64
MAX_STRING = 255
INSTALLER_TYPES = (0, 1, 2, 3)
SIG_MAGIC = b'RedBendEFDSig\0\0\0'
SIG_LENGTH = 32
SIG_TRAILER = len(SIG_MAGIC) + 4 + SIG_LENGTH


class DpError(ValueError):
    """Invalid RedBend EF DP container."""


@dataclass(frozen=True)
class DpUpdate:
    data: bytes

    def validate(self) -> None:
        if not self.data:
            raise DpError('an update payload must not be empty')
        if len(self.data) > 0xffffffff:
            raise DpError('an update payload is too large')


@dataclass(frozen=True)
class DpComponent:
    name: str
    component_id: str
    version: str = '1.0'
    vendor: str = 'LAB'
    installer_type: int = 0
    updates: tuple[DpUpdate, ...] = ()

    def validate(self) -> None:
        for label, value in (('name', self.name),
                             ('component_id', self.component_id),
                             ('version', self.version),
                             ('vendor', self.vendor)):
            if not value or len(value.encode('ascii', 'strict')) > MAX_STRING:
                raise DpError(f'{label} must be 1..{MAX_STRING} ASCII bytes')
            if '\0' in value:
                raise DpError(f'{label} must not contain NUL')
        if self.installer_type not in INSTALLER_TYPES:
            raise DpError(f'installer_type must be one of {INSTALLER_TYPES}')
        for update in self.updates:
            update.validate()


def _put_u32(data: bytearray, offset: int, value: int) -> None:
    struct.pack_into('<I', data, offset, value)


def _get_u32(data: bytes, offset: int, label: str) -> int:
    if offset + 4 > len(data):
        raise DpError(f'{label} is outside the container')
    return struct.unpack_from('<I', data, offset)[0]


def sign_dp(body: bytes, key: bytes) -> bytes:
    """Append a lab HMAC-SHA256 signature trailer (not a vendor signature)."""
    if not key:
        raise DpError('a signing key must not be empty')
    digest = hmac.new(key, body, hashlib.sha256).digest()
    return body + SIG_MAGIC + struct.pack('<I', len(digest)) + digest


def build_dp(components: list[DpComponent] | tuple[DpComponent, ...],
             signing_key: bytes | None = None) -> bytes:
    """Build a version-4 DP with component/update tables and optional signature."""
    if not components:
        raise DpError('a DP needs at least one component')
    if len(components) > MAX_COMPONENTS:
        raise DpError(f'at most {MAX_COMPONENTS} components are supported')
    for component in components:
        component.validate()

    entry_base = HEADER_SIZE
    update_base = entry_base + ENTRY_SIZE * len(components)
    update_count = sum(len(component.updates) for component in components)
    string_base = update_base + 8 * update_count
    strings = bytearray()
    entries: list[tuple[int, int, int, int, int, int, int]] = []
    update_index = 0
    for component in components:
        offsets = []
        for value in (component.name, component.component_id,
                      component.version, component.vendor):
            offsets.append(string_base + len(strings))
            strings.extend(value.encode('ascii') + b'\0')
        entries.append((*offsets, component.installer_type,
                        len(component.updates),
                        update_base + update_index * 8
                        if component.updates else 0))
        update_index += len(component.updates)

    payload_base = string_base + len(strings)
    payloads = bytearray()
    updates: list[tuple[int, int]] = []
    for component in components:
        for update in component.updates:
            updates.append((payload_base + len(payloads), len(update.data)))
            payloads.extend(update.data)
    total = payload_base + len(payloads)
    data = bytearray(total)
    data[:len(PREAMBLE)] = PREAMBLE
    _put_u32(data, 20, VERSION)
    _put_u32(data, 24, len(components))
    _put_u32(data, 28, total)
    _put_u32(data, 32, 0)          # no delta table
    _put_u32(data, 36, 0)          # no scripts table
    _put_u32(data, 40, 0)
    _put_u32(data, 44, entry_base)
    _put_u32(data, 48, string_base)
    for index, entry in enumerate(entries):
        struct.pack_into('<7I', data, entry_base + index * ENTRY_SIZE, *entry)
    for index, (offset, size) in enumerate(updates):
        struct.pack_into('<2I', data, update_base + index * 8, offset, size)
    data[string_base:string_base + len(strings)] = strings
    data[payload_base:] = payloads
    result = bytes(data)
    return sign_dp(result, signing_key) if signing_key is not None else result


def parse_dp(data: bytes, key: bytes | None = None) -> dict:
    """Parse and validate a version-4 DP container."""
    data = bytes(data)
    problems: list[str] = []
    report: dict = {'ok': False, 'size': len(data), 'body_size': len(data),
                    'version': None, 'components': [], 'signature': None,
                    'problems': problems}
    if len(data) < HEADER_SIZE:
        problems.append(f'container is shorter than {HEADER_SIZE} bytes')
        return report
    body_length = len(data)
    signature = None
    if len(data) >= SIG_TRAILER and data[-SIG_TRAILER:-SIG_TRAILER + len(SIG_MAGIC)] == SIG_MAGIC:
        stored_length = struct.unpack_from('<I', data, len(data) - SIG_LENGTH - 4)[0]
        if stored_length != SIG_LENGTH:
            problems.append(f'invalid signature length {stored_length}')
            return report
        body_length = len(data) - SIG_TRAILER
        signature = {'present': True, 'algorithm': 'hmac-sha256',
                     'valid': None}
        if key is not None:
            expected = hmac.new(key, data[:body_length], hashlib.sha256).digest()
            signature['valid'] = hmac.compare_digest(
                expected, data[-SIG_LENGTH:])
            if not signature['valid']:
                problems.append('signature mismatch')
        report['signature'] = signature
        report['body_size'] = body_length
    if data[:len(PREAMBLE)] != PREAMBLE:
        problems.append('preamble is not RedBendEFDPackage')
        return report
    version = _get_u32(data, 20, 'version')
    count = _get_u32(data, 24, 'component count')
    declared_size = _get_u32(data, 28, 'DP size')
    delta_ptr = _get_u32(data, 32, 'delta pointer')
    scripts_ptr = _get_u32(data, 36, 'scripts pointer')
    component_ptr = _get_u32(data, 44, 'component pointer')
    string_ptr = _get_u32(data, 48, 'string pointer')
    report['version'] = version
    if version != VERSION:
        problems.append(f'unsupported DP version {version} (expected {VERSION})')
    if count == 0 or count > MAX_COMPONENTS:
        problems.append(f'invalid component count {count}')
    if declared_size != body_length:
        problems.append(f'declared DP size {declared_size} != actual body {body_length}')
    if delta_ptr and delta_ptr >= body_length:
        problems.append('delta pointer is outside the container')
    if scripts_ptr and scripts_ptr >= body_length:
        problems.append('scripts pointer is outside the container')
    if component_ptr + ENTRY_SIZE * count > body_length:
        problems.append('component table is outside the container')
    if string_ptr >= body_length:
        problems.append('string table is outside the container')
    if problems:
        return report

    def read_string(offset: int, label: str) -> str:
        if offset >= body_length or offset < string_ptr:
            raise DpError(f'{label} offset {offset} is outside the string table')
        end = data.find(b'\0', offset)
        if end < 0:
            raise DpError(f'{label} is not NUL-terminated')
        raw = data[offset:end]
        try:
            return raw.decode('ascii')
        except UnicodeDecodeError as exc:
            raise DpError(f'{label} is not ASCII') from exc

    components = []
    try:
        for index in range(count):
            base = component_ptr + index * ENTRY_SIZE
            name = read_string(_get_u32(data, base, 'name offset'), 'name')
            component_id = read_string(_get_u32(data, base + 4, 'id offset'), 'id')
            version_text = read_string(_get_u32(data, base + 8, 'version offset'),
                                       'component version')
            vendor = read_string(_get_u32(data, base + 12, 'vendor offset'),
                                 'vendor')
            installer_type = _get_u32(data, base + 16, 'installer type')
            update_count = _get_u32(data, base + 20, 'update count')
            update_ptr = _get_u32(data, base + 24, 'update table pointer')
            if installer_type not in INSTALLER_TYPES:
                raise DpError(f'component {index} has invalid installer type '
                              f'{installer_type}')
            if update_count and update_ptr + update_count * 8 > body_length:
                raise DpError(f'component {index} update table is outside the container')
            updates = []
            for update_index in range(update_count):
                entry = update_ptr + update_index * 8
                offset = _get_u32(data, entry, 'update payload offset')
                size = _get_u32(data, entry + 4, 'update payload size')
                if size == 0 or offset + size > body_length:
                    raise DpError(f'component {index} update payload is outside '
                                  'the container')
                updates.append({'offset': offset, 'size': size})
            components.append({
                'name': name,
                'id': component_id,
                'version': version_text,
                'vendor': vendor,
                'installer_type': installer_type,
                'update_count': update_count,
                'updates': updates,
            })
    except DpError as exc:
        problems.append(str(exc))
        return report
    report['components'] = components
    report['ok'] = not problems
    return report


def check_dp(path: Path, key: bytes | None = None) -> dict:
    """Validate a DP file and return a JSON-ready report."""
    source = Path(path)
    try:
        data = source.read_bytes()
    except OSError as exc:
        return {'ok': False, 'source': str(source), 'size': None,
                'version': None, 'components': [], 'signature': None,
                'problems': [str(exc)]}
    report = parse_dp(data, key)
    report['source'] = str(source)
    return report
