/* SPDX-License-Identifier: GPL-3.0-only */
/* KALK: a beat composer for the M-VAVE FM-1, after the workflow of Native Instruments' Limelite (Reaktor, 2003).
 * An independent work: no code, samples or artwork of the original; every sound is synthesised.
 *
 *   KEYS   white: the 16 steps of the selected lane (tap: on / off; hold: accent; SEL + tap on HAT: open hat)
 *          black: F#3 KICK  G#3 SNARE  A#3 HAT  C#4 TIE  D#4 POD  F#4 MOD  (select; stopped: play it)
 *                 G#4 A  A#4 B  C#5 C  D#5 D  (the pattern: queued to the bar; hold one and press another: copy)
 *                 F#5 NOW (the remix, while held)
 *   PLAY   start / stop          REC   XFX page: record the pad's motion; holding a lane key: clear that lane
 *   HOME / EDIT  the selected lane's sound    FX  effects    SEQ  sequence    ARP  remix    GLO  global
 *   SELECT the page   K1..K4 the four values   ALGORITHM the tempo   PRESETS the project slot
 *   SAVE   save to the slot (stopped)   SEL + SAVE   load it   ENV mute the lane   LFO solo the lane
 *   OCT- / OCT+  the previous / next lane
 *   on TIE, POD, MOD: hold a step and turn K1 for its value
 *
 * Built on Dinghy (zvenson), which is built on Felucca (Leo Kuroshita) and SLOOP (isod89): platform.c and below. */
#define APP_NAME "KALK"
#define STORE_MAGIC 0x4B4C414Bu                 /* "KALK" */
#define CV_MAX (240u * 112u)                    /* the grid in one canvas */
static void app_idle(void);
#define LCD_IDLE app_idle
#include "platform.c"
#include "kalk_model.h"
static project_t P;
#include "kalk_dsp.h"
#include "kalk_voice.c"
#include "kalk_seq.c"
#include "kalk_fx.c"

#define BLK 32u
static volatile uint32_t audio_us;             /* (the device: the render's time) */
__attribute__((noinline)) static void app_audio(int32_t *out, uint32_t n)
{
    uint32_t off;
#ifndef DINGHY_HOST
    uint32_t t0 = fm1_ticks(), us;
#endif
    for (off = 0; off + BLK <= n; off += BLK) {
        seq_tick(BLK);
        voices_render(out + 2u * off, BLK);
        fx_render(out + 2u * off, BLK);
    }
#ifndef DINGHY_HOST
    us = (fm1_ticks() - t0) / FM1_TICKS_PER_US;   /* the GLOBAL page shows the worst of each second */
    if (us > audio_us) audio_us = us;
#endif
}

/* ======================================================================== the project */
static void proj_default(project_t *p)
{
    static const uint8_t FX0[NFX] = {
        XF_THRU, 64, 32, 1,   3, 4, 50, 0,   50, 90, 127, 0,   127, 0, 64, 0,   64, 64, 64, 0,   64, 12, 0, 40,
        70, 90, 60, 0,   40, 30, 110, 0,    /* the reverb: there, but its sends at 0 (0.2 projects sound the same) */
        114, 0, 0, 0,   4, 0, 0, 0};        /* the reson delay: filter open, no LFO depth (the 0.2 delay exactly) */
    /* the demo: A house, B broken, C electro, D half time. x on, X accent, o open hat, O open accented */
    static const char *const PAT[NPAT][NMAIN] = {
        {"X...x...X...x...", "....X.......X...", "x.o.x.oxx.o.x.oX"},
        {"X.........x..x..", "....X...x...X..x", "x.x.X.x.x.x.X.x."},
        {"X.....x...X.....", "....X.......X...", "xxxXxxxXxxxXxxxO"},
        {"X..........x....", "........X.......", "x.x.x.xXx.x.x.o."},
    };
    static const int8_t TIE0[NSTEP] = {0, 99, 99, 0, 99, 99, 12, 99, 0, 99, 3, 99, 7, 99, 12, 99};   /* 99: rest */
    static const int8_t POD0[5] = {5, 99, -3, 99, 99};
    static const int8_t MOD0[12] = {8, 99, 99, 15, 99, 99, 4, 99, 99, 12, 99, 99};
    uint32_t a, l, s;
    for (l = 0; l < sizeof *p; l++) ((uint8_t *)p)[l] = 0;
    for (a = 0; a < NPAT; a++)
        for (l = 0; l < NMAIN; l++)
            for (s = 0; s < NSTEP; s++) {
                char c = PAT[a][l][s];
                p->main[a][l][s] = (uint8_t)(c == '.' ? 0u : ST_ON | (c == 'X' || c == 'O' ? ST_ACC : 0u) |
                                             (c == 'o' || c == 'O' ? ST_OPEN : 0u));
            }
    for (s = 0; s < NSTEP; s++) {
        if (TIE0[s] != 99) { p->sec[0][s] = ST_ON | (s == 6u ? ST_ACC : 0u); p->val[0][s] = TIE0[s]; }
        if (s < 5u && POD0[s] != 99) { p->sec[1][s] = ST_ON; p->val[1][s] = POD0[s]; }
        if (s < 12u && MOD0[s] != 99) { p->sec[2][s] = ST_ON; p->val[2][s] = MOD0[s]; }
    }
    p->len = 16; p->start = 0;
    p->slen[0] = 16; p->slen[1] = 5; p->slen[2] = 12;
    p->stretch = 0; p->swing = 12; p->half = 0;
    p->chain_len = 0;
    for (s = 0; s < NCHAIN; s++) p->chain[s] = (uint8_t)(s & 3u);
    p->remix = 37; p->rfreq = 2; p->rshift = 0; p->rlanes = 7;
    p->bpm = 124;
    for (a = 0; a < NVOICE; a++) for (l = 0; l < NVP; l++) p->vp[a][l] = VP_DEFAULT[a][l];
    for (l = 0; l < NFX; l++) p->fx[l] = FX0[l];
    for (s = 0; s < NXY; s++) p->xy[s][0] = p->xy[s][1] = 255;
    p->magic = PROJ_MAGIC;
}

static struct { uint8_t slot, ch, out, sync; uint32_t magic; } set;
#define SET_MAGIC 0x4B53u

/* ======================================================================== the parameters on the screen */
enum { K_VP, K_FX, K_SQ, K_GL, K_SL };          /* voice, effects, sequence, global, the selected second-window lane */
enum { SQ_LEN, SQ_START, SQ_SWING, SQ_HALF, SQ_TLEN, SQ_PLEN, SQ_MLEN, SQ_STRETCH, SQ_CHAIN, SQ_SLOT, SQ_CPAT, SQ_BPM,
       SQ_RMX, SQ_RFREQ, SQ_RSHIFT, SQ_RLANES };
enum { GL_BPM, GL_CH, GL_OUT, GL_SYNC };
enum { SL_LEN, SL_STRETCH };
enum { F_WAV, F_NUM, F_SEMI, F_MS, F_HZ, F_LIST, F_PAN, F_BIP, F_PLUS1, F_PCT, F_TICKS, F_MASK3, F_CHAIN, F_PAT, F_OMNI, F_LEN, F_RT };
typedef struct { uint8_t kind, idx; } pref_t;

