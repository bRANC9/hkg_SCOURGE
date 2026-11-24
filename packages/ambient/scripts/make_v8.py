#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Synthesise a crossplane V8 idle loop — our own asset, no sample library.

Firing model: 4 pulses per crank revolution (8 cylinders over 720 degrees),
uneven within the revolution (crossplane V8 burble), each pulse a short burst
excited through two low resonances (80 Hz block rumble, 160 Hz body), plus a
little intake noise.  The length is an exact number of revolutions, so the loop
points match and aplay can repeat it without a click.

    python3 make_v8.py out.wav [seconds] [rpm]

Output: 16-bit mono 44100 Hz WAV.
"""
import math
import random
import struct
import sys

RATE = 44100


def build(seconds=4.0, rpm=600.0):
    rev_hz = rpm / 60.0
    n = int(RATE * seconds)
    # integer revolutions so the loop is seamless
    revs = max(1, round(rev_hz * seconds))
    rev_hz = revs / seconds
    # crossplane firing phases in a revolution (degrees -> fraction)
    phases = [0.0, 0.375, 0.5, 0.875]
    amps = [1.0, 0.72, 0.9, 0.62]
    out = [0.0] * n
    rng = random.Random(8)
    # two block resonators as one-pole band-passes
    lp1 = lp2 = 0.0
    a1 = math.exp(-2 * math.pi * 80.0 / RATE)
    a2 = math.exp(-2 * math.pi * 160.0 / RATE)
    for i in range(n):
        t = i / RATE
        rev = t * rev_hz
        frac = rev - math.floor(rev)
        exc = 0.0
        for ph, amp in zip(phases, amps):
            d = frac - ph
            if d < 0:
                d += 1.0
            # 3 ms decaying burst
            if d < 0.003:
                exc += amp * math.exp(-d * 900.0)
        lp1 = lp1 * a1 + exc * (1 - a1)
        lp2 = lp2 * a2 + exc * (1 - a2)
        noise = (rng.random() - 0.5) * 0.02
        v = 0.85 * lp1 + 0.55 * lp2 + noise
        out[i] = max(-1.0, min(1.0, v))
    peak = max(1e-6, max(abs(v) for v in out))
    scale = 0.85 / peak
    return [int(v * scale * 32767) for v in out]


def write(path, samples):
    data = b"".join(struct.pack("<h", s) for s in samples)
    hdr = (b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt "
           + struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE * 2, 2, 16)
           + b"data" + struct.pack("<I", len(data)))
    with open(path, "wb") as fh:
        fh.write(hdr)
        fh.write(data)


def main():
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 4.0
    rpm = float(sys.argv[3]) if len(sys.argv) > 3 else 600.0
    samples = build(secs, rpm)
    write(sys.argv[1], samples)
    print("%s: %.1fs %d rpm, %d samples" % (sys.argv[1], secs, int(rpm), len(samples)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
