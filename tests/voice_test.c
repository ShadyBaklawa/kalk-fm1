/* SPDX-License-Identifier: GPL-3.0-only */
/* KALK's voices on their own: every voice, every WAV variant, accented, with the demo project's sound.
 * Prints the peak, the RMS of the first 100 ms and the time to -40 dB; writes build/host/voices.wav.
 * Fails when a variant is silent, clips, or never ends.
 *   cc -O2 -Ifirmware/src -o build/host/voice_test tests/voice_test.c && build/host/voice_test */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "kalk_model.h"
static project_t P;
#include "kalk_dsp.h"
#include "kalk_voice.c"

#define FS 44100
#define N (FS * 2)
static int32_t buf[2 * N];

int main(void)
{
    static const char *const NAME[NV_RUN] = {"KICK", "SNARE", "HAT C", "HAT O", "TIE", "POD"};
    FILE *w = fopen(getenv("OUT") ? getenv("OUT") : "build/host/voices.wav", "wb");
    uint32_t vi, var, fails = 0, total = 0;
    memcpy(P.vp, VP_DEFAULT, sizeof VP_DEFAULT);
    memcpy(P.vx, VX_DEFAULT, sizeof VX_DEFAULT);
    if (getenv("VP")) {                         /* VP=voice,param,value: try a setting */
        int a, b, c;
        if (sscanf(getenv("VP"), "%d,%d,%d", &a, &b, &c) == 3) P.vp[a][b] = (uint8_t)c;
    }
    if (w) fseek(w, 44, SEEK_SET);
    for (vi = 0; vi < NV_RUN; vi++) {
        double pk_sum = 0;
        printf("%-6s", NAME[vi]);
        for (var = 0; var < 24u; var++) {
            uint32_t i, len = FS / 2, end = 0;
            double pk = 0, rms = 0;
            P.vp[V_PARAMS[vi]][VP_WAV] = (uint8_t)var;
            memset(buf, 0, sizeof buf);
            memset(&V[vi], 0, sizeof V[vi]);
            voice_trigger(vi, 1, 0, 32767);
            for (i = 0; i < N; i += 32u) voices_render(buf + 2u * i, 32u);
            for (i = 0; i < N; i++) {
                double s = fabs((double)buf[2u * i] + buf[2u * i + 1u]) / 65536.0;
                if (s > pk) pk = s;
                if (i < FS / 10) rms += s * s;
            }
            for (i = 0; i < N; i++) if (fabs((double)buf[2u * i] + buf[2u * i + 1u]) / 65536.0 > pk * 0.01) end = i;
            rms = sqrt(rms / (FS / 10));
            printf("%s %3.0f/%2.0f/%4u", var == 16u ? "\n      |" : "", pk * 100, rms * 100, end * 1000u / FS);
            pk_sum += pk;
            if (pk < 0.02 || pk > 0.99 || V[vi].on) fails++;
            total++;
            if (w)
                for (i = 0; i < len; i++) {
                    int16_t s[2] = {(int16_t)clip15(buf[2u * i]), (int16_t)clip15(buf[2u * i + 1u])};
                    fwrite(s, 2, 2, w);
                }
        }
        printf("   mean peak %.2f\n", pk_sum / 24);
    }
    if (w) {
        uint32_t bytes = (uint32_t)ftell(w) - 44u, v[] = {16, 0x00020001u, FS, FS * 4u, 0x00100004u};
        fseek(w, 0, SEEK_SET);
        fwrite("RIFF", 1, 4, w); bytes += 36; fwrite(&bytes, 4, 1, w); bytes -= 36;
        fwrite("WAVEfmt ", 1, 8, w); fwrite(v, 4, 5, w);
        fwrite("data", 1, 4, w); fwrite(&bytes, 4, 1, w);
        fclose(w);
    }
    printf("(peak %% / rms %% of the first 100 ms / ms to -40 dB)\n%u of %u variants failed (silent, clipping or endless)\n", fails, total);

    /* GRAIN / SPEED (after 0.3) on the kick and snare samples: at rest the plain player; SPEED stretches the sample
     * without changing its pitch; GRAIN moves the grains' starts; bounded */
    {
        static int32_t ref[2 * N];
        uint32_t i, sv;
        for (sv = 0; sv < 2u; sv++) {
            uint32_t ends[3], kk;
            double pk_g = 0, diff = 0, zc[3];
            static const uint8_t SPD[3] = {32, 64, 127};
            P.vp[sv][VP_WAV] = 20; P.vp[sv][VP_DECAY] = 127; P.vp[sv][VP_X4] = 127;   /* a long hold: the length shows */
            for (kk = 0; kk < 3u; kk++) {
                double lastv = 0; uint32_t z = 0;
                P.vx[sv][VX_SPEED] = SPD[kk]; P.vx[sv][VX_GRAIN] = 0;
                memset(buf, 0, sizeof buf); memset(&V[sv], 0, sizeof V[sv]);
                voice_trigger(sv, 1, 0, 32767);
                if (kk == 1u && V[sv].grn) fails++;   /* (at rest: not the grain player) */
                for (i = 0; i < N; i += 32u) voices_render(buf + 2u * i, 32u);
                ends[kk] = 0;
                for (i = 0; i < N; i++) {
                    if (buf[2u * i]) ends[kk] = i;
                    if (i < 2000u) { double x = buf[2u * i]; if ((x > 0) != (lastv > 0)) z++; lastv = x; }
                }
                zc[kk] = z;
                if (kk == 1u) memcpy(ref, buf, sizeof buf);
            }
            printf("%s sample SPEED 50 / 100 / 198 %%: lasts %u / %u / %u ms; zero crossings in 45 ms %0.f / %0.f / %0.f\n",
                   sv ? "SNARE" : "KICK", ends[0] * 1000u / FS, ends[1] * 1000u / FS, ends[2] * 1000u / FS, zc[0], zc[1], zc[2]);
            if (!(ends[0] > ends[1] * 17 / 10 && ends[2] < ends[1] * 7 / 10)) { printf("FAIL SPEED: the length\n"); fails++; }
            if (fabs(zc[0] - zc[1]) > zc[1] * 0.35 || fabs(zc[2] - zc[1]) > zc[1] * 0.35) { printf("FAIL SPEED: the pitch moved\n"); fails++; }
            P.vx[sv][VX_SPEED] = 64; P.vx[sv][VX_GRAIN] = 127;
            memset(buf, 0, sizeof buf); memset(&V[sv], 0, sizeof V[sv]);
            voice_trigger(sv, 1, 0, 32767);
            for (i = 0; i < N; i += 32u) voices_render(buf + 2u * i, 32u);
            for (i = 0; i < N; i++) {
                double a = fabs(buf[2u * i] / 32768.0);
                if (a > pk_g) pk_g = a;
                diff += fabs((double)buf[2u * i] - ref[2u * i]);
            }
            printf("%s GRAIN 127: peak %.2f, differs from GRAIN 0 by %.0f\n", sv ? "SNARE" : "KICK", pk_g, diff);
            if (pk_g > 0.99 || pk_g < 0.02 || diff < 1e5) { printf("FAIL GRAIN\n"); fails++; }
            memcpy(P.vp, VP_DEFAULT, sizeof VP_DEFAULT); memcpy(P.vx, VX_DEFAULT, sizeof VX_DEFAULT);
        }
    }
    return fails ? 1 : 0;
}
