# KALK

**A beat composer for the M-VAVE FM-1**, after the workflow of Native Instruments' *Limelite* (Reaktor). Five
drum and tone voices, each with 16 synthesised variants and 8 CC0 samples, a three-lane main sequence in four patterns, a second window of lanes that loop on
their own lengths, 127 remix variations, an XY effect with recorded motion, a tempo-synced delay and a maximiser.

*Kalk* is lime, the stone. KALK is an independent work: no code, samples or artwork of
Limelite are in it. The sounds are synthesised, or CC0 recordings from Versilian Studios' VCSL.

<p align="center"><img src="docs/screen.png" width="240" alt="KALK's screen: a lime slice of six rings, one a lane, the pattern in the middle, the four knobs along the bottom"></p>

**The screen is a lime slice.** Six rings, one a lane: KICK outside, then SNARE, HAT, TIE, POD, MOD. Each ring is cut
into as many wedges as its lane has steps, so a 5-step POD visibly turns against a 16-step kick. Lit wedges are steps,
a white edge an accent, a hollow wedge an open hat; the selected lane glows. The peel is the playhead's track, purple
while the remix plays. The pattern sits in the middle. Along the bottom the four knobs, K1 to K4 left to right as they
sit on the panel: each one's name, value and a bar. Along the top the slot, where you are (or a message) and the tempo.

> **Status: 0.2, untested on hardware.** Everything below runs and is tested on the host (the real firmware source,
> compiled for a computer: sound, screen, keys, MIDI). The first install on an FM-1 is the test that is still missing.
> Install at your own risk. The way back to the official firmware is always one install away (below).

## Install

