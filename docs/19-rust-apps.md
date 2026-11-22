# 19 — Rust apps and daemons (host-built, static musl)

TARGET: Ioniq 5 Gen5W_L (Linux), firmware 260128 (EU).

**Status: WIP.** The host build/package path is offline-validated; the
sample daemon is VM-validated on the vendor kernel. There is no on-car
validation yet, and the systemd start path is not exercised by the research
boot. Do not present this as a finished app platform.

## The model

Rust is a **builder-host** toolchain, not a head-unit runtime:

- `rustc`, `cargo` and a pinned std live under gitignored
  `third_party/rust/` on the builder.
- Cargo resolves and vendors crates on the builder.
- The unit receives a **self-contained static-musl binary** plus a systemd
  unit through the normal SCOURGE USB installer (docs/06).
- No Rust toolchain, no Cargo, and no shared Rust runtime are installed on
  the unit.

This matches the existing static-daemon policy (docs/07) and avoids a large
compiler on `/rw_data`. It also keeps crate `build.rs` code off the car.

## Why static musl

Rust has no stable ABI. A shared `libstd-<hash>.so` is tied to one exact
Rust build, so shared-libstd apps would need per-version runtime packages,
ABI checks and resolution on the unit. Static musl makes each app one file
with no runtime dependency. It also survives firmware/glibc changes.

The default build is **Cargo release with symbols kept** (the owner chooses
debuggability over a smaller file). Measured with the pinned 1.100.0
toolchain:

| Binary | Size |
|---|---:|
| `scourge-statusd` (Tokio + serde_json + anyhow), release default | ~1.1 MiB |
| same, stripped (not the default) | ~0.85 MiB |
| static C `battery_daemon`, stripped | ~1.1 MiB |
| pinned toolchain (builder only) | ~825 MiB installed |

A handful of Rust daemons is a few MiB on the unit, not a storage problem.
The toolchain stays on the builder.

Shared **libstd** is not supported. A per-app **dynamic glibc** build is a
future option for an app that must link vendor shared libraries or use
`dlopen`; it needs a glibc 2.25 sysroot and a GLIBC symbol gate, and is not
part of this workflow.

## Toolchain pin

`config/third_party.lock` `[rust-toolchain]` pins Rust 1.100.0
(2026-11-12), its commit, the host/target triples and the sha256 of the
`rustc`, `cargo`, `rust-std` (gnu + musl) dist tarballs. A hash mismatch is
a hard stop. `scripts/00_fetch_rust.sh` installs the toolchain; it never
uses rustup and never writes into the repo outside gitignored
`third_party/`.

The official host toolchain targets glibc 2.17+ / kernel 3.2+; the unit is
glibc 2.25 / kernel 4.1.27. The musl target is static and self-contained.

## Quick start

```sh
# once per builder checkout (network; ~160 MiB download)
bash scripts/00_fetch_rust.sh

# build + package the sample
bash tools/rust/scripts/package_app.sh \
    tools/rust/examples/scourge-statusd --fetch \
    --out third_party/rust/out/scourge-statusd-usb
```

Copy `third_party/rust/out/scourge-statusd-usb/` to a FAT/exFAT stick. The
initial-root failsafe runs `run_stuff.sh` as root; `run_stuff_result.txt`
reports the result. Restore by copying `run_stuff_restore.sh` over
`run_stuff.sh`.

To start a new project:

```sh
cp -R tools/rust/template ~/src/my-daemon
# edit Cargo.toml, scourge-app.env, src/main.rs, service/my-daemon.service
bash tools/rust/scripts/package_app.sh ~/src/my-daemon --fetch \
    --out third_party/rust/out/my-daemon-usb
```

## What `package_app.sh` does

1. Reads `scourge-app.env` (`APP_NAME`, `APP_VERSION`, `APP_BIN`,
   `APP_SERVICE`, `APP_DATA_DIR`, description).
2. Builds with the pinned Cargo:
   `--release --locked --offline --target x86_64-unknown-linux-musl`,
   using the toolchain's `rust-lld`; host `cc` is used only for build
   scripts and proc macros.
3. Requires the result to have no `PT_INTERP` and no `DT_NEEDED`; a dynamic
   binary is rejected.
4. Emits:
   - `run_stuff.sh` / `run_stuff_restore.sh`
   - `payload/bin/<app>` (static)
   - `payload/systemd/<app>.service`
   - `payload/app.env`, `payload/app.sha256`
   - `payload/lib/install_io.sh`
   - `manifest.md5`
   - `README.txt`

The generic installer verifies the manifest before writing, copies with a
verified read, installs the unit, labels the binary with the installer's
SMACK exec label (readback gate), is idempotent, and preserves app data on
restore unless `APP_PURGE=1`.

## App manifest (`scourge-app.env`)

The package script sources a small shell-style key=value file from the
project root. Quote values that contain spaces.

| Field | Required | Meaning |
|---|---|---|
| `APP_NAME` | yes | lowercase `a-z`/`0-9`/`-`; package id and default data-dir name |
| `APP_VERSION` | yes | displayed and recorded; informational only (the binary hash is the identity) |
| `APP_BIN` | yes | binary file name in the Cargo target and `payload/bin/` |
| `APP_SERVICE` | yes | systemd unit name ending in `.service`; the file must be at `service/<APP_SERVICE>` |
| `APP_DATA_DIR` | yes | absolute path under `/rw_data/`; the installer writes and preserves it |
| `APP_DESCRIPTION` | no | one-line description used in the generated stick README |

