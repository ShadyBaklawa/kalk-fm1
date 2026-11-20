/* SPDX-License-Identifier: GPL-3.0-only */
/* KALK: the project (what is saved) and the names of things. One project = one blob (4 KiB). */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define NSTEP 16u
#define NPAT 4u                                 /* A B C D */
#define NMAIN 3u                                /* the main sequence: kick, snare, hat */
#define NSEC 3u                                 /* the second window: tie, pod, mod (own lengths) */
#define NLANE 6u
#define NVOICE 5u                               /* kick snare hat tie pod (the hat plays two voices: closed, open) */
#define NVP 16u                                 /* parameters per voice */
#define NFX 32u                                /* (0.2 had 24: the reverb came after) */
#define NFX02 24u
#define NXY 64u                                 /* XFX motion: 64 points (4 bars of 16ths) */
#define NCHAIN 8u

enum { L_KICK, L_SNARE, L_HAT, L_TIE, L_POD, L_MOD };

/* a step: on, accent, open (hat only) */
#define ST_ON 1u
#define ST_ACC 2u
#define ST_OPEN 4u

/* voice parameters (the meaning of X1..X4 depends on the voice; kalk_ui.c names them) */
enum {
    VP_TUNE, VP_DECAY, VP_WAV, VP_GAIN,
    VP_CUT, VP_RES, VP_FTYPE, VP_PAN,
    VP_X1, VP_X2, VP_X3, VP_X4,
    VP_MODDST, VP_MODAMT, VP_ACC, VP_START,
};
enum { MD_OFF, MD_WAV, MD_PITCH, MD_DECAY, MD_CUT };

/* effects parameters, four to a page */
enum {
    FX_XTYPE, FX_X, FX_Y, FX_XAUTO,             /* XFX: effect, the XY pad, motion OFF / PLAY / SMOOTH */
    FX_DL, FX_DR, FX_DFB, FX_DSEND,             /* delay: left / right in 16ths, feedback, send */
    FX_DDAMP, FX_DWIDTH, FX_XMIX, FX_MODFX,     /* delay damping, width; XFX dry/wet; the mod lane on XFX */
    FX_FCUT, FX_FRES, FX_FMOD, FX_FTRK,         /* master filter: cutoff, resonance, mod lane depth, envelope follow */
    FX_EQLO, FX_EQMID, FX_EQHI, FX_EQAMP,       /* EQ: lo mid hi, saturation */
    FX_GAIN, FX_PITCH, FX_DIRT, FX_NORM,        /* out: volume, pitch (+-12), dirt, norm (the maximiser) */
    FX_RTIME, FX_RSIZE, FX_RDAMP, FX_RSEND,     /* reverb: RT60, room size, high damping, send from the mix */
    FX_RLOW, FX_RMOD, FX_RWIDTH, FX_RDLY,       /* reverb: low cut, modulation, width (0: mono), send from the delay */
};
enum { XF_THRU, XF_CRUSH, XF_BPF, XF_RING, XF_COMB, XF_REPEAT, XF_LPF, XF_HPF, XF_FOLD, XF_GATE, XF_PAN, XF_FLANGE, NXF };

typedef struct {
    uint8_t main[NPAT][NMAIN][NSTEP];           /* steps of A-D: ST_ bits */
    uint8_t sec[NSEC][NSTEP];                   /* tie pod mod steps: ST_ bits */
    int8_t val[NSEC][NSTEP];                    /* tie / pod: semitones -12..12; mod: 1..15 */
    uint8_t len, start;                         /* the main sequence's length 1..16 and its first step 0..15 */
    uint8_t slen[NSEC];                         /* tie pod mod: 1..16 */
    uint8_t stretch;                            /* bit per second-window lane: fit to the main length */
    uint8_t swing, half;                        /* 0..100 %; half tempo */
    uint8_t chain_len;                          /* 0: off; 2..8 bars */
    uint8_t chain[NCHAIN];                      /* the pattern sequence: 0..3 = A..D */
    uint8_t remix, rfreq, rshift, rlanes;       /* remix: variation 0 (off) ..127, how often, shift, which lanes */
    uint8_t bpm;                                /* 40..250 */
    uint8_t mute, solo;                         /* bit per lane */
    uint8_t vp[NVOICE][NVP];
    uint8_t fx[NFX];
    uint8_t xy[NXY][2];                         /* 255: no point here */
    uint8_t pad[3];
    uint32_t magic;                             /* PROJ_MAGIC: a valid project */
} project_t;
#define PROJ_MAGIC 0x4B4C4B31u                  /* "1KLK" */
_Static_assert(sizeof(project_t) < 4000u, "a project is one blob");

/* a project as 0.2 saved it (NFX02 effects): loaded, then given the new parameters' defaults */
typedef struct {
    uint8_t head[offsetof(project_t, fx)];      /* everything before fx: the same */
    uint8_t fx[NFX02];
    uint8_t xy[NXY][2];
    uint8_t pad[3];
    uint32_t magic;
} project02_t;

/* a 0.2 project into t, which already holds the defaults: everything 0.2 had is copied, the rest stays */
static void proj_upgrade02(project_t *t, const project02_t *o)
{
    uint32_t i;
    for (i = 0; i < sizeof o->head; i++) ((uint8_t *)t)[i] = o->head[i];
    for (i = 0; i < NFX02; i++) t->fx[i] = o->fx[i];
    for (i = 0; i < NXY; i++) { t->xy[i][0] = o->xy[i][0]; t->xy[i][1] = o->xy[i][1]; }
    t->magic = o->magic;
}
