# SPEC — Java 25 runtime

## 1. Target

| Item | Value |
|---|---|
| Vehicle | Hyundai Ioniq 5 Gen5W_L |
| Firmware | `NE_E.EUR.S5W_L.001.001.260128` |
| Platform | x86-64, kernel `4.1.27-abl`, glibc 2.25 |
| Deployment | `initial_root` USB failsafe (docs/06) |
| Runtime path | `/rw_data/java/runtime-<version>`, `current` symlink |
| Upstream | Eclipse Temurin `25.0.5.1+2`, sha256 `760f91b2…a62c0` |

## 2. Goals

1. Provide a working Java 25 runtime for JAR applications and systemd
   daemons on the stock rootfs.
2. Keep the install entirely under `/rw_data`; no `/app`, `/etc`, QML,
   updater or bootloader changes.
3. Preserve the repo's fetch-not-vendor rule: the JDK tarball is pinned,
   downloaded into gitignored `third_party/`, and never committed.
4. Fail closed on version, sha256, GLIBC ceiling, missing libraries, copy
   corruption and free-space exhaustion.
5. Make the USB payload exFAT-safe (no symlinks) and idempotent.
6. Prove the runtime on the exact vendor kernel and glibc, not just on the
   build host.

## 3. Non-goals

- GUI/AWT/Swing/JavaFX support. The vendor rootfs lacks X11 libraries.
- `javac`/JDK tooling in the default build. `JAVA_EXTRA_MODULES` can build a
  larger variant, but it is not the shipped default.
- Installing or enabling a systemd unit. An example is provided for the
  owner to adapt.
- Sandboxing, privilege separation or user management for Java apps.
- Any firmware, updater, bootloader or partition modification.

## 4. Architecture

```text
config/third_party.lock
  [temurin-jdk25] url + sha256 + version
        |
        v
scripts/fetch_jdk.sh  ->  third_party/temurin/jdk-25.0.5.1+2
        |
        v
scripts/build_payload.sh
  jlink --add-modules <config/modules.txt>
  cp -RLp  (materialize legal/ symlinks for exFAT)
  check_runtime.py  (GLIBC <= 2.25, NEEDED, no symlinks, size)
  SHA256SUMS + manifest.md5
        |
        v
out/usb/{payload,run_stuff.sh,run_stuff_restore.sh,run_stuff_diag.sh,README.txt}
        |
        v
car: run_stuff.sh -> /rw_data/java/runtime-<version>
```

The runtime is deliberately one directory per version. A reinstall writes a
temporary directory, verifies it, switches `current`, then prunes older
runtime directories. Application JARs live outside `/rw_data/java`, in
`/rw_data/java-apps`, so restore cannot delete them.

## 5. Install layout

```text
/rw_data/java/
  runtime-25.0.5.1+2/
    bin/java, bin/jcmd, ...
    lib/modules, lib/server/libjvm.so, ...
    legal/, NOTICE
    SHA256SUMS, release, runtime.version, SCOURGE-RUNTIME.txt
    examples/java-app@.service
  current -> runtime-25.0.5.1+2
  env.sh
  result.txt
  install.log
  .scourge-java                # restore ownership marker
/rw_data/java-apps/            # user JARs; preserved by restore
```

`env.sh` sets `JAVA_HOME=/rw_data/java/current` and prepends its `bin`.
`JAVA_APPS_DIR` overrides the app directory for testing; the default is
`/rw_data/java-apps`.

## 6. Compatibility gates

- **Version**: tarball sha256 from `config/third_party.lock`; `release`
  `JAVA_VERSION` starts with `25.`; VM `java -version` identifies
  `Temurin-25.0.5.1+2`.
- **GLIBC**: every ELF's maximum symbol version must be <= 2.25. The pinned
  build tops out at 2.17.
- **NEEDED**: resolve against the runtime and, when staged, `work/rootfs`.
  X11-only libraries are reported as expected GUI gaps, not treated as
  runtime failures.
- **Copy**: `SHA256SUMS` is verified after the copy to `/rw_data`.
- **Smoke**: the copied `bin/java -version` must run before `current` is
  switched.
- **Space**: require the payload size plus 16 MiB free in `/rw_data`.
- **exFAT**: reject symlinks in the runtime tree; `cp -RLp` materializes the
  jlink `legal/` links.

## 7. SMACK

The installer reads its own label from `/proc/self/attr/current`, applies it
to `current/bin/java` with `chsmack -e`, reads `security.SMACK64EXEC` back,
and records `smack=` in the result. If smackfs or `chsmack` is unavailable it
warns rather than blocking the runtime; a daemon that needs the `_` floor
label should be checked after install. It does not use
`SmackProcessLabel=` (unusable on this kernel).

## 8. Package rules

- `manifest.md5` covers `payload/`; the loader md5-gates `run_stuff.sh`.
- The installer writes `status=OK|FAIL` plus version/paths/SMACK details.
- Restore removes only `JAVA_RW_DIR` (default `/rw_data/java`), requires the
  `.scourge-java` marker, refuses `/`, `/rw_data` and unmarked directories,
  and preserves `JAVA_APPS_DIR` (default `/rw_data/java-apps`).
- Diagnostics are read-only and write `java_diag.txt` to the stick root.
- No third-party bytes are committed; `out/`, `third_party/` and VM images
  are gitignored.

## 9. Evidence

See `docs/STATUS.md`. The acceptance scenario is
`tools/vm/scenarios/java-runtime.json`; it must pass in Mode V (vendor
kernel) before any car claim. Mode C is supporting evidence only.
