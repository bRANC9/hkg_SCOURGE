# SPEC — AA core (Android Auto, wired + wireless)

## 1. Target

| Item | Value |
|---|---|
| Vehicle | Hyundai Ioniq 5 Gen5W (LG/Velix head unit), EU |
| Firmware | `NE_E.EUR.S5W_L.001.001.260128` |
| Stock projection | `AppProjection` (vendor), wired Android Auto only |
| Session owner today | Vendor stack; an external dongle in the stock USB port |
| Deployment | none yet. Any binary for the unit is musl-gcc, `-march=x86-64 -mtune=generic`, static-pie: a glibc-static build SIGILL'd on the Atom CPU (measured 2026-11-22) |

## 2. Goal

One Android Auto protocol core that both transports use, so a feature written
once works on a wired and on a wireless session:

1. Transport-agnostic frame layer with bounded, allocation-free encode/decode.
2. The same channel vocabulary on both transports (control, input, sensor,
   video, media/speech/system audio, AV input, Bluetooth).
3. Above that, once: EV energy data toward the phone, navigation turn-by-turn
   extracted from the session, video/audio/input plumbing.
4. Two transports behind one interface: USB AOAP (wired) and wireless
   (Bluetooth bootstrap, 5 GHz AP, TLS over TCP).

## 3. Non-goals

- No vendor binary patching in this package, no OEM material, no hash-gate
  bypass.
- No replacement of the stock USB path on the head unit.
- No firmware flash, updater, bootloader or partition change.
- No claims about MFi/Google certification, and no synthesized authentication
  or identity.
- Not a daemon, installer or QML page yet.

## 4. Wire facts this checkpoint implements

Verified read-only against aasdk commit
`046b3b381595509d0939fa84b14a90978f46ff63`; nothing is copied, vendored or
derived from that tree.

| Fact | Value |
|---|---|
| Frame header | `[channel:1][flags:1]` |
| Size field, FIRST frame | 6 bytes: `u16` frame payload length, then `u32` total message length (both big-endian) |
| Size field, other frames | 2 bytes: `u16` frame payload length (big-endian) |
| Flag bits | `0x01` FIRST, `0x02` LAST, `0x04` CONTROL, `0x08` ENCRYPTED |
| BULK (`0x03`) | this frame is the whole message, short size field |
| Unknown flag bits (`0x10`-`0x80`) | rejected |
| Reference fragmentation threshold | `0x4000` frame payload |
| Channel payload start | `u16` big-endian message ID, then the message body |
| Channel IDs | control 0, input 1, sensor 2, video 3, media audio 4, speech audio 5, system audio 6, AV input 7, Bluetooth 8 |

## 5. Local policy (not wire facts)

- `AA_WIRE_MAX_FRAME_PAYLOAD` = `0x4000`, `AA_WIRE_MAX_MESSAGE` = 4 MiB: a
  larger frame or message is rejected, never buffered.
- The encoder never emits a zero-length trailing LAST frame; the decoder
  accepts one, because the reference sender can produce it for a message that
  is exactly one chunk long.
- Every frame of one message must agree on channel, CONTROL and ENCRYPTED;
  a mismatch is `AA_WIRE_ERR_INTERLEAVED` and the decoder resets.
- Any error resets the decoder state: the caller must drop the connection
  stream, not re-feed the same bytes.
- Partial frames consume nothing; a complete frame inside an incomplete
  message is reported as consumed with `AA_WIRE_END` (this was a real defect
  caught by `tests/aa_wire_test.c:test_partial_consumption`).
- Protobuf: a varint longer than 10 bytes, group wire types, invalid UTF-8, an
  embedded NUL or a string that does not fit the caller's buffer are hard
  errors; the writer poisons itself on the first overflow.
- Control channel: a well-formed but unimplemented message is counted and never
  fails the session; a malformed one returns `AA_CONTROL_ERR_PARSE` and leaves
  the state alone; a rejected version is terminal.
- Auth complete is emitted only after the transport reports TLS up, because it
  must not be sent in the clear.
- Video and audio channels are not advertised until a decoder and an audio path
  exist, so a session against this checkpoint carries sensor, navigation and
  input descriptors only.
- TLS: the seam owns sequencing only. A provider must never report
  established unless its own verification succeeded; the engine cannot check
  that, so the transport checkpoint owns the real provider and its trust
  configuration.
- Session glue: an encrypted frame arriving before the secure session is a
  terminal TLS error, encrypted payloads are decrypted before the message ID is
  read, replies are encrypted per the reference table (discovery/channel-open/
  ping/shutdown), every non-control outbound payload goes through the secure
  session, and a full output queue fails the session instead of dropping a
  reply.
