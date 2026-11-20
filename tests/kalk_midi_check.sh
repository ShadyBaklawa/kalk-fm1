#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# KALK's MIDI on the host: notes sound, START plays at the clock's tempo, STOP stops.
set -e
cd "$(dirname "$0")/.."
sh tests/run_host.sh tests/scripts/midi.txt >/dev/null
python3 - <<'PY'
import array, sys
a = array.array('h'); a.frombytes(open("build/host/kalk.wav", "rb").read()[44:])
mono = [abs(a[i]) + abs(a[i + 1]) for i in range(0, len(a), 2)]
def peak(t0, t1): return max(mono[int(t0 * 44.1):int(t1 * 44.1)])
ok = True
def check(name, cond):
    global ok
    print(("ok   " if cond else "FAIL ") + name); ok &= cond
check("silent before the first note", peak(0, 480) < 50)
check("note 36 plays the kick", peak(500, 900) > 3000)
check("note 38 plays the snare", peak(1000, 1400) > 2000)
check("note 62 plays the tie", peak(1500, 1950) > 2000)
check("START plays", peak(2050, 3000) > 3000)
check("STOP: silent after the tails", peak(8000, 8500) < 50)
# the kicks while playing (a 150 Hz low pass finds them): four on the floor at 140 BPM -> 428.6 ms apart
lp, y = [], 0.0
for i in range(0, len(a), 2):
    y += (a[i] + a[i + 1] - y) * 0.021
    lp.append(abs(y))
def hit_near(ms):                               # a kick rises within 12 ms of ms
    return max(lp[int((ms - 12) * 44.1):int((ms + 12) * 44.1)]) > 8000 and lp[int((ms - 30) * 44.1)] < lp[int((ms + 10) * 44.1)]
first = next(i for i in range(int(1980 * 44.1), int(2100 * 44.1)) if lp[i] > 8000) / 44.1
beats = sum(hit_near(first + k * 60000 / 140) for k in range(9))
check("the clock sets the tempo: %d of 9 beats on the 140 BPM grid" % beats, beats >= 8)
sys.exit(0 if ok else 1)
PY
