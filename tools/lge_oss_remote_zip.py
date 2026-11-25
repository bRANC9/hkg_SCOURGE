#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Read a ZIP that lives on a slow HTTP server without downloading it whole.

The LG OSS portal hands out a presigned S3 URL for the head unit's source drop
(IAH1483SKDG_3.0.zip, 1.4 GB).  At the ~135 KB/s this link gives, the whole file
is a three-hour download for a handful of packages we actually need (the vendor
wpa_supplicant, connman's tether code, the bcmdhd driver).  A zip keeps its
central directory at the END of the file, so listing is a few range requests and
a member can be extracted on its own.

    python3 tools/lge_oss_remote_zip.py --url "<presigned url>" list
    python3 tools/lge_oss_remote_zip.py --url "<presigned url>" extract --match wpa_supplicant --out DIR

Reads go through curl (present everywhere here) with Range requests and a 1 MB
block cache; nothing is written except the requested members.
"""
from __future__ import annotations

import argparse
import io
import os
from pathlib import Path
import subprocess
import sys
import zipfile

BLOCK = 1 << 20


class RemoteFile(io.RawIOBase):
    """Seekable read-only file over HTTP(S) range requests, with block caching."""

    def __init__(self, url: str):
        self.url = url
        self.pos = 0
        head = subprocess.run(["curl", "-sS", "-o", "/dev/null", "-D", "-", "-r", "0-0",
                               "-H", "User-Agent: Mozilla/5.0", url],
                              capture_output=True, text=True)
        size = None
        for line in head.stdout.splitlines():
            if line.lower().startswith("content-range:"):
                size = int(line.rsplit("/", 1)[1].strip())
        if not size:
            raise SystemExit("cannot determine the remote size:\n" + head.stdout + head.stderr)
        self.size = size
        self.blocks: dict[int, bytes] = {}
        self.fetched = 0

    def readable(self) -> bool:
        return True

    def seekable(self) -> bool:
        return True

    def _block(self, index: int) -> bytes:
        if index in self.blocks:
            return self.blocks[index]
        start = index * BLOCK
        end = min(start + BLOCK, self.size) - 1
        done = subprocess.run(["curl", "-sS", "-H", "User-Agent: Mozilla/5.0",
                               "-r", f"{start}-{end}", self.url],
                              capture_output=True)
        if done.returncode != 0:
            raise SystemExit("range fetch failed: " + done.stderr.decode("utf-8", "replace"))
        data = done.stdout
        self.fetched += len(data)
        self.blocks[index] = data
        return data

    def read(self, n: int = -1) -> bytes:
        if n is None or n < 0:
            n = self.size - self.pos
        n = min(n, self.size - self.pos)
        out = bytearray()
        while n > 0:
            index, offset = divmod(self.pos, BLOCK)
            block = self._block(index)
            chunk = block[offset:offset + n]
            if not chunk:
                break
            out += chunk
            self.pos += len(chunk)
            n -= len(chunk)
        return bytes(out)

    def seek(self, offset: int, whence: int = os.SEEK_SET) -> int:
        if whence == os.SEEK_SET:
            self.pos = offset
        elif whence == os.SEEK_CUR:
            self.pos += offset
        else:
            self.pos = self.size + offset
        self.pos = max(0, min(self.pos, self.size))
        return self.pos

    def tell(self) -> int:
        return self.pos


def open_remote(url: str) -> zipfile.ZipFile:
    return zipfile.ZipFile(RemoteFile(url))


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", required=True, help="presigned URL of the zip")
    sub = parser.add_subparsers(dest="action", required=True)
    sub.add_parser("list")
    ex = sub.add_parser("extract")
    ex.add_argument("--match", action="append", default=[],
                    help="substring of the member path (repeatable)")
    ex.add_argument("--out", required=True)
    ex.add_argument("--limit-mb", type=int, default=250, help="stop after this much uncompressed")
    args = parser.parse_args(argv[1:])

    archive = open_remote(args.url)
    names = archive.namelist()
    if args.action == "list":
        for name in names:
            info = archive.getinfo(name)
            print(f"{info.file_size:12d} {info.compress_size:12d}  {name}")
        return 0

    out = Path(args.out)
    total = 0
    written = 0
    for name in names:
        if name.endswith("/"):
            continue
        if args.match and not any(m in name for m in args.match):
            continue
        info = archive.getinfo(name)
        if total + info.file_size > args.limit_mb * 1024 * 1024:
            print(f"stopping at the {args.limit_mb} MB budget before {name}", file=sys.stderr)
            break
        target = out / name.lstrip("/")
        target.parent.mkdir(parents=True, exist_ok=True)
        with archive.open(info) as src, open(target, "wb") as dst:
            while True:
                chunk = src.read(1 << 20)
                if not chunk:
                    break
                dst.write(chunk)
        total += info.file_size
        written += 1
    print(f"extracted {written} member(s), {total} bytes uncompressed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
