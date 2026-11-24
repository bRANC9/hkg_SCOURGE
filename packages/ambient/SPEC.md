# Ambience (ambient lighting) — SPEC

| | |
|---|---|
| Vehicle | Hyundai Ioniq 5 Gen5W |
| Firmware | `NE_E.EUR.S5W_L.001.001.260128` |
| Component | `/rw_data/ambient/ambient_daemon` (static x86-64), service `ambient-daemon.service` |
| Interface | `127.0.0.1:8094` (localhost only) |
| State | `/rw_data/ambient/` (`config.conf`, `presets.conf`, `status.json`, `daemon.log`, `frames.jsonl` by default) |
| UI | `AmbientTile` + `AmbientPage` overlay inside `/app/share/AppHomeScreen` |
| Hub | the SCOURGE Hub (`bRANC9/scourge-hub`), package slug `ambient` |

**Status: WIP — host-validated only.** The daemon's API, effect engine,
transitions and audio analysis are covered by `tests/test_daemon.py`; the
installer and the QML injection by `tests/test_installer.sh`. No part of this
package has run on a car. The factory ambient frame layout is unknown (see
`docs/HARDWARE.md`), so the micom backend ships no default.

## 1. Purpose

Take over ambient lighting control with:

1. **Effects** — solid, breathe, pulse (beat), rainbow, wave, comet, sparkle,
   cycle, off.
2. **Transitions** — crossfade, fade through black, cut; configurable duration,
   applied on every effect/colour/preset change.
3. **Music reactive** — an optional audio tap (raw S16LE) that modulates
   brightness, hue and beat-triggered flashes.
4. **Zones** — up to four independently coloured zones (phase-offset effects),
   mapped onto segments of the output backend.

The UI only draws and calls HTTP. All timing lives in the daemon so the effect
keeps running with the page closed.

## 2. Endpoints

| Method | Path | Purpose |
|---|---|---|
| GET | `/health` | liveness |
| GET | `/status` | full state: effect, colour (RGB + HSV), brightness, speed, intensity, transition, reactive, audio levels, backend, frame counters, last error, presets, `home_seq` |
| GET | `/frame` | render now and return the zone colours (`zones`), transition flag |
| GET | `/effects` | the effect list with descriptions |
| GET | `/presets` | presets with their effect/colour/brightness |
| GET | `/logs` | tail of `daemon.log` |
| POST | `/on`, `/off`, `/toggle` | master power |
| POST | `/effect?name=[&speed=][&intensity=]` | select an effect (also forces power on) |
| POST | `/color?r=&g=&b=` or `?h=&s=&v=` or `?hex=` | base colour (RGB 0–255, HSV 0–1) |
| POST | `/brightness?value=0..100` | master brightness |
| POST | `/transition?ms=0..10000[&style=crossfade\|black\|cut]` | transition settings |
| POST | `/reactive?enabled=0\|1[&gain=0..400]` | audio modulation |
| POST | `/preset?name=[&action=apply\|save\|delete]` | presets (default action `apply`) |
| POST | `/config?backend=&target=&target_port=&fps=&output_hz=&audio_cmd=&audio_file=&micom_template=` | output/audio configuration |

Validation is strict: unknown effect/backend/style, out-of-range numbers or a
missing parameter answer `400` with a readable `error`; the UI shows it in the
footer. The daemon never silently clamps a request it did not understand.

## 3. State model

```
power, effect, color(rgb), brightness(0..100), speed(0..100),
intensity(0..100), transition_ms(0..10000), transition_style,
reactive, audio_gain(0..400), zones(1..4)
```

Runtime configuration (not part of a preset): `backend`, `target`,
`target_port`, `fps`, `output_hz`, `micom_template`, `micom_send`, audio
source.

Persistence: `config.conf` is rewritten atomically on every mutation (unless
`--no-persist`) and read back at start (unless `--no-restore`).
`presets.conf` stores one line per preset:

```
name|effect|r,g,b|brightness|speed|intensity|transition_ms|style|reactive
```

