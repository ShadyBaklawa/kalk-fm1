/* SPDX-License-Identifier: GPL-3.0-only */
/* KALK: the effects, after the voices, in the audio interrupt (32 frames at a time).
 *
 *   XFX      one of 18 effects on an XY pad (K2 = X, K3 = Y); the pad's motion recorded over 4 bars (64 points).
 *            The glitch ones (after 0.2), all synced to the bar while playing:
 *              CHOP    each 16th cut by an envelope: X its length, Y its shape (curved .. straight .. held)
 *              SLICE   16ths (or 32nds, 8ths: X) played backwards on the steps of a pattern (Y: 16 patterns)
 *              FREEZE  X past the first notch loops the last 1/64 .. 1/4 (X); Y slows it to a stop or speeds it up
 *              SPIN    a rotating speaker: X the speed (1/16 .. 1 bar a turn), Y its depth (Doppler and level)
 *              REFLEX  a resonant low pass, L and R swept in mirror a beat a cycle: X the cutoff, Y the resonance
 *              PITCH   a two-grain pitch shifter: X the grain (12 .. 92 ms), Y the pitch (-12 .. +12)
 *            SLICE, FREEZE and PITCH share the REPEAT's buffer, SPIN the comb's: only the chosen effect runs
 *   FILTER   12 dB low pass, its cutoff moved by the mod lane and by the level (TRK); FOLLOW (after 0.3): the beat
 *            moves it too: an envelope follower on the mix, the kick, kick + snare or all the drums, its attack and
 *            release; BEAT over the middle opens the filter on the hits, under it closes it (64: off)
 *   EQ       lo / mid / hi and a saturating AMP
 *   DELAY    stereo, tempo synced in 16ths (0: a chorus), feedback, damping, width (cross feedback); RESON: a
 *            resonant LP / BP / HP in the loop with soft limiting, a low cut going in, feedback inverted; TAPE LFO:
 *            synced, stereo (90 degrees apart), on the time (DMOD: wobble) and on the cutoff (FM)
 *   REVERB   a metallic FDN at half rate: 4 diffusers, 4 lines (Hadamard), RT60, size, damping, low cut,
 *            modulation, width; fed from the mix (SEND) and from the delay (DLY>RV)
 *   OUT      DIRT (distortion), NORM (a maximiser), GAIN
 *   PULP     (after 0.3) LIME.lite's Automatik Perc: a percussion oscillator struck by the drums (TRIG) that tracks
 *            the pitch of what plays: zero crossings of the drums, low passed at HI, periods between LO and HI;
 *            GLIDE how fast it follows, DECAY, WAVE (sine .. triangle .. square), PAN; LEVEL 0: off. Before the
 *            effects, so they take it too
 *   GATE     (while playing) a 16-step lane at 1/8, 1/16 or 1/32 that cuts the sound on its steps: PRE (before the
 *            effects: the echoes and the reverb ring on) or POST (after them: hard cuts); DEPTH, SMOOTH (the ramp)
 */

#define POOL __attribute__((section(".pool")))
#define DL_LEN 32768u                           /* the delay at half rate: 1.49 s a side */
#define RP_LEN 16384u                           /* the repeat: 0.37 s */
#define CB_LEN 2048u                            /* comb / flanger */
static int16_t dl_buf[2][DL_LEN] POOL;
static int16_t rp_buf[2][RP_LEN] POOL;
static int16_t cb_buf[2][CB_LEN] POOL;
#define RV_LEN 4096u                            /* a reverb line at half rate: up to 186 ms */
#define AP_LEN 512u
static int16_t rv_buf[4][RV_LEN] POOL;
static int16_t ap_buf[4][AP_LEN] POOL;
static int32_t dl_out[2u * 32u];                /* what the delay added this block (the reverb's DLY>RV) */

static volatile uint8_t fx_x = 64, fx_y = 64;  /* the pad as it is now: the knobs or the recorded motion */

static struct {
    /* xfx */
    uint8_t type, cx, cy;                       /* the coefficients below are for this */
    svf_t f[2];
    uint32_t rph, lfo;
    int32_t held[2]; uint32_t hcnt;
    uint32_t cbw;
    uint8_t rp_on, rp_rec; uint32_t rp_len, rp_pos;
    int32_t gate_g;
    uint32_t rgw;                               /* the glitch ring (rp_buf): where it writes next */
    int32_t chop_g;
    uint32_t sl_idx, sl_start, sl_len, sl_k; uint8_t sl_rev;
    uint8_t fz_on; uint32_t fz_end, fz_p; int32_t fz_spd;
    int32_t sp_d[2];
    int32_t ps_d;
    /* filter */
    svf_t mf[2]; int32_t mcut, mres, follow;
    int32_t beat;                               /* FOLLOW's envelope, Q15 */
    /* eq */
    int32_t lo1[2], lo2[2];
    /* delay */
    uint32_t dw; int32_t dprev[2], dcur[2], ddamp[2], dodd[2];
    svf_t df[2]; int32_t dlo[2];
    /* reverb */
    uint32_t rw, apw, rlfo;
    uint8_t rp[4];                              /* TIME SIZE DAMP MOD the coefficients below are for */
    uint32_t rlen[4]; int32_t rg[4], rdamp, rmod;
    int32_t rlp[4], rhp, rprev[2], rcur[2], rquiet;
    /* out */
    int32_t norm_env, norm_g;
    int32_t gate_now;                           /* the GATE's gain now, Q15 */
} X = {.type = 255, .norm_g = 4096, .rp = {255, 255, 255, 255}, .gate_now = 32767};

/* (after 0.3) fx2[] for a new project: everything at rest (0.2 and 0.3 projects sound the same) */
static const uint8_t FX2_0[NFX2] = {64, 10, 50, BS_KICK,   0, 20, 60, 50,   0, BS_KS, 10, 64};

static int32_t mix15(int32_t dry, int32_t wet, int32_t m) { return dry + MUL15(wet - dry, m); }
static int32_t fold15(int32_t x)                /* a triangle fold: whatever goes past full scale comes back */
{
    uint32_t u = (uint32_t)(x + 32768) & 131071u;
    int32_t v = (int32_t)(u < 65536u ? u : 131071u - u);
    return v - 32768;
}
/* the phase of the bar now, in 16ths, Q8 (for the synced LFOs) */
static uint32_t pos16_q8(void) { return gstep * 256u + (S.frac >> 16); }

