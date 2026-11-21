# Store (Marketplace agent) — SPEC

| | |
|---|---|
| Vehicle | Hyundai Ioniq 5 Gen5W |
| Firmware | `NE_E.EUR.S5W_L.001.001.260128` |
| Component | `/rw_data/store/store_daemon` (static x86-64), service `store-daemon.service` |
| Interface | `127.0.0.1:8092` (localhost only) |
| State | `/rw_data/store/` (`config.conf`, `device.json`, `index.json`, `job.json`, `ledger.json`) |
| Hub | the SCOURGE Hub (`bRANC9/scourge-hub`) |

## 1. Purpose

The logic behind the **Marketplace tab**: the UI only calls these endpoints and
renders the answers. Nothing in this package draws anything.

The device **pulls**; the driver confirms on the unit; every artifact is
sha256-verified before anything executes; a package that would overwrite a file
another installed package owns is refused by the hub (HTTP 409) and the daemon
surfaces that verbatim.

## 2. Endpoints

| Method | Path | Purpose |
|---|---|---|
| GET | `/status` | build id, key (firmware/region/gui), hub, paired, job, ledger |
| GET | `/health` | liveness |
| POST | `/config?hub=URL[&allow_install=0\|1]` | set the hub / arm installs |
| POST | `/pair?code=NNNNNN` or `?token=...` | pair via the hub (6-digit code or QR token) |
| POST | `/refresh` | `register` (anonymous) + fetch the signed index for our key |
| GET | `/catalog` | the cached index |
| POST | `/install?pkg=slug` | start the install job (forked, non-blocking) |
| GET | `/job` | `starting`/`downloading`/`verifying`/`extracting`/`installing`/`installed`/`staged`/`failed` + percent + detail |
| GET | `/local` | packages found on a mounted USB stick (`<mount>/scourge-packages`) |
| POST | `/install-local?file=<name>.tar.gz` | install one of those, offline |
| POST | `/self-update` | install the `marketplace` package from the catalogue (see §4) |
| GET | `/logs` | installer output (`run_stuff.sh`) |

## 3. Install job

1. entry from the cached index (`pkg`), `sha256` + `url` required;
2. download to `stage/<pkg>/artifact.bin`;
3. `sha256sum` compare - mismatch deletes the file and fails the job;
4. `tar -xzf` into `stage/<pkg>/`;
5. `installer_sha256` (when present) must match the extracted `run_stuff.sh`;
6. `run_stuff.sh` runs with `USB_DIR` pointing at the stage, output to `install.log`;
7. ledger entry + `POST /api/v1/device/report` (best effort, `Device` token).

`allow_install=0` stops after step 4 and reports `staged`: the payload is on the
unit but nothing was executed. **Default is 0** - the installer or `/config`
must arm it.

## 4. Self-update

The app publishes itself in the catalogue under the slug **`marketplace`** (the
release tarball from `scripts/package_app.sh`), so updating it is the same job
with three differences:

1. `/status` compares the catalogue version with the compiled `STORE_VERSION`
   (`self_update: true`, `self_version: "<offered>"`); `/self-update` starts the
   job for the `marketplace` slug and answers `{"started": false, "already at …"}`
   when no newer build is offered.
2. The release's own `run_stuff.sh` does the swap: it removes the old binary
   before copying (the running process keeps the old inode alive), replaces the
   QML page/tile and icons, clears the QML caches and restarts
   `store-daemon.service` - the unit's `Restart=on-failure` covers a failed boot.
3. The install report cannot be sent after the restart, so the daemon writes
   `report.pending.json` *before* running the installer and flushes it on the
   next start (`flush_pending_report()`); `/status`/`/job` show `installed`.

`allow_install=0` still applies: a self-update is staged, never executed, until
the store arms it. The previous binary is kept as `store_daemon.prev` by the
installer for a manual rollback.

## 5. The home-screen base (tile patch)

The StoreTile patch edits the owner's own `QHMI_HomeScreenMain_Advanced.qml`, so
it needs that file as a **base** - never committed (`base/` is gitignored, like
the other packages). Two ways to provide it:

```sh
# from an extracted rootfs (a stock home screen)
SCOURGE_ROOTFS=/path/to/rootfs sh scripts/make_base.sh

# from the car's current file - carries the Battery/WiFi/Tweaks markers
sh scripts/make_base.sh --from-dump /path/to/QHMI_HomeScreenMain_Advanced.qml
```

