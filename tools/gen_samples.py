#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""KALK: the sample banks. Vogel-style one-shots rendered by tools/gen_vogel.py (ours, CC0), and CC0 one-shots fetched
from Versilian Studios' VCSL (https://github.com/sgossner/VCSL, CC0 1.0), trimmed, normalised, faded and resampled;
all written as 8-bit mu-law into
firmware/src/kalk_samples.h (in the app's flash: no RAM), with assets/samples-cc0/ATTRIBUTION.txt.

  python3 tools/gen_samples.py [CACHE_DIR]      (downloads go to CACHE_DIR, default /tmp/kalk-vcsl)

Eight samples per voice: WAV 17..24 (the gen_vogel ones first). A path "vogel:NAME" is a gen_vogel candidate. The hat has pairs: the closed step plays the first, the open step the second.
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
V = "vogel:"   # rendered by tools/gen_vogel.py (ours, CC0), not fetched

BANKS = {
    "KICK": (22050, 0.40, [
        ("FMTHUD", V + "FMTHUD"),
        ("RINGKIK", V + "RINGKIK"),
        ("CRSHKIK", V + "CRSHKIK"),
        ("COMBKIK", V + "COMBKIK"),
        ("BDRUM", MB + "Bass Drum 1/BDrumNew_hit_v7_rr1_Sum.wav"),
        ("FRAME-M", MB + "Frame Drum/HDrumL_HitMuted_v3_rr1_Sum.wav"),
        ("TUMBA", MB + "Conga/Tumba_HitFM_v3_rr1_Sum.wav"),
        ("LOG LO", ID + "Slit Drum/LogDrumLo_MedM_v3_rr1_Sum.wav"),
    ]),
    "SNARE": (32000, 0.32, [
        ("RINGSN", V + "RINGSN"),
        ("GRANSN", V + "GRANSN"),
        ("CRSHSN", V + "CRSHSN"),
        ("COMBSN", V + "COMBSN"),
        ("GLTCHCL", V + "GLTCHCL"),
        ("SNARE", MB + "Snare Drum, Modern 1/Snare2_HitSN_v9_rr1_Mid.wav"),
        ("RIMSHOT", MB + "Snare Drum, Modern 3/Snare4_rimshot_v4_rr1_Mid.wav"),
        ("CLAP", ID + "Claps/SoloClap_vl4.wav"),
    ]),
    "HAT": (32000, 0.15, 0.40, [
        ("FMHAT", V + "FMHAT", V + "FMHAT"),
        ("TICKHAT", V + "TICKHAT", V + "TICKHAT"),
        ("COMBHAT", V + "COMBHAT", V + "COMBHAT"),
        ("GRANHAT", V + "GRANHAT", V + "GRANHAT"),
        ("RINGHAT", V + "RINGHAT", V + "RINGHAT"),
        ("HIHAT", ID + "Hi-Hat Cymbal/HiHat_HitC_v4_rr1_Mid.wav", ID + "Hi-Hat Cymbal/HiHat_HitO_rr1_Mid.wav"),
        ("SHAKER", ID + "Shaker, Small/Mid_ShakerDouble_Down_rr1.wav", ID + "Shaker, Small/Mid_Shaker_Roll_Fast_rr1.wav"),
        ("TAMB", ID + "Tambourine 1/Tamb1_Hit_v2_rr1_Mid.wav", ID + "Tambourine 1/Tamb1_Shake_rr1_Mid.wav"),
    ]),
    "TIE": (32000, 0.30, [
        ("FMPING", V + "FMPING"),
        ("RINGPNG", V + "RINGPNG"),
        ("KARPLUS", V + "KARPLUS"),
        ("GRANBEL", V + "GRANBEL"),
        ("CRSHPNG", V + "CRSHPNG"),
        ("COWBELL", ID + "Cowbells/Cowbell1_Normal_v3_rr1_Mid.wav"),
        ("WOOD", ID + "Woodblock/wood_click_f_rr1.wav"),
        ("CONGA", MB + "Conga/Conga_HitN_v3_rr1_Sum.wav"),
    ]),
    "POD": (32000, 0.30, [
        ("ZAPFM", V + "ZAPFM"),
        ("COMBCLK", V + "COMBCLK"),
        ("GLASS", V + "GLASS"),
        ("GRANWOD", V + "GRANWOD"),
        ("BITBLIP", V + "BITBLIP"),
        ("CLAVES", ID + "Claves/Claves1_Hit_v2_rr1_Mid.wav"),
        ("BRAKE", ID + "Brake Drum/BrakeDrum1_Hammer_v3_rr1_Mid.wav"),
        ("TRIANGL", ID + "Triangles/Legacy/1/triangle1_hit_mp.wav"),
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
            items = [(n, pc, cs, 0) for n, pc, po in picks] + [(n, po, os_, 1) for n, pc, po in picks]
        else:
            rate, ms, picks = b
            items = [(n, p, ms, 0) for n, p in picks]
        for name, path, max_s, part in items:
            if path.startswith(V):
                import gen_vogel
                vv, parts = gen_vogel.render(path[len(V):], cache)
                assert vv == voice, f"{path} is a {vv} candidate, not {voice}"
                x = parts[part]
            else:
                d, r = load(fetch(path, cache))
                x = prepare(d, r, rate, max_s)
            off = sum(len(c) for c in blob)
            blob.append(bytes(mulaw(x).astype(np.uint8)))
            step = round(rate / 44100 * 65536)
            entries.append((voice, name, off, len(x), step))
            src = (f"tools/gen_vogel.py {path[len(V):]}: {gen_vogel.recipe(path[len(V):])}" if path.startswith(V)
                   else f"sgossner/VCSL: {path}")
            credits.append(f"{voice:5} {name:8} {len(x) / rate * 1000:4.0f} ms  <-  {src}")
            print(credits[-1])
    data = b"".join(blob)
    lines = ["/* SPDX-License-Identifier: CC0-1.0 (the samples); the header layout GPL-3.0-only */",
             "/* KALK's sample banks: written by tools/gen_samples.py, do not edit. 8-bit mu-law: rendered by",
             " * tools/gen_vogel.py (CC0), and from Versilian Studios' VCSL (CC0 1.0, https://github.com/sgossner/VCSL);",
             " * assets/samples-cc0/ATTRIBUTION.txt */",
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
                    "Sources:\n"
                    "  - tools/gen_vogel.py: synthesised in this repository (FM, ring-mod, crush, comb, Karplus-Strong),\n"
                    "    some from grains of VCSL recordings; dedicated to the public domain under CC0 1.0.\n"
                    "  - Versilian Studios (Sam Gossner), VCSL  https://github.com/sgossner/VCSL  (CC0-1.0)\n"
                    "Processed by tools/gen_samples.py: trimmed, normalised, faded, resampled, 8-bit mu-law.\n\n" +
                    "\n".join(credits) + "\n")
    print(f"{len(entries)} samples, {len(data)} bytes -> {OUT}")


if __name__ == "__main__":
    main()
