<!-- SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors -->
<!-- SPDX-License-Identifier: AGPL-3.0-only -->

# Custom UI — logic review (redundancy first)

Scope: the six first-party pages, measured on 2026-11-22 with
`tools/qmlpreview/render.sh` at 1280×720 and 1920×720 (see
`docs/ui/` and `out/preview/`). Layout defects are clean now (no clipped
single-line text, no vertically clipped wrapped text); what is left is
**information architecture**.

## WifiPage.qml — 1505 lines, 4 tabs

| Tab | Lines | What it owns | What it repeats |
|---|---|---|---|
| Status | 565–805 | mode, WLAN interface, imported creds, IP, SSH state, AP state, Key, buttons Off/Client/CarPlay AP/Import USB/Save logs | `Key:` (:748) also on CarPlay AP (:1047); `IP` (:640) also on Client; `SSH` (:770) also on the SSH tab; `Import USB` (:674) also at :911 and :1161 |
| Client | 806–1015 | scan list, connect/disconnect, "How it works" | `Import USB` (:911) is the same action as :674 |
| CarPlay AP | 1016–1227 | AP state (ready/band/channel/mDNS/iAP2/BT/device id/key), iPhone steps | iPhone steps also stated on Status; `Key:` (:1047) duplicates :748 |
| SSH | 1228–1505 | SSH server detail, authorized keys, notes, **Live log** (:1425) | `Port`/`Secure console` also on Status; the page already has a right-hand log console, so `:1425` is a second log view |

**Redundancy in numbers:** `Import USB` 3× in one page, `Live log` 2× on the
same page, four status readouts (mode/IP/SSH/key) rendered twice each, and the
CarPlay pairing instructions written in two tabs.

### P0 — DONE (2026-11-22)

Applied to **WifiPage.qml** and **Wifi1bPage.qml**: one always-visible action row
(`Import USB`, `Save logs`) at `x: 800, y: 104`, and the three per-tab `Import
USB` copies (plus the second `Save logs`) removed. Both pages are now `1 import,
1 save` and re-render clean at 1280×720 and 1920×720 (`tools/qmlpreview`). The
SSH tab never had its own log view: `Live log` is the single page-level console,
so nothing was removed there.

### Original proposal (P0 — cheap, no behaviour change)

1. **One readout strip in the header**, always visible: `mode · ssid · ip ·
   ssh port/state · ap state`. Delete the per-tab copies (`Key:` x2, `IP`,
   `SSH`, `Port`, `Secure console`).
2. **One `Import USB`** in that strip's action row (keep :674, delete :911 and
   :1161) and **one `Save logs`** (:680).
3. **One log view**: keep the page-level console, drop the SSH tab's
   `Live log` block (:1425) and point the tab at the same console.

### Proposal (P1 — structure)

Tabs `Status` + `Client` + `SSH` say the same thing three ways. Suggested
three tabs, each owning **one actor**:

* **Client** — wifi.txt import result, scan list, connect/disconnect, the one
  "how it works" block.
* **CarPlay AP** — AP state + the iPhone steps (the only place they appear).
* **SSH** — server state, keys, one button pair (Restart/Stop).

`Status` disappears: its content is the header strip plus `Save logs`.
Expected size: ~1505 → ~1100 lines, and no field rendered twice.

## Wifi1bPage.qml — same shape, same issues

4 tabs, 3× `Import USB`, `Restart stack`, `Save logs`, `Scan`,
`Connect imported`, `Stop SSH` all mirror WifiPage. Whatever is decided for
WifiPage should be applied here in the same pass, otherwise the two pages drift
apart again (they already differ in wording: `SSH is key-only…` vs
`Password auth is enabled…`).

## BatteryPage.qml — logic is clean

No layout defects, one status source (`/status` + `status.json` mirror), one
console. The only duplication is the daemon-message line shown both in the card
header and in the overlay; that is intentional (card vs overlay context).

## AmbientPage.qml — clean

One tab-less page, three control groups, no repeated readouts.

## RebootPage.qml — clean, one nit

The confirmation dialog restates the pending action text that the page already
shows; harmless, but the dialog could show only the action + timeout.

## TweaksPage.qml — dense but not redundant

Five toggles with one description each, one verify block, one console. The
descriptions are long (up to 826 px unwrapped / wrapped fine), so the page reads
dense; consider collapsing the descriptions behind the existing help affordance
if the list ever grows.

## Cross-page duplication

`Import USB`, `Save logs`, `Restart stack`, `Scan`, `Stop SSH` exist in
both WifiPage and Wifi1bPage, and `Live log` in BatteryPage, WifiPage and
Wifi1bPage. Each page carrying its own copy is fine for layout, but the **wording
and the button set should come from one shared component** so a fix lands in one
place.
