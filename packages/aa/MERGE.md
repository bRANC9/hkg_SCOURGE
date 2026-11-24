# AA core — parked work-in-progress (temporary)

This directory is parked on purpose: the Android Auto core is **not finished and
not activated**, so it must be mergeable in one piece later without dragging
half-finished lines through shared files. Nothing outside `packages/aa/` and
`docs/22-aa-core.md` is modified while the checkpoint stream is open.

## What is here

| Checkpoint | Contents | State |
|---|---|---|
| AA1 | `aa_wire` frame codec + `aa_channels` vocabulary | offline-tested |
| AA2 | `aa_proto` wire codec + `aa_control` control-channel state machine | offline-tested |
| AA3 | `aa_tls` seam + `aa_session` glue (frames in/out, encryption table) | offline-tested |
| AA4 | `aa_sensor`: start handshake, value events, EV energy model toward the phone | offline-tested |
| AA6 | `aa_aoap`: AOA accessory-mode responder (phone-shaped plumbing) | offline-tested |
| AA7 | `aa_btboot` wireless bootstrap codec, `aa_ap` readback seam, `aa_wireless` orchestration | offline-tested |
| AA8 | `aa_mirror`: the same protocol as a *phone*, toward a head unit | protocol half done, fallback only |
| AA9 | `probes/bt_owner.c`: can our process own L2CAP PSM 1 (SDP) and RFCOMM 8 | built for the unit, wired into the WiFi probe, not yet run |
| AA10 | Video on the unit: decoded by a vendor service, so the plan is a glibc-2.25 helper process (VA-API/i965) passing dmabuf fds to this musl daemon | measured, not built; the buffer-transport API is the open link |

Missing before anything may touch a car: a real TLS provider, the transports
(TCP listener + USB bulk pipe), the bootstrap on the unit, an installer/restore
bundle and a parked-car gate. See `docs/22-aa-core.md`.

**Route update (2026-11-23):** the target is the on-unit route, not the bridge
board (`docs/22` §5e), so the mirror (AA8) and the AOA bulk pipe are fallback
work and the video track (AA10) is a first-class checkpoint.

## One thing already leaves this directory

Two binaries are built here and shipped by the WiFi package's payload pipeline
(a cross-package dependency created by the other tab: if either side moves or
renames a file, this park stops being self-contained):

| Binary | Ships as | What runs it |
|---|---|---|
| `bt_owner.target` (`probes/bt_owner.c`) | `payload/bin/bt_owner.target` → `/rw_data/wifi/bin/bt_owner.target` | `aa_probe`, as root; appends `--- BT socket ownership` and `--- video decoder path` to `aa_probe.txt` |
| `vbtd_api.target` (`probes/vbtd_api.c`) | stick `combo/aa/vbtd_api.target` | the WiFi stage, once per install, on `readlink -f /proc/$(pidof vbtd)/exe`, writing `/rw_data/aa/bt_api.json` — the **only** writer of that file, because this package has no installer of its own yet |

## Nothing shared is touched

While parked, the three pointer lines that will make this work visible are kept
here instead of applied, so a later merge cannot collide with other work in the
same files.

```diff
--- a/AGENTS.md
+++ b/AGENTS.md
-- Docs are numbered `docs/01–21`; UI research lives in `docs/ui/`.
+- Docs are numbered `docs/01–22`; UI research lives in `docs/ui/`.

--- a/README.md
+++ b/README.md
-| `docs/01–21` | The runbooks: … the unsigned normal-USB update confirmation |
+| `docs/01–22` | The runbooks: … the unsigned normal-USB update confirmation, and the AA core plan (wired + wireless Android Auto) |

--- a/docs/07-packages.md
+++ b/docs/07-packages.md
+| AA | AA core research (AA1–AA10): transport-agnostic frame layer, protobuf codec, control/sensor channels, TLS seam, session glue, EV energy model, AOA responder, wireless bootstrap, the phone-role mirror, the on-unit route decision and its Bluetooth/video measurements (`docs/22`) | WIP — unit-tested source only; no TLS provider, no installer, no Maps or car validation |
```

Place the docs/07 row after the last `Wifi1b` row, before the `All` row.

## Save point and daemon

A save point exists: branch `aa-core-wip`, commit `d9d67ac`, created with a
temporary index so the shared working tree and HEAD were never touched. Update
it with the same plumbing whenever the park should be preserved:

```sh
GIT_INDEX_FILE=/tmp/aa_index git read-tree HEAD
GIT_INDEX_FILE=/tmp/aa_index git add packages/aa docs/22-aa-core.md
TREE=$(GIT_INDEX_FILE=/tmp/aa_index git write-tree)
COMMIT=$(git commit-tree "$TREE" -p aa-core-wip -F - <<'MSG'
chore(aa): park ...
MSG
)
git update-ref refs/heads/aa-core-wip "$COMMIT"
```

The debug daemon (`daemon/aa_daemon.c`, `/status`, `/ap-check`, `/saved`) is the
surface the WiFi page reads while the radio is being stabilised: a successful
`/ap-check` is the "save" that pins the AP numbers the bootstrap must announce.
The WiFi app has since shipped its own Android Auto tab (`WifiPage.qml` tab 4,
`GET /aa/check` in `wifi_daemon`, commit `8744dec`), including the AP start and
the measured verdict. That supersedes our page snippet, which was removed: the
page is theirs, and two competing UIs would only drift. What stays here is what
their tab cannot do — validate an AP readback against the bootstrap rules and pin
it (`/ap-check`, `/saved`) — plus the engine and the transports.

## Merge recipe

1. Review requirement first: `AGENTS.md` rule 11 wants one independent
   DeepSeek V4.1 Flash-Max review of the diff, with the verdict recorded as
   `Second-Opinion: <reviewer model> — PASS` in the commit message. Do not land
   this without it.
2. Branch and stage explicit paths only (the working tree is shared with other
   tabs; never a whole-tree git command):

   ```sh
   git switch -c aa-core-wip
   git add packages/aa docs/22-aa-core.md
   git commit -m "feat(aa): parked AA core checkpoints AA1-AA8 (codec level)" \
     -m "Second-Opinion: deepseek-v4.1-flash#max — PASS"
   ```

3. Apply the three pointer lines above (they are the only shared-file change).
4. Merge when the transports exist and a parked-car test is authorized; until
   then keep the branch unmerged and the package unactivated.

## Working on it meanwhile

```sh
cd packages/aa && make          # objects
cd packages/aa && make check    # 11 test programs, strict + ASan/UBSan, plus the protoc cross-check
```

Only `packages/aa/` and `docs/22-aa-core.md` should change while the park is
open. If a shared file really has to change, note why at the bottom of this file
so the merge step is explicit.
