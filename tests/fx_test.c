/* SPDX-License-Identifier: GPL-3.0-only */
/* KALK's effects on their own, starting with the reverb: an impulse and a burst of noise through the effects chain.
 * Checks the reverb rings, decays at about its TIME, never runs away (every TIME, SIZE, DAMP), stays silent at
 * send 0, and that a 0.2 project's effects (no reverb fields) leave it off. Writes build/host/reverb.wav.
 * The reson delay: the 0.2 delay bit for bit at rest, resonance at its FREQ, no runaway, the LFO, INVERT.
 * The glitch XFX (CHOP SLICE FREEZE SPIN REFLEX PITCH): bounded, nothing at XMIX 0, each does what it says.
 *   cc -O2 -Ifirmware/src -o build/host/fx_test tests/fx_test.c -lm && build/host/fx_test */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "kalk_model.h"
static project_t P;
#include "kalk_dsp.h"
#include "kalk_voice.c"
#include "kalk_seq.c"
#include "kalk_fx.c"

#define FS 44100
#define B 32u
static int fails;
static void check(int ok, const char *what)
{
    printf("%-4s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) fails++;
}

static void fx_reset(void)
{
    memset(&X, 0, sizeof X);
    X.type = 255; X.norm_g = 4096;
    memset(X.rp, 255, sizeof X.rp);
    memset(rv_buf, 0, sizeof rv_buf); memset(ap_buf, 0, sizeof ap_buf); memset(dl_buf, 0, sizeof dl_buf);
}
static void fx_neutral(void)                    /* everything off but the reverb: XFX thru, no delay, flat EQ, no norm */
{
    static const uint8_t F[NFX] = {XF_THRU, 64, 32, 0,  3, 4, 0, 0,  0, 0, 127, 0,  127, 0, 64, 0,  64, 64, 64, 0,
                                   64, 12, 0, 0,  70, 90, 60, 0,  0, 0, 127, 0,  114, 0, 0, 0,  4, 0, 0, 0};
    memcpy(P.fx, F, sizeof F);
}

/* run `secs` of audio; the input is an impulse (kind 0) or 50 ms of noise (kind 1) at the start; returns the
 * RMS of each 10 ms in env[], the peak in *pk; writes to w if given */
static void run(double secs, int kind, double *env, double *pk, FILE *w)
{
    static int32_t b[2 * B];
    uint32_t frames = (uint32_t)(secs * FS), f = 0, i, k = 0;
    double acc = 0;
    uint32_t cnt = 0;
    *pk = 0;
    while (f < frames) {
        for (i = 0; i < B; i++, f++) {
            int32_t x = 0;
            if (kind == 0 && f == 0) x = 30000;
            if (kind == 1 && f < FS / 20) x = noise15();
            b[2 * i] = b[2 * i + 1] = x;
        }
        fx_render(b, B);
        for (i = 0; i < 2 * B; i++) {
            double v = b[i] / 32768.0;
            if (fabs(v) > *pk && f > B) *pk = fabs(v);
            acc += v * v;
            if (++cnt == 2u * FS / 100u) { if (env) env[k] = sqrt(acc / cnt); k++; acc = 0; cnt = 0; }
        }
        if (w) for (i = 0; i < 2 * B; i++) { int16_t s = (int16_t)clip15(b[i]); fwrite(&s, 2, 1, w); }
    }
}

/* the bar's position at frame f, as the sequencer would have it (5512 frames a 16th) */
static void gl_pos(uint32_t f) { gstep = f / 5512u; S.frac = (f % 5512u) * (Q24 / 5512u); }

static double db(double x) { return 20 * log10(x + 1e-12); }

int main(void)
{
    static double env[3000];
    double pk;
    uint32_t t, sz, dm, i;
    FILE *w = fopen("build/host/reverb.wav", "wb");
    frames16 = 5512;

    /* send 0: nothing comes out but the dry impulse */
    fx_reset(); fx_neutral();
    run(1.0, 1, env, &pk, NULL);
    check(env[20] < 1e-6, "SEND 0: after the dry burst, silence");

    /* a burst of noise into a 2 s reverb: it rings, then falls ~60 dB in about 2 s */
    fx_reset(); fx_neutral();
    P.fx[FX_RSEND] = 127;
    for (t = 0; t < 128u && RV_MS[t] < 2000u; t++) ;
    P.fx[FX_RTIME] = (uint8_t)t;
    if (w) fseek(w, 44, SEEK_SET);
    run(6.0, 1, env, &pk, w);
    {
        double a = db(env[20]), b2 = db(env[100]), rt = 60.0 / ((a - b2) / 0.8);   /* the slope from 0.2 to 1.0 s */
        printf("     TIME %u ms: %.1f dB at 0.2 s, %.1f dB at 1.0 s: RT60 measured %.0f ms\n", RV_MS[t], a, b2, rt * 1000);
        check(env[10] > 1e-3, "the reverb rings after the burst");
        check(rt > RV_MS[t] * 0.0005 && rt < RV_MS[t] * 0.0015, "it decays at about its TIME (within 0.5x .. 1.5x; DAMP shortens it)");
        check(env[590] < 1e-6, "it ends: digital silence (no limit cycle)");
    }

    /* the delay into the reverb */
    fx_reset(); fx_neutral();
    P.fx[FX_DSEND] = 100; P.fx[FX_DFB] = 40; P.fx[FX_RDLY] = 127;
    run(2.0, 0, env, &pk, NULL);
    check(env[60] > 1e-4, "DLY>RV: the echoes reach the reverb");

    /* no runaway: every TIME, SIZE, DAMP with full sends, noise in; the output stays under full scale and finite */
    {
        int bad = 0;
        for (t = 0; t < 128u; t += 21u)
            for (sz = 0; sz < 128u; sz += 42u)
                for (dm = 0; dm < 128u; dm += 63u) {
                    fx_reset(); fx_neutral();
                    P.fx[FX_RSEND] = 127; P.fx[FX_RTIME] = (uint8_t)t; P.fx[FX_RSIZE] = (uint8_t)sz;
                    P.fx[FX_RDAMP] = (uint8_t)dm; P.fx[FX_RMOD] = 127; P.fx[FX_RLOW] = 0;
                    run(3.0, 1, env, &pk, NULL);
                    if (!(pk < 1.0) || env[290] > env[10] * 4 + 1e-3) { bad++; printf("     runaway? TIME %u SIZE %u DAMP %u: peak %.2f\n", t, sz, dm, pk); }
                }
        check(!bad, "no runaway at any TIME, SIZE, DAMP (full send, full MOD)");
    }

    /* WIDTH 0: mono */
    {
        static int32_t b[2 * B];
        int diff = 0;
        fx_reset(); fx_neutral();
        P.fx[FX_RSEND] = 127; P.fx[FX_RWIDTH] = 0;
        for (i = 0; i < 400u; i++) {
            uint32_t j;
            for (j = 0; j < B; j++) b[2 * j] = b[2 * j + 1] = i < 40u ? noise15() : 0;
            fx_render(b, B);
            for (j = 0; j < B; j++) if (i > 60u && b[2 * j] != b[2 * j + 1]) diff++;
        }
        check(!diff, "WIDTH 0: the reverb is mono");
    }


    /* ---- the reson delay */
    {   /* at rest it is the 0.2 delay, bit for bit: a fingerprint of 6 s through the chain (short and long times) */
        static const uint8_t F02[24] = {XF_THRU, 64, 32, 1, 3, 4, 90, 100, 50, 90, 127, 0, 127, 0, 64, 0, 64, 64, 64, 0, 64, 12, 0, 40};
        static const uint32_t WANT[2] = {0xc96f812eu, 0x0d7b1e9cu};   /* (from 0.2's kalk_fx.c) */
        uint32_t v, k, h;
        int same = 1;
        static int32_t b[2 * B];
        for (v = 0; v < 2u; v++) {
            fx_reset();
            memset(P.fx, 0, sizeof P.fx); memcpy(P.fx, F02, 24); P.fx[FX_DFREQ] = 114; P.fx[FX_DLFO] = 4;
            if (v) { P.fx[FX_DL] = 12; P.fx[FX_DR] = 16; }
            k_rng = 0x2545F491u;
            h = 2166136261u;
            for (k = 0; k < 44100u * 6u / 32u; k++) {
                for (i = 0; i < 2 * B; i++) b[i] = (k % 600u < 20u) ? noise15() : 0;
                fx_render(b, B);
                for (i = 0; i < 2 * B; i++) { h ^= (uint32_t)b[i]; h *= 16777619u; }
            }
            if (h != WANT[v]) { same = 0; printf("     fingerprint %u: %08x, want %08x\n", v, h, WANT[v]); }
        }
        check(same, "RESON / TAPE LFO at rest: the 0.2 delay, bit for bit (short and long times)");
    }
    {   /* resonance: BP at ~1 kHz, high RES and feedback, an impulse: the echoes ring near 1 kHz */
        static int32_t b[2 * B];
        static int16_t y[44100];
        uint32_t k, n = 0, f, zc = 0, best = 0;
        fx_reset(); fx_neutral();
        for (f = 0; f < 114u && SVF_HZ[f] < 1000u; f++) ;
        P.fx[FX_DSEND] = 127; P.fx[FX_DFB] = 100; P.fx[FX_DFREQ] = (uint8_t)f; P.fx[FX_DRES] = 120; P.fx[FX_DFTYPE] = 1;
        for (k = 0; k < 44100u / 32u; k++) {
            for (i = 0; i < 2 * B; i++) b[i] = (k == 0u && i < 2u) ? 30000 : 0;
            fx_render(b, B);
            for (i = 0; i < B && n < 44100u; i++) y[n++] = (int16_t)b[2 * i];
        }
        {                                       /* the strongest of 1/12-octave steps, 200 Hz .. 5 kHz (Goertzel) */
            double fr, bestp = 0;
            for (fr = 200; fr < 5000; fr *= 1.0595) {
                double w = 2 * cos(2 * M_PI * fr / 44100), s0, s1 = 0, s2 = 0, pw;
                for (k = 4410u; k < 44100u; k++) { s0 = y[k] + w * s1 - s2; s2 = s1; s1 = s0; }
                pw = s1 * s1 + s2 * s2 - w * s1 * s2;
                if (pw > bestp) { bestp = pw; best = (uint32_t)fr; }
            }
        }
        (void)zc;
        printf("     BP at %u Hz, RES 120: rings at ~%u Hz\n", SVF_HZ[f], best);
        check(best > SVF_HZ[f] * 7u / 10u && best < SVF_HZ[f] * 13u / 10u, "RESON: the echoes ring at its FREQ");
    }
    {   /* no runaway: full feedback, full resonance, every filter, full LFO, noise in */
        int bad = 0;
        uint32_t ft;
        for (ft = 0; ft < 3u; ft++) {
            fx_reset(); fx_neutral();
            P.fx[FX_DSEND] = 127; P.fx[FX_DFB] = 127; P.fx[FX_DRES] = 127; P.fx[FX_DFTYPE] = (uint8_t)ft;
            P.fx[FX_DFREQ] = 60; P.fx[FX_DFM] = 127; P.fx[FX_DDMOD] = 127; P.fx[FX_DLFO] = 0; P.fx[FX_DWIDTH] = 127;
            run(4.0, 1, env, &pk, NULL);
            if (!(pk < 1.0)) { bad++; printf("     filter %u: peak %.2f\n", ft, pk); }
        }
        check(!bad, "RESON: no runaway at full feedback and resonance (LP, BP, HP; full FM and DMOD)");
    }
    {   /* the tape LFO moves the time: a chorus at time 0 with DMOD differs from without; INVERT flips the 2nd echo */
        static int32_t b[2 * B];
        double e0[3000], e1[3000], p0, p1;
        int32_t first[2] = {0, 0}, second[2] = {0, 0};
        uint32_t v, k, f;
        fx_reset(); fx_neutral(); P.fx[FX_DSEND] = 127; P.fx[FX_DFB] = 60; P.fx[FX_DL] = P.fx[FX_DR] = 0;
        k_rng = 1234u;
        run(0.5, 1, e0, &p0, NULL);
        fx_reset(); fx_neutral(); P.fx[FX_DSEND] = 127; P.fx[FX_DFB] = 60; P.fx[FX_DL] = P.fx[FX_DR] = 0;
        P.fx[FX_DDMOD] = 100; P.fx[FX_DLFO] = 0;
        k_rng = 1234u;
        run(0.5, 1, e1, &p1, NULL);
        {
            double dsum = 0;
            for (k = 1; k < 8u; k++) dsum += fabs(db(e0[k]) - db(e1[k]));   /* during the 50 ms burst */
            printf("     chorus with and without DMOD: %.2f dB apart over the burst\n", dsum / 7);
            check(dsum / 7 > 0.05, "TAPE LFO: DMOD moves the time (a chorus at time 0)");
        }
        for (v = 0; v < 2u; v++) {              /* the 2nd echo against the 1st, with and without INVERT */
            uint32_t at1 = 2u * 2756u * 3u, at2 = 2u * at1, nn = 0;   /* (3/16 at 124 BPM, in frames) */
            fx_reset(); fx_neutral(); P.fx[FX_DSEND] = 127; P.fx[FX_DFB] = 100; P.fx[FX_DL] = P.fx[FX_DR] = 3;
            P.fx[FX_DINV] = (uint8_t)v;
            for (k = 0; k < 44100u * 2u / 32u; k++) {
                for (i = 0; i < 2 * B; i++) b[i] = (k == 0u && i < 2u) ? 30000 : 0;
                fx_render(b, B);
                for (f = 0; f < B; f++, nn++) {
                    if (nn + 40u > at1 / 2u * 2u && nn < at1 + 40u && abs(b[2 * f]) > abs(first[v])) first[v] = b[2 * f];
                    if (nn + 40u > at2 && nn < at2 + 40u && abs(b[2 * f]) > abs(second[v])) second[v] = b[2 * f];
                }
            }
        }
        printf("     echoes: plain %d, %d; inverted %d, %d\n", first[0], second[0], first[1], second[1]);
        check((first[0] > 0) == (second[0] > 0) && (first[1] > 0) != (second[1] > 0) && first[1] && second[1],
              "INVERT: the feedback flips each echo's sign");
    }

    /* ---- the GATE (while playing): POST cuts to silence on its steps, PRE lets the echoes ring on, stopped: nothing */
    {
        static int32_t b[2 * B];
        double pk_cut[3], pk_open[3];
        uint32_t m, k;
        for (m = 0; m < 3u; m++) {              /* 0: POST, 1: PRE, 2: POST but stopped */
            fx_reset(); fx_neutral();
            P.fx[FX_DSEND] = 127; P.fx[FX_DFB] = 90; P.fx[FX_DL] = P.fx[FX_DR] = 1;   /* echoes a 16th apart */
            memset(P.gate, 0, sizeof P.gate);
            P.gate[2] = P.gate[3] = ST_ON;      /* cut steps 3 and 4 */
            P.gmode = m == 1u ? GM_PRE : GM_POST; P.grate = 1; P.gdepth = 127; P.gsmooth = 32;
            playing = m != 2u;
            pk_cut[m] = pk_open[m] = 0;
            for (gstep = 0; gstep < 8u; gstep++)
                for (k = 0; k < 5512u / B; k++) {
                    S.frac = k * B * (Q24 / 5512u);
                    for (i = 0; i < 2 * B; i++) b[i] = (gstep < 4u) ? noise15() : 0;   /* sound on steps 1..4 */
                    fx_render(b, B);
                    if (k > 20u)                /* (past the ramp) */
                        for (i = 0; i < 2 * B; i++) {
                            double a = fabs(b[i] / 32768.0);
                            if (gstep == 2u || gstep == 3u) { if (a > pk_cut[m]) pk_cut[m] = a; }
                            else if (gstep == 1u) { if (a > pk_open[m]) pk_open[m] = a; }
                        }
                }
        }
        playing = 0;
        printf("     peaks on the cut steps: POST %.4f, PRE %.4f, stopped %.4f (open step: %.3f)\n", pk_cut[0], pk_cut[1], pk_cut[2], pk_open[0]);
        check(pk_open[0] > 0.05 && pk_cut[0] < 0.001, "GATE POST: silence on its steps, sound on the others");
        check(pk_cut[1] > 0.01, "GATE PRE: the echoes ring on through the cut");
        check(pk_cut[2] > 0.05, "GATE: stopped, it does nothing (the pads play)");
        check(sizeof(project_t) < 4000u, "the project still fits its blob");
    }

    /* a 0.2 project: everything it had is kept; the new effects keep their defaults */
    {
        static project02_t o;
        static project_t t2;
        int same = 1;
        check(offsetof(project02_t, fx) == offsetof(project_t, fx), "0.2 projects: the same layout up to the effects");
        check(sizeof(project_t) != sizeof(project02_t), "0.2 and new projects differ in size (the store tells them apart)");
        for (i = 0; i < sizeof o; i++) ((uint8_t *)&o)[i] = (uint8_t)(i * 7u + 1u);
        o.magic = PROJ_MAGIC;
        memset(&t2, 0xAA, sizeof t2);           /* stands for the defaults */
        proj_upgrade02(&t2, &o);
        if (memcmp(&t2, o.head, sizeof o.head) || memcmp(t2.fx, o.fx, NFX02) || memcmp(t2.xy, o.xy, sizeof o.xy)) same = 0;
        for (i = NFX02; i < NFX; i++) if (t2.fx[i] != 0xAA) same = 0;
        for (i = 0; i < NSTEP; i++) if (t2.gate[i] != 0xAA) same = 0;   /* the GATE and the RMX LFO: defaults too */
        if (t2.gmode != 0xAA || t2.r2depth != 0xAA || t2.r2shape != 0xAA) same = 0;
        check(same && t2.magic == PROJ_MAGIC, "0.2 project upgrade: its steps, sounds, effects and motion kept; reverb from defaults");
    }

    /* a 0.3 project (step 4's layout): everything it had is kept; the EXTRA pages and fx2[] keep their defaults */
    {
        static project03_t o;
        static project_t t3;
        int same = 1;
        check(sizeof(project03_t) != sizeof(project_t) && sizeof(project03_t) != sizeof(project02_t),
              "0.2, 0.3 and new projects differ in size (the store tells them apart)");
        for (i = 0; i < sizeof o; i++) ((uint8_t *)&o)[i] = (uint8_t)(i * 5u + 3u);
        o.magic = PROJ_MAGIC;
        memset(&t3, 0xAA, sizeof t3);
        proj_upgrade03(&t3, &o);
        if (memcmp(&t3, o.head, sizeof o.head)) same = 0;
        for (i = 0; i < sizeof t3.vx; i++) if (((uint8_t *)t3.vx)[i] != 0xAA) same = 0;
        for (i = 0; i < NFX2; i++) if (t3.fx2[i] != 0xAA) same = 0;
        check(same && t3.magic == PROJ_MAGIC, "0.3 project upgrade: all of it kept; GRAIN, FOLLOW, PULP from defaults");
        check(VX_DEFAULT[0][VX_GRAIN] == 0 && VX_DEFAULT[0][VX_SPEED] == 64 && VX_DEFAULT[1][VX_SPEED] == 64,
              "GRAIN / SPEED at rest by default (0.3 projects sound the same)");
    }

    if (w) {                                    /* the WAV header */
        uint32_t n = (uint32_t)ftell(w) - 44u, v;
        fseek(w, 0, SEEK_SET);
        fwrite("RIFF", 1, 4, w); v = n + 36; fwrite(&v, 4, 1, w); fwrite("WAVEfmt ", 1, 8, w);
        v = 16; fwrite(&v, 4, 1, w); { uint16_t h[2] = {1, 2}; fwrite(h, 2, 2, w); }
        v = FS; fwrite(&v, 4, 1, w); v = FS * 4; fwrite(&v, 4, 1, w); { uint16_t h[2] = {4, 16}; fwrite(h, 2, 2, w); }
        fwrite("data", 1, 4, w); fwrite(&n, 4, 1, w);
        fclose(w);
    }

    /* ---- the glitch XFX (after 0.2) */
    {
        static const char *const NM[6] = {"CHOP", "SLICE", "FREEZE", "SPIN", "REFLEX", "PITCH"};
        static const uint8_t XS[4] = {0, 40, 90, 127}, YS[3] = {0, 64, 127};
        static int32_t b[2 * B], in[2 * B];
        uint32_t t, xi, yi, k, f;
        int bounded = 1, thru = 1;
        double worst = 0, wmax = 0;
        playing = 1;
        for (t = XF_CHOP; t < NXF; t++)
            for (xi = 0; xi < 4u; xi++)
                for (yi = 0; yi < 3u; yi++) {
                    fx_reset(); fx_neutral(); P.fx[FX_XTYPE] = (uint8_t)t; fx_x = XS[xi]; fx_y = YS[yi];
                    for (f = 0; f < FS; f += B) {
                        gl_pos(f);
                        for (i = 0; i < 2 * B; i++) b[i] = noise15() * 2;
                        xfx(b, B);
                        for (i = 0; i < 2 * B; i++) { double a = fabs(b[i] / 32768.0); if (a > worst) worst = a; }
                    }
                    if (worst > 4.0) { bounded = 0; printf("     %s X %u Y %u: %.2f\n", NM[t - XF_CHOP], fx_x, fx_y, worst); }
                    worst = worst > wmax ? (wmax = worst) : wmax; worst = 0;
                    P.fx[FX_XMIX] = 0;              /* XMIX 0: the input, untouched */
                    for (f = 0; f < FS / 4; f += B) {
                        gl_pos(f);
                        for (i = 0; i < 2 * B; i++) in[i] = b[i] = noise15() * 2;
                        xfx(b, B);
                        if (memcmp(b, in, sizeof b)) thru = 0;
                    }
                }
        printf("     every glitch XFX, X and Y at their ends: the loudest %.2f of full scale\n", wmax);
        check(bounded, "glitch XFX: bounded at every X and Y (noise in)");
        check(thru, "glitch XFX: XMIX 0 leaves the sound alone");

        /* CHOP: X 0 cuts each 16th short; stopped, it passes */
        {
            double early = 0, late = 0, stopped = 0;
            fx_reset(); fx_neutral(); P.fx[FX_XTYPE] = XF_CHOP; fx_x = 0; fx_y = 64;
            for (f = 0; f < 4u * 5512u; f += B) {
                uint32_t ph;
                gl_pos(f);
                ph = f % 5512u;
                for (i = 0; i < 2 * B; i++) b[i] = 16000;
                xfx(b, B);
                for (i = 0; i < 2 * B; i++) { if (ph < 100u) early += fabs(b[i]); else if (ph > 2756u) late += fabs(b[i]); }
            }
            playing = 0;
            for (f = 0; f < 5512u; f += B) {
                for (i = 0; i < 2 * B; i++) b[i] = 16000;
                xfx(b, B);
                if (f > 64u) for (i = 0; i < 2 * B; i++) stopped += fabs(b[i] - 16000);
            }
            playing = 1;
            printf("     %s: the first 100 frames of each 16th %.0f, its second half %.0f\n", NM[0], early, late);
            check(early > 1e5 && late == 0, "CHOP X 0: sound at each 16th's start, silence in its second half");
            check(stopped == 0, "CHOP: stopped, it passes the sound");
        }

        /* SLICE, Y at the end (every slice backwards), 1/16: a click 100 frames into each 16th comes back near the
         * end of the next one; Y 0 (pattern 0x8888): only every 4th 16th is turned */
        {
            uint32_t late = 0, total = 0, steps_moved = 0;
            for (k = 0; k < 2u; k++) {
                fx_reset(); fx_neutral(); P.fx[FX_XTYPE] = XF_SLICE; fx_x = 64; fx_y = k ? 0 : 127;
                for (f = 0; f < 16u * 5512u; f += B) {
                    double pk = 0; uint32_t at = 0;
                    gl_pos(f);
                    for (i = 0; i < B; i++) b[2 * i] = b[2 * i + 1] = ((f + i) % 5512u == 100u) ? 30000 : 0;
                    xfx(b, B);
                    for (i = 0; i < B; i++) if (fabs(b[2 * i]) > pk) { pk = fabs(b[2 * i]); at = (f + i) % 5512u; }
                    if (f >= 5512u * 2u && pk > 15000) {
                        if (!k) { total++; if (at > 5000u) late++; }
                        else if (at > 5000u) steps_moved++;
                    }
                }
            }
            printf("     %s: all backwards: %u of %u clicks late in their 16th; pattern 1: %u moved (14 steps)\n", NM[1], late, total, steps_moved);
            check(total >= 13u && late == total, "SLICE: every 16th plays the last one backwards (the click comes late)");
            check(steps_moved >= 3u && steps_moved <= 4u, "SLICE pattern 1: only every 4th 16th is turned");
        }

        /* FREEZE: listening (X under the notch), then frozen: the loop plays on with no input; Y 0: it stops */
        {
            double rms_in = 0, rms_frz = 0, pk_end = 0;
            fx_reset(); fx_neutral(); P.fx[FX_XTYPE] = XF_FREEZE; fx_x = 0; fx_y = 64;
            for (f = 0; f < FS; f += B) {
                gl_pos(f);
                for (i = 0; i < 2 * B; i++) b[i] = noise15();
                xfx(b, B);
                for (i = 0; i < 2 * B; i++) rms_in += (double)b[i] * b[i];
            }
            fx_x = 127;
            for (f = FS; f < 3 * FS; f += B) {
                gl_pos(f);
                memset(b, 0, sizeof b);
                xfx(b, B);
                for (i = 0; i < 2 * B; i++) rms_frz += (double)b[i] * b[i];
            }
            fx_y = 0;
            for (f = 3 * FS; f < 5 * FS; f += B) {
                gl_pos(f);
                memset(b, 0, sizeof b);
                xfx(b, B);
                if (f > 4 * FS) for (i = 0; i < 2 * B; i++) if (fabs(b[i]) > pk_end) pk_end = fabs(b[i]);
            }
            rms_in = sqrt(rms_in / (2.0 * FS)); rms_frz = sqrt(rms_frz / (4.0 * FS));
            printf("     %s: in %.0f, frozen (no input) %.0f, after Y 0 a peak of %.0f\n", NM[2], rms_in, rms_frz, pk_end);
            check(rms_frz > 0.7 * rms_in, "FREEZE: the loop plays on after the input stops");
            check(pk_end == 0, "FREEZE Y 0: the tape stops (silence)");
        }

        /* SPIN and REFLEX: a mono input comes out stereo; SPIN at depth 0: still mono */
        for (k = 0; k < 3u; k++) {
            double side = 0, mid = 0;
            fx_reset(); fx_neutral(); P.fx[FX_XTYPE] = k == 2u ? XF_REFLEX : XF_SPIN; fx_x = 64; fx_y = k == 1u ? 0 : 100;
            for (f = 0; f < 2 * FS; f += B) {
                gl_pos(f);
                for (i = 0; i < B; i++) b[2 * i] = b[2 * i + 1] = noise15();
                xfx(b, B);
                if (f > FS / 4) for (i = 0; i < B; i++) {
                    double l = b[2 * i], r = b[2 * i + 1];
                    side += (l - r) * (l - r); mid += (l + r) * (l + r);
                }
            }
            printf("     %s Y %u: side / mid %.3f\n", k == 2u ? NM[4] : NM[3], fx_y, sqrt(side / (mid + 1)));
            if (k == 1u) check(side == 0, "SPIN at depth 0: mono stays mono");
            else check(sqrt(side / (mid + 1)) > 0.1, k ? "REFLEX: L and R sweep apart (stereo from mono)" : "SPIN: the speaker turns (stereo from mono)");
        }

        /* PITCH: 440 Hz in; Y at the ends: an octave down, an octave up; the middle: unchanged */
        {
            static const uint8_t PY_[3] = {0, 64, 127};
            static const double HZ[3] = {220, 440, 880};
            for (k = 0; k < 3u; k++) {
                double e[3] = {0, 0, 0}, re[3] = {0, 0, 0}, im[3] = {0, 0, 0}, ph = 0;
                uint32_t h, best = 0;
                fx_reset(); fx_neutral(); P.fx[FX_XTYPE] = XF_PITCH; fx_x = 64; fx_y = PY_[k];
                for (f = 0; f < FS; f += B) {
                    gl_pos(f);
                    for (i = 0; i < B; i++, ph += 2 * M_PI * 440.0 / FS) b[2 * i] = b[2 * i + 1] = (int32_t)(16000 * sin(ph));
                    xfx(b, B);
                    if (f > FS / 5)
                        for (h = 0; h < 3u; h++)    /* (a DFT at the three, over 0.8 s) */
                            for (i = 0; i < B; i++) {
                                double w = 2 * M_PI * HZ[h] * (f + i) / FS;
                                re[h] += b[2 * i] * cos(w); im[h] += b[2 * i] * sin(w);
                            }
                }
                for (h = 0; h < 3u; h++) e[h] = re[h] * re[h] + im[h] * im[h];
                for (h = 1; h < 3u; h++) if (e[h] > e[best]) best = h;
                printf("     %s Y %u: energy at 220 / 440 / 880 Hz %.2g %.2g %.2g\n", NM[5], PY_[k], e[0], e[1], e[2]);
                check(best == k, k == 0u ? "PITCH Y 0: an octave down" : k == 1u ? "PITCH Y 64: the same pitch" : "PITCH Y 127: an octave up");
            }
        }
        playing = 0;
    }
    printf(fails ? "FX TESTS FAILED (%d)\n" : "FX TESTS PASSED\n", fails);
    return fails != 0;
}
