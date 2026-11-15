#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Inspect and verify Gen5W micom MCU images (docs/16).

TARGET: Hyundai Ioniq 5 Gen5W_L (Linux x86-64), firmware
        NE_E.EUR.S5W_L.001.001.260128.

The micom MCU firmware ships inside the update package as
``update/micom/<variant>/<NAME>.DV.<date>.<rev>.bin``. Extract that tree under
your gitignored ``work/`` or ``input/`` directory; never stage it. This tool
only *reads* the files. It does not patch, repackage or flash anything.

What the analysis establishes, and how to reproduce each point:

* **size** — images come in two sizes, 2 MiB (46 of the 52 local files) and
  4 MiB (6, the ``*_K`` builds). ``info`` reports the size class.
* **slot map** — every image divides into 1 MiB halves. ``info`` prints the
  used/erased extent of each half, which is where the "1 MiB half" reading
  comes from rather than from a guess.
* **header records** — the first 0x80 bytes are ``{u16 0x0780, u16 addr}``
  records with twelve zero bytes each. ``info`` validates the padding and the
  magic rather than assuming them.
* **vector table** — 4-byte little-endian pointers from 0x200 while the value
  is below 0x100000. ``info`` reports the length and the distinct targets.
* **checksum** — ``Checksum.txt`` carries a ``CRC32`` per image that is **not**
  zlib/reflected CRC-32: it is the MSB-first CRC-32/MPEG-2 recurrence with init
  0xFFFFFFFF and no final XOR, over the whole file (2 or 4 MiB). ``verify``
  reproduces every published value.
* **statistics** — ``stats`` reproduces the entropy, run and block-identity
  numbers quoted in docs/16.

Usage
-----
    python3 tools/micom/mcu_image.py info    IMAGE
    python3 tools/micom/mcu_image.py crc     IMAGE...
    python3 tools/micom/mcu_image.py verify  MICOM_DIR
    python3 tools/micom/mcu_image.py stats   IMAGE
"""

from __future__ import annotations

import argparse
import collections
import math
import os
import sys
from pathlib import Path
from typing import List, Optional

BANK_SIZE = 0x100000          # 1 MiB half
KNOWN_SIZES = {0x200000: "2 MiB (one 2 MiB slot)",
               0x400000: "4 MiB (two 2 MiB slots)"}
VECTOR_TABLE_START = 0x200
BADDR_MAX = 0x100000          # addresses must be strictly below this

# CRC-32/MPEG-2: poly 0x04C11DB7, init 0xFFFFFFFF, refin/refout false, xorout 0.
_POLY = 0x04C11DB7
_TABLE: List[int] = []
for _i in range(256):
    _c = _i << 24
    for _ in range(8):
        _c = ((_c << 1) ^ _POLY) & 0xFFFFFFFF if _c & 0x80000000 else (_c << 1) & 0xFFFFFFFF
    _TABLE.append(_c)


def micom_crc32(data: bytes) -> int:
    """The updater's own checksum (micom_fw_updater, "FirmCRC Check")."""
    crc = 0xFFFFFFFF
    for byte in data:
        crc = ((crc << 8) & 0xFFFFFFFF) ^ _TABLE[((crc >> 24) ^ byte) & 0xFF]
    return crc


def entropy(data: bytes) -> float:
    if not data:
        return 0.0
    counts = collections.Counter(data)
    n = len(data)
    return -sum((v / n) * math.log2(v / n) for v in counts.values())


def vector_table(data: bytes):
    """4-byte little-endian pointers from 0x200 while the value is in range."""
    out = []
    off = VECTOR_TABLE_START
    while off + 4 <= len(data):
        val = int.from_bytes(data[off:off + 4], "little")
        if val >= BADDR_MAX:
            break
        out.append((off, val))
        off += 4
    return out


def header_records(data: bytes):
    """Parse and validate the {u16 0x0780, u16 addr, 12 zero bytes} table."""
    records = []
    for off in range(0, min(0x80, len(data)), 16):
        rec = data[off:off + 16]
        if len(rec) < 16:
            raise ValueError(f"truncated header record at 0x{off:x}")
        magic = int.from_bytes(rec[0:2], "little")
        addr = int.from_bytes(rec[2:4], "little")
        if rec[4:16] != b"\x00" * 12:
            raise ValueError(f"header record at 0x{off:x} has non-zero padding "
                             f"{rec[4:16].hex()}")
        if magic == 0 and addr == 0:
            continue
        if magic != 0x0780:
            raise ValueError(f"header record at 0x{off:x} has magic 0x{magic:04x}")
        records.append((off, magic, addr))
    return records


