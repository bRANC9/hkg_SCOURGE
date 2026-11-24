#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Transport test: a scripted phone over a real TCP socket.

The harness listens on loopback with the deterministic stand-in provider and
serves one peer; this test plays the phone over the socket and asserts the
session reaches READY with the EV sensor started.  It is the first test where
the wire is a socket rather than a buffer.
"""
from __future__ import annotations

import os
from pathlib import Path
import socket
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
BUILD_ROOT = Path("/tmp/opencode/aa")

COMMON = [
    "-std=c11",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-pedantic",
    "-Wshadow",
    "-Wstrict-prototypes",
    "-Wmissing-prototypes",
]
SOURCES = [
    ROOT / "tests" / "aa_tcp_harness.c",
    ROOT / "transport" / "aa_tcp.c",
    SRC / "aa_wire.c",
    SRC / "aa_channels.c",
    SRC / "aa_proto.c",
    SRC / "aa_control.c",
    SRC / "aa_tls.c",
    SRC / "aa_sensor.c",
    SRC / "aa_session.c",
]

CONTROL = 0
SENSOR = 2
XOR = 0x5A


def build() -> Path:
    BUILD_ROOT.mkdir(parents=True, exist_ok=True)
    binary = BUILD_ROOT / "aa_tcp_harness"
    cmd = [os.environ.get("CC", "cc"), *COMMON, "-o", str(binary)]
    cmd.extend(str(path) for path in SOURCES)
    subprocess.run(cmd, check=True, timeout=120)
    return binary


def frame(channel: int, payload: bytes, encrypted: bool) -> bytes:
    body = bytes(b ^ XOR for b in payload) if encrypted else payload
    return bytes([channel, 0x0B if encrypted else 0x03]) + len(body).to_bytes(2, "big") + body


def read_frame(sock: socket.socket) -> tuple[int, bytes, bool]:
    header = b""
    while len(header) < 4:
        chunk = sock.recv(4 - len(header))
        if not chunk:
            raise AssertionError("peer closed while reading a frame header")
        header += chunk
    channel = header[0]
    encrypted = bool(header[1] & 0x08)
    length = int.from_bytes(header[2:4], "big")
    body = b""
    while len(body) < length:
        chunk = sock.recv(length - len(body))
        if not chunk:
            raise AssertionError("peer closed inside a frame")
        body += chunk
    if encrypted:
        body = bytes(b ^ XOR for b in body)
    return channel, body, encrypted


def main() -> int:
    binary = build()
    process = subprocess.Popen(
        [str(binary), "--port", "0"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
    )
    assert process.stdout is not None
    port = 0
    for _ in range(4):
        line = process.stdout.readline()
        if line.startswith("port="):
            port = int(line.split("=", 1)[1])
            break
    assert port, "harness did not report a port"

    with socket.create_connection(("127.0.0.1", port), timeout=15) as phone:
        phone.settimeout(15)

        # The head unit opens with its version request.
        channel, body, encrypted = read_frame(phone)
        assert channel == CONTROL and not encrypted, (channel, encrypted)
        assert body == bytes.fromhex("000100010000"), body.hex()

        # Version response: matching version, status 0.
        phone.sendall(frame(CONTROL, bytes.fromhex("0002000100000000"), False))

        # Our TLS stand-in sends its first record as a plain control message.
        channel, body, encrypted = read_frame(phone)
        assert channel == CONTROL and not encrypted, (channel, encrypted)
        assert body.hex() == "000348454c4c4f31", body.hex()

        # The peer's record completes the handshake; auth complete follows.
        phone.sendall(frame(CONTROL, bytes.fromhex("000348454c4c4f32"), False))
        channel, body, _ = read_frame(phone)
        assert body.hex() == "000346494e4953484544", body.hex()
        channel, body, encrypted = read_frame(phone)
        assert body.hex() == "00040800" and not encrypted, (body.hex(), encrypted)

        # Discovery is encrypted from here on.
        phone.sendall(frame(CONTROL, bytes.fromhex("0005220550686f6e65"), True))
        channel, body, encrypted = read_frame(phone)
        assert channel == CONTROL and encrypted, (channel, encrypted)
        assert body[:2] == b"\x00\x06", body[:2].hex()

        # Sensor channel open, then the EV sensor start.
        phone.sendall(frame(SENSOR, bytes.fromhex("000708001002"), True))
        channel, body, encrypted = read_frame(phone)
        assert channel == SENSOR and encrypted and body.hex() == "00080800", (channel, body.hex())

        phone.sendall(frame(SENSOR, bytes.fromhex("8001081710e807"), True))
        channel, body, encrypted = read_frame(phone)
        assert channel == SENSOR and body.hex() == "80020800", (channel, body.hex())

    # Closing the socket must end the serve loop cleanly, not as an I/O error.
    status = process.wait(timeout=20)
    assert status == 0, status
    assert process.stdout is not None
    summary = process.stdout.readline().strip()
    assert "served=peer_closed" in summary, summary
    assert "state=ready" in summary, summary
    assert "starts=1" in summary, summary
    assert "ev=yes" in summary, summary
    print("aa tcp transport: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