static const char *const LANE_LONG[NLANE] = {"KICK", "SNARE", "HAT", "TIE", "POD", "MOD"};
static const uint16_t LANE_COL[NLANE] = {RGB(255, 120, 60), RGB(250, 210, 70), RGB(120, 230, 120),
                                         RGB(90, 200, 255), RGB(220, 120, 255), RGB(200, 200, 200)};
#define C_LIME RGB(190, 240, 60)
#define C_GREY RGB(110, 110, 110)
#define C_DARK RGB(34, 34, 34)

static const char *const VP_NAMES[NVOICE][NVP] = {
    {"TUNE", "DECAY", "WAV", "GAIN", "CUT", "RES", "FILT", "PAN", "SAT", "CLICK", "SWEEP", "HOLD", "MOD>", "MOD", "ACC", "START"},
    {"TUNE", "DECAY", "WAV", "GAIN", "CUT", "RES", "FILT", "PAN", "SAT", "SNAPPY", "FLAM", "HOLD", "MOD>", "MOD", "ACC", "START"},
    {"TUNE", "CL DEC", "WAV", "GAIN", "CUT", "RES", "FILT", "PAN", "OP DEC", "LINK", "NOISE", "HOLD", "MOD>", "MOD", "ACC", "START"},
    {"TUNE", "DECAY", "WAV", "GAIN", "CUT", "RES", "FILT", "PAN", "FM", "RATIO", "PORT", "ATTACK", "MOD>", "MOD", "ACC", "NOISE"},
    {"TUNE", "DECAY", "WAV", "GAIN", "CUT", "RES", "FILT", "PAN", "ZAP", "ZAPDEC", "NOISE", "HOLD", "MOD>", "MOD", "ACC", "START"},
};
static const char *const FX_NAMES[NFX] = {
    "XFX", "X", "Y", "MOTION", "DLY L", "DLY R", "FDBK", "SEND", "DAMP", "WIDTH", "XMIX", "MOD>FX",
    "CUT", "RES", "MOD", "TRK", "LO", "MID", "HI", "AMP", "GAIN", "PITCH", "DIRT", "NORM",
    "TIME", "SIZE", "DAMP", "SEND", "LOW", "MOD", "WIDTH", "DLY>RV",
    "FREQ", "RES", "FILT", "LO CUT", "LFO", "DMOD", "FM", "INVERT"};
static const char *const SQ_NAMES[16] = {"LEN", "START", "SWING", "HALF", "TIE LN", "POD LN", "MOD LN", "STRTCH",
                                         "CHAIN", "SLOT", "PATT", "BPM", "REMIX", "EVERY", "SHIFT", "LANES"};
static const char *const GL_NAMES[4] = {"BPM", "CHAN", "OUT", "SYNC"};

static const char *const L_FILT[] = {"LP", "BP", "HP"};
static const char *const L_MODD[] = {"OFF", "WAV", "PITCH", "DECAY", "CUT"};
static const char *const L_XFX[NXF] = {"THRU", "CRUSH", "BPF", "RING", "COMB", "REPEAT", "LPF", "HPF", "FOLD", "GATE", "PAN", "FLANGE"};
static const char *const L_MOTION[] = {"OFF", "PLAY", "SMOOTH"};
static const char *const L_MODFX[] = {"OFF", "GATE", "MIX", "X"};
static const char *const L_OFFON[] = {"OFF", "ON"};
static const char *const L_DLFO[] = {"1/16", "1/8", "1/4", "1/2", "1 BAR", "2 BAR", "4 BAR"};
static const char *const L_EVERY[] = {"OFF", "8 BAR", "4 BAR", "2 BAR", "1 BAR"};
static const char *const L_SYNC[] = {"INT", "MIDI"};

static uint8_t sel_lane, edit_pat, section, page[5], chain_slot, xy_rec;
enum { SEC_VOICE, SEC_FX, SEC_SEQ, SEC_RMX, SEC_GLO };
static const char *const SEC_NAME[5] = {"", "FX", "SEQ", "REMIX", "GLOBAL"};

/* pages: four references each */
static const pref_t PG_VOICE[4][4] = {
    {{K_VP, VP_TUNE}, {K_VP, VP_DECAY}, {K_VP, VP_WAV}, {K_VP, VP_GAIN}},
    {{K_VP, VP_CUT}, {K_VP, VP_RES}, {K_VP, VP_FTYPE}, {K_VP, VP_PAN}},
    {{K_VP, VP_X1}, {K_VP, VP_X2}, {K_VP, VP_X3}, {K_VP, VP_X4}},
    {{K_VP, VP_MODDST}, {K_VP, VP_MODAMT}, {K_VP, VP_ACC}, {K_VP, VP_START}},
};
static const char *const PG_VOICE_T[5] = {"SOUND", "FILTER", "SHAPE", "MOD", "LANE"};
static const pref_t PG_LANE[4] = {{K_SL, SL_LEN}, {K_SL, SL_STRETCH}, {K_SQ, SQ_SWING}, {K_SQ, SQ_LEN}};
static const pref_t PG_MODL[4] = {{K_SL, SL_LEN}, {K_SL, SL_STRETCH}, {K_FX, FX_MODFX}, {K_FX, FX_FMOD}};
static const pref_t PG_FX[10][4] = {
    {{K_FX, FX_XTYPE}, {K_FX, FX_X}, {K_FX, FX_Y}, {K_FX, FX_XAUTO}},
    {{K_FX, FX_DL}, {K_FX, FX_DR}, {K_FX, FX_DFB}, {K_FX, FX_DSEND}},
    {{K_FX, FX_DDAMP}, {K_FX, FX_DWIDTH}, {K_FX, FX_XMIX}, {K_FX, FX_MODFX}},
    {{K_FX, FX_DFREQ}, {K_FX, FX_DRES}, {K_FX, FX_DFTYPE}, {K_FX, FX_DLO}},
    {{K_FX, FX_DLFO}, {K_FX, FX_DDMOD}, {K_FX, FX_DFM}, {K_FX, FX_DINV}},
    {{K_FX, FX_FCUT}, {K_FX, FX_FRES}, {K_FX, FX_FMOD}, {K_FX, FX_FTRK}},
    {{K_FX, FX_EQLO}, {K_FX, FX_EQMID}, {K_FX, FX_EQHI}, {K_FX, FX_EQAMP}},
    {{K_FX, FX_GAIN}, {K_FX, FX_PITCH}, {K_FX, FX_DIRT}, {K_FX, FX_NORM}},
    {{K_FX, FX_RTIME}, {K_FX, FX_RSIZE}, {K_FX, FX_RDAMP}, {K_FX, FX_RSEND}},
    {{K_FX, FX_RLOW}, {K_FX, FX_RMOD}, {K_FX, FX_RWIDTH}, {K_FX, FX_RDLY}},
};
static const char *const PG_FX_T[10] = {"XFX", "DELAY", "DELAY 2", "RESON", "TAPE LFO", "FILTER", "EQ", "OUT", "REVERB",
                                         "REVERB 2"};
static const pref_t PG_SEQ[3][4] = {
    {{K_SQ, SQ_LEN}, {K_SQ, SQ_START}, {K_SQ, SQ_SWING}, {K_SQ, SQ_HALF}},
    {{K_SQ, SQ_TLEN}, {K_SQ, SQ_PLEN}, {K_SQ, SQ_MLEN}, {K_SQ, SQ_STRETCH}},
    {{K_SQ, SQ_CHAIN}, {K_SQ, SQ_SLOT}, {K_SQ, SQ_CPAT}, {K_SQ, SQ_BPM}},
};
static const char *const PG_SEQ_T[3] = {"MAIN", "LANES", "CHAIN"};
static const pref_t PG_RMX[4] = {{K_SQ, SQ_RMX}, {K_SQ, SQ_RFREQ}, {K_SQ, SQ_RSHIFT}, {K_SQ, SQ_RLANES}};
static const pref_t PG_GLO[4] = {{K_GL, GL_BPM}, {K_GL, GL_CH}, {K_GL, GL_OUT}, {K_GL, GL_SYNC}};

