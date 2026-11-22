# Java 25 runtime — Ioniq 5 Gen5W (260128)

A pinned, headless **Eclipse Temurin 25** runtime for JAR-based apps and
daemons on the head unit. It installs under `/rw_data/java` and is built as a
`jlink` image; no firmware, `/app`, `/etc`, updater or bootloader file is
touched.

**Status: WIP — VM-validated (vendor kernel and compat kernel); car-pending.**
See `docs/STATUS.md` for run IDs and exact evidence.

## What you get

| | |
|---|---|
| Upstream | Eclipse Temurin `25.0.5.1+2` (LTS), sha256-pinned |
| Runtime | `java`, `jcmd`, `jfr`, `jstat`, `jinfo`, `jmap`, `jstack`, `jps`, `keytool`, `jrunscript`, `rmiregistry` |
| Modules | `java.se` plus `jdk.httpserver`, `jdk.net`, `jdk.zipfs`, `jdk.localedata`, `jdk.charsets`, `jdk.crypto.ec/cryptoki`, `jdk.jfr`, `jdk.jcmd`, `jdk.management*`, `jdk.attach`, `jdk.jdwp.agent`, `jdk.security.*`, `jdk.naming.*`, `jdk.xml.dom`, `jdk.unsupported` |
| Not included | `javac`, AWT/Swing GUI support (the vendor rootfs has no X11 libs), JavaFX |
| Install path | `/rw_data/java/runtime-25.0.5.1+2`, `current` symlink |
| App JARs | `/rw_data/java-apps/` (created on install, preserved by restore) |
| Size | ~78 MiB, 250 files |

`jlink` output is materialized before packaging: the USB tree contains no
symlinks, so it copies cleanly to exFAT.

## Why this build is expected to work on the car

The head unit is x86-64 with kernel `4.1.27-abl` and **glibc 2.25**. The
pinned Temurin `libjvm.so` requires at most `GLIBC_2.17`; `libstdc++` is
statically linked into the JDK, so the vendor's GCC 5.2 runtime is not a
dependency. `scripts/check_runtime.py` enforces the GLIBC ceiling and checks
every non-GUI `NEEDED` library against `work/rootfs` when it is staged.

The AWT X11 libraries are deliberately absent from the vendor rootfs, so
`java.awt` works only in headless mode (ImageIO/buffered images). Do not
expect Swing or AWT windows.

## Build

```sh
sh packages/java/scripts/fetch_jdk.sh
sh packages/java/scripts/build_payload.sh
```

The tarball is fetched into gitignored `third_party/temurin/` and verified
against `config/third_party.lock`. For an offline/air-gapped build:

```sh
TEMURIN_TARBALL=/path/to/OpenJDK25U-jdk_x64_linux_hotspot_25.0.5.1_2.tar.gz \
  sh packages/java/scripts/build_payload.sh
```

To build a larger runtime (for example, with the compiler):

```sh
JAVA_EXTRA_MODULES=jdk.compiler,jdk.jartool sh packages/java/scripts/build_payload.sh
```

The default export is `packages/java/out/usb`, with `manifest.md5`,
`run_stuff.sh`, restore and diagnostics scripts.

## Install

Copy the whole `out/usb` tree to the root of a FAT/exFAT stick, plug it into
the car, and wait for the initial-root failsafe. The installer:

1. verifies `manifest.md5` before touching the car;
2. checks free space in `/rw_data`;
3. copies to a temporary runtime directory and verifies `SHA256SUMS`;
4. runs the copied `bin/java -version` on the car as a real smoke test;
5. switches `current`, writes `env.sh`, and prunes older runtimes;
6. applies the installer's SMACK label to `bin/java` when smackfs is
   available, with an explicit readback;
7. writes `status=OK|FAIL` to `/rw_data/java/result.txt` (and to the stick
   when it is writable).

Restore removes the runtime tree (`/rw_data/java`) and deliberately preserves
the app directory `/rw_data/java-apps`.

## Usage

```sh
/rw_data/java/current/bin/java -version
/rw_data/java/current/bin/java -jar /rw_data/java-apps/app.jar
. /rw_data/java/env.sh     # JAVA_HOME + PATH for the current shell
```

A systemd example is installed at
`/rw_data/java/current/examples/java-app@.service`; it is **not** installed
or enabled by the package. Copy it to `/etc/systemd/system`, put your JAR at
`/rw_data/java-apps/<name>.jar`, then use `java-app@<name>`. The installer
creates `/rw_data/java-apps`; restore preserves it.

The runtime runs with whatever privileges the caller has. Do not run
untrusted JARs as root.

## Validation

```sh
cd packages/java
make test        # host build/install/restore/prune/free-space checks
make vm-test     # Mode V + Mode C java-runtime scenario (VM images required)
```

The VM scenario installs through the real `run_stuff.sh`, then exercises
version, SHA-256/AES/EC, zipfs, charset/locale/timezone, headless ImageIO,
threads, files, `ProcessBuilder`, JMX, JFR, an HTTP daemon with a second JVM
client, SIGTERM shutdown, reinstall/prune and restore. `jcmd` attach is
recorded; it succeeds on both tested kernels.

## Files

```
scripts/fetch_jdk.sh        pinned download + sha256 + safe extract
scripts/build_payload.sh    jlink, materialize symlinks, manifest
scripts/check_runtime.py    GLIBC/NEEDED/symlink/size gate
scripts/run_stuff.sh        installer
scripts/run_stuff_restore.sh
scripts/run_stuff_diag.sh
config/modules.txt          jlink module list
artifacts/systemd/          example unit (not installed)
tests/                      host/installer/VM payload tests
```

## Non-goals

- No GUI/AWT/Swing or JavaFX support.
- No on-car compiler in the default build.
- No systemd unit install, sandbox, user management or app lifecycle manager.
- No firmware/updater/bootloader/partition changes.
