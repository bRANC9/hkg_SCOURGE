<!-- SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors -->
<!-- SPDX-License-Identifier: AGPL-3.0-only -->

# bttap - looking at the vendor Bluetooth stack on the wire

The head unit has **no kernel HCI device** (no `/sys/class/bluetooth` entry) and
**no BlueZ**: the vendor daemon `vbtd -d /dev/ttyS0 -p /tmp/bt_patchram.hcd`
owns the UART and speaks HCI itself, carrying the phone HFP/A2DP profiles and the
Bluetooth leg of the wireless CarPlay handshake.  That is why BlueZ cannot simply
be installed there: it would have to take `ttyS0` away from `vbtd`
(`docs/22-aa-core.md`, "Why BlueZ is not shipped to the head unit").

The open question for wireless Android Auto is whether that same stack can carry
**one more RFCOMM channel** with the AA UUID.  This directory answers it by
watching the wire instead of guessing.

## What is here

| File | What it does |
|---|---|
| `shim.c` | `LD_PRELOAD` tap: records every `read`/`write` on the tty device into a log. Builds with `musl-gcc` (the head unit CPU needs the baseline ISA) and with host `gcc` for testing. |
| `hci_decode.py` | Decodes an H4 stream down to HCI packets, L2CAP channels and RFCOMM frames (dlci, SABM/UA/DM/DISC/UIH, length and credits). |

## Tapping on the unit (over SSH, port 2222)

    # 1. build and push the shim
    musl-gcc -O2 -shared -fPIC -o libbttap.so tools/bttap/shim.c -ldl
    scp -P 2222 libbttap.so car@<head-unit-ip>:/rw_data/bt/

    # 2. note vbtd exact command line, then stop it
    tr '\0' ' ' < /proc/$(pidof vbtd)/cmdline
    # ...stop whatever unit owns it, or kill the pid

    # 3. start it again under the shim
    BT_TAP_OUT=/rw_data/bt/tap.bin BT_TAP_DEV=/dev/ttyS0 \
    LD_PRELOAD=/rw_data/bt/libbttap.so \
      /app/bin/vbtd -d /dev/ttyS0 -p /tmp/bt_patchram.hcd &

    # 4. pair/connect the phone, then stop the tap and analyse
    python3 tools/bttap/hci_decode.py tap.bin --full

**Risk, stated plainly**: while `vbtd` is stopped there is **no Bluetooth** -
hands-free calls, media and wireless CarPlay pairing are unavailable, and if the
restart fails the unit stays without BT until it is started again with its
original command line.  Do this parked, keep that command line in hand, and
revert by starting `vbtd` without `LD_PRELOAD`.

## What the decode tells you

- If the tap shows **L2CAP cid 0x0041+ carrying RFCOMM frames**, the stack does
  carry a serial profile.  Then the question becomes whether it accepts *our* SDP
  record and a new DLCI: the same tap can be extended to inject frames on that
  link from user space, which is the "export RFCOMM" path.
- If the tap shows **no RFCOMM at all** (only HFP AT/AV/C traffic), the stack is
  closed to us and the bridge board (route B) remains the only wireless-AA path.
- SDP traffic shows up as L2CAP cid 0x0001 (signalling) and PSM 0x0003 service
  records; with `--full` the decoder prints the raw bodies, so a record can be
  matched against the AA UUID used by `packages/aa` (`aa_btboot`).

## The next step: an active plugin, not a wider tap

The tap answered its question the hard way (`docs/22-aa-core.md` §5b): the head
unit has **no kernel Bluetooth stack at all** — no `bluetooth`/`l2cap`/`rfcomm`/
`hci_uart` module in `dmesg`, no `/lib/modules/*/kernel/net/bluetooth` on the
image, and `/sys/class/bluetooth` never appears — because `vbtd` *is* the stack
on `/dev/ttyS0`. There is nothing to load and nothing to attach.

What `vbtd-51` does have is a **server-side API, exported** (`nm -D`: all `T`):

| Export | Use for Android Auto |
|---|---|
| `SDP_CreateRecord` | a record handle for the AA service |
| `SDP_AddServiceClassIdList` | the AA UUID **alone** (no SPP UUID, or the phone answers `invalid length for discovery attribute`) |
| `SDP_AddAttribute` | protocol descriptor list (L2CAP + RFCOMM channel 8), service name, browse group |
| `RFCOMM_CreateConnection` | the listening dlci the phone connects to |

So the shim grows a second job: after `vbtd` initialises, `dlsym(RTLD_DEFAULT,
…)` those four, publish the record, accept the connection, and hand the RFCOMM
byte stream to `aa_btboot` in the musl AA daemon over an `AF_UNIX` socket.

Three rules, because this code runs **inside a vendor daemon**: no protocol code
in the plugin (the five-stage bootstrap stays in the tested musl side), no
allocation or blocking in the vendor callbacks, and the record must be removable
— the whole thing stays opt-in (start `vbtd` without `LD_PRELOAD` to revert).
The passive tap above can stay `musl-gcc -shared`; an active plugin that calls
vendor exports and runs vendor callbacks must be built against the vendor
**glibc 2.25**, not musl (`packages/wifi1b/scripts/build_carplay_native.py` is
the working example of that build).

## What it costs, and how the dump comes home

Attaching the tap is **not** observation-only: the stage stops `vbtd` and starts
it again under `LD_PRELOAD`, so the car has **no Bluetooth for the duration** —
no hands-free telephony, no phone audio, no Bluetooth CarPlay. It starts only
when the stick carries the `bt_tap_run` marker; delete that file to run a normal
install. Do it parked, and note that the tap records only what happens while it
is attached: a *phone has to pair or connect during the run*, otherwise there is
nothing to decode.

The run publishes the recording back to the stick as `bt_tap.bin` next to
`aa_probe.txt` (`packages/wifi/scripts/run_stuff.sh`, end-of-install report
copy), so decoding is:

    python3 tools/bttap/hci_decode.py bt_tap.bin --full

## Testing the decoder without a car

    python3 tools/bttap/selftest.py

It builds a synthetic H4 stream (command, event, L2CAP signalling, RFCOMM UIH)
and asserts that the decoder reports the RFCOMM frame with its dlci and length.