static uint32_t npages(void)
{
    switch (section) {
    case SEC_VOICE: return sel_lane == L_MOD ? 1u : sel_lane >= L_TIE ? 5u : 4u;
    case SEC_FX: return 10u;
    case SEC_SEQ: return 3u;
    default: return 1u;
    }
}
static const pref_t *page_refs(void)
{
    uint32_t pg = page[section] % npages();
    switch (section) {
    case SEC_VOICE:
        if (sel_lane == L_MOD) return PG_MODL;
        return pg == 4u ? PG_LANE : PG_VOICE[pg];
    case SEC_FX: return PG_FX[pg];
    case SEC_SEQ: return PG_SEQ[pg];
    case SEC_RMX: return PG_RMX;
    default: return PG_GLO;
    }
}
static const char *page_title(void)
{
    uint32_t pg = page[section] % npages();
    switch (section) {
    case SEC_VOICE: return sel_lane == L_MOD ? "LANE" : PG_VOICE_T[pg];
    case SEC_FX: return PG_FX_T[pg];
    case SEC_SEQ: return PG_SEQ_T[pg];
    default: return "";
    }
}
static uint32_t voice_of_lane(void) { return sel_lane < NVOICE ? sel_lane : 0u; }

/* the value behind a reference: its byte, its range, its format */
typedef struct { uint8_t *p; int32_t min, max; uint8_t fmt; const char *const *list; const char *name; uint8_t bit; } pval_t;
static pval_t pval(pref_t r)
{
    pval_t v = {0, 0, 127, F_NUM, 0, "", 0};
    uint32_t sec = sel_lane >= L_TIE ? sel_lane - L_TIE : 0u;
    switch (r.kind) {
    case K_VP: {
        uint32_t vi = voice_of_lane();
        v.p = &P.vp[vi][r.idx];
        v.name = VP_NAMES[vi][r.idx];
        switch (r.idx) {
        case VP_TUNE: v.max = 48; v.fmt = F_SEMI; break;
        case VP_DECAY: v.fmt = F_MS; break;
        case VP_WAV: v.max = 23; v.fmt = F_WAV; break;
        case VP_CUT: v.fmt = F_HZ; break;
        case VP_FTYPE: v.max = 2; v.fmt = F_LIST; v.list = L_FILT; break;
        case VP_PAN: v.fmt = F_PAN; break;
        case VP_MODDST: v.max = 4; v.fmt = F_LIST; v.list = L_MODD; break;
        case VP_MODAMT: v.fmt = F_BIP; break;
        case VP_X1: if (vi == 2u) v.fmt = F_MS; break;
        case VP_X2: if (vi == 2u) { v.max = 1; v.fmt = F_LIST; v.list = L_OFFON; } break;
        }
        break;
    }
    case K_FX:
        v.p = &P.fx[r.idx];
        v.name = FX_NAMES[r.idx];
        switch (r.idx) {
        case FX_XTYPE: v.max = NXF - 1; v.fmt = F_LIST; v.list = L_XFX; break;
        case FX_XAUTO: v.max = 2; v.fmt = F_LIST; v.list = L_MOTION; break;
        case FX_DL: case FX_DR: v.max = 16; v.fmt = F_TICKS; break;   /* 0: CHORUS (a few ms) */
        case FX_MODFX: v.max = 3; v.fmt = F_LIST; v.list = L_MODFX; break;
        case FX_FCUT: v.fmt = F_HZ; break;
        case FX_FMOD: case FX_EQLO: case FX_EQMID: case FX_EQHI: v.fmt = F_BIP; break;
        case FX_PITCH: v.max = 24; v.fmt = F_SEMI; break;
        case FX_RTIME: v.fmt = F_RT; break;
        case FX_DFREQ: v.max = 114; v.fmt = F_HZ; break;   /* (at half rate: up to ~9 kHz) */
        case FX_DFTYPE: v.max = 2; v.fmt = F_LIST; v.list = L_FILT; break;
        case FX_DLFO: v.max = 6; v.fmt = F_LIST; v.list = L_DLFO; break;
        case FX_DINV: v.max = 1; v.fmt = F_LIST; v.list = L_OFFON; break;
        }
        break;
    case K_SQ:
        v.name = SQ_NAMES[r.idx];
        switch (r.idx) {
        case SQ_LEN: v.p = &P.len; v.min = 1; v.max = 16; break;
        case SQ_START: v.p = &P.start; v.max = 15; v.fmt = F_PLUS1; break;
        case SQ_SWING: v.p = &P.swing; v.max = 100; v.fmt = F_PCT; break;
        case SQ_HALF: v.p = &P.half; v.max = 1; v.fmt = F_LIST; v.list = L_OFFON; break;
        case SQ_TLEN: case SQ_PLEN: case SQ_MLEN: v.p = &P.slen[r.idx - SQ_TLEN]; v.min = 1; v.max = 16; break;
        case SQ_STRETCH: v.p = &P.stretch; v.max = 7; v.fmt = F_MASK3; break;
        case SQ_CHAIN: v.p = &P.chain_len; v.max = 8; v.fmt = F_CHAIN; break;
        case SQ_SLOT: v.p = &chain_slot; v.max = 7; v.fmt = F_PLUS1; break;
        case SQ_CPAT: v.p = &P.chain[chain_slot & 7u]; v.max = 3; v.fmt = F_PAT; break;
        case SQ_BPM: v.p = &P.bpm; v.min = 40; v.max = 250; break;
        case SQ_RMX: v.p = &P.remix; v.fmt = F_NUM; v.name = "REMIX"; break;
        case SQ_RFREQ: v.p = &P.rfreq; v.max = 4; v.fmt = F_LIST; v.list = L_EVERY; break;
        case SQ_RSHIFT: v.p = &P.rshift; v.max = 15; break;
        case SQ_RLANES: v.p = &P.rlanes; v.min = 1; v.max = 7; v.fmt = F_MASK3; break;
        }
        break;
    case K_GL:
        v.name = GL_NAMES[r.idx];
        switch (r.idx) {
        case GL_BPM: v.p = &P.bpm; v.min = 40; v.max = 250; break;
        case GL_CH: v.p = &set.ch; v.max = 16; v.fmt = F_OMNI; break;
        case GL_OUT: v.p = &set.out; v.max = 1; v.fmt = F_LIST; v.list = L_OFFON; break;
        case GL_SYNC: v.p = &set.sync; v.max = 1; v.fmt = F_LIST; v.list = L_SYNC; break;
        }
        break;
    case K_SL:
        if (r.idx == SL_LEN) { v.p = &P.slen[sec]; v.min = 1; v.max = 16; v.name = "LEN"; }
        else { v.p = &P.stretch; v.max = 1; v.fmt = F_LIST; v.list = L_OFFON; v.name = "STRTCH"; v.bit = (uint8_t)(1u << sec); }
        break;
    }
    return v;
}
static int32_t pget(const pval_t *v) { return v->bit ? (*v->p & v->bit ? 1 : 0) : *v->p; }
static void pset(const pval_t *v, int32_t x)
{
    x = clampi(x, v->min, v->max);
    if (v->fmt == F_CHAIN && x == 1) x = *v->p >= 2u ? 0 : 2;   /* the chain: OFF or 2..8 */
    if (v->bit) *v->p = (uint8_t)(x ? *v->p | v->bit : *v->p & ~v->bit);
    else *v->p = (uint8_t)x;
}

