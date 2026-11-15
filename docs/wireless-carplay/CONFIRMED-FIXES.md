# Confirmed review fixes — isolated follow-up

## Resume point

Branch: `wireless-carplay-confirmed-fixes`, based on `c850b4e`.

This worktree isolates follow-up fixes from concurrent changes in the original
checkout. Existing commits, including `2b9226f` and `0d6b84b`, are preserved.
Their review trailers do not establish closure of the findings listed below.
No push, merge, deployment, OEM execution or vehicle validation is authorized
by this checkpoint.

The earlier C1 protocol checkpoint is `6e4d7f1`. Reviewed follow-up commits:

- `6ffd3e7`: SPP build-output cleanup and fault tests.
- `dd970f3`: passive receiver pin verification and bounded test execution.
- `cc18b9d`: authentication initialization, RPC errors and native build cleanup.
- `8287120`: bounded session receive bursts and generation-bound offer holds.
- `3815f5f`: bounded/deferred EAKs and refreshed retransmission ACKs.

Each has one independent Flash-Max PASS covering its committed source/tests.
Runtime integration remains WIP; source and mock tests do not establish
working wireless CarPlay or physical USB coexistence.

## Required fixes and verification

| Area | Confirmed findings | Current follow-up |
|---|---|---|
| SPP build output | Temporary-output cleanup could omit remaining-artifact errors or remove replacement files | Independent Flash-Max PASS, reviewed tree `34fe7701`; creation-error, persistent-denial, replacement-preservation and descriptor-cleanup regressions pass; build-only, no activation |
| Passive receiver pins | Leaf symlinks/nonregular inputs, growing-file read bound and test hang risk | Independent PASS; committed `dd970f3`; offline only |
| Installer cleanup | Current-state archive authorization and unreadable process evidence | Candidate fixture tests pass; overall installer review still FAIL, not committed |
| Installer ownership | Incomplete destination-ancestor checks; foreign service runtime preservation | Fixing latest review findings; release blocked |
| Installer lifecycle | Restore mode preservation, current masks, cleanup failure reporting and bare-parent archive path rejection | Fixing latest review findings; not committed |
| Session stream | Coalesced and reorder-completion bursts could overflow deferred storage | Independent PASS; committed `8287120`; combined link integration tests pass |
| Session lifecycle | Duplicate starts, hidden/new USB identities, zero-clock policy, inactive readback and changed-AP hold invalidation | Independent PASS; committed `8287120`; trusted receiver evidence still required |
| Link output | EAK backpressure/capacity/truncated follow-up and stale retransmission ACKs | Independent PASS; committed `3815f5f`; unit/sanitizer validation only |
| OEM authentication | Initialization failure bypass and hidden earlier RPC errors | Independent PASS; committed `cc18b9d`; authentication activation blocked |
| Authentication hardening | Loading/output gates, caller deadline, rollback reporting and production-build checks | Independent PASS; committed `cc18b9d`; OEM/chip calls are not bounded by the caller timeout |
| DHCP/network | Pending ownership, mode-client worker supersession/shutdown, explicit router requirement and route rollback evidence | Latest review FAIL; fixing lifecycle findings; not committed |

Each milestone requires one independent DeepSeek 4.1 Flash-Max PASS covering
the exact commit contents. A green old suite or an earlier commit trailer is
not a substitute. Keep unrelated milestone changes out of each commit.

## Tests repeated in this worktree

- `python3 packages/wifi1b/tests/test_carplay_spp_patch.py`: PASS, including
  persistent cleanup-denial and replacement-file regressions, normal and
  ASan/UBSan hook tests, and read-only pinned firmware patch verification.
- `python3 packages/wifi1b/tests/test_carplay_spp_socket.py`: PASS, normal and
  ASan/UBSan first-party fixtures. No live Bluetooth connection was tested.
- OEM auth and async auth Python fixture suites: PASS after verifying the
  exact six-file auth review manifest.
- Link, bootstrap and session suites: PASS together against `3815f5f` link
  and `8287120` bootstrap/session, normal and ASan/UBSan; bootstrap oracle
  passes. This closes the frozen-link integration condition for those bytes.
- `python3 scripts/policy_check.py tests`: PASS after the auth checkpoint;
  this is the VM unit-test gate, not a booted-vehicle test.

Network and installer fixture suites have passed candidate revisions, but
independent review found additional defects. Those green suites are not
closure. The source-only release test reports SKIP without generated release
artifacts. A coherent rebuild and release-artifact checks remain pending;
existing releases have not been replaced or published.

Actual iPhone interoperability, real MFi operations, trusted receiver
attestation/observation, target loader/dependencies and USB coexistence remain
unverified. Preserve GB, owner credentials and all three SSH keys, modem
routing, wired USB, Battery/Tweaks/Reboot and shared micom behavior. The V20
deployable layout remains the two-file wrapper/archive pair.

Asynchronous authentication completion wiring and the full runtime broker
remain incomplete. Do not activate the synchronous session auth path as a
real-time link solution. No push, deployment or hardware validation has been
performed for these follow-up commits.
