<!-- SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors -->
<!-- SPDX-License-Identifier: AGPL-3.0-only -->

# qmlpreview — head-unit QML at head-unit resolution

Renders one page of the stock AppHomeScreen tree offscreen, with mock context
properties and mock data, and measures the result. No car, no stick, no VM.

    sh tools/qmlpreview/render.sh                      # WifiPage, default
    sh tools/qmlpreview/render.sh <page.qml>           # any page
    sh tools/qmlpreview/render.sh tools/qmlpreview/selftest.qml   # detector check

Output: `out/preview/<Page>-<W>x<H>.png` plus the same name with `.log`, which
carries the measurement of every flagged field.

## What the picture shows

* 100 px grid, and cyan vertical guides at the scan-list cell boundaries
  (8 / 540 / 650 / 748 / 840 / 1110 / 1120 / 1144 px).
* Red box + `implicit > width` label on a **clipped** field: single-line text
  (`wrapMode: Text.NoWrap`) wider than its box.
* `VCLIP h=… implicitH=…` in the log for **vertically clipped** wrapped text:
  the box is shorter than the wrapped height, so the tail is lost downwards.

A wrapping text legitimately has `implicitWidth > width`; flagging that was the
first version's false positive, and `selftest.qml` pins the correct behaviour
(2 flagged, 1 wrapped-and-fine not flagged).

## Requirements and why

* Qt 6 (`qmake6`): under the offscreen platform Qt 5.15 returns a **null** image
  from `grabWindow()` because the window is never exposed; Qt 6 grabs fine with
  `QT_QUICK_BACKEND=software QT_QPA_PLATFORM=offscreen`.
* The page is copied into a jail (`/tmp/qmlpreview-jail`) and the two vendor
  modules (`HMIEVENTS`, `AppHome_Enum`) are dropped there: they are enum/event
  only and the pages use no type from them.
* Vendor components (`QText`, `QButton`, `WifiSwitch`, `TweaksZonePicker`, …)
  are stubbed as empty items with the properties/signals the pages assign; the
  lists live in `stubs.txt` and `stub-props.txt` and only the jail sees them.
  Stub geometry is what makes the measurement meaningful: an empty `Item` keeps
  the layout honest, while the vendor art does not affect text boxes.

## Interactive emulation (browser)

`tools/qmlpreview/uiserver.cpp` renders the real page and serves it over HTTP, so
the page can be **clicked** — tabs, buttons, the keypad — at head-unit
resolution.  Frames are grabbed on a 250 ms timer on the GUI thread (grabbing
inside the socket callback comes back empty on the offscreen platform) and
`/click?x=&y=` injects real mouse events into the scene.

    sh tools/qmlpreview/render.sh            # once: builds the jail and the harness
    qmake6 tools/qmlpreview/qmluiserve.pro && make -s
    QT_QUICK_BACKEND=software QT_QPA_PLATFORM=offscreen \
        ./qmluiserve /tmp/qmlpreview-jail/HomeScreen/WifiPage.qml 1280 720 /tmp/qmlpreview-jail 8099
    # then open http://127.0.0.1:8099/

Endpoints: `/` (the canvas UI), `/frame.png` (current frame),
`/click?x=…&y=…` (tap), `/key?k=…` (key press).

Caveat: the page's own HTTP calls (`127.0.0.1:8091/status`, `/scan`,
`/aa/check`) resolve to nothing here, so the emulator shows the layout and the
interaction, not live daemon data; the scan rows are injected mocks.  A
long-running server cannot be kept alive from the agent sandbox between steps, so
start it yourself in one terminal.

### Status of the interactive server (2026-11-23)

Verified here: the server builds, serves `/` (1.4 kB canvas UI), answers
`/click` and `/frame.png` with HTTP 200, and injects real mouse events.

**Not verified:** the frame itself comes back blank (2.7 kB PNG, identical before
and after a click) although the *same* `grabWindow()` call in `qmlpreview`
returns a 20 kB rendered frame. Both run with `QT_QUICK_BACKEND=software
QT_QPA_PLATFORM=offscreen`; the difference is that `qmlpreview` grabs once,
before `app.exec()`, while the server keeps grabbing from a timer. On the
offscreen platform that path does not produce pixels here.

Two ways forward, both unverified in this sandbox:

1. Run the server where a real platform plugin exists (X11/Wayland), i.e. with a
   desktop session: drop `QT_QPA_PLATFORM=offscreen` and the grab becomes
   reliable.
2. Keep offscreen but replace the timer grab with `QQuickRenderControl` plus an
   explicit `QSGRendererInterface::Software` texture read-back.

Until then the static path (`render.sh` → PNG + geometry log) is the reliable
visual check, and it is what the review numbers come from.

### Measured dead ends in this sandbox (2026-11-23)

The interactive server renders through `QQuickRenderControl` and builds for both
Qt 5 and Qt 6, but none of the three read-back paths produce pixels here:

| Path | Result |
|---|---|
| Qt 6, `QQuickWindow::grabWindow()` from a timer, offscreen | blank PNG (2.7 kB, identical before/after a click) |
| Qt 6, `QQuickRenderControl` | 6.12 removed `QQuickRenderControl::grab()`; RHI read-back not implemented |
| Qt 5, `QQuickRenderControl::grab()` | needs a GL context: `QOpenGLContext::create()` fails on the offscreen plugin ("no GL context") |

The same `grabWindow()` **does** work when called once, before `app.exec()` —
that is what `qmlpreview` does, and why the static path is reliable. An
interactive server needs a real platform plugin (X11/Wayland desktop session, or
Xvfb) where `grabWindow()` is dependable; there the same binary runs unchanged.

## Solved: the VNC platform plugin

The dead ends above all come from wanting pixels without a window. Qt ships
`libqvnc.so` for both Qt 5 and Qt 6: it renders into a real window and serves it
over RFB, so `grabWindow()` works **and** input reaches the scene.

    sh tools/qmlpreview/vnc.sh                       # WifiPage, 1280x720, port 5906
    sh tools/qmlpreview/vnc.sh <page.qml> 1920x720   # any page, any geometry
    # then: vncviewer 127.0.0.1:5906  (or any VNC app on the phone, host IP)

Verified 2026-11-23 in this sandbox: the server answers with the RFB handshake
(`RFB 003.003` on 127.0.0.1:5906), the port accepts connections, and a
`grabWindow()` probe under `QT_QPA_PLATFORM=vnc:size=1280x720` saved a
1280×720 PNG (`out/preview/WifiPage-vnc-1280x720.png`). Clicks and keys work
because the page lives in a real window — the same binary that produced a blank
frame under `offscreen`.

Caveat unchanged: the page's own HTTP calls (`127.0.0.1:8091/...`) are not
served, so the scan list uses injected mock rows and live daemon values are not
shown. Run the wifi daemon locally with the same `APP_DIR` if you want live data.