- EV energy: only fields with real-car evidence are emitted. `min_usable_capacity`
  carries the *current* energy because that is how Maps reads it, the
  consumption rate is derived from the car's own remaining range (zero when the
  range is unknown), and an invalid or out-of-range state is never sent — a
  fabricated state of charge is worse than none. Charging curves, thermal and
  wheel blocks are deliberately absent: their field names and types are not
  established.

## 6. Tests

`python3 tests/test_aa.py` — three programs, each built strictly and again
under ASan/UBSan:

| Gate | Contents |
|---|---|
| Strict build | `-std=c11 -Werror -pedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes` |
| `aa_wire` fixed vectors | version request, encrypted service discovery, FIRST/LAST video pair, empty trailing LAST |
| `aa_wire` behaviour | fragmentation at a 4096 chunk, byte-at-a-time streaming, 200 deterministic pseudo-random round-trips |
| `aa_wire` malformed corpus | unknown flags, oversize frame, zero/short FIRST total, MIDDLE without start, interleaved channel, ENCRYPTED-bit change, nested FIRST, short/overlong message, buffer-too-small, encoder argument refusals, recovery after an error |
| `aa_proto` | varint vectors and error forms, field iteration and skipping, UTF-8 and string bounds, nested writer including the length-prefix overflow |
| `aa_control` | start → version match → TLS handshake → auth complete → discovery → READY, channel-open accept/refuse, ping echo, shutdown, state/channel/parse errors, advertised-channel policy |
| `aa_tls` | provider contract (missing ops, provider errors, `take()` buffer handling), establishment gate on encrypt/decrypt, handshake round and blob bounds, plaintext bound |
| `aa_sensor` | repeated channel open, start handshake (including a non-EV type and malformed bodies), all five value events with fixed vectors, range gates, remaining-energy derivation, and the EV model parsed back (current/capacity/reserve/powers/regen/consumption/prefs) |
| `test_tcp.py` | a scripted phone over a real socket: version, handshake, auth, discovery, sensor open and start, ending in `state=ready` and `ev=yes` |
| `aa_wireless` | the whole wireless path: AP readback, the five RFCOMM stages, then an AA session driven to READY with the EV push queued; plus off-air, public-address, wrong-order and truncated-packet refusals |
| `aa_ap` | a Wifi1b-shaped readback end to end into the first packet, plus every refusal (off air, short key, non-canonical BSSID, public or malformed IPv4, zero port) |
| `aa_mirror` | the full phone-side conversation, a refused channel, a sensor-only head unit, the EV report (current/capacity) and ping echo |
| `aa_btboot` | framing in both readings, fixed start and info-response vectors, BSSID text/parse strictness, credential and address validation, the full five-stage handshake, refusal and out-of-order paths |
| `aa_session` | scripted-phone negotiation end to end with a deterministic provider; encrypted discovery/open/ping/shutdown replies; sensor channel open, EV start request and a pushed state of charge; early-encryption, handshake-flood, oversized-handshake, partial-frame and foreign-channel paths; queue accounting |
| Differential (`protoc`) | The same wire facts are stated twice: once in C, once in `tests/proto/aa_differential.proto`. protoc decodes what our encoder produced (discovery response, EV model, gear event) and our decoder parses what protoc encoded. A drifting field number, wire type or nesting level fails here even when our encoder and decoder agree with each other. Skipped with a notice when `protoc` is absent or `AA_NO_PROTOC=1` |
| Source policy | SPDX headers, no allocation/sockets/threads/I-O in the engine, no protobuf runtime or TLS library, no reference-library code |

## 7. Checkpoints

