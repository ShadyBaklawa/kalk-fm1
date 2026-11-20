#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""KALK: Vogel-style one-shots, made here (no third-party recordings except CC0 VCSL grains).

FM and ring-mod pings, bitcrushed ticks, comb-filtered clicks, and granular smears of Versilian VCSL (CC0 1.0).
Every sound is rendered from code with a fixed seed, so the output is the same on every run and the result is
ours to license (the rendered audio: CC0, like the VCSL source).

  python3 tools/gen_vogel.py [OUT_DIR] [CACHE_DIR]

Writes OUT_DIR/<NN>_<VOICE>_<NAME>.wav (16-bit, already passed through 8-bit mu-law at the voice's rate, so you
hear what the FM-1 plays), one audition WAV per voice and audition_all.wav (each candidate 4 times on 8ths at 120 BPM, a gap
between), and OUT_DIR/candidates.txt (number, voice, name, length, flash bytes, where it starts in
audition_all.wav, recipe).
Default OUT_DIR: build/vogel. Hats are pairs: closed, then open.
"""
import sys
from math import gcd
from pathlib import Path

import numpy as np
from scipy.io import wavfile
from scipy.signal import butter, lfilter, resample_poly, sosfilt

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_samples import ID, MB, fetch, load, mulaw, mulaw_table  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
FS = 48000                       # render rate; resampled to the voice's rate at the end
RATE = {"KICK": 22050, "SNARE": 32000, "HAT": 32000, "TIE": 32000, "POD": 32000}
MAXS = {"KICK": 0.40, "SNARE": 0.32, "HAT": (0.15, 0.40), "TIE": 0.30, "POD": 0.30}
CACHE = Path("/tmp/kalk-vcsl")


# ---------------------------------------------------------------- building blocks
def t_(dur):
    return np.arange(int(FS * dur)) / FS


def expenv(dur, tau, att=0.0005):
    t = t_(dur)
    a = np.minimum(1.0, t / att) if att > 0 else 1.0
    return a * np.exp(-t / tau)


def osc(freq, dur, phase=0.0):
    """sine with a per-sample frequency (array or scalar)"""
    f = np.broadcast_to(np.asarray(freq, float), (int(FS * dur),))
    return np.sin(2 * np.pi * np.cumsum(f) / FS + phase)


def sweep(f0, f1, tau, dur):
    """exponential pitch drop from f0 towards f1"""
    t = t_(dur)
    return f1 + (f0 - f1) * np.exp(-t / tau)


def fm(fc, ratio, index, dur):
    """2-op FM: carrier fc (array ok), modulator fc*ratio, index (array ok)"""
    fc = np.broadcast_to(np.asarray(fc, float), (int(FS * dur),))
    mod = np.sin(2 * np.pi * np.cumsum(fc * ratio) / FS)
    return np.sin(2 * np.pi * np.cumsum(fc) / FS + np.asarray(index) * mod)


def noise(dur, rng):
    return rng.uniform(-1, 1, int(FS * dur))


def bp(x, lo, hi, order=2):
    return sosfilt(butter(order, [lo, hi], "bandpass", fs=FS, output="sos"), x)


def hp(x, f, order=2):
    return sosfilt(butter(order, f, "highpass", fs=FS, output="sos"), x)


def lp(x, f, order=2):
    return sosfilt(butter(order, f, "lowpass", fs=FS, output="sos"), x)


def comb(x, delay_s, fb, damp=0.3):
    """feedback comb with a one-pole lowpass in the loop (damp 0 = bright)"""
    d = max(1, int(round(delay_s * FS)))
    y = np.zeros(len(x))
    lpz = 0.0
    for n in range(len(x)):
        fbk = y[n - d] if n >= d else 0.0
        lpz = (1 - damp) * fbk + damp * lpz
        y[n] = x[n] + fb * lpz
    return y


def crush(x, bits, hold):
    """bit depth and sample-and-hold (hold: keep every n-th sample)"""
    x = np.repeat(x[::hold], hold)[: len(x)]
    q = 2 ** (bits - 1)
    return np.round(x * q) / q


def fold(x, amt):
    """wavefolder"""
    return np.sin(x * amt * np.pi / 2)


def karplus(f, dur, rng, decay=0.996, bright=0.5):
    n = int(FS * dur)
    p = max(2, int(FS / f))
    buf = rng.uniform(-1, 1, p)
    out = np.zeros(n)
    for i in range(n):
        j = i % p
        out[i] = buf[j]
        buf[j] = decay * (bright * buf[j] + (1 - bright) * buf[(j + 1) % p])
    return out


def vcsl(path):
    d, r = load(fetch(path, CACHE))
    g = gcd(FS, r)
    d = resample_poly(d, FS // g, r // g)
    pk = np.abs(d).max() or 1.0
    s = int(np.argmax(np.abs(d) > pk * 0.02))
    return d[s:] / pk


def granular(src, dur, rng, grain=(0.012, 0.045), n=60, pitch=(0.7, 1.6), rev=0.4, spread=1.0, head=0.012):
    """the source's attack, then grains of it scattered over dur, thinning out; some reversed, some re-pitched"""
    out = np.zeros(int(FS * dur))
    h = min(len(src), int(FS * head))
    out[:h] += src[:h] * np.hanning(2 * h)[h:] if h else 0
    src_span = max(1, int(len(src) * 0.6))
    for _ in range(n):
        gl = int(FS * rng.uniform(*grain))
        ps = rng.uniform(*pitch)
        start = rng.integers(0, max(1, src_span - int(gl * ps)))
        seg = src[start:start + int(gl * ps)]
        if len(seg) < 8:
            continue
        seg = np.interp(np.linspace(0, len(seg) - 1, gl), np.arange(len(seg)), seg)
        if rng.random() < rev:
            seg = seg[::-1]
        seg = seg * np.hanning(gl)
        pos = int(len(out) * spread * rng.random() ** 2.2)     # dense at the start, sparse later
        e = min(len(out), pos + gl)
        out[pos:e] += seg[: e - pos] * rng.uniform(0.3, 1.0) * np.exp(-pos / (FS * dur * 0.35))
    return out


