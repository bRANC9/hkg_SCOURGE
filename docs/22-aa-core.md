# 18 — The AA core: one Android Auto implementation, two transports

TARGET: Ioniq 5 Gen5W_L (Linux), firmware `260128` (EU), stock projection
`AppProjection`. This document records the decision behind `packages/aa` and
the order of work. **Nothing here is implemented on the car.** The frame layer
is offline-tested only; no phone, no vehicle, no activation.

## 1. The problem this answers

Two owner-visible gaps remain when a ready-made wireless Android Auto dongle
sits in the stock USB port:

- **Navigation does not reach the cluster or the HUD.** Turn-by-turn is an AA
  *NAVIGATION* data exchange from the phone; what the Gen5W cluster/HUD shows
  today is produced by the factory navigation stack (`com.mnsoft.navi.canservice`
  → micom cluster frames), not by AA.
- **No battery state of charge in Google Maps.** Maps only plans EV routes when
  the head unit advertises an electric vehicle and keeps sending a vehicle
  energy model plus connector state.

A transparent dongle cannot fix either: both are messages the phone accepts
only *from the head unit*. The head unit's own projection stack speaks wired
AA only and offers no extension point. Two properties therefore have to be met
by our own code: it must sit inside the AA session, and it must serve the wired
and the wireless case without two protocol implementations.

## 2. Decision

Implement one Android Auto core with a transport seam:

| Layer | Scope |
|---|---|
| Shared: frame codec | `aa_wire` — header/size field, fragmentation, bounded reassembly, strict validation |
| Shared: channel vocabulary | `aa_channels` — channel IDs, control message IDs, feature names |
| Shared: session | control channel (version, service discovery, channel open, ping, focus), channel multiplexing |
| Shared: features | EV energy toward the phone, navigation extraction, video/audio/input |
| USB-only | AOAP accessory handshake, bulk endpoints, gadget descriptors |
| Wireless-only | Bluetooth RFCOMM bootstrap, 5 GHz AP + DHCP, TCP listener, TLS server role |

Nothing above the transport may know which one is in use. That is what makes
the feature work affordable: the EV and navigation code is written and tested
once.

Evidence for the split: aasdk commit
`046b3b381595509d0939fa84b14a90978f46ff63` implements the same core over a USB
transport and a TCP transport with one messenger, and `aa-proxy-rs` runs the
same protobuf core over both. Both were read as protocol references only —
**no code is vendored, copied or derived**, and neither tree is a dependency.

## 3. Routes considered

