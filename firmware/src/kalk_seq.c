/* SPDX-License-Identifier: GPL-3.0-only */
/* KALK: the sequencer, in the audio interrupt (a tick every 32 frames: 0.73 ms).
 *
 *   main sequence    kick, snare, hat: patterns A-D, LEN 1..16 from a START step, swing, half tempo
 *   pattern chain    2..8 bars of A-D, in any order
 *   second window    TIE, POD, MOD: each loops on its own length; STRETCH fits a lane to the main length (a lane of 3
 *                    then plays 3 even steps across the bar)
 *   remix            one of 127 variations of the main sequence, every 8th / 4th / 2nd / every bar, or held (NOW);
 *                    SHIFT moves where the variation starts
 */

#define Q24 (1u << 24)
static volatile uint32_t tempo_inc;            /* a 16th per frame, Q24 (the main loop writes it) */
static volatile uint32_t frames16 = 5512;      /* frames in a 16th (the effects' sync) */
static volatile uint8_t playing, play_req, stop_req, now_held;
static volatile uint32_t ext_clk;              /* MIDI clocks since START (the main loop counts them) */
static volatile uint8_t ext_lock;              /* follow them: never ahead, catch up when behind */
static volatile uint8_t cur_pat, next_pat = 0xFF;
/* what the screen shows */
static volatile uint8_t ui_pos, ui_spos[NSEC], ui_remix, ui_chain;
static volatile uint32_t ui_bar, gstep;

static struct {
    uint32_t frac, sfrac[NSEC];
    uint8_t pos, spos[NSEC];
    uint8_t pend, spend[NSEC];                  /* a swung step waits for this fraction */
    uint32_t pend_at;
    uint8_t chain_pos, remix_on, started;
} S;

/* ---- the 127 remix variations: a source position for each of 16 positions (-1: a rest) */
static int8_t RMX[128][16];
static void remix_build(void)
{
    static const uint8_t PERM[24][4] = {
        {0,1,2,3},{0,1,3,2},{0,2,1,3},{0,2,3,1},{0,3,1,2},{0,3,2,1},{1,0,2,3},{1,0,3,2},{1,2,0,3},{1,2,3,0},{1,3,0,2},{1,3,2,0},
        {2,0,1,3},{2,0,3,1},{2,1,0,3},{2,1,3,0},{2,3,0,1},{2,3,1,0},{3,0,1,2},{3,0,2,1},{3,1,0,2},{3,1,2,0},{3,2,0,1},{3,2,1,0}};
    uint32_t v, i, seed = 0x6B616C6Bu;
    for (i = 0; i < 16u; i++) RMX[0][i] = (int8_t)i;
    for (v = 1; v < 128u; v++) {
        uint32_t fam = v % 8u, a = v / 8u;      /* a: 0..15 */
        for (i = 0; i < 16u; i++) {
            int32_t s = (int32_t)i;
            switch (fam) {
            case 0: s = (int32_t)((i + a + 1u) & 15u); break;                         /* turned */
            case 1: s = (int32_t)((15u - i + a) & 15u); break;                        /* backwards */
            case 2: s = (int32_t)(((i >> 1) + (a & 1u) * 8u + (a >> 1)) & 15u); break;   /* half time */
            case 3: s = (int32_t)((i * 2u + a) & 15u); break;                         /* double time */
            case 4: {                                                                 /* stutter */
                uint32_t bs = 1u << (a % 3u), base = (a / 3u % 4u) * 4u;
                s = i < 8u ? (int32_t)i : (int32_t)(base + i % bs);
                break;
            }
            case 5: s = (int32_t)(PERM[(a + v / 3u) % 24u][i >> 2] * 4u + (i & 3u)); break;   /* the beats shuffled */
            case 6: s = ((i * 7u + a) % 5u == 0u || (a & 8u && (i & 3u) == 3u)) ? -1 : (int32_t)i; break;   /* holes */
            default:                                                                  /* scattered */
                seed = seed * 1664525u + 1013904223u;
                s = (seed >> 28) & 1u ? (int32_t)i : (int32_t)((seed >> 20) & 15u);
                break;
            }
            RMX[v][i] = (int8_t)s;
        }
    }
}

