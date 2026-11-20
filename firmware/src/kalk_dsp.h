/* SPDX-License-Identifier: GPL-3.0-only */
/* KALK: fixed-point DSP helpers. Integers only (docs/RULES.md): Q15 signals (+-32767), 32-bit phases, int64 multiplies
 * (fine on the chip), no 64-bit division. */
#pragma once
#include <stdint.h>
#include "kalk_tables.h"

#define MUL15(a, b) ((int32_t)(((int64_t)(a) * (int64_t)(b)) >> 15))
#define MUL16(a, b) ((int32_t)(((int64_t)(a) * (int64_t)(b)) >> 16))
#define ENV_ONE (1u << 30)                      /* envelopes: Q30 */

static inline int32_t clip15(int32_t x) { return x > 32767 ? 32767 : x < -32767 ? -32767 : x; }
static inline int32_t clampi(int32_t x, int32_t lo, int32_t hi) { return x < lo ? lo : x > hi ? hi : x; }

/* a sine of a 32-bit phase, Q15, table + linear interpolation */
static inline int32_t sin15(uint32_t ph)
{
    uint32_t i = ph >> 22;
    int32_t a = SINE_T[i], b = SINE_T[i + 1u], fr = (int32_t)((ph >> 7) & 0x7FFFu);
    return a + (((b - a) * fr) >> 15);
}
static inline int32_t tri15(uint32_t ph)        /* triangle, Q15 */
{
    int32_t x = (int32_t)(ph >> 16) - 32768;    /* -32768..32767: a saw */
    x = x < 0 ? -x : x;                         /* 0..32768 */
    return clip15((x << 1) - 32768);
}

/* white noise: xorshift32 */
static uint32_t k_rng = 0x2545F491u;
static inline int32_t noise15(void)
{
    k_rng ^= k_rng << 13;
    k_rng ^= k_rng >> 17;
    k_rng ^= k_rng << 5;
    return (int32_t)k_rng >> 17;               /* -16384..16383 */
}
static inline uint32_t rnd(void) { (void)noise15(); return k_rng; }

/* a soft clip with unity slope: x - 4/27 x^3 on [-1.5, 1.5] (full scale at 1.5), flat beyond */
static inline int32_t soft15(int32_t x)
{
    int64_t c;
    if (x > 49151) return 32767;
    if (x < -49151) return -32767;
    c = (((int64_t)x * x) >> 15) * x >> 15;     /* x^3, Q15 */
    return x - (int32_t)((c * 4855) >> 15);
}
/* the last word: straight up to -2.5 dB, then a quarter of what is over, clipped */
static inline int32_t limit15(int32_t x)
{
    if (x > 24576) x = 24576 + ((x - 24576) >> 2);
    else if (x < -24576) x = -24576 + ((x + 24576) >> 2);
    return clip15(x);
}
/* drive (Q8: 256 = unity, up to 16x) then the soft clip */
static inline int32_t drive15(int32_t x, int32_t drv) { return soft15((int32_t)(((int64_t)x * drv) >> 8)); }

/* envelope step: env * k >> 32 */
static inline uint32_t env_dec(uint32_t env, uint32_t k) { return (uint32_t)(((uint64_t)env * k) >> 32); }

/* ---- the state-variable filter, zero delay feedback (Simper / Cytomic), stable at every cutoff */
typedef struct { int32_t ic1, ic2, a1, a2, a3, k; } svf_t;
/* g: SVF_G[] (Q16); res 0..127 -> k from 2 (none) to ~0.03 (rings) */
static void svf_set(svf_t *f, uint32_t g16, uint32_t res)
{
    int32_t k15 = 65536 - (int32_t)((res > 127u ? 127u : res) * 508u);   /* 65536 .. 1020 */
    int64_t gk = ((int64_t)g16 * (int64_t)(g16 + (uint32_t)k15 * 2u)) >> 16;
    uint32_t den = 65536u + (uint32_t)(gk > 0x7F000000 ? 0x7F000000 : gk);
    f->a1 = (int32_t)(0x80000000u / den);       /* Q15 */
    f->a2 = (int32_t)(((int64_t)g16 * f->a1) >> 16);
    f->a3 = (int32_t)(((int64_t)g16 * f->a2) >> 16);
    f->k = k15;
}
static inline void svf_reset(svf_t *f) { f->ic1 = f->ic2 = 0; }
/* mode 0 low pass, 1 band pass, 2 high pass */
static inline int32_t svf_run(svf_t *f, int32_t v0, uint32_t mode)
{
    int32_t v3 = v0 - f->ic2;
    int32_t v1 = MUL15(f->a1, f->ic1) + MUL15(f->a2, v3);
    int32_t v2 = f->ic2 + MUL15(f->a2, f->ic1) + MUL15(f->a3, v3);
    f->ic1 = clampi(2 * v1 - f->ic1, -0x3FFFFFFF, 0x3FFFFFFF);
    f->ic2 = clampi(2 * v2 - f->ic2, -0x3FFFFFFF, 0x3FFFFFFF);
    if (mode == 0u) return v2;
    if (mode == 1u) return v1;
    return v0 - MUL15(f->k, v1) - v2;
}

/* the phase increment of a note in 1/16 semitones (n16 = MIDI note * 16) */
static inline uint32_t inc_n16(int32_t n16)
{
    uint32_t n, fr;
    n16 = clampi(n16, 0, 127 * 16);
    n = (uint32_t)n16 >> 4;
    fr = (uint32_t)n16 & 15u;
    return (uint32_t)(((uint64_t)INC_NOTE[n] * FINE_Q16[fr]) >> 16);
}

/* constant-power pan, 0..127 (64 the middle) -> left / right gains Q15 */
static inline void pan_gains(uint32_t pan, int32_t *gl, int32_t *gr)
{
    uint32_t i = (pan > 127u ? 127u : pan) * 256u / 127u;   /* 0..256: a quarter cycle */
    *gr = SINE_T[i];
    *gl = SINE_T[256u - i < 1024u ? 256u - i : 0u];
}