The variant name comes from the payload markers in the file (`stock`, `hello`,
`battery_tile_wifi`, …), which is what `build_artifacts.py` emits and what the
installer looks for at install time. A base without the shared allocator (a
plain stock screen) is **skipped with a warning**: the page and the daemon still
install and the result says `page installed, tile skipped`. The installer never
falls back to `stock` once any other payload marker is present, so it cannot
wipe the Battery/WiFi/Tweaks tiles.

## 6. Store-installed packages inject their tile at install time

The Store itself is built like every other package (variant files from the
owner's base). **Anything the Store installs afterwards has no base and no
variant**, so its installer injects its tile into the live home screen with
`scripts/qml_inject.sh`:

```sh
# copy the component next to the others, then inject the instance snippet
cp MyTile.qml /app/share/AppHomeScreen/qml_eco3.0/component/
sh qml_inject.sh --file /app/share/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Advanced.qml \
                 --anchor menuPage --block MyTile.instance.qml
sh qml_inject.sh --file … --revert      # undo, from the backup
```

Rules the injector enforces: `--block` is an **instance** snippet (`MyTile { … }`),
never the component file - imports are only legal at the top of a file and the
script strips them with a warning; the insertion is brace-matched after the item
whose `id: <anchor>` matches (the same allocator the build-time variants use);
the brace balance is verified before writing; the original is kept in
`/rw_data/store/qml-backup/` and `Advanced.before-store.qml`-style backups stay
on the stick; SMACK64 is preserved; the QML caches are cleared. `--dry-run`
reports where it would insert. After an injection the AppHomeScreen must be
restarted (the package installers already do that).

## 7. Two sources, one pipeline

The app installs from **WiFi (the hub)** and from a **USB stick**, and both end
in the same `stage_and_run()`: sha256 -> tar -> `run_stuff.sh` -> ledger ->
report. Only the source differs.

- **WiFi / hub**: `/refresh` caches the signed index, `/install?pkg=` downloads.
- **USB / offline**: drop `<pkg>-<version>.tar.gz` (plus optional `.json` with
  `pkg`/`version`/`sha256`, and a `.tar.gz.sha256`) into
  `<stick>/scourge-packages/`; `/local` lists them (`file`, `sha256_sidecar`,
  `meta`) and `/install-local?file=` installs one with no network at all. The
  USB view works unpaired; the catalogue view needs pairing to queue.
  A stick with no sidecar hash is installed with a warning row instead of a
  silent trust.

`packages/store/scripts/build_payload.sh` writes the app itself into
`scourge-packages/` on the stick, so the installing stick doubles as an offline
catalogue.

## 7. Transport

Plain `http://` in-process (POSIX sockets, no TLS, no libcurl).

- **Redirects are followed**: 301/302/303/307/308 with a `Location` header, up to
  `STORE_MAX_REDIRECTS` (3) hops, absolute or root-relative. This is what makes a
  hub behind a reverse proxy work: `http://` -> `https://` at the edge no longer
  turns into `index fetch failed (301)`.
- **`https://` works**: the daemon drives the vendor
  `/usr/bin/openssl s_client -quiet -connect host:443 -servername host
  -verify_return_error -verify 5 -CApath /etc/ssl/certs` over pipes, writes the
  HTTP request to it and parses the answer (Content-Length, or chunked).
  `-verify_return_error` fails the handshake on a bad chain, so a forged answer
  never reaches the parser; there is no `--insecure` switch. The CA path is
  `--ca-path` / `ca_path=` (default `/etc/ssl/certs`, where the unit ships 440
  hashed CAs) and the read timeout is `--tls-timeout` (default 15 s).
  A public hub at `https://hub.example` therefore needs no proxy or helper on the
  unit; plain `http://` stays for LAN/local use.
  Integrity does not depend on TLS either way: the index is ECDSA-signed and
  every artifact is sha256-checked, so a tampered plain-http answer is rejected.
DNS: the daemon resolves A records itself (`/etc/hosts`, then the
`resolv.conf` nameserver) because a static glibc binary cannot use NSS.

## 8. Safety

- installs are opt-in (`allow_install`), never on by default;
- every artifact is sha256-verified against the signed index before execution;
- the hub refuses overlapping packages (409) and the daemon says why;
- nothing touches firmware, the updater, bootloader or partitions;
- the daemon binds `127.0.0.1` only.

## 9. Tests

`tests/test_daemon.py` (host, no car): fake hub serves `register`, `pair`,
`index` and `report`; the job downloads a real tarball, verifies sha256, extracts,
runs `run_stuff.sh`, records the ledger and reports. Also covers a tampered
artifact (fails before execution) and the `allow_install=0` staging path.
