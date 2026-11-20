/* SPDX-License-Identifier: GPL-3.0-only */
/* KALK: the five sound sources, all synthesised (no samples): KICK, SNARE, HAT (closed and open, two voices that can
 * choke each other), TIE (a two-operator FM tone with glide) and POD (a tone whose pitch is struck by a decay
 * envelope: Limelite's idea of a decay as the FM source). WAV picks one of 16 variants of each.
 *
 * Runs in the audio interrupt: voice_trigger() at a step, voice_render() every block. Reads P.vp (bytes the main
 * loop writes: each read is atomic). */

#include "kalk_samples.h"

enum { V_KICK, V_SNARE, V_HATC, V_HATO, V_TIE, V_POD, NV_RUN };
static const uint8_t V_PARAMS[NV_RUN] = {0, 1, 2, 2, 3, 4};   /* which P.vp row a running voice reads */

typedef struct {
    uint8_t on, var, mode, filt;
    uint32_t ph, ph2, inc, inc2, tinc, ratio;   /* tinc: the glide's target (tie); ratio Q4 */
    uint32_t hph[6], hinc[6];                   /* hat oscillators */
    uint32_t env, ek, att, hold;                /* amplitude: attack step, hold samples, decay */
    uint32_t penv, pk;                          /* pitch / tone / FM envelope */
    uint32_t nenv, nk;                          /* noise / click envelope */
    uint32_t sweep, clap;                       /* pitch sweep at penv = 1; clap burst counter */
    int32_t lvl, gl, gr, drv, nmix, click, fm, fb, last;
    uint32_t glide;                             /* tie: shift of the glide per 16 frames (0 = none) */
    uint32_t flam;                              /* snare: frames to the second stroke */
    uint8_t shape, n16;
    svf_t f;
    const ksmp_t *sm;                           /* WAV 17..24: a sample (inc, tinc: its step, Q16) */
    uint32_t spos;                              /* where in it, Q16 */
} voice_t;
static voice_t V[NV_RUN];
static volatile int8_t mod_now;                /* the mod lane's value now (0..15) */
static volatile int8_t pitch_all;              /* FX_PITCH - 12: the out section's transpose */

/* ---- the variants (WAV) */
typedef struct { uint8_t sw, pdec, click, noise, shape, note; } kick_var_t;   /* sw: sweep, 16 = one octave up */
static const kick_var_t KICKV[16] = {
    {40, 22, 12, 0, 0, 0},   {90, 14, 60, 0, 0, 2},   {120, 9, 90, 0, 2, 3},   {14, 34, 0, 0, 0, -2 & 0xFF},
    {250, 26, 20, 0, 0, 0},  {28, 42, 0, 0, 0, -3 & 0xFF}, {60, 6, 127, 0, 0, 2}, {50, 20, 30, 0, 1, 0},
    {80, 16, 40, 0, 3, 1},   {24, 10, 30, 40, 0, 1}, {160, 11, 70, 0, 3, 4}, {40, 18, 20, 70, 1, 0},
    {30, 24, 10, 0, 0, 12},  {180, 15, 85, 0, 2, 2}, {30, 20, 0, 0, 0, 0},   {255, 38, 10, 0, 0, 5},
};
typedef struct { uint8_t ratio, tone, noise, tdec, clap, note; } snare_var_t;   /* ratio Q4; tone, noise 0..127 */
static const snare_var_t SNAREV[16] = {
    {24, 100, 70, 0, 0, 0},   {28, 70, 110, 0, 0, 2},   {26, 90, 80, 8, 0, 5},   {22, 127, 60, 0, 0, -5 & 0xFF},
    {16, 0, 127, 0, 1, 0},    {37, 110, 30, 20, 0, 14}, {16, 0, 127, 0, 0, 0},   {24, 127, 0, 40, 0, -2 & 0xFF},
    {16, 10, 127, 0, 2, 0},   {16, 10, 90, 0, 0, 0},    {30, 100, 70, 6, 0, 10}, {21, 80, 100, 0, 1, 3},
    {32, 60, 127, 12, 0, 7},  {24, 127, 40, 0, 0, -9 & 0xFF}, {20, 40, 127, 0, 3, 0}, {44, 90, 90, 4, 0, 12},
};
/* hat: frequency sets (Hz) and the noise in the mix */
static const uint16_t HAT_HZ[4][6] = {
    {205, 304, 370, 523, 540, 800},             /* the 808's six squares */
    {317, 449, 594, 703, 851, 1017},
    {263, 400, 263, 400, 263, 400},             /* two tones: a ringing, tuned hat */
    {587, 845, 1031, 1253, 1497, 1789},
};
typedef struct { uint8_t set, noise, note; } hat_var_t;
static const hat_var_t HATV[16] = {
    {0, 10, 0}, {0, 40, 0}, {1, 20, 0}, {1, 60, 0}, {2, 15, 0}, {3, 30, 0}, {0, 127, 0}, {3, 0, 0},
    {0, 0, 12}, {1, 0, 7}, {2, 50, 12}, {3, 80, 5}, {0, 70, 5}, {1, 100, 0}, {2, 0, 19}, {3, 127, 0},
};
/* tie: modulator feedback and the carrier's shape */
static const uint8_t TIE_FB[16] = {0, 0, 40, 40, 80, 80, 120, 120, 0, 20, 60, 100, 0, 30, 70, 127};
static const uint8_t TIE_SHAPE[16] = {0, 1, 0, 1, 0, 1, 0, 1, 2, 2, 2, 2, 3, 3, 3, 3};
static const uint8_t RATIO_Q4[16] = {8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 255};

