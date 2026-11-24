# Ambience — ambient lighting for the Ioniq 5 Gen5W

An app for the SCOURGE Hub: it takes over ambient lighting control with
effects, transitions and an optional music-reactive mode.

```sh
make -C src                       # -> src/ambient_daemon (static, needs -lm)
python3 tests/test_daemon.py      # host API/engine tests, no car needed
python3 tests/test_qml_contract.py  # overlay rules + injection snippets
sh tests/test_installer.sh        # installer + injection round-trip
sh scripts/build_payload.sh packages/ambient/out/usb
sh scripts/package_release.sh 1.0.0   # hub release: tarball + sidecar in dist/
```

What it does, one line each:

- **effects** — solid, breathe, pulse, rainbow, wave, comet, sparkle, cycle;
- **transitions** — crossfade, fade through black, cut, per change;
- **music reactive** — reads raw S16LE audio (a capture process or a FIFO) and
  modulates brightness/hue on the level and the beat;
- **outputs** — `none`, `file` (dry-run), `wled` (aftermarket controller on the
  LAN), `micom` (raw frame through the vendor D-Bus, explicit template only);
- **UI** — its own tile and a full-screen overlay with the Gen5W overlay rules.

Status: **WIP — host-validated only, never run on a car.** The factory ambient
frame layout is not known, so no default micom template ships. See `SPEC.md`
for the contract, `docs/HARDWARE.md` for the buses, the audio tap and the
backend configuration, and `docs/README_STICK.txt` for the install steps.

Layout:

```
src/ambient_daemon.c            static daemon, 127.0.0.1:8094
artifacts/qml/…                  AmbientPage.qml, AmbientTile.qml, AmbientSlider.qml
artifacts/assets/ambient/…       generated tile icons (scripts/make_icons.py)
artifacts/systemd/…              ambient-daemon.service
scripts/                         payload builder, installer, restore, QML injector
tests/                           host daemon + installer tests
```
