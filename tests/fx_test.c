/* SPDX-License-Identifier: GPL-3.0-only */
/* KALK's effects on their own, starting with the reverb: an impulse and a burst of noise through the effects chain.
 * Checks the reverb rings, decays at about its TIME, never runs away (every TIME, SIZE, DAMP), stays silent at
 * send 0, and that a 0.2 project's effects (no reverb fields) leave it off. Writes build/host/reverb.wav.
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
                                   64, 12, 0, 0,  70, 90, 60, 0,  0, 0, 127, 0};
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
        check(same && t2.magic == PROJ_MAGIC, "0.2 project upgrade: its steps, sounds, effects and motion kept; reverb from defaults");
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
    printf(fails ? "FX TESTS FAILED (%d)\n" : "FX TESTS PASSED\n", fails);
    return fails != 0;
}
