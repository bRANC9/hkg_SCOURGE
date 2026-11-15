# 15 — The micom command surface (`com.lge.car.micom`)

TARGET: Hyundai Ioniq 5 Gen5W_L (Linux x86-64), EU region, firmware
`NE_E.EUR.S5W_L.001.001.260128`.

**Status: derived, not car-validated.** Everything here is read out of the
shipped binaries. Nothing in this document was exercised on the vehicle. Where
a claim rests on an inference rather than on a decoded instruction it says so.

This is a map of an interface the platform already exposes. It is
documentation, not a new capability: every command below is reachable today by
the vendor's own apps, and most of them by `dbus-send`.

---

## 1. What the micom is

On Gen5W the head unit does not talk to the vehicle directly. A separate
microcontroller — the **micom** — owns the hardware that must survive the SoC
being off: power rails, the power key, display backlight and PWM, fan, camera
power, RTC, CAN bridging, tuner and amplifier control, and the sleep/wake
sequencing for the whole head unit.

The SoC reaches it over UART, through `/app/bin/micommanager_userspace`, which
is a Qt5 program that owns the bus name `com.lge.car.micom` on the system bus
and translates D-Bus method calls into micom frames.

```
   app (AppHomeScreen, AppSetup, CANService, …)
        │  D-Bus: com.lge.car.micom.<iface>.<Method>
        ▼
   micommanager_userspace
        │  UART: AA <op_hi> <op_lo> <len> <payload…> <xor>
        ▼
      micom MCU   ──►  CAN / LIN / backlight / power rails
```

There is a second consumer, `/app/bin/micom_fw_updater`, which flashes the
micom itself through the same service (§9).

---

## 2. Provenance and how to regenerate

| | |
|---|---|
| Service | `/app/bin/micommanager_userspace`, export `bin_e5f040ffc41cc980`, 542,856 B, sha256 `e5f040ffc41cc980…d7949` |
| Flasher | `/app/bin/micom_fw_updater`, export `bin_7182ad17ef4d5cd5`, 149,632 B, sha256 `7182ad17ef4d5cd5…d250` |
| 2021 baseline | same path in the 2021.08.17 package, 444,544 B, sha256 `bd4b4bdb33ad229b…aeb4b`, BuildID `69b7350…` |
| Export | Ghidra 12.1.4, `image_base 0x100000`, 1,407/1,407 functions decompiled |

All three hashes are pinned in `config/firmware_locks.env`
(`MICOM_MANAGER_SHA256`, `MICOM_FW_UPDATER_SHA256`,
`MICOM_MANAGER_210817_SHA256`).

**Addresses in this document are Ghidra export addresses.** Subtract
`0x100000` to get the ELF virtual address. For the code addresses listed here
the file offset and the virtual address are the same, because the first LOAD
segment is identity-mapped; the writable segment at `0x282578` is not, so do not
generalise this to data addresses.

### What the export has to look like

The opcode half reads a Ghidra C export, not the binary. `load_export` walks
every `functions_*.c` in the directory and expects one banner per function:

```
/* ===== 0010de00 FUN_0010de00 ===== */
```

followed by the decompiled body and terminated by the next banner. `meta.json`
must exist and carry `image_base` as a hex string (`"0x100000"`) or a JSON
integer; a function's export address is `vaddr + image_base`. The directory
name must be `bin_<sha256[:16]>` of the ELF, which is what
`gate_export` checks.

Regenerate the whole table from your own firmware:

```sh
python3 tools/micom/extract_commands.py table \
    --elf   path/to/micommanager_userspace \
    --export path/to/ghidra/export/bin_e5f040ffc41cc980 \
    --json  work/micom/commands.json

python3 tools/micom/extract_commands.py signals \
    --elf   path/to/micommanager_userspace \
    --notes tools/micom/signal_notes.json \
    --json  work/micom/signals.json

python3 tools/micom/extract_commands.py render \
    --json         work/micom/commands.json \
    --signals-json work/micom/signals.json \
    --doc          docs/15-micom-commands.md
```

The tool joins three independent sources and **aborts rather than emit a
partial table**:

1. **moc half** — `QMetaObject` records in `.data.rel.ro`, their `moc` data
   table and `QByteArrayData` literal array give `(class, index, name, argc,
   kind)`. This half needs no decompiler.
2. **interface XML** — the service embeds its own
   `<interface name="com.lge.car.micom.…">` strings in `.rodata`; parsing them
   gives the In/Out D-Bus types, also without a decompiler. The same pass
   yields the 124 signal declarations and their `Out` types.
3. **opcode half** — the `qt_static_metacall` switch maps `(class, index)` to
   the slot function, whose body contains `FUN_0010ec60(manager, 0xNNNN)` or
   `FUN_0010f400(manager, 0xNNNN, …)`. This needs the Ghidra export.

When given `--notes`, `signals` refuses to run if a declared signal lacks an
entry in `tools/micom/signal_notes.json`, or if the notes name a signal the
ELF does not declare. The notes themselves are first-party **inference**
(AGENTS.md rule 10); they are not decoded protocol fields.

Gates, all of which abort:

- the ELF must match its `MICOM_*_SHA256` pin — a missing pin is a failure, not
  a warning;
- the export directory must be named `bin_<sha256[:16]>`, so opcodes cannot be
  joined against a decompilation of a different build;
- every slot of the 16 interface classes must resolve to a metacall case, a
  slot body and at most one distinct opcode;
- the export must contain a `meta.json` with a usable `image_base`.

On 260128 that join accounts for **503 rows**: 253 service slots and 250
`…Adaptor` proxy slots, of which **242 service rows carry an opcode** and
**238 distinct opcode values** are used (four pairs share a code — §11). Three
service slots are withheld under rule 6 (§7), so the emitted JSON and stdout
carry 500 rows; the counts in this document include the withheld ones where
they affect a total. The three internal helper classes (`CMsgProcessor`,
`CUartCommThread`, `replyDbusConnection`) are not D-Bus interfaces and are
reported rather than joined.

---

## 3. The service

`micommanager_userspace` registers **eight** D-Bus interfaces. They do not all
live at the same object path — the path is per interface:

| Interface | Object path | Methods | Signals | moc class (service / adaptor) |
|---|---|---:|---:|---|
| `com.lge.car.micom.audio` | `/com/lge/car/micom` | 16 | 7 | `AUDIO` / `AUDIOAdaptor` |
| `com.lge.car.micom.radio` | `/Radio` | 16 | 11 | `RADIO` / `RADIOAdaptor` |
| `com.lge.car.micom.tmu` | `/Tmu` | 21 | 15 | `TMU` / `TMUAdaptor` |
| `com.lge.car.micom.ihu` | `/Ihu` | 106 | 34 | `IHU` / `IHUAdaptor` |
| `com.lge.car.micom.hdr` | `/Hdr` | 8 | 12 | `HDR` / `HDRAdaptor` |
| `com.lge.car.micom.tmc` | `/Tmc` | 11 | 10 | `TMC` / `TMCAdaptor` |
| `com.lge.car.micom.can` | `/Can` | 47 | 16 | `CAN` / `CANAdaptor` |
| `com.lge.car.micom.eng` | `/Eng` | 26 | 19 | `ENG` / `ENGAdaptor` |
| **total** | | **251** | **124** | 16 classes + 3 helpers |

Getting this wrong is not cosmetic: `dbus-send` to `/com/lge/car/micom` for a
CAN method fails, and `packages/battery/docs/MICOM_WHITELIST.md` correctly uses
`/Can`. The paths come from the eight `QDBusConnection::registerObject` calls
at `FUN_00107d10`, and the failure strings next to them name the interface. One
of those strings is wrong in the vendor's own code: the `/Radio` registration
logs `Cannot register audio object`, a copy-paste slip that has no behavioural
effect.

Each interface exists twice in the binary: a service class that holds the
opcode and an `…Adaptor` class generated by `qdbusxml2cpp` that only forwards.
Only the service class carries an opcode.

### The numbers, precisely

Four different counts appear above and they do not have to agree:

| Count | Value | What it is |
|---|---:|---|
| methods + signals declared in the embedded interface XML | 251 + 124 | what a D-Bus client sees |
| slots implemented by the eight service classes | 253 | 251 − 1 unimplemented + 3 undeclared |
| of those, emitting a micom frame | 242 | the tables in §6 |
| of those, never touching the UART | 11 | 8 in §7, 3 withheld |

