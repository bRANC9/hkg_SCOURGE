#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Install a dumped Hello-app base set into packages/wifi/base/.

Why this is a dump and never a synthesis: a base here decides whether the WiFi
builder emits a hello-family QML variant, and the installer carries that variant
to the car.  A fabricated base makes the variant reference a tile component the
unit's OEM app set may not define (2026-11-24: the synthesised hello base also
put the WiFi tile on firstEmptySlot(0) instead of (1)), so the fabricated route
was closed in build_artifacts._have().  The only honest source is a unit that
really ships the Hello tile; payload/dump_hello.sample.sh copies exactly what
the builder reads, and this script refuses anything it cannot prove.

Usage:
    python3 packages/wifi/scripts/install_hello_base.py --dump /media/usb/hello_dump/<stamp>
    python3 packages/wifi/scripts/install_hello_base.py --dump <dir> --check
    python3 packages/wifi/scripts/install_hello_base.py --dump <dir> --root qml_eco3.0 \
            --as battery_tile_hello

Exit codes: 0 installed (or --check passed), 2 the dump proves no Hello base,
3 the dump is damaged (manifest mismatch / missing files).
"""

from __future__ import annotations

import argparse
import hashlib
import re
import shutil
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve()
WIFI = HERE.parents[1]
BASE_DIR = WIFI / "base"

ADV = "HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
BASE_FILES = (
    ADV,
    "HomeScreen/QHMI_HomeScreenMain_Wide.qml",
    "HomeScreen/Screen_HomeScreen_AllMenus.qml",
    "HomeScreen/Screen_HomeScreen_Submenu_Setup.qml",
    "component/QCustomizedMenuListAllowEmpty.qml",
)

# variant -> the single file the builder reads for it
#   hello              : the whole five-file base (QHMI_HomeScreenMain_Advanced.qml)
#   battery_hello      : HomeScreen/QHMI_HomeScreenMain_Advanced.hello.qml
#   battery_tile_hello : HomeScreen/QHMI_HomeScreenMain_Advanced.qml
VARIANTS = ("hello", "battery_hello", "battery_tile_hello")
PAGE_BATTERY = re.compile(r"id:\s*batteryPage")


def md5_file(path: Path) -> str:
    digest = hashlib.md5()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 16), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify_manifest(dump: Path) -> list[str]:
    """Fail closed on a damaged dump: the stick is the only record of that unit."""
    problems: list[str] = []
    manifest = dump / "manifest.md5"
    if not manifest.is_file():
        return [f"{manifest} missing (the payload writes it last)"]
    listed: set[str] = set()
    for number, line in enumerate(manifest.read_text(errors="replace").splitlines(), 1):
        if not line.strip():
            continue
        parts = line.split(None, 1)
        if len(parts) != 2:
            problems.append(f"manifest line {number} is not '<md5>  <path>': {line!r}")
            continue
        want, rel = parts[0].strip().lower(), parts[1].strip()
        rel = rel[2:] if rel.startswith("./") else rel
        listed.add(rel)
        target = dump / rel
        if not target.is_file():
            problems.append(f"{rel}: listed but missing")
        elif md5_file(target) != want:
            problems.append(f"{rel}: md5 mismatch (dump damaged)")
    for path in sorted(p for p in dump.rglob("*") if p.is_file()):
        rel = str(path.relative_to(dump))
        if rel in ("manifest.md5", "report.txt") or rel in listed:
            continue
        problems.append(f"{rel}: present but not in the manifest")
    return problems


def report_values(dump: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    report = dump / "report.txt"
    if not report.is_file():
        return values
    for line in report.read_text(errors="replace").splitlines():
        if "=" in line and not line.startswith(" "):
            key, _, value = line.partition("=")
            values.setdefault(key.strip(), value.strip())
    return values


def root_dirs(dump: Path) -> list[Path]:
    roots = dump / "roots"
    if not roots.is_dir():
        return []
    return sorted(p for p in roots.iterdir() if p.is_dir())


def has_hello_tile(path: Path) -> bool:
    try:
        return "HelloTile" in path.read_text(errors="replace")
    except OSError:
        return False


def pick_root(dump: Path, wanted: str | None) -> tuple[Path, list[str]]:
    roots = root_dirs(dump)
    if wanted:
        for root in roots:
            if root.name == wanted:
                return root, []
        return dump / "roots" / wanted, [f"root {wanted} is not in the dump ({', '.join(r.name for r in roots) or 'none'})"]
    with_tile = [r for r in roots if has_hello_tile(r / ADV)]
    if len(with_tile) == 1:
        return with_tile[0], []
    best = report_values(dump).get("best_root")
    if best:
        for root in with_tile:
            if root.name == best:
                return root, []
    if not with_tile:
        return dump / "roots", ["no dumped root contains a Hello tile in its Advanced page"]
    return dump / "roots", [f"several roots carry a Hello tile ({', '.join(r.name for r in with_tile)}); pick one with --root"]


def validate(root: Path, variant: str) -> list[str]:
    """Everything the base must prove before it may become a build input."""
    problems: list[str] = []
    source_adv = root / ADV
    if not source_adv.is_file():
        return [f"{source_adv} missing"]
    text = source_adv.read_text(errors="replace")
    if "HelloTile" not in text:
        problems.append(f"{root.name}/{ADV} carries no Hello tile: this is not a Hello base")
    if variant == "battery_hello" and not PAGE_BATTERY.search(text):
        problems.append(f"{root.name}/{ADV} has no page-based battery block (id: batteryPage); "
                        "the battery_hello base must come from such a unit")
    if variant == "battery_tile_hello" and "BatteryTile" not in text:
        problems.append(f"{root.name}/{ADV} carries no BatteryTile; not a battery_tile_hello base")
    if variant == "hello":
        missing = [rel for rel in BASE_FILES if not (root / rel).is_file()]
        if missing:
            problems.append(f"{root.name} is incomplete: missing {', '.join(missing)}")
    return problems


def install(dump: Path, root: Path, variant: str, base_dir: Path, force: bool) -> list[str]:
    problems = validate(root, variant)
    if problems:
        return problems
    source_adv = root / ADV
    target = base_dir / variant
    if target.exists() and any(target.iterdir()) and not force:
        return [f"{target} already exists; move it aside or pass --force"]

    written: list[str] = []
    if variant == "hello":
        for rel in BASE_FILES:
            dest = target / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(root / rel, dest)
            written.append(rel)
    else:
        name = ADV if variant == "battery_tile_hello" else "HomeScreen/QHMI_HomeScreenMain_Advanced.hello.qml"
        dest = target / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source_adv, dest)
        written.append(name)

    stamp = report_values(dump).get("stamp", "unknown")
    marker = target / ".dumped"
    marker.write_text(
        "dumped base, not synthesised\n"
        f"source_dump={dump}\n"
        f"source_root={root.name}\n"
        f"dump_stamp={stamp}\n"
        f"installed_at={time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}\n"
        f"source_md5={md5_file(source_adv)}\n"
        "files=\n" + "".join(f"  {rel}\n" for rel in written),
    )
    print(f"installed {variant} from {root.name} ({stamp}): {len(written)} file(s)")
    for rel in written:
        print(f"  {target / rel}")
    return []


def main() -> int:
    parser = argparse.ArgumentParser(description="Install a dumped Hello-app base set.")
    parser.add_argument("--dump", required=True, type=Path, help="the hello_dump/<stamp> directory copied off the stick")
    parser.add_argument("--root", help="which dumped qml root to use (default: the one that proves a Hello tile)")
    parser.add_argument("--as", dest="variant", choices=VARIANTS, default="hello")
    parser.add_argument("--base-dir", type=Path, default=BASE_DIR)
    parser.add_argument("--check", action="store_true", help="verify and report only, write nothing")
    parser.add_argument("--force", action="store_true", help="replace an existing base of that variant")
    args = parser.parse_args()

    dump = args.dump.resolve()
    if not dump.is_dir():
        print(f"no dump directory at {dump}", file=sys.stderr)
        return 3

    problems = verify_manifest(dump)
    if problems:
        print("the dump is damaged; nothing installed:", file=sys.stderr)
        for problem in problems:
            print(f"  {problem}", file=sys.stderr)
        return 3

    # The payload already judged the unit; trust it before looking for a root, so
    # the error names the real reason (no Hello tile) instead of the symptom.
    values = report_values(dump)
    if values.get("verdict") and values["verdict"] != "complete":
        print(f"the dump reports verdict={values['verdict']}: no usable Hello base on that unit", file=sys.stderr)
        return 2

    root, problems = pick_root(dump, args.root)
    if problems:
        for problem in problems:
            print(problem, file=sys.stderr)
        return 2

    if args.check:
        problems = validate(root, args.variant)
        if problems:
            for problem in problems:
                print(problem, file=sys.stderr)
            return 2
        print(f"dump ok: {len(root_dirs(dump))} root(s), using {root.name}, variant={args.variant}")
        return 0

    problems = install(dump, root, args.variant, args.base_dir, args.force)
    if problems:
        for problem in problems:
            print(problem, file=sys.stderr)
        return 2

    print("next:")
    print("  python3 packages/wifi/scripts/build_artifacts.py")
    print("  make -C packages/wifi test")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