static char *put_s(char *d, const char *s) { while (*s) *d++ = *s++; *d = 0; return d; }
static char *put_u(char *d, uint32_t u)
{
    char t[12];
    int i = 0;
    do { t[i++] = (char)('0' + u % 10u); u /= 10u; } while (u);
    while (i) *d++ = t[--i];
    *d = 0;
    return d;
}
static char *put_i(char *d, int32_t x) { if (x < 0) { *d++ = '-'; x = -x; } else if (x > 0) *d++ = '+'; return put_u(d, (uint32_t)x); }

static void fmt_val(const pval_t *v, char *d)
{
    int32_t x = pget(v);
    switch (v->fmt) {
    case F_SEMI: put_i(d, x - (v->max / 2)); break;
    case F_MS: {
        uint32_t ms = DECAY_MS[x & 127];
        if (ms >= 1000u) { d = put_u(d, ms / 1000u); *d++ = '.'; d = put_u(d, ms / 100u % 10u); put_s(d, "s"); }
        else { d = put_u(d, ms); put_s(d, "ms"); }
        break;
    }
    case F_RT: {                                /* the reverb's RT60 */
        uint32_t ms = RV_MS[x & 127];
        if (ms >= 1000u) { d = put_u(d, ms / 1000u); *d++ = '.'; d = put_u(d, ms / 100u % 10u); put_s(d, "s"); }
        else { d = put_u(d, ms); put_s(d, "ms"); }
        break;
    }
    case F_HZ: {
        uint32_t hz = SVF_HZ[x & 127];
        if (hz >= 1000u) { d = put_u(d, hz / 1000u); *d++ = '.'; d = put_u(d, hz / 100u % 10u); put_s(d, "k"); }
        else put_u(d, hz);
        break;
    }
    case F_LIST: put_s(d, v->list[x]); break;
    case F_WAV:                                 /* 1..16 the synth's variants, then the samples by name */
        if (x < 16) { d = put_s(d, "SYN "); put_u(d, (uint32_t)x + 1u); }
        else {
            static const ksmp_t *const BANKS[NVOICE] = {SMP_KICK, SMP_SNARE, SMP_HAT, SMP_TIE, SMP_POD};
            put_s(d, BANKS[voice_of_lane()][x - 16].name);
        }
        break;
    case F_PAN: if (x == 64) put_s(d, "C"); else { *d++ = x < 64 ? 'L' : 'R'; put_u(d, (uint32_t)(x < 64 ? 64 - x : x - 64)); } break;
    case F_BIP: put_i(d, x - 64); break;
    case F_PLUS1: put_u(d, (uint32_t)x + 1u); break;
    case F_PCT: d = put_u(d, (uint32_t)x); put_s(d, "%"); break;
    case F_TICKS: if (!x) put_s(d, "CHORUS"); else { d = put_u(d, (uint32_t)x); put_s(d, "/16"); } break;
    case F_MASK3:
        d[0] = v->p == &P.stretch ? (x & 1 ? 'T' : '-') : (x & 1 ? 'K' : '-');
        d[1] = v->p == &P.stretch ? (x & 2 ? 'P' : '-') : (x & 2 ? 'S' : '-');
        d[2] = v->p == &P.stretch ? (x & 4 ? 'M' : '-') : (x & 4 ? 'H' : '-');
        d[3] = 0;
        break;
    case F_CHAIN: if (x < 2) put_s(d, "OFF"); else { d = put_u(d, (uint32_t)x); put_s(d, " BAR"); } break;
    case F_PAT: d[0] = (char)('A' + x); d[1] = 0; break;
    case F_OMNI: if (!x) put_s(d, "OMNI"); else put_u(d, (uint32_t)x); break;
    default:
        if (v->p == &P.remix && !x) put_s(d, "OFF");
        else put_u(d, (uint32_t)x);
        break;
    }
}

/* ======================================================================== the screen */
static char msg[32];
static uint32_t msg_until;
static void say(const char *s) { put_s(msg, s); msg_until = fm1_ms + 1400u; }

static uint8_t *lane_step(uint32_t l, uint32_t s) { return l < NMAIN ? &P.main[edit_pat][l][s] : &P.sec[l - NMAIN][s]; }

/* ---- the screen: a lime slice. Six rings, one a lane (the kick outside), each cut into as many wedges as its lane
 * has steps, so a 5-step pod turns against a 16-step kick; the peel is the playhead's track (purple while the remix
 * plays); the pattern in the middle; the four knobs in the corners; the page or a message along the bottom. */
#define R2(r) (4u * (r) * (r))                  /* radii as the pixel loop measures them: (2dx+1)^2 + (2dy+1)^2 */
static const uint8_t RING_OUT[NLANE] = {98, 87, 76, 65, 54, 43};
static const uint8_t RING_IN[NLANE] = {88, 77, 66, 55, 44, 33};
#define R_PEEL 104u
#define R_PITH 99u
#define R_CORE 31u

static uint16_t col_scale(uint16_t c, uint32_t pct)   /* pct 0..100 */
{
    uint32_t r = (c >> 11) * pct / 100u, g = ((c >> 5) & 63u) * pct / 100u, b = (c & 31u) * pct / 100u;
    return (uint16_t)(r << 11 | g << 5 | b);
}
static uint16_t col_mix(uint16_t c, uint16_t d, uint32_t pct)   /* pct of d */
{
    uint32_t r = ((c >> 11) * (100u - pct) + (d >> 11) * pct) / 100u;
    uint32_t g = (((c >> 5) & 63u) * (100u - pct) + ((d >> 5) & 63u) * pct) / 100u;
    uint32_t b = ((c & 31u) * (100u - pct) + (d & 31u) * pct) / 100u;
    return (uint16_t)(r << 11 | g << 5 | b);
}

static int8_t held_step = -1;                   /* a white key held on tie / pod / mod: K1 edits it */
static uint32_t cpu_pct;                        /* the render's share of its 5.8 ms, worst of the last second */

static void corner(uint32_t k, const char *name, const char *val, int32_t frac64, uint16_t tint)
{
    /* K1 top left, K2 top right, K3 bottom left, K4 bottom right */
    int32_t right = k & 1u, bottom = k >> 1, w;
    int32_t yl = bottom ? 205 : 1, yv = bottom ? 222 : 17, yb = bottom ? 201 : 35;
    w = text_w(&FONT_S, name);
    cv_text(right ? 237 - w : 3, yl, &FONT_S, name, C_GREY);
    w = text_w(&FONT_S, val);
    cv_text(right ? 237 - w : 3, yv, &FONT_S, val, C_WHITE);
    cv_rect(right ? 193 : 3, yb, 44, 3, C_DARK);
    if (frac64 > 0) cv_rect(right ? 237 - frac64 * 44 / 64 : 3, yb, frac64 * 44 / 64, 3, tint);
}

