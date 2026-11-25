# Host tests

    python3 -m unittest discover -s tests -v

What is covered (the layer that repeatedly broke on the way to the car):

- ShellSyntax: a stage or helper that does not parse under /bin/sh.
- EarlyVariableUse: a variable used in the top-level prologue before it is
  assigned - the set -u trap that leaves status=missing with no log line
  (both real failures on 2026-11-23 were of this shape).
- StagedBundle: a corrupted stage.sh on the medium (baked digest table vs the
  actual bytes), a stage that never writes run_stuff_result.txt, unmanifested
  helpers.
- QmlInjector: the injector on a base file carrying marker comments - anchor,
  append, revert, dry-run, and fail-closed on a missing anchor.
- Collectors: boot_profile.sh and perf_snapshot.sh producing their txt/csv.
- HelloDump: payload/dump_hello.sample.sh -> dump -> install_hello_base.py. The
  payload copies exactly the base files the WiFi builder reads (with a manifest
  the importer verifies), a unit without the Hello tile reports verdict=absent
  instead of a fake base, a damaged dump is refused, and build_artifacts._have()
  accepts a dumped base while it skips a SYNTHESIZED one.

Run it after every packages/compose.sh build and BEFORE
scripts/24_sync_combo_stick.sh: the tests read out/firstround, so a fresh build
is what they check.
