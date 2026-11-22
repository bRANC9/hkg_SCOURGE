# 21 — Java 25 runtime (headless, JAR apps/daemons)

TARGET: Ioniq 5 Gen5W_L (Linux), firmware 260128 (EU).

**Status: WIP.** The runtime package is VM-validated on the vendor kernel
and the compat kernel; there is no on-car validation yet. The default
runtime is headless: the vendor rootfs has no X11 libraries, so AWT/Swing
GUI applications are unsupported. Do not present this as a finished app
platform.

## The model

Java is a **head-unit runtime**, unlike the host-built Rust workflow in
docs/19:

- a pinned Eclipse Temurin 25 JDK is fetched on the builder, verified
  against `config/third_party.lock`, and reduced with `jlink`;
- the unit receives a self-contained headless runtime under `/rw_data/java`;
- user JARs live in `/rw_data/java-apps`, outside the runtime tree;
- the package installs no `javac`, no systemd unit, no QML, no `/app` or
  `/etc` files, and no updater/bootloader content.

The runtime is deliberately one directory per version:

```text
/rw_data/java/
  runtime-25.0.5.1+2/
    bin/java, bin/jcmd, ...
    lib/modules, lib/server/libjvm.so, ...
    legal/, NOTICE, release
    SHA256SUMS, runtime.version, SCOURGE-RUNTIME.txt
    examples/java-app@.service
  current -> runtime-25.0.5.1+2
  env.sh
  .scourge-java                  # restore ownership marker
/rw_data/java-apps/              # user JARs; preserved by restore
```

## Why this build works on glibc 2.25

The head unit is x86-64 with kernel `4.1.27-abl` and **glibc 2.25**. The
pinned Temurin 25.0.5.1+2 `libjvm.so` requires at most `GLIBC_2.17`, and
the JDK has no runtime `libstdc++` dependency. The packaged runtime is
~78 MiB / 250 files, has no symlinks (exFAT-safe), and reports:

```text
openjdk version "25.0.5.1" 2026-11-17 LTS
OpenJDK Runtime Environment Temurin-25.0.5.1+2
OpenJDK 64-Bit Server VM Temurin-25.0.5.1+2
```

The static gate (`packages/java/scripts/check_runtime.py`) enforces the
GLIBC ceiling, rejects symlinks, checks non-GUI `NEEDED` libraries against
a staged `work/rootfs`, and verifies the runtime size. The vendor-kernel VM
is the authoritative runtime check.

## Build

```sh
sh packages/java/scripts/build_payload.sh
# -> packages/java/out/usb
```

The pinned tarball is fetched into gitignored `third_party/temurin/`. For an
offline/air-gapped build:

```sh
TEMURIN_TARBALL=/path/to/OpenJDK25U-jdk_x64_linux_hotspot_25.0.5.1_2.tar.gz \
  sh packages/java/scripts/build_payload.sh
```

The default module set is in `packages/java/config/modules.txt`
(`java.se`, HTTP server, zipfs, charsets/locales, crypto, JMX, attach,
JFR, remote debugging). To build a larger runtime:

```sh
JAVA_EXTRA_MODULES=jdk.compiler,jdk.jartool \
  sh packages/java/scripts/build_payload.sh
```

## Install

Copy `packages/java/out/usb` to the root of a FAT/exFAT stick and plug it
in. The initial-root failsafe runs `run_stuff.sh` within ~8 s. The installer:

1. creates `/rw_data/java` and `/rw_data/java-apps`, logs the attempt, and
   verifies the stick's `manifest.md5` before installing runtime files;
2. cleans interrupted temp copies, checks free space, then writes the
   `.scourge-java` claim marker;
3. copies to a temporary runtime directory, hardens modes, verifies
   `SHA256SUMS`, and runs the copied `bin/java -version`;
4. switches `current`, writes `env.sh`, and prunes older runtimes;
5. applies the installer's own SMACK label (normally `_` in the USB flow)
   to `bin/java` when smackfs is available, with a readback;
