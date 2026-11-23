#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Client-mode decisions in private paths, never touching vehicle/host /etc."""
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="wifi-client-", dir="/tmp/opencode") as temporary:
    root = Path(temporary)
    binary = root / "client_path"
    subprocess.run(["gcc", "-O1", "-g", "-Wall", "-Wextra", "-Wno-unused-function",
                    "-Wno-unused-parameter", "-Wno-format-truncation",
                    "-fsanitize=undefined",
                    "-DAPP_DIR=" + chr(34) + str(root / "state") + chr(34),
                    "-DRADIO_OWNER_PATH=" + chr(34) + str(root / "radio") + chr(34),
                    str(ROOT / "tests/test_client_path.c"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=30)
