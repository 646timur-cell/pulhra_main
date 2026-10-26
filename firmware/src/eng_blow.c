/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Tim (646timur-cell), on Felucca by Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* BLOW (STRANGE): wind instruments as waveguides, after STK's Clarinet, Flute and Saxofony (P. Cook, G. Scavone; the
 * models of J. O. Smith, M. E. McIntyre, R. T. Schumacher, J. Woodhouse). A bore (a delay line closed by a lossy
 * reflection: TONE) is driven by a valve that the breath and the bore's own pressure open and close:
 *   CLAR   a reed: the reed table (a line clipped at +-1) on the difference between the breath and the bore's
 *          returning pressure; a cylinder closed at the reed (half a period long: odd harmonics, hollow). REED the
 *          reed's stiffness (soft, buzzy .. stiff, pure)
 *   FLUTE  an air jet: the breath crosses a short jet delay into a cubic x (x^2 - 1), the bore's pressure fed back
 *          into it; JET (REED) the jet's length against the bore's: around its middle the note, short or long it
 *          overblows to the upper registers
 *   SAX    a reed on a cone: the bore in two parts either side of the reed, POS (REED) where (all the harmonics:
 *          a saxophone, a horn as TONE closes)
 * The breath is the track's envelope x velocity (ATK the attack of the breath, REL its end), times BRTH (high:
 * overblown, squealing). NOIS is breath noise inside the model (turbulence), AIR breath noise heard beside it, VIBR a
 * vibrato that comes in after the note starts, GROWL the throat (the breath shaken an octave down: growl, flutter).
 * Lines of Q20; 2048 + 1024 samples (below ~34 Hz a note plays an octave up); poly 2. */

#define BW_LEN 2048u
#define BW_JLEN 1024u
#define BW_POLY 2
enum { BW_CLAR, BW_FLUTE, BW_SAX, BW_N };
static const char *const N_BW_TYPE[] = {"CLAR", "FLUTE", "SAX"};

typedef struct {
    int32_t bore[BW_LEN];                                /* the bore (SAX: from the reed to the bell) */
    int32_t jet[BW_JLEN];                                /* FLUTE: the jet; SAX: the bore from the reed to its top */
    uint32_t w, rng, vph;
    int32_t lp, oz, dcx, dcy, alp, onset;                /* TONE's low-pass, the one-zero, DC block, AIR, vibrato */
} bw_slot_t;
ENG_MEM_FITS("BLOW", sizeof(bw_slot_t) * BW_POLY);

static bw_slot_t *bw_slot(track_t *t, voice_t *v)
{
    uint32_t i = (uint32_t)(v - t->v);
    if (t < &trk[0] || t >= &trk[NLIVE] || i >= BW_POLY)
        return 0;
    return (bw_slot_t *)ENG_MEM(t - trk) + i;
}

static void blow_note_on(track_t *t, voice_t *v)
{
    bw_slot_t *S = bw_slot(t, v);
    uint32_t i;
    if (!S)
        return;
    if (!v->env && !v->env_out) {                        /* from rest: still air */
        for (i = 0; i < BW_LEN; i++)
            S->bore[i] = 0;
        for (i = 0; i < BW_JLEN; i++)
            S->jet[i] = 0;
        S->lp = S->oz = S->dcx = S->dcy = S->alp = 0;
        S->rng = (uint32_t)rng() | 1u;
        S->onset = 0;
    }
}

/* the reed (STK's ReedTable): offset + slope x, clipped to +-1; x Q20, slope Q15, the result Q15 */
static inline int32_t bw_reed(int32_t x, int32_t off, int32_t slope)
{
    return clamp(off + px_m(slope, x >> 5, 15), -32767, 32767);
}

