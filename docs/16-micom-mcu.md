# 16 — The micom MCU images

TARGET: Hyundai Ioniq 5 Gen5W_L (Linux x86-64), EU region, firmware
`NE_E.EUR.S5W_L.001.001.260128`.

**Status: partially analysed.** The container format, the size classes and the
checksum are established and mechanically verifiable with
`tools/micom/mcu_image.py`. The instruction set and the code itself are **not**
decoded — §6 says exactly what is known and what is not.

This document is about the *other end* of the UART. `docs/15` describes what the
head unit asks the micom to do; this describes the firmware that does it.

Nothing in this document was executed. The images were never run, simulated or
flashed. Every claim below is static.

---

## 1. Where the images live

The update package carries a per-vehicle-variant micom image directory next to
the head-unit payloads:

```
update/micom/Checksum.txt
update/micom/<variant>/<NAME>.DV.<date>.<rev>.bin
```

Examples: `update/micom/ceev/CE_EV.DV.260120.09.RevC.bin`,
`update/micom/neev/NE_EV_K.DV.260120.89_K.RevC.bin`.

Extract that tree under your gitignored `work/` or `input/` directory. Do not
copy it into the repository root — `update/` is not in `.gitignore`, and a
stray `update/micom/` would put OEM binaries one `git add -A` away from being
committed.

`Checksum.txt` is a flat manifest of 152 entries covering every variant the
package knows about. The tree used for this analysis holds 47 variant
directories and **52 `.bin` files**.

There are **two image sizes**, and the split is exact across the 52 local files:

| Size | Count | Variants | Identification string |
|---|---:|---|---|
| 2 MiB (`0x200000`) | 46 | the bulk of the fleet | `SH850T01G255_PV103` |
| 4 MiB (`0x400000`) | 6 | the `*_K` builds: `de22myev`, `de22myhev`, `de22myphev`, `ja22my`, `neev`, `yb22my` | `SH850T01xxxxxxV213` |

The 4 MiB group is not simply a longer file: §2 shows it carries a second
2 MiB slot that the 2 MiB images do not have, and its vector table is a
different length.

---

## 2. Container layout

`tools/micom/mcu_image.py info <image>` prints the size class, validates the
header records, and prints the vector table and the used extents.

### Header records

The first `0x80` bytes are a record table, 16 bytes per record:

```
+0x00  u16 magic = 0x0780
+0x02  u16 addr
+0x04  twelve zero bytes
```

`info` checks the magic and the twelve zero bytes rather than assuming them.
On `CE_EV.DV.260120.09.RevC.bin` the populated records carry addresses
`0x0668`, `0x22d0`, `0x22b0`, `0x22a0`, `0x2290`, `0x2280` — descending, in
steps with one record skipped between entries. We do not know what they
select.

### Vector table

At `0x200` there is a table of 4-byte little-endian pointers that continues
while the value is strictly below `0x100000`:

| Entries | Images | Size class |
|---:|---:|---|
| 282 | 46 | 2 MiB |
| 358 | 6 | 4 MiB (`*_K`) |

The targets are plausible code addresses. Most entries point at a single
default stub (`0x000022e0` on `CE_EV`); the live handlers on that image are
`0x000a6f60`, `0x000a74e8`, `0x000a801c`, `0x000ea690`, `0x000ea6e8`,
`0x000ea7f0`, `0x000ea846`.

A table of in-range handler addresses at a fixed low offset, plus a shared
default stub, is what an interrupt vector table looks like. That is the
strongest evidence that this is executable content rather than a bag of data —
but it is an inference from shape, not from a decoded instruction (§6).

### Slots and halves

Every image divides into 1 MiB halves. The 2 MiB images use two of them; the
4 MiB images use four, i.e. two 2 MiB slots.

`CE_EV.DV.260120.09.RevC.bin` (2 MiB) — complete extent map at 64 KiB
granularity (sums to 2048 KiB):

```
0x000000-0x010000    64 KiB   USED     header / record table
0x010000-0x020000    64 KiB   erased
0x020000-0x100000   896 KiB   USED     code region
0x100000-0x110000    64 KiB   erased
0x110000-0x200000   960 KiB   USED     code region
```

`NE_EV_K.DV.260120.89_K.RevC.bin` (4 MiB) — complete extent map (sums to
4096 KiB):

