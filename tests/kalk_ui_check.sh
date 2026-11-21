#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# KALK's panel on the host (tests/scripts/ui.txt), checked on the screen: what each press should have drawn.
set -e
cd "$(dirname "$0")/.."
rm -f build/host/kalk-*.png
sh tests/run_host.sh tests/scripts/ui.txt >/dev/null
python3 - <<'PY'
import math, sys
from PIL import Image
RING_OUT = [85, 75, 66, 56, 47, 37]; RING_IN = [76, 67, 57, 48, 39, 29]; CY = 104   # (kalk.c: RING_*, SCR_CY)
def shot(ms): return Image.open("build/host/kalk-%d.png" % ms).convert("RGB")   # (480 x 480: the screen x 2)
def px(im, lane, step, r=None, n=16):
    """the pixel of a wedge: lane ring, step of n, at radius r (the middle by default)"""
    r = r if r is not None else (RING_IN[lane] + RING_OUT[lane]) / 2
    t = (step + 0.5) / n * 2 * math.pi
    return im.getpixel((int(2 * (120 + r * math.sin(t))), int(2 * (CY - r * math.cos(t)))))
def lum(c): return max(c)
def on(im, lane, step): return lum(px(im, lane, step)) > 80
def row(im, lane): return "".join("#" if on(im, lane, s) else "." for s in range(16))
def peel(im): return im.getpixel((int(2 * (120 - 88 * math.sin(0.35))), int(2 * (CY - 88 * math.cos(0.35)))))   # (the peel at about 11 o'clock)
ok = True
def check(name, cond):
    global ok
    print(("ok   " if cond else "FAIL ") + name); ok &= cond
a = shot(1100)
check("tap: snare step 2 on", on(a, 1, 1))
check("hold: snare step 3 accented (a white edge)", min(px(a, 1, 2, RING_OUT[1] - 1.5)) > 240 and min(px(a, 1, 1, RING_OUT[1] - 1.5)) < 200)
o = shot(5500)
check("SEL + tap: hat step 1 is an open hat (hollow)", lum(px(o, 2, 0, RING_OUT[2] - 2)) > 120 and lum(px(o, 2, 0, RING_IN[2] + 2)) < 40)
check("A copied onto D: the kick of D is A's", row(shot(3000), 0) == "#...#...#...#...")
m = px(shot(3300), 0, 0)
check("ENV mutes the kick: its steps grey", abs(m[0] - m[1]) < 20 and abs(m[1] - m[2]) < 20)
check("lane key + REC clears the kick of D", row(shot(7800), 0) == "." * 16)
check("SEL + SAVE loads the saved project: D's cleared kick is back", row(shot(8190), 0) == "#...#...#...#...")
check("NOW held: the peel turns purple", peel(shot(6100))[2] > peel(shot(6100))[1])
check("NOW let go: the peel is green again", peel(shot(6300))[1] > peel(shot(6300))[2])
check("the mod lane shows its own length: 12 wedges", lum(px(shot(1100), 5, 0, None, 12)) > 0)

# the FX pages (tests/scripts/pages.txt): the top line names the page
import subprocess, glob, os
for f in glob.glob("build/host/kalk-*.png"): os.remove(f)
subprocess.run(["sh", "tests/run_host.sh", "tests/scripts/pages.txt"], stdout=subprocess.DEVNULL, check=True)
def bottom(ms): return shot(ms).crop((0, 0, 480, 32))   # (the top line: where we are)
def flagged(im): return sum(1 for c in list(zip(*[iter(im.tobytes())] * 3)) if (c[0] > 200 and c[1] < 120) or (c[1] > 200 and c[2] < 120))   # red or lime text
SH = [int(l.split()[0]) for l in open("tests/scripts/pages.txt") if l.split()[1:2] == ["shot"]]   # (its shots, in order)
check("FX pressed once per FX group (7): back on the XFX page", list(bottom(SH[0]).tobytes()) == list(bottom(SH[1]).tobytes()))
check("FX pressed 6 more times: another group (MASTER: EQ), not XFX", bottom(SH[2]).tobytes() != bottom(SH[0]).tobytes())
check("REC on the EQ page: no motion recording", flagged(bottom(SH[3])) == 0)
check("REC on the XFX page: REC MOTION", flagged(bottom(SH[4])) > 20)
g = shot(SH[5])
c3, c1 = px(g, 5, 2), px(g, 5, 0)
check("ARP three times: REMIX > GATE; a white key cuts step 3 (red on the inner ring), step 1 stays open", c3[0] > 200 and c3[1] < 100 and c1[0] < 90)
check("a lane key on an FX page: still the FX page", bottom(SH[6]).tobytes() == bottom(SH[7]).tobytes())
t = shot(SH[8]).getpixel((2 * 44, 2 * 80)); u = shot(SH[8]).getpixel((2 * 120, 2 * 36))
check("TOR ANIM: white key 4 lights box 4 (CUTOFF) orange, box 2 stays dark", t[0] > 200 and 100 < t[1] < 180 and t[2] < 80 and u[0] < 90)
sys.exit(0 if ok else 1)
PY
