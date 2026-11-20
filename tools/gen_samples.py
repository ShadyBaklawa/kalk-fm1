#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""KALK: the sample banks. Fetches CC0 one-shots from Versilian Studios' VCSL (https://github.com/sgossner/VCSL,
CC0 1.0), trims, normalises, fades and resamples them, and writes them as 8-bit mu-law into
firmware/src/kalk_samples.h (in the app's flash: no RAM), with assets/samples-cc0/ATTRIBUTION.txt.

  python3 tools/gen_samples.py [CACHE_DIR]      (downloads go to CACHE_DIR, default /tmp/kalk-vcsl)

Eight samples per voice: WAV 17..24. The hat has pairs: the closed step plays the first, the open step the second.
"""
import io
import sys
import urllib.parse
import urllib.request
from pathlib import Path

import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "firmware" / "src" / "kalk_samples.h"
ATTR = ROOT / "assets" / "samples-cc0" / "ATTRIBUTION.txt"
RAW = "https://raw.githubusercontent.com/sgossner/VCSL/master/"
ID = "Idiophones/Struck Idiophones/"
MB = "Membranophones/Struck Membranophones/"

# voice: (rate, max seconds, [(name, path), ...]); hat: (rate, closed s, open s, [(name, closed, open), ...])
BANKS = {
    "KICK": (22050, 0.40, [
        ("CAJON", ID + "Cajon/Cajon_hit1_fff_rr1.wav"),
        ("FRAME", MB + "Frame Drum/HDrumL_Hit_v3_rr1_Sum.wav"),
        ("FRAME-M", MB + "Frame Drum/HDrumL_HitMuted_v3_rr1_Sum.wav"),
        ("BDRUM", MB + "Bass Drum 1/BDrumNew_hit_v7_rr1_Sum.wav"),
        ("CONCERT", MB + "Bass Drum 2/bassdrum_hit_ff.wav"),
        ("TUMBA", MB + "Conga/Tumba_HitFM_v3_rr1_Sum.wav"),
        ("LOW TOM", MB + "Tom 2/Stick/TomL_HitS_v4_rr1_Mid.wav"),
        ("LOG LO", ID + "Slit Drum/LogDrumLo_MedM_v3_rr1_Sum.wav"),
    ]),
    "SNARE": (32000, 0.32, [
        ("SNARE", MB + "Snare Drum, Modern 1/Snare2_HitSN_v9_rr1_Mid.wav"),
        ("SN OFF", MB + "Snare Drum, Modern 1/Snare2_HitNS_v6_rr1_Mid.wav"),
        ("RIMSHOT", MB + "Snare Drum, Modern 3/Snare4_rimshot_v4_rr1_Mid.wav"),
        ("X-STICK", MB + "Snare Drum, Modern 3/Snare4_Xstick_v2_rr1_Mid.wav"),
        ("ROPE", MB + "Snare Drum, Rope Tension/RopeSnare_stick_Main_vl1_rr1.wav"),
        ("CLAPS", ID + "Claps/Clap_rr1.wav"),
        ("CLAP", ID + "Claps/SoloClap_vl4.wav"),
        ("SLAP", ID + "Cajon/Cajon_hit3_f_rr1.wav"),
    ]),
    "HAT": (32000, 0.15, 0.40, [
        ("HIHAT", ID + "Hi-Hat Cymbal/HiHat_HitC_v4_rr1_Mid.wav", ID + "Hi-Hat Cymbal/HiHat_HitO_rr1_Mid.wav"),
        ("HH SOFT", ID + "Hi-Hat Cymbal/HiHat_HitC_v2_rr1_Mid.wav", ID + "Hi-Hat Cymbal/HiHat_HitOC_rr5_Mid.wav"),
        ("PEDAL", ID + "Hi-Hat Cymbal/HiHat_Close_rr1_Mid.wav", ID + "Hi-Hat Cymbal/HiHat_HitLoose_rr1_Mid.wav"),
        ("SHAKER", ID + "Shaker, Small/Mid_ShakerDouble_Down_rr1.wav", ID + "Shaker, Small/Mid_Shaker_Roll_Fast_rr1.wav"),
        ("TAMB", ID + "Tambourine 1/Tamb1_Hit_v2_rr1_Mid.wav", ID + "Tambourine 1/Tamb1_Shake_rr1_Mid.wav"),
        ("CABASA", ID + "Cabasa/Cabasa1_Hit_rr1_Mid.wav", ID + "Cabasa/Cabasa1_Rub_v2_rr1_Mid.wav"),
        ("SLEIGH", ID + "Sleigh Bells/Sleighbells_Hit_rr1_Mid.wav", ID + "Sleigh Bells/sleighbell2_shake1.wav"),
        ("CYMBAL", ID + "Suspended Cymbal 1/susCymb1_hit_stick_mp1.wav", ID + "Suspended Cymbal 1/susCymb1_hit_stick_f1.wav"),
    ]),
    "TIE": (32000, 0.30, [
        ("LOG HI", ID + "Slit Drum/LogDrumHi_MedM_v3_rr1_Sum.wav"),
        ("COWBELL", ID + "Cowbells/Cowbell1_Normal_v3_rr1_Mid.wav"),
        ("COWBL 2", ID + "Cowbells/Cowbell2_Normal_v3_rr1_Mid.wav"),
        ("AGOGO", ID + "Agogo Bells/Agogo_High_v2_rr1_Mid.wav"),
        ("WOOD", ID + "Woodblock/wood_click_f_rr1.wav"),
        ("CONGA", MB + "Conga/Conga_HitN_v3_rr1_Sum.wav"),
        ("BONGO", MB + "Bongos/BongoH_Hit1_v3_rr1_Mid.wav"),
        ("HI TOM", MB + "Tom 1/Stick/TomH_HitS_v4_rr1_Mid.wav"),
    ]),
    "POD": (32000, 0.30, [
        ("CLAVES", ID + "Claves/Claves1_Hit_v2_rr1_Mid.wav"),
        ("TRIANGL", ID + "Triangles/Legacy/1/triangle1_hit_mp.wav"),
        ("BRAKE", ID + "Brake Drum/BrakeDrum1_Hammer_v3_rr1_Mid.wav"),
        ("QUINTO", MB + "Conga/Quinto_HitFM1_v2_rr1_Sum.wav"),
        ("VIBRSLP", ID + "Vibraslap/Legacy/vibraslap_rr1.wav"),
        ("BONGO L", MB + "Bongos/BongoL_Hit1_v3_rr1_Mid.wav"),
        ("AGOGO L", ID + "Agogo Bells/Agogo_Low_v2_rr1_Mid.wav"),
        ("TRI MUT", ID + "Triangles/Legacy/1/triangle1_hit_mp_muted1.wav"),
    ]),
}


def fetch(path, cache):
    f = cache / path.replace("/", "__")
    if not f.exists():
        url = RAW + urllib.parse.quote(path)
        with urllib.request.urlopen(url, timeout=120) as r:
            f.write_bytes(r.read())
    return f


def load(f):
    """mono float -1..1 and its rate (8 / 16 / 24 / 32-bit int or float WAVs)"""
    rate, d = wavfile.read(io.BytesIO(f.read_bytes()))
    if d.dtype == np.int16: d = d / 32768.0
    elif d.dtype == np.int32: d = d / 2147483648.0
    elif d.dtype == np.uint8: d = (d - 128) / 128.0
    d = d.astype(np.float64)
    if d.ndim == 2: d = d.mean(1)
    return d, rate


def prepare(d, rate, out_rate, max_s):
    """the hit from its start, resampled, -1 dBFS, cut where it has died away (or at max_s) with a fade"""
    pk = np.abs(d).max() or 1.0
    start = int(np.argmax(np.abs(d) > pk * 0.02))
    d = d[max(0, start - int(rate * 0.0005)):]       # (half a millisecond before the attack)
    from math import gcd
    g = gcd(out_rate, rate)
    d = resample_poly(d, out_rate // g, rate // g)
    d = d / (np.abs(d).max() or 1.0) * 0.89
    n = min(len(d), int(out_rate * max_s))
    env = np.abs(d[:n])
    quiet = np.where(env > 0.0035)[0]                   # -49 dB: where it ends
    n = min(n, (quiet[-1] + 1) if len(quiet) else n)
    d = d[:n].copy()
    fade = min(n // 3, int(out_rate * 0.03))
    if fade > 0:
        d[n - fade:] *= np.linspace(1, 0, fade) ** 2
    return d


def mulaw(x):
    """G.711 mu-law, 8 bits (the decoder is MULAW_T in the header)"""
    s = np.clip(np.round(x * 32635), -32635, 32635).astype(np.int32)
    sign = (s < 0).astype(np.int32)
    s = np.abs(s) + 0x84
    exp = np.floor(np.log2(np.maximum(s, 1))).astype(np.int32) - 7
    exp = np.clip(exp, 0, 7)
    man = (s >> (exp + 3)) & 0x0F
    return (~((sign << 7) | (exp << 4) | man)) & 0xFF


def mulaw_table():
    t = []
    for u in range(256):
        u = ~u & 0xFF
        sign, exp, man = u & 0x80, (u >> 4) & 7, u & 0x0F
        s = (((man << 3) + 0x84) << exp) - 0x84
        t.append(-s if sign else s)
    return t


def main():
    cache = Path(sys.argv[1] if len(sys.argv) > 1 else "/tmp/kalk-vcsl")
    cache.mkdir(parents=True, exist_ok=True)
    blob, entries, credits = [], [], []
    order = ["KICK", "SNARE", "HAT", "TIE", "POD"]
    for voice in order:
        b = BANKS[voice]
        if voice == "HAT":
            rate, cs, os_, picks = b
            items = [(n, p, cs) for n, pc, po in picks for p in [pc]] + [(n, po, os_) for n, pc, po in picks]
        else:
            rate, ms, picks = b
            items = [(n, p, ms) for n, p in picks]
        for name, path, max_s in items:
            d, r = load(fetch(path, cache))
            x = prepare(d, r, rate, max_s)
            off = sum(len(c) for c in blob)
            blob.append(bytes(mulaw(x).astype(np.uint8)))
            step = round(rate / 44100 * 65536)
            entries.append((voice, name, off, len(x), step))
            credits.append(f"{voice:5} {name:8} {len(x) / rate * 1000:4.0f} ms  <-  sgossner/VCSL: {path}")
            print(credits[-1])
    data = b"".join(blob)
    lines = ["/* SPDX-License-Identifier: CC0-1.0 (the samples); the header layout GPL-3.0-only */",
             "/* KALK's sample banks: written by tools/gen_samples.py, do not edit. 8-bit mu-law, from Versilian",
             " * Studios' VCSL (CC0 1.0, https://github.com/sgossner/VCSL); assets/samples-cc0/ATTRIBUTION.txt */",
             "#pragma once", "#include <stdint.h>", "",
             "typedef struct { uint32_t off, len; uint16_t step; char name[8]; } ksmp_t;   /* step: rate / 44100, Q16 */",
             "#define NSMP 8u", "",
             "/* mu-law -> Q15 */", "static const int16_t MULAW_T[256] = {"]
    t = mulaw_table()
    for i in range(0, 256, 16):
        lines.append("    " + ", ".join(str(v) for v in t[i:i + 16]) + ",")
    lines.append("};")
    lines.append("")
    lines.append(f"/* {len(data)} bytes */")
    lines.append(f"static const uint8_t SMP_DATA[{len(data)}] = {{")
    for i in range(0, len(data), 24):
        lines.append("    " + ",".join(str(b) for b in data[i:i + 24]) + ",")
    lines.append("};")
    lines.append("")
    for k, voice in enumerate(order):
        es = [e for e in entries if e[0] == voice]
        nm = {"HAT": "SMP_HAT", "KICK": "SMP_KICK", "SNARE": "SMP_SNARE", "TIE": "SMP_TIE", "POD": "SMP_POD"}[voice]
        lines.append(f"static const ksmp_t {nm}[{len(es)}] = {{" + ("   /* closed 0..7, open 8..15 */" if voice == "HAT" else ""))
        for _, name, off, n, step in es:
            lines.append(f'    {{{off}u, {n}u, {step}u, "{name[:7]}"}},')
        lines.append("};")
    OUT.write_text("\n".join(lines) + "\n")
    ATTR.parent.mkdir(parents=True, exist_ok=True)
    ATTR.write_text("KALK sample banks - source material\n"
                    "Licence: CC0 1.0 Universal (public domain dedication)\n"
                    "  https://creativecommons.org/publicdomain/zero/1.0/\n"
                    "Source: Versilian Studios (Sam Gossner), VCSL  https://github.com/sgossner/VCSL  (CC0-1.0)\n"
                    "Processed by tools/gen_samples.py: trimmed, normalised, faded, resampled, 8-bit mu-law.\n\n" +
                    "\n".join(credits) + "\n")
    print(f"{len(entries)} samples, {len(data)} bytes -> {OUT}")


if __name__ == "__main__":
    main()