One declared method — `CAN.NotifyCanDTCFinishEvent`, declared as a *method*
taking an `ay` — is **not implemented as a method** in this build. The class
declares a same-named *signal*, but there is no method slot, so a method call
fails at the service rather than at the bus.

Three other slots are implemented but **not declared** in the XML, and are
absent from the `…Adaptor` classes as well. They are described as a group in
§7 and deliberately not enumerated command-by-command.

### Access control

`micommanager_userspace` is reachable over the system bus by any local process
the D-Bus policy admits (`/etc/dbus-1/system.d/com.lge.car.micom.conf`), but
the *CAN send* methods carry an extra check: the handler resolves the caller's
`/proc/<pid>/exe`, takes the basename, and compares it against a hardcoded
list. That mechanism and its workaround are documented separately in
`packages/battery/docs/MICOM_WHITELIST.md`; it applies to the CAN interface,
not to the rest.

---

## 4. The wire protocol

Decoded from `FUN_0010de00` (transmit) and `FUN_00111060` (receive) in
`bin_e5f040ffc41cc980`.

### Frame

```
  0xAA   op_hi   op_lo   len   data[0] … data[len-1]   xor
  ────   ─────   ─────   ───   ────────────────────    ───
  SOF    16-bit opcode   payload length               XOR of all preceding
         big-endian                                      bytes, one byte
```

The checksum is a single byte: the XOR of every byte from the SOF through the
last payload byte, inclusive. The frame is `len + 5` bytes.

The opcode is big-endian on the wire and the payload length is one byte, so a
command carries at most 255 payload bytes. In practice the methods on this
interface pass one `ay` array or a handful of `i` integers, so frames are
short.

### Receive

`FUN_00111060` rejects anything shorter than 4 bytes
(`"SPI data size is too short(%d)"`), derives the opcode as
`data[1] << 8 | data[2]`, strips `data[0]`, and dispatches the remainder to
registered observers before publishing the matching `Notify…` signal.

### What `0x8000` is *not*

`FUN_0010efc0` computes `opcode | 0x8000` and stores it as a **key in the
internal pending-request map**, so that an inbound frame can be correlated with
the call that is waiting for it. That value never reaches the wire — the same
function then calls `FUN_0010de00(manager, opcode, …)` with the unmodified
opcode.

So `0x8000` is an internal correlation tag, not a wire flag and not an "Ask
bit". Do not OR it into a frame.

### Sequence counter

A per-manager byte at `[manager+0x18]` is incremented on every transmit
(`add BYTE PTR [r13+0x18], 0x1`). It is **not** part of the frame. Its purpose
is to key the internal bookkeeping, not the UART protocol.

### DLT trace

Every transmitted command is logged through DLT as
`[writeToMicom] Ask CMD: 0x…`, and every received frame as `MICOM to MW`. That
makes `micommanager_userspace`'s DLT context a live protocol trace if you ever
need to watch the bus without touching the UART.

---

## 5. Opcode families

The 16-bit opcode space is partitioned by the high byte:

| Family | Count | Naming | Meaning |
|---|---:|---|---|
| `0x01xx` | 132 | `Set…` except one | commands from the head unit to the micom |
| `0x02xx` | 75 | all `Ask…` | queries from the head unit to the micom |
| `0x04xx`, `0x07xx`, `0x08xx` | 5 + 1 + 3 | `SetCAN…` / `SetExt…` / `AskExt…` | CAN raw-frame bridging |
| `0x06xx` | 25 | `SetMsgSelf…`, engineering | self-programming and factory use |
| `0x9999` | 1 | `SetETCOptionSet` | anomalous — see §11 |

The `0x01xx` / `0x02xx` split is exact in one direction and nearly so in the
other: **no** `0x01xx` opcode belongs to an `Ask…` method, and **no** `0x02xx`
opcode belongs to a `Set…` method. The single exception to the `Set…` label in
`0x01xx` is `CAN.NavigationTxMessageRequest` (`0x018f`), which is named for
what it does rather than for a direction. That near-perfect split is the
cleanest confirmation that the join in §2 is correct.

The CAN interface is **not** confined to the `0x04xx`/`0x07xx`/`0x08xx`
families: 24 of its 40 opcodes are ordinary `0x01xx` commands. What is special
about the interface is the raw-frame subset below, which bypasses the normal
command numbering entirely:

| Opcode | Method | Note |
|---|---|---|
| `0x0400` | `SetCANMicomRecv`, `SetCANFixedMicomRecv` | both raw-frame sends share one opcode. The four-argument form has two separate send sites, for the standard and extended CAN-id paths (its log strings distinguish `SetCANFixedMicomRecv` from `SetCANFixedMicomRecv(Extended)`). |
| `0x0403` | `SetCANTPMicomRecv` | CAN transport protocol |
| `0x0480` | `SetExtMMCanTxReq` | multi-media CAN |
| `0x0482` | `AskExtMMCanReq` | |
| `0x0700` | `SetBCANMicomRecv` | body CAN |
| `0x0800` | `SetCCANMicomRecv` | chassis CAN |
| `0x0880` | `SetExtICanTxReq` | |
| `0x0882` | `AskExtICanReq` | |

The `0x04xx`/`0x07xx`/`0x08xx` grouping is observable but we have no decoded
explanation for the encoding; it may be a bus-type field rather than a command
number. Treat it as an observation, not a definition.

---

## 6. The command and signal tables

Columns: the opcode as it appears on the wire, the D-Bus method, and the
direction-tagged argument types from the interface XML that the service itself
embeds. Signatures use D-Bus type codes (`ay` = byte array, `i` = int32,
`s` = string, `b` = boolean). Where a method has an `out` argument it returns a
value synchronously; where it has none the reply is empty.

<!-- BEGIN:ihu -->

### `com.lge.car.micom.ihu` — the head unit's own hardware, power and settings

106 commands.

