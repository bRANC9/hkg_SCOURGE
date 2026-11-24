#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Build and run the standalone AA-core tests.

Three programs: the frame layer, the protobuf wire codec and the control-channel
state machine.  Each is built twice — the normal build treats warnings as
errors, the second re-runs the same logic under ASan/UBSan — and the
source-policy checks keep the engine transport-agnostic and dependency-free so
one codec can serve the USB and the wireless transport.

Build artifacts live under /tmp/opencode only.
"""
from __future__ import annotations

import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
TESTS = ROOT / "tests"
BUILD_ROOT = Path("/tmp/opencode")

PROGRAMS = [
    ("aa_wire", ["src/aa_wire.c", "src/aa_channels.c", "tests/aa_wire_test.c"]),
    ("aa_proto", ["src/aa_proto.c", "tests/aa_proto_test.c"]),
    (
        "aa_control",
        ["src/aa_proto.c", "src/aa_channels.c", "src/aa_control.c", "tests/aa_control_test.c"],
    ),
    ("aa_tls", ["src/aa_tls.c", "tests/aa_tls_test.c"]),
    ("aa_aoap", ["src/aa_aoap.c", "tests/aa_aoap_test.c"]),
    (
        "aa_btboot",
        ["src/aa_btboot.c", "src/aa_proto.c", "tests/aa_btboot_test.c"],
    ),
    (
        "aa_ap",
        ["src/aa_ap.c", "src/aa_btboot.c", "src/aa_proto.c", "tests/aa_ap_test.c"],
    ),
    (
        "aa_mirror",
        [
            "src/aa_mirror.c",
            "src/aa_proto.c",
            "src/aa_wire.c",
            "src/aa_channels.c",
            "src/aa_control.c",
            "src/aa_sensor.c",
            "tests/aa_mirror_test.c",
        ],
    ),
    (
        "aa_sensor",
        ["src/aa_proto.c", "src/aa_channels.c", "src/aa_sensor.c", "tests/aa_sensor_test.c"],
    ),
    (
        "aa_session",
        [
            "src/aa_wire.c",
            "src/aa_channels.c",
            "src/aa_proto.c",
            "src/aa_control.c",
            "src/aa_tls.c",
            "src/aa_sensor.c",
            "src/aa_session.c",
            "tests/aa_session_test.c",
        ],
    ),
    (
        "aa_wireless",
        [
            "src/aa_wireless.c",
            "src/aa_ap.c",
            "src/aa_btboot.c",
            "src/aa_proto.c",
            "src/aa_wire.c",
            "src/aa_channels.c",
            "src/aa_control.c",
            "src/aa_tls.c",
            "src/aa_sensor.c",
            "src/aa_session.c",
            "tests/aa_wireless_test.c",
        ],
    ),
    (
        "aa_dump",
        [
            "src/aa_wire.c",
            "src/aa_channels.c",
            "src/aa_proto.c",
            "src/aa_control.c",
            "src/aa_sensor.c",
            "tests/aa_dump.c",
        ],
    ),
]

COMMON = [
    "-std=c11",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-pedantic",
    "-Wshadow",
    "-Wstrict-prototypes",
    "-Wmissing-prototypes",
    "-Wunused-const-variable=1",
]
SANITIZERS = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]

# Programs that exist to be driven by the differential checks instead of run on
# their own (their no-argument run would only print usage).
TOOLS = {"aa_dump"}


def compile_and_run(tmp: Path, program: str, sources: list[str], extra: list[str],
                    env_extra: dict[str, str]) -> None:
    tmp.mkdir(parents=True, exist_ok=True)
    binary = tmp / f"{program}_{'asan' if extra else 'plain'}"
    cmd = [os.environ.get("CC", "cc"), *COMMON, *extra, "-o", str(binary)]
    cmd.extend(str(ROOT / source) for source in sources)
    subprocess.run(cmd, check=True, timeout=120)
    if program in TOOLS:
        return
    env = dict(os.environ)
    env.update(env_extra)
    subprocess.run([str(binary)], check=True, timeout=120, env=env)


def assert_source_policy() -> None:
    engine = {path: path.read_text() for path in sorted(SRC.iterdir()) if path.suffix in {".c", ".h"}}
    test_sources = {path: path.read_text() for path in sorted(TESTS.iterdir()) if path.suffix == ".c"}
    harness = Path(__file__).read_text()

    for path, text in {**engine, **test_sources, Path(__file__): harness}.items():
        assert "SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors" in text, path
        assert "SPDX-License-Identifier: AGPL-3.0-only" in text, path

    # No allocation, sockets, threading or I/O in the engine.
    for path, text in engine.items():
        for forbidden in (
            r"\bmalloc\s*\(",
            r"\bcalloc\s*\(",
            r"\brealloc\s*\(",
            r"\bfree\s*\(",
            r"\bsocket\s*\(",
            r"\bconnect\s*\(",
            r"\bbind\s*\(",
            r"\blisten\s*\(",
            r"\baccept\s*\(",
            r"\bpthread_",
            r"\bprintf\s*\(",
            r"\bfprintf\s*\(",
            r"\bfopen\s*\(",
        ):
            assert not re.search(forbidden, text), (path, forbidden)

    # No protobuf runtime, TLS library or reference-library dependency.
    for path, text in engine.items():
        for forbidden in ("#include <openssl", "#include <google", "f1x/", "namespace"):
            assert forbidden not in text, (path, forbidden)

    # The frame layer keeps its wire facts and bounds in the public header.
    wire_header = (SRC / "aa_wire.h").read_text()
    for needle in (
        "AA_WIRE_FLAG_FIRST 0x01u",
        "AA_WIRE_FLAG_LAST 0x02u",
        "AA_WIRE_FLAG_CONTROL 0x04u",
        "AA_WIRE_FLAG_ENCRYPTED 0x08u",
        "AA_WIRE_MAX_FRAME_PAYLOAD 0x4000u",
        "AA_WIRE_MAX_MESSAGE",
        "transport-agnostic",
        "big-endian",
        "aa_wire_decode_init",
        "aa_wire_encode_message",
        "aa_wire_decode_message",
    ):
        assert needle in wire_header, needle

    # The channel vocabulary keeps the control message IDs this tree uses.
    channels = (SRC / "aa_channels.h").read_text()
    for needle in (
        "AA_CHANNEL_CONTROL = 0",
        "AA_CHANNEL_BLUETOOTH = 8",
        "AA_CHANNEL_NAVIGATION = 9",
        "AA_CONTROL_VERSION_REQUEST 0x0001u",
        "AA_CONTROL_SERVICE_DISCOVERY_RESPONSE 0x0006u",
        "AA_CONTROL_CHANNEL_OPEN_REQUEST 0x0007u",
        "AA_CONTROL_PING_REQUEST 0x000Bu",
        "AA_FEATURE_EV_ENERGY",
        "AA_FEATURE_NAVIGATION",
    ):
        assert needle in channels, needle

    # The AP seam must keep the three fields Wifi1b does not expose yet.
    ap_header = (SRC / "aa_ap.h").read_text()
    for needle in (
        "AA_AP_DEFAULT_PORT 5288u",
        "aa_ap_from_readback",
        "aa_ap_btboot_info",
        "aa_ap_btboot_start",
        "10.42.0.1/24",
        "wpa_cli status",
    ):
        assert needle in ap_header, needle

    # The phone-role mirror must keep the conversation and the EV report.
    mirror_header = (SRC / "aa_mirror.h").read_text()
    for needle in (
        "AA_MIRROR_DEVICE_NAME",
        "AA_MIRROR_SENSOR_INTERVAL_MS",
        "AA_MIRROR_EVENT_EV_MODEL",
        "aa_mirror_on_message",
        "aa_mirror_set_secure",
    ):
        assert needle in mirror_header, needle

    # The wireless path must keep its order and its boundaries.
    wireless_header = (SRC / "aa_wireless.h").read_text()
    for needle in (
        "aa_wireless_begin",
        "aa_wireless_on_bt_packet",
        "aa_wireless_ready_for_tcp",
        "aa_wireless_start_session",
        "aa_wireless_on_tcp",
        "about 30 KiB",
    ):
        assert needle in wireless_header, needle

    # The control header must keep the pinned message shapes and its policy.
    control_header = (SRC / "aa_control.h").read_text()
    for needle in (
        "device_name = 4",
        "head_unit_name = 2",
        "navigation_channel = 8",
        "minimum_interval_ms = 1",
        "channel_id = 2",
        "timestamp = 1",
        "AA_CONTROL_ERR_VERSION",
        "AA_CONTROL_MAX_PAYLOAD",
        "ignored",
    ):
        assert needle in control_header, needle

    # The TLS seam must stay crypto-free and bounded.
    tls_header = (SRC / "aa_tls.h").read_text()
    for needle in (
        "AA_TLS_MAX_HANDSHAKE_BLOB",
        "AA_TLS_MAX_ROUNDS",
        "AA_TLS_MAX_PLAINTEXT",
        "aa_tls_provider",
        "aa_tls_begin",
        "aa_tls_on_handshake",
        "aa_tls_is_established",
    ):
        assert needle in tls_header, needle

    # The wireless bootstrap must keep its packet table and its named guesses.
    btboot_header = (SRC / "aa_btboot.h").read_text()
    for needle in (
        "AA_BTBOOT_MSG_START_REQUEST 1u",
        "AA_BTBOOT_MSG_INFO_REQUEST 2u",
        "AA_BTBOOT_MSG_INFO_RESPONSE 3u",
        "AA_BTBOOT_MSG_CONNECT_STATUS 6u",
        "AA_BTBOOT_MSG_START_RESPONSE 7u",
        "AA_BTBOOT_SECURITY_WPA2_PERSONAL 8u",
        "aa_btboot_set_framing",
        "aa_btboot_on_packet",
    ):
        assert needle in btboot_header, needle

    # The AOA responder must keep the request table and the role facts.
    aoap_header = (SRC / "aa_aoap.h").read_text()
    for needle in (
        "AA_AOAP_REQ_GET_PROTOCOL 51u",
        "AA_AOAP_REQ_SEND_STRING 52u",
        "AA_AOAP_REQ_START 53u",
        "AA_AOAP_PROTOCOL_VERSION 2u",
        "AA_AOAP_GOOGLE_VENDOR_ID 0x18D1u",
        "AA_AOAP_ACCESSORY_PRODUCT_ID 0x2D00u",
        "SSL_set_connect_state",
        "aa_aoap_on_control",
    ):
        assert needle in aoap_header, needle

    # The sensor channel must keep the EV facts that are easy to get wrong.
    sensor_header = (SRC / "aa_sensor.h").read_text()
    for needle in (
        "AA_SENSOR_EV_ENERGY_MODEL_TYPE 23u",
        "AA_SENSOR_START_REQUEST 0x8001u",
        "AA_SENSOR_START_RESPONSE 0x8002u",
        "AA_SENSOR_EVENT_INDICATION 0x8003u",
        "min_usable_capacity",
        "current* battery energy",
        "Wh per km",
        "aa_sensor_encode_ev_energy",
    ):
        assert needle in sensor_header, needle

    # The session glue must keep the encryption table and its bounds visible.
    session_header = (SRC / "aa_session.h").read_text()
    for needle in (
        "AA_SESSION_MAX_PAYLOAD 4096u",
        "AA_SESSION_QUEUE_DEPTH 4u",
        "aa_session_feed",
        "aa_session_peek",
        "aa_session_consume",
        "An encrypted frame is decrypted",
        "message ID is read, because the ID is inside the encrypted",
        "AA_SESSION_ERR_TLS",
    ):
        assert needle in session_header, needle

    # The protobuf codec must stay schema-free and bounded.
    proto_header = (SRC / "aa_proto.h").read_text()
    for needle in (
        "AA_PROTO_MAX_VARINT_BYTES 10u",
        "AA_PROTO_ERR_VARINT",
        "AA_PROTO_ERR_UTF8",
        "aa_proto_begin_message",
        "aa_proto_end_message",
        "No schema, no reflection",
    ):
        assert needle in proto_header, needle

    # Fixed external vectors, not only codec round-trips.
    vectors = {
        "aa_wire_test.c": [
            "00030006000100010000",
            "000b0009000608011203616263",
            "030100040000000641414141",
        ],
        "aa_proto_test.c": ["0a0308ac02", "808001", "0a0208020a020809"],
        "aa_control_test.c": [
            "000100010000",
            "220550686f6e652a07416e64726f6964",
            "00080800",
            "000c08ac02",
        ],
        "aa_session_test.c": [
            "000100010000",
            "000348454c4c4f31",
            "000346494e4953484544",
            "00040800",
            "00080800",
            "000c08ac02",
            "8001081710e807",
            "80020800",
        ],
        "aa_wireless_test.c": [
            "001000010a0931302e34322e302e3110a829",
            "10.42.0.1",
            "0a0a53434f555247452d4141",
            "1a1144433a41363a33323a45373a35413a4645",
            "000100010000",
        ],
        "aa_ap_test.c": [
            "10.42.0.1",
            "001000010a0931302e34322e302e3110a829",
            "DC:A6:32:E7:5A:FE",
            "AA_AP_DEFAULT_PORT",
        ],
        "aa_mirror_test.c": [
            "AA_CONTROL_VERSION_REQUEST",
            "AA_MIRROR_EVENT_EV_MODEL",
            "AA_SENSOR_EV_ENERGY_MODEL_TYPE",
            "aa_sensor_encode_ev_energy",
            "AA_CONTROL_CHANNEL_OPEN_RESPONSE",
        ],
        "aa_btboot_test.c": [
            "001000010a0931302e302e322e313010a829",
            "0a0a53434f555247452d4141",
            "1a1144433a41363a33323a45373a35413a4645",
            "DC:A6:32:E7:5A:FE",
        ],
        "aa_aoap_test.c": [
            "AA_AOAP_REQ_GET_PROTOCOL",
            "AA_AOAP_REQ_SEND_STRING",
            "AA_AOAP_REQ_START",
            "0x18D1u",
            "0x2D00u",
        ],
        "aa_sensor_test.c": [
            "00080800",
            "80020800",
            "800342020865",
            "800352020801",
            "80033a020801",
            "80036a020808",
            "80031a020832",
        ],
    }
    for name, needles in vectors.items():
        text = (TESTS / name).read_text()
        for needle in needles:
            assert needle in text, (name, needle)


def assert_protoc_differential(tmp: Path) -> None:
    """Second opinion from the official protobuf tooling.

    The C codec is hand-written, so the same wire facts are stated twice: once
    in C, once in tests/proto/aa_differential.proto.  protoc decodes what our
    encoder produced, and our decoder parses what protoc encoded.  A drifting
    field number, wire type or nesting level fails here even if both our encoder
    and our decoder agree with each other.
    """
    protoc = shutil.which("protoc")
    if protoc is None or os.environ.get("AA_NO_PROTOC") == "1":
        print("aa differential: protoc not available, schema cross-check skipped")
        return

    schema = TESTS / "proto" / "aa_differential.proto"
    assert schema.is_file(), schema
    dump = tmp / "aa_dump_plain"
    assert dump.is_file(), dump

    # protoc only accepts files that live under its proto path, so run it from
    # the schema directory with the bare file name.
    def decode(mode: str, message: str) -> str:
        produced = subprocess.run([str(dump), mode], check=True, capture_output=True,
                                  timeout=60).stdout
        assert produced, mode
        result = subprocess.run(
            [protoc, f"--decode=aa_test.{message}", schema.name],
            input=produced, check=True, capture_output=True, timeout=60, cwd=str(schema.parent))
        return result.stdout.decode()

    discovery = decode("discovery-response", "ServiceDiscoveryResponse")
    assert discovery.count("channels {") == 3, discovery
    assert discovery.count("sensors {") == 6, discovery
    for needle in (
        "channel_id: 2",
        "channel_id: 9",
        "channel_id: 1",
        "type: 23",
        "type: 13",
        "minimum_interval_ms: 500",
        "colour_depth_bits: 32",
        "width: 1280",
        "height: 720",
        'head_unit_name: "SCOURGE AA"',
        "left_hand_drive_vehicle: true",
        "hide_clock: true",
    ):
        assert needle in discovery, (needle, discovery)

    energy = decode("ev-model", "SensorEventIndication")
    for needle in (
        "ev_energy_model {",
        "min_usable_capacity {",
        "watt_hours: 40400",
        "max_capacity {",
        "watt_hours: 80000",
        "reserve_energy {",
        "watt_hours: 4000",
        "max_charge_power_w: 150000",
        "max_discharge_power_w: 150000",
        "regen_braking_capable: true",
        "rate: 134.66",
        "charging_prefs {",
        "mode: 1",
    ):
        assert needle in energy, (needle, energy)

    gear = decode("gear", "SensorEventIndication")
    assert "gear {" in gear and "gear: 101" in gear, gear

    # Reverse direction: protoc encodes, our decoder parses.
    request_text = 'device_name: "Test Phone"\ndevice_brand: "Android"\n'
    encoded = subprocess.run([protoc, "--encode=aa_test.ServiceDiscoveryRequest", schema.name],
                             input=request_text.encode(), check=True, capture_output=True,
                             timeout=60, cwd=str(schema.parent)).stdout
    parsed = subprocess.run([str(dump), "parse-discovery"], input=encoded, check=True,
                            capture_output=True, timeout=60).stdout.decode().strip()
    assert parsed == "device_name=Test Phone", parsed


def main() -> int:
    assert_source_policy()
    tmp = BUILD_ROOT / "aa"
    sanitizers = os.environ.get("AA_NO_SANITIZERS") != "1"
    for program, sources in PROGRAMS:
        compile_and_run(tmp, program, sources, [], {})
        if sanitizers:
            compile_and_run(
                tmp,
                program,
                sources,
                SANITIZERS,
                {"ASAN_OPTIONS": "detect_leaks=1:abort_on_error=1"},
            )
    assert_protoc_differential(tmp)
    # The debug daemon has its own integration test (it owns a socket).
    subprocess.run([sys.executable, str(TESTS / "test_daemon.py")], check=True, timeout=300)
    # The TCP transport owns a socket too; same arrangement.
    subprocess.run([sys.executable, str(TESTS / "test_tcp.py")], check=True, timeout=300)
    # The vendor-API probe reads ELF dynamic symbol tables; fixtures are built
    # on the host, so this runs everywhere (probes/vbtd_api.c).
    subprocess.run([sys.executable, str(TESTS / "test_vbtd_api.py")], check=True, timeout=300)
    print("aa core checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
