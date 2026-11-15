# Confirmed review fixes — isolated follow-up

## Resume point

Branch: `wireless-carplay-confirmed-fixes`, based on `c850b4e`.

This worktree isolates follow-up fixes from concurrent changes in the original
checkout. Existing commits, including `2b9226f` and `0d6b84b`, are preserved.
Their review trailers do not establish closure of the findings listed below.
No push, merge, deployment, OEM execution or vehicle validation is authorized
by this checkpoint.

The reviewed C1 protocol checkpoint is `6e4d7f1`. Runtime integration remains
WIP, not working wireless CarPlay merely because source and mock tests exist.

## Required fixes and verification

| Area | Confirmed findings | Current follow-up |
|---|---|---|
| SPP build output | Temporary-output cleanup could omit remaining-artifact errors or remove replacement files | Independent Flash-Max PASS, reviewed tree `34fe7701`; creation-error, persistent-denial, replacement-preservation and descriptor-cleanup regressions pass; build-only, no activation |
| Installer cleanup | Stale/empty backup could authorize deletion of current credentials; incomplete live-process, asset and unreadable-evidence gates | Fixing; release blocked |
| Installer ownership | Unknown output-helper deletion, weak unit ownership, symlinked product directories | Fixing; release blocked |
| Installer lifecycle | Direct-payload cleanup order/proof, masked/active service restoration and stale failure results | Fixing and extending tests |
| Session stream | Coalesced valid CSM burst could overflow deferred receive storage before processing | Fixing; runtime activation blocked |
| Session lifecycle | Active receiver could receive duplicate start after offer expiry; zero-clock policy and inactive readback transitions | Fixing and extending tests |
| OEM authentication | Failed initialization could be bypassed on later requests; RPC test client could hide an earlier error | Fixing; authentication activation blocked |
| Authentication hardening | Helper loading/output paths, timeout accounting, cleanup and production-build checks | Fixing; hardware coexistence still requires target validation |
| DHCP/network | Renewal/rebind and route changes need complete retesting; reply padding, INFORM and pool bounds need verification | Fixing/testing before independent review |

Each milestone requires one independent DeepSeek 4.1 Flash-Max PASS covering
the exact commit contents. A green old suite or an earlier commit trailer is
not a substitute. Keep unrelated milestone changes out of each commit.

## Tests repeated in this worktree

- `python3 packages/wifi1b/tests/test_carplay_spp_patch.py`: PASS, including
  persistent cleanup-denial and replacement-file regressions, normal and
  ASan/UBSan hook tests, and read-only pinned firmware patch verification.
- `python3 packages/wifi1b/tests/test_carplay_spp_socket.py`: PASS, normal and
  ASan/UBSan first-party fixtures. No live Bluetooth connection was tested.

Actual iPhone interoperability, real MFi operations, trusted receiver
attestation/observation, target loader/dependencies and USB coexistence remain
unverified. Preserve GB, owner credentials and all three SSH keys, modem
routing, wired USB, Battery/Tweaks/Reboot and shared micom behavior. The V20
deployable layout remains the two-file wrapper/archive pair.