/* ---- the glitch effects (after 0.2) */
#define RG_MASK (RP_LEN - 1u)
static void ring_rec(const int32_t *b, uint32_t n)   /* the input into the ring, as it is */
{
    uint32_t i;
    for (i = 0; i < n; i++, X.rgw++) {
        rp_buf[0][X.rgw & RG_MASK] = (int16_t)clip15(b[2u * i]);
        rp_buf[1][X.rgw & RG_MASK] = (int16_t)clip15(b[2u * i + 1u]);
    }
}
static int32_t edge64(uint32_t k, uint32_t len)  /* a fade in and out at a loop's ends, 64 frames: Q15 */
{
    uint32_t e = k < len - k ? k : len - k;
    return (int32_t)(e < 64u ? e : 64u) * 512;
}
static void glitch_reset(void)
{
    X.chop_g = 32767; X.sl_idx = 0xFFFFFFFFu; X.sl_rev = 0; X.sl_len = 0; X.fz_on = 0; X.ps_d = 0;
    X.sp_d[0] = X.sp_d[1] = 160 << 8;
    svf_reset(&X.f[0]); svf_reset(&X.f[1]);
}

/* CHOP's gain at p (the 16th's phase, Q8): X the length (4 % .. all of the 16th), Y the shape */
static int32_t chop_gain(uint32_t p, int32_t x, int32_t y)
{
    int32_t len = 10 + x * 246 / 127, t, lin, cur, held;
    if ((int32_t)p >= len) return 0;
    t = (int32_t)(p << 15) / len;               /* 0..32767 through the envelope */
    lin = 32767 - t;
    cur = MUL15(lin, lin); cur = MUL15(cur, cur);   /* (1-t)^4: a sharp, curved decay */
    held = MUL15(t, t); held = 32767 - MUL15(held, held);   /* 1-t^4: held, then cut */
    return y < 64 ? cur + (lin - cur) * y / 64 : lin + (held - lin) * (y - 64) / 63;
}

/* SLICE's patterns: bit k set = the k-th slice of the bar plays backwards */
static const uint16_t SL_PAT[16] = {0x8888, 0x2222, 0xAAAA, 0x5555, 0xCCCC, 0x3333, 0xF0F0, 0x0F0F,
                                    0xFF00, 0x00FF, 0x8080, 0x0808, 0xA0A0, 0x4924, 0x6DB6, 0xFFFF};
/* FREEZE's loop, in 8ths of a 16th: 1/64 1/32 1/16 1/8 3/16 1/4 */
static const uint8_t FZ_LEN8[6] = {2, 4, 8, 16, 24, 32};
/* PITCH: 2^(s/12), Q16, s = -12..12 */
static const uint32_t PS_R[25] = {32768, 34716, 36781, 38968, 41285, 43740, 46341, 49097, 52016, 55109, 58386, 61858,
    65536, 69433, 73562, 77936, 82570, 87480, 92682, 98193, 104032, 110218, 116772, 123715, 131072};