/* the sound of a new project (and of tests/voice_test.c) */
static const uint8_t VP_DEFAULT[NVOICE][NVP] = {
    /* TUNE DEC WAV GAIN  CUT RES TYP PAN  X1 X2 X3 X4  MD MA ACC ST */
    {24, 70, 0, 104, 127, 0, 0, 64, 0, 10, 64, 10, 0, 64, 50, 0},     /* kick: SAT CLICK SWEEP HOLD */
    {24, 64, 1, 100, 127, 0, 0, 58, 0, 64, 0, 0, 0, 64, 50, 0},       /* snare: SAT SNAPPY FLAM HOLD */
    {24, 44, 0, 100, 92, 10, 2, 72, 74, 1, 20, 0, 0, 64, 60, 0},      /* hat: (closed decay) OPEN LINK NOISE HOLD */
    {24, 62, 2, 92, 78, 50, 0, 64, 52, 8, 34, 0, 0, 64, 40, 0},       /* tie: FM RATIO PORT ATTACK; START = NOISE */
    {24, 44, 0, 90, 127, 0, 0, 44, 60, 30, 8, 0, 2, 92, 50, 0},       /* pod: ZAP ZAPDEC NOISE HOLD; the mod lane on pitch */
};

static uint32_t decay_k(int32_t i) { return DECAY_K[clampi(i, 0, 127)]; }
static int32_t level15(uint32_t gain) { return (int32_t)(gain * gain * 2u); }   /* 0..127 -> 0..32258, squared feel */

/* the mod lane's offset for this voice: delta in -60..59 */
static int32_t mod_delta(const uint8_t *p, uint32_t dst)
{
    int32_t m = mod_now;
    if (p[VP_MODDST] != dst || !m) return 0;
    return m * ((int32_t)p[VP_MODAMT] - 64) / 16;
}

static void voice_filter(voice_t *v, const uint8_t *p, int32_t cutoff_add)
{
    int32_t cut = clampi((int32_t)p[VP_CUT] + cutoff_add, 0, 127);
    v->mode = p[VP_FTYPE] > 2u ? 0u : p[VP_FTYPE];
    v->filt = !(v->mode == 0u && cut >= 127 && p[VP_RES] < 4u) && !(v->mode == 2u && cut == 0 && p[VP_RES] < 4u);
    if (v->filt) {
        svf_set(&v->f, SVF_G[cut], p[VP_RES]);
        svf_reset(&v->f);
    }
}

