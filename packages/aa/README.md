# AA core

The Android Auto side of the projection work: one protocol core, two
transports. Wired (USB AOAP) and wireless (Bluetooth bootstrap, 5 GHz AP, then
TLS over TCP) share the same frame layer, protobuf message set and channels, so
anything implemented above the transport — EV energy data, navigation
turn-by-turn, video, input — only has to be written once.

**Status: research-quality source only.** No daemon, no installer, no
activation, no phone and no car validation. The current checkpoints implement
the frame layer, the protobuf wire codec, the control-channel state machine, the
TLS seam, the session glue and the sensor channel with the EV energy model; all
of it is offline-tested (normal build plus ASan/UBSan) and wired into nothing.
Treat it exactly like the `wifi1b` CarPlay research modules: the tests passing
means these layers are sound, not that projection works.

## Why this exists

`docs/19-aa-core.md` carries the reasoning. In short:

- The head unit's stock Android Auto is wired-only and vendor-owned
  (`AppProjection`); the repo previously touched nothing but its D-Bus
  play-enable signal (`packages/tweaks/src/tweaks_daemon.c`).
- A transparent wireless-AA dongle cannot deliver the two features owners ask
  for: navigation turn-by-turn to the cluster/HUD, and battery state of charge
  to Google Maps. Both need a party inside the AA session, because both are
  messages the phone only receives from the head unit.
- The OEM projection stack on this unit speaks wired AA only, so the shared
  core is the only place where one implementation serves both transports.

## Layout