```
0x000000-0x010000    64 KiB   USED     header / record table
0x010000-0x020000    64 KiB   erased
0x020000-0x100000   896 KiB   USED     code region
0x100000-0x1f0000   960 KiB   erased
0x1f0000-0x200000    64 KiB   USED     tail block
0x200000-0x210000    64 KiB   erased   (second slot's header block is blank)
0x210000-0x300000   960 KiB   USED     code region
0x300000-0x3f0000   960 KiB   erased
0x3f0000-0x400000    64 KiB   USED     tail block
```

The two are **not** the same shape repeated. The 4 MiB image's first 1 MiB
matches the 2 MiB image's first 1 MiB, but its second 1 MiB is mostly erased
apart from a 64 KiB tail block, and its second slot carries code in the third
1 MiB and nothing but a tail block in the fourth. What that implies about the
flash part or the update scheme is §6.

The two halves of a 2 MiB image are largely the same content. On `CE_EV`,
**72,655 bytes (6.9%) differ**, spread over 5,637 short runs. The largest runs
are 2,133 bytes at relative `0x107a` and `0x1107a`, and 1,660 bytes at `0x0000`
and `0x10000` — note the 64 KiB period between each pair.

At 4 KiB granularity the halves are 140/256 blocks identical (55%), while there
is **no** self-similarity at lags `0x40000` or `0x80000` (0/448 and 0/384). So
the image is a primary and a near-copy, not a repeating structure.

For the 4 MiB `*_K` images, slot 0 versus slot 1 differs by 78,707 bytes
(3.75%), with 392/512 identical 4 KiB blocks. That comparison is weakened by
both slots sharing large erased regions, so read it as a byte count rather than
as evidence of a copy relationship.

Whether the second half or second slot is a rollback copy, an A/B update
scheme, or a per-bank calibration area is not established.

---

## 3. The checksum

`Checksum.txt` lines look like:

```
CE_EV.DV.260120.09.REVC.BIN	CRC32=0x42F9C184
```

It looks like zlib CRC-32. It is not. A zlib `crc32` of that file yields
`0x7539987B`. The value the updater actually computes comes from
`micom_fw_updater` and is the **MSB-first CRC-32/MPEG-2 recurrence**:

```
poly      0x04C11DB7   (normal, not reflected)
init      0xFFFFFFFF
refin     false
refout    false
xorout    0x00000000   (no final XOR — this is where zlib and this differ)
width     32
check     0x0376E6E7
```

over the **whole file**, whether it is 2 MiB or 4 MiB.

That is directly visible in `bin_7182ad17ef4d5cd5`:

```c
uVar13 = 0xffffffff;
for (i = 0; i < len; i++)
    uVar13 = (uVar13 << 8) ^ table[((uVar13 >> 24) ^ data[i]) & 0xff];
```

and it is then compared against the parsed `Checksum.txt` value, logging
`FirmCRC Check : binary %d / Cksum.txt %d`.

Verified: **52 of 52** locally present images reproduce their published value,
across both size classes.

```sh
python3 tools/micom/mcu_image.py verify update/micom
```

---

## 4. Two firmware families

The part string embedded in the image splits the fleet the same way the size
does:

| Part string | Images | Size |
|---|---:|---:|
| `SH850T01G255_PV103` | 46 | 2 MiB |
| `SH850T01xxxxxxV213` | 6 | 4 MiB |

The second contains **literal `xxxxxx` where a part or version code belongs**.
Six production images carry the literal `xxxxxx` in their identification string,
a different image size and a different vector-table length, so they are a
distinct build of the micom application rather than a rename. Why the field is
blank is §6.

Right next to the identifier are strings that look like calibration/version
records, present in every image:

```
R10 CE1/1/0
R155 CE1/1/0
R156 CE1/1/0
```

`CE1/1/0` is plausible as a hardware/EEPROM revision triple. Not decoded.

---

## 5. What the image contains

`strings` finds only ~80 printable runs in 2 MiB. They cluster in two places:

1. **`~0x5100`–`0x5900`** — identity and calibration (`SH850T01G255_PV103`,
   the `R1xx CE1/1/0` records).
2. **`~0x2c800`–`0x2ca00`** — a diagnostic/interpreter vocabulary:

```
ERROR(CPU)   ENV2(%d)   ENV3(%d)   ERROR(%d)   ERROR[%d]   LKB(%d)
ERROR(CRC)   ERROR(OVF)
GetOp[%d]    ErrOp(%d)  SetOp(%d)
```