/* a hit. vi: V_..., acc: accented, semi: semitones (tie / pod steps), vel: Q15 velocity scale */
static void voice_trigger(uint32_t vi, uint32_t acc, int32_t semi, int32_t vel)
{
    voice_t *v = &V[vi];
    const uint8_t *p = P.vp[V_PARAMS[vi]];
    int32_t tune = (int32_t)p[VP_TUNE] - 24 + pitch_all + mod_delta(p, MD_PITCH) / 4 + semi;
    int32_t dec = (int32_t)p[VP_DECAY] + mod_delta(p, MD_DECAY) / 2;
    int32_t wav = p[VP_WAV] > 23u ? 23 : p[VP_WAV], md = mod_delta(p, MD_WAV) / 8;
    int32_t var = wav < 16 ? ((wav + md) & 15) : 16 + ((wav - 16 + md) & 7);   /* (a synth stays a synth, a sample a sample) */
    const ksmp_t *prev_sm = v->sm;
    int32_t lvl = level15(p[VP_GAIN]), st = 127 - (int32_t)p[VP_START];
    int32_t was_on = v->on;
    if (!acc) lvl = lvl * (127 - (int32_t)p[VP_ACC] * 3 / 4) / 127;
    v->lvl = MUL15(lvl, vel);
    pan_gains(p[VP_PAN], &v->gl, &v->gr);
    voice_filter(v, p, mod_delta(p, MD_CUT));
    v->var = (uint8_t)var;
    v->sm = 0;
    v->hold = (uint32_t)p[VP_X4] * 44u;         /* X4 is HOLD for kick, snare, hat, pod: 0..~127 ms */
    v->att = 0;
    v->env = ENV_ONE;
    v->ek = decay_k(dec);
    switch (vi) {
    case V_KICK: {
        const kick_var_t *k = &KICKV[var & 15];
        v->inc = inc_n16((33 + tune + (int8_t)k->note) * 16);
        v->sweep = (uint32_t)(((uint64_t)v->inc * k->sw) >> 4);
        v->penv = (uint32_t)(((uint64_t)ENV_ONE * (uint32_t)st) >> 7);
        v->pk = decay_k(k->pdec + ((int32_t)p[VP_X3] - 64) / 3);   /* X3: SWEEP time */
        v->click = (int32_t)(k->click + p[VP_X2] > 127u ? 127u : k->click + p[VP_X2]) * st / 127 * 200;
        v->nenv = ENV_ONE;
        v->nk = decay_k(2);
        v->nmix = (int32_t)k->noise * 200;
        v->drv = 256 + (int32_t)p[VP_X1] * 24 + (k->shape == 3u ? 1500 : 0);   /* X1: SAT */
        v->shape = k->shape;
        v->ph = 0;
        break;
    }
    case V_SNARE: {
        const snare_var_t *s = &SNAREV[var & 15];
        v->inc = inc_n16((52 + tune + (int8_t)s->note) * 16);
        v->inc2 = (uint32_t)(((uint64_t)v->inc * s->ratio) >> 4);
        v->penv = ENV_ONE;
        v->pk = decay_k(dec - 20 + s->tdec);    /* the body falls faster than the noise */
        v->nenv = ENV_ONE;
        v->nk = decay_k(dec);
        v->fm = (int32_t)s->tone * 230 * st / 127;            /* (tone level) */
        v->nmix = (int32_t)s->noise * (64 + (int32_t)p[VP_X2]);   /* X2: SNAPPY */
        v->clap = s->clap ? 1u : 0u;
        v->shape = s->clap;
        v->drv = 256 + (int32_t)p[VP_X1] * 24;  /* X1: SAT */
        v->flam = (uint32_t)p[VP_X3] * 22u;   /* X3: FLAM, up to ~63 ms */
        v->ph = v->ph2 = 0;
        break;
    }
    case V_HATC:
    case V_HATO: {
        const hat_var_t *h = &HATV[var & 15];
        uint32_t i, mul = inc_n16((69 + tune + (int8_t)h->note) * 16);   /* A4 = these frequencies as written */
        for (i = 0; i < 6u; i++)
            v->hinc[i] = (uint32_t)(((uint64_t)mul * (HAT_HZ[h->set][i] * 38130u)) >> 24);   /* hz * mul / 440 */
        v->nmix = (int32_t)(h->noise + (uint32_t)p[VP_X3] > 127u ? 127u : h->noise + p[VP_X3]);   /* X3: NOISE */
        if (vi == V_HATO) v->ek = decay_k(p[VP_X1]);   /* X1: OPEN decay */
        if (p[VP_X2] && vi == V_HATC) V[V_HATO].ek = decay_k(0);   /* X2: LINK: the closed hat chokes the open */
        if (p[VP_X2] && vi == V_HATO) V[V_HATC].ek = decay_k(0);
        break;
    }
    case V_TIE: {
        int32_t port = p[VP_X3];                /* X3: PORT */
        v->tinc = inc_n16((48 + tune) * 16);
        if (!was_on || !port) v->inc = v->tinc;
        v->glide = port ? (uint32_t)(1 + (127 - port) / 14) : 0u;
        v->ratio = RATIO_Q4[p[VP_X2] >> 3];    /* X2: MP, the modulator's pitch */
        v->inc2 = (uint32_t)(((uint64_t)v->inc * v->ratio) >> 4);
        v->fm = (int32_t)p[VP_X1] * 258;        /* X1: FM depth, Q15 */
        v->fb = TIE_FB[var & 15] * 160;
        v->shape = TIE_SHAPE[var & 15];
        v->att = p[VP_X4] ? ENV_ONE / ((uint32_t)p[VP_X4] * 88u) : 0u;   /* X4: ATTACK, up to ~250 ms */
        v->env = v->att ? (was_on ? v->env : 0u) : ENV_ONE;
        v->hold = 0;
        v->penv = ENV_ONE;
        v->pk = decay_k(dec - 12);
        v->nmix = (int32_t)p[VP_START] * 120;   /* tie: START is the NOISE in it */
        if (!was_on) v->ph = v->ph2 = 0;
        break;
    }
    case V_POD: {
        v->inc = inc_n16((60 + tune) * 16);
        v->sweep = (uint32_t)(((uint64_t)v->inc * p[VP_X1]) >> 3);   /* X1: ZAP depth: up to 16x */
        v->penv = ENV_ONE;
        v->pk = decay_k((int32_t)p[VP_X2] - 20);   /* X2: ZAP decay */
        v->nmix = (int32_t)p[VP_X3] * 200;      /* X3: NOISE */
        v->shape = (uint8_t)(var & 3);
        v->fb = ((var & 15) >> 2) * 3000;             /* WAV 4..15: more of the ring */
        v->ph = 0;
        break;
    }
    }
    if (var >= 16) {                            /* a sample: the voice's envelope, filter, pitch, glide and zap on it */
        static const ksmp_t *const BANK[NV_RUN] = {SMP_KICK, SMP_SNARE, SMP_HAT, SMP_HAT + NSMP, SMP_TIE, SMP_POD};
        const ksmp_t *sm = &BANK[vi][var - 16];
        int32_t note = 60 + tune + (vi == V_TIE ? 12 : 0);   /* (tie: TUNE 0 plays the sample as recorded) */
        uint32_t ratio = (uint32_t)(((uint64_t)inc_n16(note * 16 - (vi == V_TIE ? 192 : 0)) * RECIP60) >> 32);
        uint32_t step = (uint32_t)(((uint64_t)sm->step * ratio) >> 16);
        v->sm = sm;
        v->spos = vi == V_TIE ? 0u : (sm->len * p[VP_START] / 256u) << 16;   /* START: into the sample, up to half */
        v->tinc = step;
        if (vi != V_TIE || !was_on || !v->glide || !prev_sm) v->inc = step;
        v->sweep = vi == V_POD ? (step * p[VP_X1]) >> 5 : 0u;   /* pod: ZAP on the sample's pitch */
        v->penv = ENV_ONE;
        if (vi == V_KICK || vi == V_SNARE) v->drv = 256 + (int32_t)p[VP_X1] * 24;
        {                                       /* Limelite's shape: hold, then release; DECAY sets both */
            int32_t di = clampi(vi == V_HATO ? (int32_t)p[VP_X1] : dec, 0, 127);
            v->hold = (uint32_t)DECAY_MS[di] * 22u + (uint32_t)p[VP_X4] * 44u;   /* half the decay time + HOLD */
        }
        v->nmix = 0;
    }
    v->on = 1;
}