def ring(a, b):
    return a * b


def pad(x, dur):
    n = int(FS * dur)
    return np.pad(x, (0, max(0, n - len(x))))[:n]


# ---------------------------------------------------------------- the candidates
# Each recipe returns mono float at FS. Hats return (closed, open).
def kick_fmthud(rng):
    d = 0.38
    f = sweep(190, 44, 0.035, d)
    idx = 2.8 * np.exp(-t_(d) / 0.012)
    return fm(f, 1.5, idx, d) * expenv(d, 0.085)


def kick_ringkik(rng):
    d = 0.34
    body = osc(sweep(140, 52, 0.04, d), d) * expenv(d, 0.09)
    rm = ring(body, osc(sweep(400, 87, 0.02, d), d))
    click = hp(noise(d, rng), 2500) * expenv(d, 0.002)
    return 0.75 * body + 0.45 * rm + 0.35 * click


def kick_crshkik(rng):
    d = 0.30
    x = osc(sweep(260, 48, 0.03, d), d) * expenv(d, 0.08)
    return lp(crush(fold(x, 1.4), 5, 6), 6000)


def kick_combkik(rng):
    d = 0.40
    exc = lp(noise(d, rng), 900) * expenv(d, 0.004)
    x = comb(exc, 1 / 58.0, 0.93, damp=0.55)
    x = x + 0.6 * osc(sweep(120, 50, 0.03, d), d) * expenv(d, 0.06)
    return lp(x, 2500) * expenv(d, 0.15, att=0)


def snare_ringsn(rng):
    d = 0.26
    tone = ring(osc(184, d), osc(333, d)) * expenv(d, 0.035)
    nz = bp(noise(d, rng), 1800, 9000) * expenv(d, 0.045)
    return 0.6 * tone + 0.8 * nz


def snare_gransn(rng):
    src = vcsl(MB + "Snare Drum, Modern 1/Snare2_HitSN_v9_rr1_Mid.wav")
    return granular(src, 0.30, rng, grain=(0.006, 0.025), n=70, pitch=(0.8, 2.2), rev=0.5)


def snare_crshsn(rng):
    d = 0.24
    nz = noise(d, rng) * expenv(d, 0.06)
    body = osc(sweep(330, 210, 0.01, d), d) * expenv(d, 0.03)
    return crush(0.8 * nz + 0.6 * body, 4, 9)


def snare_combsn(rng):
    d = 0.30
    exc = noise(d, rng) * expenv(d, 0.012)
    x = 0.5 * comb(exc, 0.0023, 0.86, 0.2) + 0.5 * comb(exc, 0.0041, 0.84, 0.25)
    return hp(x, 300) * expenv(d, 0.09, att=0)