static void glitch(int32_t *b, uint32_t n, uint32_t type, int32_t x, int32_t y, int32_t mix)
{
    uint32_t i, c;
    switch (type) {
    case XF_CHOP: {
        int32_t g0 = X.chop_g, g1 = playing ? chop_gain(pos16_q8() & 255u, x, y) : 32767;
        if (g0 == 32767 && g1 == 32767) break;  /* (open: as it is) */
        for (i = 0; i < n; i++) {
            int32_t g = g0 + (g1 - g0) * (int32_t)(i + 1u) / (int32_t)n;   /* a ramp across the block: no clicks */
            for (c = 0; c < 2u; c++) b[2u * i + c] = mix15(b[2u * i + c], MUL15(b[2u * i + c], g), mix);
        }
        X.chop_g = g1;
        break;
    }
    case XF_SLICE: {
        uint32_t sh = 7u + (uint32_t)x * 3u / 128u, idx = pos16_q8() >> sh;   /* 1/32, 1/16, 1/8 */
        if (idx != X.sl_idx) {                  /* a new slice: the last one is what plays backwards */
            uint32_t len = X.rgw - X.sl_start;
            X.sl_len = len > RP_LEN / 2u ? RP_LEN / 2u : len;   /* (the ring holds this one and the last) */
            X.sl_start = X.rgw; X.sl_idx = idx; X.sl_k = 0;
            X.sl_rev = playing && X.sl_len >= 128u && ((SL_PAT[(uint32_t)y >> 3] >> (idx & 15u)) & 1u);
        }
        if (!playing) X.sl_rev = 0;
        for (i = 0; i < n; i++, X.sl_k++) {
            int32_t in[2];
            for (c = 0; c < 2u; c++) in[c] = b[2u * i + c];
            rp_buf[0][X.rgw & RG_MASK] = (int16_t)clip15(in[0]);
            rp_buf[1][X.rgw & RG_MASK] = (int16_t)clip15(in[1]);
            X.rgw++;
            if (!X.sl_rev) continue;
            for (c = 0; c < 2u; c++) {
                int32_t w;
                if (X.sl_k < X.sl_len) w = MUL15(rp_buf[c][(X.sl_start - 1u - X.sl_k) & RG_MASK], edge64(X.sl_k, X.sl_len));
                else w = MUL15(in[c], (int32_t)((X.sl_k - X.sl_len) < 64u ? (X.sl_k - X.sl_len) : 64u) * 512);
                b[2u * i + c] = mix15(in[c], w, mix);
            }
        }
        break;
    }
    case XF_FREEZE: {
        uint32_t len, len16;
        if (x < 16) { X.fz_on = 0; ring_rec(b, n); break; }   /* the first notch: listening */
        len = frames16 * FZ_LEN8[(x - 16) * 6 / 112] / 8u;
        if (len > RP_LEN - 1u) len = RP_LEN - 1u;
        if (len < 64u) len = 64u;
        len16 = len << 16;
        if (!X.fz_on) { X.fz_on = 1; X.fz_end = X.rgw; X.fz_p = 0; X.fz_spd = 65536; }
        X.fz_spd = clampi(X.fz_spd + (y - 64) * 3 / 2, 0, 4 << 16);   /* Y: under the middle down to a stop, over: up */
        if (X.fz_p >= len16) X.fz_p %= len16;   /* (X moved: a shorter loop) */
        for (i = 0; i < n; i++) {
            uint32_t k = X.fz_p >> 16, fr = (X.fz_p >> 8) & 255u, k1 = k + 1u < len ? k + 1u : 0u;
            int32_t e = MUL15(edge64(k, len), X.fz_spd < 16384 ? X.fz_spd * 2 : 32767);   /* (and gone at a stop) */
            for (c = 0; c < 2u; c++) {
                int32_t a = rp_buf[c][(X.fz_end - len + k) & RG_MASK], bb = rp_buf[c][(X.fz_end - len + k1) & RG_MASK];
                b[2u * i + c] = mix15(b[2u * i + c], MUL15(a + (((bb - a) * (int32_t)fr) >> 8), e), mix);
            }
            X.fz_p += (uint32_t)X.fz_spd;
            if (X.fz_p >= len16) X.fz_p -= len16;
        }
        break;
    }
    case XF_SPIN: {                             /* the horn goes round: one side comes nearer while the other goes */
        static const uint8_t PER[5] = {1, 2, 4, 8, 16};
        uint32_t per = PER[x * 5 / 128], ph = pos16_q8() * (0x1000000u / per);
        int32_t s = playing ? sin15(ph) : 0, dep = y * 256, am = y * 100, d1[2], g[2];
        d1[0] = (160 << 8) + MUL15(s, dep); d1[1] = (160 << 8) - MUL15(s, dep);   /* Q8 frames: ~3.6 ms +- 2.9 ms */
        g[0] = 32767 - (MUL15(am, 32767 - s) >> 1); g[1] = 32767 - (MUL15(am, 32767 + s) >> 1);
        for (i = 0; i < n; i++) {
            for (c = 0; c < 2u; c++) {
                int32_t d = X.sp_d[c] + (d1[c] - X.sp_d[c]) * (int32_t)(i + 1u) / (int32_t)n;
                uint32_t dw = (uint32_t)d >> 8, fr = (uint32_t)d & 255u;
                int32_t in = b[2u * i + c], a, bb;
                cb_buf[c][X.cbw & (CB_LEN - 1u)] = (int16_t)clip15(in);
                a = cb_buf[c][(X.cbw - dw) & (CB_LEN - 1u)]; bb = cb_buf[c][(X.cbw - dw - 1u) & (CB_LEN - 1u)];
                b[2u * i + c] = mix15(in, MUL15(a + (((bb - a) * (int32_t)fr) >> 8), g[c]), mix);
            }
            X.cbw++;
        }
        X.sp_d[0] = d1[0]; X.sp_d[1] = d1[1];
        break;
    }
    case XF_REFLEX: {                           /* L and R: the same sweep, mirrored, a beat a cycle */
        int32_t t = playing ? tri15(pos16_q8() * (0x1000000u / 4u)) : 0, base = x * 110 / 127 + 8;
        svf_set(&X.f[0], SVF_G[clampi(base + t * 24 / 32768, 0, 120)], (uint32_t)y);
        svf_set(&X.f[1], SVF_G[clampi(base - t * 24 / 32768, 0, 120)], (uint32_t)y);
        for (i = 0; i < n; i++)
            for (c = 0; c < 2u; c++) b[2u * i + c] = mix15(b[2u * i + c], soft15(svf_run(&X.f[c], b[2u * i + c], 0)), mix);
        break;   /* (soft15: the resonance limited) */
    }
    case XF_PITCH: {                            /* two taps a half grain apart, each faded in and out (a triangle) */
        int32_t g = 512 + x * 28, g16 = g << 16, step = 65536 - (int32_t)PS_R[(y * 25) >> 7];
        uint32_t rcp = 0x80000000u / (uint32_t)g;   /* Q31 / g: the window's phase without a division */
        for (i = 0; i < n; i++, X.rgw++) {
            int32_t d2, w1, w2;
            uint32_t f1, f2;
            for (c = 0; c < 2u; c++) rp_buf[c][X.rgw & RG_MASK] = (int16_t)clip15(b[2u * i + c]);
            X.ps_d += step;
            if (X.ps_d < 0) X.ps_d += g16;
            if (X.ps_d >= g16) X.ps_d -= g16;
            if (X.ps_d < 0 || X.ps_d >= g16) X.ps_d = 0;   /* (X moved: a shorter grain) */
            d2 = X.ps_d + (g16 >> 1); if (d2 >= g16) d2 -= g16;
            f1 = (uint32_t)(((uint64_t)(uint32_t)X.ps_d * rcp) >> 31);   /* 0..65535 through the grain */
            f2 = (uint32_t)(((uint64_t)(uint32_t)d2 * rcp) >> 31);
            w1 = (int32_t)(f1 < 32768u ? f1 : 65536u - f1); w2 = (int32_t)(f2 < 32768u ? f2 : 65536u - f2);
            for (c = 0; c < 2u; c++) {
                uint32_t a1 = (uint32_t)X.ps_d >> 16, a2 = (uint32_t)d2 >> 16;
                int32_t s1 = rp_buf[c][(X.rgw - a1) & RG_MASK], s1b = rp_buf[c][(X.rgw - a1 - 1u) & RG_MASK];
                int32_t s2 = rp_buf[c][(X.rgw - a2) & RG_MASK], s2b = rp_buf[c][(X.rgw - a2 - 1u) & RG_MASK];
                s1 += ((s1b - s1) * (int32_t)(((uint32_t)X.ps_d >> 8) & 255u)) >> 8;
                s2 += ((s2b - s2) * (int32_t)(((uint32_t)d2 >> 8) & 255u)) >> 8;
                b[2u * i + c] = mix15(b[2u * i + c], (s1 * w1 + s2 * w2) >> 15, mix);
            }
        }
        break;
    }
    }
}

