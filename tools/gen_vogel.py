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


# ---------------------------------------------------------------- the dark / techno / glitch set (0.7 candidates)
def sat(x, drive):
    return np.tanh(x * drive) / np.tanh(drive)


def saw(freq, dur, phase=0.0):
    f = np.broadcast_to(np.asarray(freq, float), (int(FS * dur),))
    p = (np.cumsum(f) / FS + phase) % 1.0
    return 2 * p - 1


def sq(freq, dur, phase=0.0):
    return np.sign(osc(freq, dur, phase))


def rlp(x, fc, q):
    """resonant 2-pole low pass (Chamberlin SVF), fc may be an array"""
    fc = np.broadcast_to(np.asarray(fc, float), x.shape)
    lo = bd = 0.0
    y = np.zeros(len(x))
    damp = 1.0 / q
    for n in range(len(x)):
        f = 2 * np.sin(np.pi * min(fc[n], FS / 6) / FS)
        lo += f * bd
        hi = x[n] - lo - damp * bd
        bd += f * hi
        y[n] = lo
    return y


def stutter(x, piece, times, gap=0.0):
    """the first `piece` s repeated `times` times (with a gap), then the whole sound"""
    p = x[: int(piece * FS)] * np.hanning(int(piece * FS) * 2)[int(piece * FS):] ** 0.2
    g = np.zeros(int(gap * FS))
    return np.concatenate([np.concatenate([p, g]) for _ in range(times)] + [x])


def tail(x, d):
    """a dark rumble: the sound smeared by a short noise 'room', low-passed"""
    rng = np.random.default_rng(7)
    ir = rng.uniform(-1, 1, int(FS * d)) * np.exp(-t_(d) / (d / 4))
    y = np.convolve(x, ir)[: len(x)] * 0.02
    return lp(y, 220, 2)


def k909(d=0.40, f0=280, f1=50, tau=0.028, dec=0.14):
    return osc(sweep(f0, f1, tau, d), d) * expenv(d, dec)


def dk_k909(rng):
    d = 0.40
    click = hp(noise(d, rng), 3000) * expenv(d, 0.0015)
    return sat(k909(d) + 0.25 * click, 1.6)


def dk_rumble(rng):
    d = 0.40
    k = k909(d, 220, 46, 0.03, 0.09)
    r = tail(k, 0.35)
    return sat(k * 0.8 + 3.0 * r / (np.abs(r).max() + 1e-9) * 0.6 * expenv(d, 0.2, att=0.03), 1.8)


def dk_distkik(rng):
    d = 0.36
    return lp(sat(k909(d, 170, 44, 0.03, 0.12), 7), 3000)


def dk_hardkik(rng):
    d = 0.38
    x = k909(d, 420, 55, 0.018, 0.16)
    return lp(sat(fold(x * 1.2, 1.6), 3), 5000)


def dk_subkik(rng):
    d = 0.40
    return osc(sweep(90, 42, 0.05, d), d) * expenv(d, 0.2, att=0.002)


def dk_dustkik(rng):
    d = 0.34
    x = crush(k909(d, 240, 48), 6, 4) + 0.04 * crush(noise(d, rng), 2, 30) * expenv(d, 0.1)
    return lp(x, 4500)


def dk_tapekik(rng):
    d = 0.38
    wob = 1 + 0.03 * osc(7, d)
    return lp(sat(osc(sweep(200, 47, 0.035, d) * wob, d) * expenv(d, 0.13), 3), 700)


def dk_boxkik(rng):
    d = 0.22
    knock = bp(noise(d, rng), 900, 1600) * expenv(d, 0.006)
    return k909(d, 200, 68, 0.012, 0.05) + 0.5 * knock


def dk_fmdark(rng):
    d = 0.38
    f = sweep(150, 40, 0.04, d)
    return fm(f, 0.5, 3.5 * np.exp(-t_(d) / 0.03), d) * expenv(d, 0.11)


def dk_gltchkk(rng):
    x = sat(k909(0.30, 300, 52, 0.02, 0.11), 2)
    return stutter(x, 0.022, 3)[: int(FS * 0.40)]


def dk_bitkik(rng):
    d = 0.34
    return crush(sat(k909(d, 260, 50, 0.025, 0.12), 2), 3, 10)