6. writes `status=OK|FAIL` and paths/SMACK details to
   `/rw_data/java/result.txt` (and to the stick when writable).

`JAVA_RW_DIR` and `JAVA_APPS_DIR` are canonicalized with `readlink -f`
before any check. The installer refuses `/`, `/rw_data`, and any runtime/
app path overlap; restore additionally requires the `.scourge-java` marker
with the exact `SCOURGE_JAVA_RUNTIME` content.

## Use

```sh
/rw_data/java/current/bin/java -version
/rw_data/java/current/bin/java -jar /rw_data/java-apps/app.jar
. /rw_data/java/env.sh     # JAVA_HOME and PATH for this shell
```

A systemd example is installed at
`/rw_data/java/current/examples/java-app@.service`, but it is not installed
or enabled. To use it:

```sh
cp /rw_data/java/current/examples/java-app@.service /etc/systemd/system/
# put the JAR at /rw_data/java-apps/<name>.jar
systemctl daemon-reload
systemctl enable --now java-app@<name>
```

The example uses `-XX:+UseSerialGC -Xms16m -Xmx128m`; adjust the heap for
the daemon. If the daemon needs the head-unit SMACK floor label, the
installer applies its own SMACK label (normally `_` in the USB flow) to
`bin/java` when smackfs is available; verify the live process label with
`/proc/<pid>/attr/current`.

Java runs with the privileges of its launcher. Do not run untrusted JARs as
root. The package does not sandbox, create users, or supervise app
processes.

## Restore

Copy `run_stuff_restore.sh` over the loader filename on the stick and plug
it in. It removes the runtime tree (and only that), preserves
`/rw_data/java-apps`, and refuses `/`, `/rw_data`, unmarked directories, and
runtime/app path overlap.

## VM validation

`tools/vm/scenarios/java-runtime.json` installs through the real
`run_stuff.sh`, then checks:

- `java-install` — manifest, SHA-256 copy verification, marker, app
  directory, `current`, installed `java`
- `java-version` — Temurin 25.0.5.1+2
- `java-selftest` — SHA-256, AES-GCM, EC P-256, zipfs/jarfs,
  charset/locale/timezone, headless ImageIO, threads, files,
  `ProcessBuilder`, JMX, JFR
- `java-daemon` — HTTP daemon + second-JVM client + SIGTERM shutdown hook
- `java-idempotent` — reinstall, one runtime, app JAR preserved
- `java-restore` — path guards, runtime removed, app JAR preserved
- `java-runtime` — final marker

`jcmd` attach is recorded (`JAVA_JCMD_OK` on both tested kernels). The
probe also exercises `/rw_data/`, `/rw_data/.`, `<runtime>/..`, and
runtime==apps guards before the normal install/restore.

```sh
sh packages/java/tests/build_vm_payload.sh
python3 -B tools/vm/launch.py run --profile research \
  --scenario java-runtime --payload packages/java/out/vm --timeout 420
python3 -B tools/vm/launch.py run --kernel compat --profile research \
  --scenario java-runtime --payload packages/java/out/vm --timeout 420
```

Current evidence:

| Mode | Result | Run |
|---|---|---|
| Mode V — vendor `4.1.27-abl`, glibc 2.25 | **PASS**, 7/7 checks | `1795311226052276579` |
| Mode C — pinned compat kernel | **PASS**, 7/7 checks | `1795311288097083935` |

See `packages/java/docs/STATUS.md` for the full dated record and
`packages/java/README.md` for package-level usage.

## Non-goals

- GUI/AWT/Swing/JavaFX support. The vendor rootfs has no X11 libraries;
  headless AWT/ImageIO is supported.
- `javac` or a compiler toolchain on the car in the default build.
- Installing or enabling a systemd unit automatically.
- Sandboxing, privilege separation, or user management for Java apps.
- Firmware, updater, bootloader, or partition changes.