static void xfx(int32_t *b, uint32_t n)
{
    uint32_t type = P.fx[FX_XTYPE], i, c;
    int32_t x = fx_x, y = fx_y, mix = (int32_t)P.fx[FX_XMIX] * 258, modfx = P.fx[FX_MODFX];
    if (modfx == 1u && !mod_now) mix = 0;       /* GATE: only on the mod lane's steps */
    if (modfx == 2u) mix = MUL15(mix, mod_now * 2184);   /* MIX: the mod value is the amount */
    if (modfx == 3u && mod_now) x = mod_now * 8;   /* X: the mod value moves X */
    if (type >= NXF || type == XF_THRU || !mix) {
        X.rp_on = 0;
        if (type == XF_SLICE || type == XF_FREEZE || type == XF_PITCH) {   /* (the ring keeps listening) */
            if (type != X.type) { glitch_reset(); X.type = (uint8_t)type; }
            X.fz_on = 0; X.sl_rev = 0; X.sl_idx = 0xFFFFFFFFu;
            ring_rec(b, n);
        }
        return;
    }
    if (type >= XF_CHOP) {
        if (type != X.type) { glitch_reset(); X.type = (uint8_t)type; X.cx = (uint8_t)x; X.cy = (uint8_t)y; }
        glitch(b, n, type, x, y, mix);
        return;
    }
    if (type != X.type || x != X.cx || y != X.cy) {
        if (type == XF_LPF || type == XF_HPF || type == XF_BPF) {
            for (c = 0; c < 2u; c++) svf_set(&X.f[c], SVF_G[clampi(x, 0, 120)], (uint32_t)y);
            if (type != X.type) { svf_reset(&X.f[0]); svf_reset(&X.f[1]); }
        }
        X.type = (uint8_t)type; X.cx = (uint8_t)x; X.cy = (uint8_t)y;
    }
    switch (type) {
    case XF_CRUSH: {                            /* X: bits 16..2, Y: the rate 1..64 */
        int32_t sh = x * 14 / 127, hold = 1 + ((y * y) >> 8);
        for (i = 0; i < n; i++) {
            if (++X.hcnt >= (uint32_t)hold) {
                X.hcnt = 0;
                X.held[0] = (b[2u * i] >> sh) << sh;
                X.held[1] = (b[2u * i + 1u] >> sh) << sh;
            }
            b[2u * i] = mix15(b[2u * i], X.held[0], mix);
            b[2u * i + 1u] = mix15(b[2u * i + 1u], X.held[1], mix);
        }
        break;
    }
    case XF_LPF: case XF_HPF: case XF_BPF: {
        uint32_t mode = type == XF_LPF ? 0u : type == XF_BPF ? 1u : 2u;
        for (i = 0; i < n; i++)
            for (c = 0; c < 2u; c++) b[2u * i + c] = mix15(b[2u * i + c], svf_run(&X.f[c], b[2u * i + c], mode), mix);
        break;
    }
    case XF_RING: {                             /* X: the frequency, Y: a wobble on it, a beat long */
        uint32_t inc = inc_n16((24 + x * 84 / 127) * 16);
        int32_t wob = MUL15(sin15((uint32_t)pos16_q8() << 22), y * 64);
        inc += (uint32_t)(((int64_t)inc * wob) >> 15);
        for (i = 0; i < n; i++) {
            int32_t r;
            X.rph += inc;
            r = sin15(X.rph);
            for (c = 0; c < 2u; c++) b[2u * i + c] = mix15(b[2u * i + c], MUL15(b[2u * i + c], r), mix);
        }
        break;
    }
    case XF_COMB: case XF_FLANGE: {             /* comb: X the time, Y feedback; flanger: X the rate, Y depth */
        int32_t fb = (y - 64) * 500, d0;
        uint32_t lfo_inc = (uint32_t)(x * x) * 40u;
        for (i = 0; i < n; i++) {
            uint32_t d;
            if (type == XF_COMB) d = 2u + (uint32_t)((x * x) >> 4);
            else {
                X.lfo += lfo_inc;
                d0 = 20 + MUL15(sin15(X.lfo) + 32767, 2 + y * 3);   /* 20..~400 frames */
                d = (uint32_t)d0;
                fb = 16000;
            }
            for (c = 0; c < 2u; c++) {
                int32_t dl = cb_buf[c][(X.cbw - d) & (CB_LEN - 1u)], in = b[2u * i + c];
                int32_t w = clip15(in + MUL15(dl, fb));
                cb_buf[c][X.cbw & (CB_LEN - 1u)] = (int16_t)w;
                b[2u * i + c] = mix15(in, type == XF_COMB ? w : (in + dl) >> 1, mix);
            }
            X.cbw++;
        }
        break;
    }
    case XF_REPEAT: {                           /* X: the length (1/32 .. 1/4), Y over the middle: on */
        static const uint8_t LEN8[8] = {4, 5, 8, 11, 16, 21, 24, 32};   /* in 8ths of a 16th */
        uint32_t len = frames16 * LEN8[x >> 4] / 8u;
        if (len > RP_LEN) len = RP_LEN;
        if (len < 64u) len = 64u;
        if (y >= 64 && !X.rp_on) { X.rp_on = 1; X.rp_rec = 1; X.rp_pos = 0; X.rp_len = len; }
        if (y < 64) X.rp_on = 0;
        if (!X.rp_on) break;
        for (i = 0; i < n; i++) {
            if (X.rp_rec) {
                rp_buf[0][X.rp_pos] = (int16_t)clip15(b[2u * i]);
                rp_buf[1][X.rp_pos] = (int16_t)clip15(b[2u * i + 1u]);
                if (++X.rp_pos >= X.rp_len) { X.rp_rec = 0; X.rp_pos = 0; }
            } else {
                int32_t e = (int32_t)(X.rp_pos < 64u ? X.rp_pos : X.rp_len - X.rp_pos < 64u ? X.rp_len - X.rp_pos : 64u) * 512;
                for (c = 0; c < 2u; c++)
                    b[2u * i + c] = mix15(b[2u * i + c], MUL15(rp_buf[c][X.rp_pos], e), mix);
                if (++X.rp_pos >= X.rp_len) X.rp_pos = 0;
            }
        }
        break;
    }
    case XF_FOLD: {                             /* X: drive, Y: bias */
        int32_t g = 256 + x * 40, bias = (y - 64) * 256;
        for (i = 0; i < n; i++)
            for (c = 0; c < 2u; c++)
                b[2u * i + c] = mix15(b[2u * i + c], fold15((b[2u * i + c] * g >> 8) + bias), mix);
        break;
    }
    case XF_GATE: case XF_PAN: {                /* X: the rate (1/32, 1/16, 1/8, 1/4), Y: depth */
        static const uint8_t SH[4] = {7, 8, 9, 10};   /* the period in 16ths Q8: 2^SH / 256 */
        uint32_t p = pos16_q8(), sh = SH[x >> 5], ph = (p << (32u - sh));   /* the period's phase, 32 bits */
        int32_t depth = y * 258;
        if (type == XF_GATE) {
            int32_t tg = ph < 0x80000000u ? 32767 : 32767 - depth;
            for (i = 0; i < n; i++) {
                X.gate_g += (tg - X.gate_g) >> 5;
                for (c = 0; c < 2u; c++) b[2u * i + c] = mix15(b[2u * i + c], MUL15(b[2u * i + c], X.gate_g), mix);
            }
        } else {
            int32_t s = MUL15(sin15(ph), depth), gl = 32767 - (s > 0 ? s : 0), gr = 32767 + (s < 0 ? s : 0);
            for (i = 0; i < n; i++) {
                b[2u * i] = mix15(b[2u * i], MUL15(b[2u * i], gl), mix);
                b[2u * i + 1u] = mix15(b[2u * i + 1u], MUL15(b[2u * i + 1u], gr), mix);
            }
        }
        break;
    }
    }
}

