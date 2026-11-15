# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Deterministic offline FUMO exchange model.

This is not the vendor dmengine and it does not speak SyncML.  It models the
OMA FUMO state machine (Idle=10, Downloading=20, Downloaded=30, Updating=40),
package admission checks and a mock DM endpoint so update scripts and failure
handling can be tested without credentials, network access or OEM code.
"""
from __future__ import annotations

import hashlib
import re
from dataclasses import dataclass, field
from enum import IntEnum
from urllib.parse import urlsplit

NAME = re.compile(r'^[A-Za-z0-9._-]{1,64}$')
VERSION = re.compile(r'^\d+(?:\.\d+)*$')
HEX = frozenset('0123456789abcdef')


class FumoError(ValueError):
    """Invalid package, authentication or state transition."""


class State(IntEnum):
    IDLE = 10
    DOWNLOADING = 20
    DOWNLOADED = 30
    UPDATING = 40


def version_key(version: str) -> tuple[int, ...]:
    if not VERSION.match(version):
        raise FumoError(f'invalid version: {version!r}')
    return tuple(int(part) for part in version.split('.'))


@dataclass(frozen=True)
class Package:
    name: str
    version: str
    url: str
    data: bytes = field(repr=False)
    size: int = 0
    digest: str = ''

    def __post_init__(self) -> None:
        self.validate()

    @classmethod
    def build(cls, name: str, version: str, url: str, data: bytes) -> 'Package':
        data = bytes(data)
        package = cls(name, version, url, data, size=len(data),
                      digest=hashlib.sha256(data).hexdigest())
        package.validate()
        return package

    def validate(self) -> None:
        if not NAME.match(self.name):
            raise FumoError(f'invalid package name: {self.name!r}')
        version_key(self.version)
        parsed = urlsplit(self.url)
        if parsed.scheme not in ('http', 'https'):
            raise FumoError(f'package URL scheme must be http(s): {self.url!r}')
        if not parsed.hostname:
            raise FumoError(f'package URL has no host: {self.url!r}')
        if parsed.username or parsed.password or '@' in parsed.netloc:
            raise FumoError('package URL must not contain credentials')
        if parsed.fragment:
            raise FumoError('package URL must not contain a fragment')
        if not self.data:
            raise FumoError('package payload is empty')
        if self.size <= 0:
            raise FumoError('package size must be positive')
        if len(self.data) != self.size:
            raise FumoError('package size does not match payload')
        if (len(self.digest) != 64
                or any(char not in HEX for char in self.digest.lower())):
            raise FumoError('package digest must be 64 lowercase hex characters')
        if hashlib.sha256(self.data).hexdigest() != self.digest.lower():
            raise FumoError('package digest does not match payload')


def make_package(name: str, version: str,
                 url: str = 'http://update.invalid/package.bin',
                 data: bytes | None = None) -> Package:
    payload = data or f'fumo-package:{name}:{version}'.encode()
    package = Package(name, version, url, payload,
                      size=len(payload),
                      digest=hashlib.sha256(payload).hexdigest())
    package.validate()
    return package


class FumoMachine:
    """Strict OMA FUMO state transitions with a replayable event log."""

    def __init__(self, installed_version: str | None = None) -> None:
        if installed_version is not None:
            version_key(installed_version)
        self.installed_version = installed_version
        self.state = State.IDLE
        self.package: Package | None = None
        self.downloaded = bytearray()
        self.events: list[dict] = []

    def _event(self, action: str, before: State, result: str = 'ok',
               reason: str | None = None) -> None:
        self.events.append({
            'action': action, 'from': int(before), 'to': int(self.state),
            'result': result,
            'package': self.package.version if self.package else None,
            'installed': self.installed_version,
            'reason': reason,
        })

    def _require(self, expected: State, action: str) -> None:
        if self.state != expected:
            self._event(action, self.state, 'error', f'state={int(self.state)}')
            raise FumoError(f'{action} requires state {int(expected)}, '
                            f'current {int(self.state)}')

    def begin_download(self, package: Package) -> None:
        before = self.state
        try:
            self._require(State.IDLE, 'begin-download')
            package.validate()
            if (self.installed_version is not None
                    and version_key(package.version)
                    <= version_key(self.installed_version)):
                raise FumoError('package version is not newer than installed')
            self.package = package
            self.downloaded.clear()
            self.state = State.DOWNLOADING
            self._event('begin-download', before)
        except FumoError as exc:
            if not self.events or self.events[-1]['action'] != 'begin-download':
                self._event('begin-download', before, 'error', str(exc))
            raise

    def append_download(self, chunk: bytes) -> None:
        before = self.state
        self._require(State.DOWNLOADING, 'download-chunk')
        if not chunk:
            self._event('download-chunk', before, 'error', 'empty chunk')
            raise FumoError('download chunk is empty')
        if len(self.downloaded) + len(chunk) > (self.package.size if self.package else 0):
            self._event('download-chunk', before, 'error', 'size overflow')
            raise FumoError('download exceeds declared package size')
        self.downloaded.extend(chunk)
        self._event('download-chunk', before)

    def complete_download(self) -> None:
        before = self.state
        try:
            self._require(State.DOWNLOADING, 'download-complete')
            assert self.package is not None
            if len(self.downloaded) != self.package.size:
                raise FumoError('downloaded size does not match package')
            if hashlib.sha256(self.downloaded).hexdigest() != self.package.digest.lower():
                raise FumoError('downloaded digest does not match package')
            self.state = State.DOWNLOADED
            self._event('download-complete', before)
        except FumoError as exc:
            if not self.events or self.events[-1]['action'] != 'download-complete':
                self._event('download-complete', before, 'error', str(exc))
            raise

    def fail_download(self, reason: str = 'injected') -> None:
        before = self.state
        self._require(State.DOWNLOADING, 'download-failed')
        self.state = State.IDLE
        self.package = None
        self.downloaded.clear()
        self._event('download-failed', before, reason=reason)

    def begin_install(self) -> None:
        before = self.state
        self._require(State.DOWNLOADED, 'begin-install')
        self.state = State.UPDATING
        self._event('begin-install', before)

    def complete_install(self) -> None:
        before = self.state
        try:
            self._require(State.UPDATING, 'install-complete')
            assert self.package is not None
            self.installed_version = self.package.version
            self.state = State.IDLE
            self._event('install-complete', before)
            self.package = None
            self.downloaded.clear()
        except FumoError as exc:
            if not self.events or self.events[-1]['action'] != 'install-complete':
                self._event('install-complete', before, 'error', str(exc))
            raise

    def fail_install(self, reason: str = 'injected') -> None:
        before = self.state
        self._require(State.UPDATING, 'install-failed')
        self.state = State.DOWNLOADED
        self._event('install-failed', before, reason=reason)

    def reset(self) -> None:
        before = self.state
        self.state = State.IDLE
        self.package = None
        self.downloaded.clear()
        self._event('reset', before)

    def status(self) -> dict:
        return {
            'state': int(self.state),
            'state_name': self.state.name.lower(),
            'installed': self.installed_version,
            'package': self.package.version if self.package else None,
            'downloaded_bytes': len(self.downloaded),
            'events': len(self.events),
        }


class MockDmEndpoint:
    """Offline descriptor endpoint; no sockets, no real credentials."""

    def __init__(self, token: str, packages: dict[str, Package],
                 installed_version: str | None = None) -> None:
        if not token:
            raise FumoError('endpoint token must be non-empty')
        self.token = token
        self.packages = dict(packages)
        self.installed_version = installed_version
        self.queries: list[dict] = []

    def query(self, name: str, token: str) -> Package:
        if token != self.token:
            self.queries.append({'name': name, 'result': 'auth-error'})
            raise FumoError('DM authentication failed')
        package = self.packages.get(name)
        if package is None:
            self.queries.append({'name': name, 'result': 'not-found'})
            raise FumoError(f'package not found: {name}')
        package.validate()
        if (self.installed_version is not None
                and version_key(package.version)
                <= version_key(self.installed_version)):
            self.queries.append({'name': name, 'result': 'not-newer'})
            raise FumoError('package version is not newer than installed')
        self.queries.append({'name': name, 'result': 'ok',
                             'version': package.version})
        return package


def run_script(steps: list[dict], installed: str | None = None,
               token: str = 'lab-token', chunk_size: int = 7,
               packages: dict[str, Package] | None = None) -> dict:
    """Run a scripted offline FUMO exchange with per-step state evidence."""
    if not isinstance(steps, list) or not steps:
        raise FumoError('script must be a non-empty list')
    if chunk_size <= 0:
        raise FumoError('chunk size must be positive')
    available = packages or {'mango-rootfs': make_package('mango-rootfs', '8')}
    endpoint = MockDmEndpoint(token, available, installed)
    machine = FumoMachine(installed)
    report = {'ok': True, 'installed_before': installed,
              'installed_after': None, 'final_state': None,
              'steps': [], 'queries': endpoint.queries, 'events': machine.events,
              'error': None}
    for index, step in enumerate(steps):
        if not isinstance(step, dict):
            raise FumoError(f'script step {index} is not an object')
        operation = step.get('op')
        before = machine.state
        record = {'index': index, 'op': operation, 'from': int(before),
                  'to': None, 'ok': False}
        try:
            if operation == 'download':
                selected = endpoint.query(str(step.get('package', 'mango-rootfs')),
                                          str(step.get('token', token)))
                machine.begin_download(selected)
                for offset in range(0, len(selected.data), chunk_size):
                    machine.append_download(selected.data[offset:offset + chunk_size])
                machine.complete_download()
            elif operation == 'begin-download':
                selected = endpoint.query(str(step.get('package', 'mango-rootfs')),
                                          str(step.get('token', token)))
                machine.begin_download(selected)
            elif operation == 'append':
                data = step.get('data')
                if not isinstance(data, str):
                    raise FumoError('append requires a hex string')
                machine.append_download(bytes.fromhex(data))
            elif operation == 'complete-download':
                machine.complete_download()
            elif operation == 'fail-download':
                machine.fail_download(str(step.get('reason', 'scripted')))
            elif operation == 'install':
                machine.begin_install()
                machine.complete_install()
            elif operation == 'begin-install':
                machine.begin_install()
            elif operation == 'complete-install':
                machine.complete_install()
            elif operation == 'fail-install':
                machine.fail_install(str(step.get('reason', 'scripted')))
            elif operation == 'reset':
                machine.reset()
            elif operation == 'status':
                pass
            else:
                raise FumoError(f'unknown op: {operation!r}')
            record['ok'] = True
        except (FumoError, ValueError) as exc:
            record['error'] = str(exc)
            report['error'] = str(exc)
        record['to'] = int(machine.state)
        report['steps'].append(record)
        if not record['ok']:
            break
    report.update({'installed_after': machine.installed_version,
                   'final_state': int(machine.state),
                   'events': machine.events})
    report['ok'] = report['error'] is None
    return report


def run_exchange(installed: str | None = None, package_version: str = '8',
                 token: str = 'lab-token', fail_stage: str | None = None,
                 chunk_size: int = 7) -> dict:
    """Run the canonical download/install exchange and return a transcript."""
    if chunk_size <= 0:
        raise FumoError('chunk size must be positive')
    if fail_stage not in (None, 'download', 'install'):
        raise FumoError(f'unknown failure stage: {fail_stage!r}')
    package = make_package('mango-rootfs', package_version)
    endpoint = MockDmEndpoint(token, {package.name: package}, installed)
    machine = FumoMachine(installed)
    report = {'ok': False, 'failed_stage': fail_stage,
              'installed_before': installed, 'installed_after': None,
              'package_version': package_version, 'final_state': None,
              'status': None, 'queries': None, 'events': None}
    try:
        selected = endpoint.query(package.name, token)
        machine.begin_download(selected)
        if fail_stage == 'download':
            machine.fail_download('injected download failure')
        else:
            for offset in range(0, len(selected.data), chunk_size):
                machine.append_download(selected.data[offset:offset + chunk_size])
            machine.complete_download()
            machine.begin_install()
            if fail_stage == 'install':
                machine.fail_install('injected install failure')
            else:
                machine.complete_install()
        report['ok'] = True
    except FumoError as exc:
        report['error'] = str(exc)
    report.update({
        'installed_after': machine.installed_version,
        'final_state': int(machine.state),
        'status': machine.status(),
        'queries': endpoint.queries,
        'events': machine.events,
    })
    return report
