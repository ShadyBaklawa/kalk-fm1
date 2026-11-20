#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""KALK: write firmware/src/kalk_tables.h (the chip has no double and no exp/tan: everything is a table).

  python3 tools/gen_tables.py
"""
import math
from pathlib import Path

FS = 44100
OUT = Path(__file__).resolve().parent.parent / "firmware" / "src" / "kalk_tables.h"


def arr(name, ctype, vals, per=8):
    lines = [f"static const {ctype} {name}[{len(vals)}] = {{"]
    for i in range(0, len(vals), per):
        lines.append("    " + ", ".join(str(v) + ("u" if ctype.startswith("uint") else "") for v in vals[i:i + per]) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    out = ["/* SPDX-License-Identifier: GPL-3.0-only */",
           "/* KALK tables: written by tools/gen_tables.py, do not edit. */",
           "#pragma once", "#include <stdint.h>", ""]

    # sine, 1024 points + 1 guard (linear interpolation), Q15
    sine = [round(32767 * math.sin(2 * math.pi * i / 1024)) for i in range(1025)]
    out.append("/* one sine cycle, Q15, 1024 points and a guard */")
    out.append(arr("SINE_T", "int16_t", sine, 16))
    out.append("")

    # phase increment per MIDI note: f * 2^32 / FS
    inc = [min(round(440 * 2 ** ((n - 69) / 12) * 2 ** 32 / FS), 2 ** 31 - 1) for n in range(128)]
    out.append("/* phase increment of MIDI note n at 44.1 kHz: f * 2^32 / 44100 */")
    out.append(arr("INC_NOTE", "uint32_t", inc, 6))
    out.append("")

    # semitone fractions for fine pitch: 2^(i/16/12) Q16, i = 0..15 (1/16 semitone)
    fine = [round(65536 * 2 ** (i / 16 / 12)) for i in range(16)]
    out.append("/* 2^(i / 192): sixteenths of a semitone, Q16 */")
    out.append(arr("FINE_Q16", "uint32_t", fine, 8))
    out.append("")

    # decay: per-sample multiplier (Q32) to fall 60 dB in t60, t60 from 8 ms to 6 s over 128 steps (exponential)
    t_lo, t_hi = 0.008, 6.0
    dk = []
    for i in range(128):
        t60 = t_lo * (t_hi / t_lo) ** (i / 127)
        k = 10 ** (-3 / (t60 * FS))
        dk.append(min(round(k * 2 ** 32), 2 ** 32 - 1))
    out.append(f"/* exponential decay, -60 dB in {t_lo * 1000:.0f} ms .. {t_hi:.0f} s: env = env * k >> 32 each sample */")
    out.append(arr("DECAY_K", "uint32_t", dk, 6))
    out.append("")
    # the decay times in ms (for the screen)
    dms = [round(t_lo * (t_hi / t_lo) ** (i / 127) * 1000) for i in range(128)]
    out.append("/* DECAY_K's times in ms (the screen) */")
    out.append(arr("DECAY_MS", "uint16_t", dms, 16))
    out.append("")

    # SVF (zero delay feedback) g = tan(pi fc / fs), Q16, fc 25 Hz .. 18 kHz over 128 steps
    f_lo, f_hi = 25.0, 18000.0
    g = []
    hz = []
    for i in range(128):
        fc = f_lo * (f_hi / f_lo) ** (i / 127)
        g.append(round(math.tan(math.pi * fc / FS) * 65536))
        hz.append(round(fc))
    out.append(f"/* filter: g = tan(pi fc / fs) Q16, fc {f_lo:.0f} Hz .. {f_hi / 1000:.0f} kHz */")
    out.append(arr("SVF_G", "uint32_t", g, 8))
    out.append("")
    out.append("/* SVF_G's cutoffs in Hz (the screen) */")
    out.append(arr("SVF_HZ", "uint16_t", hz, 16))
    out.append("")

    # sample playback: 2^48 / INC_NOTE[60], so (inc_n16(n * 16) * RECIP60) >> 32 is the pitch ratio of n against C4, Q16
    out.append("/* 2^48 / INC_NOTE[60]: the pitch ratio of a note against C4, Q16, is inc * RECIP60 >> 32 */")
    out.append(f"#define RECIP60 {round(2 ** 48 / inc[60])}u")
    out.append("")

    # the screen's lime slice: the angle of a pixel in a quadrant, clockwise from the vertical, 0..1023 (= 90 degrees)
    ang = []
    for ay in range(120):
        for ax in range(120):
            a = math.atan2(ax + 0.5, ay + 0.5) / (math.pi / 2) * 1024
            ang.append(min(1023, int(a)))
    out.append("/* the screen: a pixel's angle in its quadrant, from the vertical, 0..1023 = 90 degrees; [ay * 120 + ax] */")
    out.append(arr("ANG_Q", "uint16_t", ang, 24))
    out.append("")
    OUT.write_text("\n".join(out) + "\n")
    print("tables ->", OUT)


if __name__ == "__main__":
    main()