/* FOLLOW: the times its ATTACK and RELEASE stand for (ms, to about 2/3), and their one-pole steps per block */
static uint32_t follow_blocks(uint32_t x) { return 1u + x * x / 32u; }   /* 1 .. 505 blocks of 32 */
static uint32_t follow_ms(uint32_t x) { return follow_blocks(x) * 32u * 1000u / 44100u; }

/* the level FOLLOW listens to, Q15: the mix's peak, or the drums' envelopes (no ringing, no detection needed) */
static int32_t voice_level(uint32_t vi)
{
    const voice_t *v = &V[vi];
    uint32_t e;
    if (!v->on) return 0;
    e = v->env;
    if (vi == V_SNARE && !v->sm) e = v->nenv > v->penv ? v->nenv : v->penv;   /* (the synth snare's two envelopes) */
    return MUL15((int32_t)(e >> 15), v->lvl);
}
static void beat_follow(int32_t mixpeak)
{
    int32_t lev, k;
    switch (P.fx2[F2_BSRC]) {
    case BS_MIX: lev = mixpeak; break;
    case BS_KICK: lev = voice_level(V_KICK); break;
    case BS_KS: lev = voice_level(V_KICK) + voice_level(V_SNARE); break;
    default: lev = voice_level(V_KICK) + voice_level(V_SNARE) + voice_level(V_HATC) + voice_level(V_HATO); break;
    }
    lev = lev > 32767 ? 32767 : lev;
    k = lev > X.beat ? (int32_t)follow_blocks(P.fx2[F2_BATK]) : (int32_t)follow_blocks(P.fx2[F2_BREL]);
    k = (lev - X.beat) / k;                     /* (32-bit) */
    X.beat += k ? k : (lev > X.beat) - (lev < X.beat);   /* (the last steps one at a time: it settles exactly) */
}

static void master_filter(int32_t *b, uint32_t n)
{
    int32_t cut = P.fx[FX_FCUT], res = P.fx[FX_FRES], peak = 0, beat = (int32_t)P.fx2[F2_BEAT] - 64;
    uint32_t i, c;
    cut += ((int32_t)P.fx[FX_FMOD] - 64) * mod_now / 8;
    cut += MUL15(X.follow, (int32_t)P.fx[FX_FTRK]) >> 1;
    if (beat) {                                 /* FOLLOW (64: off, not even listening) */
        int32_t mp = 0;
        if (P.fx2[F2_BSRC] == BS_MIX)
            for (i = 0; i < 2u * n; i++) { int32_t a = b[i] < 0 ? -b[i] : b[i]; if (a > mp) mp = a; }
        beat_follow(mp);
        cut += MUL15(X.beat, beat * 4);         /* +-63: up to about +-2 sweeps of the range at full level */
        if (cut > 126) cut = 126;               /* (it keeps running wide open: no jump when it closes again) */
    } else X.beat = 0;
    cut = clampi(cut, 0, 127);
    if (cut >= 127 && res < 4) { X.mcut = 255; return; }
    if (cut != X.mcut || res != X.mres) {
        for (c = 0; c < 2u; c++) svf_set(&X.mf[c], SVF_G[cut], (uint32_t)res);
        X.mcut = cut; X.mres = res;
    }
    for (i = 0; i < n; i++)
        for (c = 0; c < 2u; c++) {
            int32_t v = b[2u * i + c], a = v < 0 ? -v : v;
            if (a > peak) peak = a;
            b[2u * i + c] = svf_run(&X.mf[c], v, 0);
        }
    X.follow = peak > X.follow ? peak : X.follow - (X.follow >> 6);
}

static int32_t eq_gain(uint32_t p) { return p <= 64u ? (int32_t)p * 64 : 4096 + ((int32_t)p - 64) * 195; }   /* Q12 */
static void eq(int32_t *b, uint32_t n)
{
    uint32_t i, c;
    int32_t gl = eq_gain(P.fx[FX_EQLO]), gm = eq_gain(P.fx[FX_EQMID]), gh = eq_gain(P.fx[FX_EQHI]), amp = P.fx[FX_EQAMP];
    if (gl == 4096 && gm == 4096 && gh == 4096 && !amp) return;
    for (i = 0; i < n; i++)
        for (c = 0; c < 2u; c++) {
            int32_t v = b[2u * i + c], lo, hi, mid;
            X.lo1[c] += MUL15(v - X.lo1[c], 1146);   /* 250 Hz */
            X.lo2[c] += MUL15(v - X.lo2[c], 11400);  /* 3 kHz */
            lo = X.lo1[c];
            hi = v - X.lo2[c];
            mid = v - lo - hi;
            v = (int32_t)(((int64_t)lo * gl + (int64_t)mid * gm + (int64_t)hi * gh) >> 12);
            if (amp) v = drive15(v, 256 + amp * 16);
            b[2u * i + c] = v;
        }
}

/* the delay's LFO: synced to the bar, one cycle in 1/16 .. 4 bars; L and R 90 degrees apart */
static const uint8_t DLFO_16[7] = {1, 2, 4, 8, 16, 32, 64};
static int32_t delay_lfo(uint32_t c)
{
    uint32_t per = DLFO_16[P.fx[FX_DLFO] < 7u ? P.fx[FX_DLFO] : 4u];
    uint32_t ph = pos16_q8() * (0x1000000u / per) + (c ? 0x40000000u : 0u);
    return sin15(ph);
}