static void draw_screen(void)
{
    static uint16_t cell[NLANE][NSTEP];         /* swapped RGB565 */
    static uint8_t flag[NLANE][NSTEP];          /* 1 accent (a white edge), 2 open (hollow) */
    uint32_t ring_len[NLANE], l, s, strip, len = P.len ? P.len : 16u;
    uint32_t play_main = (playing && edit_pat == cur_pat) ? (P.start + ui_pos) & 15u : 99u;
    int remix = (ui_remix && playing) || now_held;
    uint16_t peel = swap16(remix ? RGB(120, 40, 150) : playing ? RGB(40, 105, 30) : RGB(26, 60, 20));
    uint16_t hand = swap16(remix ? RGB(255, 170, 255) : C_LIME), pith = swap16(RGB(20, 28, 8));
    uint16_t pith_edge = swap16(RGB(90, 120, 40)), tint = section == SEC_VOICE ? LANE_COL[sel_lane] : C_LIME;
    const pref_t *r = page_refs();
    char a[40], b[24];

    /* ---- the wedges' colours, once */
    for (l = 0; l < NLANE; l++) {
        uint32_t muted = !lane_live(l), sel = l == sel_lane, ph = 99;
        uint16_t base = muted ? RGB(110, 110, 110) : LANE_COL[l];
        ring_len[l] = l < NMAIN ? 16u : (P.slen[l - NMAIN] ? P.slen[l - NMAIN] : 16u);
        if (playing) ph = l < NMAIN ? play_main : ui_spos[l - NMAIN];
        for (s = 0; s < ring_len[l]; s++) {
            uint32_t st = *lane_step(l, s), in = l < NMAIN ? ((s - P.start) & 15u) < len : 1u;
            uint16_t c;
            flag[l][s] = 0;
            if (!in) c = RGB(16, 16, 16);
            else if (st & ST_ON) {
                uint32_t pct = sel ? 100u : 72u;
                if (l >= NMAIN) {               /* the value: tie / pod lighter up, darker down; mod brighter higher */
                    int32_t v = P.val[l - NMAIN][s];
                    if (l == L_MOD) pct = pct * (40u + (uint32_t)clampi(v, 0, 15) * 4u) / 100u;
                    c = v > 0 ? col_mix(col_scale(base, pct), C_WHITE, (uint32_t)v * 3u) : col_scale(base, pct - (uint32_t)(v < 0 ? -v : 0) * 3u);
                    if (l == L_MOD) c = col_scale(base, pct);
                } else c = col_scale(base, pct);
                if (st & ST_ACC) flag[l][s] |= 1u;
                if (st & ST_OPEN) flag[l][s] |= 2u;
                if (s == ph) c = col_mix(c, C_WHITE, 55);
            } else {
                c = col_scale(base, sel ? 24u : 13u);
                if ((s & 3u) == 0u && l < NMAIN) c = col_scale(base, sel ? 32u : 18u);   /* the beats */
                if (s == ph) c = col_scale(base, 55u);
            }
            cell[l][s] = swap16(c);
        }
    }

    for (strip = 0; strip < 3u; strip++) {
        uint32_t y0 = strip * 80u, y, x;
        cv_begin(240, 80, C_BLACK);
        for (y = y0; y < y0 + 80u; y++) {
            uint32_t ay = y < 120u ? 119u - y : y - 120u, sy = (2u * ay + 1u) * (2u * ay + 1u);
            uint16_t *row = cv_px + (y - y0) * 240u;
            if (sy >= R2(R_PEEL)) continue;
            for (x = 0; x < 240u; x++) {
                uint32_t ax = x < 120u ? 119u - x : x - 120u, d2 = (2u * ax + 1u) * (2u * ax + 1u) + sy, q, ang, k;
                if (d2 >= R2(R_PEEL)) continue;
                q = ANG_Q[ay * 120u + ax];
                ang = x >= 120u ? (y < 120u ? q : 2048u - q) : (y < 120u ? 4095u - q : 2048u + q);   /* 0..4095 from 12 o'clock */
                if (d2 >= R2(R_PITH)) {         /* the peel, and the hand on it */
                    row[x] = (playing && ((ang * len) >> 12) == ui_pos) ? hand : peel;
                    continue;
                }
                if (d2 < R2(R_CORE + 1u)) { row[x] = d2 >= R2(R_CORE) ? pith_edge : pith; continue; }
                for (k = 0; k < NLANE; k++)
                    if (d2 >= R2(RING_IN[k])) break;
                if (k == NLANE || d2 >= R2(RING_OUT[k])) continue;   /* between the rings: black */
                {
                    uint32_t L = ring_len[k], al = ang * L, seg = al >> 12, frac = al & 4095u;
                    uint32_t rm = (RING_IN[k] + RING_OUT[k]) / 2u;
                    if (frac * rm < 652u * L) continue;   /* a one-pixel cut between wedges */
                    if ((flag[k][seg] & 1u) && d2 >= R2(RING_OUT[k] - 2u)) { row[x] = 0xFFFFu; continue; }   /* accent */
                    if ((flag[k][seg] & 2u) && d2 < R2(rm)) { row[x] = swap16(RGB(10, 10, 10)); continue; }   /* open */
                    row[x] = cell[k][seg];
                }
            }
        }
        cv_oy = -(int32_t)y0;                   /* the texts in screen coordinates, clipped to this strip */
        /* the knobs */
        {
            uint32_t k;
            for (k = 0; k < 4u; k++) {
                pval_t v = pval(r[k]);
                if (held_step >= 0 && k == 0u && sel_lane >= L_TIE) {
                    int32_t sv = P.val[sel_lane - NMAIN][held_step];
                    put_u(put_s(a, "STEP "), (uint32_t)held_step + 1u);
                    if (sel_lane == L_MOD) put_u(b, (uint32_t)sv); else put_i(b, sv);
                    corner(0, a, b, sel_lane == L_MOD ? sv * 64 / 15 : (sv + 12) * 64 / 24, tint);
                    continue;
                }
                if (!v.p) continue;
                fmt_val(&v, b);
                corner(k, v.name, b, v.max > v.min ? (pget(&v) - v.min) * 64 / (v.max - v.min) : 0, tint);
            }
        }
        /* the top: the slot and the tempo */
        {
            char *d = put_s(a, "S");
            d = put_u(d, set.slot + 1u);
            d = put_s(d, "  ");
            d = put_u(d, P.bpm);
            if (P.half) put_s(d, "/2");
            cv_text(120 - text_w(&FONT_S, a) / 2, 0, &FONT_S, a, C_GREY);
        }
        /* the core: the pattern (or the held step's value), the chain under it */
        {
            char *d = a;
            if (held_step >= 0 && sel_lane >= L_TIE) {
                int32_t sv = P.val[sel_lane - NMAIN][held_step];
                if (sel_lane == L_MOD) put_u(a, (uint32_t)sv); else put_i(a, sv);
            } else {
                *d++ = (char)('A' + (playing ? cur_pat : edit_pat));
                if (next_pat < NPAT) { *d++ = '>'; *d++ = (char)('A' + next_pat); }
                else if (edit_pat != cur_pat && playing) { *d++ = '/'; *d++ = (char)('A' + edit_pat); }
                *d = 0;
            }
            cv_text(120 - text_w(&FONT_L, a) / 2, 99, &FONT_L, a, held_step >= 0 && sel_lane >= L_TIE ? LANE_COL[sel_lane] : C_LIME);
            if (P.chain_len >= 2u && !(held_step >= 0)) {
                d = put_u(b, ui_chain + 1u);
                d = put_s(d, "/");
                put_u(d, P.chain_len);
                cv_text(120 - text_w(&FONT_S, b) / 2, 129, &FONT_S, b, RGB(140, 170, 80));
            }
        }
        /* the bottom: a message, or where we are */
        {
            uint16_t c = C_WHITE;
            char *d = a;
            if (fm1_ms < msg_until) { put_s(a, msg); c = C_LIME; }
            else {
                if (section == SEC_VOICE) { d = put_s(d, LANE_LONG[sel_lane]); c = LANE_COL[sel_lane]; }
                else d = put_s(d, SEC_NAME[section]);
                if (*page_title()) { d = put_s(d, " "); d = put_s(d, page_title()); }
                if (npages() > 1u) { d = put_s(d, " "); d = put_u(d, page[section] % npages() + 1u); d = put_s(d, "/"); d = put_u(d, npages()); }
                if (section == SEC_GLO) { d = put_s(d, " CPU "); d = put_u(d, cpu_pct); put_s(d, "%"); }
                if (section == SEC_FX && page[SEC_FX] % 6u == 0u && xy_rec) { put_s(a, "REC MOTION"); c = RGB(255, 70, 70); }
            }
            cv_text(120 - text_w(&FONT_S, a) / 2, 224, &FONT_S, a, c);
        }
        cv_oy = 0;
        cv_blit(0, y0);
    }
}