| Route | What it means | Verdict |
|---|---|---|
| A — upstream dongle | Keep a commercial wireless-AA dongle in the stock USB port | No work, but cannot deliver cluster navigation or EV data |
| B — bridge dongle | Own hardware between phone and head unit: wireless on the phone side, USB AOAP gadget on the head-unit side, payload passed through | The tractable implementation route: no decode, no projection window, stock USB untouched |
| C — the AA stack on the unit itself | The head unit speaks AA to the phone and renders it | **The decided route (§5e)**: no OEM AA receiver to reuse (unlike CarPlay's libAirPlay/iAP2), so video decode and window ownership are ours; the Bluetooth half turned out to be cheap |

Route B is why the transport seam matters: the bridge is the only process that
uses *both* transports, so a single core is not a stylistic choice there, it is
the assembly. Route C would reuse the same core if it is ever attempted.

### Why BlueZ is not shipped to the head unit

Shipping BlueZ is technically possible and practically the wrong trade:

- **The controller already has an owner.** `vbtd -d /dev/ttyS0 -p
  /tmp/bt_patchram.hcd` opens the HCI over `ttyS0` and loads the patchram
  firmware. A second HCI owner is not a coexistence problem but a hijack: the
  vendor daemon loses the device, and with it hands-free telephony (HFP),
  media (A2DP/AVRCP), phonebook (PBAP) and the Bluetooth leg of the wireless
  CarPlay handshake. Those are safety- and legality-relevant features.
- **The runtime environment fights back.** The rootfs is vendor glibc 2.25 with
  SMACK (`onlycap`), the kernel is the 4.1.27 vendor fork, and `bluetoothd`
  expects D-Bus system policy, udev rules and its own `hciattach` sequencing
  instead of the vendor patchram path. Everything we install there has to be
  maintained against an image we do not control, and a vendor update can undo
  it silently.
- **We would own the whole Bluetooth surface forever**, not just the 40 lines of
  SDP registration the AA bootstrap needs.

So the order of preference stays: (1) ask the vendor stack for an RFCOMM channel
if it exposes one (socket or D-Bus) - `aa_probe.txt` collects exactly that;
(2) route B, where BlueZ runs on hardware we control and the head unit sees only
USB AOAP; (3) shipping BlueZ, only if (1) and (2) both fail and the owner
accepts losing the vendor BT profiles.

## 4. Where the two missing features live

| Feature | Layer | Source of truth |
|---|---|---|
| EV energy model + connector state | Shared feature, SENSOR channel | `I-CAN 0x2FC` SoC, `0x326` connector/charging — already decoded by `packages/battery` (`packages/battery/SPEC.md:53`) |
| Turn-by-turn to cluster/HUD | Shared feature (extract), head-unit side (deliver) | AA navigation data in, then the unit's own cluster path: `com.mnsoft.navi.canservice` (`packages/battery/docs/MICOM_WHITELIST.md:156`) and the cluster frames Battery already sends (`0x4E8`/`0x4CC`) |

Both are why a pass-through dongle cannot work: on route B this lives in our
bridge, in route C on the unit, but never in a transparent accessory.

## 5. Checkpoints

| ID | Work | Status |
|---|---|---|
| AA1 | Frame layer + channel vocabulary + strict/ASan tests | **Done offline** — `packages/aa`, `make check` passes |
| AA2 | Protobuf wire codec + control-channel state machine (version, auth, discovery, channel open, ping, shutdown) | **Done offline** — three test programs pass normally and under ASan/UBSan |
| AA3 | TLS seam (provider contract, handshake sequencing, bounds) + session glue (frames in/out, encryption table, queue) | **Done offline** — a scripted phone drives the whole negotiation; five test programs pass normally and under ASan/UBSan |
| AA4 | SENSOR channel: start handshake, value events, EV energy model toward the phone | **Done offline** — fed from the same values Battery decodes; six test programs pass normally and under ASan/UBSan |
| AA5 | NAVIGATION: turn-by-turn extraction and cluster delivery plan | Pending |
| AA6 | AOA accessory-mode responder (`aa_aoap`): request table, six identification strings, START gate, descriptor values | **Done offline** for the responder half; the bulk frame pipe and the phone-role protocol mirror are AA8 |
| AA7c | TCP transport (`transport/aa_tcp.c`): listener, accept, byte pump, and the queued-frames-first rule | **Done**, tested over a real loopback socket; bind address and the firewall accept rule wait on the unit probe |
| AA7b | Wireless path orchestration (`aa_wireless`): readback → bootstrap → phone on the AP → head-unit-role session | **Done offline** end to end on the Wifi1b numbers; sockets, BlueZ and the real TLS provider stay outside |
| AA7a | AP-to-bootstrap seam (`aa_ap`): access-point readback → the two RFCOMM messages, validation delegated to `aa_btboot` | **Done offline** against a Wifi1b-shaped readback; the AP layer must still expose the BSSID and its IPv4 address |
| AA7 | Wireless bootstrap codec (`aa_btboot`): RFCOMM framing, start/info/status messages, credential gates, five-stage state machine | **Done offline** for the codec half; the BlueZ SDP/HFP/RFCOMM plumbing and the TCP listener are the transport half, and they belong on the bridge |
| AA8 | Phone-role mirror (`aa_mirror`): version answer, discovery request, channel opens, sensor start, EV read-back | **Done offline** for the protocol half, tested against head-unit messages generated by the head-unit code; needed only on the bridge fallback |
| AA9 | On-unit Bluetooth ownership (`probes/bt_owner.c`) | **Superseded, kept as a diagnostic**: the kernel has no Bluetooth stack and no modules on disk (§5b), so it can only ever print `EAFNOSUPPORT`/`ENODEV` |
| AA9b | The vendor seam: does `vbtd` expose SDP record creation and an RFCOMM server | **Answered: yes** — `SDP_CreateRecord`, `SDP_AddAttribute`, `SDP_AddServiceClassIdList`, `RFCOMM_CreateConnection` are exported (`nm -D vbtd-51`) |
| AA9c | The active shim: AA SDP record (UUID only, RFCOMM 8) + RFCOMM server in `tools/bttap`'s plugin, RFCOMM bytes pumped to `aa_btboot` over a UNIX socket, glibc-2.25 build, opt-in and revertible | **Next implementation step of the Bluetooth half** |
| AA10 | Video track for the on-unit route: H.264 decode, ivi surface on Weston, input events | **Measured, not built** (§5e-2): no in-process decoder, VA-API/i965 exists but only as glibc `.so` → a glibc-2.25 helper process + dmabuf fd passing; the open link is the buffer-transport API (`libImageProvider`/EGL-image/vendor IPC) |

Each checkpoint records: exact commit, commands and results, coverage gaps and
the next action. Passing unit tests is not transport validation, and a mocked
exchange is never a phone or vehicle result.

## 5b. AA7 wireless bootstrap: the facts, pinned

Documentation-derived, **not verified on this unit**. Source: the
`open-android-auto` wireless Bluetooth setup guide (BlueZ 5.82, Raspberry Pi 4,
Android 14 phone, 2024-02), cross-checked against `aa-proxy-rs`/`bluer`
behaviour. Facts only — no third-party code is copied.

**RFCOMM exchange.** Packet format is `[length: u16 BE][message id: u16 BE]
[protobuf body]`. Five stages, head unit first:

| Stage | Direction | ID | Message | Note |
|---|---|---|---|---|
| 1 | HU → phone | 1 | `WifiStartRequest { ip_address = 1, port = 2 }` | announces the TCP endpoint the phone must dial |
| 2 | phone → HU | 7 | `WifiStartResponse { status }` | 0 = accepted |
| 3 | phone → HU | 2 | `WifiInfoRequest` | empty, or with cached credentials |
| 4 | HU → phone | 3 | `WifiInfoResponse` | the AP credentials, see below |
| 5 | phone → HU | 6 | `WifiConnectStatus { status }` | 0 = the phone joined the AP |

After stage 5 the phone opens the TCP connection to stage 1's address and the
AA session (TLS over TCP) begins — that is the point where `aa_session_feed`
takes over.

**`WifiInfoResponse` fields** (the classic mistake is a varint BSSID):

```
ssid              = 1  LEN
key               = 2  LEN
bssid             = 3  LEN   uppercase colon-separated MAC, e.g. "DC:A6:32:E7:5A:FE"
security_mode     = 4  VARINT   WPA2_PERSONAL = 8 (WPA_PERSONAL = 4, WPA_WPA2 = 12, OPEN = 1)
access_point_type = 5  VARINT   DYNAMIC = 1 (STATIC = 0)
```

**Bluetooth service setup** — this is where the effort actually is:

- the SDP record must contain **only** the 128-bit AA UUID
  `4de17a00-52cb-11e6-bdf4-0800200c9a66` in its service-class list, L2CAP +
  RFCOMM **channel 8** in the protocol list, no SPP UUID and no profile
  descriptor list: Android compares UUID *sizes* strictly and rejects a mixed
  record as `invalid length for discovery attribute`;
- **HFP AG** (`0000111f-0000-1000-8000-00805f9b34fb`) must be registered and
  connected, otherwise the phone refuses to start wireless setup at all
  (`WIRELESS_SETUP_FAILED_TO_START_NO_HFP_FROM_HU_PRESENCE`); it does not need
  to carry calls. **HSP HS** (`…1108`) keeps the link alive;
- BlueZ wants `--compat` (legacy SDP socket) and `-P *` (no auto-registered
  profiles); WirePlumber and a second BT controller add records that break the
  strict-UUID rule;
- the socket BlueZ hands over is non-blocking: set it blocking with a timeout
  before the first read, or the info response is never seen
  (`WIRELESS_RFCOMM_INFO_RESPONSE_MISSING`).

**Measured on the unit (2026-11-23, from `aa_probe.txt`, collected by the WiFi
tab's probe).** There is **no BlueZ**: `bluetooth.service` is `not-found`,
`bluetoothd` is absent, `/sys/class/bluetooth` does not exist, and neither
`hciconfig` nor `sdptool` is installed. The stack is the vendor one:
`/app/bin/vbtd -d /dev/ttyS0 -p /tmp/bt_patchram.hcd` (BCM4349B1 firmware),
with `BluetoothHMI`, `btsco`, `vbtd-51` and `vbtd-63` beside it.

The vendor stack is nevertheless not empty of the pieces AA needs: its
strings include `sdp_conn_originate`, `sdp_db_find_record`, `SDP_ReadRecord`,
`sdpu_compare_bt_uuids`, `rfcomm_l2cap_if_init` and `RFCOMM_DataReq`, and both
`VBTConfig.json` copies carry an `ENABLE_WIRELESS_CARPLAY` key plus an `HFP`
section. An on-unit wireless path is therefore a reverse-engineering question
about that stack's profile registry — the same shape as the CarPlay C3
research — **not a proven impossibility**. It is not what this checkpoint
pursues: the bridge board stays the practical route because it needs no
vendor ABI work.

The Ghidra pass over `vbtd-51`/`vbtd-63` then found the vendor's *pairing*
surface: `v2::org::bluez::Adapter` (CommonAPI, on the **session** bus, not the
system bus where BlueZ would be) with `setPairableAttribute`,
`trySetPairableAttribute(bool)`, `setDiscoverableAttribute`,
`setDiscoverableTimeoutAttribute`, plus `RemoteEventHandler::onRemoteSetPairableAttribute`,
`fireDiscoverableAttributeChanged`, `VBTGapManager::SetDiscoverableConfig(unsigned)`
and `DeviceManager::AddPairedDeviceInterface`. So the unit *can* be made
pairable and discoverable without BlueZ — that half of the on-unit path is an
API call, not research. The half that is still missing is the one AA actually
needs: an **SDP record registration**. The SDP symbols found are client-side
(`sdp_conn_originate`, `sdp_db_find_record`, `SDP_ReadRecord`,
`sdpu_compare_bt_uuids`) — they read records, they do not add one in the shape
§5b requires (AA UUID alone, no SPP UUID, RFCOMM channel 8). Until an
add-record path turns up, the AA UUID service cannot be published from the unit.

The same pass pinned the AA switch the phone settings drive:
`deviceSettings.phoneProjectionSettings.androidAutoSettings.enableAndroidAuto`
via `BluetoothHMI`, and `connected PP Type: AndroidAuto`.

**The kernel-socket assumption does not survive the stick probe (2026-11-23,
ten runs).** An earlier hope in this document was that SDP (L2CAP PSM 1) and
AA's RFCOMM channel 8 could be *ours* because both are kernel sockets. The
device's own kernel module list, captured from `dmesg` in the same probe, has
**no Bluetooth subsystem at all**:

```
Modules linked in: … bcmdhd(O) mfd_iap2 g_iap2 usb_f_iap2 u_iap2 usb_f_ncm
cfg80211 … libcomposite dwc3 … lgivi_usbrmnet(PO) lgivi_usbser(O) usbserial …
```

No `bluetooth`, no `l2cap`, no `rfcomm`, no `hci_uart`, no `btusb` — and
`/sys/class/bluetooth` is absent in **every** run while
`/app/bin/vbtd -d /dev/ttyS0 -p /tmp/bt_patchram.hcd` holds the UART (its fd
table shows `/dev/ttyS0` open, plus its D-Bus sockets). In other words the
vendor stack is not a client of the kernel's Bluetooth stack: **it *is* the
stack**, implemented in userspace on the Broadcom controller, with the patchram
loaded by the daemon. So `probes/bt_owner.c` cannot succeed here for the reason
it was built — with no HCI device behind `AF_BLUETOOTH`, a bind can only return
`EAFNOSUPPORT`/`ENODEV`, and the three real options are:

1. **The vendor stack exports the server-side API — and this is the route.**
   `nm -D /app/bin/vbtd-51` shows `T` (callable) exports for
   `SDP_CreateRecord`, `SDP_AddAttribute`, `SDP_AddServiceClassIdList` and
   `RFCOMM_CreateConnection`, with the rest of a BTE/BTA stack beside them
   (`BTA_AvkEnable`/`Open`/`ProtectReq`…). So an AA SDP record and an RFCOMM
   server can be published **inside the vendor stack's own database**, with no
   kernel module and no guessed ABI. The vehicle already drives this from our
   side: `tools/bttap/shim.c` is an `LD_PRELOAD` shim that the WiFi stage
   installs and restarts `vbtd` under
   (`bt tap shim installed: /rw_data/bt/libbttap.so`, `vbtd restarted under the
   shim`). Today it only taps the UART; making it *active* — `dlsym` the four
   exports, add the record, accept RFCOMM, pump the payload to our daemon — is
   the smallest real step this route has had.
2. **Replace the stack for the UART — not available.** The next measurement
   killed it: `/lib/modules/4.1.27-abl/kernel/net/bluetooth` does not exist on
   the image and there is no `bluetooth.ko`, `rfcomm.ko`, `hci_uart.ko` or
   `bnep.ko` in the module tree. There is nothing to load; only the vendor stack
   can talk to the controller.
3. **The bridge board** (§5e-3), where BlueZ is ours and the unit only sees USB.

What the shim must be careful about, because it runs **inside a vendor daemon**:
a fault there is not our process dying, it is the car losing Bluetooth. So the
plugin is small and defensive (no allocation in callbacks, bounded buffers, no
`exit`), it is opt-in and revertible (start `vbtd` without the preload), and it
carries **no protocol code**: the five-stage AA bootstrap (`aa_btboot`) stays in
the tested musl daemon, the plugin only forwards RFCOMM bytes over a UNIX socket.
One build detail follows from that: the present tap compiles with `musl-gcc
-shared` because it is passive, but a plugin that *calls* vendor exports and
runs callbacks in the vendor process must be built against the vendor glibc 2.25
(the `build_carplay_native.py` path), not with musl.

**Which binary carries them, and why that matters.** The exports were proven on
`/app/bin/vbtd-51` (`nm -D`), but the process that actually runs is
`/app/bin/vbtd -d /dev/ttyS0 -p /tmp/bt_patchram.hcd`. Both facts are needed,
because the shim calls `dlsym(RTLD_DEFAULT, …)` inside that *running* process:
a symbol resolves only if the object that defines it is loaded and its
`.dynsym` is exported (`--export-dynamic`), so the binary to check first is the
one the init script launches, then `vbtd-51`/`vbtd-63` as fallbacks. Whether
those three are the same file is unresolved — the probe should record
`ls -l` plus a sha256 for each, one line apiece, and then the answer stops being
a guess. `probes/vbtd_api.c` does the actual check (defined vs imported symbol,
no `nm` needed on the unit) and writes the two flags the status JSON wants.

One useful side-fact from the same module list: `libcomposite`, `dwc3`,
`u_ether`, `usb_f_ncm` and `mfd_iap2`/`g_iap2`/`usb_f_iap2` are loaded, so the
unit's USB port demonstrably works as a **device** (the CarPlay direction). The
USB **host** role that wired AA needs is not proven by anything in the probe.

Everything else the probe measured, in one place:

| Fact | Value |
|---|---|
| AP interface | `wlan0`, ifindex 13, currently `type managed` |
| Radio | Cypress CYW88359 / BCM4359 Rev 5, `bcmdhd 1.363.68`, firmware `wl0 9.40.112.10` |
| AP mode supported | yes — `#{ AP } <= 2, #{ managed } <= 2, total <= 4, #channels <= 2` |
| Radio MAC (the BSSID to announce) | `28:0F:EB:02:21:8A` — explicitly *not* the Bluetooth MAC |
| 5 GHz | Band 2 present, measured at **5745 MHz (ch 149)**; the AP config asks for **ch 36 (5180)** — same band, different channel, so a real join is the only proof |
| Country | nvram `ccode=GB`, runtime `wldev_set_country DE` / `wl country DE`; cfg80211 reports `00` (world) → 5 GHz **passive scan**; `iw reg set` has no effect; MICOM Country EU |
| AP config | SSID `SCOURGE-AA`, band 5, channel 36, hidden 0, max_sta 2, PSK set — redacted here and never committed |
| Projection binary | `/app/bin/AppProjection`, sha256 `8f0096bc…60e3`, matching the pin in `docs/wireless-carplay/PROGRESS.md` |
| Platform | Linux 4.1.27-abl, x86_64, **glibc 2.25** |
| `/rw_data` | 1.2 GB with 274 MB free (76 % used) — an installer budget |
| Session bus | no `android_auto` name while nothing is projecting |

The glibc 2.25 line is why the unit build is musl + baseline ISA (§5d).

The AP side is measured too: the CarPlay AP is configured from
`carplay_ssid`/`carplay_band=5`/`carplay_channel=36`/`carplay_psk`, and in AP
mode there is no `bssid=` to read (the supplicant control socket does not exist
while the radio is in client mode, and a host AP has no upstream BSSID) — the
BSSID to announce is the radio's own MAC, measured as `28:0f:eb:02:21:8a`.
`aa_ap_bssid_canonicalize()` accepts exactly that form, and `/ap-check` pins the
canonical uppercase text.

The firewall has CarPlay precedent worth copying for our port: `CARPLAY_IN` /
`CARPLAY_OUT` chains that accept DHCP (`--dport 67`) across the AP interface, and
a separate `WL_STATIC_IN -i ncm0` chain for the USB NCM link. When the AA TCP
listener exists it needs its own accept rule on the AP interface, not a hole in
the global chain.

**What this means for us.** On the head unit the Bluetooth stack is the vendor
`vbtd`, so AA7 belongs to the bridge (the Pi), not to the unit: there we own
BlueZ, the 5 GHz AP (`wifi1b`'s static `wpa_supplicant` + our DHCP server already
produce the SSID/PSK/BSSID we must announce here) and the TCP listener. Our own
wire format for the five RFCOMM stages is a small bounded codec on top of
`aa_proto` — the protobuf bodies are two tiny messages, so this is a day of work
once the transport exists, not a research project. The expensive part is
BlueZ/SDP plumbing, and it lives outside the head unit.

## 5b-2. What the owner's dongle already proves, and what is actually missing

Owner fact (2026-11-24): Android Auto **already works in this car** through a
wired/wireless adapter — a dongle presents itself as the phone and the head unit
projects it. What does **not** work is the two things this project exists for:
the **cluster/HUD turn-by-turn** and the **EV energy model** in Maps. That
reorders the whole plan, and mostly cancels its most expensive part:

- The head unit as an AA **receiver** is proven working. Nothing in this package
  needs to decode H.264, own an ivi surface, or fight the Weston watchdog: the
  vendor stack already renders AA (§5e-2's video problem is a *receiver* problem,
  and we are not building a receiver any more).
- What the dongle cannot know is the car's state, because it only pipes frames:
  it opens no SENSOR channel of its own and it has no cluster path. So the missing
  role is a **protocol-aware middle**, not a renderer.
- The middle is cheap by design if it stays at the message level: forward every
  channel verbatim, **drop the head unit's SENSOR request and answer it
  ourselves** from `battery`'s I-CAN decode (`aa_sensor` already builds that
  message), and **extract the NAVIGATION channel** for the cluster (`aa_nav`,
  AA5). Video and audio are never touched, because they are never understood.
- The roles are the ones the checkpoints already implement: toward the phone the
  bridge is the head unit (host/TLS client, `aa_session`), toward the car it is
  the phone (accessory, `aa_aoap`'s AOA responder + a TLS **server** half, which
  is the one genuinely new piece).

So the shortest useful result is no longer "wireless AA in this car" — it is
**EV + cluster through a pass-through bridge**, and the two deliverables that do
not need any of the hardware first are AA5 (navigation out) and the sensor
injection path, both testable against our own scripted phone.

## 5c. The AA entry on the WiFi page, and why it is not an AP toggle

The WiFi app already has a CarPlay tab: the access point's SSID, key, band and
channel, then a transport block (iAP2, mDNS, ready) and a start button. The
Android Auto analogue looks the same but is *not* a mode of its own:

- the access point is the same 5 GHz AP the CarPlay mode brings up, so a second
  "Start AA AP" toggle would create a second radio owner for one radio;
- what AA adds is a Bluetooth handshake *before* the phone joins — an SDP record
  with the AA UUID, the HFP AG profile and an RFCOMM listener. On this unit the
  Bluetooth stack is the vendor's \`vbtd\`, so those profiles cannot be
  registered there. A toggle that cannot complete the handshake would look like
  a feature and do nothing;
- therefore the AA entry is a **status entry**, fed by \`aa_daemon\` on
  127.0.0.1:8095, showing what is really true: the AP readback that has been
  pinned (\\`/ap-check\\`), the four transport links (Bluetooth bootstrap, TCP
  listener, TLS, session) and a one-line reason. Today every link reports
  stopped and the reason says a bridge board is required;
- the session button exists, is visible and is disabled on purpose. When a
  bridge board supplies the Bluetooth side, the same block lights up link by
  link without a page redesign.

The WiFi app now ships that entry itself: `WifiPage.qml` gained a fourth tab
("Android Auto") with the AP start button and the measured verdict, and
`wifi_daemon` answers `GET /aa/check` with `{ap:{...}, bt:{bluez, vbtd},
verdict}` (commit `8744dec`). The earlier snippet in this package was removed so
there is one UI, not two; `aa_daemon` keeps only what that tab cannot do
(validating a readback against the bootstrap rules and pinning it).

An older copy of that block lived at `packages/aa/daemon/wifipage-debug.snippet.qml`
next to the daemon it reads, not in the WiFi package: the WiFi app only hosts it,
and the AA core stays a separate, mergeable unit.

**The verdict vocabulary is now out of date, and this document owns it.** The
page colours the row on `aa-wireless-possible`, and the daemon derives the
verdict from `bluetoothd` presence — which §5b has since shown is the wrong
signal: BlueZ is not needed, the vendor stack exports `SDP_CreateRecord` and
`RFCOMM_CreateConnection`, and "bridge required" is no longer the honest reason.
The contract the page and the daemon should share:

```json
{"ok":true,
 "ap":{"state":0,"ready":true,"ssid":"…","ip":"…","band":5,"channel":36},
 "bt":{"bluez":false,"vbtd":true,"sdp_api":true,"rfcomm_api":true},
 "configured_ssid":"…",
 "verdict":"aa-wireless-possible"}
```

| Field | Meaning | Source |
|---|---|---|
| `ap.ready` | AP up **and** DHCP running (today: `ap.state == AP_READY && tether == 1`) | `ap_probe` |
| `bt.bluez` | kept for compatibility; **not** a blocker any more | `command -v bluetoothd` |
| `bt.vbtd` | the vendor stack is running | `pidof vbtd` |
| `bt.sdp_api`, `bt.rfcomm_api` | the vendor stack **exports** the calls the bootstrap needs (`dlsym` can only find defined symbols) | `packages/aa/probes/vbtd_api.c` → `/rw_data/aa/bt_api.json`, measured once at install and cached like `hwinfo.txt`; a plain string search is not enough — an *imported* name also lives in `.dynstr` |

Two rules that keep that file honest: the probe target is
`readlink -f /proc/$(pidof vbtd)/exe` — the object actually loaded, not a
hardcoded path, because `dlsym(RTLD_DEFAULT, …)` searches that process's scope —
and **the WiFi stage is its only writer**. This package has no installer yet, so
two writers would only guarantee a stale answer; the AA side supplies the binary,
the WiFi stage runs it and publishes the JSON.
| `verdict` | `aa-wireless-possible` (AP ready **and** a BT path) · `aa-wireless-needs-ap` · `aa-wireless-bt-vendor-api` (AP ready, no BlueZ but `vbtd`+SDP/RFCOMM exports → the shim route) · `aa-wireless-bt-missing` | derived |

Two UI rules that follow: "BlueZ API absent" must not be painted as a warning
(it is expected and irrelevant), and the AA entry must not infer AA-ness from the
SSID text — `apSsid === "SCOURGE-AA"` breaks the moment the owner renames the
access point, which he already has; use the config flag or the AP readback.

## 5d. Build baseline for the unit

The deployment lesson came from Battery, not from this package: a glibc-static
binary died with `SIGILL` (`code=dumped, signal=ILL`) on the unit's Atom-class
CPU, because the static glibc start-up used ISA extensions the CPU lacks. The
unit-side build is therefore musl (`musl-gcc`), `-march=x86-64 -mtune=generic`,
`-static-pie` — `make target-aa_daemon` in `packages/aa` does exactly that, and
the host build stays for tests only. Nothing here may assume a newer baseline
than that, which is also why the engine avoids float-heavy or vectorised paths.

## 5e. Where the AA stack runs: on the unit (decided)

Owner decision (2026-11-23): the target is the **on-unit route** — the head unit
speaks AA to the phone itself, with nothing extra plugged into the car. The
bridge board (§5e-3 below) stays as a fallback that reuses the same core, not as
the plan. The reason it moved from §3's "research corner" to the plan is that
the expensive half turned out to be already done:

| What the on-unit route was missing | State now |
|---|---|
| a way to be pairable/discoverable without BlueZ | **solved** — vendor CommonAPI `v2::org::bluez::Adapter` on the session bus, reversed with Ghidra (§5b) |
| the HFP AG presence the phone insists on before wireless setup starts | **already shipped** — `vbtd` runs the telephony profiles for the car |
| an SDP record and RFCOMM listener of our own | **solved in principle** — the unit loads no Bluetooth module at all and `vbtd` is the stack on `/dev/ttyS0`, but that stack *exports* `SDP_CreateRecord`, `SDP_AddAttribute`, `SDP_AddServiceClassIdList` and `RFCOMM_CreateConnection`, and our existing `LD_PRELOAD` shim already runs inside it (§5b) |
| a real TLS provider | **open** — no crypto in this tree yet; either the unit's own libssl or a TLS built into the musl-static binary |
| rendering the phone's video | **open, and the biggest item, but now measured** — no decoder is linked into the projection app at all; the platform has VA-API + the Intel i965 driver but only as glibc libraries. See §5e-2 for the two-process shape this forces |

What on-unit costs that the bridge did not: a bridge forwards the session and
lets the stock projection app decode; on the unit **we** are the projection app,
so a video track (decode, ivi surface, input) joins the checkpoints, and the
unit's hard safety rule applies to us — never crash Weston, because its death
triggers the MICOM watchdog reset of the whole head unit
(`docs/ui/deployment-modes.md:249`).

**The measurement that decides whether this is cheap or expensive** — run it in
the car, as root, before writing another line of the Bluetooth side:

```
make target-bt-owner     # musl, baseline ISA, static-pie (§5d)
./bt_owner.target
```

| Output | Meaning |
|---|---|
| `sdp=ours-possible rfcomm8=ours-possible` | we own the bootstrap: the vendor stack is only pairing, and the Bluetooth half of this route is software we already have |
| `vendor-owned` on either socket | that socket needs the vendor ABI (`vbtd`), and the honest options are finding that API or the bridge board |
| `kernel-unavailable` | the `bluetooth`/`l2cap`/`rfcomm` modules are not loaded; load them and re-run — a different problem from an occupied port |
| `permission-denied` | run as root; on the unit we are |

The probe also sweeps RFCOMM channels 1–14, so if channel 8 is taken the output
says which channels are free instead of only that one is not.

### 5e-2. The video path on the unit: one sink process, measured end to end

Ghidra + rootfs dependency analysis of `AppProjection` (2026-11-23, done offline
by the "Repó értékfelmérés" tab) settles how video works there — and it is not
in-process:

| Question | Answer |
|---|---|
| Does the projection app decode H.264 itself? | **No.** Its `NEEDED` list has no `libavcodec`, `libOMX*`, `libva*`, `libmfx` or `v4l2`; `libAirPlay.so` contains no `h264`/`avcodec`/`OMX` string either. Decoding belongs to a vendor media/pipeline service and the app only receives the surface |
| What does the projection app depend on? | `libivi-window-manager-api.so.1`, `libImageProvider.so.1`, `libpersistence_client_library.so.7`, `libCarPlayAppFramework.so`, `libIQAirPlay*`, Qt5 {Quick,Gui,DBus,Qml,Core}, `libusb-1.0`, `libudev`, `libdlt` |
| Is a hardware decoder available at all? | **Yes, as glibc libraries**: `libva` + `libva-drm/egl/wayland`, the Intel CM runtime `libigfxcmrt64`, and the drivers `i965_dri.so` + `i965_drv_video.so` |
| How does the app place its window? | Through `libivi-window-manager-api`: `ivi_window_manager_interface`, `iwm_request_move_layer`, `iwm_set_surface_to_top_order_within_layer`. No `wl_drm`, no `zwp_linux_dmabuf`, no NV12/I420 string in the app |
| Is there a zero-copy path anywhere in the vendor stack? | **No.** `AppProjection`, `libImageProvider`, `libCarPlayAppFramework`, `libAirPlay` and `libCarPlayMediaFramework` contain no `eglCreateImageKHR`, `EGL_EXT_image_dma_buf_import`, `eglCreateStreamKHR`, `gralloc`, `ashmem`, `binder` or `ion` symbol or string; `libImageProvider` is image *generation* (`qrcodegen`, Qt5 Quick, `libGLESv2`), not frame intake |
| Then what does the vendor stack receive? | **Encoded bytes.** `libCarPlayMediaFramework.so` exports `OSStatus ScreenStreamProcessData(ScreenStreamRef, const uint8_t *, size_t, uint64_t, CFLDictionaryRef, ScreenStreamCompletion_f, void *)` — a compressed H.264 stream, not pixels |
| Is the decoder stack usable on this rootfs? | **Yes, proven by tooling, with one path quirk**: `usr/bin/vainfo` exists beside the vendor VA test tools (`avcenc`, `h264encode`, `mpeg2vaenc`, `mpeg2vldemo`, `jpegenc`, `loadjpeg`). On the unit `vainfo` reports VA-API **0.99.0** and then fails: it opens `/usr/lib/media-libva/dri/i965_drv_video.so` and `va_openDriver()` returns -1 — the vendor's driver path, not `/usr/lib/dri`. So the helper must set `LIBVA_DRIVERS_PATH`/`LIBVA_DRIVER_NAME` (and `XDG_RUNTIME_DIR`), not assume the stock layout |
| What actually decodes video? | **GStreamer**, not a private codec: `ldd` on `AppProjection` pulls `libgstreamer-1.0`, `libgstapp`, `libgstvideo`, `libgstbase`, and `/usr/lib/gstreamer-1.0` ships a full element set. The running media stack is `playerengine` + `mediamanager` (`/app/bin`), and the vendor already drives VA-API from the shell: `AppEarlyCamera … h264 -vaapi -hw -uyvy` |
| Is there real TLS on the image? | **Yes**: `libssl.so.1.0.2` (462 KiB) + `libcrypto.so.1.0.2` (in `/lib`) — OpenSSL 1.0.2, glibc, so the same two-process rule applies, but the "no TLS provider exists" blocker is now only a packaging question |
| Is a USB device controller present? | **Yes**: `/sys/class/udc` lists `dwc3.2.auto` (with a `bind` node), so gadget role is wired |
| Can the same port be the **host** (what wired AA needs)? | **Yes, in the update flow it already is**: docs/01's rig is `HU USB host ──► dwc2 gadget`, and every SCOURGE stick run is the unit reading a mass-storage device. The port is therefore dual-role: host for USB media/updates, device for CarPlay (`g_iap2`/`usb_f_iap2` hold the UDC). What is unproven is switching it *at runtime* while the vendor projection app lives |
| Which vendor binary must be probed? | `/app/bin/vbtd` is a **symlink to `/tmp/vbtd`** whose md5 equals `vbtd-51`'s (`be8406c8…`); `vbtd-63` differs (`fc8b7922…`). So the exports live in `vbtd-51`, and `readlink -f /proc/<pid>/exe` names the loaded object exactly |
| Can the platform import a dmabuf as an EGL image? | **Yes.** `/usr/lib/libEGL.so.1` carries `EGL_EXT_image_dma_buf_import`, `EGL_KHR_image`/`_base`/`_pixmap`, `EGL_ANDROID_image_native_buffer` and `EGL_WL_bind_wayland_display` (`libGLESv2` has none of these — EGL owns the path) |
| Can the compositor take a dmabuf from a client? | **Yes: `zwp_linux_dmabuf_v1`, version 1.** Weston is **1.10.0** with `drm-backend` + `gl-renderer`; the `linux_dmabuf_buffer`/`bind_linux_dmabuf` names are internal helpers, and the drm-backend carries the full v1 error set (`ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_{ALREADY_USED,INCOMPLETE,INVALID_DIMENSIONS,INVALID_FORMAT,OUT_OF_BOUNDS}`). No Wayland headers exist on the rootfs (`usr/include/wayland*` is empty), so the interface is bound by hand at v1 weights — no later plane-offset fields, and no third-party XML in this repo |
| What window layer and input does our client get? | Layer: `/etc/xdg/weston/weston_1920.ini` sets `shell=ivi-shell.so`, `backend=ias-backend.so`, `ivi-module=ivi-window-manager.so,ivi-controller.so`, `application-layer-id=4000`, `base-layer-id=1000`. Input: `ivi-input-controller.so` is **not** loaded, so pointer/keyboard focus is not automatic — `ivi_input_set_accept_focus` needs that module, and loading it edits the watchdog-protected Weston config. Open decision |
| What does the decoder emit? | `NV12` (and `I420`) — `i965_drv_video.so` references `VA_FOURCC_NV12`, `gen7_pp_nv12_*` and `get_image_i420`, which is exactly the layout dmabuf/EGL import wants |
| Can VA-API hand a decoded surface out as a dmabuf? | **Yes, through the old API.** `libva.so.1.3903.0` (2017 vintage) exposes `vaAcquireBufferHandle` and does **not** have `vaExportSurfaceHandle` — the handle→fd route is the one that works here |

The zero-copy chain is therefore **alive on this platform**, whole:
`vaAcquireBufferHandle` (decoded surface → drm buffer handle → fd) →
`EGL_EXT_image_dma_buf_import` or `wl_linux_dmabuf` v1 → Weston 1.10's
`gl-renderer` `dmabuf_image_create`. Two consequences, both structural:

1. **The decoder cannot live in our binary, and it should not be a separate
   decoder hop either.** VA-API and the i965 driver are glibc `.so` files while
   this package's unit build is musl static-pie (§5d), so the video side is a
   **second process** — but that process is the *sink*, not a broker: it is the
   Wayland client. It owns the `wl_surface`, the ivi surface and the input
   events, decodes with VA-API, and imports its own frames. The musl daemon keeps
   the protocol (bootstrap, TCP, TLS, session, sensor/EV) and forwards H.264
   access units over a UNIX socket; **no dmabuf fd crosses a process boundary**,
   which is why `SCM_RIGHTS` is not part of the design. This is not a new
   mechanism in this repo: `packages/wifi1b/scripts/build_carplay_native.py`
   already builds such a binary against the owner's rootfs glibc 2.25 and refuses
   any imported GLIBC symbol above 2.25.
2. **The protocol choice is forced by the compositor, not chosen.** Weston 1.10
   offers `wl_linux_dmabuf` v1, an unstable interface whose client binding we
   write by hand (the same way the Bluetooth sockets are hand-declared — no
   third-party XML is vendored into this repo); `wl_drm` +
   `EGL_WL_bind_wayland_display` is the alternative if the v1 binding proves
   awkward. `zwp_linux_dmabuf_v1` simply does not exist on this image, so no
   amount of client-side work makes the modern interface appear.

**Wired AA is a shorter road than this section implies**, and the measurement above
changed its standing. Wired AA wants the head unit to be the USB **host** and the
TLS **client**, with the phone as the accessory and TLS server — which is exactly
the role `aa_session` already implements; the AOA request table in `aa_aoap` is
the sequence, only the host-side sender is missing. Going wired removes the three
most expensive wireless-only pieces outright: the vendor SDP/RFCOMM shim (§5b),
the 5 GHz AP + DHCP, and the RFCOMM five-stage bootstrap. What it adds instead:
a cable, a runtime USB role switch (stop the iAP2 gadget and CarPlay projection
while AA is up), and a real TLS provider (libssl 1.0.2 is on the image). Video
stays the common cost either way.

What is still open on this track: the **input path** — whether a new ivi surface
receives pointer/keyboard focus directly or only through the vendor
`hmi-controller`/`ias-shell` layers (that decides how AA touch/key events get
back to the phone) — and the `vainfo` profile list, which decides which H.264
profile the phone may use. The `wl_shm` copy path stays as the fallback that
costs CPU on an Atom-class part.

### 5e-3. The fallback: the bridge board

If the probe says `vendor-owned`, the board is not a preference but a
requirement list read off the measured unit — route B (pass-through) needs no
video decode, so a board forwards the session instead of rendering it:

| Requirement | Why it is a requirement, not a nicety | Disqualifies |
|---|---|---|
| 5 GHz AP | the phone only joins 5 GHz for wireless AA, and the bootstrap announces the AP's band; the unit's own AP is `band 5` | single-band 2.4 GHz boards |
| USB **device**/gadget port | the head unit is the USB **host**; the bridge must enumerate as an AOAP accessory (AA6) | boards with a power-only or single-role USB port |
| Own Bluetooth + recent BlueZ with `--compat` | the AA UUID record, HFP AG and RFCOMM ch 8 live there (§5b) | any board expected to reuse the unit's `vbtd` |
| ~3 A peak at 5 V | the car's data port does not deliver Pi-class current | powering from the port alone |

| Board | 5 GHz | USB gadget | Verdict |
|---|---|---|---|
| Raspberry Pi 4B, 2 GB | yes | yes (`dwc2` on the USB-C port) | **Recommended** — it is the reference hardware of the guide the protocol facts come from, so the BlueZ/SDP behaviour matches what §5b pins |
| Pi Zero 2 W | **no**, 2.4 GHz only | yes | Out: cannot host the AP the phone requires |
| Pi 5 | yes | USB-C is power-only; gadget mode on the RP1 ports is unproven | Only after a bench test |
| Orange Pi Zero 3 class | yes | kernel-dependent | Cost fallback |
| Reflashing an existing AA/CP dongle | — | — | Out: signed firmware, and the one in use works |

Power: USB-C splitter (data + PD) or 12 V→5 V from the fuse box; heat and space
in the glovebox are the practical limits, not the CPU.

**What is still open in the fallback:** which board is at hand and how it will be
powered. Everything else — band, gadget role, BlueZ ownership — is fixed by the
measurements above. This is no longer the plan: it is what we buy if
`bt_owner` reports `vendor-owned`.

## 6. Gates

1. Frame and session round-trips against fixed external vectors; malformed
   lengths, flag misuse, fragmentation, interleaving and buffer bounds.
2. State machines survive out-of-order, duplicate and late messages, transport
   loss, cancellation and reconnect with generation-bound teardown.
3. Features: EV data only ever reflects real vehicle state (never synthesized),
   and a missing value stays missing instead of being invented.
4. Cluster/HUD delivery is gated behind an on-car test with the owner present;
   no writes to the cluster while the car is moving.
5. The stock USB path, modem routing, shared micom helper, Battery, Tweaks and
   approved Reboot behaviour stay untouched.
6. Everything that is not car-validated is marked WIP wherever it appears.

## 7. Preserved boundaries

Inherited from `docs/wireless-carplay/PROGRESS.md` and `AGENTS.md`: owner
hardware only, no OEM binaries or credentials in Git, exact hash gates for any
firmware-specific byte change, no global radio or Bluetooth replacement, no
authentication bypass, no guessed ABI, first-action cleanup stays first, and
the `wifi1b` C1-reviewed index is not disturbed by this work.

## 8. Next action

The order changed with the decision in §5e. On-unit means the unit is the
receiver, so the cheap measurements come first and the video track is the real
cost:

1. **AA9c — the active shim** is the Bluetooth half's next implementation step
   (§5b): in `tools/bttap`, `dlsym` the vendor exports, add the AA SDP record
   (AA UUID alone, no SPP UUID, RFCOMM channel 8), accept the RFCOMM connection
   and pump its bytes to `aa_btboot` in the musl daemon over a UNIX socket. The
   plugin is glibc-2.25, opt-in, revertible, and deliberately dumb: no protocol
   code, no allocation in callbacks, because a fault inside `vbtd` costs the car
   its Bluetooth.
   *Ownership: handed to the "Repó értékfelmérés" tab, which holds the SSH link
   to the unit and the Ghidra workspace; the same request carries the crypto and
   decoder library inventory (`ldd /app/bin/AppProjection`, `ls /usr/lib`).
   Results come back here into §5b and the AA9/AA10 rows.*
2. **AA10a — the platform's zero-copy question**, now that the vendor binaries
   are ruled out (§5e-2: they hand encoded bytes to a decoder service and never
   touch a dmabuf): does `libEGL` expose `EGL_EXT_image_dma_buf_import`, does
   Weston offer `wl_drm`/`zwp_linux_dmabuf_v1`, and does the installed `libva`
   expose `vaExportSurfaceHandle`? Missing pieces drop the target to 720p30 over
   `wl_shm`. *Ownership: the "Repó értékfelmérés" tab's probe — those three
   `strings`/`ls` checks plus `ps`/`/proc/*/maps` (with the found service's
   open fds) to name the decoder service and the channel it reads. If the maps
   sweep finds nothing, the decoder is a kernel V4L2 node and the next check is
   `/dev/video*` + `VIDIOC_QUERYCAP`, not VA-API.*
3. **AA7-unit — the bootstrap on the unit itself**: our SDP record (AA UUID
   only), the RFCOMM listener driving `aa_btboot`, pairing/discoverability
   through the vendor CommonAPI adapter, the existing 5 GHz AP, and the TCP
   listener that feeds `aa_session_feed` — plus a real TLS provider, either the
   unit's own libssl or one built into the musl-static binary.
4. Fallback only: the bridge board (§5e-3) and AA8's phone-role mirror, which is
   what a bridge needs to drive a stock head unit.

Role map to keep in mind (from the same reference commit): wired AA puts the
head unit in the USB-host *and* TLS-client role (SSL_set_connect_state) with the
phone as accessory and TLS server; wireless AA keeps those TLS roles and only
swaps the pipe, with the phone dialling the endpoint the head unit announces
over RFCOMM. Our `aa_session` is the head-unit role — the useful half for the
wireless side and for tests — and the mirror is the half that talks to the car.

Open follow-ups (on-unit): the discovery response advertises sensor, navigation and input
only, so the phone will not open video or audio against this checkpoint; their
focus requests are counted as ignored. The navigation channel number 9 is a
local assignment declared in the descriptor, not a protocol constant. The TLS
seam takes a provider from the transport; until AA6/AA7 exist, the only provider
is the deterministic test one, which is not a cipher. Two EV items are still
open: the discovery response does not yet declare the electric fuel type or EV
connector list (the phone currently relies on sensor type 23 alone), and no
capture proves the VEM is enough for arrival estimates on this car — the battery
figures come from Battery's I-CAN decode, which is validated on the car, but the
Maps side is not.