#define CHORUS_D 180u                           /* time 0: ~8 ms at half rate (a chorus, a flanger with DMOD) */
static void delay(int32_t *b, uint32_t n)
{
    uint32_t i, c, f16 = frames16 / 2u;
    uint32_t dt[2] = {P.fx[FX_DL] ? f16 * P.fx[FX_DL] : CHORUS_D, P.fx[FX_DR] ? f16 * P.fx[FX_DR] : CHORUS_D};
    int32_t send = (int32_t)P.fx[FX_DSEND] * 258, fb = (int32_t)P.fx[FX_DFB] * 240, w = (int32_t)P.fx[FX_DWIDTH] * 258;
    int32_t damp = 32767 - (int32_t)P.fx[FX_DDAMP] * 240;
    int32_t dmod = (int32_t)P.fx[FX_DDMOD] * P.fx[FX_DDMOD] * 4;   /* the wobble: up to ~250 frames, Q8 */
    int32_t lo = P.fx[FX_DLO] ? (int32_t)P.fx[FX_DLO] * P.fx[FX_DLO] * 9337 / 16129 : 0;
    uint32_t ftype = P.fx[FX_DFTYPE] < 3u ? P.fx[FX_DFTYPE] : 0u, d8[2];
    int filt = P.fx[FX_DFREQ] < 114u || P.fx[FX_DRES] || ftype || P.fx[FX_DFM];   /* (all at rest: the 0.2 delay) */
    for (uint32_t k = 0; k < 2u * n; k++) dl_out[k] = 0;
    if (P.fx[FX_DINV]) fb = -fb;
    if (!send) {
        if (!X.dcur[0] && !X.dcur[1]) return;   /* (the tail plays out, then nothing to do) */
    }
    for (c = 0; c < 2u; c++) {
        int32_t l = (dmod || filt) ? delay_lfo(c) : 0, dd;
        if (dt[c] >= DL_LEN - (dmod ? 300u : 1u)) dt[c] = DL_LEN - (dmod ? 300u : 1u);   /* (room for the wobble) */
        dd = (int32_t)(dt[c] << 8) + MUL15(l, dmod);   /* the time, wobbling (Q8 frames) */
        d8[c] = (uint32_t)(dd < 512 ? 512 : dd);
        if (filt) {                             /* the cutoff, moved by FM; at half rate: 13 table steps an octave up */
            int32_t cut = (int32_t)P.fx[FX_DFREQ] + 13 + MUL15(l, (int32_t)P.fx[FX_DFM] * 64 / 127);
            svf_set(&X.df[c], SVF_G[clampi(cut, 0, 127)], P.fx[FX_DRES]);
        }
    }
    for (i = 0; i < n; i++) {
        if (i & 1u) {                           /* every other frame: one step of the half-rate line */
            int32_t r[2];
            for (c = 0; c < 2u; c++) {
                uint32_t dw = d8[c] >> 8, fr = d8[c] & 255u;
                int32_t a = dl_buf[c][(X.dw - dw) & (DL_LEN - 1u)];
                if (fr) a += ((dl_buf[c][(X.dw - dw - 1u) & (DL_LEN - 1u)] - a) * (int32_t)fr) >> 8;
                if (filt) a = svf_run(&X.df[c], a, ftype);
                r[c] = a;
                X.ddamp[c] += MUL15(r[c] - X.ddamp[c], damp);
            }
            for (c = 0; c < 2u; c++) {
                int32_t in = MUL15((X.dodd[c] + b[2u * i + c]) >> 1, send);
                int32_t back = mix15(X.ddamp[c], X.ddamp[c ^ 1u], w);   /* width: the other side's echo */
                if (lo) { X.dlo[c] += MUL15(in - X.dlo[c], lo); in -= X.dlo[c]; }   /* LO CUT going in */
                dl_buf[c][X.dw & (DL_LEN - 1u)] = (int16_t)(filt ? soft15(in + MUL15(back, fb))   /* (limiting: resonance) */
                                                                 : clip15(in + MUL15(back, fb)));
                X.dprev[c] = X.dcur[c];
                X.dcur[c] = r[c];
            }
            X.dw++;
        } else
            for (c = 0; c < 2u; c++) X.dodd[c] = b[2u * i + c];
        for (c = 0; c < 2u; c++) {              /* (half way between the last two: a linear step) */
            int32_t o = (i & 1u) ? X.dcur[c] : (X.dprev[c] + X.dcur[c]) >> 1;
            b[2u * i + c] += o;
            dl_out[2u * i + c] = o;
        }
    }
    if (X.dcur[0] > -4 && X.dcur[0] < 4 && X.dcur[1] > -4 && X.dcur[1] < 4 && !send) X.dcur[0] = X.dcur[1] = 0;
}

/* ---- the reverb */
static const uint16_t RV_L[4] = {1777, 2111, 2593, 3001};   /* the lines at full SIZE (half-rate frames) */
static const uint16_t AP_L[4] = {105, 79, 281, 205};        /* the diffusers */
static const int16_t AP_C[4] = {20480, 20480, 16384, 16384}; /* their coefficients: 0.625, 0.5 */

static int32_t q31_pow(uint32_t d, uint32_t e)  /* d^e, both Q31 (d < 1) */
{
    uint64_t r = 0x80000000u, b = d;
    while (e) {
        if (e & 1u) r = (r * b) >> 31;
        b = (b * b) >> 31;
        e >>= 1;
    }
    return (int32_t)(r >> 16);                  /* Q15 */
}

static void reverb_coeffs(void)                 /* when TIME, SIZE, DAMP or MOD moved: a few multiplies */
{
    uint32_t k, t = P.fx[FX_RTIME], sz = P.fx[FX_RSIZE], dm = P.fx[FX_RDAMP], md = P.fx[FX_RMOD];
    if (t == X.rp[0] && sz == X.rp[1] && dm == X.rp[2] && md == X.rp[3]) return;
    for (k = 0; k < 4u; k++) {
        X.rlen[k] = RV_L[k] * (40u + sz * 87u / 127u) / 127u;   /* 31 % .. 100 % */
        X.rg[k] = q31_pow(RV_DECAY[t & 127u], X.rlen[k]);
    }
    X.rdamp = 32767 - (int32_t)dm * 232;        /* the loop's low pass: open .. ~1 kHz */
    X.rmod = (int32_t)md * 128;                 /* up to 16 frames of wobble, Q8 */
    X.rp[0] = (uint8_t)t; X.rp[1] = (uint8_t)sz; X.rp[2] = (uint8_t)dm; X.rp[3] = (uint8_t)md;
}

/* toward zero, not toward minus infinity: inside a feedback loop, floor() keeps a -1 going round forever */
static inline int32_t mulz15(int32_t a, int32_t b)   /* |a| < 65536, 0 <= b < 32768: fits 32 bits */
{
    int32_t p = a * b;
    return p >= 0 ? p >> 15 : -((-p) >> 15);
}
static inline int32_t halfz(int32_t x) { return (x + (x < 0)) >> 1; }

