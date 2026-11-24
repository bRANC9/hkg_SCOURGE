# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
Ambience — ambient lighting for Ioniq 5 Gen5W (firmware 260128)
===============================================================

This stick installs an "Ambience" app tile, a full-screen control page and a
background daemon that drives ambient lighting: effects, transitions and an
optional music-reactive mode.

Install
-------
1. Make sure the stick contains, at its root:
     run_stuff.sh
     payload/
     manifest.md5
     README.txt
2. Plug the stick into the head unit USB port.
3. Wait about 15 seconds. The initial-root failsafe runs run_stuff.sh as root.
4. Remove the stick and read run_stuff_result.txt from its root.
   Expected: status=OK, inject=tile+page, daemon=systemd-ok,
   smack_self=_, daemon_smack=_, smack_check=ok, restart=dbus-ok.
5. Return to the home screen and tap the "Ambience" tile.

Restore
-------
1. Copy run_stuff_restore.sh over run_stuff.sh on the stick.
2. Plug the stick in and wait for run_stuff_result.txt.
   The tile/page injection is reverted from the injector's backup, the app
   files are removed and the service is disabled. config.conf and presets.conf
   stay under /rw_data/ambient, so a reinstall keeps your settings.

First start
-----------
Nothing is lit until you pick an output. The daemon defaults to the "file"
backend, which only writes computed frames to /rw_data/ambient/frames.jsonl —
handy for checking that effects run without touching a lamp.

  # an aftermarket RGB(W) controller on the LAN (WLED-compatible JSON API)
  wget -q -O- "http://127.0.0.1:8094/config?backend=wled&target=192.0.2.10"

  # the factory module through the micom (WIP: template is car-specific)
  wget -q -O- "http://127.0.0.1:8094/config?backend=micom"
  # then set micom_template in /rw_data/ambient/config.conf, e.g.
  #   micom_template=04 A2 08 %R %G %B 00 00 00 00 00
  # and restart ambient-daemon.service.  No default template ships on purpose:
  # an unknown module must not be poked by accident.

Music reactive needs an audio source. Configure one of:
  audio_cmd=arecord -q -D hw:Loopback,1 -f S16_LE -r 16000 -c 1 -t raw
  audio_file=/path/to/raw-s16le-mono          (FIFO or growing file)
Both are 16 kHz mono S16LE unless --audio-rate says otherwise.

Notes
-----
- Nothing is flashed. The updater, bootloader and partitions are untouched.
- The daemon listens only on 127.0.0.1:8094 and writes /rw_data/ambient/.
- The daemon and its dbus helpers carry the installer's own SMACK label
  (normally the floor label "_"). The install aborts if that label cannot be
  applied and read back.
- The micom backend only ever sends the explicit template you configure, via
  /rw_data/ambient/bin/micom-send (a copy of the vendor dbus-send installed as
  CANService, the name the vendor whitelist accepts).
- The app is host-tested against the daemon's HTTP API and the payload
  installer. It has not been validated on a car yet.