| ID | Work | Status |
|---|---|---|
| AA1 | Frame layer (`aa_wire`) + channel vocabulary + tests | Done offline: strict and ASan/UBSan builds pass |
| AA2 | Protobuf wire codec (`aa_proto`) + control-channel state machine (`aa_control`): version, auth complete, service discovery, channel open, ping, shutdown | Done offline: strict and ASan/UBSan builds pass; focus requests are counted as ignored until the audio/video channels exist |
| AA3 | TLS seam (`aa_tls`) + session glue (`aa_session`): handshake sequencing, encryption table, frame queue | Done offline: a scripted phone drives version → handshake → auth → discovery → channel open → ping → shutdown, normally and under ASan/UBSan |
| AA4 | Sensor channel (`aa_sensor`): start handshake, simple value events, EV energy model toward the phone, wired into the session | Done offline: fixed vectors plus the model parsed back field by field; the scripted phone requests type 23 and receives the live state of charge |
| AA5 | Navigation channel: turn-by-turn extraction, cluster/HUD delivery path | Pending |
| AA6 | AOA accessory-mode responder (`aa_aoap`): protocol query, six identification strings, START gate, descriptor values | Done offline for the responder half; the bulk frame pipe and the phone-role protocol mirror are AA8 |
| AA7b | Wireless path orchestration (`aa_wireless`): AP readback → bootstrap packets → phone joins the AP → a live head-unit-role session, with no socket or Bluetooth code inside | Done offline end to end with the real Wifi1b numbers (10.42.0.1/24) through READY and an EV push |
| AA7a | AP-to-bootstrap seam (`aa_ap`): turns the access-point readback (SSID, PSK, BSSID, IPv4, port) into the two RFCOMM messages, with the validation delegated to `aa_btboot` | Done offline against a Wifi1b-shaped readback (10.42.0.1); the AP layer still has to expose the BSSID and its address |
| AA7 | Wireless bootstrap codec (aa_btboot): RFCOMM framing, the start/info/status messages, credential validation, five-stage state machine | Done offline for the codec half; the BlueZ SDP/HFP/RFCOMM plumbing and the TCP listener are the transport half |
| AA7c | TCP transport (`transport/aa_tcp.c`): bind/listen, accept one peer, pump bytes between the socket and the session, drain the queued frames before the first read | Done, host-tested over a real loopback socket (`tests/test_tcp.py`) with the stand-in provider; the bind address and the firewall rule still depend on the unit probe |
| AA-D | Debug daemon (`daemon/aa_daemon.c`): `GET /status`, `GET /ap-check`, `GET /saved` on 127.0.0.1:8095; a successful `/ap-check` pins the validated AP readback to a state file | Done, host-tested (`tests/test_daemon.py`); not installed, no service unit, no session behind it |
| AA8 | Phone-role mirror (`aa_mirror`): version answer, discovery request, channel opens, sensor start, EV read-back | Done offline for the protocol half, driven by head-unit-side messages generated with `aa_control`/`aa_sensor`; needed only on the bridge fallback (`docs/19` §5e) |
| AA9 | On-unit Bluetooth ownership (`probes/bt_owner.c`): can our process bind L2CAP PSM 1 (SDP) and RFCOMM channel 8, or does `vbtd` hold them | Tool done, self-tested on the host; **not yet run on the unit** — it is the measurement that decides the on-unit route |
| AA10 | Video track for the on-unit route: H.264 decode, ivi surface on Weston 1.10, input events | Pending — the largest open item, and it inherits "never crash Weston" (its death resets the head unit) |

## 8. Risks and open questions

- Wireless AA on the head unit: there is no BlueZ (measured 2026-11-23), but the
  vendor `vbtd` stack is reversed (Ghidra): pairing/discoverability is the
  CommonAPI `v2::org::bluez::Adapter` on the session bus, HFP AG already runs
  for telephony, and the two sockets AA actually needs — L2CAP PSM 1 for SDP and
  RFCOMM channel 8 — are *kernel* sockets, so they may be ours without touching
  the vendor ABI at all. `probes/bt_owner.c` measures that; until it has been
  run on the unit, "no vendor ABI work needed" is a hypothesis, not a fact.
- The on-unit route makes the unit the receiver, so video is ours, and it is a
  **two-process** design by measurement, not by preference: `AppProjection`
  links no decoder at all (the vendor media service decodes), the platform's
  decoder stack (`libva` + `i965_drv_video`) is glibc-only, and this package
  builds musl static-pie. So a dynamically linked glibc-2.25 helper decodes and
  passes dmabuf fds over a UNIX socket — the pattern
  `packages/wifi1b/scripts/build_carplay_native.py` already proves. The open
  link is the platform's zero-copy capability, not the vendor binaries: they
  hand `libCarPlayMediaFramework` a *compressed* H.264 stream
  (`ScreenStreamProcessData`) and never touch a dmabuf. So the checks are
  `EGL_EXT_image_dma_buf_import` in `libEGL`, `wl_drm`/`zwp_linux_dmabuf_v1`
  in Weston, and `vaExportSurfaceHandle` in `libva`; missing any of them drops
  the target to 720p30 over `wl_shm` (`docs/19` §5e-2, §8).
- The EV energy model is reverse-engineered protocol (undocumented sensor
  type), so it is version-sensitive: validate against the phone's AA version
  before claiming support.
- Cluster/HUD delivery is not part of AA; it depends on the head unit's own
  navigation CAN frames (`com.mnsoft.navi.canservice`, micom cluster frames).
  Battery already proves frame injection works there, but treasure-hunt logic
  on a moving car needs an on-car gate.
- Buffer sizes for real video are unknown until a capture exists; the 4 MiB
  message bound is a policy, not a measurement.