static void blow_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    bw_slot_t *S = bw_slot(t, v);
    uint32_t type = (uint32_t)clamp(p[P_E0], 0, BW_N - 1), i, w, P, D, D2 = 0, vinc, gph = v->ph[0], ginc = m->inc >> 1;
    int32_t brth, reed = p[P_E2], nois = p[P_E3] * 77, kt, vib, growl = p[P_E6] * 200, air = p[P_E7] * 128;
    int32_t slope = 0, dvib, lp, oz, dcx, dcy;
    if (!S)
        return;
    if (n > CTL)
        n = CTL;
    P = sx_period(m->inc);
    if (P > (1300u << 16))                               /* (the lowest notes: an octave up rather than out of line) */
        P >>= 1;
    /* the bores (Q16), as STK's, less the loop's own delays (measured: tests/strange_test.c tunes them) */
    switch (type) {
    case BW_CLAR:                                        /* closed at the reed: half a period */
        D = (P >> 1) - (1u << 16);
        slope = -16384 + reed * 129;                     /* the reed's stiffness: -0.5 .. 0 (STK: -0.3) */
        brth = 19661 + p[P_E1] * 180;                    /* 0.6 .. 1.3 of the envelope */
        break;
    case BW_FLUTE:                                       /* STK's: 1.5 periods, the jet sounding the note */
        D = (uint32_t)(((uint64_t)(P + (P >> 1)) * 68289u) >> 16) - (2u << 16);   /* (x 1.042: the jet sounds sharp) */
        D2 = (uint32_t)(((uint64_t)D * (uint32_t)(17039 + reed * 43)) >> 16);   /* the jet: 0.26 .. 0.34 .. 0.43 */
        brth = 36045 + p[P_E1] * 230;                    /* 1.1 .. 2.0 */
        break;
    default:                                             /* SAX: a cone, the reed at REED of its length (STK) */
        D = P - 45875u;                                  /* - 0.7 */
        D2 = (uint32_t)(((uint64_t)D * (uint32_t)(9830 + reed * 160)) >> 16);   /* 0.15 .. 0.46 */
        brth = 19661 + p[P_E1] * 180;
        break;
    }
    kt = clamp(8000 + p[P_E4] * 194 + m->cutoff, 3000, 32767);   /* TONE: the bore's loss (ENV / LFO -> FLT) */
    brth += px_m(brth, MF_E8(p) * 516, 15);              /* OVBL (MONOFM's ninth): blown up to 3 x harder: the upper
                                                          * registers, a squeal */
    /* the vibrato: 5.3 Hz, coming in over ~0.4 s, up to +-1.2 % of the bore (about +-20 cents) */
    S->onset = S->onset < 32767 - 80 ? S->onset + 80 : 32767;
    vinc = 516178u * 32u;
    vib = px_m(px_sin(S->vph) >> 15, px_m(p[P_E5] * 258, S->onset, 15), 15);
    S->vph += vinc;
    dvib = (int32_t)(((int64_t)D * vib) >> 22);
    D += (uint32_t)dvib;
    D2 += (uint32_t)(((int64_t)D2 * vib) >> 22);
    D = clamp((int32_t)(D >> 1), 2 << 15, (int32_t)(BW_LEN - 8u) << 15) << 1;
    D2 = clamp((int32_t)(D2 >> 1), 2 << 15, (int32_t)(BW_JLEN - 8u) << 15) << 1;
    w = S->w;
    lp = S->lp;
    oz = S->oz;
    dcx = S->dcx;
    dcy = S->dcy;
    for (i = 0; i < n; i++) {
        int32_t amp = amp_at(m, i), bp, b = sx_read(S->bore, BW_LEN - 1u, w, D - D2 * (type == BW_SAX)), o, f;
        int32_t nz = sx_noise(&S->rng);
        bp = px_m(amp << 5, brth, 15);                   /* the breath (Q20) */
        if (growl) {                                     /* the throat: shaken an octave down, and roughened */
            bp += px_m(bp, px_m(gph < 0x80000000u ? 16384 : -16384, growl, 15) + px_m(nz >> 5, growl, 15), 15);
            gph += ginc;
        }
        if (nois)                                        /* turbulence */
            bp += px_m(px_m(nz, bp, 20), nois, 15);
        f = (b + oz) >> 1;                               /* the bore's loss: STK's one-zero, then TONE */
        oz = b;
        lp += px_m(f - lp, kt, 15);
        switch (type) {
        case BW_CLAR: {
            int32_t pd = -px_m(lp, 31130, 15) - bp;      /* -0.95 */
            S->bore[w & (BW_LEN - 1u)] = bp + px_m(pd, bw_reed(pd, 22938, slope), 15);
            o = b;
            break;
        }
        case BW_FLUTE: {
            int32_t tp = -lp, pd, j;
            tp = tp - dcx + px_m(dcy, 32604, 15);        /* DC block */
            dcx = -lp;
            dcy = tp;
            pd = bp - (tp >> 1);
            S->jet[w & (BW_JLEN - 1u)] = clamp(pd, -5 * SQ_ONE, 5 * SQ_ONE);   /* (not +-4: build.py takes -4.0 for a ROM
                                                                                 * address) */
            j = sx_read(S->jet, BW_JLEN - 1u, w + 1u, D2);
            j = clamp(px_m(j, px_m(j, j, 20) - SQ_ONE, 20), -SQ_ONE, SQ_ONE);   /* the jet: x (x^2 - 1) */
            S->bore[w & (BW_LEN - 1u)] = j + (tp >> 1);
            o = (b * 5) >> 3;                            /* (STK's 0.3, and some gain back) */
            break;
        }
        default: {                                       /* SAX */
            int32_t l1 = sx_read(S->jet, BW_JLEN - 1u, w, D2), tp = -px_m(lp, 31130, 15), pd;
            o = tp - l1;
            pd = bp - o;
            S->jet[w & (BW_JLEN - 1u)] = tp;
            S->bore[w & (BW_LEN - 1u)] = bp - px_m(pd, bw_reed(pd, 22938, 9830), 15) - tp;
            break;
        }
        }
        w++;
        o >>= 5;                                         /* Q20 -> Q15 (26.10: was / 2, BLOW 6 dB under the others) */
        if (air) {                                       /* the breath heard beside the tone */
            S->alp += px_m((nz >> 5) - S->alp, 9000, 15);
            o += px_m(px_m(S->alp, amp, 15), air, 15);
        }
        o = clamp(soft_knee(o, 24000), -32767, 32767);
        out[i] += mulq15(o, VOICE_FS);
    }
    S->w = w;
    S->lp = lp;
    S->oz = oz;
    S->dcx = dcx;
    S->dcy = dcy;
    v->ph[0] = gph;                                      /* (GROWL's octave-down phase) */
}

