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
import json
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


def mint_url(os_seq: str, file_type: str = "Op", model: str = "IWK2G3FQLWG") -> str:
    """Ask the LG OSS portal for a fresh presigned URL (valid 30 minutes).

    The download links expire; the portal's own form does exactly this POST.
    """
    import urllib.parse
    import urllib.request
    body = urllib.parse.urlencode({"osSeq": os_seq, "fileType": file_type,
                                   "fileIdx": "1", "modelName": model}).encode()
    request = urllib.request.Request(
        "https://opensource.lge.com/download/releaseFileDownloadUrl", data=body,
        headers={"Content-Type": "application/x-www-form-urlencoded; charset=UTF-8",
                 "X-Requested-With": "XMLHttpRequest",
                 "Referer": "https://opensource.lge.com/product/list?ctgr=024&subCtgr=051",
                 "Origin": "https://opensource.lge.com",
                 "User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(request, timeout=60) as response:
        payload = json.loads(response.read().decode())
    if not payload.get("success"):
        raise SystemExit(f"portal refused: {payload}")
    return payload["data"]

def _curl_range(url: str, start: int, end: int) -> bytes:
    # --max-time/--retry: a stalled connection must fail loudly, not hang the
    # whole read (S3 throttling after a burst is real).
    done = subprocess.run(["curl", "-sS", "--max-time", "120", "--retry", "2",
                           "--retry-delay", "2", "-H", "User-Agent: Mozilla/5.0",
                           "-r", f"{start}-{end}", url], capture_output=True)
    if done.returncode != 0 or len(done.stdout) != end - start + 1:
        raise SystemExit(f"range {start}-{end} failed: {done.stderr.decode('utf-8', 'replace')[:200]}")
    return done.stdout


def fetch_member(url: str, match: str, out_path: Path, workers: int = 12) -> None:
    """Download ONE member with parallel range requests, in its stored order."""
    from concurrent.futures import ThreadPoolExecutor
    archive = open_remote(url)
    names = [n for n in archive.namelist() if match in n]
    if not names:
        raise SystemExit(f"no member matching {match!r}")
    info = archive.getinfo(names[0])
    # The local header sits in front of the data; the central directory's
    # compress_size is authoritative (a streamed entry leaves it 0 locally).
    gro = archive.fp
    gro.seek(info.header_offset)
    header = gro.read(30)
    if header[:4] != b"PK\x03\x04":
        raise SystemExit("bad local header signature")
    name_len = int.from_bytes(header[26:28], "little")
    extra_len = int.from_bytes(header[28:30], "little")
    start = info.header_offset + 30 + name_len + extra_len
    length = info.compress_size
    # Some members put a few bytes between the local header and the payload (the
    # 2023 drop: five).  Find the compression magic in the first 64 bytes and
    # start there, otherwise every chunk is shifted and the stream is corrupt.
    if info.compress_type != 0:
        # A deflated member: the stored bytes are a zip deflate stream, not the
        # payload.  Fetch those bytes in parallel (the slow part) and inflate
        # locally; the CRC check below is the same proof zipfile would give.
        import zlib as _zlib
        from concurrent.futures import ThreadPoolExecutor
        start = info.header_offset + 30 + name_len + extra_len
        length = info.compress_size
        chunk = max(1 << 20, length // (workers * 4))
        spans = [(s, min(s + chunk - 1, start + length - 1))
                 for s in range(start, start + length, chunk)]
        print(f"member {names[0]}: {info.file_size} bytes deflated, {len(spans)} ranges", flush=True)
        inflater = _zlib.decompressobj(-15)
        crc = 0
        written = 0
        out_path.parent.mkdir(parents=True, exist_ok=True)
        with ThreadPoolExecutor(max_workers=workers) as pool, open(out_path, "wb") as fh:
            futures = [pool.submit(_curl_range, url, a, b) for a, b in spans]
            for future in futures:
                data = inflater.decompress(future.result())
                if data:
                    crc = _zlib.crc32(data, crc)
                    fh.write(data)
                    written += len(data)
        tail = inflater.flush()
        if tail:
            crc = _zlib.crc32(tail, crc)
            with open(out_path, "ab") as fh:
                fh.write(tail)
            written += len(tail)
        if crc != info.CRC or written != info.file_size:
            raise SystemExit(f"{out_path}: inflate mismatch (crc {crc:#x} vs {info.CRC:#x}, {written} vs {info.file_size})")
        print(f"wrote {out_path} ({written} bytes, crc ok)", flush=True)
        return

    probe = _curl_range(url, start, min(start + 63, start + length - 1))
    shift = 0
    for sig in (bytes.fromhex("1f8b08"), b"BZh9", bytes.fromhex("fd377a585a00"), b"PK\x03\x04"):
        pos = probe.find(sig)
        if pos > 0:
            shift = pos
            break
    if shift:
        print(f"  payload starts {shift} byte(s) after the local header", flush=True)
        start += shift
    print(f"member {names[0]}: {info.file_size} bytes ({length} compressed, data at {start})", flush=True)
    chunk = max(1 << 20, length // (workers * 4))
    spans = [(s, min(s + chunk - 1, start + length - 1)) for s in range(start, start + length, chunk)]
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with ThreadPoolExecutor(max_workers=workers) as pool:
        futures = [pool.submit(_curl_range, url, a, b) for a, b in spans]
        with open(out_path, "wb") as fh:
            done_bytes = 0
            for future in futures:
                data = future.result()
                fh.write(data)
                done_bytes += len(data)
                print(f"  {done_bytes}/{length}", flush=True)
    # Some members carry a few bytes of local-header padding in front of the
    # real payload (the 2023 kernel member has five).  Align on the compression
    # magic and pull the missing tail so the file is complete and testable.
    raw = out_path.read_bytes()
    magic = (b"\x1f\x8b\x08", b"\xfd7zXZ\x00", b"BZh9", b"PK\x03\x04")
    shift = 0
    for sig in magic:
        pos = raw.find(sig, 0, 64)
        if pos > 0 and (shift == 0 or pos < shift):
            shift = pos
    if shift:  # only if the probe above did not already align us
        print(f"  payload starts {shift} byte(s) into the member; realigning", flush=True)
        body = raw[shift:]
        need = length - len(body)
        if need > 0:
            # A member is followed immediately by the next local header: if that
            # is what sits after our copy, the payload is complete and the extra
            # bytes were header padding, not data.
            following = _curl_range(url, start + len(raw), start + len(raw) + 3)
            if following[:4] != b"PK\x03\x04":
                body += _curl_range(url, start + len(raw), start + length - 1)
        out_path.write_bytes(body)
        print(f"  realigned to {len(body)} bytes", flush=True)
    print(f"wrote {out_path} ({out_path.stat().st_size} bytes)", flush=True)

def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", help="presigned URL of the zip")
    parser.add_argument("--portal-os-seq", help="mint a fresh URL for this osSeq (511245=2.0, 511246=3.0)")
    parser.add_argument("--portal-file-type", default="Op", help="Op = source, Li = OSS notice")
    sub = parser.add_subparsers(dest="action", required=True)
    sub.add_parser("list")
    ex = sub.add_parser("extract")
    ex.add_argument("--match", action="append", default=[],
                    help="substring of the member path (repeatable)")
    ex.add_argument("--out", required=True)
    ex.add_argument("--limit-mb", type=int, default=250, help="stop after this much uncompressed")
    fm = sub.add_parser("fetch-member")
    fm.add_argument("--match", required=True, help="substring of the member path")
    fm.add_argument("--out", required=True)
    fm.add_argument("--parallel", type=int, default=12)
    args = parser.parse_args(argv[1:])
    if not args.url:
        if not args.portal_os_seq:
            raise SystemExit("give --url or --portal-os-seq")
        args.url = mint_url(args.portal_os_seq, args.portal_file_type)
        print("minted:", args.url.split("?")[0], flush=True)

    if args.action == "fetch-member":
        fetch_member(args.url, args.match, Path(args.out), args.parallel)
        return 0

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
