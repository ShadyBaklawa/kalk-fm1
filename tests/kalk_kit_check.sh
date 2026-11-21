#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# The starter kits and the random kit, measured on the host: tests/scripts/kits.txt (each empty slot's kit, two
# random kits, SEL + GLO back) and tests/scripts/rolls.txt (24 random kits while it plays).
set -e
cd "$(dirname "$0")/.."
sh tests/run_host.sh tests/scripts/kits.txt >/dev/null
python3 - <<'PY'
import sys, wave, numpy as np
from PIL import Image
ok = True
def check(name, cond):
    global ok
    print(("ok   " if cond else "FAIL ") + name); ok &= bool(cond)
w = wave.open("build/host/kalk.wav"); r = w.getframerate()
x = np.frombuffer(w.readframes(w.getnframes()), np.int16).reshape(-1, 2).mean(1) / 32768
starts = [200 + i * 5000 for i in range(8)] + [40200, 45200, 50200]
def feat(b):
    s = x[int((b + 800) / 1000 * r): int((b + 4300) / 1000 * r)]
    n = 4096; fr = [np.abs(np.fft.rfft(s[i:i + n] * np.hanning(n))) for i in range(0, len(s) - n, n // 2)]
    m = np.log10(np.mean(fr, 0) + 1e-7); e = np.geomspace(4, 2000, 25)
    return np.array([m[int(a):int(b)].mean() for a, b in zip(e[:-1], e[1:])]), np.sqrt((s ** 2).mean())
F = [feat(b) for b in starts]
D = lambda i, j: np.sqrt(((F[i][0] - F[j][0]) ** 2).mean())
check("the 8 slots' kits all sound different (closest %.2f > 0.08)" % min(D(i, j) for i in range(8) for j in range(i + 1, 8)),
      min(D(i, j) for i in range(8) for j in range(i + 1, 8)) > 0.08)
lv = [20 * np.log10(f[1]) for f in F[:8]]
check("their levels within 8 dB of each other (%.1f .. %.1f dB)" % (min(lv), max(lv)), max(lv) - min(lv) < 8)
check("two rolls: two different kits", D(8, 9) > 0.08)
check("SEL + GLO: back to the first roll (%.3f)" % D(10, 8), D(10, 8) < 0.03)
def top(ms): return Image.open("build/host/kalk-%d.png" % ms).crop((60, 0, 420, 32)).tobytes()
check("each slot's kit names itself on the top line", len({top(b + 300) for b in starts[:8]}) == 8)
sys.exit(0 if ok else 1)
PY
sh tests/run_host.sh tests/scripts/rolls.txt >/dev/null
python3 - <<'PY'
import sys, wave, numpy as np
w = wave.open("build/host/kalk.wav"); r = w.getframerate()
x = np.frombuffer(w.readframes(w.getnframes()), np.int16).reshape(-1, 2) / 32768
bars = [x[int((1000 + i * 1935 + 100) / 1000 * r): int((1000 + (i + 1) * 1935) / 1000 * r)] for i in range(24)]
pk = [np.abs(b).max() for b in bars]; rms = [20 * np.log10(np.sqrt((b ** 2).mean()) + 1e-9) for b in bars]
good = max(pk) < 0.99 and min(rms) > -32
print(("ok   " if good else "FAIL ") + "24 random kits while playing: every bar heard (quietest %.1f dB RMS), none clips (loudest peak %.2f)" % (min(rms), max(pk)))
sys.exit(0 if good else 1)
PY