def dk_indkik(rng):
    d = 0.40
    k = sat(k909(d, 230, 50, 0.025, 0.13), 3)
    m = comb(bp(noise(d, rng), 400, 3000) * expenv(d, 0.01), 1 / 91.0, 0.9, 0.3)
    return k + 0.35 * m / (np.abs(m).max() + 1e-9) * expenv(d, 0.12, att=0)


def dk_acidkk(rng):
    d = 0.36
    x = saw(sweep(110, 41, 0.05, d), d) * expenv(d, 0.12)
    return sat(rlp(x, 120 + 2600 * np.exp(-t_(d) / 0.025), 6), 2) + 0.5 * k909(d, 160, 41, 0.03, 0.12)


def dk_deepkk(rng):
    d = 0.40
    return lp(sat(k909(d, 180, 44, 0.04, 0.18), 1.4), 400)


def dk_clipkik(rng):
    d = 0.26
    return lp(sat(osc(sweep(120, 52, 0.02, d), d) * expenv(d, 0.07), 14), 2500)


def dk_zapkik(rng):
    d = 0.36
    return sat(osc(sweep(2400, 48, 0.006, d), d) * expenv(d, 0.12), 1.5)


def clap(d, rng, lo=900, hi=3000, tl=0.06, bursts=(0.0, 0.010, 0.019, 0.030)):
    x = np.zeros(int(FS * d))
    for k, off in enumerate(bursts):
        o = int(off * FS)
        last = k == len(bursts) - 1
        seg = bp(noise(d - off, rng), lo, hi) * expenv(d - off, tl if last else 0.004)
        x[o:o + len(seg)] += seg[: len(x) - o]
    return x


def dk_clap9(rng):
    return clap(0.30, rng, 950, 3200, 0.07)


def dk_drkclap(rng):
    return lp(clap(0.32, rng, 500, 1600, 0.10, (0.0, 0.013, 0.024, 0.038)), 2500)


def dk_sn909(rng):
    d = 0.26
    tone = osc(sweep(250, 180, 0.02, d), d) * expenv(d, 0.04) + 0.6 * osc(330, d) * expenv(d, 0.03)
    return 0.7 * tone + 0.9 * hp(noise(d, rng), 1500) * expenv(d, 0.07)


def dk_noisesn(rng):
    d = 0.20
    return crush(hp(noise(d, rng), 1800) * expenv(d, 0.04), 6, 2)


def dk_rimsht(rng):
    d = 0.10
    return (osc(1700, d) + 0.8 * osc(480, d)) * expenv(d, 0.008) + 0.5 * hp(noise(d, rng), 4000) * expenv(d, 0.002)


def dk_metlsn(rng):
    d = 0.32
    exc = bp(noise(d, rng), 500, 8000) * expenv(d, 0.008)
    x = comb(exc, 0.00193, 0.9, 0.15) + comb(exc, 0.00311, 0.88, 0.15) + comb(exc, 0.00457, 0.86, 0.2)
    return hp(x, 250) * expenv(d, 0.1, att=0) + 0.3 * k909(d, 300, 190, 0.01, 0.03)


def dk_distsn(rng):
    d = 0.26
    x = osc(sweep(260, 170, 0.02, d), d) * expenv(d, 0.05) + hp(noise(d, rng), 800) * expenv(d, 0.08)
    return lp(sat(x, 9), 7000)


def dk_gatesn(rng):
    d = 0.24
    x = dk_sn909(rng)[: int(FS * d)] + 0.5 * tail(hp(noise(d, rng), 600), 0.2) * 40
    g = np.where(t_(d) < 0.17, 1.0, 0.0)
    return lp(x, 6000) * g


def dk_gltchsn(rng):
    x = crush(dk_sn909(rng), 5, 3)
    return stutter(x, 0.018, 3, 0.006)[: int(FS * 0.32)]


def dk_bitsn(rng):
    d = 0.20
    return crush(noise(d, rng) * expenv(d, 0.05) + 0.6 * sq(200, d) * expenv(d, 0.03), 2, 5)


def dk_snap(rng):
    d = 0.12
    return bp(noise(d, rng), 1800, 4500) * expenv(d, 0.012) + 0.6 * osc(2600, d) * expenv(d, 0.003)


