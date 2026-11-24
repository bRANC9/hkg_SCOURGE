# Ambience — hardware and output backends (WIP)

Everything here is host-tested only. Nothing in this package has been
validated on a car. Treat the section numbers as claims with evidence, not
facts about every trim.

## 1. What the head unit can actually reach

The Gen5W unit has `can0` = **I-CAN** (powertrain/charging frames) and, through
the vendor micom D-Bus service, the M-CAN send path
(`com.lge.car.micom /Can SetCANMicomRecv`). Interior ambient lighting on HMG
cars is normally a BCM/lighting-module function on a body bus, and this unit
ships no ambient-lighting UI or CAN API for it. Two consequences:

1. There is **no documented factory frame** for "set ambient colour" that this
   project has verified. The micom backend therefore ships **no default
   template**: you supply the frame layout yourself once you know it.
2. The reliable, reversible path is an **aftermarket RGB(W) controller** (an
   ESP/WLED box on the car's WiFi or a USB-powered LED strip controller) that
   the app drives over HTTP. That is what the `wled` backend does.

### Prior art on the sibling platform

The NEMESIS Package Store (Android `S5W_M` Gen5W units) lists **JMK Active
Ambient** (`jmk-ambient-sound`, "Ambient light follows the music — AI DJ, mood,
beat, mono") and a **JMK Launcher Menu** whose menu carries an ambient entry.
So a music-reactive ambient app already exists for the Android side of Gen5W.

That is a feature reference, not a transferable implementation: NEMESIS runs
Android processes on a different hardware/software combination
(`S5W_L` is explicitly unsupported there), and anything it drives is reachable
from the Android stack of that unit, not from this Linux `AppHomeScreen`. It
does confirm two things worth knowing before choosing a backend here:

- on those units the lighting exists as a controllable target from the head
  unit's own software, so a factory path may exist for some trims — but it is
  not documented for 260128 EU and this package has not found one;
- the accepted feature set (mood presets, beat/mono modes, an AI DJ-style
  selector) is worth matching in ours: presets, beat-driven effects and a
  monochrome mode are already in `AmbientPage`/the effect list.

Do not select the micom backend with a guessed frame. A wrong frame on the
M-CAN is at best ignored and at worst moves something else.

## 2. Backends

| Backend | What it does | Evidence |
|---|---|---|
| `none` | renders frames, sends nothing | host tests |
| `file` | appends `{"t","z":[[r,g,b],…]}` lines to `target` (default `frames.jsonl`) | host tests; **default on a fresh install** |
| `wled` | `POST http://<target>:80/json/state` with `{"on","bri","seg":[{"id","col"}]}` at `output_hz` | host tests against a socket-level fake; no hardware test |
| `micom` | spawns `/rw_data/ambient/bin/micom-send array:byte:…` built from `micom_template` | helper reuses the Battery package's whitelist workaround; **frame layout unverified** |

Host resolution: an IPv4 literal, or a name that appears in `/etc/hosts`. The
daemon is a static glibc binary on the vendor kernel and deliberately does not
use NSS.

`micom_template` syntax: hex bytes with optional `%R`, `%G`, `%B` placeholders
for the current frame's first-zone colour, space or no separator:

```
04 A2 08 %R %G %B 00 00 00 00 00
```

Only zone 0 feeds the template — a single factory lighting module is the
realistic case.

## 3. Audio tap (music reactive)

The daemon never opens ALSA itself. It reads **raw PCM: 16-bit little-endian,
mono, `--audio-rate` Hz (default 16000)**, from:

- `--audio-cmd CMD` — a capture process writing to stdout, or
- `--audio-file PATH` — a FIFO or a growing file.

Candidates on the unit (check what the firmware actually exposes):

```sh
cat /proc/asound/cards
cat /proc/asound/pcm
```

- a **loopback** device (`snd-aloop`) fed by the media player,
- the media sink's monitor via PulseAudio (`parec --device=<monitor>`), if the
  unit ships a PulseAudio server on the session bus,
- an **ALSA file/`arecord`** capture of the codec's output when a loopback
  cannot be configured.

Whichever you pick, verify it with `audio_level` in `GET /status` before
turning `reactive` on: a silent capture shows `level: 0.000`.

## 4. Analysis

16 kHz, 10 ms hop (160 samples). Per hop:

- RMS → smoothed envelope (attack 0.55 / release 0.08) → `level`,
- three RBJ band-pass filters (60 Hz / 450 Hz / 3000 Hz, Q 1.0) → `bass`,
  `mid`, `high` envelopes,
- beat detector: bass envelope above 1.45× its slow average and 150 ms
  refractory → `beats` counter and a decaying flash.

`gain` scales `level` (0–400 %). Effects react through `level` (brightness),
`high - mid` (hue shift) and the beat flash (comet/sparkle/pulse).

## 5. Output rate and safety

| Limit | Default | Why |
|---|---|---|
| render `fps` | 25 | one poll tick per frame is enough for lighting |
| output `output_hz` | 20 | limits LAN frames; WLED controllers are happy at 20 fps |
| unchanged frames | never re-sent | a static colour costs nothing after the first send |
| WLED connect timeout | 300 ms | a controller that vanished must not stall the UI |

`output_errors` and `last_error` in `/status` (and the app footer) are the
first thing to look at when nothing lights up.
