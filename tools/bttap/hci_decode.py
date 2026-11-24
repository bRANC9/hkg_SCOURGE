#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Decode a tapped HCI stream (H4 framing) down to L2CAP and RFCOMM.

Why: the head unit has no BlueZ and no kernel HCI device - the vendor daemon
(vbtd) owns /dev/ttyS0 and speaks HCI itself.  To learn whether its stack can
carry an Android Auto RFCOMM channel, we have to look at the wire.  This decoder
turns a tap of that wire (see README.md) into a readable list: HCI events, ACL
frames, L2CAP channels (signalling 0x0001, SDP 0x0003, RFCOMM 0x0003 PSM 0x0003)
and RFCOMM frames (SABM/UA/DM/DISC/UIH with DLCI and credits).

usage: hci_decode.py <tap.bin> [--full]
"""
import sys

H4 = {1: "CMD", 2: "ACL", 3: "SCO", 4: "EVT", 5: "ISO"}
RFCOMM_PSM = 0x0003


def rfcomm_summary(payload: bytes) -> str:
    if len(payload) < 4:
        return "rfcomm: short"
    addr = payload[0]
    ctrl = payload[1]
    dlci = addr >> 2
    kinds = {0x2F: "SABM", 0x63: "UA", 0x0F: "DM", 0x43: "DISC", 0xEF: "UIH"}
    kind = kinds.get(ctrl, f"ctrl=0x{ctrl:02x}")
    out = f"RFCOMM dlci={dlci} {kind}"
    if kind == "UIH" and len(payload) > 4:
        out += f" len={payload[2] >> 1} credits={payload[3]}"
    return out


def l2cap_summary(acl: bytes) -> str:
    if len(acl) < 4:
        return "l2cap: short"
    dlen = int.from_bytes(acl[0:2], "little")
    cid = int.from_bytes(acl[2:4], "little")
    body = acl[4:4 + dlen]
    if cid == 0x0001:
        return "L2CAP signalling (SDP/RFCOMM setup happens here)"
    if cid == 0x0003 or cid >= 0x0040:
        return f"L2CAP cid=0x{cid:04x} " + rfcomm_summary(body)
    return f"L2CAP cid=0x{cid:04x} len={dlen}"


def decode(path: str, full: bool) -> int:
    try:
        data = open(path, "rb").read()
    except OSError as error:
        # The tap file only exists after a run with the `bt_tap_run` marker, so
        # "not there yet" is the normal case, not a crash.
        print(f"cannot read {path}: {error}", file=sys.stderr)
        return 2
    if not data:
        print(f"{path} is empty: the tap was attached but nothing crossed it", file=sys.stderr)
        return 2
    at, n, counts = 0, 0, {}
    acl_psm = {}
    while at + 1 < len(data):
        kind = data[at]
        name = H4.get(kind, f"0x{kind:02x}")
        at += 1
        if kind == 1:      # command: opcode(2) len(1) body
            if at + 3 > len(data):
                break
            op = int.from_bytes(data[at:at + 2], "little")
            ln = data[at + 2]
            body = data[at + 3:at + 3 + ln]
            at += 3 + ln
            counts["cmd"] = counts.get("cmd", 0) + 1
            if full:
                print(f"  CMD  opcode=0x{op:04x} len={ln} {body.hex()}")
        elif kind == 2:    # ACL: handle+flags(2) len(2) body
            if at + 4 > len(data):
                break
            handle = int.from_bytes(data[at:at + 2], "little")
            ln = int.from_bytes(data[at + 2:at + 4], "little")
            body = data[at + 4:at + 4 + ln]
            at += 4 + ln
            counts["acl"] = counts.get("acl", 0) + 1
            if full:
                print(f"  ACL  handle=0x{handle & 0x0fff:03x} pb={(handle >> 12) & 3} "
                      f"len={ln} :: {l2cap_summary(body)}")
            else:
                s = l2cap_summary(body)
                # Only an actual RFCOMM frame counts; the signalling summary line
                # also mentions the word RFCOMM.
                if "signalling" in s:
                    counts["l2cap_signalling"] = counts.get("l2cap_signalling", 0) + 1
                    print(f"  {s}")
                elif s.startswith("L2CAP cid") and "RFCOMM" in s:
                    counts["rfcomm"] = counts.get("rfcomm", 0) + 1
                    print(f"  {s}")
        elif kind == 4:    # event: code(1) len(1) body
            if at + 2 > len(data):
                break
            code = data[at]
            ln = data[at + 1]
            body = data[at + 2:at + 2 + ln]
            at += 2 + ln
            counts["evt"] = counts.get("evt", 0) + 1
            if code == 0x3E and body:      # LE meta
                counts["le_meta"] = counts.get("le_meta", 0) + 1
            if full:
                print(f"  EVT  0x{code:02x} len={ln} {body.hex()}")
        else:
            break
        n += 1
    print(f"decoded {n} H4 packets: {counts}")
    if counts.get("acl") and not counts.get("rfcomm"):
        print("no RFCOMM seen: the stack is not carrying a serial profile in this tap")
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        print(__doc__)
        sys.exit(0 if len(sys.argv) > 1 else 2)
    sys.exit(decode(sys.argv[1], "--full" in sys.argv))
