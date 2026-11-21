#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# KALK's tests that need no toolchain and no FM-1: the voices, the demo, MIDI, the panel walkthrough.
#   sh tests/run_host_tests.sh        (tests/run_tests.sh adds the platform's: the update path, the packages)
set -e
cd "$(dirname "$0")/.."
mkdir -p build/host
fail=0
run() { echo "== $1"; shift; "$@" || fail=1; }
cc -O2 -Wall -Wno-unused-function -Ifirmware/src -o build/host/voice_test tests/voice_test.c -lm
run "voices: every voice, every WAV variant sounds, stays under full scale and ends" build/host/voice_test
cc -O2 -Wall -Wno-unused-function -Ifirmware/src -o build/host/fx_test tests/fx_test.c -lm
run "effects: reverb, reson delay (0.2's bit for bit at rest), GATE; 0.2 projects load" build/host/fx_test
cc -O2 -Wall -Wno-unused-function -Ifirmware/src -o build/host/seq_test tests/seq_test.c -lm
run "sequencer: the RMX LFO moves the kick within its DEPTH, 0.2's timing at depth 0, LANES" build/host/seq_test
run "the demo: patterns, NOW, an XFX sweep, the pages" sh tests/run_host.sh tests/scripts/demo.txt
run "MIDI: notes play the voices, a clock sets tempo and phase, start / stop" sh tests/kalk_midi_check.sh
run "the panel: steps, accent, values, stretch, chain, copy, mute, open hat, motion, save / clear / load" \
    sh tests/kalk_ui_check.sh
run "kits: each empty slot its own kit, random kits (SEL + HOME) bounded, SEL + GLO back" sh tests/kalk_kit_check.sh
[ "$fail" = 0 ] && echo "HOST TESTS PASSED" || echo "SOME HOST TESTS FAILED"
exit "$fail"
