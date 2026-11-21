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

## 4. Transport

Plain `http://` in-process (POSIX sockets, no TLS, no libcurl). `https://`
requires the hub to be reachable through a TLS terminator on the unit or a
future `STORE_FETCH_HELPER`; the logic is independent of the transport.
DNS: the daemon resolves A records itself (`/etc/hosts`, then the
`resolv.conf` nameserver) because a static glibc binary cannot use NSS.

## 5. Safety

- installs are opt-in (`allow_install`), never on by default;
- every artifact is sha256-verified against the signed index before execution;
- the hub refuses overlapping packages (409) and the daemon says why;
- nothing touches firmware, the updater, bootloader or partitions;
- the daemon binds `127.0.0.1` only.

## 6. Tests

`tests/test_daemon.py` (host, no car): fake hub serves `register`, `pair`,
`index` and `report`; the job downloads a real tarball, verifies sha256, extracts,
runs `run_stuff.sh`, records the ledger and reports. Also covers a tampered
artifact (fails before execution) and the `allow_install=0` staging path.