def snare_glitchcl(rng):
    d = 0.28
    x = np.zeros(int(FS * d))
    for k, off in enumerate([0.0, 0.009, 0.017, 0.028]):
        o = int(off * FS)
        seg = bp(noise(d - off, rng), 900, 6000) * expenv(d - off, 0.004 if k < 3 else 0.04)
        x[o:o + len(seg)] += seg[: len(x) - o] * (0.7 if k < 3 else 1.0)
    return ring(x, 0.6 + 0.4 * osc(1130, d))


def _metal(d, base, rng):
    """six square-ish partials at inharmonic ratios (the TR-808 idea), FM'd together"""
    ratios = [1.0, 1.4471, 1.6170, 1.9265, 2.5028, 2.6637]
    x = sum(np.sign(osc(base * r, d, rng.uniform(0, 6.28))) for r in ratios)
    return hp(bp(x, 6000, 14000), 7000)


def hat_fmhat(rng):
    m = _metal(0.40, 317, rng)
    return m[: int(0.15 * FS)] * expenv(0.15, 0.025), m * expenv(0.40, 0.07)


def hat_tickhat(rng):
    def one(d, tau):
        x = np.sign(noise(d, rng)) * expenv(d, tau)
        return hp(crush(x, 3, 3), 4000)
    return one(0.15, 0.012), one(0.40, 0.06)


def hat_combhat(rng):
    def one(d, tau):
        exc = noise(d, rng) * expenv(d, tau)
        return hp(comb(exc, 0.00031, 0.7, 0.05) - 0.6 * comb(exc, 0.00047, 0.6, 0.05), 5000)
    return one(0.15, 0.02), one(0.40, 0.07)


def hat_granhat(rng):
    c = vcsl(ID + "Hi-Hat Cymbal/HiHat_HitC_v4_rr1_Mid.wav")
    o = vcsl(ID + "Suspended Cymbal 1/susCymb1_hit_stick_f1.wav")
    a = granular(c, 0.15, rng, grain=(0.003, 0.012), n=40, pitch=(1.0, 2.5), rev=0.6)
    b = granular(o, 0.40, rng, grain=(0.004, 0.03), n=90, pitch=(1.2, 2.8), rev=0.5)
    return hp(a, 3000), hp(b, 3000)


def hat_ringhat(rng):
    def one(d, tau):
        x = ring(osc(3170, d), osc(4730 + 900 * osc(61, d), d))
        return hp(0.7 * x + 0.4 * noise(d, rng), 5000) * expenv(d, tau)
    return one(0.15, 0.018), one(0.40, 0.065)


def tie_fmping(rng):
    d = 0.30
    return fm(440, 3.5, 4.0 * np.exp(-t_(d) / 0.03), d) * expenv(d, 0.07)


def tie_ringpng(rng):
    d = 0.28
    return ring(osc(440, d), osc(621, d)) * expenv(d, 0.05) + 0.3 * osc(440, d) * expenv(d, 0.07)


def tie_karplus(rng):
    return karplus(220, 0.30, rng, decay=0.985, bright=0.35) * expenv(0.30, 0.08, att=0)


def tie_granbel(rng):
    src = vcsl(ID + "Agogo Bells/Agogo_High_v2_rr1_Mid.wav")
    return granular(src, 0.30, rng, grain=(0.015, 0.05), n=30, pitch=(0.98, 1.02), rev=0.3)


def tie_crshpng(rng):
    d = 0.26
    x = fm(330, 1.41, 3.0 * np.exp(-t_(d) / 0.05), d) * expenv(d, 0.08)
    return crush(x, 4, 4)


def pod_zapfm(rng):
    d = 0.22
    f = sweep(2400, 180, 0.012, d)
    return fm(f, 2.0, 2.5 * np.exp(-t_(d) / 0.02), d) * expenv(d, 0.06)


def pod_combclk(rng):
    d = 0.24
    exc = np.zeros(int(FS * d))
    exc[:3] = [1.0, -0.7, 0.4]
    return hp(comb(exc, 1 / 1210.0, 0.95, 0.12), 400) * expenv(d, 0.07, att=0)


def pod_glass(rng):
    d = 0.18
    x = ring(osc(2093, d), osc(2793, d)) + 0.5 * osc(4186, d)
    return x * expenv(d, 0.03)


