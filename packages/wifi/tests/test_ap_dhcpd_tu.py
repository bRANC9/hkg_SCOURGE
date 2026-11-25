#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""packages/common/ap_dhcpd.inc must compile as a standalone unit, strictly.

The WiFi1b includer reported three things a shared include has to get right on
its own (commit b5af3fc review): errno/stdbool declarations, the usleep() feature
macro, and a /proc path snprintf that trips -Wformat-truncation.  This compiles
tests/ap_dhcpd_tu.c with -std=c11 -Werror -Wformat-truncation=2 and no daemon
headers, so a regression shows up here instead of in the other package's build.
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TU = ROOT / "tests" / "ap_dhcpd_tu.c"

with tempfile.TemporaryDirectory(prefix="wifi-apdhcpd-", dir="/tmp/opencode") as tmp:
    binary = Path(tmp) / "ap_dhcpd_tu"
    subprocess.run(["gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-Wformat-truncation=2", str(TU), "-o", str(binary)], check=True)
    env = dict(os.environ)
    env.pop("SCOURGE_DHCP_TEST_PORT", None)
    subprocess.run([str(binary)], check=True, timeout=60, env=env)
print("PASS: ap_dhcpd.inc compiles standalone under -std=c11 -Werror and runs")