static int32_t rv_read(uint32_t k, uint32_t d8)  /* line k, d8 frames back (Q8): linear between two */
{
    uint32_t d = d8 >> 8, fr = d8 & 255u;
    int32_t a = rv_buf[k][(X.rw - d) & (RV_LEN - 1u)], b = rv_buf[k][(X.rw - d - 1u) & (RV_LEN - 1u)];
    return a + (((b - a) * (int32_t)fr) >> 8);
}

static void rv_step(int32_t in, int32_t *ol, int32_t *or_)   /* one half-rate frame */
{
    uint32_t k;
    int32_t r[4], h[4], a, bq, c, d, wob, low = P.fx[FX_RLOW];
    if (low) {                                  /* LOW: a one-pole high pass on the way in, 20 Hz .. ~1 kHz */
        X.rhp += MUL15(in - X.rhp, low * low * 9337 / 16129);
        in -= X.rhp;
    }
    for (k = 0; k < 4u; k++) {                  /* the diffusers: allpasses in a row */
        int32_t dl = ap_buf[k][(X.apw - AP_L[k]) & (AP_LEN - 1u)], v = clip15(in - mulz15(dl, AP_C[k]));
        ap_buf[k][X.apw & (AP_LEN - 1u)] = (int16_t)v;
        in = dl + mulz15(v, AP_C[k]);
    }
    X.apw++;
    X.rlfo += 0x00030000u;                      /* ~0.7 Hz at half rate */
    wob = MUL15(sin15(X.rlfo), X.rmod);         /* Q8 frames */
    r[0] = rv_read(0, (uint32_t)((int32_t)(X.rlen[0] << 8) + wob));   /* two lines wobble (MOD), two don't */
    r[1] = rv_buf[1][(X.rw - X.rlen[1]) & (RV_LEN - 1u)];
    r[2] = rv_read(2, (uint32_t)((int32_t)(X.rlen[2] << 8) - wob));
    r[3] = rv_buf[3][(X.rw - X.rlen[3]) & (RV_LEN - 1u)];
    for (k = 0; k < 4u; k++) {
        X.rlp[k] += mulz15(clampi(r[k] - X.rlp[k], -65535, 65535), X.rdamp);
        h[k] = mulz15(X.rlp[k], X.rg[k]);
    }
    a = h[0] + h[1]; bq = h[0] - h[1]; c = h[2] + h[3]; d = h[2] - h[3];   /* Hadamard, /2: it keeps the energy */
    in = halfz(in);
    rv_buf[0][X.rw & (RV_LEN - 1u)] = (int16_t)clip15(halfz(a + c) + in);
    rv_buf[1][X.rw & (RV_LEN - 1u)] = (int16_t)clip15(halfz(bq + d) - in);
    rv_buf[2][X.rw & (RV_LEN - 1u)] = (int16_t)clip15(halfz(a - c) + in);
    rv_buf[3][X.rw & (RV_LEN - 1u)] = (int16_t)clip15(halfz(bq - d) - in);
    X.rw++;
    *ol = halfz(r[0] + r[2]);
    *or_ = halfz(r[1] + r[3]);
}

static void reverb(int32_t *b, const int32_t *dry, uint32_t n)
{
    uint32_t i, c;
    int32_t send = (int32_t)P.fx[FX_RSEND] * 258, dsend = (int32_t)P.fx[FX_RDLY] * 258;
    int32_t width = (int32_t)P.fx[FX_RWIDTH] * 258, peak = 0;
    if (!send && !dsend && X.rquiet > 4000) return;   /* (the tail has played out: nothing to do) */
    reverb_coeffs();
    for (i = 1; i < n; i += 2u) {               /* one half-rate step per pair of frames */
        int32_t l, r, m, sd, in, o;
        in = ((dry[i - 1u] + dry[i]) >> 1) * (send >> 7)
           + ((dl_out[2u * i - 2u] + dl_out[2u * i - 1u] + dl_out[2u * i] + dl_out[2u * i + 1u]) >> 2) * (dsend >> 7);
        rv_step(in >> 8, &l, &r);               /* (send, dsend: Q15 >> 7 = Q8) */
        m = (l + r) >> 1; sd = (((l - r) >> 1) * (width >> 7)) >> 8;   /* WIDTH: 0 mono .. 127 full */
        X.rprev[0] = X.rcur[0]; X.rprev[1] = X.rcur[1];
        X.rcur[0] = m + sd; X.rcur[1] = m - sd;
        for (c = 0; c < 2u; c++) {
            int32_t mid = (X.rprev[c] + X.rcur[c]) >> 1;   /* (half way between the last two: a linear step) */
            b[2u * (i - 1u) + c] += mid;
            b[2u * i + c] += X.rcur[c];
            o = X.rcur[c] < 0 ? -X.rcur[c] : X.rcur[c];
            if (o > peak) peak = o;
        }
    }
    X.rquiet = (peak > 3 || send || dsend) ? 0 : X.rquiet + (int32_t)n;   /* frames of silence in a row */
}

static void out_stage(int32_t *b, uint32_t n)
{
    uint32_t i;
    int32_t dirt = P.fx[FX_DIRT], norm = P.fx[FX_NORM], gain = (int32_t)P.fx[FX_GAIN] * P.fx[FX_GAIN], g0, g1, peak = 0;
    if (dirt)
        for (i = 0; i < 2u * n; i++) b[i] = mix15(b[i], drive15(b[i], 256 + dirt * 40), dirt * 258);
    for (i = 0; i < 2u * n; i++) { int32_t a = b[i] < 0 ? -b[i] : b[i]; if (a > peak) peak = a; }
    X.norm_env = peak > X.norm_env ? peak : X.norm_env - (X.norm_env >> 7);
    g0 = X.norm_g;
    g1 = 4096;
    if (norm) {                                 /* NORM: up to 4x, so the loudest part sits at -3 dB */
        int32_t gmax = 4096 + norm * 96, env = X.norm_env > 256 ? X.norm_env : 256;
        g1 = (int32_t)((23000u << 12) / (uint32_t)env);
        if (g1 > gmax) g1 = gmax;
    }
    X.norm_g = g1 < g0 ? g1 : g0 + ((g1 - g0) >> 4);   /* down at once, up slowly */
    for (i = 0; i < n; i++) {
        int32_t g = g0 + (((X.norm_g - g0) * (int32_t)i) >> 5), c;   /* (n is BLK, 32) */
        g = (g * gain) >> 12;                   /* GAIN: 0..127 -> 0..~4x, 64 = 1x */
        for (c = 0; c < 2; c++) b[2u * i + c] = limit15((int32_t)(((int64_t)b[2u * i + c] * g) >> 12));
    }
}