| Opcode | Method | In | Out |
|---|---|---|---|
| `0x0101` | `SetSYSVariantOptionSet` | ay | ay |
| `0x0102` | `SetSysPartNumberInfo` | ay | — |
| `0x0103` | `SetSysUPMSet` | ay | ay |
| `0x0104` | `SetSysFactoryModeSetEng` | ay | — |
| `0x0105` | `SetSysUpmReset` | ay | ay |
| `0x0106` | `SetSysDspTypeReset` | ay | — |
| `0x0107` | `SetSysDebuggingMode` | ay | — |
| `0x0160` | `SetHWPowerKeyControl` | ay | — |
| `0x0161` | `SetHWLCDBackLightControl` | ay | — |
| `0x0162` | `SetHWLCDPWMControl` | ay | — |
| `0x0163` | `SetHWFanControl` | ay | — |
| `0x0164` | `SetHWPeripheralReset` | ay | — |
| `0x0165` | `SetHWMicControl` | ay | — |
| `0x0166` | `SetHWRTCSet` | ay | — |
| `0x0167` | `SetHWCameraPowerControl` | ay | — |
| `0x0168` | `SetHWDiscIndicationLEDControl` | ay | — |
| `0x0169` | `SetHWBTPowerControl` | ay | — |
| `0x016a` | `SetUpdateTouchICControl` | ay | ay |
| `0x016c` | `SetHwLcd2SensorSupportPwmControl` | ay | — |
| `0x016d` | `SetHwLcd2SensorNotSupportPwmControl` | ay | — |
| `0x016e` | `SetHWLcdThemaInfoControl` | ay | ay |
| `0x0190` | `SetETCGUIMode` | ay | — |
| `0x0191` | `SetETCAppBootComplete` | ay | — |
| `0x0192` | `SetETCFactorySetting` | ay | — |
| `0x0193` | `SetETCAdmSettingValue` | ay | — |
| `0x0194` | `SetETCSleepConfirm` | ay | — |
| `0x0195` | `SetValetModeSet` | ay | — |
| `0x0196` | `SetETCUSMMode` | ay | — |
| `0x0197` | `SetETCLanguageInfoSet` | ay | — |
| `0x0198` | `SetMainImageUpgrade` | ay | — |
| `0x0199` | `SetETCNaviSCCDataSend` | ay | — |
| `0x019a` | `SetETCCPUAliveMsg` | ay | — |
| `0x019b` | `SetETCMPAUTOModeTestMsg` | ay | — |
| `0x019c` | `SetETCEmergencyConditionMsg` | ay | — |
| `0x019d` | `SetSettingValueStatusMsg` | ay | — |
| `0x019f` | `SetETCBTMACAddrStore` | ay | — |
| `0x01a0` | `SetETCCPUBIOSWDTCNTCLR` | ay | — |
| `0x01a1` | `SetHWPeripheralPowerControl` | ay | — |
| `0x01a2` | `SetETCCPUECALLSTATUS` | ay | — |
| `0x01a3` | `SetCustomSetting` | ay | — |
| `0x01a4` | `SetControlKeyLock` | ay | — |
| `0x01a5` | `SetScheduledUpdateInfo` | ay | ay |
| `0x01a6` | `SetControlMicomSleep` | ay | ay |
| `0x01a7` | `SetCameraBrightness` | ay | — |
| `0x01a8` | `SetControlTimeFormat` | ay | — |
| `0x01a9` | `SetMMCANEnable` | ay | — |
| `0x01aa` | `SetETCCPUWDTReset` | ay | ay |
| `0x01ab` | `SetRecoveryMode` | ay | — |
| `0x01ac` | `SetSecureMode` | ay | — |
| `0x01ad` | `SetCameraSetting` | ay | — |
| `0x01ae` | `SetETCMainChannelVolumeKeyLock` | ay | — |
| `0x01af` | `SetVolumeTuneKeySwapCtrl` | ay | ay |
| `0x01b0` | `SetDIDInfo` | ay | — |
| `0x01b1` | `SetTraceDate` | ay | — |
| `0x01b2` | `SetSerialNumber` | ay | — |
| `0x01b3` | `SetDMBCASID` | ay | — |
| `0x01bb` | `SetDiagAdvanceSeedData` | ay | — |
| `0x01c0` | `SetControlOtaUpgradeMsg` | ay | — |
| `0x01c2` | `SetControlVehicleOtaUpgrade` | ay | — |
| `0x01c3` | `SetEtcNaviOutermostRegion` | ay | — |
| `0x0201` | `AskSysBootinfo` | — | ay |
| `0x0203` | `AskSysVersion` | — | ay |
| `0x0204` | `AskTuningVersion` | — | ay |
| `0x0205` | `AskHDRVersion` | — | ay |
| `0x0206` | `AskExternalSPDIFAMPVersion` | — | ay |
| `0x0207` | `AskPartNumberInfo` | — | ay |
| `0x0208` | `AskSysUpmID` | — | ay |
| `0x0209` | `AskFactoryModeInfo` | — | ay |
| `0x020a` | `AskSysAutomaticBrightness` | ay | ay |
| `0x020b` | `AskVehicleTypeAndEnergySource` | — | ay |
| `0x020c` | `AskDebuggingMode` | — | ay |
| `0x020d` | `AskMicomCRC` | — | ay |
| `0x020e` | `AskRxswinInfo` | — | ay |
| `0x0283` | `AskInfoFaultCodeStatus` | ay | ay |
| `0x0285` | `AskUSMMessageTimeout` | — | ay |
| `0x0290` | `AskLCDSetInfoValue` | — | ay |
| `0x0293` | `AskLanguageInfoValue` | — | ay |
| `0x0294` | `AskEEPROMRead` | ay | ay |
| `0x0295` | `AskValetMode` | — | ay |
| `0x0296` | `AskUSMSetting` | ay | ay |
| `0x029a` | `AskEtcCarInformationRead` | — | ay |
| `0x029b` | `AskEtcLcd2SensorSupportPwmControl` | ay | ay |
| `0x029c` | `AskEtcLcd2SensorNotSupportPwmControl` | ay | ay |
| `0x029d` | `AskSettingInfoValue` | — | ay |
| `0x029e` | `AskEtcLcdThemaInfo` | ay | ay |
| `0x029f` | `AskBTMACAddr` | — | ay |
| `0x02a0` | `AskCPUBIOSCountInfo` | — | ay |
| `0x02a1` | `AskCPUECALLSTATUS` | — | ay |
| `0x02a3` | `AskCustomSetting` | ay | ay |
| `0x02a5` | `AskScheduledUpdateInfo` | ay | ay |
| `0x02a7` | `AskCameraBrightness` | — | ay |
| `0x02a8` | `AskSocLevel` | — | ay |
| `0x02a9` | `AskRearMonitorStatus` | — | ay |
| `0x02ab` | `AskETCEcallStatus` | — | ay |
| `0x02ac` | `AskSecureMode` | — | ay |
| `0x02ad` | `AskCameraSetting` | ay | ay |
| `0x02ae` | `AskEtcAdmSettingValue` | — | ay |
| `0x02af` | `AskVolumeTuneKeySwap` | — | ay |
| `0x02b1` | `AskTraceDate` | — | ay |
| `0x02b2` | `AskSerialNumber` | — | ay |
| `0x02b2` | `AskSerialNumber2` | ay | ay |
| `0x02b3` | `AskDMBCASID` | — | ay |
| `0x02b4` | `AskVariantCoding` | — | ay |
| `0x02b5` | `AskHUDAbsentStatus` | — | ay |
| `0x02c3` | `AskNaviOutermostRegion` | — | ay |
| `0x9999` | `SetETCOptionSet` | ay | ay |

<!-- END:ihu -->
<!-- BEGIN:can -->

### `com.lge.car.micom.can` — vehicle bus bridging

40 commands.

| Opcode | Method | In | Out |
|---|---|---|---|
| `0x0171` | `SetAutoTestKeyControl` | ay | — |
| `0x0172` | `SetAutoTestWIPIDControl` | ay | — |
| `0x0173` | `SetSXMRADIOID` | ay | — |
| `0x0180` | `SetDATCInitialRequest` | — | — |
| `0x0181` | `SetCARSETInitialRequest` | — | — |
| `0x0182` | `SetGreenCarInitialRequest` | ay | — |
| `0x0182` | `SetPGSInitialRequest` | — | — |
| `0x0183` | `SetAVMInitialRequest` | — | — |
| `0x0184` | `SetClusterInitialRequest` | — | — |
| `0x0185` | `SetUSMInitialRequest` | — | — |
| `0x0186` | `SetExternalClockInitialRequest` | — | — |
| `0x0187` | `SetLowFuelWarnningRequest` | — | — |
| `0x0188` | `SetLevelGearInitialRequest` | — | — |
| `0x0189` | `SetCANTPSENDMsg` | ay | — |
| `0x018a` | `SetDTCSendMsg` | ay | — |
| `0x018b` | `SetEDTAUXMICSTARTMsg` | ay | — |
| `0x018c` | `SetDTCRequest` | ay | ay |
| `0x018d` | `SetDVRSInitialRequest` | — | — |
| `0x018e` | `SetCANAMP_HU_P_01Request` | — | — |
| `0x018f` | `NavigationTxMessageRequest` | ay | — |
| `0x019b` | `SetVehicleInfoMsg` | ay | — |
| `0x01b5` | `SetCANIDMsgRequest` | ay | — |
| `0x01b6` | `SetTPIDMsgRequest` | ay | — |
| `0x01b7` | `SetEtcMmCanEnable` | ay | — |
| `0x0270` | `AskAutotestWIPID` | — | ay |
| `0x0280` | `AskDTCReadRequest` | ay | ay |
| `0x0281` | `AskDTCClearRequest` | ay | ay |
| `0x0289` | `AskCANMMCANMicomReq` | ay | — |
| `0x028a` | `AskCANICANMicomReq` | ay | — |
| `0x028b` | `AskCANTPMicomReq` | ay | — |
| `0x028c` | `AskNaviAppData` | — | ay |
| `0x0400` | `SetCANFixedMicomRecv` | ay, i, i, i | — |
| `0x0400` | `SetCANMicomRecv` | ay | — |
| `0x0403` | `SetCANTPMicomRecv` | ay | — |
| `0x0480` | `SetExtMMCanTxReq` | ay | — |
| `0x0482` | `AskExtMMCanReq` | ay | — |
| `0x0700` | `SetBCANMicomRecv` | ay | — |
| `0x0800` | `SetCCANMicomRecv` | ay | — |
| `0x0880` | `SetExtICanTxReq` | ay | — |
| `0x0882` | `AskExtICanReq` | ay | — |