def dk_revsn(rng):
    d = 0.16
    x = hp(noise(d, rng), 1200) * (t_(d) / d) ** 3
    x[-int(FS * 0.004):] *= np.linspace(1, 0, int(FS * 0.004))
    return x


def dk_tomsn(rng):
    d = 0.30
    return osc(sweep(180, 115, 0.04, d), d) * expenv(d, 0.09) + 0.5 * bp(noise(d, rng), 300, 2500) * expenv(d, 0.05)


def dk_fmsnr(rng):
    d = 0.24
    return fm(sweep(330, 200, 0.02, d), 2.71, 9 * np.exp(-t_(d) / 0.05), d) * expenv(d, 0.06)


def dk_static(rng):
    d = 0.30
    x = crush(noise(d, rng), 3, 6)
    gate = np.repeat(rng.random(int(d * 200) + 1) > 0.45, int(FS / 200))[: int(FS * d)]
    return bp(x * gate, 1500, 9000) * expenv(d, 0.12)


def dk_shotsn(rng):
    d = 0.18
    return sat(hp(noise(d, rng), 2500) * expenv(d, 0.02, att=0), 6) * expenv(d, 0.05, att=0)


def hats(fn, rng, closed=0.15, opn=0.40, tc=0.022, to=0.11):
    m = fn(opn, rng)
    return m[: int(closed * FS)] * expenv(closed, tc), m * expenv(opn, to)


def dk_hat909(rng):
    return hats(lambda d, r: 0.6 * _metal(d, 345, r) + 0.6 * hp(noise(d, r), 8000), rng)


def dk_drkhat(rng):
    return hats(lambda d, r: lp(bp(_metal(d, 280, r), 4000, 9000) + 0.3 * bp(noise(d, r), 4000, 8000), 7500), rng)


def dk_noishat(rng):
    return hats(lambda d, r: hp(noise(d, r), 7000, 4), rng, tc=0.018, to=0.09)


def dk_tick(rng):
    return hats(lambda d, r: hp(noise(d, r), 5000), rng, tc=0.003, to=0.03)


def dk_sizzle(rng):
    return hats(lambda d, r: hp(crush(noise(d, r), 4, 3), 5000), rng, tc=0.03, to=0.15)


def dk_ride(rng):
    def r_(d, r):
        return hp(fm(1250, 2.414, 3.0, d) + 0.5 * _metal(d, 460, r), 3000)
    return hats(r_, rng, tc=0.05, to=0.25)


def dk_shkhat(rng):
    def s_(d, r):
        x = bp(noise(d, r), 5000, 12000)
        return x * (expenv(d, 0.02) + 0.8 * np.roll(expenv(d, 0.02), int(0.03 * FS)))
    return hats(s_, rng, tc=0.06, to=0.16)


def dk_gltchht(rng):
    c, o = dk_hat909(rng)
    return stutter(c, 0.008, 3)[: int(FS * 0.15)], stutter(o, 0.012, 4, 0.004)[: int(FS * 0.40)]


def dk_bithat(rng):
    return hats(lambda d, r: hp(crush(np.sign(noise(d, r)), 1, 7), 3000), rng, tc=0.015, to=0.08)


def dk_chnhat(rng):
    def c_(d, r):
        x = np.zeros(int(FS * d))
        for k in range(9):
            o = int(r.uniform(0, 0.12) * FS)
            s = _metal(0.03, r.uniform(300, 500), r) * expenv(0.03, 0.006)
            x[o:o + len(s)] += s[: len(x) - o] * r.uniform(0.3, 1)
        return x
    return hats(c_, rng, tc=0.04, to=0.12)


def dk_dustht(rng):
    def d_(d, r):
        x = np.where(r.random(int(FS * d)) > 0.995, r.uniform(-1, 1, int(FS * d)), 0.0)
        return hp(x + 0.15 * noise(d, r), 4000)
    return hats(d_, rng, tc=0.04, to=0.14)


def dk_fmhat2(rng):
    return hats(lambda d, r: hp(fm(8300, 1.41, 6.0, d), 5000) + 0.3 * hp(noise(d, r), 9000), rng)