/* a cheap fingerprint of what the page shows: redraw when it changes */
static uint32_t page_sig(void)
{
    const pref_t *r = page_refs();
    uint32_t k, h = section * 31u + page[section] * 7u + sel_lane * 131u + (fm1_ms < msg_until ? msg_until : 0u) * 977u + xy_rec * 13u +
                 (section == SEC_GLO ? cpu_pct * 7919u : 0u);
    for (k = 0; k < 4u; k++) { pval_t v = pval(r[k]); h = h * 33u + (v.p ? (uint32_t)pget(&v) : 0u); }
    if (held_step >= 0) h = h * 33u + 1000u + (uint32_t)held_step * 37u + (uint32_t)(P.val[sel_lane - NMAIN][held_step] + 64);
    return h;
}
static uint32_t grid_sig(void)
{
    uint32_t h = edit_pat * 7u + sel_lane * 3u + P.mute * 11u + P.solo * 13u + P.len * 17u + P.start * 19u + playing * 23u;
    uint32_t i;
    const uint8_t *b = &P.main[edit_pat][0][0];
    for (i = 0; i < NMAIN * NSTEP; i++) h = h * 31u + b[i];
    b = &P.sec[0][0];
    for (i = 0; i < NSEC * NSTEP; i++) h = h * 31u + b[i] + (uint32_t)P.val[i / NSTEP][i % NSTEP] * 3u;
    for (i = 0; i < NSEC; i++) h = h * 31u + P.slen[i];
    if (playing) { h = h * 31u + ui_pos + cur_pat * 64u; for (i = 0; i < NSEC; i++) h = h * 31u + ui_spos[i]; }
    return h;
}
static uint32_t head_sig(void)
{
    return set.slot + cur_pat * 8u + edit_pat * 32u + next_pat * 128u + P.bpm * 1024u + P.half * 7u + playing * 3u +
           ((ui_remix && playing) || now_held) * 5u + ui_chain * 11u + P.chain_len * 13u;
}

/* ======================================================================== the keys */
#define NKEY 27u
static const int8_t KEY_STEP[NKEY] = {0, -1, 1, -1, 2, -1, 3, 4, -1, 5, -1, 6, 7, -1, 8, -1, 9, -1, 10, 11, -1, 12, -1, 13, 14, -1, 15};
enum { BK_KICK = 1, BK_SNARE = 3, BK_HAT = 5, BK_TIE = 8, BK_POD = 10, BK_MOD = 13, BK_A = 15, BK_B = 17, BK_C = 20, BK_D = 22, BK_NOW = 25 };
static int8_t black_lane(uint32_t k)
{
    switch (k) {
    case BK_KICK: return L_KICK; case BK_SNARE: return L_SNARE; case BK_HAT: return L_HAT;
    case BK_TIE: return L_TIE; case BK_POD: return L_POD; case BK_MOD: return L_MOD;
    }
    return -1;
}
static int8_t black_pat(uint32_t k) { return k == BK_A ? 0 : k == BK_B ? 1 : k == BK_C ? 2 : k == BK_D ? 3 : -1; }
static int8_t step_key(uint32_t s) { uint32_t k; for (k = 0; k < NKEY; k++) if (KEY_STEP[k] == (int8_t)s) return (int8_t)k; return -1; }

static uint32_t key_t0[NKEY];
static uint8_t key_long[NKEY], key_off[NKEY];
static int8_t last_val[NSEC] = {0, 0, 8};
static uint8_t proj_dirty;

static void step_down(uint32_t s)
{
    uint8_t *st = lane_step(sel_lane, s);
    int8_t k = step_key(s);
    if (fm1_in.buttons & (1u << BTN_SEL) && sel_lane == L_HAT) {   /* SEL + a hat step: open / closed */
        *st = (uint8_t)((*st | ST_ON) ^ ST_OPEN);
        key_long[k] = 1;
        key_off[k] = 0;
        return;
    }
    key_off[k] = 0;
    key_long[k] = 0;
    if (*st & ST_ON) key_off[k] = 1;            /* it goes off at the release, unless held long */
    else {
        *st = ST_ON;
        if (sel_lane >= L_TIE) P.val[sel_lane - NMAIN][s] = last_val[sel_lane - NMAIN];
    }
    if (sel_lane >= L_TIE) held_step = (int8_t)s;
    proj_dirty = 1;
}
static void step_hold(uint32_t s)
{
    int8_t k = step_key(s);
    uint8_t *st = lane_step(sel_lane, s);
    if (key_long[k] || fm1_ms - key_t0[k] < 350u || sel_lane == L_MOD) return;
    key_long[k] = 1;
    key_off[k] = 0;
    *st = (uint8_t)((*st | ST_ON) ^ ST_ACC);    /* held: the accent */
    say(*st & ST_ACC ? "ACCENT" : "NORMAL");
}
static void step_up(uint32_t s)
{
    int8_t k = step_key(s);
    uint8_t *st = lane_step(sel_lane, s);
    if (key_off[k]) *st = 0;
    key_off[k] = 0;
    if (held_step == (int8_t)s) held_step = -1;
}

static uint8_t pat_held = 0xFF;
static void select_pattern(uint32_t p)
{
    if (pat_held < NPAT && pat_held != p) {     /* hold one, press another: copy */
        uint32_t l, s;
        for (l = 0; l < NMAIN; l++) for (s = 0; s < NSTEP; s++) P.main[p][l][s] = P.main[pat_held][l][s];
        say("COPIED");
        edit_pat = (uint8_t)p;
        return;
    }
    pat_held = (uint8_t)p;
    edit_pat = (uint8_t)p;
    if (P.chain_len < 2u) {
        if (playing) next_pat = (uint8_t)p;
        else cur_pat = (uint8_t)p;
    }
}