<!-- END:can -->
<!-- BEGIN:eng -->

### `com.lge.car.micom.eng` — engineering, manufacturing and self-programming

25 commands.

| Opcode | Method | In | Out |
|---|---|---|---|
| `0x0601` | `SetMsgSelfGO` | ay | ay |
| `0x0602` | `SetMsgSelfEPV` | ay | ay |
| `0x0603` | `SetMsgSelfErase` | ay | ay |
| `0x0604` | `SetMsgSelfProgram` | ay | ay |
| `0x0605` | `SetMsgSelfProgEnd` | ay | ay |
| `0x0606` | `SetMsgSelfReset` | ay | ay |
| `0x0608` | `SetSaturnGpioCtrl` | ay | ay |
| `0x0610` | `SetDiagnosticInfoRequest` | ay | — |
| `0x0620` | `SetRadioAntennaSet` | ay | ay |
| `0x0622` | `SetHDRadioEngineerModeCommand` | ay | — |
| `0x0624` | `SetSubTunerCommand` | ay | ay |
| `0x0631` | `SetSubTunerFreqSet` | ay | ay |
| `0x0632` | `SetFMQualityWeightSet` | ay | ay |
| `0x0640` | `SetMPAutoTestModeCtrl` | ay | — |
| `0x0641` | `SetMPAutoTestVolumeCtrl` | ay | ay |
| `0x0657` | `SetAMPUpgrade` | ay | — |
| `0x0660` | `SetMPModeInfoRequest` | ay | ay |
| `0x0661` | `AskMPModeInfo` | — | ay |
| `0x0662` | `SetEnginnerModeEntryCheck` | ay | ay |
| `0x0663` | `AskEcallTirggerCount` | ay | ay |
| `0x0666` | `SetSoundSrcScaleCtrl` | ay | — |
| `0x0671` | `SetKeyPWMTest` | ay | ay |
| `0x0673` | `SetEngineerModeCameraBrightness` | ay | — |
| `0x0675` | `SetDisPlayRheostatValueCtrl` | ay | ay |
| `0x0676` | `SetDisPlayUserSettingValueCtrl` | ay | ay |

<!-- END:eng -->
<!-- BEGIN:tmu -->

### `com.lge.car.micom.tmu` — telematics unit

20 commands.

| Opcode | Method | In | Out |
|---|---|---|---|
| `0x0140` | `SetTMUServiceControl` | ay | — |
| `0x0141` | `SetTMUProvisionInfo` | ay | — |
| `0x0142` | `SetTMUVRMRequest` | ay | — |
| `0x0143` | `SetTMUVCRMRequest` | ay | — |
| `0x0144` | `SetTMUAppBootComplete` | ay | — |
| `0x0145` | `SetTMURemoteCANWakeUp` | ay | — |
| `0x0146` | `SetTMUOBCWakeUp` | ay | — |
| `0x0147` | `SetTmuShutDownReqeust` | ay | — |
| `0x0148` | `SetTmuBattSocShutDownRequest` | ay | — |
| `0x014c` | `SetTMUDIAGVINInform` | — | — |
| `0x014d` | `SetTmuAvoidExtDiagByVcrm` | ay | — |
| `0x014f` | `SetTmuPowerConditionRequest` | ay | — |
| `0x0240` | `AskTMUCANDATA` | ay | ay |
| `0x0241` | `AskTMUVRMGatherDataRequest` | ay | ay |
| `0x0242` | `AskTMUVCRMGatherDataRequest` | ay | ay |
| `0x0243` | `AskTMUMASGatherDataRequest` | ay | ay |
| `0x0245` | `AskTMUDIAGRMkeyStuck` | — | ay |
| `0x0246` | `AskTMUMCANStatus` | — | ay |
| `0x0247` | `AskTMUPinStatus` | — | ay |
| `0x0248` | `AskTMUVanHistory` | — | ay |

<!-- END:tmu -->
<!-- BEGIN:audio -->

### `com.lge.car.micom.audio` — amplifier and volume

16 commands.

| Opcode | Method | In | Out |
|---|---|---|---|
| `0x0110` | `SetAudioMainCHChange` | ay | — |
| `0x0112` | `SetAudioMixingCHChange` | ay | — |
| `0x0113` | `SetAudioVolume` | ay | — |
| `0x0114` | `SetAudioSetup` | ay | — |
| `0x0115` | `SetBeepPlay` | ay | — |
| `0x0116` | `SetAudioMute` | ay | ay |
| `0x0117` | `SetCurrentModeVolume` | ay | — |
| `0x0118` | `SetAudioSpeakerCtrl` | ay | — |
| `0x0119` | `SetAudioLastMemory` | ay | ay |
| `0x011a` | `SetAudioClusterAnimationCtrl` | ay | ay |
| `0x0210` | `AskSoundSetupInfo` | — | ay |
| `0x0211` | `AskVolumeInfo` | — | ay |
| `0x0212` | `AskAudioLastMemory` | — | ay |
| `0x0216` | `AskAudioMuteInfo` | — | ay |
| `0x0217` | `AskAMPSupportInfo` | — | ay |
| `0x0232` | `AskAudioSRCScaleInfo` | ay | ay |

<!-- END:audio -->
<!-- BEGIN:radio -->

### `com.lge.car.micom.radio` — tuner

16 commands.

| Opcode | Method | In | Out |
|---|---|---|---|
| `0x0120` | `SetRadioFreqChange` | ay | — |
| `0x0121` | `SetRadioPresetRecall` | ay | — |
| `0x0122` | `SetRadioPresetStore` | ay | — |
| `0x0123` | `SetRadioStationSearch` | ay | — |
| `0x0124` | `SetRadioPresetDelete` | ay | — |
| `0x0125` | `SetRadioSeekStopLevel` | ay | — |
| `0x0128` | `SetRadioBandFreqChange` | ay | — |
| `0x0129` | `SetRadioBandPresetRecall` | ay | — |
| `0x012a` | `SetRadioInfoCheckEnable` | ay | — |
| `0x012b` | `SetRadioMute` | ay | — |
| `0x019e` | `SetRadioOffMode` | ay | — |
| `0x0220` | `AskBroadcastingInfo` | ay | ay |
| `0x0221` | `AskPreset` | ay | ay |
| `0x0222` | `AskRadioEachPresetInfo` | ay | ay |
| `0x0224` | `AskSeekStopLevel` | — | ay |
| `0x0297` | `AskRadioOffMode` | — | ay |

<!-- END:radio -->
<!-- BEGIN:tmc -->

### `com.lge.car.micom.tmc` — traffic message channel

11 commands.

| Opcode | Method | In | Out |
|---|---|---|---|
| `0x0131` | `SetRDSOptionSets` | ay | — |
| `0x0132` | `SetRDSInfoCancel` | ay | — |
| `0x0133` | `SetRDSInfocheckEnable` | ay | — |
| `0x0134` | `SetRDSAFCounterClear` | ay | — |
| `0x0136` | `SetTMCManualFreqChange` | ay | — |
| `0x0137` | `SetTMCCountryCodeandDecodeStatus` | ay | — |
| `0x0138` | `SetDABFMPICode` | ay | — |
| `0x0139` | `SetDABFMFollowingFunction` | ay | — |
| `0x013a` | `SetRadioDABFMMultiplePICodeRequest` | ay | — |
| `0x013b` | `SetDABTAStatus` | ay | — |
| `0x0230` | `AskRDSOptionSetting` | — | ay |

<!-- END:tmc -->
<!-- BEGIN:hdr -->

### `com.lge.car.micom.hdr` — HD Radio

8 commands.

| Opcode | Method | In | Out |
|---|---|---|---|
| `0x0150` | `SetHDRTune` | ay | — |
| `0x0151` | `SetHDRiTuneTagging` | ay | — |
| `0x0152` | `SetHDROptionSet` | ay | — |
| `0x0154` | `SetHDRSignalReAquire` | ay | — |
| `0x0155` | `SetHDRAECommand` | ay | — |
| `0x0250` | `AskHDRSetValue` | — | ay |
| `0x0251` | `AskHDRCurStatus` | — | ay |
| `0x0266` | `AskHWRTCInfo` | — | ay |