def used_runs(data: bytes, granularity: int = 0x10000):
    """Contiguous runs of blocks that are not wholly erased (0xFF)."""
    runs = []
    start = None
    for i in range(0, len(data), granularity):
        block = data[i:i + granularity]
        used = block != b"\xff" * len(block)
        if used and start is None:
            start = i
        elif not used and start is not None:
            runs.append((start, i))
            start = None
    if start is not None:
        runs.append((start, len(data)))
    return runs


def cmd_info(args) -> int:
    data = Path(args.image).read_bytes()
    label = KNOWN_SIZES.get(len(data))
    print(f"image       {args.image}")
    print(f"size        {len(data)} (0x{len(data):x})  "
          f"{label if label else '!! unrecognised size'}")
    if len(data) == 0:
        print("  !! empty file")
        return 2
    print(f"crc32       0x{micom_crc32(data):08X}  "
          f"(MSB-first, init FFFFFFFF, no xorout)")

    print("\nheader records (offset 0, {u16 0x0780, u16 addr, 12 zero bytes}):")
    try:
        records = header_records(data)
    except ValueError as exc:
        print(f"  !! {exc}")
        return 2
    for off, magic, addr in records:
        print(f"  +0x{off:03x}  magic=0x{magic:04x} addr=0x{addr:08x}")
    if not records:
        print("  (empty)")

    vt = vector_table(data)
    distinct = collections.Counter(v for _, v in vt)
    print(f"\nvector table at 0x{VECTOR_TABLE_START:x}: {len(vt)} entries, "
          f"{len(distinct)} distinct targets")
    for target, count in distinct.most_common(6):
        print(f"  0x{target:08x} x{count}")

    print("\n1 MiB halves:")
    for i in range(0, len(data), BANK_SIZE):
        half = data[i:i + BANK_SIZE]
        print(f"  0x{i:07x}..0x{i + len(half):07x}  H={entropy(half):4.2f}")
        for s, e in used_runs(half):
            tag = "header/code" if s == 0 else "code"
            print(f"      used 0x{i + s:07x}..0x{i + e:07x} ({e - s} bytes, {tag})")
        if not used_runs(half):
            print("      (erased)")

    print(f"\n{'slot' if len(data) > 0x200000 else 'half-pair'} comparison:")
    if len(data) == 0x400000:
        a, b = data[:0x200000], data[0x200000:]
        diff = sum(1 for x, y in zip(a, b) if x != y)
        same = sum(1 for i in range(0, 0x200000, 4096)
                   if a[i:i + 4096] == b[i:i + 4096])
        print(f"  slot 0 vs slot 1: {diff} differing bytes "
              f"({diff * 100.0 / 0x200000:.2f}%), {same}/512 identical 4 KiB blocks")
    else:
        a, b = data[:len(data) // 2], data[len(data) // 2:]
        diff = sum(1 for x, y in zip(a, b) if x != y)
        print(f"  first vs second half: {diff} differing bytes "
              f"({diff * 100.0 / len(a):.2f}%)")
    return 0


def cmd_crc(args) -> int:
    for name in args.images:
        data = Path(name).read_bytes()
        print(f"{micom_crc32(data):08X}  {name}")
    return 0


def load_checksum_txt(path: Path):
    claims = {}
    for line in path.read_text(errors="replace").splitlines():
        parts = line.split()
        if len(parts) >= 2 and "CRC32=0x" in parts[1]:
            claims[parts[0].upper()] = int(parts[1].split("0x")[1], 16)
    return claims


def cmd_verify(args) -> int:
    root = Path(args.micom_dir)
    manifest = root / "Checksum.txt"
    if not manifest.is_file():
        print(f"error: no Checksum.txt under {root}", file=sys.stderr)
        return 2
    claims = load_checksum_txt(manifest)
    if not claims:
        print(f"error: {manifest} contains no CRC32 claims", file=sys.stderr)
        return 2
    files = sorted(p for p in root.glob("*/*") if p.suffix.lower() == ".bin")
    if not files:
        print(f"error: no *.bin under {root}/*/", file=sys.stderr)
        return 2

    ok = bad = absent = badsize = 0
    for f in files:
        data = f.read_bytes()
        if len(data) not in KNOWN_SIZES:
            badsize += 1
            print(f"BAD-SIZE  {f.name}: {len(data)} bytes, expected one of "
                  f"{sorted(KNOWN_SIZES)}")
            continue
        key = f.name.upper()
        if key not in claims:
            absent += 1
            print(f"NO-CLAIM  {f.name}")
            continue
        got = micom_crc32(data)
        if got == claims[key]:
            ok += 1
        else:
            bad += 1
            print(f"MISMATCH  {f.name}: image 0x{got:08X} != Checksum.txt 0x{claims[key]:08X}")

    print(f"\n{ok}/{len(files)} images verified "
          f"({len(claims)} claims, {bad} CRC mismatches, {badsize} wrong size, "
          f"{absent} unclaimed)")
    if ok == 0 or bad or badsize or absent:
        return 1
    return 0


def cmd_stats(args) -> int:
    """Reproduce the numeric observations quoted in docs/16."""
    data = Path(args.image).read_bytes()
    print(f"image {args.image}  size {len(data)}")

    print("\nidentification strings:")
    for token in (b"SH850", b"R10 CE1", b"R155 CE1", b"R156 CE1"):
        start = 0
        while True:
            i = data.find(token, start)
            if i < 0:
                break
            end = data.find(b"\x00", i)
            print(f"  offset 0x{i:06x}  {data[i:end].decode('latin1')!r}")
            start = i + 1

    print("\nmod-4 byte-class entropy:")
    for base in args.regions:
        seg = data[base:base + args.length]
        if not seg:
            print(f"  region 0x{base:06x}: (past end of image)")
            continue
        print(f"  region 0x{base:06x}..0x{base + len(seg):06x} H={entropy(seg):.2f}")
        for k in range(4):
            col = seg[k::4]
            top = collections.Counter(col).most_common(3)
            print(f"     mod4=={k} H={entropy(col):.2f} top="
                  + ", ".join(f"0x{b:02x}x{c}" for b, c in top))

    print("\n64 KiB use map:")
    for s, e in used_runs(data):
        print(f"   used 0x{s:06x}-0x{e:06x} ({e - s} bytes)")

    if len(data) >= 0x200000:
        a, b = data[:0x100000], data[0x100000:0x200000]
        runs = []
        start = None
        for i, (x, y) in enumerate(zip(a, b)):
            if x != y and start is None:
                start = i
            elif x == y and start is not None:
                runs.append((start, i))
                start = None
        if start is not None:
            runs.append((start, BANK_SIZE))
        total = sum(e - s for s, e in runs)
        print(f"\nfirst 1 MiB half vs second: {total} differing bytes "
              f"({total * 100.0 / BANK_SIZE:.3f}%) in {len(runs)} runs")
        for s, e in sorted(runs, key=lambda r: r[1] - r[0], reverse=True)[:args.top]:
            print(f"   0x{s:06x}-0x{e:06x} ({e - s} bytes)")

    print("\n4 KiB block identity, by lag:")
    for lag in (0x10000, 0x20000, 0x40000, 0x80000, 0x100000, 0x200000):
        if lag >= len(data):
            continue
        same = total = 0
        for i in range(0, len(data) - lag, 4096):
            if data[i:i + 4096] == data[i + lag:i + lag + 4096]:
                same += 1
            total += 1
        print(f"   lag 0x{lag:06x}: {same}/{total} identical")
    return 0


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("info", help="container layout of one image")
    p.add_argument("image", type=Path)
    p.set_defaults(func=cmd_info)

    p = sub.add_parser("crc", help="compute the updater's CRC-32")
    p.add_argument("images", nargs="+", type=Path)
    p.set_defaults(func=cmd_crc)

    p = sub.add_parser("verify", help="check every image against Checksum.txt")
    p.add_argument("micom_dir", type=Path,
                   help="the update/micom directory (contains Checksum.txt)")
    p.set_defaults(func=cmd_verify)

    p = sub.add_parser("stats", help="reproduce the numbers quoted in docs/16")
    p.add_argument("image", type=Path)
    p.add_argument("--regions", type=lambda s: int(s, 0), nargs="+",
                   default=[0x20000, 0x40000, 0x80000],
                   help="region start offsets (default 0x20000 0x40000 0x80000)")
    p.add_argument("--length", type=lambda s: int(s, 0), default=0x10000)
    p.add_argument("--top", type=int, default=6)
    p.set_defaults(func=cmd_stats)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