static const param_desc_t BW_D_JET = {"JET", F_PCT, 0, 127, 40, 0, 0};
static const param_desc_t BW_D_POS = {"POS", F_PCT, 0, 127, 64, 0, 0};
static const param_desc_t *blow_desc(const track_t *t, uint32_t k)
{
    return k != 2u ? 0 : t->p[P_E0] == BW_FLUTE ? &BW_D_JET : t->p[P_E0] == BW_SAX ? &BW_D_POS : 0;
}

#define STRANGE_CAT_BLOW "LLLBLPFFPP"
static const preset_t BLOW_PRESETS[] = {
    /* name, {TYPE, BRTH, REED, NOIS, TONE, VIBR, GROWL, AIR}, {A D S R}, fenv, mono */
    {"CLARINET", {BW_CLAR, 60, 64, 10, 70, 20, 0, 6}, {20, 64, 127, 30}, 0, 1, FX(0, 0, 0, 40)},
    {"FLUTE", {BW_FLUTE, 64, 40, 30, 80, 40, 0, 20}, {24, 64, 127, 30}, 0, 1, FX(0, 0, 10, 50)},
    {"TENOR SAX", {BW_SAX, 60, 120, 14, 80, 24, 4, 8}, {14, 64, 127, 26}, 0, 1, FX(0, 0, 0, 40)},
    {"HORN", {BW_SAX, 54, 90, 4, 40, 10, 0, 0}, {24, 64, 127, 30}, 0, 1, FX(0, 0, 0, 30)},
    {"SHAKUHACHI", {BW_FLUTE, 80, 40, 30, 80, 50, 0, 80}, {40, 64, 127, 50}, 0, 1, FX(0, 0, 10, 70)},
    {"REED DRONE", {BW_CLAR, 64, 64, 16, 56, 0, 30, 10}, {60, 64, 127, 70}, 0, 0, FX(0, 20, 0, 60)},
    {"OVERBLOWN", {BW_SAX, 127, 40, 30, 110, 30, 30, 20}, {10, 64, 127, 40}, 0, 1, FX(10, 0, 20, 50)},
    /* textures */
    {"AIR COLUMN", {BW_CLAR, 70, 64, 40, 60, 20, 0, 110}, {100, 64, 127, 120}, 0, 0, FX(0, 30, 20, 110)},
    {"THROAT DRONE", {BW_CLAR, 80, 64, 30, 40, 0, 50, 30}, {100, 64, 127, 120}, 0, 0, FX(0, 20, 0, 100)},
    {"FOG HORN", {BW_SAX, 54, 90, 20, 20, 0, 40, 40}, {110, 64, 127, 124}, 0, 0, FX(0, 0, 20, 110)},
};

static const engine_t ENG_BLOW = {
    .name = "BLOW",
    .page_title = {"BREATH", "THROAT"},
    .edit = {
        {"TYPE", F_ENUM, 0, BW_N - 1, 0, N_BW_TYPE, 0},
        {"BRTH", F_PCT, 0, 127, 64, 0, 0},
        {"REED", F_PCT, 0, 127, 64, 0, 0},
        {"NOIS", F_PCT, 0, 127, 10, 0, 0},
        {"TONE", F_PCT, 0, 127, 70, 0, 0},
        {"VIBR", F_PCT, 0, 127, 20, 0, 0},
        {"GROWL", F_PCT, 0, 127, 0, 0, 0},
        {"AIR", F_PCT, 0, 127, 6, 0, 0},
    },
    .presets = BLOW_PRESETS,
    .npresets = NELEM(BLOW_PRESETS),
    .note_on = blow_note_on,
    .render = blow_render,
    .desc = blow_desc,
    .knob = {P_E1, P_E2, P_E4, P_E5},
    .poly = BW_POLY,
    .mem = sizeof(bw_slot_t) * BW_POLY,
    .xyd = {{XY_E(1), XY_E(3), XY_E(6)}, {XY_E(4), XY_E(5), XY_E(7)}},   /* X: BRTH NOIS GROWL (blown harder), Y: TONE VIBR AIR */
    .xya = {{40, 40, 30}, {40, 40, 40}},
#if FELUCCA_MONOFM
    .e8 = {"OVBL", F_PCT, 0, 127, 0, 0, 0},
#endif
};
