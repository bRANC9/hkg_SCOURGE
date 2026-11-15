# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Inspect and validate a RedBend RDM tree without exposing credentials.

The captured ``tree.xml`` contains DM server addresses, account names and
SyncML auth secrets.  Reports from this module expose structural facts and
FUMO state only; leaf values are never printed and account paths are replaced
with positional placeholders in diffs.
"""
from __future__ import annotations

import hashlib
import re
import xml.etree.ElementTree as ET
from pathlib import Path

MAX_TREE_BYTES = 2 * 1024 * 1024
FUMO_TYPE = 'urn:oma:mo:oma-fumo:1.0'
AUTH_FIELDS = ('AAuthName', 'AAuthType', 'AAuthLevel', 'AAuthSecret', 'AAuthData')
AUTH_ROLES = ('APPSRV', 'CLIENT')
EXPECTED_LEVELS = {'APPSRV': 'SRVCRED', 'CLIENT': 'CLCRED'}
NUMBER = re.compile(r'^\d+$')


class TreeError(ValueError):
    """The tree cannot be read or parsed safely."""


def _local(tag: str) -> str:
    return tag.rsplit('}', 1)[-1]


def _child(elem: ET.Element, name: str) -> ET.Element | None:
    for child in elem:
        if _local(child.tag) == name:
            return child
    return None


def _children(elem: ET.Element, name: str) -> list[ET.Element]:
    return [child for child in elem if _local(child.tag) == name]


def _text(elem: ET.Element, name: str) -> str | None:
    child = _child(elem, name)
    if child is None or child.text is None:
        return None
    return child.text.strip()


def _display(path: tuple[str, ...]) -> str:
    parts = list(path[1:] if path and path[0] == '.' else path)
    if 'DMAcc' in parts:
        index = parts.index('DMAcc')
        if index + 1 < len(parts):
            parts[index + 1] = '<account>'
    return '/'.join(parts)


def _read_tree(path: Path) -> tuple[str, bytes]:
    data = Path(path).read_bytes()
    if len(data) > MAX_TREE_BYTES:
        raise TreeError(f'tree is larger than {MAX_TREE_BYTES} bytes')
    if re.search(rb'<!DOCTYPE|<!ENTITY', data, re.IGNORECASE):
        raise TreeError('DTD or entity declarations are not allowed')
    return data.decode('utf-8', errors='replace'), data


def parse_tree_text(text: str) -> dict:
    """Parse RDM tree text and return structural facts without leaf values."""
    problems: list[str] = []
    warnings: list[str] = []
    try:
        root = ET.fromstring(text)
    except ET.ParseError as exc:
        return {'version': None, 'nodes': [], 'leaves': [], 'fumo': None,
                'accounts': {}, 'problems': [f'XML parse error: {exc}'],
                'warnings': warnings}
    if _local(root.tag) != 'rdmtree':
        problems.append(f'root element is {_local(root.tag)!r}, expected rdmtree')
    version = _text(root, 'version')
    if not version:
        problems.append('tree version is missing')

    nodes: list[dict] = []
    leaves: list[tuple[tuple[str, ...], str | None]] = []

    def walk(parent: ET.Element, prefix: tuple[str, ...]) -> None:
        for node in _children(parent, 'node'):
            name = _text(node, 'name')
            if not name:
                problems.append(f'node under {_display(prefix) or "/"} has no name')
                continue
            path = prefix + (name,)
            record = {'path': path, 'type': _text(node, 'type'), 'leaves': {}}
            for leaf in _children(node, 'leaf'):
                leaf_name = _text(leaf, 'name')
                if not leaf_name:
                    problems.append(f'leaf under {_display(path)} has no name')
                    continue
                value = _text(leaf, 'value')
                if leaf_name in record['leaves']:
                    problems.append(f'duplicate leaf {_display(path)}/{leaf_name}')
                    continue
                record['leaves'][leaf_name] = value
                leaves.append((path + (leaf_name,), value))
            nodes.append(record)
            walk(node, path)

    walk(root, ())

    fumo_nodes = [node for node in nodes
                  if node['path'] and node['path'][-1] == 'FUMO'
                  and node['type'] == FUMO_TYPE]
    fumo = None
    if len(fumo_nodes) != 1:
        problems.append(f'expected exactly one {FUMO_TYPE} FUMO node, '
                        f'found {len(fumo_nodes)}')
    else:
        fumo_path = fumo_nodes[0]['path']
        fumo_leaves = {path[-1]: value for path, value in leaves
                       if path[:len(fumo_path)] == fumo_path}
        state_text = fumo_leaves.get('State')
        state = None
        if state_text is None:
            problems.append('FUMO State leaf is missing')
        elif not NUMBER.match(state_text):
            problems.append('FUMO State is not an integer')
        else:
            state = int(state_text)
            if not 0 <= state <= 255:
                problems.append(f'FUMO State out of range: {state}')
        pkg_version = fumo_leaves.get('PkgVersion')
        if not pkg_version:
            problems.append('FUMO PkgVersion leaf is missing')
        pkg_url_present = any(value for path, value in leaves
                              if path[:len(fumo_path)] == fumo_path
                              and path[-1] == 'PkgURL')
        if not any(path[-1] == 'PkgURL' and path[:len(fumo_path)] == fumo_path
                   for path, _ in leaves):
            problems.append('FUMO PkgURL leaf is missing')
        fumo = {'state': state, 'pkg_version': pkg_version,
                'pkg_url_present': pkg_url_present}

    accounts: dict[str, dict] = {}
    for path, value in leaves:
        if 'DMAcc' not in path:
            continue
        index = path.index('DMAcc')
        if len(path) < index + 5 or path[index + 2] != 'AppAuth':
            continue
        account = path[index + 1]
        role = path[index + 3]
        field = path[index + 4]
        if role not in AUTH_ROLES or field not in AUTH_FIELDS:
            continue
        accounts.setdefault(account, {}).setdefault(role, {})[field] = value
    if not accounts:
        problems.append('no DMAcc account with AppAuth credentials found')
    auth_types: set[str] = set()
    auth_levels: set[str] = set()
    for account, roles in accounts.items():
        for role in AUTH_ROLES:
            fields = roles.get(role, {})
            missing = [field for field in AUTH_FIELDS if field not in fields]
            if missing:
                problems.append(f'DM account {role} auth is missing: '
                                + ', '.join(missing))
            if fields.get('AAuthType'):
                auth_types.add(fields['AAuthType'])
                if fields['AAuthType'] != 'syncml:auth-md5':
                    warnings.append(f'{role} auth type is not syncml:auth-md5')
            if fields.get('AAuthLevel'):
                auth_levels.add(fields['AAuthLevel'])
                expected = EXPECTED_LEVELS[role]
                if fields['AAuthLevel'] != expected:
                    warnings.append(f'{role} auth level is not {expected}')
    port_paths = [path for path, _ in leaves
                  if 'DMAcc' in path and 'AppAddr' in path
                  and path[-1] == 'PortNbr']
    if not port_paths:
        problems.append('no DMAcc AppAddr PortNbr leaf found')
    for path, value in leaves:
        if path in port_paths and (not value or not NUMBER.match(value)
                                    or not 1 <= int(value) <= 65535):
            problems.append('DMAcc PortNbr is not a valid TCP port')

    return {'version': version, 'nodes': nodes, 'leaves': leaves,
            'fumo': fumo, 'accounts': accounts,
            'auth_types': sorted(auth_types), 'auth_levels': sorted(auth_levels),
            'problems': problems, 'warnings': warnings}


def check_dm_tree(path: Path) -> dict:
    """Validate a tree file and return a credential-free report."""
    source = Path(path)
    report = {'ok': False, 'source': str(source), 'sha256': None, 'size': None,
              'version': None, 'nodes': 0, 'leaves': 0, 'fumo': None,
              'accounts': 0, 'auth_types': [], 'auth_levels': [],
              'problems': [], 'warnings': []}
    try:
        text, data = _read_tree(source)
    except (OSError, TreeError) as exc:
        report['problems'].append(str(exc))
        return report
    parsed = parse_tree_text(text)
    report.update({
        'sha256': hashlib.sha256(data).hexdigest(),
        'size': len(data),
        'version': parsed['version'],
        'nodes': len(parsed['nodes']),
        'leaves': len(parsed['leaves']),
        'fumo': parsed['fumo'],
        'accounts': len(parsed['accounts']),
        'auth_types': parsed.get('auth_types', []),
        'auth_levels': parsed.get('auth_levels', []),
        'problems': parsed['problems'],
        'warnings': parsed['warnings'],
    })
    report['ok'] = not parsed['problems']
    return report


def _redacted_paths(tree: dict) -> dict[str, str | None]:
    accounts = sorted(tree['accounts'])
    placeholders = {name: f'<account{index + 1}>'
                    for index, name in enumerate(accounts)}
    result: dict[str, str | None] = {}
    for path, value in tree['leaves']:
        parts = list(path)
        if 'DMAcc' in parts:
            index = parts.index('DMAcc')
            if index + 1 < len(parts):
                parts[index + 1] = placeholders.get(parts[index + 1], '<account>')
        result['/'.join(parts[1:] if parts and parts[0] == '.' else parts)] = value
    return result


def diff_trees(left: Path, right: Path) -> dict:
    """Compare two trees by leaf path and value; never print values or accounts."""
    ltree = check_dm_tree(left)
    rtree = check_dm_tree(right)
    reports = [ltree, rtree]
    problems = [problem for report in reports for problem in report['problems']]
    if problems:
        return {'ok': False, 'identical': False, 'changed': [], 'added': [],
                'removed': [], 'problems': problems,
                'warnings': [warning for report in reports for warning in report['warnings']]}
    lparsed = parse_tree_text(_read_tree(Path(left))[0])
    rparsed = parse_tree_text(_read_tree(Path(right))[0])
    left_map = _redacted_paths(lparsed)
    right_map = _redacted_paths(rparsed)
    changed = sorted(path for path in left_map.keys() & right_map.keys()
                     if left_map[path] != right_map[path])
    added = sorted(right_map.keys() - left_map.keys())
    removed = sorted(left_map.keys() - right_map.keys())
    return {'ok': True, 'identical': not (changed or added or removed),
            'changed': changed, 'added': added, 'removed': removed,
            'problems': [],
            'warnings': ltree['warnings'] + rtree['warnings']}
