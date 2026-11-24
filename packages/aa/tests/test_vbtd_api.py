#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""End-to-end test for probes/vbtd_api.c.

The tool decides whether the vendor Bluetooth stack *exports* the SDP/RFCOMM
calls the Android Auto bootstrap needs (docs/19 §5b), because a plain string
search cannot tell an export from an import.  The test builds two shared objects
on the host: one that exports the names, and one that only *imports* them, and
asserts the tool separates the two.  Nothing here needs the head unit.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

PACKAGE = Path(__file__).resolve().parents[1]
PROBE = PACKAGE / "probes" / "vbtd_api.c"
CC = os.environ.get("CC", "cc")

EXPORTER = """
int SDP_CreateRecord(unsigned short *handle) { *handle = 1u; return 0; }
int SDP_AddAttribute(void *record, unsigned short id, unsigned char type, unsigned long len, void *value)
{ (void)record; (void)id; (void)type; (void)len; (void)value; return 0; }
int SDP_AddServiceClassIdList(void *record, unsigned char count, const void *uuids)
{ (void)record; (void)count; (void)uuids; return 0; }
int RFCOMM_CreateConnection(unsigned short uuid, unsigned char scn, int server, unsigned short mtu,
                            unsigned long bdaddr, unsigned short *handle, void *callback)
{ (void)uuid; (void)scn; (void)server; (void)mtu; (void)bdaddr; (void)handle; (void)callback;
  return 0; }
"""

# The importer must actually *reference* both symbols, otherwise the compiler
# drops the undefined entry and the fixture would test nothing.
IMPORTER = """
extern int SDP_CreateRecord(unsigned short *handle);
extern int RFCOMM_CreateConnection(unsigned short uuid, unsigned char scn, int server,
                                   unsigned short mtu, unsigned long bdaddr,
                                   unsigned short *handle, void *callback);
int aa_uses_the_stack(unsigned short *h)
{
    return SDP_CreateRecord(h) +
           RFCOMM_CreateConnection(0, 8, 1, 990, 0, h, (void *)0);
}
"""


def run(command):
    return subprocess.run(command, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, check=False, timeout=120)


def build_so(directory, name, source):
    src = directory / (name + ".c")
    src.write_text(source, encoding="utf-8")
    out = directory / (name + ".so")
    result = run([CC, "-O1", "-shared", "-fPIC", "-o", str(out), str(src)])
    if result.returncode != 0:
        raise SystemExit("fixture build failed: " + result.stderr)
    return out


def probe(directory, target):
    report = directory / "bt_api.json"
    if report.exists():
        report.unlink()
    result = run([str(directory / "vbtd_api"), str(target), "--json", str(report)])
    data = json.loads(report.read_text(encoding="utf-8")) if report.exists() else {}
    return result.returncode, {"stdout": result.stdout, "stderr": result.stderr, "json": data}


def main():
    failures = 0

    def check(name, ok):
        nonlocal failures
        print(("PASS " if ok else "FAIL ") + name)
        if not ok:
            failures += 1

    with tempfile.TemporaryDirectory(prefix="aa_vbtd_api_") as tmp_name:
        tmp = Path(tmp_name)
        build = run([CC, "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror", "-o",
                     str(tmp / "vbtd_api"), str(PROBE)])
        if build.returncode != 0:
            print(build.stderr)
            return 1

        exporter = build_so(tmp, "exporter", EXPORTER)
        importer = build_so(tmp, "importer", IMPORTER)

        status, out = probe(tmp, exporter)
        check("exporter: exit 0", status == 0)
        check("exporter: sdp_api true", out["json"].get("sdp_api") is True)
        check("exporter: rfcomm_api true", out["json"].get("rfcomm_api") is True)
        check("exporter: symbols exported",
              all(v == "exported" for v in out["json"]["symbols"].values()))

        status, out = probe(tmp, importer)
        check("importer: exit 1", status == 1)
        check("importer: sdp_api false", out["json"].get("sdp_api") is False)
        check("importer: RFCOMM is only imported",
              out["json"]["symbols"].get("RFCOMM_CreateConnection") == "imported")
        check("importer: absent symbol reported",
              out["json"]["symbols"].get("SDP_AddServiceClassIdList") == "absent")
        check("importer: no export claimed in stdout", "exported" not in out["stdout"])

        missing = tmp / "no_such_file"
        status, out = probe(tmp, missing)
        check("missing file: exit 2", status == 2)
        check("missing file: explains itself", "cannot open" in out["stderr"])

        text = tmp / "not_elf.txt"
        text.write_text("this is not an object file\n", encoding="utf-8")
        status, out = probe(tmp, text)
        check("non-ELF: exit 2", status == 2)

    print("vbtd_api: all checks passed" if failures == 0 else "vbtd_api: " + str(failures) + " check(s) failed")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
