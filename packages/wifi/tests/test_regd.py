#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""regd: the CRDA stand-in must read the shipped database and build the exact
NL80211_CMD_SET_REG frame cfg80211 expects.

Kernel 4.1.27-abl has no regulatory.bin, no CRDA and no /lib/crda, so every
regulatory hint leaves cfg80211 on country 00 (world).  src/regd.c answers it
from the signed regulatory.db we ship.  Nothing on the car verifies the wire
format for us, so this test does:

  * the country rules regd prints must equal the published wireless-regdb rules
    for HU (the mW figures in db.txt are the cross-check: 100 mW -> 2000 cBm,
    200 mW -> 2301 cBm, 500 mW -> 2698 cBm, 25 mW -> 1397 cBm),
  * the frame it builds must be a SET_REG with the alpha2, the DFS region and
    one nested rule attribute per rule, carrying exactly those numbers,
  * a corrupt or truncated database must fail closed (exit 2), never install an
    empty regdomain,
  * the uevent parser must find COUNTRY= in a kobject payload.

Run: python3 tests/test_regd.py
"""
import os
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src" / "regd.c"
REGDB = Path(os.environ.get("SCOURGE_REGDB",
                            ROOT / "build" / "regdb" / "regulatory.db"))

NL80211_CMD_SET_REG = 26
NL80211_ATTR_REG_ALPHA2 = 33
NL80211_ATTR_REG_RULES = 34
NL80211_ATTR_DFS_REGION = 146
NLA_F_NESTED = 0x8000
NLA_TYPE_MASK = 0x3FFF
RRF = {
    "NO-OFDM": 1 << 0, "NO-CCK": 1 << 1, "NO-INDOOR": 1 << 2,
    "NO-OUTDOOR": 1 << 3, "DFS": 1 << 4, "PTP-ONLY": 1 << 5,
    "PTMP-ONLY": 1 << 6, "NO-IR": 1 << 7, "AUTO-BW": 1 << 11,
    "IR-CONCURRENT": 1 << 12,
}

# wireless-regdb HU (DFS-ETSI), as published.  (start kHz, end kHz, max bw kHz,
# eirp cBm, kernel rule flags) - the eirp column is the db.txt power in mW
# converted to cBm, which is what db2fw stores.
HU_RULES = [
    (2400000, 2483500, 40000, 2000, 0),                  # 100 mW
    (5150000, 5250000, 80000, 2301, RRF["NO-OUTDOOR"] | RRF["AUTO-BW"]),        # 200 mW
    (5250000, 5350000, 80000, 2000,
     RRF["NO-OUTDOOR"] | RRF["DFS"] | RRF["AUTO-BW"]),   # 100 mW
    (5470000, 5725000, 160000, 2698, RRF["DFS"]),        # 500 mW
    (5725000, 5875000, 80000, 1397, 0),                  # 25 mW  <- ch149..165
    (5945000, 6425000, 320000, 2300, RRF["NO-OUTDOOR"]),  # 200 mW
    (57000000, 66000000, 2160000, 4000, 0),              # 40 dBm
]


def fail(msg):
    print("FAIL: " + msg)
    sys.exit(1)


def build(tmp):
    binary = Path(tmp) / "regd"
    subprocess.run(["gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-Wno-format-truncation",
                    str(SRC), "-o", str(binary)], check=True)
    return binary


def run(binary, *args, expect=0):
    proc = subprocess.run([str(binary), *args], capture_output=True, text=True,
                          timeout=30)
    if proc.returncode != expect:
        fail("%s -> rc=%d (expected %d)\nstdout: %s\nstderr: %s"
             % (" ".join(args), proc.returncode, expect, proc.stdout,
                proc.stderr))
    return proc.stdout


def parse_dump(text):
    rules = []
    dfs = None
    for line in text.splitlines():
        if line.startswith("country "):
            fields = dict(part.split("=", 1) for part in line.split()[2:])
            dfs = int(fields["dfs_region"])
            continue
        if not line.startswith("  "):
            continue
        m = re.match(r"\s+(\d+)-\s*(\d+) kHz bw=\s*(\d+) kHz "
                     r"eirp=\s*(\d+) cBm flags=(\S+)\s*$", line)
        if not m:
            fail("unparsable rule line: %r" % line)
        start, end, bw, eirp, name_list = m.groups()
        flags = 0
        if name_list != "-":
            for name in name_list.split(","):
                flags |= RRF[name]
        rules.append((int(start), int(end), int(bw), int(eirp), flags))
    return dfs, rules


def parse_attrs(blob, offset=0):
    """Walk a netlink attribute stream into {type: payload or [children]}."""
    out = []
    while offset + 4 <= len(blob):
        alen, atype = struct.unpack_from("<HH", blob, offset)
        if alen < 4 or offset + alen > len(blob):
            fail("attribute length %d overruns the frame at %d" % (alen, offset))
        payload = blob[offset + 4:offset + alen]
        if atype & NLA_F_NESTED:
            out.append((atype & NLA_TYPE_MASK, parse_attrs(payload)))
        else:
            out.append((atype & NLA_TYPE_MASK, payload))
        offset += (alen + 3) & ~3
    return out


def one(attrs, want, what):
    hits = [v for (t, v) in attrs if t == want]
    if len(hits) != 1:
        fail("%s: attribute %d appears %d times" % (what, want, len(hits)))
    return hits[0]


def u32(payload):
    return struct.unpack("<I", payload)[0]


def check_frame(binary, rules, dfs):
    text = run(binary, "--dump-message", "--country", "HU",
               "--db", str(REGDB), "--family-id", "42")
    blob = bytes.fromhex("".join(text.split()))
    if len(blob) < 20:
        fail("the SET_REG frame is only %d bytes" % len(blob))
    nlmsg_len, nlmsg_type, nlmsg_flags, _seq = struct.unpack_from("<IHHI", blob, 0)
    if nlmsg_len != len(blob):
        fail("nlmsg_len %d != frame length %d" % (nlmsg_len, len(blob)))
    if nlmsg_type != 42:
        fail("nlmsg_type %d, expected the family id" % nlmsg_type)
    if nlmsg_flags & 0x5 != 0x5:
        fail("nlmsg_flags 0x%x lacks REQUEST|ACK" % nlmsg_flags)
    cmd = blob[16]
    if cmd != NL80211_CMD_SET_REG:
        fail("genl command %d, expected NL80211_CMD_SET_REG" % cmd)

    attrs = parse_attrs(blob, 20)
    if one(attrs, NL80211_ATTR_REG_ALPHA2, "alpha2") != b"HU":
        fail("alpha2 attribute is not HU")
    if one(attrs, NL80211_ATTR_DFS_REGION, "dfs") != bytes([dfs]):
        fail("DFS region %d does not match the database" % dfs)
    nested = one(attrs, NL80211_ATTR_REG_RULES, "rules")
    if len(nested) != len(rules):
        fail("frame carries %d rules, the database has %d"
             % (len(nested), len(rules)))
    for idx, (rtype, children) in enumerate(nested):
        if rtype != idx + 1:
            fail("rule attribute %d is indexed %d, not from 1" % (idx, rtype))
        got = (
            u32(one(children, 2, "start")),
            u32(one(children, 3, "end")),
            u32(one(children, 4, "max bw")),
            u32(one(children, 6, "max eirp")),
            u32(one(children, 1, "flags")),
        )
        if u32(one(children, 5, "max ant gain")) != 0:
            fail("MAX_ANT_GAIN must be 0: the database never carries one")
        if got != rules[idx]:
            fail("rule %d on the wire is %r, the database says %r"
                 % (idx, got, rules[idx]))
    return blob


def main():
    if not REGDB.exists():
        print("SKIP: %s missing (run make payload to fetch the pinned regdb)"
              % REGDB)
        return 0
    with tempfile.TemporaryDirectory(prefix="wifi-regd-") as tmp:
        binary = build(tmp)

        text = run(binary, "--dump", "--country", "HU", "--db", str(REGDB))
        dfs, rules = parse_dump(text)
        if dfs != 2:
            fail("HU must be DFS-ETSI (region 2), got %d" % dfs)
        if rules != HU_RULES:
            for got, want in zip(rules, HU_RULES):
                if got != want:
                    fail("HU rule differs: %r != %r" % (got, want))
            fail("HU rule count %d, expected %d" % (len(rules), len(HU_RULES)))

        # The reason regd exists: HU allows 25 mW in 5725-5875 (ch149..165) with
        # no NO-IR, so the 5 GHz AP may beacon there once the regdomain is HU.
        band = [r for r in rules if r[0] <= 5745000 <= r[1]]
        if not band or band[0][4] & RRF["NO-IR"]:
            fail("HU lost its 5725-5875 MHz band (ch149)")
        print("HU: 5745 MHz (ch149) allowed at %.2f dBm, no NO-IR"
              % (band[0][3] / 100.0))

        check_frame(binary, rules, dfs)
        print("SET_REG frame: alpha2, dfs_region and %d nested rules verified"
              % len(rules))

        # US is FCC (dfs_region 1) - the attribute must follow the database.
        us_text = run(binary, "--dump", "--country", "US", "--db", str(REGDB))
        us_dfs, us_rules = parse_dump(us_text)
        if us_dfs != 1:
            fail("US must be FCC (region 1), got %d" % us_dfs)
        us_blob = run(binary, "--dump-message", "--country", "US",
                      "--db", str(REGDB), "--family-id", "42")
        us_attrs = parse_attrs(bytes.fromhex("".join(us_blob.split())), 20)
        if one(us_attrs, NL80211_ATTR_DFS_REGION, "dfs") != b"\x01":
            fail("US frame does not carry DFS region 1")
        print("US: %d rules, DFS region 1" % len(us_rules))

        # Failing closed: a database we do not understand must never produce an
        # empty set_reg (cfg80211 would then drop the radio to world).
        bad = Path(tmp) / "regulatory.db"
        bad.write_bytes(b"XXXX" + REGDB.read_bytes()[4:])
        run(binary, "--dump", "--country", "HU", "--db", str(bad), expect=2)
        truncated = Path(tmp) / "truncated.db"
        truncated.write_bytes(REGDB.read_bytes()[:32])
        run(binary, "--dump", "--country", "HU", "--db", str(truncated),
            expect=2)
        run(binary, "--dump", "--country", "HU",
            "--db", str(Path(tmp) / "nope.db"), expect=2)
        run(binary, "--dump", "--country", "ZZ", "--db", str(REGDB), expect=3)
        run(binary, "--dump", "--country", "h", "--db", str(REGDB), expect=1)
        print("corrupt/missing database and unknown country fail closed")

        # Live kernel (optional): reading the regdomain back needs no privileges
        # and exercises the generic-netlink path this tool depends on.  A host
        # without cfg80211 loaded is not a failure of this package.
        proc = subprocess.run([str(binary), "--read"], capture_output=True,
                              text=True, timeout=30)
        if proc.returncode == 0:
            live_dfs, live_rules = parse_dump(proc.stdout)
            print("live kernel: country %s, %d rules read back over nl80211"
                  % (proc.stdout.split()[1], len(live_rules)))
        else:
            print("live kernel: skipped (%s)"
                  % proc.stderr.strip().splitlines()[-1:])

        # uevent parsing: '|' stands in for the NUL separator argv cannot carry.
        out = run(binary, "--parse-uevent",
                  "action@/devices/platform/regulatory.0|ACTION=change|"
                  "DEVPATH=/devices/platform/regulatory.0|COUNTRY=GB|SEQNUM=7")
        if out.strip() != "GB":
            fail("uevent parser returned %r" % out)
        run(binary, "--parse-uevent", "ACTION=add|DEVPATH=/devices/x", expect=1)
        print("uevent parser: COUNTRY= found, a foreign event is ignored")
    print("PASS: regd parses the pinned regdb and builds a valid SET_REG frame")
    return 0


if __name__ == "__main__":
    sys.exit(main())
