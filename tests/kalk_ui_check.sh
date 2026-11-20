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
RING_OUT = [98, 87, 76, 65, 54, 43]; RING_IN = [88, 77, 66, 55, 44, 33]
def shot(ms): return Image.open("build/host/kalk-%d.png" % ms).convert("RGB")   # (480 x 480: the screen x 2)
def px(im, lane, step, r=None, n=16):
    """the pixel of a wedge: lane ring, step of n, at radius r (the middle by default)"""
    r = r if r is not None else (RING_IN[lane] + RING_OUT[lane]) / 2
    t = (step + 0.5) / n * 2 * math.pi
    return im.getpixel((int(2 * (120 + r * math.sin(t))), int(2 * (120 - r * math.cos(t)))))
def lum(c): return max(c)
def on(im, lane, step): return lum(px(im, lane, step)) > 80
def row(im, lane): return "".join("#" if on(im, lane, s) else "." for s in range(16))
def peel(im): return im.getpixel((180, 40))     # (the peel at about 11 o'clock)
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
sys.exit(0 if ok else 1)
PY