def dk_ringht2(rng):
    return hats(lambda d, r: hp(ring(osc(5200, d), osc(7900, d)) + 0.4 * noise(d, r), 4500), rng)


def dk_airhat(rng):
    def a_(d, r):
        return hp(noise(d, r), 6000) * np.minimum(1, t_(d) / 0.012)
    return hats(a_, rng, tc=0.04, to=0.2)


def dk_crshcym(rng):
    def c_(d, r):
        return hp(0.7 * noise(d, r) + 0.6 * _metal(d, 390, r), 3500)
    return hats(c_, rng, tc=0.05, to=0.6)


def dk_tinhat(rng):
    return hats(lambda d, r: hp(comb(noise(d, r), 0.00021, 0.8, 0.1), 3000), rng)


A2, A3, A4 = 110.0, 220.0, 440.0


def chord(fs, d, fn=saw, det=0.0):
    return sum(fn(f * (1 + det * k), d) for k in (-1, 1) for f in fs) / len(fs)


def dk_stab(rng):
    d = 0.30
    x = chord([A3, A3 * 1.1892, A3 * 1.4983], d, det=0.004)
    return rlp(x, 300 + 4000 * np.exp(-t_(d) / 0.04), 2.5) * expenv(d, 0.09)


def dk_dubstb(rng):
    d = 0.30
    x = chord([A3, A3 * 1.1892, A3 * 1.4983, A3 * 1.7818], d, det=0.003)
    y = lp(x, 900) * expenv(d, 0.06)
    return y + 0.5 * np.roll(y, int(0.075 * FS)) * (t_(d) > 0.075)


def dk_acidpl(rng):
    d = 0.28
    return sat(rlp(saw(A2, d), 150 + 3500 * np.exp(-t_(d) / 0.05), 8) * expenv(d, 0.1), 2.5)


def dk_reese(rng):
    d = 0.30
    return lp(chord([A2], d, det=0.012) + 0.6 * osc(A2 / 2, d), 600) * expenv(d, 0.12, att=0.004)


def dk_fmbell2(rng):
    d = 0.30
    return fm(A3, 1.4, 5 * np.exp(-t_(d) / 0.06), d) * expenv(d, 0.09)


def dk_blip(rng):
    d = 0.10
    return lp(sq(A4 * 2, d), 5000) * expenv(d, 0.02) * 0.6


def dk_pluckd(rng):
    return lp(karplus(A2, 0.30, rng, decay=0.992, bright=0.2), 1500) * expenv(0.30, 0.1, att=0)


def dk_organ(rng):
    d = 0.24
    x = sq(A3, d) + 0.5 * sq(A3 * 2, d) + 0.7 * osc(A3 / 2, d)
    return lp(x, 2200) * expenv(d, 0.07, att=0.002) * 0.6


def dk_metalt(rng):
    d = 0.30
    return ring(osc(A3, d), osc(A3 * 2.76, d)) * expenv(d, 0.08) + 0.4 * osc(A3, d) * expenv(d, 0.1)


def dk_subtone(rng):
    d = 0.30
    return osc(A2 / 2 * (1 + 0.5 * np.exp(-t_(d) / 0.008)), d) * expenv(d, 0.15) + 0.2 * hp(noise(d, rng), 3000) * expenv(d, 0.002)


def dk_hoover(rng):
    d = 0.30
    f = A3 * (1 - 0.06 * np.exp(-t_(d) / 0.05))
    return lp(chord([f, f * 0.5], d, det=0.01), 2500) * expenv(d, 0.12, att=0.005)


def dk_gltchtn(rng):
    d = 0.28
    x = crush(fm(A3, 2.0, 2.0, d) * expenv(d, 0.09), 4, 3)
    return stutter(x, 0.025, 2, 0.01)[: int(FS * 0.30)]


def dk_chordm(rng):
    d = 0.30
    fs = [A3, A3 * 1.1892, A3 * 1.4983, A3 * 2.2449]
    return sum(fm(f, 1.0, 1.5 * np.exp(-t_(d) / 0.05), d) for f in fs) / 4 * expenv(d, 0.09)


def dk_noisetn(rng):
    d = 0.30
    x = comb(lp(noise(d, rng), 2500) * expenv(d, 0.01), 1 / A3, 0.97, 0.5)
    return lp(hp(x, 100), 3000) * expenv(d, 0.12, att=0)