Four defaults (Relax, Drive, Party, Night) exist in code; a `presets.conf`
adds to them, it never wipes a saved preset it does not mention.

## 4. Effect engine

Frames are pure functions of time (and of the audio envelopes), rendered at
`fps` (default 25). `speed` scales the effect's internal time
(0.25×…2.25×), `intensity` drives sparkle density, `brightness` is applied
last.

| Effect | Behaviour |
|---|---|
| `off` | black |
| `solid` | the base colour |
| `breathe` | sinusoidal brightness, 25–100 % |
| `pulse` | beat-synced flash; without audio, a 1.5 s sine pulse |
| `rainbow` | hue rotation, one offset per zone |
| `wave` | travelling brightness wave across zones in the base hue |
| `comet` | a moving head with a quadratic falling tail |
| `sparkle` | dim base colour plus random twinkles per zone |
| `cycle` | drifts through hues around the base colour |

Reactive modulation (when `reactive` is on and audio is live):
`v × (0.35 + 1.45 × level)`, hue shifted by `0.10 × (high − mid)`, and the beat
flash brightens comet/sparkle/pulse.

## 5. Transitions

On any change of effect, colour or preset:

- `crossfade` — smoothstep blend from the last rendered frame to the new one,
- `black` — the old frame dims to black over the first half, the new frame
  rises over the second half,
- `cut` — immediate.

`transition_ms = 0` is treated as a cut. The `transition` field of `/frame`
says whether a blend is in progress.

## 6. Audio

Raw **S16LE mono**, `--audio-rate` (default 16000). Source is `--audio-cmd`
(child process stdout) or `--audio-file` (FIFO/growing file). 10 ms hop:

- RMS → envelope (attack 0.55 / release 0.08) → `level`,
- RBJ band-passes at 60/450/3000 Hz (Q 1.0) → `bass`/`mid`/`high`,
- beat: bass > 1.45 × slow average, 150 ms refractory.

The daemon never opens ALSA (static glibc on the vendor kernel, no NSS).
`audio.error` in `/status` reports a dead source instead of pretending.

## 7. Output backends

See `docs/HARDWARE.md`. `none`, `file`, `wled`, `micom`. Frames are sent at
`output_hz` (default 20) and only when the rendered frame actually changed.
`frames`, `frames_sent`, `output_errors`, `last_error` are in `/status`.

## 8. UI

`AmbientPage.qml` follows the Gen5W overlay contract (`docs/ui/`):

- `y: ConstantModel.MENU_BAR_Y`, height `DEVICE_HEIGHT - MENU_BAR_Y`;
- `visible: requested && !AppHomeModel.isAtOtherApp`, `enabled` gated on
  `sysPopupShowing`;
- closes on the back button, `isAtOtherApp`, `currentScreen`,
  `currentHomeAdvancedPageIdx`, `requestHomeByPP` and on `home_seq`;
- a full-screen `preventStealing` MouseArea shield, content at `z: 2`;
- `home_seq` comes from the daemon's session-bus watcher (`dbus-monitor` on
  `NotifyRequestHomeScreen` / `custom_key_value`), never from QML.

Colours, fonts and control shapes use the decoded eco3.0 tokens
(`ConstantModel.*`, `HomeThemeHelper.*`) — no hard-coded theme colours.

## 8b. Manual install over SSH (no stick, no Store)

Once the WiFi package's dropbear is up (`car`/`electric` on port 2222, UID 0),
the same payload installs by hand — the installer does not care whether the
stick, the Store or a shell started it:

```sh
scp -P 2222 ambient-<version>.tar.gz car@<unit>:/tmp/ambient.tar.gz
ssh -P 2222 car@<unit>
# on the unit:
mkdir -p /tmp/ambient && cd /tmp/ambient
tar -xzf ../ambient.tar.gz
USB_DIR=$PWD sh run_stuff.sh          # root; the log is /rw_data/ambient/install.log
cat run_stuff_result.txt              # expect status=OK inject=tile+page
```