/* ======================================================================== MIDI, tempo, storage */
static uint32_t clk_n, clk_t0, ext_x10, ext_last, ext_run;
static void set_tempo(void)
{
    uint32_t x10 = (set.sync && ext_x10 && fm1_ms - ext_last < 500u) ? ext_x10 : (uint32_t)P.bpm * 10u;
    if (x10 < 200u) x10 = 200u;
    tempo_inc = x10 * 253624u / 100000u;        /* 4 * 2^24 / (60 * 44100) = 25.3624 per BPM; x10 */
    if (ext_run && fm1_ms - ext_last > 500u) ext_run = 0;   /* the clock has gone */
    ext_lock = (uint8_t)(set.sync && ext_run);
    frames16 = 6615000u / x10;
}
static uint32_t idle_ok;
static void midi_in(void)
{
    uint8_t st, d1, d2;
    while (midi_get(&st, &d1, &d2)) {
        uint32_t ty = st & 0xF0u;
        if (st == 0xF8u) {
            if (clk_n % 24u == 0u) {
                if (clk_n) { uint32_t ms = fm1_ms - clk_t0; if (ms) ext_x10 = 600000u / ms; }
                clk_t0 = fm1_ms;
            }
            clk_n++;
            ext_last = fm1_ms;
            if (ext_run) ext_clk++;
            continue;
        }
        if (set.sync && (st == 0xFAu || st == 0xFBu)) { clk_n = 0; ext_clk = 0; ext_run = 1; ext_last = fm1_ms; play_req = 1; continue; }
        if (set.sync && st == 0xFCu) { ext_run = 0; stop_req = 1; continue; }
        if (set.ch && (st & 15u) != set.ch - 1u) continue;
        if (ty == 0x90u && d2) {
            uint32_t acc = d2 >= 100u ? 1u : 0u;
            switch (d1) {
            case 36: case 35: trig_push(L_KICK, acc, 0); break;
            case 38: case 40: trig_push(L_SNARE, acc, 0); break;
            case 42: case 44: trig_push(L_HAT, acc, 0); break;
            case 46: trig_push(L_HAT, acc | 2u, 0); break;
            case 39: trig_push(L_POD, acc, 0); break;
            default: if (d1 >= 48u && d1 <= 72u) trig_push(L_TIE, acc, (int32_t)d1 - 60); break;
            }
        }
    }
}
static void app_idle(void) { if (idle_ok) midi_in(); }

static int proj_load(uint32_t slot)
{
    static project_t t;
    static project02_t o;
    if (!blob_load(slot, &t, sizeof t) && t.magic == PROJ_MAGIC) { P = t; return 0; }
    /* a 0.2 project: the same up to the effects, which were fewer; the new ones get their defaults */
    if (blob_load(slot, &o, sizeof o) || o.magic != PROJ_MAGIC) return -1;
    proj_default(&t);
    proj_upgrade02(&t, &o);
    P = t;
    return 0;
}

/* ======================================================================== the main loop */
static void encoders(void)
{
    const pref_t *r = page_refs();
    int32_t e;
    uint32_t k;
    if ((e = fm1_enc_take(ENC_SELECT)) != 0) {
        uint32_t n = npages();
        page[section] = (uint8_t)((page[section] + (uint32_t)(e > 0 ? 1 : (int32_t)n - 1)) % n);
    }
    if ((e = fm1_enc_take(ENC_ALGORITHM)) != 0) { P.bpm = (uint8_t)clampi((int32_t)P.bpm + e, 40, 250); proj_dirty = 1; }
    if ((e = fm1_enc_take(ENC_PRESETS)) != 0) {
        char s[24];
        set.slot = (uint8_t)((set.slot + (uint32_t)(e > 0 ? 1 : 7)) & 7u);
        put_u(put_s(s, "SLOT "), set.slot + 1u);
        say(s);
    }
    for (k = 0; k < 4u; k++) {
        if ((e = fm1_enc_take(ENC_KNOB1 + k)) == 0) continue;
        if (k == 0u && held_step >= 0 && sel_lane >= L_TIE) {   /* the held step's value */
            uint32_t sec = sel_lane - NMAIN;
            int32_t lo = sel_lane == L_MOD ? 1 : -12, hi = sel_lane == L_MOD ? 15 : 12;
            int32_t v = clampi(P.val[sec][held_step] + e, lo, hi);
            P.val[sec][held_step] = (int8_t)v;
            last_val[sec] = (int8_t)v;
            key_off[step_key((uint32_t)held_step)] = 0;   /* (turned: it stays on) */
            continue;
        }
        {
            pval_t v = pval(r[k]);
            if (!v.p) continue;
            if ((e > 1 || e < -1) && v.max - v.min > 40) e *= 3;   /* fast: bigger steps */
            pset(&v, pget(&v) + e);
            proj_dirty = 1;
        }
    }
}