/* ---- MIDI out of the hits: the main loop sends them */
static volatile uint8_t hitq[32];
static volatile uint32_t hitq_w, hitq_r;
static const uint8_t LANE_NOTE[6] = {36, 38, 42, 0, 39, 0};   /* GM: kick snare closed hat; pod on the clap; (46 open hat) */
static void hit_out(uint32_t note) { if (hitq_w - hitq_r < 32u) { hitq[hitq_w % 32u] = (uint8_t)note; hitq_w++; } }

static int lane_live(uint32_t l)
{
    uint32_t m = P.mute, s = P.solo;
    if (m >> l & 1u) return 0;
    return !s || (s >> l & 1u);
}

/* ---- the triggers from the main loop (pads, MIDI in): lane | accent << 4 | open << 5, semitone */
static volatile uint16_t trigq[16];
static volatile uint32_t trigq_w, trigq_r;
static void trig_push(uint32_t lane, uint32_t flags, int32_t semi)
{
    if (trigq_w - trigq_r < 16u) {
        trigq[trigq_w % 16u] = (uint16_t)(lane | flags << 4 | ((uint32_t)(semi + 64) & 0x7Fu) << 8);
        trigq_w++;
    }
}

static void play_lane(uint32_t l, uint32_t st, int32_t val)
{
    uint32_t acc = (st & ST_ACC) != 0u;
    switch (l) {
    case L_KICK: voice_trigger(V_KICK, acc, 0, 32767); hit_out(36); break;
    case L_SNARE: voice_trigger(V_SNARE, acc, 0, 32767); hit_out(38); break;
    case L_HAT:
        if (st & ST_OPEN) { voice_trigger(V_HATO, acc, 0, 32767); hit_out(46); }
        else { voice_trigger(V_HATC, acc, 0, 32767); hit_out(42); }
        break;
    case L_TIE: voice_trigger(V_TIE, acc, val, 32767); break;
    case L_POD: voice_trigger(V_POD, acc, val, val ? 18000 + (val < 0 ? -val : val) * 1200 : 32767); hit_out(39); break;
    }
}

static void fire_main(void)
{
    uint32_t l, len = P.len ? P.len : 16u, pos = S.pos, pat = cur_pat & 3u;
    int remixing = P.remix && (S.remix_on || now_held);
    for (l = 0; l < NMAIN; l++) {
        uint32_t src = pos, st;
        if (remixing && (P.rlanes >> l & 1u)) {
            int32_t m = RMX[P.remix & 127u][(pos + P.rshift) & 15u];
            if (m < 0) continue;
            src = (uint32_t)m % len;
        }
        st = P.main[pat][l][(P.start + src) & 15u];
        if ((st & ST_ON) && lane_live(l))
            play_lane(l, st, 0);
    }
}

static void fire_sec(uint32_t k)                /* k: 0 tie, 1 pod, 2 mod */
{
    uint32_t st = P.sec[k][S.spos[k]];
    int32_t val = P.val[k][S.spos[k]];
    if (k == 2u) { mod_now = (int8_t)((st & ST_ON) ? clampi(val, 0, 15) : 0); return; }
    if ((st & ST_ON) && lane_live(L_TIE + k))
        play_lane(L_TIE + k, st, val);
}

static void new_bar(void)
{
    uint32_t n, k;
    ui_bar++;
    if (P.chain_len >= 2u) {
        S.chain_pos = (uint8_t)((S.chain_pos + 1u) % P.chain_len);
        cur_pat = P.chain[S.chain_pos] & 3u;
    } else if (next_pat < NPAT) {
        cur_pat = next_pat;
        next_pat = 0xFF;
    }
    ui_chain = S.chain_pos;
    n = P.rfreq == 1u ? 8u : P.rfreq == 2u ? 4u : P.rfreq == 3u ? 2u : 1u;
    S.remix_on = (uint8_t)(P.rfreq && ui_bar % n == n - 1u);
    ui_remix = S.remix_on;
    for (k = 0; k < NSEC; k++)                  /* the stretched lanes start with the bar */
        if (P.stretch >> k & 1u) { S.sfrac[k] = 0; S.spos[k] = 0; S.spend[k] = 1; }
}