def dk_formnt(rng):
    d = 0.28
    s = saw(A2, d)
    return (bp(s, 350, 550) + 0.6 * bp(s, 750, 1000)) * expenv(d, 0.1, att=0.003)


def dk_sinepng(rng):
    d = 0.24
    return osc(A4, d) * expenv(d, 0.05)


def dk_laser(rng):
    d = 0.18
    return sq(sweep(4000, 200, 0.03, d), d) * expenv(d, 0.05) * 0.5


def dk_gltch1(rng):
    d = 0.24
    x = np.zeros(int(FS * d))
    o = 0
    while o < len(x) - 200:
        n = int(rng.uniform(0.004, 0.02) * FS)
        k = rng.integers(3)
        s = (noise(n / FS, rng) if k == 0 else sq(rng.uniform(200, 4000), n / FS) if k == 1 else osc(rng.uniform(100, 3000), n / FS))
        e = min(len(x), o + len(s))
        x[o:e] = s[: e - o] * (rng.random() > 0.3)
        o += n
    return crush(x, 4, 2) * expenv(d, 0.08, att=0)


def dk_click(rng):
    d = 0.04
    x = np.zeros(int(FS * d))
    x[:4] = [1, -1, 0.6, -0.3]
    return hp(x, 1000) * 0.5 + osc(3100, d) * expenv(d, 0.004) + 0.4 * osc(6900, d) * expenv(d, 0.002)


def dk_bleep(rng):
    d = 0.14
    f = np.where(t_(d) < 0.05, 2000, 1500)
    return sq(f, d) * expenv(d, 0.05) * 0.5


def dk_data(rng):
    d = 0.20
    f = np.repeat(rng.choice([600, 1200, 1800, 2400, 3600], int(d * 250) + 1), int(FS / 250))[: int(FS * d)]
    return sq(f, d) * expenv(d, 0.09) * 0.5


def dk_static2(rng):
    d = 0.20
    x = np.where(rng.random(int(FS * d)) > 0.985, rng.uniform(-1, 1, int(FS * d)), 0.0)
    return bp(x, 1000, 10000) * expenv(d, 0.08, att=0)


def dk_drop(rng):
    d = 0.16
    return sq(sweep(800, 100, 0.03, d), d) * expenv(d, 0.06) * 0.5


def dk_rise(rng):
    d = 0.12
    return sq(200 * np.exp(t_(d) / 0.04), d) * np.minimum(1, t_(d) / 0.08) * (t_(d) < 0.115) * 0.5


def dk_tink(rng):
    d = 0.12
    return fm(3520, 3.01, 2.5 * np.exp(-t_(d) / 0.01), d) * expenv(d, 0.02)


def dk_buzz(rng):
    d = 0.10
    return crush(sq(100, d) + 0.5 * sq(151, d), 3, 4) * expenv(d, 0.05, att=0) * 0.5


def dk_scratch(rng):
    d = 0.22
    n = noise(d, rng)
    c = 1000 + 2500 * (0.5 + 0.5 * osc(9, d))
    return rlp(n, c, 4) * expenv(d, 0.1, att=0.01)


def dk_bonk(rng):
    d = 0.14
    return osc(sweep(900, 560, 0.01, d), d) * expenv(d, 0.025) + 0.3 * bp(noise(d, rng), 600, 2000) * expenv(d, 0.003)


def dk_spring(rng):
    d = 0.30
    exc = lp(noise(d, rng), 3000) * expenv(d, 0.003)
    y = np.zeros(len(exc))
    for n in range(len(exc)):
        dl = int(FS * (0.007 + 0.003 * np.sin(2 * np.pi * 6 * n / FS)))
        y[n] = exc[n] + (0.9 * y[n - dl] if n >= dl else 0)
    return lp(hp(y, 150), 4000) * expenv(d, 0.1, att=0)


def dk_ratcht(rng):
    d = 0.16
    x = np.zeros(int(FS * d))
    for k in range(4):
        o = int(k * 0.028 * FS)
        s = hp(noise(0.01, rng), 2000) * expenv(0.01, 0.0015)
        x[o:o + len(s)] += s * (1 - 0.18 * k)
    return x


