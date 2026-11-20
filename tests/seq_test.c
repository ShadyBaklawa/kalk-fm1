/* SPDX-License-Identifier: GPL-3.0-only */
/* KALK's sequencer on its own: the RMX LFO (RMX2). A single kick on step 1: at depth 0 it plays where 0.2 played it;
 * at depth D (any shape, any rate) every kick lands within D steps of step 1; the LANES mask keeps the others still.
 *   cc -O2 -Ifirmware/src -o build/host/seq_test tests/seq_test.c -lm && build/host/seq_test */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kalk_model.h"
static project_t P;
#include "kalk_dsp.h"
#include "kalk_voice.c"
#include "kalk_seq.c"

static int fails;
static void check(int ok, const char *what)
{
    printf("%-4s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) fails++;
}

/* play `bars` bars; hits[lane][n] = the step (counted from the start) of each hit; returns the hits per lane */
static uint32_t hits[3][1024], nh[3];
static void play(uint32_t bars)
{
    uint32_t t, last_q = 0;
    memset(nh, 0, sizeof nh);
    hitq_r = hitq_w = 0;
    tempo_inc = (uint32_t)((double)Q24 / 5512.5);   /* 120 BPM */
    play_req = 1;
    for (t = 0; t < bars * 16u * 5513u / 32u + 4u; t++) {
        seq_tick(32);
        while (hitq_r != hitq_w) {
            uint32_t note = hitq[hitq_r % 32u], l = note == 36u ? 0u : note == 38u ? 1u : 2u;
            hitq_r++;
            if (nh[l] < 1024u) hits[l][nh[l]++] = gstep;
        }
        (void)last_q;
    }
    stop_req = 1;
    seq_tick(32);
}

int main(void)
{
    uint32_t i, d, sh, rt;
    memset(&P, 0, sizeof P);
    memcpy(P.vp, VP_DEFAULT, sizeof VP_DEFAULT);
    P.len = 16; P.rlanes = 7; P.r2rate = 2;
    P.main[0][0][1] = ST_ON;                    /* a kick on step 2 (index 1) */
    P.main[0][1][4] = P.main[0][1][12] = ST_ON; /* snares on 5 and 13 */
    remix_build();

    play(4);
    {
        int same = nh[0] == 4u;
        for (i = 0; i < nh[0]; i++) same &= hits[0][i] == 1u + 16u * i;
        check(same, "RMX LFO depth 0: the kick plays where it did (step 2 of each bar)");
    }
    {
        int bad = 0, moved = 0;
        for (d = 1; d <= 15u; d += 7u)
            for (sh = 0; sh < NR2; sh++)
                for (rt = 0; rt < 6u; rt += 2u) {
                    P.r2depth = (uint8_t)d; P.r2shape = (uint8_t)sh; P.r2rate = (uint8_t)rt;
                    play(8);
                    for (i = 0; i < nh[0]; i++) {
                        int32_t p = (int32_t)(hits[0][i] % 16u) - 1, dist = abs(p) < 8 ? abs(p) : 16 - abs(p);
                        if (dist > (int32_t)d) { bad++; printf("     depth %u shape %u rate %u: a kick at step %u\n", d, sh, rt, hits[0][i] % 16u + 1u); }
                        if (dist) moved = 1;
                    }
                }
        check(!bad, "RMX LFO: every kick within DEPTH steps of where it was (every shape, depth, rate)");
        check(moved, "RMX LFO: it does move them");
    }
    {   /* LANES without the snare: the snare stays on 5 and 13 */
        int still = 1;
        P.r2depth = 6; P.r2shape = R2_RANDOM; P.rlanes = 1;   /* the kick only */
        play(8);
        for (i = 0; i < nh[1]; i++) still &= (hits[1][i] % 16u == 4u || hits[1][i] % 16u == 12u);
        check(still && nh[1] == 16u, "RMX LFO: LANES leaves the other lanes where they are");
    }
    printf(fails ? "SEQ TESTS FAILED (%d)\n" : "SEQ TESTS PASSED\n", fails);
    return fails != 0;
}