/* ---- the renders: add n frames (L R) */
static void voice_out(voice_t *v, int32_t *out, uint32_t i, int32_t s)
{
    if (v->filt) s = svf_run(&v->f, s, v->mode);
    s = MUL15(s, v->lvl);                       /* (the out section's limiter catches the sum) */
    out[2u * i] += MUL15(s, v->gl);
    out[2u * i + 1u] += MUL15(s, v->gr);
}
static int env_step(voice_t *v)                 /* the amplitude envelope; 0 when it has died away */
{
    if (v->att) {
        if (v->env < ENV_ONE - v->att) { v->env += v->att; return 1; }
        v->env = ENV_ONE;
        v->att = 0;
    }
    if (v->hold) { v->hold--; return 1; }
    v->env = env_dec(v->env, v->ek);
    return v->env > (1u << 17);                 /* -78 dB: done */
}

static void render_kick(voice_t *v, int32_t *out, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        int32_t s, e15;
        if (!env_step(v)) { v->on = 0; return; }
        v->penv = env_dec(v->penv, v->pk);
        v->ph += v->inc + (uint32_t)(((uint64_t)v->sweep * v->penv) >> 30);
        s = v->shape == 1u ? tri15(v->ph) : sin15(v->ph);
        if (v->shape >= 2u) s = drive15(s, 1024);
        e15 = (int32_t)(v->env >> 15);
        s = MUL15(s, e15);
        if (v->click | v->nmix) {
            int32_t ne = (int32_t)(v->nenv >> 15), nz = noise15();
            v->nenv = env_dec(v->nenv, v->nk);
            s += MUL15(MUL15(nz, v->click), ne) + MUL15(MUL15(nz, v->nmix), e15 >> 2);
        }
        if (v->drv > 300) s = drive15(s, v->drv);
        voice_out(v, out, i, s);
    }
}