<!-- END:hdr -->

**Parameter note (`*`).** The only command with more than one D-Bus argument
is `CAN.SetCANFixedMicomRecv` (`ay, i, i, i`). The vendor wrapper uses it as
`array:byte:idhi,idlo`, `int32:start`, `int32:size`, `int32:data`; that is
usage-derived (`*`), not a decoded MCU payload. See
`packages/battery/docs/MICOM_WHITELIST.md`.

### Signals

The service declares **124 signals**. They travel from the micom to the head
unit; a client subscribes, it does not call them. 123 carry one `ay` byte
array; `ENG.NotifyMPAutoModeTestStartEvent` carries no payload. The interface,
signal name and `Out` type below come from the embedded interface XML and moc
metadata.

The two right-hand columns are **inferred**. `*` marks a statement that is not
decoded from the micom protocol: the meaning comes from the signal name and
the surrounding interface, and the example is a well-formed guess, not a
captured value. Capture real payloads with `dbus-monitor` while the signal
fires.

<!-- BEGIN:signals -->

| Interface | Signal | Out | Meaning (inferred)* | Example payload (inferred)* |
|---|---|---|---|---|
| `ihu` | `NotifyCANNodeStatusEvent` | ay | CAN node online state* | `array:byte:0x01`* |
| `ihu` | `NotifyDiagAdvancedSecurityKeyEvent` | ay | diagnostic security key* | — |
| `ihu` | `NotifyDiagBTInspectionEndEvent` | ay | Bluetooth inspection end* | — |
| `ihu` | `NotifyDiagBTInspectionStartEvent` | ay | Bluetooth inspection start* | — |
| `ihu` | `NotifyDiagSessionInfoEvent` | ay | diagnostic session info* | — |
| `ihu` | `NotifyDMBCASIDSetEvent` | ay | DMB CAS ID* | — |
| `ihu` | `NotifyETCCANSleepEvent` | ay | CAN sleep state* | `array:byte:0x01`* |
| `ihu` | `NotifyETCCarIFStatusEvent` | ay | vehicle-interface status* | `array:byte:0x01`* |
| `ihu` | `NotifyETCDayNightStatusEvent` | ay | day/night state* | `array:byte:0x01`* |
| `ihu` | `NotifyEtcDCOffSetStatusEvent` | ay | audio DC offset status* | `array:byte:0x01`* |
| `ihu` | `NotifyETCILLPWMStatusEvent` | ay | illumination PWM value* | — |
| `ihu` | `NotifyEtcLcd2SensorNotSupportPwmControl` | ay | LCD2 PWM unsupported* | `array:byte:0x01`* |
| `ihu` | `NotifyEtcLcd2SensorSupportPwmControl` | ay | LCD2 PWM supported* | `array:byte:0x01`* |
| `ihu` | `NotifyETCLCDSetInfoEvent` | ay | LCD setting* | — |
| `ihu` | `NotifyETCNaviSCCMsgTriggerEvent` | ay | navigation-to-SCC trigger* | — |
| `ihu` | `NotifyETCPeriodicAVNStatusEvent` | ay | periodic AVN status* | — |
| `ihu` | `NotifyETCPopupEvent` | ay | popup request* | — |
| `ihu` | `NotifyETCPowerKeyEvent` | ay | power-key event* | `array:byte:0x01`* |
| `ihu` | `NotifyETCSleepRequest` | ay | sleep request* | `array:byte:0x01`* |
| `ihu` | `NotifyFactoryModeInfoByFScan` | ay | factory-mode F-scan info* | — |
| `ihu` | `NotifyHUDAbsentStatusEvent` | ay | HUD present/absent* | `array:byte:0x01`* |
| `ihu` | `NotifyInfoFaultCodeStatusEvent` | ay | fault-code status* | `array:byte:0x01`* |
| `ihu` | `NotifyMonitorPowerEvent` | ay | monitor power state* | `array:byte:0x01`* |
| `ihu` | `NotifyMPCANTestEvent` | ay | media-player CAN test event* | — |
| `ihu` | `NotifyRearMonitorStatusEvent` | ay | rear monitor state* | `array:byte:0x01`* |
| `ihu` | `NotifyRecoveryModeEvent` | ay | recovery-mode state* | `array:byte:0x01`* |
| `ihu` | `NotifyScheduledUpdateEvent` | ay | scheduled update state* | `array:byte:0x01`* |
| `ihu` | `NotifySerialNumberSetEvent` | ay | serial number set* | — |
| `ihu` | `NotifySpeedValueEvent` | ay | vehicle speed* | `array:byte:0x00`* |
| `ihu` | `NotifySystemHashDataEvent` | ay | system hash data* | — |
| `ihu` | `NotifySYSVariantOptionSetEvent` | ay | variant option set* | — |
| `ihu` | `NotifyTraceDateSetEvent` | ay | trace date* | — |
| `ihu` | `NotifyUpdateTouchICControl` | ay | touch-IC update status* | `array:byte:0x01`* |
| `ihu` | `NotifyVehicleOtaSessionInfoEvent` | ay | OTA session information* | — |
| `can` | `NotifyBCANEachMsg` | ay | body-CAN frame* | — |
| `can` | `NotifyCANTPMicomSend` | ay | CAN-TP send* | — |
| `can` | `NotifyCANTPMicomSendException` | ay | CAN-TP send exception* | — |
| `can` | `NotifyCANTPRECVMsg` | ay | CAN-TP receive* | — |
| `can` | `NotifyEDTStatusEvent` | ay | EDT status* | `array:byte:0x01`* |
| `can` | `NotifyExtMMCanRxEvent` | ay | external MM-CAN frame* | — |
| `can` | `NotifyICANEachMsg` | ay | I-CAN frame* | — |
| `can` | `NotifyICANRxExtMsg` | ay | external I-CAN frame* | — |
| `can` | `NotifyKeyCCPEvent` | ay | CCP key event* | `array:byte:0x01`* |
| `can` | `NotifyKeyFrontPannelEvent` | ay | front-panel key event* | `array:byte:0x01`* |
| `can` | `NotifyKeyFrontSpecialEvent` | ay | special front-panel key event* | `array:byte:0x01`* |
| `can` | `NotifyKEYPressCounterEvent` | ay | key-press counter* | `array:byte:0x01`* |
| `can` | `NotifyKeyREAREvent` | ay | rear key event* | `array:byte:0x01`* |
| `can` | `NotifyKeyRRCEvent` | ay | RRC key event* | `array:byte:0x01`* |
| `can` | `NotifyKeySWRCEvent` | ay | steering-wheel key event* | `array:byte:0x01`* |
| `can` | `NotifyMCANEachMsg` | ay | M-CAN frame* | — |
| `eng` | `NotifyAFTraceLogResponse` | ay | AF trace log* | — |
| `eng` | `NotifyAMResponse` | ay | AM tuner response* | — |
| `eng` | `NotifyANTConnectivityStatusInfoResponse` | ay | antenna connectivity status* | `array:byte:0x01`* |
| `eng` | `NotifyEngAirConditionKeyTest` | ay | A/C key test* | — |
| `eng` | `NotifyFMNewInfoResponse` | ay | FM new information* | — |
| `eng` | `NotifyFMResponse` | ay | FM tuner response* | — |
| `eng` | `NotifyHDAFResponse` | ay | HD AF response* | — |
| `eng` | `NotifyHDRConfigInfoAdvance` | ay | HD configuration, advanced* | — |
| `eng` | `NotifyHDRConfigInfoBasic` | ay | HD configuration, basic* | — |
| `eng` | `NotifyHDTunerStatusResponse` | ay | HD tuner status* | — |
| `eng` | `NotifyMPAutoModeTestMsgEvent` | ay | media-player auto-mode test message* | — |
| `eng` | `NotifyMPAutoModeTestStartEvent` | — | media-player auto-mode test start* | — |
| `eng` | `NotifyMsgSelfEmergencyGO` | ay | emergency self-programming start* | `array:byte:0x01`* |
| `eng` | `NotifyRDSAFLogResponse` | ay | RDS AF log* | — |
| `eng` | `NotifySaturnStatusResponse` | ay | Saturn tuner status* | — |
| `eng` | `NotifySndAResponse` | ay | sound processor A response* | — |
| `eng` | `NotifySndBResponse` | ay | sound processor B response* | — |
| `eng` | `NotifyTMCResponse` | ay | TMC response* | — |
| `eng` | `NotifyTunerStatusResponse` | ay | tuner status* | — |
| `tmu` | `NotifyTMUBCANCARIFEvent` | ay | body-CAN vehicle-interface event* | — |
| `tmu` | `NotifyTMUDIAGUROBOTConnectEvent` | ay | diagnostic robot connection* | — |
| `tmu` | `NotifyTMUDIAGVINInformEvent` | ay | VIN reported for diagnostics* | — |
| `tmu` | `NotifyTMUFuelConsumptionEvent` | ay | fuel consumption data* | — |
| `tmu` | `NotifyTMUKeyPressEvent` | ay | key press via TMU* | `array:byte:0x01`* |
| `tmu` | `NotifyTMUMCANStatusEvent` | ay | M-CAN status* | `array:byte:0x01`* |
| `tmu` | `NotifyTMUPanicEvent` | ay | panic/alarm state* | `array:byte:0x01`* |
| `tmu` | `NotifyTMUPowerConditionEvent` | ay | TMU power condition* | `array:byte:0x01`* |
| `tmu` | `NotifyTMURMKEYEvent` | ay | remote-key button* | `array:byte:0x01`* |
| `tmu` | `NotifyTMURMKeyStuckEvent` | ay | stuck remote key* | `array:byte:0x01`* |
| `tmu` | `NotifyTMUServiceAlarmEvent` | ay | telematics service alarm* | — |
| `tmu` | `NotifyTMUServiceBootStatusEvent` | ay | TMU service boot status* | `array:byte:0x01`* |
| `tmu` | `NotifyTMUVCRMResponseEvent` | ay | VCRM response* | — |
| `tmu` | `NotifyTMUVRMResponseEvent` | ay | VRM response* | — |
| `tmu` | `NotifyTMUVRMResponseexceptionEvent` | ay | VRM response exception* | — |
| `audio` | `NotifyAudioMainCHChange` | ay | new main audio channel/source* | `array:byte:0x01`* |
| `audio` | `NotifyAudioMixingCHChange` | ay | new mixing channel (navigation/voice overlay)* | `array:byte:0x01`* |
| `audio` | `NotifyAudioSetupControl` | ay | changed sound setting* | — |
| `audio` | `NotifyAudioVolumeStep` | ay | current volume step* | `array:byte:0x01`* |
| `audio` | `NotifyAudioVolumeStepChangeEvent` | ay | volume step delta* | `array:byte:0x01`* |
| `audio` | `NotifySetAudioVolume` | ay | applied volume value* | `array:byte:0x01`* |
| `audio` | `NotifySoundMICStatus` | ay | microphone on/off status* | `array:byte:0x01`* |
| `radio` | `NotifyAMRefreshEvent` | ay | AM refresh result* | — |
| `radio` | `NotifyITMNoSignalStatusChange` | ay | no-signal state* | `array:byte:0x01`* |
| `radio` | `NotifyRadioBSMPreset` | ay | BSM/autostore result* | — |
| `radio` | `NotifyRadioFreqUpdate` | ay | tuned frequency* | — |
| `radio` | `NotifyRadioPresetDelete` | ay | deleted preset index* | — |
| `radio` | `NotifyRadioSearchFinish` | ay | search result/station* | — |
| `radio` | `NotifyRefreshFinishEvent` | ay | refresh completion* | — |
| `radio` | `NotifyStationListUpdate` | ay | station list data* | — |
| `radio` | `NotifyTPEGSearchFinish` | ay | TPEG search result* | — |
| `radio` | `NotifyTPEGUpdate` | ay | TPEG traffic data* | — |
| `radio` | `NotifyTuneStressTest` | ay | stress-test tune result* | — |
| `tmc` | `NotifyDABToFMCoChannelDetect` | ay | DAB/FM co-channel detection* | — |
| `tmc` | `NotifyDABToFMPICodeEvent` | ay | DAB-to-FM PI code* | — |
| `tmc` | `NotifyDABToFMQuality` | ay | DAB/FM quality comparison* | — |
| `tmc` | `NotifyRDSInfoFinishEvent` | ay | RDS information end* | — |
| `tmc` | `NotifyRDSInfoStartEvent` | ay | RDS information start* | — |
| `tmc` | `NotifyRDSRTPlusUpdateEvent` | ay | RT+ data* | — |
| `tmc` | `NotifyRDSStationListUpdate` | ay | RDS station list* | — |
| `tmc` | `NotifyRDSTextUpdateEvent` | ay | RDS radio text* | — |
| `tmc` | `NotifyRDSTPLossEvent` | ay | TP-flag loss* | `array:byte:0x01`* |
| `tmc` | `NotifyTMCInfoUpdateEvent` | ay | TMC traffic information* | — |
| `hdr` | `NotifyFactoryResetCompleteEvent` | ay | factory reset complete* | `array:byte:0x01`* |
| `hdr` | `NotifyHDRAECommand` | ay | HD AE command/data* | — |
| `hdr` | `NotifyHDRAEDataUpdate` | ay | HD AE data update* | — |
| `hdr` | `NotifyHDRCommercialMessage` | ay | commercial message* | — |
| `hdr` | `NotifyHDRCommonMessage` | ay | general HD message* | — |
| `hdr` | `NotifyHDRiTuneStatus` | ay | iTunes tagging status* | `array:byte:0x01`* |
| `hdr` | `NotifyHDROnOffStatusEvent` | ay | HD on/off state* | `array:byte:0x01`* |
| `hdr` | `NotifyHDRPSDData` | ay | PSD title/artist* | — |
| `hdr` | `NotifyHDRSISData` | ay | SIS station name/info* | — |
| `hdr` | `NotifyHDRStatusInfo` | ay | HD status and signal strength* | — |
| `hdr` | `NotifyRBDSDataUpdateEvent` | ay | RBDS data* | — |
| `hdr` | `NotifyTaggingTokenData` | ay | tagging token* | — |

