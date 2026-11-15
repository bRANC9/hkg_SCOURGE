# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Decode and build lab MOF/FUMO notification URIs.

The captured notification is ``/dm/message?msgtype=4&cor=<base64>`` where the
base64 payload is a pipe/caret-delimited device and version record containing
a VIN.  Reports expose only structural fields and the lab VIN/version values;
the server host is never printed.  This is a codec, not the vendor MOF client.
"""
from __future__ import annotations

import base64
import binascii
import re
from urllib.parse import parse_qs, urlsplit

VERSION = re.compile(r'^[A-Z0-9_]+\.[A-Z0-9_]+\.[A-Z0-9_]+\.\d{3}\.\d{3}\.\d{6}$')
VIN = re.compile(r'^VIN:[A-Z0-9]{17}_AVN$')
UPDATE_ID = re.compile(r'^\d+$')
FIELD = re.compile(r'^[^^|\s]+$')


class MofError(ValueError):
    """Malformed MOF notification or lab field."""


def _b64decode(value: str) -> str:
    try:
        data = base64.b64decode(value + '=' * (-len(value) % 4), validate=True)
    except (binascii.Error, ValueError) as exc:
        raise MofError('cor is not valid base64') from exc
    try:
        return data.decode('utf-8')
    except UnicodeDecodeError as exc:
        raise MofError('cor payload is not UTF-8') from exc


def _b64encode(value: str) -> str:
    return base64.b64encode(value.encode()).decode().rstrip('=')


def decode_notification(uri: str) -> dict:
    if not isinstance(uri, str) or not uri:
        raise MofError('notification URI is empty')
    parsed = urlsplit(uri)
    if parsed.scheme not in ('http', 'https'):
        raise MofError('notification URI must be http(s)')
    if not parsed.hostname:
        raise MofError('notification URI has no host')
    if parsed.path != '/dm/message':
        raise MofError('notification path must be /dm/message')
    query = parse_qs(parsed.query, keep_blank_values=True)
    msgtype = query.get('msgtype', [''])[-1]
    if msgtype != '4':
        raise MofError('notification msgtype must be 4')
    cor = query.get('cor', [''])[-1]
    if not cor:
        raise MofError('notification cor is missing')
    text = _b64decode(cor)
    parts = text.split('|')
    if len(parts) != 8:
        raise MofError('cor payload must have 8 pipe-delimited fields')
    brand, model, region, year, market, field1, field2, tail = parts
    if brand != 'LGE':
        raise MofError('cor brand must be LGE')
    for label, value in (('model', model), ('region', region), ('year', year),
                         ('market', market)):
        if not FIELD.match(value):
            raise MofError(f'cor {label} is invalid')
    items = tail.split('^')
    vin_index = next((index for index, item in enumerate(items)
                      if item.startswith('VIN:')), None)
    if vin_index is None:
        raise MofError('cor payload has no VIN field')
    vin = items[vin_index]
    if not VIN.match(vin):
        raise MofError('cor VIN must be VIN:<17 chars>_AVN')
    after = items[vin_index + 1:]
    if len(after) < 8:
        raise MofError('cor payload is missing version/update fields')
    previous, next_version, update_id, client = after[4], after[5], after[6], after[7]
    if not VERSION.match(previous):
        raise MofError('cor previous version has an unexpected format')
    if not VERSION.match(next_version):
        raise MofError('cor next version has an unexpected format')
    if not UPDATE_ID.match(update_id):
        raise MofError('cor update id is not numeric')
    if not FIELD.match(client):
        raise MofError('cor client field is invalid')
    return {'msgtype': 4, 'brand': brand, 'model': model, 'region': region,
            'year': year, 'market': market, 'vin': vin,
            'previous': previous, 'next': next_version,
            'update_id': update_id, 'client': client,
            'host_present': True}


def build_notification(vin: str, previous: str, next_version: str,
                       update_id: str, host: str = '127.0.0.1:8002',
                       model: str = 'AVN5.0_STW', region: str = 'NE_E',
                       year: str = '2021', market: str = 'EUR') -> str:
    if not VIN.match(vin):
        raise MofError('VIN must be VIN:<17 chars>_AVN')
    for label, value in (('previous', previous), ('next', next_version)):
        if not VERSION.match(value):
            raise MofError(f'{label} version has an unexpected format')
    if not UPDATE_ID.match(update_id):
        raise MofError('update id must be numeric')
    if not host or any(char in host for char in '/?#@'):
        raise MofError('host must be host[:port] without a path')
    for label, value in (('model', model), ('region', region),
                         ('year', year), ('market', market)):
        if not FIELD.match(value):
            raise MofError(f'{label} is invalid')
    text = '|'.join(('LGE', model, region, year, market, '***', '***',
                     f'***^{vin}^75^100^null^null^{previous}^{next_version}'
                     f'^{update_id}^CLIENT'))
    return (f'http://{host}/dm/message?msgtype=4&cor={_b64encode(text)}')