static void render_snare(voice_t *v, int32_t *out, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        int32_t s, te, ne;
        if (v->hold) v->hold--;                 /* HOLD: the noise stays up */
        else v->nenv = env_dec(v->nenv, v->nk);
        v->penv = env_dec(v->penv, v->pk);
        if (v->flam && --v->flam == 0u)         /* the flam's second stroke */
            v->penv = v->nenv = ENV_ONE - (ENV_ONE >> 2);
        if (v->clap && v->clap < 1800u) {       /* a clap: the noise struck again every ~10 ms, three times */
            if (++v->clap % 440u == 0u) v->nenv = ENV_ONE;
        }
        if (v->nenv < (1u << 17) && v->penv < (1u << 17) && !v->flam) { v->on = 0; return; }
        v->ph += v->inc;
        v->ph2 += v->inc2;
        te = (int32_t)(v->penv >> 15);
        ne = (int32_t)(v->nenv >> 15);
        s = MUL15((sin15(v->ph) + (sin15(v->ph2) >> 1)) >> 1, MUL15(te, v->fm));
        s += MUL15(MUL15(noise15() * 2, v->nmix), ne);
        if (v->drv > 300) s = drive15(s, v->drv);
        voice_out(v, out, i, s);
    }
}

static void render_hat(voice_t *v, int32_t *out, uint32_t n)
{
    uint32_t i, k;
    int32_t nm = v->nmix * 258, mm = 32767 - nm;
    for (i = 0; i < n; i++) {
        int32_t metal = 0, s;
        if (!env_step(v)) { v->on = 0; return; }
        for (k = 0; k < 6u; k++) {
            v->hph[k] += v->hinc[k];
            metal += (int32_t)(v->hph[k] >> 31) ? 5461 : -5461;
        }
        s = MUL15(metal, mm) + MUL15(noise15() * 2, nm);
        voice_out(v, out, i, MUL15(s, (int32_t)(v->env >> 15)));
    }
}