def pod_granwod(rng):
    src = vcsl(ID + "Claves/Claves1_Hit_v2_rr1_Mid.wav")
    return granular(src, 0.26, rng, grain=(0.004, 0.02), n=35, pitch=(0.6, 1.8), rev=0.5)


def pod_bitblip(rng):
    d = 0.16
    f = np.where(t_(d) < 0.02, 1760, 880)
    return crush(np.sign(osc(f, d)) * expenv(d, 0.04), 3, 2) * 0.6


CANDIDATES = [
    ("KICK", "FMTHUD", kick_fmthud, "2-op FM, pitch drop 190->44 Hz, index decays in 12 ms"),
    ("KICK", "RINGKIK", kick_ringkik, "sine body ring-modded by a falling 400->87 Hz sine, noise click"),
    ("KICK", "CRSHKIK", kick_crshkik, "folded sine kick, 5-bit, 1/6 sample-hold"),
    ("KICK", "COMBKIK", kick_combkik, "dark noise burst into a 58 Hz comb, plus a sine body"),
    ("SNARE", "RINGSN", snare_ringsn, "184 x 333 Hz ring-mod tone with band-passed noise"),
    ("SNARE", "GRANSN", snare_gransn, "granular smear of a VCSL snare (6-25 ms grains, half reversed)"),
    ("SNARE", "CRSHSN", snare_crshsn, "noise + 330 Hz body, 4-bit, 1/9 sample-hold"),
    ("SNARE", "COMBSN", snare_combsn, "noise burst through 2.3 ms and 4.1 ms combs: metallic"),
    ("SNARE", "GLTCHCL", snare_glitchcl, "4-burst clap ring-modded at 1130 Hz"),
    ("HAT", "FMHAT", hat_fmhat, "six inharmonic square partials, band-passed (closed / open)"),
    ("HAT", "TICKHAT", hat_tickhat, "1-bit noise, 3-bit crush, high-passed ticks"),
    ("HAT", "COMBHAT", hat_combhat, "noise through 0.31 / 0.47 ms combs"),
    ("HAT", "GRANHAT", hat_granhat, "granular smears of VCSL hi-hat / suspended cymbal"),
    ("HAT", "RINGHAT", hat_ringhat, "3.17 kHz x wobbling 4.73 kHz ring-mod plus noise"),
    ("TIE", "FMPING", tie_fmping, "FM bell, ratio 3.5, A4"),
    ("TIE", "RINGPNG", tie_ringpng, "440 x 621 Hz ring-mod ping"),
    ("TIE", "KARPLUS", tie_karplus, "Karplus-Strong pluck, A3"),
    ("TIE", "GRANBEL", tie_granbel, "granular agogo bell, grains kept near pitch"),
    ("TIE", "CRSHPNG", tie_crshpng, "FM ping (ratio 1.41), 4-bit, 1/4 sample-hold"),
    ("POD", "ZAPFM", pod_zapfm, "FM zap, 2.4 kHz -> 180 Hz in 12 ms"),
    ("POD", "COMBCLK", pod_combclk, "3-sample click into a 1210 Hz comb"),
    ("POD", "GLASS", pod_glass, "C7 x F7 ring-mod plus C8: short glassy ping"),
    ("POD", "GRANWOD", pod_granwod, "granular claves, scattered and re-pitched"),
    ("POD", "BITBLIP", pod_bitblip, "3-bit square blip, octave drop after 20 ms"),
]


