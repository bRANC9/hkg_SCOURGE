#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Read the stick boot_profile.csv and show what a boot costs, boot by boot.

    python3 tools/boot_report.py boot_profile.csv [--delta]

The CSV holds one line per boot (key=value pairs, written by
packages/common/boot_profile.sh on the unit).  Every timing is a start offset in
seconds after boot, so lower is earlier.  --delta prints the change between the
last two boots: that is the interesting number after adding a package.
"""
import sys


def parse(path):
    boots = []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if not line or "=" not in line:
                continue
            row = {}
            for part in line.split():
                if "=" in part:
                    k, v = part.split("=", 1)
                    row[k] = v
            if row:
                boots.append(row)
    return boots


def num(value):
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def main(argv):
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    path = argv[1]
    boots = parse(path)
    if not boots:
        print("no boot lines in " + path, file=sys.stderr)
        return 1
    boots.sort(key=lambda r: r.get("measured_at", ""))
    keys = []
    for row in boots:
        for k in row:
            if k not in keys and k not in ("boot", "measured_at", "uptime"):
                keys.append(k)
    width = max([len(k) for k in keys] + [8])
    print(" " * (width + 2) + "  ".join("boot %d" % (i + 1) for i in range(len(boots))))
    for k in keys:
        cells = []
        for row in boots:
            cells.append(("%6s" % row.get(k, "-"))[-6:])
        print("%-*s  %s" % (width, k, "  ".join(cells)))
    print()
    for i, row in enumerate(boots, 1):
        print("boot %d: %s  uptime=%ss measured=%s"
              % (i, row.get("boot", "?")[:8], row.get("uptime", "?"), row.get("measured_at", "?")))
    if "--delta" in argv and len(boots) >= 2:
        prev, last = boots[-2], boots[-1]
        print()
        print("delta (previous -> last):")
        for k in keys:
            a, b = num(prev.get(k)), num(last.get(k))
            if a is None or b is None:
                continue
            mark = ""
            if b > a:
                mark = "  (+%d slower)" % (b - a)
            elif b < a:
                mark = "  (%d faster)" % (a - b)
            print("  %-16s %6d -> %-6d%s" % (k, a, b, mark))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