/* the GATE: its step from the bar's position (1/8: >> 9, 1/16: >> 8, 1/32: >> 7 of the 16ths in Q8) */
static void gate(int32_t *b, uint32_t n, uint32_t where)
{
    static const uint8_t SH[3] = {9, 8, 7};
    uint32_t i, sh = SH[P.grate < 3u ? P.grate : 1u];
    int32_t tg = 32767, k = 32767 >> (P.gsmooth >> 4);   /* SMOOTH 0: at once .. 127: ~4 ms (a ramp per frame) */
    if (P.gmode != where) return;
    if (!playing) { if (X.gate_now == 32767) return; }
    else if (P.gate[(pos16_q8() >> sh) & 15u] & ST_ON) tg = 32767 - (int32_t)P.gdepth * 258;
    if (tg < 0) tg = 0;
    if (tg == 32767 && X.gate_now == 32767) return;
    for (i = 0; i < n; i++) {
        X.gate_now += MUL15(tg - X.gate_now, k);
        if (X.gate_now > 32700 && tg == 32767) X.gate_now = 32767;
        b[2u * i] = MUL15(b[2u * i], X.gate_now);
        b[2u * i + 1u] = MUL15(b[2u * i + 1u], X.gate_now);
    }
}

/* ---- PULP (after 0.3) */
static struct {
    svf_t lp; int32_t hi;                       /* the tracker's low pass, at HI */
    uint32_t cnt; uint8_t armed;                /* frames since the last rising crossing; below the lower threshold */
    int32_t inc, tinc;                          /* the oscillator's step, gliding to what the tracker heard */
    uint32_t ph, env;
    int32_t pk;                                 /* TRIG MIX: the recent peak, for the onsets */
} U = {.hi = -1};
static const uint8_t PT_MASK[4] = {0, 1u << V_KICK, (1u << V_KICK) | (1u << V_SNARE),
                                   (1u << V_KICK) | (1u << V_SNARE) | (1u << V_HATC) | (1u << V_HATO)};

static int32_t pulp_wave(uint32_t ph, int32_t w)   /* WAVE: 0 sine .. 64 triangle .. 127 square (soft) */
{
    int32_t s = sin15(ph), t = tri15(ph - 0x40000000u);   /* (the triangle in phase with the sine) */
    if (w < 64) return s + (t - s) * w / 64;
    return t + (drive15(s, 4096) - t) * (w - 64) / 63;
}

static void pulp(int32_t *b, uint32_t n)
{
    uint32_t i, hits = pulp_hits, lo, hi, pmin, pmax;
    int32_t lvl = P.fx2[F2_PLVL], amp, gl, gr, w = P.fx2[F2_PWAVE], k, d;
    uint32_t ek;
    pulp_hits = 0;
    if (!lvl) { U.env = 0; return; }
    lo = SVF_HZ[P.fx2[F2_PLO] & 127u]; hi = SVF_HZ[P.fx2[F2_PHI] & 127u];
    if (lo > hi) { uint32_t x = lo; lo = hi; hi = x; }
    pmin = 44100u / hi; pmax = 44100u / lo;
    if (pmin < 3u) pmin = 3u;
    if (U.hi != (int32_t)P.fx2[F2_PHI]) {
        U.hi = P.fx2[F2_PHI];
        svf_set(&U.lp, SVF_G[P.fx2[F2_PHI] & 127u], 0);
        if (!U.tinc) U.tinc = U.inc = (int32_t)(lo * 97392u);   /* (nothing heard yet: LO) */
    }
    if (P.fx2[F2_PTRIG] == BS_MIX) {            /* MIX: an onset is a peak well over the recent ones */
        int32_t pk = 0;
        for (i = 0; i < 2u * n; i++) { int32_t a = b[i] < 0 ? -b[i] : b[i]; if (a > pk) pk = a; }
        if (pk > 2 * U.pk + 1500) hits = 1;
        else hits = 0;
        U.pk = pk > U.pk ? pk : U.pk - (U.pk >> 4);
    } else hits &= PT_MASK[P.fx2[F2_PTRIG] & 3u];
    if (hits) U.env = ENV_ONE;
    ek = DECAY_K[P.fx2[F2_PDEC] & 127u];
    k = (int32_t)follow_blocks(P.fx2[F2_PGLIDE]);   /* GLIDE: 1 .. 505 blocks */
    d = (U.tinc - U.inc) / k;
    U.inc += d ? d : (U.tinc > U.inc) - (U.tinc < U.inc);
    amp = lvl * lvl * 2;
    pan_gains(P.fx2[F2_PPAN], &gl, &gr);
    for (i = 0; i < n; i++) {
        int32_t f = svf_run(&U.lp, (b[2u * i] + b[2u * i + 1u]) >> 1, 0), s;
        if (U.cnt < 60000u) U.cnt++;
        if (U.armed && f > 96) {                /* a rising crossing (with a little hysteresis): a period */
            if (U.cnt >= pmin && U.cnt <= pmax) U.tinc = (int32_t)(0xFFFFFFFFu / U.cnt);
            U.cnt = 0; U.armed = 0;
        } else if (f < -96) U.armed = 1;
        if (U.env < (1u << 17)) continue;
        U.ph += (uint32_t)U.inc;
        s = MUL15(MUL15(pulp_wave(U.ph, w), (int32_t)(U.env >> 15)), amp);
        U.env = env_dec(U.env, ek);
        b[2u * i] += MUL15(s, gl);
        b[2u * i + 1u] += MUL15(s, gr);
    }
}

static void fx_render(int32_t *b, uint32_t n)
{
    int32_t dry[32];                            /* (n is BLK, 32) the mix before the delay, mono: the reverb's SEND */
    uint32_t i;
    pulp(b, n);
    gate(b, n, GM_PRE);
    xfx(b, n);
    master_filter(b, n);
    eq(b, n);
    for (i = 0; i < n && i < 32u; i++) dry[i] = (b[2u * i] + b[2u * i + 1u]) >> 1;
    delay(b, n);
    reverb(b, dry, n);
    out_stage(b, n);
    gate(b, n, GM_POST);
}