| Path | Contents |
|---|---|
| `src/aa_wire.h`, `src/aa_wire.c` | Frame codec: header/size parsing, fragmentation, bounded reassembly, strict validation. No allocation, no I/O, no logging. |
| `src/aa_channels.h`, `src/aa_channels.c` | Channel IDs, control message IDs, feature names. |
| `src/aa_proto.h`, `src/aa_proto.c` | Protobuf wire codec: varints, length-delimited fields, skipping, UTF-8 strings, nested-message writer that fails closed. |
| `src/aa_control.h`, `src/aa_control.c` | Control-channel state machine: version negotiation, auth complete, service discovery (sensor, navigation and input channels), channel open, ping, shutdown. Clock-free and transport-agnostic. |
| `src/aa_tls.h`, `src/aa_tls.c` | TLS seam: a memory-BIO-style provider contract, handshake sequencing, round and size bounds, and the establishment gate on encrypt/decrypt. No cryptography, no key material. |
| `src/aa_aoap.h`, `src/aa_aoap.c` | AOA accessory-mode responder: the vendor control requests a head-unit host sends (protocol query, six identification strings, START), the string table, the START gate and the descriptor values a platform layer needs. |
| `src/aa_sensor.h`, `src/aa_sensor.c` | Sensor channel: repeated channel open, start handshake, the five simple value events and the EV energy model toward the phone. Encodes only the model fields with real-car evidence. |
| `transport/aa_tcp.c`, `transport/aa_tcp.h` | The wireless pipe: listener, accept, and a byte pump between a socket and the session (queued frames leave before the first read). The engine stays socket-free. |
| `daemon/aa_daemon.c` | Debug daemon: `/status`, `/ap-check` (validates an AP readback with the bootstrap rules and pins it) and `/saved`. The WiFi app ships its own Android Auto tab (`/aa/check` in `wifi_daemon`, commit `8744dec`), so this daemon carries no page snippet any more: what it adds is the validation and the pinned AP readback. |
| `src/aa_wireless.h`, `src/aa_wireless.c` | The wireless path as one object: AP readback in, bootstrap packets out, and once the phone joins the AP a live head-unit-role session. No sockets, no Bluetooth, no clocks — a transport only moves bytes. |
| `src/aa_mirror.h`, `src/aa_mirror.c` | The same protocol as a *phone*: answers the version request, sends the discovery request, opens the wanted channels, starts the EV sensor and reports the energy model it receives. This is what a bridge needs toward the car, and what an in-car sniff test can use on its own. |
| `src/aa_session.h`, `src/aa_session.c` | Session glue: frames in, frames out. Decodes frames, decrypts encrypted payloads, drives `aa_control` and `aa_sensor`, encrypts everything that leaves after the handshake, and queues complete frames for a transport. |
| `tests/aa_wire_test.c` | Fixed external byte vectors, round-trips, byte-at-a-time streaming, malformed corpus, deterministic pseudo-random round-trips. |
| `tests/aa_proto_test.c` | Varint vectors and errors, field iteration and skipping, UTF-8 and string bounds, nested writer including its overflow mode. |
| `tests/aa_control_test.c` | The happy path to READY, fixed message vectors, channel-open accept/refuse, ping echo, shutdown, and the error policy. |
| `tests/aa_tls_test.c` | Provider contract and guards: establishment gate, handshake rounds, blob bound, provider errors, `take()` buffer handling. |
| `tests/aa_sensor_test.c` | Repeated channel open, start handshake, value-event vectors, range gates and the EV model parsed back field by field. |
| `tests/aa_aoap_test.c` | The exact host request sequence, the stored string table, START gating and the refusal paths. |
| `tests/test_tcp.py` | A scripted phone over a real loopback socket: version, handshake, auth, discovery, sensor open and start, ending in `state=ready`, `ev=yes`. |
| `tests/test_daemon.py` | Starts the daemon on an ephemeral port, checks `/status`, a good `/ap-check` (pins and saves), a lowercase BSSID and a public address refused, and that the saved endpoint survives a restart. |
| `tests/aa_wireless_test.c` | The full path with the real Wifi1b numbers: readback, five RFCOMM stages, then version → handshake → auth → discovery → sensor → EV push. |
| `tests/aa_ap_test.c` | A Wifi1b-shaped readback through the codec into the first packet, and every refusal the seam must make. |
| `tests/aa_mirror_test.c` | The phone-side conversation with head-unit messages generated by the head-unit code, a refused channel, a sensor-only unit and the EV read-back. |
| `tests/aa_btboot_test.c` | Framing vectors in both readings, BSSID canonical text, credential and address validation, the full handshake, refusal and out-of-order paths. |
| `tests/aa_session_test.c` | A scripted phone through version, handshake, auth, discovery, channel open, ping, shutdown and the sensor channel (EV start request, pushed state of charge), plus early-encryption, flood, oversized-handshake, partial-frame and foreign-channel paths. |
| `tests/proto/aa_differential.proto`, `tests/aa_dump.c` | Second opinion from the official protobuf tooling: `protoc` decodes what our encoders produced, and our decoder parses what `protoc` encoded, so the two independent statements of the same wire facts are cross-checked. |
| `probes/bt_owner.c` | Diagnostic for the kernel Bluetooth path: whether our process could bind the SDP (L2CAP PSM 1) and AA RFCOMM (channel 8) sockets. On the measured unit the answer is *unavailable* — no kernel Bluetooth stack exists at all (`docs/19` §5b), the vendor daemon is the stack — so this now records the errno and the adapter/module evidence; the real route is the vendor's exported SDP/RFCOMM API via `tools/bttap` |
| `probes/vbtd_api.c` | (shipped by the WiFi stage as `combo/aa/vbtd_api.target`, writes `/rw_data/aa/bt_api.json`) Reads an ELF's `.dynsym` and answers whether the vendor Bluetooth stack **exports** `SDP_CreateRecord`, `SDP_AddServiceClassIdList`, `SDP_AddAttribute` and `RFCOMM_CreateConnection` — the condition for `dlsym` to find them from inside `vbtd` (`docs/19` §5b). Writes `{"sdp_api":…,"rfcomm_api":…}` for the WiFi status JSON; a string search cannot distinguish an export from an import |
| `tests/test_aa.py` | Builds and runs every program (strict, then ASan/UBSan), runs the `protoc` cross-check and enforces the source policy: SPDX, no allocation/sockets/threads/I-O, no protobuf runtime or TLS library, no reference-library code. |

## Building for the head unit

The host build here is for tests only. Anything shipped to the unit must be
built with musl and a baseline ISA: a glibc-static binary died with SIGILL
(`code=dumped, signal=ILL`) on the unit's Atom-class CPU on 2026-11-22, which is
why Battery moved to `musl-gcc -march=x86-64 -mtune=generic` as well.

```sh
sh scripts/00_fetch_third_party.sh      # once, fetches musl
cd packages/aa && make target-aa_daemon # -> aa_daemon.target (static-pie)
```

## Run

```sh
sh -c 'cd packages/aa && make check'
```

## Not implemented yet

A real TLS provider (a transport checkpoint supplies one backed by a library),
the navigation/video/input channel state machines, and the wireless bootstrap.
The route decided in `docs/19-aa-core.md` §5e is the **on-unit** one, so the
next steps are the `bt_owner` probe, the video question and then the bootstrap on
the unit; the phone-role mirror and the bulk frame pipe only matter on the bridge
fallback. Video and audio channels are deliberately not advertised
by the discovery response yet, and the discovery response does not yet declare
the electric fuel type or EV connector list. See the checkpoint list in
`docs/19-aa-core.md`.
