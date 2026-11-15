# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Audit the vendor /update state without exposing URLs or credentials.

Reads only small metadata files from a directory or raw ext4 image: the
installed version, FUMO USB timing, install parameters, DM state and the RDM
tree's FUMO state.  It never opens update payloads, decrypts packages or
prints signed URLs, server addresses or credentials.
"""
from __future__ import annotations

import re
from pathlib import Path

from .dm_tree import parse_tree_text
from .update_media import MediaError, open_media

VERSION = re.compile(r'^[A-Z0-9_]+\.[A-Z0-9_]+\.[A-Z0-9_]+\.\d{3}\.\d{3}\.\d{6}$')
DATE = re.compile(r'^\d{4}\.\d{2}\.\d{2}$')
TIMESTAMP = re.compile(r'^\d{14}$')
INTEGER = re.compile(r'^-?\d+$')
INSTALL_KEYS = ('targetVersion', 'nextCountFumo', 'nextCountScomo',
                'installTime', 'execCommand')


def parse_version(value: str | None, label: str) -> tuple[str | None, list[str]]:
    if value is None:
        return None, [f'{label} is missing']
    value = value.strip()
    if not VERSION.match(value):
        return None, [f'{label} has an unexpected format']
    return value, []


def parse_fumo_time(value: str | None) -> dict:
    result = {'timestamp': None, 'previous': None, 'current': None,
              'problems': []}
    if value is None:
        result['problems'].append('FumoUsbUpdateTimeVersion is missing')
        return result
    parts = value.strip().split('^')
    if len(parts) != 3:
        result['problems'].append('FumoUsbUpdateTimeVersion must have 3 fields')
        return result
    timestamp, previous, current = parts
    if not TIMESTAMP.match(timestamp):
        result['problems'].append('FumoUsbUpdateTimeVersion timestamp is not 14 digits')
    else:
        result['timestamp'] = timestamp
    result['previous'], problems = parse_version(previous, 'FUMO previous version')
    result['problems'].extend(problems)
    result['current'], problems = parse_version(current, 'FUMO current version')
    result['problems'].extend(problems)
    return result


def parse_install(value: str | None) -> dict:
    result = {key: None for key in INSTALL_KEYS}
    result['problems'] = []
    if value is None:
        result['problems'].append('fota/install_param.txt is missing')
        return result
    value = value.strip()
    if not value.startswith('ui:user-interaction/'):
        result['problems'].append('install_param must start with ui:user-interaction/')
        return result
    fields: dict[str, str] = {}
    for item in value.split('/')[1:]:
        key, separator, field_value = item.partition(':')
        if not separator or not key:
            result['problems'].append('install_param has a malformed field')
            continue
        if key in fields:
            result['problems'].append(f'install_param duplicates {key}')
        fields[key] = field_value
    for key in INSTALL_KEYS:
        if key not in fields:
            result['problems'].append(f'install_param is missing {key}')
            continue
        field_value = fields[key]
        if key == 'targetVersion':
            result[key], problems = parse_version(field_value, 'install targetVersion')
            result['problems'].extend(problems)
        elif key in ('nextCountFumo', 'nextCountScomo', 'installTime'):
            if not INTEGER.match(field_value):
                result['problems'].append(f'install_param {key} is not an integer')
            else:
                result[key] = int(field_value)
        else:
            if not field_value:
                result['problems'].append(f'install_param {key} is empty')
            result[key] = field_value
    return result


def check_update_state(path: Path) -> dict:
    source = Path(path)
    report = {'ok': False, 'source': str(source), 'mode': None,
              'version': None, 'fumo_time': None, 'install': None,
              'dm_state': None, 'fumo_state': None,
              'state_consistent': None, 'scheduled_version': None,
              'scheduled_date': None, 'package_blob': None,
              'problems': [], 'warnings': []}
    try:
        media = open_media(source)
    except (MediaError, OSError) as exc:
        report['problems'].append(str(exc))
        return report
    report['mode'] = media.mode
    problems: list[str] = report['problems']
    warnings: list[str] = report['warnings']

    def text(name: str) -> str | None:
        try:
            return media.read_text(name)
        except (MediaError, OSError) as exc:
            problems.append(f'{name}: {exc}')
            return None

    version, version_problems = parse_version(text('updateVersionInfo'),
                                              'updateVersionInfo')
    report['version'] = version
    problems.extend(version_problems)

    fumo = parse_fumo_time(text('FumoUsbUpdateTimeVersion'))
    report['fumo_time'] = {key: fumo[key] for key in
                           ('timestamp', 'previous', 'current')}
    problems.extend(fumo['problems'])

    install = parse_install(text('fota/install_param.txt'))
    report['install'] = {key: install[key] for key in INSTALL_KEYS}
    problems.extend(install['problems'])

    dm_state = text('fota/dm_state.txt')
    if dm_state is None:
        problems.append('fota/dm_state.txt is missing')
    else:
        dm_state = dm_state.strip()
        report['dm_state'] = dm_state
        if dm_state not in ('0', '1'):
            problems.append('fota/dm_state.txt must be 0 or 1')

    scheduled_raw = text('scheduledUpdateVersion')
    scheduled_version, problems_ = parse_version(
        scheduled_raw, 'scheduledUpdateVersion')
    if scheduled_raw is not None:
        problems.extend(problems_)
    report['scheduled_version'] = scheduled_version
    scheduled_date = text('scheduledUpdateDate')
    if scheduled_date is not None:
        scheduled_date = scheduled_date.strip()
        report['scheduled_date'] = scheduled_date
        if not DATE.match(scheduled_date):
            problems.append('scheduledUpdateDate must be YYYY.MM.DD')

    if fumo['current'] is not None and scheduled_version is not None \
            and fumo['current'] != scheduled_version:
        problems.append('FUMO current version does not match scheduledUpdateVersion')

    tree_text = text('fota/dme/tree.xml')
    if tree_text is not None:
        tree = parse_tree_text(tree_text)
        problems.extend(f'tree: {problem}' for problem in tree['problems'])
        warnings.extend(f'tree: {warning}' for warning in tree['warnings'])
        if tree.get('fumo'):
            report['fumo_state'] = tree['fumo']['state']
    if report['fumo_state'] is not None and report['dm_state'] in ('0', '1'):
        idle = report['fumo_state'] == 10
        report['state_consistent'] = idle == (report['dm_state'] == '0')
        if not report['state_consistent']:
            warnings.append('dm_state/tree FUMO state disagree')

    blob = text('usb/bin/.lge.upgrade.xml')
    if blob is None:
        report['package_blob'] = 'missing'
    else:
        report['package_blob'] = 'present'
        if not blob:
            problems.append('usb/bin/.lge.upgrade.xml is empty')
        elif blob.startswith('<'):
            problems.append('usb/bin/.lge.upgrade.xml is plaintext XML')

    signature = text('package.tar.sign')
    if signature is not None and not signature.strip():
        problems.append('package.tar.sign is empty')

    report['ok'] = not problems
    return report
