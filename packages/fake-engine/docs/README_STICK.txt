# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
Fake engine sound (parked)
--------------------------
Install: put run_stuff.sh + payload/ + manifest.md5 at the stick root, plug in.
Control:  curl -X POST http://127.0.0.1:8095/sound?action=start
          curl -X POST http://127.0.0.1:8095/sound?action=stop
          curl -X POST http://127.0.0.1:8095/sound?action=volume&value=80
          curl          http://127.0.0.1:8095/status
Restore: copy run_stuff_restore.sh over run_stuff.sh and plug in.
The asset is synthesised by scripts/make_engine.py (crossplane V8-style idle,
our own audio, no sample library).  Playback uses aplay on plughw:0,0 - the
path the installer beep proves works.  The external AVAS module is untouched.
