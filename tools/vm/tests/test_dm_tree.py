# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from harness.dm_tree import check_dm_tree, diff_trees, parse_tree_text

VM = Path(__file__).resolve().parents[1]
SENSITIVE_FIELDS = {'AAuthSecret', 'AAuthData', 'AAuthName', 'Addr',
                    'PortNbr', 'ServerID', 'AppID', 'Name'}


def tree_xml(state: str = '10', account: str = 'fixture-account',
             fumo: bool = True, missing_client: bool = False,
             pkg_version: bool = True, pkg_name: bool = True) -> str:
    server = ''.join(
        f'<leaf><name>{name}</name><value>server-{name}</value></leaf>'
        for name in ('AAuthName', 'AAuthType', 'AAuthLevel', 'AAuthSecret',
                     'AAuthData'))
    server = server.replace('server-AAuthType', 'syncml:auth-md5')
    server = server.replace('server-AAuthLevel', 'SRVCRED')
    client = ''.join(
        f'<leaf><name>{name}</name><value>client-{name}</value></leaf>'
        for name in ('AAuthName', 'AAuthType', 'AAuthLevel', 'AAuthSecret',
                     'AAuthData'))
    client = client.replace('client-AAuthType', 'syncml:auth-md5')
    client = client.replace('client-AAuthLevel', 'CLCRED')
    fumo_block = ''
    if fumo:
        pkg = ('<leaf><name>PkgVersion</name><value>8</value></leaf>'
               if pkg_version else '')
        if pkg_name:
            pkg += '<leaf><name>PkgName</name><value>1.1 to 1.8</value></leaf>'
        fumo_block = f'''
      <node><name>FUMO</name>
        <type>urn:oma:mo:oma-fumo:1.0</type>
        <leaf><name>State</name><value>{state}</value></leaf>
        {pkg}
        <node><name>Download</name>
          <leaf><name>PkgURL</name><value>http://update-package-location</value></leaf>
        </node>
      </node>'''
    client_block = ('' if missing_client else
                    f'<node><name>CLIENT</name>{client}</node>')
    return f'''<rdmtree>
  <version>1.02</version>
  <node><name>.</name>
    <node><name>ManagedObjects</name>{fumo_block}
    </node>
    <node><name>DMAcc</name>
      <node><name>{account}</name>
        <node><name>AppAddr</name>
          <node><name>APPSRV</name>
            <leaf><name>Addr</name><value>server.example</value></leaf>
            <leaf><name>AddrType</name><value>URI</value></leaf>
            <node><name>Port</name><node><name>Port</name>
              <leaf><name>PortNbr</name><value>8002</value></leaf>
            </node></node>
          </node>
        </node>
        <node><name>AppAuth</name>
          <node><name>APPSRV</name>{server}</node>
          {client_block}
        </node>
      </node>
    </node>
  </node>
</rdmtree>
'''


class DmTreeTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix='vm-dm-tree-')
        self.root = Path(self._tmp.name)
        self.path = self.root / 'tree.xml'
        self.path.write_text(tree_xml())

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_valid_tree_and_redaction(self) -> None:
        report = check_dm_tree(self.path)
        self.assertTrue(report['ok'], report['problems'])
        self.assertEqual(report['version'], '1.02')
        self.assertEqual(report['fumo'],
                         {'state': 10, 'pkg_version': '8',
                          'pkg_url_present': True})
        self.assertEqual(report['accounts'], 1)
        self.assertEqual(report['auth_types'], ['syncml:auth-md5'])
        self.assertEqual(report['auth_levels'], ['CLCRED', 'SRVCRED'])
        encoded = json.dumps(report)
        for secret in ('fixture-account', 'server.example', 'server-AAuthSecret',
                       'client-AAuthSecret', 'server-AAuthName'):
            self.assertNotIn(secret, encoded)

    def test_missing_fumo_fails(self) -> None:
        self.path.write_text(tree_xml(fumo=False))
        report = check_dm_tree(self.path)
        self.assertFalse(report['ok'])
        self.assertIn('FUMO node', '\n'.join(report['problems']))

    def test_bad_state_and_missing_pkg_version_fail(self) -> None:
        self.path.write_text(tree_xml(state='ten', pkg_version=False))
        report = check_dm_tree(self.path)
        joined = '\n'.join(report['problems'])
        self.assertFalse(report['ok'])
        self.assertIn('State is not an integer', joined)
        self.assertIn('PkgVersion leaf is missing', joined)

    def test_missing_client_auth_fails(self) -> None:
        self.path.write_text(tree_xml(missing_client=True))
        report = check_dm_tree(self.path)
        self.assertFalse(report['ok'])
        self.assertIn('CLIENT auth is missing',
                      '\n'.join(report['problems']))

    def test_dtd_and_entity_are_rejected(self) -> None:
        self.path.write_text(
            '<!DOCTYPE rdmtree [<!ENTITY x "y">]>\n' + tree_xml())
        report = check_dm_tree(self.path)
        self.assertFalse(report['ok'])
        self.assertIn('DTD or entity', '\n'.join(report['problems']))

    def test_malformed_xml_is_rejected(self) -> None:
        self.path.write_text('<rdmtree>')
        report = check_dm_tree(self.path)
        self.assertFalse(report['ok'])
        self.assertIn('XML parse error', '\n'.join(report['problems']))

    def test_diff_identical_and_redacted(self) -> None:
        second = self.root / 'second.xml'
        second.write_text(tree_xml())
        diff = diff_trees(self.path, second)
        self.assertTrue(diff['ok'])
        self.assertTrue(diff['identical'])
        self.assertEqual(diff['changed'], [])
        self.assertNotIn('fixture-account', json.dumps(diff))

    def test_diff_reports_changed_added_removed_paths(self) -> None:
        second = self.root / 'second.xml'
        second.write_text(tree_xml(state='20', pkg_name=False))
        diff = diff_trees(self.path, second)
        self.assertTrue(diff['ok'])
        self.assertFalse(diff['identical'])
        self.assertIn('ManagedObjects/FUMO/State', diff['changed'])
        self.assertIn('ManagedObjects/FUMO/PkgName', diff['removed'])
        encoded = json.dumps(diff)
        self.assertNotIn('fixture-account', encoded)
        self.assertNotIn('server-AAuthSecret', encoded)

    def test_cli_and_fail_on_diff(self) -> None:
        second = self.root / 'second.xml'
        second.write_text(tree_xml(state='20'))
        completed = subprocess.run(
            [sys.executable, '-B', str(VM / 'launch.py'), 'check-dm-tree',
             str(self.path)], cwd=VM, capture_output=True, text=True,
            check=False)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertTrue(json.loads(completed.stdout)['ok'])
        completed = subprocess.run(
            [sys.executable, '-B', str(VM / 'launch.py'), 'check-dm-tree',
             str(self.path), '--compare', str(second), '--fail-on-diff'],
            cwd=VM, capture_output=True, text=True, check=False)
        self.assertEqual(completed.returncode, 1)
        self.assertFalse(json.loads(completed.stdout)['identical'])
        self.assertNotIn('fixture-account', completed.stdout)

    @unittest.skipUnless(os.environ.get('VM_REAL_DM_TREE') == '1',
                         'set VM_REAL_DM_TREE=1 for the captured tree')
    def test_real_captured_tree(self) -> None:
        path = (VM / 'captures/car-20261115/analysis/update/fota/dme/tree.xml')
        if not path.is_file():
            self.skipTest('captured tree unavailable')
        report = check_dm_tree(path)
        self.assertTrue(report['ok'], report['problems'])
        self.assertEqual(report['fumo']['state'], 10)
        sensitive = [value for leaf, value in parse_tree_text(path.read_text())['leaves']
                     if value and leaf[-1] in SENSITIVE_FIELDS]
        encoded = json.dumps(report)
        for value in sensitive:
            self.assertNotIn(value, encoded)


if __name__ == '__main__':
    unittest.main()