static void render_tie(voice_t *v, int32_t *out, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        int32_t m, s, e15, depth;
        if ((i & 15u) == 0u && v->glide && v->inc != v->tinc) {   /* glide, every 16 frames */
            int32_t d = (int32_t)(v->tinc - v->inc) >> v->glide;
            v->inc = d ? v->inc + (uint32_t)d : v->tinc;
            v->inc2 = (uint32_t)(((uint64_t)v->inc * v->ratio) >> 4);
        }
        if (!env_step(v)) { v->on = 0; return; }
        v->penv = env_dec(v->penv, v->pk);
        e15 = (int32_t)(v->env >> 15);
        v->ph2 += v->inc2;
        m = sin15(v->ph2 + ((uint32_t)(v->last * v->fb) << 5));
        v->last = m >> 4;
        depth = MUL15(v->fm, (int32_t)(v->penv >> 15));
        v->ph += v->inc;
        s = sin15(v->ph + ((uint32_t)(m * depth) << 2));
        if (v->shape == 1u) s = drive15(s, 900);
        else if (v->shape == 2u) s = (s + tri15(v->ph)) >> 1;
        else if (v->shape == 3u) s = (s + (sin15(v->ph * 2u) >> 1)) * 2 / 3;
        if (v->nmix) s += MUL15(noise15(), v->nmix);
        voice_out(v, out, i, MUL15(s, e15));
    }
}

static void render_pod(voice_t *v, int32_t *out, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        int32_t s;
        if (!env_step(v)) { v->on = 0; return; }
        v->penv = env_dec(v->penv, v->pk);
        v->ph += v->inc + (uint32_t)(((uint64_t)v->sweep * v->penv) >> 30);
        switch (v->shape) {
        case 0: s = sin15(v->ph); break;
        case 1: s = tri15(v->ph); break;
        case 2: s = drive15(sin15(v->ph), 2048); break;
        default: s = (sin15(v->ph) + sin15(v->ph * 2u)) >> 1; break;
        }
        if (v->fb) s = MUL15(s, 32767 - v->fb + MUL15(sin15(v->ph * 3u), v->fb));   /* ring */
        if (v->nmix) s += MUL15(MUL15(noise15(), v->nmix), (int32_t)(v->penv >> 15));
        voice_out(v, out, i, MUL15(s, (int32_t)(v->env >> 15)));
    }
}

static void render_sample(voice_t *v, int32_t *out, uint32_t n)
{
    const uint8_t *d = SMP_DATA + v->sm->off;
    uint32_t i, last = v->sm->len - 1u;
    for (i = 0; i < n; i++) {
        uint32_t k = v->spos >> 16, step = v->inc;
        int32_t a, b, s;
        if ((i & 15u) == 0u && v->glide && v->inc != v->tinc) {   /* tie: glide */
            int32_t dd = (int32_t)(v->tinc - v->inc) >> v->glide;
            v->inc = dd ? v->inc + (uint32_t)dd : v->tinc;
        }
        if (v->flam && --v->flam == 0u) { v->spos = 0; k = 0; v->env = ENV_ONE - (ENV_ONE >> 2); }   /* snare: FLAM */
        if (k >= last || !env_step(v)) { v->on = 0; return; }
        a = MULAW_T[d[k]];
        b = MULAW_T[d[k + 1u]];
        s = a + (((b - a) * (int32_t)((v->spos >> 1) & 0x7FFFu)) >> 15);
        if (v->sweep) {
            v->penv = env_dec(v->penv, v->pk);
            step += (uint32_t)(((uint64_t)v->sweep * v->penv) >> 30);
        }
        v->spos += step;
        if (v->drv > 300) s = drive15(s, v->drv);
        voice_out(v, out, i, MUL15(s, (int32_t)(v->env >> 15)));
    }
}

static void voices_render(int32_t *out, uint32_t n)
{
    uint32_t k;
    for (k = 0; k < NV_RUN; k++)                /* the samples first (one loop for all) */
        if (V[k].on && V[k].sm) render_sample(&V[k], out, n);
    if (V[V_KICK].on && !V[V_KICK].sm) render_kick(&V[V_KICK], out, n);
    if (V[V_SNARE].on && !V[V_SNARE].sm) render_snare(&V[V_SNARE], out, n);
    if (V[V_HATC].on && !V[V_HATC].sm) render_hat(&V[V_HATC], out, n);
    if (V[V_HATO].on && !V[V_HATO].sm) render_hat(&V[V_HATO], out, n);
    if (V[V_TIE].on && !V[V_TIE].sm) render_tie(&V[V_TIE], out, n);
    if (V[V_POD].on && !V[V_POD].sm) render_pod(&V[V_POD], out, n);
}
