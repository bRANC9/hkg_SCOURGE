# Java 25 runtime — status

**Overall: WIP — VM-validated, car-pending.** No car deployment or car claim
has been made.

## 2026-11-22 — build and VM acceptance

| Gate | Result |
|---|---|
| Pinned upstream | Temurin `25.0.5.1+2`, sha256 `760f91b22c3efc9bd1b64ff950598231be82928cc807e30691a0e7a2995a62c0` |
| Runtime build | `jlink` headless SE, 78 MiB, 250 files |
| Static gate | max `GLIBC_2.17` <= target 2.25; no symlinks; the build gate run with the staged vendor rootfs reported `vendor_rootfs=yes`. A clean checkout without `work/rootfs` skips external `NEEDED` lookups; the VM run is the authoritative runtime check. |
| Host build test | `test_runtime.py` PASS |
| Host installer test | `test_installer.sh` PASS (install, idempotent, prune, free-space failure, restore) |
| Mode V — vendor `4.1.27-abl`, glibc 2.25 | **PASS**, run `1795311226052276579` |
| Mode C — pinned compat kernel | **PASS**, run `1795311288097083935` |
| Result schema | `check-run` OK for both runs |

### What the vendor-kernel run proved

The `java-runtime` scenario (`tools/vm/scenarios/java-runtime.json`) passed
all checks:

- `java-install` — real `run_stuff.sh` install from a USB payload image,
  manifest + SHA-256 verification, `current` symlink, installed `java`
- `java-version` — `openjdk version "25.0.5.1"`, `Temurin-25.0.5.1+2`
- `java-selftest` — SHA-256, AES-GCM, EC P-256, ISO-8859-15, German locale,
  Europe/Berlin DST, files/FileChannel.map, zipfs, jarfs, headless ImageIO,
  4-thread executor, `/bin/true` process, JMX, JFR
- `java-daemon` — `com.sun.net.httpserver` daemon + second JVM client +
  SIGTERM shutdown hook; `jcmd VM.version` attach succeeded (`JAVA_JCMD_OK`)
- `java-idempotent` — second install, one runtime retained, `current` kept
- `java-restore` — path guards rejected `/rw_data/`, `/rw_data/.`,
  `<runtime>/..` and runtime==apps; normal restore removed the runtime
  tree and preserved the app directory and its marker JAR
- `java-runtime` — final marker

The install copies and verifies ~80 MiB and then runs the installed
`java -version`; the whole vendor scenario completes well inside the 420 s
timeout, including boot.

## Not yet validated

- Car deployment. The package is **car-pending** until a real head unit
  install/run/restore is recorded.
- GUI/AWT/Swing: intentionally unsupported; the vendor rootfs has no X11
  libraries.
- Long-running production daemons under the car's systemd (the example unit
  is not installed or exercised as a unit).
- Other firmware/region/car versions. The package pins the 260128 EU target;
  other firmware may need a fresh compatibility run.
