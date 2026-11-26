#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""The two network tools this unit was missing still behave without a network.

neither is reachable through the package's shell scripts, so this runs them
directly:

  * net_iface.inc  - the interface ioctls must work on a real interface and
    refuse names that would be truncated,
  * dhcp_client   - the DISCOVER/OFFER/REQUEST/ACK exchange against a scripted
    server (the unit has no DHCP client at all, measured 2026-11-25),
  * mdns_adv      - the responder's records, plus one live unicast query to a
    real process on loopback (multicast is not usable in every CI sandbox).

Run: python3 tests/test_net_tools.py
"""
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
TESTS = ROOT / "tests"

CC = ["gcc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Wno-unused-parameter",
      "-Wno-unused-function"]


def fail(msg):
    print("FAIL: " + msg)
    sys.exit(1)


def build(out, source, extra=()):
    cmd = CC + list(extra) + [str(source), "-o", str(out)]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        fail("compile failed: %s\n%s%s" % (source, proc.stdout, proc.stderr))
    return out


def run(binary, *args, env=None, expect=0, timeout=60):
    proc = subprocess.run([str(binary), *args], capture_output=True, text=True,
                          timeout=timeout, env=env)
    if proc.returncode != expect:
        fail("%s %s -> rc=%d (expected %d)\n%s%s"
             % (binary.name, " ".join(args), proc.returncode, expect,
                proc.stdout, proc.stderr))
    return proc.stdout


def encode_name(name):
    out = b""
    for label in name.split("."):
        out += bytes([len(label)]) + label.encode()
    return out + b"\x00"


def query(name, rtype, txid=0x1234, qu=False):
    qclass = 0x8001 if qu else 1          # QU bit asks for a unicast answer
    return (struct.pack(">HHHHHH", txid, 0, 1, 0, 0, 0) + encode_name(name) +
            struct.pack(">HH", rtype, qclass))


def read_answer(payload):
    """Minimal mDNS reader: (records) as (name, type, class, ttl, rdata)."""
    txid, flags, qd, an, ns, ar = struct.unpack(">HHHHHH", payload[:12])
    off = 12

    def read_name(off):
        labels = []
        while True:
            length = payload[off]
            if length & 0xC0 == 0xC0:
                off = ((length & 0x3F) << 8) | payload[off + 1]
                continue
            off += 1
            if length == 0:
                return ".".join(labels), off
            labels.append(payload[off:off + length].decode())
            off += length

    for _ in range(qd):
        _, off = read_name(off)
        off += 4
    records = []
    for _ in range(an):
        name, off = read_name(off)
        rtype, rclass, ttl, rdlen = struct.unpack(">HHIH", payload[off:off + 10])
        off += 10
        rdata = payload[off:off + rdlen]
        off += rdlen
        records.append((name, rtype, rclass, ttl, rdata))
    return flags, records


def live_mdns(binary):
    """One real process, one real unicast query on loopback."""
    port = 25353
    env = dict(os.environ, SCOURGE_MDNS_TEST_PORT=str(port))
    proc = subprocess.Popen(
        [str(binary), "-i", "lo", "--ip", "127.0.0.1", "--hostname", "scopetest",
         "--ssh-port", "2222", "--interval", "600", "-v"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
    try:
        deadline = time.time() + 5
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(1.0)
        answer = None
        while time.time() < deadline and answer is None:
            sock.sendto(query("scopetest.local", 1, 0x4242, qu=True),
                        ("127.0.0.1", port))
            try:
                answer = sock.recv(1500)
            except socket.timeout:
                continue
        if answer is None:
            fail("mdns_adv did not answer a unicast query:\n%s%s"
                 % (proc.stdout.read() if proc.poll() is not None else "",
                    proc.stderr.read() if proc.poll() is not None else ""))
        flags, records = read_answer(answer)
        if struct.unpack(">H", answer[:2])[0] != 0x4242:
            fail("the unicast answer did not echo the query id")
        addrs = [r for r in records if r[1] == 1]
        if not addrs or addrs[0][0] != "scopetest.local":
            fail("no A record for scopetest.local in %r" % (records,))
        if addrs[0][4] != socket.inet_aton("127.0.0.1"):
            fail("A record carries %r" % (addrs[0][4],))

        # A name we do not serve must stay unanswered.
        sock.settimeout(0.6)
        sock.sendto(query("elsewhere.lan", 1, 0x4243, qu=True),
                    ("127.0.0.1", port))
        try:
            extra = sock.recv(1500)
            fail("answered a foreign name: %r" % (read_answer(extra),))
        except socket.timeout:
            pass
        print("mdns_adv: live unicast query on lo answered (A 127.0.0.1), "
              "foreign name ignored")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


def main():
    with tempfile.TemporaryDirectory(prefix="wifi-nettools-") as tmp:
        tmp = Path(tmp)

        neti = build(tmp / "neti_tu", TESTS / "net_iface_tu.c")
        print(run(neti).strip())

        dhcp = build(tmp / "dhcp_client_tu", TESTS / "dhcp_client_tu.c")
        print(run(dhcp, timeout=120).strip().splitlines()[-1])

        mdns = build(tmp / "mdns_adv_tu", TESTS / "mdns_adv_tu.c")
        print(run(mdns).strip().splitlines()[-1])

        # The two binaries must also build strictly, the way the package builds
        # them, and both must refuse bad arguments instead of guessing.
        client = build(tmp / "dhcp_client", SRC / "dhcp_client.c")
        responder = build(tmp / "mdns_adv", SRC / "mdns_adv.c")
        run(client, "--bogus", expect=1)
        run(responder, "--bogus", expect=1)
        print("dhcp_client and mdns_adv build standalone and refuse bad arguments")

        live_mdns(responder)

    print("PASS: net_iface, dhcp_client and mdns_adv (including one live query)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
