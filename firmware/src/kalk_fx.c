/* SPDX-License-Identifier: GPL-3.0-only */
/* KALK: the effects, after the voices, in the audio interrupt (32 frames at a time).
 *
 *   XFX      one of 12 effects on an XY pad (K2 = X, K3 = Y); the pad's motion recorded over 4 bars (64 points)
 *   FILTER   12 dB low pass, its cutoff moved by the mod lane and by the level (TRK)
 *   EQ       lo / mid / hi and a saturating AMP
 *   DELAY    stereo, tempo synced in 16ths, feedback, damping, width (ping-pong)
 *   REVERB   a metallic FDN at half rate: 4 diffusers, 4 lines (Hadamard), RT60, size, damping, low cut,
 *            modulation, width; fed from the mix (SEND) and from the delay (DLY>RV)
 *   OUT      DIRT (distortion), NORM (a maximiser), GAIN
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
    /* filter */
    svf_t mf[2]; int32_t mcut, mres, follow;
    /* eq */
    int32_t lo1[2], lo2[2];
    /* delay */
    uint32_t dw; int32_t dprev[2], dcur[2], ddamp[2], dodd[2];
    /* reverb */
    uint32_t rw, apw, rlfo;
    uint8_t rp[4];                              /* TIME SIZE DAMP MOD the coefficients below are for */
    uint32_t rlen[4]; int32_t rg[4], rdamp, rmod;
    int32_t rlp[4], rhp, rprev[2], rcur[2], rquiet;
    /* out */
    int32_t norm_env, norm_g;
} X = {.type = 255, .norm_g = 4096, .rp = {255, 255, 255, 255}};

static int32_t mix15(int32_t dry, int32_t wet, int32_t m) { return dry + MUL15(wet - dry, m); }
static int32_t fold15(int32_t x)                /* a triangle fold: whatever goes past full scale comes back */
{
    uint32_t u = (uint32_t)(x + 32768) & 131071u;
    int32_t v = (int32_t)(u < 65536u ? u : 131071u - u);
    return v - 32768;
}
/* the phase of the bar now, in 16ths, Q8 (for the synced LFOs) */
static uint32_t pos16_q8(void) { return gstep * 256u + (S.frac >> 16); }

static void xfx(int32_t *b, uint32_t n)
{
    uint32_t type = P.fx[FX_XTYPE], i, c;
    int32_t x = fx_x, y = fx_y, mix = (int32_t)P.fx[FX_XMIX] * 258, modfx = P.fx[FX_MODFX];
    if (modfx == 1u && !mod_now) mix = 0;       /* GATE: only on the mod lane's steps */
    if (modfx == 2u) mix = MUL15(mix, mod_now * 2184);   /* MIX: the mod value is the amount */
    if (modfx == 3u && mod_now) x = mod_now * 8;   /* X: the mod value moves X */
    if (type >= NXF || type == XF_THRU || !mix) { X.rp_on = 0; return; }
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

static void master_filter(int32_t *b, uint32_t n)
{
    int32_t cut = P.fx[FX_FCUT], res = P.fx[FX_FRES], peak = 0;
    uint32_t i, c;
    cut += ((int32_t)P.fx[FX_FMOD] - 64) * mod_now / 8;
    cut += MUL15(X.follow, (int32_t)P.fx[FX_FTRK]) >> 1;
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

static void delay(int32_t *b, uint32_t n)
{
    for (uint32_t k = 0; k < 2u * n; k++) dl_out[k] = 0;
    uint32_t i, c, f16 = frames16 / 2u;
    uint32_t dt[2] = {f16 * (P.fx[FX_DL] ? P.fx[FX_DL] : 1u), f16 * (P.fx[FX_DR] ? P.fx[FX_DR] : 1u)};
    int32_t send = (int32_t)P.fx[FX_DSEND] * 258, fb = (int32_t)P.fx[FX_DFB] * 240, w = (int32_t)P.fx[FX_DWIDTH] * 258;
    int32_t damp = 32767 - (int32_t)P.fx[FX_DDAMP] * 240;
    if (!send) {
        if (!X.dcur[0] && !X.dcur[1]) return;   /* (the tail plays out, then nothing to do) */
    }
    for (c = 0; c < 2u; c++) if (dt[c] >= DL_LEN) dt[c] = DL_LEN - 1u;
    for (i = 0; i < n; i++) {
        if (i & 1u) {                           /* every other frame: one step of the half-rate line */
            int32_t r[2];
            for (c = 0; c < 2u; c++) {
                r[c] = dl_buf[c][(X.dw - dt[c]) & (DL_LEN - 1u)];
                X.ddamp[c] += MUL15(r[c] - X.ddamp[c], damp);
            }
            for (c = 0; c < 2u; c++) {
                int32_t in = MUL15((X.dodd[c] + b[2u * i + c]) >> 1, send);
                int32_t back = mix15(X.ddamp[c], X.ddamp[c ^ 1u], w);   /* width: the other side's echo */
                dl_buf[c][X.dw & (DL_LEN - 1u)] = (int16_t)clip15(in + MUL15(back, fb));
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

static void fx_render(int32_t *b, uint32_t n)
{
    int32_t dry[32];                            /* (n is BLK, 32) the mix before the delay, mono: the reverb's SEND */
    uint32_t i;
    xfx(b, n);
    master_filter(b, n);
    eq(b, n);
    for (i = 0; i < n && i < 32u; i++) dry[i] = (b[2u * i] + b[2u * i + 1u]) >> 1;
    delay(b, n);
    reverb(b, dry, n);
    out_stage(b, n);
}
