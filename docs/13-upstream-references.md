# 13 — Upstream references

SCOURGE vendors no third-party code. The dependencies below are fetched
at setup time into `third_party/` (gitignored) by
`scripts/00_fetch_third_party.sh`, pinned per `config/third_party.lock`.
This index tells agents (and humans) what is worth reading in the fetched
material and where to find it, plus the wider ecosystem.

## The head unit's own OSS drop (LG, 2023)

Model `IWK2G3FQLWG` ("IAH1483SKDG") is listed on
`opensource.lge.com` → B2B → Vehicle Component, two entries:

| Entry | Source | Notice (component versions) |
|---|---|---|
| IAH1483SKDG 2.0 (osSeq 511245) | `IAH1483SKDG_2.0.zip` | `OSSNotice-2329_…_230713.html` |
| IAH1483SKDG 3.0 (osSeq 511246) | `IAH1483SKDG_3.0.zip` (1.4 GB) | `OSSNotice-2652_…_230705.html` |

The notice is the fastest way to know **which upstream versions the unit runs**:
Linux 4.1.27, glibc 2.25, Qt 5.4.1, connman 1.29, systemd 225, BusyBox 1.23.2,
bluez 5.43, hostap-utils 0.4.7, Wireless Tools 30, iw 4.1, libnl 3.2.25,
dropbear 2015.68, dlt-daemon 2.16.0, CommonAPI 3.1.12, persistence 7.0.

The zip carries **only copyleft sources** (GPL/LGPL/MPL) plus a toolchain —
there is **no BSD/Apache/ISC material**, so the vendor `wpa_supplicant`
(`android-external-wpa_supplicant_8`, BSD-3), `iw`, `dropbear` and the Qt apps
(`AppHomeScreen`, `AppStandbyClock`) are **not** in it. Useful members:

| Need | Member |
|---|---|
| Kernel + `bcmdhd` driver, AP/scan capability | `GPL-2.0/Linux Kernel/kernel-apl-df18113.tar.gz` (129 MB) |
| Why `connmanctl tether` refuses (EACCES / EINVAL / EOPNOTSUPP, the LG `WIFI_AP_NOT_SUPPORTED` flag) | `GPL-2.0/connman/connman.tar.bz2` → `src/technology.c`, `plugins/wifi.c`, `client/commands.c` |
| Which applets the unit really has | `GPL-2.0/busybox/busybox.tar.gz` → `.config` (`CONFIG_UDHCPD=y`, `CONFIG_UDHCPC=y`) |
| Unit ordering / boot targets | `GPL-2.0/systemd/systemd.tar.bz2` |

`tools/lge_oss_remote_zip.py` reads a member of that zip over HTTP range
requests, because the link runs at ~135 KB/s and the listing (central directory,
at the end of the file) is a few requests:

```sh
# presigned URL: POST /download/releaseFileDownloadUrl
#   osSeq=511246&fileType=Op&fileIdx=1&modelName=IWK2G3FQLWG   (Op = source, Li = notice)
python3 tools/lge_oss_remote_zip.py --url "<signed url>" list
python3 tools/lge_oss_remote_zip.py --url "<signed url>" extract --out DIR \
        --match connman/ --match busybox/ --limit-mb 60
```

A HEAD request returns 403 on a presigned GET URL — use a range GET (`-r 0-0`)
to learn the size. The signed URL lives 30 minutes.

## The fetched TOCTOU repo (`third_party/ioniq5-usb-toctou/`)

Its documentation is the deep background for docs/01–05. Read it from the
local clone (paths below) or on GitHub.

| Need | Read |
|---|---|
| TOCTOU deep-dive + honest evidence grading | `docs/01-vulnerability.md`, `docs/03-evidence.md` |
| Rig architecture + nbdkit swap logic | `docs/02-attack-chain.md` |
| **Every rig bring-up bug and fix** (swap trigger, watchdog reboot, offline logging) — the single most valuable file when the rig misbehaves | `docs/04-field-journal.md` |
| Update-engine verification behavior + vendor-side fix patterns — needed to (a) understand why our package shapes work and (b) **recognize when a future firmware closes the TOCTOU door** (copy-then-verify = stop; see docs/09 Gate 2) | `docs/05-mitigations.md` |
| Owner Tools image design/build (the earlier payload; its docs/07 §3 is the original static-musl dropbear recipe that `packages/wifi/scripts/build_dropbear.sh` implements) | `docs/06-owner-menu-v2.md`, `docs/07-build-and-flash-runbook.md` |
| From-nothing rig reproduction | `REPRODUCE.md`, `pizero/DEPLOY.md` (re-arm, offline logging), `pizero/STATUS.md` |
| No-car race demo | `poc/run_poc.sh` |
| Rig deployment detail | `pizero/DEPLOY.md` |

**Boundary:** the upstream docs describe *their* payload ("Owner Tools"),
which differs from SCOURGE's stage-1. Our docs stay authoritative for
SCOURGE workflows; the upstream set is background/deep-dive only. Note
that the upstream repo grants no license — which is exactly why we fetch
it pinned and never copy its code into this repo.

## The fetched key source (`third_party/hkmcavn-tools/`)

GPL-2.0 public toolset for HMG/LGe head-unit firmware. `xcrypt/xcrypt.py`
is the canonical public reference for the TE2 container format and the
source `00_fetch_third_party.sh` extracts the content key from.

## The fetched downloader (`third_party/nudl/`)

GPL-3.0 unofficial client for the OEM's own update servers (same publicly
available firmware as the official Navigation Updater, no login).
`nudl list -b hyundai`, `nudl download -m <model> -r <region> -v <ver>`,
`nudl verify -d <dir>` (offline integrity check against the package's own
`.ver` checksums). Releases are SSH-signed; verify per the upstream
instructions.

## Ecosystem

| Resource | What |
|---|---|
| [JaredReabow/ioniq-5-Reverse-Engineering](https://github.com/JaredReabow/ioniq-5-Reverse-Engineering) | Foundational Ioniq 5 RE work (credited by the TOCTOU repo) |
| The Gen5W Discord | The community behind this work; SCOURGE has a channel there (see README) |
| Costantino, De Vincenzi, Matteucci — *A vehicle firmware security vulnerability: an IVI exploitation* (2024), DOI `10.1007/s11416-024-00522-4` | Peer-reviewed prior work on this platform family |
| [xwtk/hmg_nemesis](https://github.com/xwtk/hmg_nemesis) | The (unrelated, unaffiliated) Android-side counterpart for Gen5W S5W_M units; SCOURGE's README credits it as inspiration. Note: per their compatibility table, their approach does not apply to S5W_L or Ioniq 6 |
| galaxy4public/hkmcavn-tools, chenxiaolong/nudl | Fetched dependencies above |
| rgerganov/gen5fw, greenluigi1/HyundaiFirmwareDecrypter | Public HMG firmware tooling for *other* platform generations (Telechips Gen5, D-Audio2) — background only, not applicable to Gen5W_L |

## Takedown resilience

If an upstream disappears: keep local copies (the fetch script accepts
`--from-archive <path>` to seed `third_party/` from a saved clone or
tarball — maintainers should keep one). A failed pin is a hard stop;
restore from your archive or ask in the Discord. The
`config/third_party.lock` file records every pin and the sha256 of each
consumed file so restored copies can be verified.