<!-- END:signals -->

Two moc signal implementations are outside the 124 declared entries:
`CAN.NotifyCanDTCFinishEvent` (the XML declares that name as a method) and
`CAN.NotifyMPCANTestEvent` (a same-named signal in the `CAN` class, also
declared under `IHU`). They are not counted in the table.

---

## 7. Methods that emit no micom frame

Eleven service slots produce no UART command; they are the exact complement of
the 242 derived opcodes, which is a useful completeness check on the join.

**Eight are local test hooks** — they manufacture a D-Bus signal without any
hardware involvement, which is why they are listed:

| Interface | Method | Why it has no opcode |
|---|---|---|
| `can` | `TestNotifyKeyFrontPannelEvent` | emits a `NotifyKey…Event` signal locally |
| `can` | `TestNotifyKeyCCPEvent` | " |
| `can` | `TestNotifyKeyRRCEvent` | " |
| `can` | `TestNotifyKeySWRCEvent` | " |
| `can` | `TestNotifyKeyREAREvent` | " |
| `can` | `TestNotifyKeyFrontSpecialEvent` | " |
| `eng` | `TestNotifyMPAutoModeTestEvent` | " |
| `tmu` | `TestNotifyTMURMKEYEvent` | " |

**Three are withheld.** They are implemented, absent from the interface XML
and from every `…Adaptor` class, and referenced by no shipped binary. AGENTS.md
rule 6 keeps undeclared, unused primitives out of published material, so they
are excluded from every output `tools/micom/extract_commands.py` produces —
named there only by opaque `(class, index)` pairs in `WITHHELD_SLOTS` — and are
not described here. They still count toward the slot totals above, so a partial
join cannot hide them. If you are auditing this service rather than using it,
enumerate the slots the interface XML does not declare; this document will not
do that for you.

<!-- BEGIN:nonuart -->

| Interface | Method | In | Out |
|---|---|---|---|
| `can` | `TestNotifyKeyFrontPannelEvent` | ay | — |
| `can` | `TestNotifyKeyCCPEvent` | ay | — |
| `can` | `TestNotifyKeyRRCEvent` | ay | — |
| `can` | `TestNotifyKeySWRCEvent` | ay | — |
| `can` | `TestNotifyKeyREAREvent` | ay | — |
| `can` | `TestNotifyKeyFrontSpecialEvent` | ay | — |
| `eng` | `TestNotifyMPAutoModeTestEvent` | ay | — |
| `tmu` | `TestNotifyTMURMKEYEvent` | ay | — |