1. Get the way back ready: M-VAVE's stock firmware (FM-1 V15, "PC Firmware" at m-vave.com) or the
   [FM-1 Firmware Hub](https://thefirmwarehub.com)'s *Return to official V15*.
2. Get `kalk.fwsc` from this repository's Actions run ("packages" artifact) or its Releases.
3. Connect the FM-1 by USB and install it with any FM-1 web installer that takes a file (the Hub's, Felucca's), or:
   `python3 tools/fm1_install.py kalk.fwsc`
4. If it does not start: hold **OCT-** while switching on (USB rescue), and install another firmware.

## The panel

```
 black keys   F#3 KICK  G#3 SNARE  A#3 HAT  |  C#4 TIE  D#4 POD  |  F#4 MOD  G#4 A  A#4 B  |  C#5 C  D#5 D  |  F#5 NOW
 white keys   the 16 steps of the selected lane
```

| | |
| --- | --- |
| **white key** | tap: step on / off. Hold (350 ms): accent. **SEL** + tap on HAT: open hat. On REMIX > GATE and TORSION > ANIM the white keys do that page's job (the top line says so) |
| **TIE / POD / MOD step** | hold it and turn **K1** (it shows NOTE / VAL): its value (TIE, POD: -12..+12 semitones; MOD: 1..15) |
| **lane key** | choose the lane (stopped: you hear it); you stay on the page you're on. **OCT- / OCT+**: previous / next lane |
| **A B C D** | the pattern: playing, it changes at the end of the bar. Hold one and press another: copy |
| **NOW** (F#5) | the remix, while held |
| **PLAY** | start / stop. With MIDI clock (GLOBAL > SYNC MIDI): START / STOP from the clock, KALK follows its tempo and phase |
| **REC** | XFX pages: record the pad's motion (X / Y) over 4 bars. SEL + REC: forget it. Holding a lane key: clear that lane. REMIX page: NOW on / off |
| **HOME** | the selected lane's SOUND page |
| **EDIT** | the selected lane's pages: SOUND, FILTER, SHAPE, MOD, EXTRA, LANE (again: next; SEL + EDIT: back) |
| **FX** | the effects. Again: the next group (XFX, TORSION, FILTER, DELAY, REVERB, PULP, MASTER); SEL + FX: back |
| **SEQ, ARP** | sequence, remix (again: next page; SEL + it: back). **GLO**: global, with the CPU meter |
| **SELECT** | every page of the section, one by one. **K1-K4**: the four values along the bottom. **ALGORITHM**: tempo. **PRESETS**: the project slot (1-8) |
| **SAVE** | save the project to the slot (stopped). **SEL + SAVE**: load it |
| **ENV / LFO** | mute / solo the selected lane |

## The voices

All five have TUNE, DECAY, WAV (SYN 1..16: synthesised; then 8 samples by name), GAIN, CUT, RES, FILT (LP / BP / HP), PAN, ACC (how much softer an
unaccented hit is), MOD> and MOD (where the mod lane goes: WAV, PITCH, DECAY or CUT, and how much), and four of their own:

| | X1 | X2 | X3 | X4 | START |
| --- | --- | --- | --- | --- | --- |
| **KICK** | SAT | CLICK | SWEEP (the pitch drop's time) | HOLD | skips into the attack |
| **SNARE** | SAT | SNAPPY | FLAM | HOLD | softens the body |
| **HAT** (DECAY: closed) | OP DEC (open) | LINK: closed and open choke each other | NOISE | HOLD | |
| **TIE** (FM tone) | FM | RATIO (the modulator) | PORT (glide) | ATTACK | NOISE |
| **POD** (a struck tone) | ZAP | ZAPDEC | NOISE | HOLD | |

### The samples (WAV 17..24)

| | |
| --- | --- |
| KICK | FMTHUD, RINGKIK, CRSHKIK, COMBKIK, BDRUM, FRAME-M, TUMBA, LOG LO |
| SNARE | RINGSN, GRANSN, CRSHSN, COMBSN, GLTCHCL, SNARE, RIMSHOT, CLAP |
| HAT (closed / open) | FMHAT, TICKHAT, COMBHAT, GRANHAT, RINGHAT, HIHAT, SHAKER, TAMB |
| TIE (pitched by its steps, glides) | FMPING, RINGPNG, KARPLUS, GRANBEL, CRSHPNG, COWBELL, WOOD, CONGA |
| POD (ZAP bends them) | ZAPFM, COMBCLK, GLASS, GRANWOD, BITBLIP, CLAVES, BRAKE, TRIANGL |

A sample plays through the voice's filter, SAT, pitch and envelope: DECAY holds it for half its time, then releases
(Limelite's hold and release); START skips into it. The first ones in each row are synthesised here by
`tools/gen_vogel.py` (FM and ring-mod pings, bitcrushed ticks, comb clicks, granular smears); the rest are recordings
from [VCSL](https://github.com/sgossner/VCSL) by Versilian Studios. All CC0 1.0: `assets/samples-cc0/ATTRIBUTION.txt`.
`tools/gen_samples.py` rebuilds the banks (8-bit mu-law, in the app's flash); `python3 tools/gen_vogel.py` renders
the synthesised candidates to `build/vogel/` to audition.

### The EXTRA page (after MOD; TIE's LANE comes after it)

| | K1 | K2 | K3 | K4 |
| --- | --- | --- | --- | --- |
| **KICK**, **SNARE**: GRAIN (on the samples) | GRAIN: each grain starts up to ~200 ms further on (OFF: none) | SPEED: how fast the sample goes by, 0-198 % (100 %: as recorded); the pitch stays | SIZE: the grains, 5-86 ms | |
| **HAT**: 808 (on the synth hats) | 808: NOISE (as before) or 808: the six squares through two band passes, ~3.4 and ~7.1 kHz | TONE: moves both, about +-1.3 octaves | COLOR: from the low band to the high | SNAP: a sharper start |
| **TIE**: FM ENV (a richer FM drum) | FM ATK: the FM fades in (OFF: struck) | FM DEC: the FM's own decay (AUTO: from DECAY) | DROP: starts up to 3 octaves above and falls to the note | NOISFM: noise into the modulator |

At GRAIN OFF and SPEED 100 % the sample plays as before. A project saved by 0.2 or 0.3 loads with the EXTRA pages at rest.

## The sequence

- **Main**: KICK, SNARE, HAT in patterns A-D. SEQ > MAIN: LEN 1-16, START, SWING, HALF (half tempo).
- **Second window**: TIE, POD, MOD each loop on their own length (SEQ > LANES, or the lane's LANE page: LEN, STRTCH). STRTCH fits a
  lane to the main length: a lane of 3 plays 3 even steps across the bar.
- **Chain**: SEQ > CHAIN: 2-8 bars of A-D in any order (SLOT, PATT).
- **Remix** (ARP): REMIX 1-127 picks a variation of the main lanes (turned, backwards, half and double time, stutters,
  beats shuffled, holes, scattered); EVERY plays it every 8th / 4th / 2nd / every bar; SHIFT moves its start; LANES
  which of K S H it touches. NOW plays it while held.
- **Remix LFO** (ARP again): DEPTH 0-15 steps that an LFO pushes the play position of the main lanes ahead or back
  (steps get skipped, repeated, played backwards); RATE 1/4 bar .. 8 bars; SHAPE SINE, TRI, SAW UP, SAW DN, SQUARE,
  RANDOM (a new jump every step), DRUNK (a step at a time); LANES as above. DEPTH 0: off.
- **Gate** (ARP a third time): a 16-step lane that cuts the sound on its steps, while playing. On this page the white keys
  are its steps and the inner ring shows them in red. GATE: OFF, PRE FX (the echoes and the reverb ring on through the
  cuts) or POST FX (hard cuts); RATE 1/8, 1/16 or 1/32 (the 16 steps then run twice a bar: stutters); DEPTH (how much
  it cuts); SMOOTH (the edge: 0 clicks, higher is softer).

## The effects

FX's pages come in seven groups, in the order you'd reach for them; FX jumps to the next group. On each effect's first
page **K4 is how much of it, and 0 is off** (XFX MIX, TORSION BLEND, FOLLOW BEAT, DELAY SEND, REVERB SEND, PULP LEVEL).

| Group | Page | K1 | K2 | K3 | K4 |
| --- | --- | --- | --- | --- | --- |
| XFX | XFX | the effect | X: named for the effect | Y: named for the effect | MIX (dry / wet; OFF: off) |
| | XFX MOTION | MOTION: OFF, PLAY, SMOOTH | MOD>FX: the mod lane GATEs XFX, sets its MIX or its X | | |
| TORSION | TORSION | PRESET: WARM, LO-FI, RADIO, HOWL, GRID, RUBBER, SHRED, GHOST (sets the knobs; the SEED stays) | DRIVE | LEVEL (64: 1x) | BLEND (OFF: off) |
| | TOR CRUSH | BITS (16 .. 2) | S.RATE (the sample rate it holds to) | FOLD | CLIP |
| | TOR FILTER | CUTOFF | RES | NOT>BP (from a notch to a band pass) | FDBK (the end back into the filter: it howls) |
| | TOR ANIM | DEPTH (how far ANIM moves them, under or over the knob) | LENGTH (1-64 steps) | DENSTY (how many steps bring a new value) | SEED (which random sequence: the same every time) |
| | TOR MORE | SPREAD (stereo from mono) | N.GATE (a noise gate: shuts under it) | STEP (1/32 .. 1/4) | |
| FILTER | FILTER | CUT | RES | MOD (the mod lane) | TRK (follows the level) |
| | FOLLOW | SOURCE: MIX, KICK, K+SN, DRUMS | ATTACK | RELEAS | BEAT: the beat moves CUT: over 0 the hits open it, under 0 they close it (0: off) |
| DELAY | DELAY | L (16ths; 0: CHORUS, ~8 ms) | R (16ths; 0: CHORUS) | FDBK | SEND |
| | DELAY 2 | DAMP | WIDTH (cross feedback: ping-pong) | INVERT (the feedback) | |
| | RESON | FREQ (the filter in the echoes' loop) | RES | FILT (LP, BP, HP) | LO CUT (going in) |
| | TAPE LFO | LFO (1/16 .. 4 bars, synced; L and R a quarter apart) | DMOD (on the time: wobble, or a flanger at 0) | FM (on the cutoff) | |
| REVERB | REVERB | TIME (RT60 0.12-21.6 s) | SIZE | DAMP (the highs die first) | SEND (from the mix) |
| | REVERB 2 | LOW (cuts the lows going in: keeps the kick dry) | MOD (wobble, for a metallic shimmer) | WIDTH (0: mono) | DLY>RV (the echoes into the reverb) |
| PULP | PULP | LO HZ | HI HZ (the pitch it listens for) | DECAY | LEVEL (OFF: off) |
| | PULP 2 | WAVE (sine, triangle, square) | TRIG: what strikes it: MIX (its onsets), KICK, K+SN, DRUMS | GLIDE (how fast it follows the pitch) | PAN |
| MASTER | EQ | LO | MID | HI | AMP |
| | OUT | GAIN | PITCH (+-12, all voices) | DIRT | NORM (the maximiser) |

On TOR ANIM the screen shows TORSION's first 12 knobs as boxes, one per white key: a key turns its box orange, and ANIM
moves it.

The delay is a tape-style reson delay: with RESON and TAPE LFO at rest it is the plain delay, and once the filter is in,
the echoes are soft-limited, so high RES and FDBK ring and sing rather than clip.

The reverb is a feedback network of four lines with four diffusers before it, at half rate. With both its sends at 0
it switches itself off once its tail has died away. Projects saved by 0.2 load with the reverb off.

XFX: THRU, CRUSH (BITS, S.RATE), BPF, LPF, HPF (CUTOFF, RES), RING (FREQ, WOBBLE), COMB (TIME, FDBK), REPEAT (LENGTH;
REPEAT on over the middle), FOLD (DRIVE, BIAS), GATE, PAN, FLANGE (RATE, DEPTH; tempo synced), and the glitch ones,
synced to the bar while playing (K2 and K3 show their names):

| XFX | X | Y |
|---|---|---|
| CHOP | LENGTH: how much of each 16th sounds (4 % .. all) | SHAPE: the cut's, curved, straight, held |
| SLICE | SIZE: 1/32, 1/16, 1/8 | PATTRN: one of 16 patterns of slices that play the last one backwards |
| FREEZE | LOOP: OFF (listening), then the last 1/64 .. 1/4 | SPEED: under 0 slows to a stop; over: speeds up |
| SPIN | SPEED: a rotating speaker, 1/16 .. 1 bar a turn | DEPTH (Doppler and level, L and R opposite) |
| REFLEX | CUTOFF: a resonant low pass, L and R swept in mirror, a beat a cycle | RES |
| PITCH | GRAIN: 12 .. 92 ms | PITCH: -12 .. +12 semitones |

PULP is LIME.lite's Automatik Perc: a percussion oscillator struck by the drums that plays at the pitch it hears
them at (between LO and HI), before the effects.

TORSION is an animated distortion after boscomac's Bit Torsion (Reaktor): its behaviour, our own code. On TOR ANIM the
white keys 1-12 pick which of BITS, S.RATE, DRIVE, CUTOFF, RES, NOT>BP, FDBK, FOLD, CLIP, SPREAD, N.GATE, LEVEL a random
step sequence moves (their boxes turn orange); it repeats every LENGTH steps, and a SEED always gives the same one, so a
project sounds the same when loaded again. Projects saved by 0.2 or 0.3 load with TORSION off.

Only the chosen XFX runs; SLICE, FREEZE and PITCH use REPEAT's buffer, so they cost no memory.

## MIDI

In (USB and the TRS jack), on CHAN (OMNI or 1-16): 36 kick, 38 / 40 snare, 42 / 44 closed hat, 46 open hat, 39 pod,
48-72 tie (60 = its pitch); velocity 100 and up is an accent. Clock, START, CONTINUE, STOP with SYNC MIDI. Out (OUT ON):
every hit as a note on channel 10 (or CHAN).

## Build

Linux x86-64 (macOS / Windows: Docker or WSL), Python 3 with Pillow, the JieLi toolchain and three files of the AC79
SDK, as for every FM-1 firmware of this family:

```sh
tools/get_toolchain.sh
git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/fw-AC79_AIoT_SDK
./build.sh                       # -> build/kalk.fwsc (identity FM-1_960)
./build.sh --release 0.1         # -> build/kalk-0.1.fwsc (FM-1_961)
```

Or push to GitHub: `.github/workflows/release.yml` builds it with the Firmware Hub's attested workflow
([zvenson/fm1-build](https://github.com/zvenson/fm1-build)).

Without the toolchain, on any computer with a C compiler:

```sh
sh tests/run_host_tests.sh       # the voices, the demo, MIDI, the panel: build/host/*.wav and screens
sh tests/run_host.sh tests/scripts/showcase.txt   # a minute of KALK -> build/host/kalk.wav
python3 tools/gen_tables.py      # after changing a table's range
```

The source: `firmware/src/kalk.c` (the panel, the screen, the project), `kalk_voice.c`, `kalk_seq.c`, `kalk_fx.c`,
`kalk_dsp.h`, `kalk_model.h`. What the platform gives: [docs/API.md](docs/API.md); the chip's rules:
[docs/RULES.md](docs/RULES.md). The GLOBAL page shows the audio render's CPU share on the device.

## Credits

Built on [Dinghy](https://github.com/zvenson/dinghy) (zvenson), on the platform of
[Felucca](https://github.com/hugelton/Felucca) (Leo Kuroshita, Hügelton Instruments) and
[SLOOP](https://github.com/isod89/sloop-fm1) (isod89). GPL-3.0-only; who made what: [LICENSING.md](LICENSING.md).
