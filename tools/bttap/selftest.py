#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Self-test for hci_decode.py: synthetic H4 stream, assert the RFCOMM line."""
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent


def acl(body, handle=0x0041):
    return b'\x02' + struct.pack('<H', handle | 0x2000) + struct.pack('<H', len(body)) + body


def l2cap(cid, payload):
    return struct.pack('<H', len(payload)) + struct.pack('<H', cid) + payload


def main():
    data = b"\x01" + struct.pack("<H", 0x0C03) + b"\x00"
    data += b"\x04\x0e\x04\x01\x03\x0c\x00"
    data += acl(l2cap(0x0001, b"\x01\x02\x03\x00"))
    data += acl(l2cap(0x0043, b"\x03\xef\x0c\x00hello-aa"))
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as fh:
        fh.write(data)
        path = fh.name
    out = subprocess.run([sys.executable, str(HERE / "hci_decode.py"), path],
                         capture_output=True, text=True, check=True).stdout
    assert "L2CAP signalling" in out, repr(out)
    assert "RFCOMM" in out and "dlci=0" in out, repr(out)
    assert "'rfcomm': 1" in out, repr(out)
    print("PASS: decoder reports L2CAP signalling + one RFCOMM UIH frame")


if __name__ == "__main__":
    main()