<!-- END:nonuart -->

---

## 8. Who calls what

Across the full 2,241-ELF export, **56 binaries mention `com.lge.car.micom`**;
**53 of them also reference a recognisable interface or method name**. Sorted by
how much of the command surface they touch:

| Binary | Interfaces | Methods |
|---|---|---:|
| `app/bin/micommanager_userspace` | all 8 | 242 |
| `app/bin/AppSetup` | all 8 | 141 |
| `app/bin/AppHomeScreen` | 7 | 116 |
| `app/bin/LocationManager` | can, ihu | 98 |
| `app/bin/AppEngineerMode` | audio, eng, hdr | 91 |
| `app/bin/AppUpgrade` | audio, can, ihu, tmu | 91 |
| `embedded-7c64.elf` (AppUpgrade engine) | audio, can, ihu, tmu | 80 |
| `embedded-5721c8.elf` (AppScomoDM engine) | can, ihu, tmu | 77 |
| `app/bin/AppMediaPlayer` | audio, can, hdr, ihu | 55 |
| `app/bin/micom_fw_updater` | audio, eng, ihu | 47 |
| `app/bin/AppVH` | audio, can, ihu, tmu | 29 |
| `app/bin/AppMPAutoTest` | 6 | 28 |
| `Bin/AppNavi` | audio, can, ihu, tmc | 27 |
| `app/bin/BluetoothHMI` | audio, can | 22 |
| `app/share/Statusbar/lib/libStatusbar.so.1.0.0` | audio, can, ihu | 11 |
| `app/bin/AppEURadio` | audio | 9 |
| `app/bin/AppStandbyClock` | audio, can, eng, ihu | 9 |
| `app/bin/UserProfileManager` | ihu | 8 |
| `app/bin/AppLauncher` | audio, eng, ihu | 7 |
| `usr/lib/audioManager/routing/libPluginRoutingInterfaceDbus.so` | audio | 7 |
| `app/bin/AppOSDManager` | audio, can, ihu | 5 |
| `usr/sbin/ofonod` | audio, can, ihu, tmu | 5 |
| `app/bin/AppCamera` | audio, can | 4 |
| `usr/bin/TimeManager` | can, hdr, ihu | 4 |
| `app/bin/ApplicationManager` | ihu | 3 |
| `Bin/CANService` | can | 2 |
| `app/bin/NotificationManager` | audio, can, ihu | 2 |
| `app/bin/TmsGuiApp` | audio, can, tmu | 2 |
| `usr/lib/qt5/qml/Keyboard/libKeyboard.so` | audio, ihu | 2 |
| `app/bin/adp_fw_updater{,_FUMO,_SCOMO}` (3 binaries) | can, ihu | 2 each |
| `app/bin/amp_fw_updater{,_FUMO,_SCOMO}` (3 binaries) | can, ihu | 2 each |
| `app/bin/mkbd_fw_updater{,_FUMO,_SCOMO}` (3 binaries) | can, ihu | 2 each |
| `app/bin/vess_fw_updater{,_FUMO,_SCOMO}` (3 binaries) | can, ihu | 2 each |
| `app/bin/MOFManager` | can | 1 |
| `app/bin/ccprrc_fw_updater` | can | 1 |
| `app/bin/hdradio_fw_updater` | eng | 1 |
| `app/bin/jf_fw_updater` | can | 1 |
| `app/bin/lightmediascannerd` | ihu | 1 |
| `app/bin/supplicantmanager` | ihu, tmu | 1 |
| `app/lib/liblightmediascanner.so.0.5.1` | ihu | 1 |
| `app/plugins/CalendarWidget/libCalendarWidget.so.1.0.0` | ihu | 1 |
| `app/share/AssistantWindow/lib/libAssistantWindow.so.1.0.0` | audio, ihu | 1 |
| `usr/bin/watchdogd` | can | 1 |
| `app/bin/AppAutoKey` | can | 0 |
| `app/bin/btsco` | audio | 0 |

(45 rows; the four brace rows expand to 12 binaries, so 53 in total.)

The counts come from a full-export SQLite index with two tables —
`binaries(id, path)` and `strings(binary_id, string)` — built over the same
2,241-ELF Ghidra export. That index is not produced by this repository; treat
it as an external artefact. Regenerate the table with:

```sh
python3 tools/micom/extract_commands.py callers \
    --db work/sqlite/ioniq5_260128.db --table work/micom/commands.json
```

If you do not have that index, the table is reproducible by hand: run `strings`
over each ELF and check for `com.lge.car.micom` plus any of the 242 method
names.

Two things fall out of this:

- **`AppSetup` and `AppHomeScreen` are the heaviest consumers**, which fits:
  settings and the home surface are where power, display and vehicle state are
  read and written.
- **Every `*_fw_updater` binary touches the CAN or ENG interface.** Sixteen of
  them: the micom's own flasher, three variants each of ADP, AMP, MKBD and
  VESS (FUMO/SCOMO/plain), plus JF, HDR radio and CCPRRC. Each pushes its own
  sub-firmware and each borrows the micom path to do it. `micom_fw_updater` is
  the one that flashes the micom itself.

The `0`-method rows still reference the service: `AppAutoKey` and `btsco` carry
the destination string without a recognised method name, which usually means
they call a method we did not match, or they only qualify a name for the bus.

"Referenced" means the method-name string appears in the binary. It does not
prove the call executes, and it cannot distinguish a live call from a string
retained in a table.

---

## 9. The micom flashing path

`/app/bin/micom_fw_updater` is the in-car flasher. It is a Qt program that
drives `com.lge.car.micom.eng` over the same D-Bus service.

Inputs, from its string table:

```
/update/usb/micom/bin        directory of per-variant images
Checksum.txt                "NAME.DV.<date>.<rev>.BIN  CRC32=0x…"
```

Flow, reconstructed from `bin_7182ad17ef4d5cd5`:

1. Read `Checksum.txt`, parse each `CRC32=0x…` claim.
2. `AskSysVersion` (`0x0203`, ihu) and `AskAMPSupportInfo` (`0x0217`, audio)
   to identify the variant and confirm the bus is alive. That it reaches into
   the *audio* interface for an amplifier-support query is a good illustration
   of how the interface split reflects the vendor's internal organigram rather
   than the hardware.
3. `SetMsgSelfErase` (`0x0603`) — erase the micom's application area.
4. `SetMsgSelfProgram` (`0x0604`) — program a block.
5. `SetMsgSelfProgEnd` (`0x0605`) — finish.
6. `SetMsgSelfReset` (`0x0606`) — restart the micom into the new image.

Before flashing it computes a checksum over the image and compares it with the
`Checksum.txt` value, logging `FirmCRC Check : binary %d / Cksum.txt %d`.
**That checksum is not zlib CRC-32.** It is the MSB-first CRC-32/MPEG-2
recurrence (polynomial `0x04C11DB7`, init `0xFFFFFFFF`, no reflect, **no final
XOR**). `tools/micom/mcu_image.py crc` reproduces it; `docs/16` covers the
container.

The strings also include `/update/fota/micom_emergency_recovered` and
`micom emergency rollback completed = %s`, which **suggest** a recovery marker
and a rollback path. Whether that path actually runs, and whether it can
recover a failed flash, is not established — no recovery was observed or
executed.

`SetMsgSelfErase`/`Program`/`ProgEnd`/`Reset` are four of the 25 `0x06xx`
commands. The rest are engineering and factory paths: `SetMsgSelfGO`,
`SetMsgSelfEPV`, `SetAMPUpgrade`, `SetMPAutoTestModeCtrl`, `SetKeyPWMTest`,
`SetSaturnGpioCtrl` and so on. No **other** shipped flasher drives them, but
`micom_fw_updater` itself retains the strings for `SetMsgSelfGO`,
`SetMsgSelfEPV` and `SetMPAutoTestModeCtrl`, so treat "unused" as unproven in
the same way §8 treats "referenced".

---

## 10. Change history: 2021.08.17 → 2026.01.28

