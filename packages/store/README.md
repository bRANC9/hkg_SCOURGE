# Store — Marketplace agent

The head-unit side of the SCOURGE Hub marketplace. One static daemon on
`127.0.0.1:8092`; the future **Marketplace tab** only calls it and draws the
answers. See `SPEC.md` for the contract.

```sh
make -C src                # -> src/store_daemon (static)
python3 tests/test_daemon.py   # fake hub, whole install job, no car needed
sh scripts/build_payload.sh /run/media/<stick>   # USB payload
```

What it does, in one line each:

- **pair** with the hub using the 6-digit code or the QR token from the unit;
- **refresh** the signed catalogue for this unit's firmware/region/gui;
- **install** a package: download → sha256 → tar → `run_stuff.sh` → report back
  (with the boot counter), refusing anything the hub flags as an overlap;
- keep a **ledger** of what was installed, and never execute anything unless
  `allow_install=1` is set explicitly.

The daemon does not draw UI, does not touch QML, firmware, the updater or
partitions, and binds localhost only.
