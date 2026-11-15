# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Validate a car ground-truth capture directory.

A capture is intentionally outside the tracked tree.  This module only
inspects names, sizes and hashes; it never extracts or executes target data.
"""
from __future__ import annotations

import hashlib
from pathlib import Path

SUBDIRS = ('archives', 'partitions', 'runtime', 'meta', 'critical')
REQUIRED_FILES = (
    'archives/rootfs.tar',
    'critical/critical.tar',
    'meta/SOURCE.txt',
    'meta/SHA256SUMS',
)
HEX = frozenset('0123456789abcdef')


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with Path(path).open('rb') as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def _normalize(rel: str) -> str:
    rel = rel[2:] if rel.startswith('./') else rel
    if rel.startswith('/'):
        parts = rel.split('/')
        for index, part in enumerate(parts):
            if part.startswith('car-dump-'):
                return '/'.join(parts[index + 1:])
    return rel


def parse_sums(path: Path) -> tuple[list[tuple[str, str]], list[str]]:
    """Parse sha256sum output; return entries and malformed-line problems."""
    entries: list[tuple[str, str]] = []
    problems: list[str] = []
    try:
        lines = Path(path).read_text(errors='replace').splitlines()
    except OSError as exc:
        return [], [f'cannot read {path.name}: {exc}']
    for lineno, line in enumerate(lines, 1):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        parts = line.split(None, 1)
        if (len(parts) != 2 or len(parts[0]) != 64
                or any(char not in HEX for char in parts[0].lower())
                or not parts[1]):
            problems.append(f'{path.name}:{lineno}: malformed sha256 entry')
            continue
        entries.append((parts[0].lower(), _normalize(parts[1])))
    return entries, problems


def _target(root: Path, rel: str, problems: list[str]) -> Path | None:
    candidate = root / rel
    try:
        resolved = candidate.resolve()
        base = root.resolve()
    except OSError as exc:
        problems.append(f'cannot resolve hash target {rel}: {exc}')
        return None
    if resolved != base and base not in resolved.parents:
        problems.append(f'hash target escapes capture root: {rel}')
        return None
    return candidate


def check_capture(root: Path, verify_hashes: bool = False,
                  reject_maps: bool = False) -> dict:
    """Check capture layout and optional hashes; never extract target data."""
    root = Path(root)
    problems: list[str] = []
    warnings: list[str] = []
    if not root.is_dir():
        return {
            'ok': False, 'root': str(root), 'archives': [], 'partitions': [],
            'runtime_files': 0, 'hash_entries': 0, 'hashes_verified': 0,
            'missing': [], 'mismatched': [], 'unlisted': [],
            'problems': ['capture root is not a directory'], 'warnings': [],
        }
    for name in SUBDIRS:
        if not (root / name).is_dir():
            problems.append(f'missing directory: {name}/')
    for rel in REQUIRED_FILES:
        if not (root / rel).is_file():
            problems.append(f'missing required file: {rel}')

    archives = sorted(path.name for path in (root / 'archives').glob('*.tar')) \
        if (root / 'archives').is_dir() else []
    partitions = sorted(path.name for path in (root / 'partitions').glob('*.img')) \
        if (root / 'partitions').is_dir() else []
    runtime_files = len(list((root / 'runtime').glob('*'))) \
        if (root / 'runtime').is_dir() else 0
    if archives and 'rootfs.tar' not in archives:
        problems.append('archives/rootfs.tar is required')
    if not archives:
        problems.append('no tar archives found')
    map_warnings = []
    for name in archives:
        if 'navi' in name.lower() and 'nonmap' not in name.lower():
            map_warnings.append(f'archive may contain map data: {name}')
    for name in partitions:
        if 'navi' in name.lower():
            map_warnings.append(f'raw image may contain map data: {name}')
    warnings.extend(map_warnings)
    if reject_maps:
        problems.extend(f'rejected map data: {warning}' for warning in map_warnings)

    entries: list[tuple[str, str]] = []
    covered: set[str] = set()
    missing: list[str] = []
    mismatched: list[str] = []
    verified = 0
    sums = root / 'meta' / 'SHA256SUMS'
    if sums.is_file():
        entries, sum_problems = parse_sums(sums)
        problems.extend(sum_problems)
        for expected, rel in entries:
            covered.add(rel)
            target = _target(root, rel, problems)
            if target is None:
                continue
            if not target.is_file():
                missing.append(rel)
                continue
            if verify_hashes:
                verified += 1
                if file_sha256(target) != expected:
                    mismatched.append(rel)
    navi_entries = 0
    navi_sums = root / 'meta' / 'SHA256SUMS.navi2'
    if navi_sums.is_file():
        extra, navi_problems = parse_sums(navi_sums)
        problems.extend(navi_problems)
        navi_entries = len(extra)
        for expected, rel in extra:
            covered.add(rel)
            target = _target(root, rel, problems)
            if target is None:
                continue
            if not target.is_file():
                missing.append(rel)
                continue
            if verify_hashes:
                verified += 1
                if file_sha256(target) != expected:
                    mismatched.append(rel)

    expected = {f'archives/{name}' for name in archives}
    expected.update(f'partitions/{name}' for name in partitions)
    if (root / 'critical/critical.tar').is_file():
        expected.add('critical/critical.tar')
    unlisted = sorted(expected - covered)
    problems.extend(f'unlisted artifact: {rel}' for rel in unlisted)

    return {
        'ok': not problems and not missing and not mismatched,
        'root': str(root),
        'archives': archives,
        'partitions': partitions,
        'runtime_files': runtime_files,
        'hash_entries': len(entries) + navi_entries,
        'hashes_verified': verified,
        'missing': sorted(set(missing)),
        'mismatched': sorted(set(mismatched)),
        'unlisted': unlisted,
        'problems': problems,
        'warnings': warnings,
    }