That second cluster is the interesting one. `GetOp` / `SetOp` / `ErrOp` with a
numeric operand, plus `ERROR(CPU)`, `ERROR(CRC)` and `ERROR(OVF)`, reads like
the diagnostics of a **command interpreter** — a dispatcher that fetches, sets
and errors on numbered operations. If that is what it is, it is the micom's
internal counterpart to the 242 opcodes in docs/15, and decoding it is the path
to giving those opcodes payload semantics.

Note also `LKB(%d)`, which does not obviously fit that reading.

This is a reading of the vocabulary, not a decoded interpreter.

---

## 6. What is *not* established

This is the honest boundary of the analysis. Do not build on these without
checking them.

### The instruction set is unidentified

No host disassembler target matches. `objdump` on this workstation supports only
`i386`/`x86-64`/`bpf`; there is no `v850`, `rh850`, `h8`, `sh`, `78k` or `arm`
target available to test against.

The evidence is genuinely ambiguous:

- **For "executable":** a plausible vector table of in-range handler addresses
  (§2), a `0x0780` record table whose padding validates, and low-entropy
  header/erased regions in the right places.
- **Against "plain native code":** at the sampled handler `0x000ea7f0` the bytes
  begin `ff 47 60 f9 e1 47 60 11 e6 47 60 99 fe 47 60 f1` — the two bytes
  `47 60` recur every four bytes across a long run. And the byte-class
  statistics are not flat: in the 64 KiB region at `0x40000`, positions
  `mod 4 == 0` and `mod 4 == 2` have nearly identical entropy (6.74 vs 6.73)
  and identical top bytes (`0xdf`, ~985 each), while `mod 4 == 1` and
  `mod 4 == 3` form a second pair (5.28 vs 5.31). That is a two-byte positional
  period in that region — which is not what compiled code for any common MCU
  looks like.

Note those are two *different* observations: the `0xea7f0` handler shows a
four-byte period, the `0x40000` region a two-byte one. Reproduce both with:

```sh
python3 tools/micom/mcu_image.py stats update/micom/ceev/CE_EV.DV.260120.09.RevC.bin
```

The most likely readings, in order, are: a 16-bit MCU family whose opcodes
happen to repeat, a proprietary bytecode, or lightly obfuscated code. We have
not distinguished them, and this document does not claim a family.

**What would settle it:** any one of (a) a `binutils`/Ghidra build with an
RH850 or V850 back end and a decode-plausibility score over the vector-table
handlers, (b) the micom part number from a teardown, or (c) the `GetOp` dispatch
table located and read as data.

### Other open threads

- **Header record semantics.** `{0x0780, addr}` × 8 at offset 0: unknown.
- **Why the halves differ.** 6.9% of bytes on a 2 MiB image, in 5,637 runs with
  64 KiB periodicity; 3.75% between the two slots of a 4 MiB image. Whether the
  second copy is rollback, A/B update or calibration is not established.
- **The `xxxxxx` placeholder.** Six shipped images carry it. Either an upstream
  template was never filled or the field is deliberately blank in those builds.
- **Erase/program granularity.** `micom_fw_updater` drives `SetMsgSelfErase` /
  `SetMsgSelfProgram` (docs/15 §9) but the block size it uses is not decoded.
- **The 2 MiB / 4 MiB split.** The 4 MiB builds carry a second slot with a
  blanked header. That is consistent with a larger flash part, but the part
  numbers are not known.

---

## 7. Tools

```sh
# container layout, header validation, vector table, used extents, slot compare
python3 tools/micom/mcu_image.py info update/micom/ceev/CE_EV.DV.260120.09.RevC.bin

# the updater's checksum for one or more images
python3 tools/micom/mcu_image.py crc update/micom/*/*.bin

# verify a whole tree against Checksum.txt (rejects wrong sizes, fails if it
# checked nothing, and exits non-zero on any mismatch)
python3 tools/micom/mcu_image.py verify update/micom

# reproduce the entropy, run and block-identity numbers quoted here
# (the 4 MiB slot-0 vs slot-1 byte total comes from `info`, not this command)
python3 tools/micom/mcu_image.py stats update/micom/ceev/CE_EV.DV.260120.09.RevC.bin
```

The tool is read-only: it never writes to, patches or repackages an image.

---

## 8. References

- `docs/15` — the host-side command surface, including the
  `SetMsgSelfErase`/`Program`/`ProgEnd`/`Reset` flashing path.
- Decompiled export `bin_7182ad17ef4d5cd5` (`micom_fw_updater`): the CRC loop
  and the `Checksum.txt` parser.
- `tools/micom/mcu_image.py` — the checks in this document, reproducibly.