The installer reads its own SMACK label from `/proc/self/attr/current`; the
dropbear daemon carries the floor label `_`, the same one the USB failsafe
runs under, so the label gates behave identically. If the label cannot be
applied and read back the install aborts instead of leaving a daemon that
cannot reach D-Bus. Restore is the same with `run_stuff_restore.sh`.

The hub catalogue is fetchable from the unit too (the index is public and
ECDSA-signed; pairing is only needed to *queue* an install from the hub UI):

```sh
wget -qO- "http://<hub>/api/v1/index/NE_E.EUR.S5W_L.001.001.260128/EU/qml_eco3.0/ambient.json"
```

that entry carries the artifact URL and sha256, so a fetched tarball can be
verified with `sha256sum -c` before it runs — the same gate `store_daemon`
applies, done by hand.

**NEMESIS packages are not obtainable-and-runnable this way.** They are
Android zips (`NEMESIS.data` + APKs/scripts) for `S5W_M` units with the NEMESIS
runtime installed; this unit is `S5W_L` Linux and has neither the stack nor the
runtime. Downloading the file proves nothing — only the target runtime could
execute it.

## 9. Install

The package is hub/USB delivered and has **no build-time QML base**: the
installer copies the page/tile/slider, then injects the two instance snippets
into the live `QHMI_HomeScreenMain_Advanced.qml` with
`payload/lib/qml_inject.sh`.

- A base with the SCOURGE allocator (`function firstEmptyIndex`) gets the tile
  (ordinal 6) **and** the page.
- Any other base gets the page appended before the root item's closing brace —
  the app stays reachable, the tile is skipped and the result says
  `inject=page-only (no tile allocator)`.
- The injector is brace-matched, verifies the brace balance, keeps a backup in
  `/rw_data/ambient/qml-backup/` and preserves SMACK64.
- `run_stuff_restore.sh` reverts the injection and removes the app; the
  configuration survives.

Privileged files (daemon, `CANService`, `micom-send`, `dbus-monitor`) are
stamped with the installer's own SMACK64EXEC label and the installer aborts if
the label cannot be applied and read back. The running daemon's label is
checked after start.

## 10. Safety

- no firmware, updater, bootloader or partition changes;
- the daemon binds `127.0.0.1` only;
- the micom backend never sends without an explicit template;
- installs are idempotent (re-running replaces the files and re-injects only
  when the marker is absent);
- output failures are reported, never retried in a tight loop.

## 11. Publishing to the hub

The hub imports a package from a GitHub release: one `<slug>-<version>.tar.gz`
whose **root is the USB payload tree** (`run_stuff.sh`, `payload/`,
`manifest.md5`, `README.txt`) plus a `<slug>-<version>.json` sidecar.

```sh
sh scripts/package_release.sh 1.0.0        # -> dist/ambient-1.0.0.{tar.gz,.json}
```

The script builds the payload and the icons, writes the tarball with fixed
ownership/mtime, and emits the sidecar with the tarball's sha256, the firmware
key (`NE_E.EUR.S5W_L.001.001.260128` / `EU` / `qml_eco3.0`) and the store-page
copy. Upload all three to the release; the hub's `sync_sources` imports the
asset and applies the sidecar to the `Package` row. Publishing goes through the
hub's field-verified gate: the release only enters the public index after a
unit that ran it checks in with `status=ok` — which for this package means the
first real car result. Until then it stays WIP.

## 12. Tests

`tests/test_daemon.py` (host, no car): builds the daemon, starts it on a free
port with a temporary state dir, and checks the API contract, effect rendering,
colour conversion, transitions, audio analysis from a synthetic PCM stream,
reactive modulation, presets, persistence across a restart, validation
failures and the file backend.

`tests/test_installer.sh` (host, no car): builds the payload, runs the real
installer against a fake rootfs tree with stubbed system commands, then checks
the files, the service unit, the injection and the restore round-trip.