def dk_crack(rng):
    d = 0.10
    return sat(hp(noise(d, rng), 3000) * expenv(d, 0.006, att=0), 8)


def dk_cowdrk(rng):
    d = 0.24
    x = sq(400, d) + sq(595, d)
    return lp(bp(x, 500, 2000), 1800) * (0.6 * expenv(d, 0.01) + 0.4 * expenv(d, 0.07)) * 0.5


DARK = [
    ("KICK", "K909", dk_k909, "909-ish: sine 280 -> 50 Hz, a click, a little drive"),
    ("KICK", "RUMBLE", dk_rumble, "kick plus a low, smeared rumble tail (techno)"),
    ("KICK", "DISTKIK", dk_distkik, "170 -> 44 Hz kick, driven hard, 3 kHz low pass"),
    ("KICK", "HARDKIK", dk_hardkik, "fast 420 Hz attack, folded and driven"),
    ("KICK", "SUBKIK", dk_subkik, "deep 90 -> 42 Hz sub, round"),
    ("KICK", "DUSTKIK", dk_dustkik, "lo-fi: 6-bit, 1/4 sample-hold, dust"),
    ("KICK", "TAPEKIK", dk_tapekik, "saturated, wobbling, 700 Hz low pass: muffled"),
    ("KICK", "BOXKIK", dk_boxkik, "short tight 68 Hz with a knock (minimal)"),
    ("KICK", "FMDARK", dk_fmdark, "FM ratio 0.5 thud, 150 -> 40 Hz"),
    ("KICK", "GLTCHKK", dk_gltchkk, "the attack stuttered 3 times, then the kick"),
    ("KICK", "BITKIK", dk_bitkik, "3-bit, 1/10 sample-hold: aliasing kick"),
    ("KICK", "INDKIK", dk_indkik, "driven kick plus a 91 Hz metal comb (industrial)"),
    ("KICK", "ACIDKK", dk_acidkk, "saw through a resonant sweep plus a sine body"),
    ("KICK", "DEEPKK", dk_deepkk, "long 909, 400 Hz low pass: dark and round"),
    ("KICK", "CLIPKIK", dk_clipkik, "clipped to a square-ish sub, short"),
    ("KICK", "ZAPKIK", dk_zapkik, "psy zap: 2.4 kHz -> 48 Hz in 6 ms"),
    ("SNARE", "CLAP9", dk_clap9, "909-ish clap: four bursts, 0.95-3.2 kHz"),
    ("SNARE", "DRKCLAP", dk_drkclap, "low dark clap 500-1600 Hz, longer tail"),
    ("SNARE", "SN909", dk_sn909, "909-ish snare: two tones + noise"),
    ("SNARE", "NOISESN", dk_noisesn, "tight high-passed noise, 6-bit"),
    ("SNARE", "RIMSHT", dk_rimsht, "synth rim: 1.7 kHz + 480 Hz, a click"),
    ("SNARE", "METLSN", dk_metlsn, "noise through three combs: metallic, industrial"),
    ("SNARE", "DISTSN", dk_distsn, "snare driven hard"),
    ("SNARE", "GATESN", dk_gatesn, "gated tail, cut at 170 ms"),
    ("SNARE", "GLTCHSN", dk_gltchsn, "crushed snare, attack stuttered"),
    ("SNARE", "BITSN", dk_bitsn, "2-bit noise and square"),
    ("SNARE", "SNAP", dk_snap, "short snap / click"),
    ("SNARE", "REVSN", dk_revsn, "reverse swell, cut (sucks into the beat)"),
    ("SNARE", "TOMSN", dk_tomsn, "dark low tom-snare, 180 -> 115 Hz"),
    ("SNARE", "FMSNR", dk_fmsnr, "FM noise snare, high index"),
    ("SNARE", "STATIC", dk_static, "radio static burst with dropouts"),
    ("SNARE", "SHOTSN", dk_shotsn, "hard crack, driven noise"),
    ("HAT", "HAT909", dk_hat909, "metal partials + noise (closed / open)"),
    ("HAT", "DRKHAT", dk_drkhat, "darker metal, 4-9 kHz"),
    ("HAT", "NOISHAT", dk_noishat, "plain high noise"),
    ("HAT", "TICK", dk_tick, "tiny ticks"),
    ("HAT", "SIZZLE", dk_sizzle, "4-bit noise sizzle"),
    ("HAT", "RIDE", dk_ride, "FM ride ping / ride"),
    ("HAT", "SHKHAT", dk_shkhat, "shaker-ish double hit"),
    ("HAT", "GLTCHHT", dk_gltchht, "909-ish hat, stuttered"),
    ("HAT", "BITHAT", dk_bithat, "1-bit, 1/7 sample-hold"),
    ("HAT", "CHNHAT", dk_chnhat, "a scatter of tiny metal hits (chains)"),
    ("HAT", "DUSTHT", dk_dustht, "vinyl crackle"),
    ("HAT", "FMHAT2", dk_fmhat2, "FM 8.3 kHz, ratio 1.41"),
    ("HAT", "RINGHT2", dk_ringht2, "5.2 x 7.9 kHz ring-mod"),
    ("HAT", "AIRHAT", dk_airhat, "breathy, soft attack"),
    ("HAT", "CRSHCYM", dk_crshcym, "cymbal: noise + metal, long open"),
    ("HAT", "TINHAT", dk_tinhat, "noise through a 0.21 ms comb: tinny"),
    ("TIE", "STAB", dk_stab, "A minor stab, resonant filter snap"),
    ("TIE", "DUBSTB", dk_dubstb, "Am7 dub-techno chord, 900 Hz, an echo"),
    ("TIE", "ACIDPL", dk_acidpl, "303-ish: saw A2, resonant sweep, driven"),
    ("TIE", "REESE", dk_reese, "detuned saws A2, dark"),
    ("TIE", "FMBELL2", dk_fmbell2, "dark FM bell, ratio 1.4, A3"),
    ("TIE", "BLIP", dk_blip, "square blip A5"),
    ("TIE", "PLUCKD", dk_pluckd, "dark pluck A2"),
    ("TIE", "ORGAN", dk_organ, "short organ, A3 + sub"),
    ("TIE", "METALT", dk_metalt, "A3 x 2.76 ring: gong-ish"),
    ("TIE", "SUBTONE", dk_subtone, "pitched sub hit A1"),
    ("TIE", "HOOVER", dk_hoover, "detuned saws with a dip (rave)"),
    ("TIE", "GLTCHTN", dk_gltchtn, "crushed FM tone, stuttered"),
    ("TIE", "CHORDM", dk_chordm, "A minor 9, soft FM"),
    ("TIE", "NOISETN", dk_noisetn, "noise tuned to A3 by a comb"),
    ("TIE", "FORMNT", dk_formnt, "vowel-ish 'o', saw A2"),
    ("TIE", "SINEPNG", dk_sinepng, "pure sine ping A4"),
    ("POD", "LASER", dk_laser, "square zap 4 kHz -> 200 Hz"),
    ("POD", "GLTCH1", dk_gltch1, "random micro-chops of noise and tones"),
    ("POD", "CLICK", dk_click, "a digital click"),
    ("POD", "BLEEP", dk_bleep, "two-note 1-bit bleep"),
    ("POD", "DATA", dk_data, "modem data burst"),
    ("POD", "STATIC2", dk_static2, "crackle burst"),
    ("POD", "DROP", dk_drop, "square 800 -> 100 Hz drop"),
    ("POD", "RISE", dk_rise, "square 200 Hz rising fast"),
    ("POD", "TINK", dk_tink, "high FM tink"),
    ("POD", "BUZZ", dk_buzz, "crushed 100 Hz buzz"),
    ("POD", "SCRATCH", dk_scratch, "scratch-ish wobbling filter noise"),
    ("POD", "BONK", dk_bonk, "synth wood block"),
    ("POD", "SPRING", dk_spring, "spring boing (wobbling comb)"),
    ("POD", "RATCHT", dk_ratcht, "four fast clicks"),
    ("POD", "CRACK", dk_crack, "driven noise crack"),
    ("POD", "COWDRK", dk_cowdrk, "dark 808-ish cowbell"),
]


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
CANDIDATES += DARK      # appended: the first 24 keep their numbers (and seeds)


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