static void seq_start(void)
{
    uint32_t k;
    S.frac = 0; S.pos = 0; S.chain_pos = 0; S.pend = 1; S.pend_at = 0;
    for (k = 0; k < NSEC; k++) { S.sfrac[k] = 0; S.spos[k] = 0; S.spend[k] = 1; }
    if (P.chain_len >= 2u) cur_pat = P.chain[0] & 3u;
    else if (next_pat < NPAT) { cur_pat = next_pat; next_pat = 0xFF; }
    ui_bar = 0;
    gstep = 0;
    S.remix_on = (uint8_t)(P.rfreq == 4u);
    playing = 1;
}

/* every 32 frames */
static void seq_tick(uint32_t frames)
{
    uint32_t k, len = P.len ? P.len : 16u, inc;
    while (trigq_r != trigq_w) {                /* the pads and MIDI first: they play now */
        uint32_t t = trigq[trigq_r % 16u];
        trigq_r++;
        play_lane(t & 15u, t >> 4 & 15u, (int32_t)(t >> 8 & 0x7Fu) - 64);
    }
    if (play_req) { play_req = 0; seq_start(); }
    if (stop_req) { stop_req = 0; playing = 0; }
    if (!playing) return;
    inc = tempo_inc * frames;
    if (P.half) inc >>= 1;
    if (ext_lock) {                             /* 6 clocks a 16th (12 at half tempo) */
        uint32_t cps = P.half ? 12u : 6u, at = gstep * cps + (uint32_t)(((uint64_t)S.frac * cps) >> 24);
        uint32_t clk = ext_clk;
        if (at >= clk + 1u) inc = 0;            /* ahead: wait for the clock */
        else if (at < clk) inc += inc * (clk - at > 4u ? 4u : clk - at) / 2u;   /* behind: catch up, faster the further */
    }

    /* the swung or due steps of this tick */
    if (S.pend && S.frac >= S.pend_at) {
        S.pend = 0;
        for (k = 0; k < NSEC; k++) {            /* mod, pod, tie: the mod lane first, the hits read it */
            uint32_t kk = 2u - k;
            if (!(P.stretch >> kk & 1u) && S.spend[kk]) { S.spend[kk] = 0; fire_sec(kk); }
        }
        fire_main();
    }
    for (k = 0; k < NSEC; k++) {
        uint32_t kk = 2u - k;                   /* mod, pod, tie */
        if ((P.stretch >> kk & 1u) && S.spend[kk]) { S.spend[kk] = 0; fire_sec(kk); }
    }

    S.frac += inc;
    for (k = 0; k < NSEC; k++)
        if (P.stretch >> k & 1u) {
            uint32_t sl = P.slen[k] ? P.slen[k] : 16u;
            S.sfrac[k] += inc / len * sl;
            if (S.sfrac[k] >= Q24) {
                S.sfrac[k] -= Q24;
                if (S.spos[k] + 1u < sl) { S.spos[k]++; S.spend[k] = 1; }   /* (the bar restarts it) */
            }
        }
    if (S.frac >= Q24) {
        S.frac -= Q24;
        gstep++;
        S.pos = (uint8_t)((S.pos + 1u) % len);
        if (S.pos == 0u) new_bar();
        for (k = 0; k < NSEC; k++)
            if (!(P.stretch >> k & 1u)) {
                uint32_t sl = P.slen[k] ? P.slen[k] : 16u;
                S.spos[k] = (uint8_t)((S.spos[k] + 1u) % sl);
                S.spend[k] = 1;
            }
        S.pend = 1;
        S.pend_at = (gstep & 1u) ? (uint32_t)P.swing * (Q24 / 200u) : 0u;   /* 100 %: half a step late */
    }
    ui_pos = S.pos;
    for (k = 0; k < NSEC; k++) ui_spos[k] = S.spos[k];
}