static void app_main(void)
{
    uint32_t prev = 0, k, gs = ~0u, set_dirty = 0, t_set = 0;
    uint32_t last_play_led = 0, last_set;
    remix_build();
    if (settings_load(&set, sizeof set) || set.magic != SET_MAGIC || set.slot > 7u || set.ch > 16u) {
        set.slot = 0; set.ch = 0; set.out = 0; set.sync = 1; set.magic = SET_MAGIC;
    }
    if (proj_load(set.slot)) proj_default(&P);
    pitch_all = (int8_t)((int32_t)P.fx[FX_PITCH] - 12);
    set_tempo();
    last_set = set.slot | set.ch << 4 | set.out << 9 | set.sync << 10;
    lcd_fill(0, 0, 240, 240, C_BLACK);

    for (;;) {
        uint32_t keys, down, up, b, rel;
        platform_poll();

        /* ---- keys */
        keys = fm1_in.notes;
        down = keys & ~prev;
        up = prev & ~keys;
        prev = keys;
        for (k = 0; k < NKEY; k++) {
            int8_t s = KEY_STEP[k];
            if (down >> k & 1u) {
                key_t0[k] = fm1_ms;
                if (s >= 0) step_down((uint32_t)s);
                else if (black_lane(k) >= 0) {
                    uint32_t l = (uint32_t)black_lane(k);
                    if (sel_lane != l) held_step = -1;
                    sel_lane = (uint8_t)l;
                    section = SEC_VOICE;
                    if (!playing && l != L_MOD) trig_push(l, 0, 0);   /* stopped: hear it */
                } else if (black_pat(k) >= 0) select_pattern((uint32_t)black_pat(k));
                else if (k == BK_NOW) now_held = 1;
            }
            if ((keys >> k & 1u) && s >= 0) step_hold((uint32_t)s);
            if (up >> k & 1u) {
                if (s >= 0) step_up((uint32_t)s);
                else if (k == BK_NOW) now_held = 0;
                else if (black_pat(k) >= 0 && (uint8_t)black_pat(k) == pat_held) pat_held = 0xFF;
            }
        }

        /* ---- buttons */
        b = fm1_input_edges(&rel);
        if (b & (1u << BTN_PLAY)) { ext_run = 0; if (playing) { stop_req = 1; xy_rec = 0; } else play_req = 1; }
        if (b & (1u << BTN_HOME) || b & (1u << BTN_EDIT)) {
            if (section == SEC_VOICE) page[SEC_VOICE] = (uint8_t)((page[SEC_VOICE] + 1u) % npages());
            section = SEC_VOICE;
        }
        if (b & (1u << BTN_FX)) { if (section == SEC_FX) page[SEC_FX] = (uint8_t)((page[SEC_FX] + 1u) % 6u); section = SEC_FX; }
        if (b & (1u << BTN_SEQ)) { if (section == SEC_SEQ) page[SEC_SEQ] = (uint8_t)((page[SEC_SEQ] + 1u) % 3u); section = SEC_SEQ; }
        if (b & (1u << BTN_ARP)) section = SEC_RMX;
        if (b & (1u << BTN_GLO)) section = SEC_GLO;
        if (b & (1u << BTN_OCT_DOWN)) { sel_lane = (uint8_t)((sel_lane + NLANE - 1u) % NLANE); held_step = -1; }
        if (b & (1u << BTN_OCT_UP)) { sel_lane = (uint8_t)((sel_lane + 1u) % NLANE); held_step = -1; }
        if (b & (1u << BTN_ENV)) { P.mute ^= (uint8_t)(1u << sel_lane); say(P.mute >> sel_lane & 1u ? "MUTE" : "UNMUTE"); }
        if (b & (1u << BTN_LFO)) { P.solo ^= (uint8_t)(1u << sel_lane); say(P.solo >> sel_lane & 1u ? "SOLO" : "SOLO OFF"); }
        if (b & (1u << BTN_REC)) {
            int lane_key = -1;
            for (k = 0; k < NKEY; k++) if ((keys >> k & 1u) && black_lane(k) >= 0) lane_key = black_lane(k);
            if (lane_key >= 0) {                /* a lane key held + REC: clear the lane */
                uint32_t s;
                for (s = 0; s < NSTEP; s++) *lane_step((uint32_t)lane_key, s) = 0;
                say("CLEARED");
            } else if (section == SEC_FX && page[SEC_FX] % 6u == 0u) {
                if (fm1_in.buttons & (1u << BTN_SEL)) {   /* SEL + REC: forget the motion */
                    for (k = 0; k < NXY; k++) P.xy[k][0] = P.xy[k][1] = 255;
                    xy_rec = 0;
                    say("MOTION CLEARED");
                } else {
                    xy_rec = (uint8_t)!xy_rec;
                    if (xy_rec && !playing) play_req = 1;
                    say(xy_rec ? "REC MOTION" : "MOTION KEPT");
                }
            } else if (section == SEC_RMX) { now_held = (uint8_t)!now_held; say(now_held ? "REMIX NOW" : "REMIX AS SET"); }
        }
        if (b & (1u << BTN_SAVE)) {
            char s[24];
            if (fm1_in.buttons & (1u << BTN_SEL)) {
                if (playing) say("STOP TO LOAD");
                else if (proj_load(set.slot)) say("EMPTY SLOT");
                else { put_u(put_s(s, "LOADED S"), set.slot + 1u); say(s); edit_pat = cur_pat = 0; next_pat = 0xFF; }
            } else if (playing) say("STOP TO SAVE");
            else {
                P.magic = PROJ_MAGIC;
                if (blob_save(set.slot, &P, sizeof P)) say("SAVE FAILED");
                else { put_u(put_s(s, "SAVED S"), set.slot + 1u); say(s); set_dirty = 1; t_set = fm1_ms; }
            }
        }
        encoders();
        midi_in();

        /* ---- the effects' pad: the knobs, or the recorded motion */
        {
            uint32_t i = gstep % NXY, x = P.fx[FX_X], y = P.fx[FX_Y];
            if (xy_rec && playing) { P.xy[i][0] = (uint8_t)x; P.xy[i][1] = (uint8_t)y; }
            else if (P.fx[FX_XAUTO] && playing && P.xy[i][0] != 255u) {
                x = P.xy[i][0]; y = P.xy[i][1];
                if (P.fx[FX_XAUTO] == 2u) {     /* SMOOTH: toward the next point across the 16th */
                    uint32_t j = (i + 1u) % NXY, f = S.frac >> 16;   /* 0..255 */
                    if (P.xy[j][0] != 255u) {
                        x = (x * (256u - f) + P.xy[j][0] * f) >> 8;
                        y = (y * (256u - f) + P.xy[j][1] * f) >> 8;
                    }
                }
            }
            fx_x = (uint8_t)x;
            fx_y = (uint8_t)y;
        }
        pitch_all = (int8_t)((int32_t)P.fx[FX_PITCH] - 12);
        set_tempo();
        {
            static uint32_t t_cpu;
            if (fm1_ms - t_cpu >= 1000u) { t_cpu = fm1_ms; cpu_pct = audio_us * 100u / 5805u; audio_us = 0; }
        }

        /* ---- MIDI out: the hits */
        while (hitq_r != hitq_w) {
            uint32_t n = hitq[hitq_r % 32u], ch = set.ch ? set.ch - 1u : 9u;
            hitq_r++;
            if (set.out) { midi_send(0x90u | ch, n, 100); midi_send(0x80u | ch, n, 0); }
        }

        /* ---- the LEDs */
        {
            uint32_t len = sel_lane < NMAIN ? (P.len ? P.len : 16u) : (P.slen[sel_lane - NMAIN] ? P.slen[sel_lane - NMAIN] : 16u);
            uint32_t ph = sel_lane < NMAIN ? (P.start + ui_pos) & 15u : ui_spos[sel_lane - NMAIN];
            for (k = 0; k < NKEY; k++) {
                int8_t s = KEY_STEP[k];
                int on = 0, glow = 0;
                if (s >= 0) {
                    on = (*lane_step(sel_lane, (uint32_t)s) & ST_ON) != 0u;
                    if (sel_lane >= NMAIN && (uint32_t)s >= len) on = 0;
                    if (playing && (uint32_t)s == ph && (sel_lane >= NMAIN || edit_pat == cur_pat)) on = !on;
                } else if (black_lane(k) >= 0) { on = black_lane(k) == (int8_t)sel_lane; glow = 1; }
                else if (black_pat(k) >= 0) { on = black_pat(k) == (int8_t)edit_pat; glow = black_pat(k) == (int8_t)cur_pat; }
                else if (k == BK_NOW) on = now_held || (playing && ui_remix);
                fm1_led_key(KEY_LED(k), on);
                fm1_led_key_glow(KEY_LED(k), glow);
            }
            if (playing && fm1_ms - last_play_led > 0u) last_play_led = fm1_ms;
            fm1_led_key(BTN_PLAY, playing && (ui_pos & 3u) != 3u);
            fm1_led_key(BTN_REC, xy_rec);
            fm1_led_key(BTN_HOME, section == SEC_VOICE);
            fm1_led_key(BTN_FX, section == SEC_FX);
            fm1_led_key(BTN_SEQ, section == SEC_SEQ);
            fm1_led_key(BTN_ARP, section == SEC_RMX);
            fm1_led_key(BTN_GLO, section == SEC_GLO);
            fm1_led_key(BTN_ENV, P.mute >> sel_lane & 1u);
            fm1_led_key(BTN_LFO, P.solo >> sel_lane & 1u);
        }

        /* ---- the screen: what changed */
        idle_ok = 1;
        if ((k = grid_sig() * 31u + page_sig() * 7u + head_sig()) != gs) { gs = k; draw_screen(); }
        idle_ok = 0;

        /* ---- the settings (slot, MIDI) 2 s after a change, when stopped */
        {
            uint32_t sig = set.slot | set.ch << 4 | set.out << 9 | set.sync << 10;
            if (sig != last_set) { last_set = sig; set_dirty = 1; t_set = fm1_ms; }
            if (set_dirty && !playing && fm1_ms - t_set > 2000u) { settings_save(&set, sizeof set); set_dirty = 0; }
        }
        (void)proj_dirty;
    }
}
