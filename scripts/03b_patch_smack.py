#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Build-5 / stage-4 SMACK flag patch for /etc/smack_setup.sh.

TARGET: Hyundai Ioniq 5 Gen5W_L (Linux), EU,
        firmware NE_E.EUR.S5W_L.001.001.260128.

The vendor script already sets `bringup-switch=1` (permissive access checks)
on non-UNECE cars, but it also writes `onlycap=Privileged`.  That second
write removes CAP_MAC_ADMIN from every process in the `_` label, which is
why the WiFi payload's `setfattr -n security.SMACK64` failed and DeSmack
had to be run from a Privileged trampoline.

This patch adds `/etc/smack_permissive` as the flag and changes the two
places where the vendor writes `onlycap` so that, when the flag is present,
the write is `-` (clear) instead.  It must happen at the vendor's write
site: once onlycap is `Privileged`, a `_` process can no longer clear it.

The flag also forces `bringup-switch=1` regardless of `optUNECE`.

The generated files are build artifacts (work/smack/etc); the repo does
not commit the vendor smack_setup.sh.

Usage:
  03b_patch_smack.py --stock-tar work/stock-plain.tar.gz \
      --out work/smack/etc
"""
import argparse
import difflib
import os
import subprocess
import sys

STOCK_MEMBER = "./etc/smack_setup.sh"
FLAG_NAME = "smack_permissive"
FLAG_TEXT = """Stage-4 SMACK flag.

When this file is present, /etc/smack_setup.sh:
  - sets /sys/fs/smackfs/bringup-switch to 1 (permissive access checks), and
  - leaves /sys/fs/smackfs/onlycap empty, so CAP_MAC_ADMIN is not restricted
    to the Privileged label and payloads in the `_` label can write SMACK
    labels (setfattr/chsmack).

Remove this file and reflash to restore the vendor onlycap policy.
"""

REPLACEMENTS = [
    (
        "nosmack onlycap",
        "\techo \"Privileged\" > /sys/fs/smackfs/onlycap\n\texit 5\n",
        "\tif [ -f /etc/smack_permissive ]; then\n"
        "\t\tprintf '%s' '-' > /sys/fs/smackfs/onlycap\n"
        "\t\techo \"[SMACK] permissive flag: onlycap cleared (nosmack)\"\n"
        "\telse\n"
        "\t\techo \"Privileged\" > /sys/fs/smackfs/onlycap\n"
        "\tfi\n"
        "\texit 5\n",
    ),
    (
        "nosmack bringup condition",
        "        if [ $CHECK_UNECE1 == \"0\" ]; then\n",
        "        if [ $CHECK_UNECE1 == \"0\" ] || [ -f /etc/smack_permissive ]; then\n",
    ),
    (
        "check_label_final bringup condition",
        "        if [ $CHECK_UNECE2 == \"0\" ]; then\n",
        "        if [ $CHECK_UNECE2 == \"0\" ] || [ -f /etc/smack_permissive ]; then\n",
    ),
    (
        "check_label_final onlycap",
        "                ONLYCAP_FILE=/sys/fs/smackfs/onlycap\n"
        "                echo \"Privileged\" >> $ONLYCAP_FILE\n",
        "                ONLYCAP_FILE=/sys/fs/smackfs/onlycap\n"
        "                if [ -f /etc/smack_permissive ]; then\n"
        "                        printf '%s' '-' > $ONLYCAP_FILE\n"
        "                        echo \"[SMACK] permissive flag: onlycap cleared\"\n"
        "                else\n"
        "                        echo \"Privileged\" >> $ONLYCAP_FILE\n"
        "                fi\n",
    ),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stock-tar", required=True)
    ap.add_argument("--out", required=True, help="output etc directory (build artifact)")
    a = ap.parse_args()

    os.makedirs(a.out, exist_ok=True)
    stock = subprocess.check_output(
        ["tar", "-xOzf", a.stock_tar, STOCK_MEMBER], text=True)
    patched = stock
    for label, old, new in REPLACEMENTS:
        n = patched.count(old)
        if n != 1:
            raise SystemExit("%s: expected exactly 1 match, got %d" % (label, n))
        patched = patched.replace(old, new, 1)
        print("patched: %s" % label)

    if "/etc/smack_permissive" not in patched:
        raise SystemExit("flag reference missing after patch")
    if "printf '%s' '-' > $ONLYCAP_FILE" not in patched:
        raise SystemExit("onlycap clear missing after patch")

    out_script = os.path.join(a.out, "smack_setup.sh")
    with open(out_script, "w", encoding="utf-8") as f:
        f.write(patched)
    os.chmod(out_script, 0o700)

    flag = os.path.join(a.out, FLAG_NAME)
    with open(flag, "w", encoding="utf-8") as f:
        f.write(FLAG_TEXT)
    os.chmod(flag, 0o644)

    # Syntax check with the target's /bin/sh grammar.
    r = subprocess.run(["/bin/sh", "-n", out_script])
    if r.returncode != 0:
        raise SystemExit("sh -n failed on %s" % out_script)

    diff = list(difflib.unified_diff(
        stock.splitlines(keepends=True), patched.splitlines(keepends=True),
        fromfile="stock/etc/smack_setup.sh", tofile="stage4/etc/smack_setup.sh"))
    sys.stdout.writelines(diff)
    print("\nwrote %s (%d bytes) and %s (%d bytes)" %
          (out_script, os.path.getsize(out_script),
           flag, os.path.getsize(flag)))


if __name__ == "__main__":
    main()
