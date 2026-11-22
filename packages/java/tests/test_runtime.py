#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Host-side acceptance checks for the packaged Java 25 runtime.

The authoritative compatibility check is the vendor-kernel VM scenario; this
script catches build/packaging mistakes quickly on the build host.
"""
from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "out" / "usb"


def fail(message: str) -> None:
    print(f"RUNTIME_TEST_FAIL {message}", file=sys.stderr)
    raise SystemExit(1)


def main() -> int:
    runtimes = sorted((OUT / "payload" / "java").glob("runtime-*"))
    if len(runtimes) != 1:
        fail(f"expected exactly one runtime under {OUT}/payload/java, got {len(runtimes)}")
    runtime = runtimes[0]
    java = runtime / "bin" / "java"
    if not java.is_file():
        fail(f"missing {java}")

    check = subprocess.run(
        [sys.executable, str(ROOT / "scripts" / "check_runtime.py"), str(runtime)],
        text=True,
    )
    if check.returncode != 0:
        fail("check_runtime.py rejected the runtime")

    version = subprocess.run([str(java), "-version"], capture_output=True, text=True)
    if version.returncode != 0:
        fail(f"java -version failed: {version.stderr.strip()}")
    text = version.stdout + version.stderr
    for needle in ('openjdk version "25.0.5.1"', "Temurin-25.0.5.1+2",
                   "64-Bit Server VM"):
        if needle not in text:
            fail(f"java -version does not contain {needle!r}: {text!r}")

    modules = subprocess.run([str(java), "--list-modules"], capture_output=True, text=True)
    if modules.returncode != 0:
        fail("java --list-modules failed")
    for module in ("jdk.httpserver", "jdk.zipfs", "jdk.jcmd", "jdk.jfr",
                   "jdk.localedata", "jdk.charsets", "jdk.crypto.ec"):
        if f"{module}@" not in modules.stdout:
            fail(f"runtime is missing {module}")

    if (runtime / "bin" / "javac").exists():
        fail("default runtime unexpectedly contains javac")
    if not (runtime / "bin" / "jcmd").is_file():
        fail("default runtime is missing jcmd")
    if (OUT / "payload" / "java" / "runtime.version").read_text().strip() == "":
        fail("empty runtime.version")
    unit = runtime / "examples" / "java-app@.service"
    if not unit.is_file():
        fail("example systemd unit missing")
    if "/rw_data/java-apps/" not in unit.read_text():
        fail("example unit does not use the preserved app directory")
    if not (runtime / "SCOURGE-RUNTIME.txt").is_file():
        fail("runtime provenance file missing")

    size_mb = sum(p.stat().st_size for p in runtime.rglob("*") if p.is_file()) // (1024 * 1024)
    print(f"RUNTIME_TEST_OK version={version.stderr.splitlines()[0]} size_mb={size_mb}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