The micom service is one of the few components we can diff across five years of
firmware, because its whole interface is encoded in Qt meta-object tables and
those can be read out of any `micommanager_userspace` without a decompiler:

```sh
python3 tools/micom/extract_commands.py diff \
    --old micommanager_userspace.210817 \
    --new micommanager_userspace.260128
```

Both builds are `x86-64`, stripped, and expose the same 19 meta-objects and the
same 8 interfaces at the same object paths. The 2021 build is 444,544 B; the
2026 build is 542,856 B. Both hashes are pinned (§2), so the diff cannot run
against a different build.

**233 → 253 service-class slots: 22 methods added and 2 removed (net +20),
plus 5 new signals, with 0 arity changes.** Twenty of the 22 added methods are
listed below; the other two are withheld under rule 6 (§7).

Added methods:

| Interface | Method | Opcode (2026) |
|---|---|---|
| `can` | `SetVehicleInfoMsg` | `0x019b` |
| `ihu` | `AskDebuggingMode` | `0x020c` |
| `ihu` | `AskInfoFaultCodeStatus` | `0x0283` |
| `ihu` | `AskMicomCRC` | `0x020d` |
| `ihu` | `AskNaviOutermostRegion` | `0x02c3` |
| `ihu` | `AskRxswinInfo` | `0x020e` |
| `ihu` | `AskTuningVersion` | `0x0204` |
| `ihu` | `AskUSMMessageTimeout` | `0x0285` |
| `ihu` | `AskVehicleTypeAndEnergySource` | `0x020b` |
| `ihu` | `SetControlOtaUpgradeMsg` | `0x01c0` |
| `ihu` | `SetControlVehicleOtaUpgrade` | `0x01c2` |
| `ihu` | `SetDiagAdvanceSeedData` | `0x01bb` |
| `ihu` | `SetEtcNaviOutermostRegion` | `0x01c3` |
| `ihu` | `SetSysDebuggingMode` | `0x0107` |
| `ihu` | `SetSysDspTypeReset` | `0x0106` |
| `radio` | `AskRadioOffMode` | `0x0297` |
| `radio` | `SetRadioMute` | `0x012b` |
| `radio` | `SetRadioOffMode` | `0x019e` |
| `tmu` | `SetTmuAvoidExtDiagByVcrm` | `0x014d` |
| `tmu` | `SetTmuPowerConditionRequest` | `0x014f` |

Added signals: `CAN.NotifyCANTPMicomSendException`,
`IHU.NotifyDiagAdvancedSecurityKeyEvent`, `IHU.NotifyInfoFaultCodeStatusEvent`,
`IHU.NotifySystemHashDataEvent`, `IHU.NotifyVehicleOtaSessionInfoEvent`.

Removed: `IHU.SetETCSWVersionMsg`, `RADIO.SetSabreDirectTune`.

The pattern is informative. The 2021 build predates three things that are
plainly visible in the 2026 additions:

- **over-the-air update plumbing** (`SetControlOtaUpgradeMsg`,
  `SetControlVehicleOtaUpgrade`, and the `NotifyVehicleOtaSessionInfoEvent`
  signal, alongside the pre-existing `SetScheduledUpdateInfo`) — the micom
  gained a role in vehicle OTA;
- **diagnostic hardening** (`SetDiagAdvanceSeedData`, `AskMicomCRC`,
  `NotifyDiagAdvancedSecurityKeyEvent`) — seed/key diagnostic security;
- **debuggability** (`AskDebuggingMode`, `SetSysDebuggingMode`,
  `AskInfoFaultCodeStatus`).

The two removals look like consolidation rather than capability loss:
`SetETCSWVersionMsg` has no `Ask…` counterpart left, and `SetSabreDirectTune`
was a direct-tune shortcut that the `SetRadioMute` / `SetRadioOffMode` /
`AskRadioOffMode` trio supersedes.

This answers, for this component, the question "did the newest firmware change
the micom command set?" — the service grew by 20 net methods plus 5 signals,
and the growth is concentrated in OTA and diagnostics rather than in vehicle
control.

---

## 11. Open questions

Recorded so nobody re-derives them as if they were settled:

- **`SetETCOptionSet` is opcode `0x9999`.** The literal is real — the slot at
  `FUN_001266d0` calls `FUN_0010f400(manager, 0x9999, …)` — but `0x9999` sits
  outside every other family and looks like a sentinel or an unfinished
  feature. We do not know whether the micom accepts it.
- **The `0x04xx`/`0x07xx`/`0x08xx` CAN grouping** is unexplained (§5). It may
  be a bus-type field rather than an opcode number.
- **Shared opcodes.** Four opcode values serve two methods each:
  `0x0182` = `CAN.SetPGSInitialRequest` + `CAN.SetGreenCarInitialRequest`;
  `0x019b` = `CAN.SetVehicleInfoMsg` + `IHU.SetETCMPAUTOModeTestMsg`;
  `0x02b2` = `IHU.AskSerialNumber` + `IHU.AskSerialNumber2`;
  `0x0400` = `CAN.SetCANMicomRecv` + `CAN.SetCANFixedMicomRecv`.
  Presumably they differ only in payload shape; the payload semantics are not
  decoded.
- **Payload semantics are decoded only for the CAN bridge.** For every other
  command we know the opcode, the name and the D-Bus argument types, but not
  what the bytes mean to the MCU. The two `ay` blobs that *are* understood are
  in `packages/battery/docs/MICOM_WHITELIST.md`.
- **Signal payload bytes are not decoded.** §6 lists the 124 declared signals,
  their `Out` types and an inferred meaning for each; the byte layout of the
  `ay` payload is unknown. The `*` marks in that table are inference, not
  protocol facts.
- **The `…Adaptor` classes** are not exact mirrors. `IHUAdaptor` has 106 slots
  against `IHU`'s 107, and `ENGAdaptor` 26 against `ENG`'s 28 — the adaptors
  omit exactly the three undeclared slots described in §7. That is also why
  those slots are not reachable through the published interface.
- **`CAN.NotifyCanDTCFinishEvent`** is declared but unimplemented (§3). Whether
  it existed in 2021 and was dropped, or was never built, is not checked.

### A cross-reference correction

`packages/battery/docs/MICOM_WHITELIST.md` names `FUN_00132450` as the
`SetCANFixedMicomRecv` handler. The metacall join puts `SetCANMicomRecv`
(1 argument) at `FUN_00132450` and `SetCANFixedMicomRecv` (4 arguments) at
`FUN_00133560`. Both slots call the transmit wrapper with **`0x0400`**, so the
whitelist note's operational conclusion — that the fixed four-argument form and
the raw form are the same micom command — is correct; only the function
attribution differs.

---

## 12. References

Decompiled export `bin_e5f040ffc41cc980` (`micommanager_userspace`):

| Address | What |
|---|---|
| `FUN_00109a30` | opcode assembly: `(hi << 8) | lo` |
| `FUN_0010de00` | transmit: `AA`, opcode, length, payload, XOR |
| `FUN_00111060` | receive: length check, opcode extraction, observer dispatch |
| `FUN_0010ec60` | send with no arguments (138 call sites) |
| `FUN_0010f400` | send with D-Bus arguments (105 call sites) |
| `FUN_0010efc0` | query path; `opcode | 0x8000` pending-request key |
| `FUN_0010b1e0` | CAN-sender basename whitelist |
| `FUN_00107d10` | service registration: the eight object paths |
| `FUN_00144560` | `IHU::qt_static_metacall` (141 cases) |
| `FUN_00140f10` | `IHUAdaptor::qt_static_metacall` (140 cases) |

Decompiled export `bin_7182ad17ef4d5cd5` (`micom_fw_updater`): the flash state
machine and the CRC-32/MPEG-2 loop.

Tools:

- `tools/micom/extract_commands.py` — moc parse, XML signatures, signal
  declarations, opcode join, version diff, doc-table render, caller index.
- `tools/micom/mcu_image.py` — MCU container layout and the updater's CRC.

Related:

- `docs/16` — the micom MCU images themselves.
- `packages/battery/docs/MICOM_WHITELIST.md` — the CAN sender whitelist, and
  the only place where actual micom payload bytes are documented.
- `docs/09-porting.md` — the hash gates this tool participates in.