# ---------------------------------------------------------------- to the FM-1's format and back
def shape(x, rate, max_s):
    """what gen_samples.py does: DC out, to the voice's rate, -1 dBFS, cut where it has died away, a 30 ms fade"""
    x = hp(np.asarray(x, float), 25, 1)             # no DC (ring-mod and crush can leave some)
    g = gcd(rate, FS)
    x = resample_poly(x, rate // g, FS // g)
    x = x / (np.abs(x).max() or 1.0) * 0.89
    n = min(len(x), int(rate * max_s))
    quiet = np.where(np.abs(x[:n]) > 0.0035)[0]
    n = min(n, (quiet[-1] + 1) if len(quiet) else n)
    x = x[:n].copy()
    fade = min(n // 3, int(rate * 0.03))
    if fade:
        x[n - fade:] *= np.linspace(1, 0, fade) ** 2
    return x


def finish(x, rate, max_s):
    """shape(), then 8-bit mu-law and back: returns (audio at rate as the FM-1 plays it, flash bytes)"""
    x = shape(x, rate, max_s)
    t = np.array(mulaw_table(), float) / 32768.0
    return t[mulaw(x)], len(x)


def render(name, cache=None):
    """one candidate by name, shaped for its voice (not yet mu-law): [one-shot] or [closed, open] for a hat,
    with the same seed as in the audition, so a pick sounds exactly as auditioned"""
    global CACHE
    if cache is not None:
        CACHE = Path(cache)
    for i, (voice, n, fn, _) in enumerate(CANDIDATES, 1):
        if n == name:
            res = fn(np.random.default_rng(1000 + i))
            parts = res if voice == "HAT" else (res,)
            maxs = MAXS[voice] if voice == "HAT" else (MAXS[voice],)
            return voice, [shape(x, RATE[voice], m) for x, m in zip(parts, maxs)]
    raise KeyError(f"no gen_vogel candidate {name!r}")


def recipe(name):
    return next(r for _, n, _, r in CANDIDATES if n == name)


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build" / "vogel"
    global CACHE
    CACHE = Path(sys.argv[2]) if len(sys.argv) > 2 else CACHE
    CACHE.mkdir(parents=True, exist_ok=True)
    out.mkdir(parents=True, exist_ok=True)
    for f in out.glob("*.wav"):
        f.unlink()
    rows, aud, allbuf, alllen = [], {}, [], 0
    for i, (voice, name, fn, recipe) in enumerate(CANDIDATES, 1):
        rng = np.random.default_rng(1000 + i)
        rate = RATE[voice]
        res = fn(rng)
        parts = res if voice == "HAT" else (res,)
        maxs = MAXS[voice] if voice == "HAT" else (MAXS[voice],)
        heard, nbytes, ms = [], 0, []
        for p, m in zip(parts, maxs):
            y, n = finish(p, rate, m)
            heard.append(y)
            nbytes += n
            ms.append(f"{n / rate * 1000:.0f}")
        tag = f"{i:02d}_{voice}_{name}"
        both = np.concatenate([np.concatenate([h, np.zeros(int(rate * 0.15))]) for h in heard])
        wavfile.write(out / f"{tag}.wav", rate, (both * 32767).astype(np.int16))
        # audition: 4 hits on 8ths at 120 BPM; hats: closed, closed, open, closed, open; then a gap (tails overlap)
        a = aud.setdefault(voice, {"rate": rate, "buf": [], "len": 0})
        hits = [0, 0, 1, 0, 1] if voice == "HAT" else [0, 0, 0, 0]
        stepn = int(rate * 0.25)
        seg = np.zeros(stepn * len(hits) + int(rate * 0.9))
        for k, h in enumerate(hits):
            seg[k * stepn:k * stepn + len(heard[h])] += heard[h]
        a["buf"].append(seg)
        a["len"] += len(seg)
        g = gcd(44100, rate)
        seg44 = resample_poly(seg, 44100 // g, rate // g)
        start = alllen / 44100
        allbuf.append(seg44)
        alllen += len(seg44)
        rows.append((i, voice, name, "/".join(ms), nbytes, recipe, start))
        print(f"{i:2d} {voice:5} {name:8} {'/'.join(ms):>7} ms {nbytes:6d} B  {recipe}")
    for voice, a in aud.items():
        y = np.concatenate(a["buf"])
        wavfile.write(out / f"audition_{voice}.wav", a["rate"], (y * 32767).astype(np.int16))
    y = np.clip(np.concatenate(allbuf), -1, 1)
    wavfile.write(out / "audition_all.wav", 44100, (y * 32767).astype(np.int16))
    with open(out / "candidates.txt", "w") as f:
        f.write("#   voice  name      ms (closed/open)  flash   at (audition_all)  recipe\n")
        for i, v, n, ms, b, r, s in rows:
            m, sec = divmod(s, 60)
            f.write(f"{i:2d}  {v:5}  {n:8}  {ms:>9}  {b / 1024:5.1f} KB  {int(m)}:{sec:04.1f}  {r}\n")
    print(f"{len(rows)} candidates, {sum(r[4] for r in rows) / 1024:.1f} KB if all were used -> {out}")


if __name__ == "__main__":
    main()
