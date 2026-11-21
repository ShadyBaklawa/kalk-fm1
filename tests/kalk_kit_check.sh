#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# The starter kits and the random kit, measured on the host (Python's standard library + Pillow, as in CI):
# tests/scripts/kits.txt (each empty slot's kit, two random kits, SEL + GLO back) and tests/scripts/rolls.txt
# (24 random kits while it plays).
set -e
cd "$(dirname "$0")/.."
sh tests/run_host.sh tests/scripts/kits.txt >/dev/null
cat > build/host/kit_measure.py <<'PY'
import wave, array, math
def load():
    w = wave.open("build/host/kalk.wav"); r = w.getframerate()
    a = array.array("h", w.readframes(w.getnframes()))
    return [(a[i] + a[i + 1]) / 65536 for i in range(0, len(a), 2)], r
def part(x, r, t0, t1): return x[int(t0 / 1000 * r): int(t1 / 1000 * r)]
def feat(s, r):
    """per 16th (124 BPM): the level (dB) of the low end (< ~150 Hz), the whole and the top end (the slope)"""
    n = int(r * 60 / 124 / 4); f = []; lp = prev = 0.0; k = 1 - math.exp(-2 * math.pi * 150 / r)
    for i in range(0, len(s) - n, n):
        el = ea = eh = 1e-9
        for v in s[i:i + n]:
            lp += k * (v - lp); el += lp * lp; ea += v * v; eh += (v - prev) ** 2; prev = v
        f += [10 * math.log10(e / n) for e in (el, ea, eh)]
    return f
def dist(a, b): return math.sqrt(sum((p - q) ** 2 for p, q in zip(a, b)) / len(a))
def rms_db(s): return 10 * math.log10(sum(v * v for v in s) / len(s) + 1e-12)
def peak(s): return max(abs(v) for v in s)
PY
python3 - <<'PY'
import sys
sys.path.insert(0, "build/host")
from kit_measure import *
from PIL import Image
ok = True
def check(name, cond):
    global ok
    print(("ok   " if cond else "FAIL ") + name); ok &= bool(cond)
x, r = load()
starts = [200 + i * 5000 for i in range(8)] + [40200, 45200, 50200]
S = [part(x, r, b + 800, b + 4300) for b in starts]
F = [feat(s, r) for s in S]
close = min(dist(F[i], F[j]) for i in range(8) for j in range(i + 1, 8))
check("the 8 slots' kits all sound different (closest %.1f dB apart > 2)" % close, close > 2)
lv = [rms_db(s) for s in S[:8]]
check("their levels within 8 dB of each other (%.1f .. %.1f dB)" % (min(lv), max(lv)), max(lv) - min(lv) < 8)
check("two rolls: two different kits (%.1f dB apart)" % dist(F[8], F[9]), dist(F[8], F[9]) > 2)
check("SEL + GLO: back to the first roll (%.1f dB from it, %.1f from the second)" % (dist(F[10], F[8]), dist(F[10], F[9])),
      dist(F[10], F[8]) < 1.5 < dist(F[10], F[9]))
def top(ms): return Image.open("build/host/kalk-%d.png" % ms).crop((60, 0, 420, 32)).tobytes()
check("each slot's kit names itself on the top line", len({top(b + 300) for b in starts[:8]}) == 8)
sys.exit(0 if ok else 1)
PY
sh tests/run_host.sh tests/scripts/rolls.txt >/dev/null
python3 - <<'PY'
import sys
sys.path.insert(0, "build/host")
from kit_measure import *
x, r = load()
bars = [part(x, r, 1000 + i * 1935 + 100, 1000 + (i + 1) * 1935) for i in range(24)]
pk = max(peak(b) for b in bars); quiet = min(rms_db(b) for b in bars)
good = pk < 0.99 and quiet > -32
print(("ok   " if good else "FAIL ") + "24 random kits while playing: every bar heard (quietest %.1f dB RMS), none clips (loudest peak %.2f)" % (quiet, pk))
sys.exit(0 if good else 1)
PY
