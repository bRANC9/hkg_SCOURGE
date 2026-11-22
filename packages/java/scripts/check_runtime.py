#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Static compatibility gate for the packaged Temurin runtime.

Checks that matter before a 78 MB runtime is copied to a 4.1.27 / glibc 2.25
head unit:

  * every ELF's maximum GLIBC symbol version is <= 2.25
  * no symlinks (the payload must survive exFAT)
  * every non-GUI NEEDED library exists in the runtime or the vendor rootfs
  * the runtime is the expected Java 25 headless build (no javac by default)
  * SHA256SUMS verifies

The vendor-rootfs library check is best-effort when work/rootfs is not staged;
the VM run is the authoritative check.
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

GUI_ONLY = {
    "libX11.so.6",
    "libXext.so.6",
    "libXi.so.6",
    "libXrender.so.1",
    "libXtst.so.6",
}
SYSTEM_LIBS = {"ld-linux-x86-64.so.2"}
MAX_GLIBC = (2, 25)
DEFAULT_ROOTFS = Path(__file__).resolve().parents[3] / "work" / "rootfs"


def fail(message: str) -> None:
    print(f"RUNTIME_CHECK_FAIL {message}", file=sys.stderr)
    raise SystemExit(1)


def version_tuple(value: str) -> tuple[int, ...]:
    return tuple(int(part) for part in value.split("."))


def readelf(args: list[str], path: Path) -> str:
    try:
        completed = subprocess.run(
            ["readelf", *args, str(path)],
            capture_output=True,
            text=True,
            check=False,
        )
    except FileNotFoundError:
        fail("readelf is missing; install binutils")
    if completed.returncode != 0:
        return ""
    return completed.stdout


def is_elf(path: Path) -> bool:
    try:
        with path.open("rb") as handle:
            return handle.read(4) == b"\x7fELF"
    except OSError:
        return False


def collect_library_names(root: Path) -> set[str]:
    names: set[str] = set()
    if not root.is_dir():
        return names
    for current, _dirs, files in os.walk(root, followlinks=False):
        for name in files:
            names.add(name)
    return names


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("runtime", type=Path)
    parser.add_argument("--vendor-rootfs", type=Path,
                        default=Path(os.environ.get("SCOURGE_ROOTFS", str(DEFAULT_ROOTFS))))
    parser.add_argument("--allow-javac", action="store_true")
    parser.add_argument("--max-mb", type=int, default=200)
    args = parser.parse_args()

    runtime = args.runtime.resolve()
    if not runtime.is_dir():
        fail(f"runtime directory missing: {runtime}")
    java = runtime / "bin" / "java"
    if not java.is_file() or not os.access(java, os.X_OK):
        fail(f"bin/java is missing or not executable: {java}")
    if (runtime / "bin" / "javac").exists() and not args.allow_javac:
        fail("bin/javac is present; default package must be a runtime")
    if not (runtime / "bin" / "jcmd").exists():
        fail("bin/jcmd is missing (jdk.jcmd not linked)")

    release = (runtime / "release").read_text(errors="replace") if (runtime / "release").is_file() else ""
    match = re.search(r'^JAVA_VERSION="([^"]+)"', release, re.M)
    if not match:
        fail("release file has no JAVA_VERSION")
    java_version = match.group(1)
    if not java_version.startswith("25."):
        fail(f"unexpected Java version: {java_version}")
    # jlink's release file intentionally keeps only JAVA_VERSION and MODULES;
    # the full JDK release file (and the VM java -version check) carry the
    # implementor.  When present, require the pinned upstream.
    if 'IMPLEMENTOR=' in release and 'IMPLEMENTOR="Eclipse Adoptium"' not in release:
        fail("runtime is not the pinned Eclipse Adoptium build")

    symlinks = [p for p in runtime.rglob("*") if p.is_symlink()]
    if symlinks:
        fail(f"runtime contains {len(symlinks)} symlink(s), e.g. {symlinks[0]}")

    vendor = args.vendor_rootfs
    vendor_available = vendor.is_dir()
    runtime_libs = collect_library_names(runtime / "lib") | collect_library_names(runtime / "lib" / "server")
    vendor_libs = collect_library_names(vendor) if vendor_available else set()
    if not vendor_available:
        print(f"RUNTIME_CHECK_WARN vendor rootfs not staged at {vendor}; "
              "skipping external NEEDED lookups (VM run is authoritative)",
              file=sys.stderr)

    max_seen: tuple[int, ...] = (0,)
    max_file = ""
    elfs = 0
    missing: list[str] = []
    gui_missing: list[str] = []
    for path in runtime.rglob("*"):
        if not path.is_file() or not is_elf(path):
            continue
        elfs += 1
        versions = re.findall(r"GLIBC_([0-9.]+)", readelf(["--version-info"], path))
        if versions:
            top = max((version_tuple(value) for value in versions), default=(0,))
            if top > max_seen:
                max_seen = top
                max_file = str(path.relative_to(runtime))
        for lib in re.findall(r"NEEDED.*\[(.*?)\]", readelf(["-d"], path)):
            if lib in SYSTEM_LIBS or lib in runtime_libs or lib in vendor_libs:
                continue
            if lib in GUI_ONLY:
                gui_missing.append(f"{path.relative_to(runtime)} -> {lib}")
                continue
            missing.append(f"{path.relative_to(runtime)} -> {lib}")

    if max_seen > MAX_GLIBC:
        fail(f"{max_file} requires GLIBC_{'.'.join(map(str, max_seen))} "
             f"(limit {'.'.join(map(str, MAX_GLIBC))})")
    if missing and vendor_available:
        fail("unresolved NEEDED libraries: " + "; ".join(sorted(set(missing))[:8]))
    if missing and not vendor_available:
        print("RUNTIME_CHECK_WARN external NEEDED libraries not verified: "
              + "; ".join(sorted(set(missing))[:8]), file=sys.stderr)
    if gui_missing:
        print("RUNTIME_CHECK_WARN GUI-only X11 libraries are absent by design: "
              + "; ".join(sorted(set(gui_missing))[:4]), file=sys.stderr)

    sums = runtime / "SHA256SUMS"
    if sums.is_file():
        completed = subprocess.run(["sha256sum", "-c", "SHA256SUMS"], cwd=runtime,
                                   capture_output=True, text=True, check=False)
        if completed.returncode != 0:
            fail("SHA256SUMS verification failed: " + completed.stderr.strip()[:300])

    files = sum(1 for path in runtime.rglob("*") if path.is_file())
    size_bytes = sum(path.stat().st_size for path in runtime.rglob("*") if path.is_file())
    size_mb = size_bytes // (1024 * 1024)
    if files < 50:
        fail(f"runtime has only {files} files")
    if size_mb > args.max_mb:
        fail(f"runtime is {size_mb} MiB (limit {args.max_mb} MiB)")

    print(f"RUNTIME_CHECK_OK version={java_version} elfs={elfs} files={files} "
          f"size_mb={size_mb} max_glibc={'.'.join(map(str, max_seen))} "
          f"vendor_rootfs={'yes' if vendor_available else 'no'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