The Cargo package name may differ from `APP_BIN`; the package script copies
the exact `target/x86_64-unknown-linux-musl/release/<APP_BIN>` file. The
unit's `ExecStart` should point at
`<APP_DATA_DIR>/bin/<APP_BIN>` and its `--status`/config paths should live
under `APP_DATA_DIR`.

## Installed layout and result

```
/rw_data/<app>/bin/<APP_BIN>        installed static binary
/rw_data/<app>/install.log          installer log
/rw_data/<app>/result.txt           copy of the stick result
/rw_data/<app>/backup/<stamp>/      previous unit file, if one was replaced
/etc/systemd/system/<APP_SERVICE>   installed unit
<USB>/backup_current/               copy of the previous unit
<USB>/run_stuff_result.txt          final result
```

Typical result on the stick:

```
status=OK
app=<APP_NAME>
version=<APP_VERSION>
action=installed|upgraded|up-to-date
binary=/rw_data/<app>/bin/<APP_BIN>
sha256=<binary sha256>
service=<APP_SERVICE>
start=started|already-running|skipped:no-systemd
smack_self=<label>
smack_check=ok
```

`action=up-to-date` means the binary hash and unit were already identical;
the installer still re-checks the SMACK exec label. `APP_PURGE=1` on restore
removes `APP_DATA_DIR`; otherwise app data, logs and unit backups are kept.

## Offline by default

`package_app.sh` builds with `--offline` from
`third_party/rust/vendor/<app>/`. `--fetch` is the explicit network step:
it runs `cargo vendor` and writes `Cargo.lock` if missing. Commit the
lockfile. Later builds need no network. The unit never fetches crates.

## Upgrades and rollback

Re-run `package_app.sh` with the same `APP_NAME`/`APP_BIN` and a new binary.
The installer compares the binary sha256 and unit, stops the service (when
systemd is present), replaces the binary, keeps the old unit in
`/rw_data/<app>/backup/<stamp>/`, and restarts. App data is not touched.

There is no automatic binary rollback. Keep the previous USB tree; restoring
it re-installs the old binary and unit, and preserves data. `APP_VERSION` is
recorded for humans but does not order upgrades; the sha256 is the identity.

## Limitations

- Pure-Rust crates are the tested path. A crate that compiles C needs a musl
  C compiler (`musl-gcc` or a container); not covered by the sample.
- No Rust GUI/Qt apps; this kit targets headless daemons and CLI tools.
- No on-unit `cargo`/`rustc` (an optional on-device toolchain was
  prototyped separately but is not shipped).
- Systemd is not running in the research VM boot; the VM test starts the
  daemon directly. The unit is installed/checked, but the on-car systemd
  start remains pending.
- App data is preserved on restore; a firmware update can still replace
  `/etc` and `/app`. Re-run the installer after a firmware update if needed.

## Security

- `build.rs` and proc macros run on the **builder**, with the builder user's
  privileges, not on the car. Build only crates you trust.
- The unit installer writes only the app directory and its own unit; it does
  not patch stock binaries or touch the updater/bootloader/partitions.
- The binary and manifest are hash-gated before install. SMACK exec labels
  are read back and a mismatch aborts.

## Troubleshooting

| Symptom | Action |
|---|---|
| `pinned Rust toolchain not found` | `bash scripts/00_fetch_rust.sh` (builder-only) |
| `no vendored crate cache` | run `package_app.sh ... --fetch` once; commit the crate's `Cargo.lock` |
| `Cargo.lock` changed / `--locked` failure | commit the lockfile generated by `--fetch`; do not carry an unpinned lock |
| `has a program interpreter` / `dynamic library dependencies` | the build is not static musl; use the template/target config and do not pass a glibc target |
| C build failure in a crate | a `-sys`/`cc` crate needs a musl C compiler; the sample is pure Rust |
| `APP_DATA_DIR must be under /rw_data/` | move the app data under `/rw_data/`; do not write into `/etc` or `/app` |
| `manifest verification failed` | re-copy the whole USB tree; do not edit `payload/` after building |
| `exec label readback failed` / `cannot read back SMACK64EXEC` | the installer is not running under a usable SMACK label, or `chsmack`/`getfattr` is missing; install through the normal USB failsafe |
| `service start failed` | read `/rw_data/<app>/install.log` and `systemctl status <APP_SERVICE>`; verify `ExecStart` uses the installed binary |
| `start=skipped:no-systemd` | expected in the VM research boot; on the car systemd should start the unit |
| Installer says `up-to-date` but the daemon is down | `systemctl status <APP_SERVICE>`; the installer restarts an inactive unit when systemd is present |
| After a firmware update the app is gone | firmware can replace `/etc` and `/app`; re-run the stick. App data under `/rw_data` is preserved. |
| Restore kept the app directory | expected; use `APP_PURGE=1` only when you also want to delete app data and logs |

## VM validation

The `rust-app` scenario (Mode V, vendor kernel 4.1.27) attaches the
generated USB tree as `--payload` and checks:

- install (`status=OK`, SMACK readback), static linking (`ldd`)
- daemon start, HTTP `/status`, no libc mapping
- SMACK process/exec labels
- SIGTERM clean shutdown and status mirror
- idempotent reinstall (`action=up-to-date`)
- restore removes the binary/unit and preserves app data

Run:

```sh
cd tools/vm
python3 -B launch.py run --scenario rust-app \
    --payload ../../third_party/rust/out/scourge-statusd-usb \
    --timeout 300 --memory 1024 --smp 1 --label rust-app
```

The result is recorded in `tools/rust/docs/FINDINGS.md`. **2026-11-20:
PASS** on the vendor kernel at 1 vCPU / 1 GiB (all eight checks), with the
sample daemon started directly because the research boot has no systemd.
This is VM evidence only, not a car claim.
