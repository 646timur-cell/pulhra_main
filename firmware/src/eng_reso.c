/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Tim (646timur-cell), on Felucca by Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* RESO (STRANGE): strange exciters into strange resonators, a physical model of things that are not instruments.
 *   EXCT   FRIC   friction: stick-slip, a rough sawtooth that catches and slips at RATE (a finger on a wet glass, a
 *                 door), its own jitter
 *          BALL   a bouncing ball: a hit, then hits ever closer and softer (RATE: the first gap, 40 ms .. 0.6 s)
 *          SHAKE  rattling: hits at random, RATE a few to hundreds a second (seeds in a gourd, a box of bolts)
 *          SPARK  an electric discharge: bursts of crackle, RATE their density, GRIT how harsh
 *          SCRAPE a surface dragged: noise through RATE's ridges (a pitched grind, 20 .. 800 Hz)
 *   RES    SHEET  a thin metal sheet: eight inharmonic modes (a plate's)
 *          GONG   a gong: eight modes that start sharp and sink (STIFF: how far) as it settles
 *          BOX    a wooden box: eight low modes, short (a crate, a drawer)
 *          TUBE   a glass tube: a waveguide closed at one end (odd harmonics), a little dispersion
 *          WIRE   a stretched wire: a waveguide with four all-passes in the loop (STIFF: a spring's chirp, a cable's)
 *   RATE   the exciter's speed; GRIT its brightness and roughness; DAMP how long the body rings (~30 ms .. ~12 s);
 *   STIFF  the modes stretched apart (SHEET BOX), GONG's sink, the waveguides' dispersion; POS where it is struck /
 *          picked up (the modes' levels, the waveguides' tap); DIRT the body driven (a tanh in the loop: rattle,
 *          buzz) and the exciter heard through
 * The note tunes the body. The track's envelope is the exciting (FRIC SHAKE SPARK SCRAPE go on while it lasts); the
 * body rings on after. Poly 3, its state in the part's engine region (rs_slot_t). */

#define RS_POLY 3
#define RS_NM 8                                          /* modes of SHEET GONG BOX */
#define RS_WG 2048u                                      /* a waveguide line: down to ~22 Hz */
#define RS_WMASK (RS_WG - 1u)
enum { RX_FRIC, RX_BALL, RX_SHAKE, RX_SPARK, RX_SCRAPE, RX_N };
enum { RR_SHEET, RR_GONG, RR_BOX, RR_TUBE, RR_WIRE, RR_N };
static const char *const N_RS_EXCT[] = {"FRIC", "BALL", "SHAKE", "SPARK", "SCRAPE"};
static const char *const N_RS_RES[] = {"SHEET", "GONG", "BOX", "TUBE", "WIRE"};
/* the modes' ratios (log2, Q16) of SHEET GONG BOX */
static const int32_t RS_LOG2[3][RS_NM] = {
    {0, 36420, 62164, 81161, 94893, 114064, 126422, 141573},   /* 1 1.47 1.93 2.36 2.73 3.34 3.81 4.47 */
    {0, 39589, 65536, 79178, 103210, 116698, 133820, 151767},  /* 1 1.52 2 2.31 2.98 3.43 4.12 4.98 */
    {0, 15649, 51862, 67858, 91029, 106986, 128701, 144348},   /* 1 1.18 1.73 2.05 2.62 3.1 3.9 4.6 */
};

typedef struct {
    int32_t y1[RS_NM], y2[RS_NM], a1[RS_NM], r2[RS_NM], gi[RS_NM];
    int32_t wg[RS_WG];                                   /* TUBE / WIRE's line (Q20) */
    uint32_t ww, rng, nm;
    int32_t ap[4], apx[4];                               /* WIRE's all-passes, TUBE's two: their last in / out */
    int32_t wlp, xlp, xhp, ph, sink;                     /* the loop's low-pass; the exciter's filters, its phase;
                                                          * GONG's sink (Q16 of an octave) */
    int32_t gap, cnt, amp;                               /* BALL: the gap to the next hit, the samples to it, its height */
    uint32_t kinc;                                       /* what the modes were set for */
    int32_t ksink;
    int16_t kp[8];
} rs_slot_t;
ENG_MEM_FITS("RESO", sizeof(rs_slot_t) * RS_POLY);

static rs_slot_t *rs_slot(track_t *t, voice_t *v)
{
    uint32_t i = (uint32_t)(v - t->v);
    if (t < &trk[0] || t >= &trk[NLIVE] || i >= RS_POLY)
        return 0;
    return (rs_slot_t *)ENG_MEM(t - trk) + i;
}

static void reso_note_on(track_t *t, voice_t *v)
{
    rs_slot_t *S = rs_slot(t, v);
    uint32_t i;
    if (!S)
        return;
    if (!v->env && !v->env_out) {                        /* from rest: a quiet body */
        memset(S->y1, 0, sizeof S->y1);
        memset(S->y2, 0, sizeof S->y2);
        memset(S->wg, 0, sizeof S->wg);
        for (i = 0; i < 4u; i++)
            S->ap[i] = S->apx[i] = 0;
        S->wlp = S->xlp = S->xhp = 0;
        S->rng = (uint32_t)rng() | 1u;
        S->kinc = 0;
    }
    S->gap = (int32_t)sx_expmap(t->p[P_E2], 1764u << 16, 4) >> 16;   /* BALL: 40 ms .. 0.64 s */
    S->cnt = 0;
    S->amp = 1 << 20;
    S->sink = t->p[P_E5] * 600;                          /* GONG: up to ~1.2 semitones sharp at the hit */
}

/* the modes for this block (SHEET GONG BOX) */
static __attribute__((noinline)) void reso_coef(rs_slot_t *S, const track_t *t, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t res = (uint32_t)clamp(p[P_E1], 0, 2), k, nm = 0;
    int32_t stretch = res == RR_GONG ? 0 : (p[P_E5] - 40) * 500;   /* the ratios' exponent: -0.3 .. +0.67 (Q16) */
    int32_t t60 = (int32_t)sx_expmap(p[P_E4], 1323u, 9) >> (res == RR_BOX ? 2 : 0);   /* 30 ms .. 15 s (BOX: 1/4) */
    uint32_t a0 = (uint32_t)(((uint64_t)sx_inv40((uint32_t)(t60 > 300 ? t60 : 300)) * 452704u) >> 24);
    uint32_t pos = (uint32_t)p[P_E6] * 8454660u;
    for (k = 0; k < RS_NM; k++) {
        int32_t lg = RS_LOG2[res][k], r, lv;
        uint32_t f, a, cs, sn;
        uint64_t f64;
        lg += (int32_t)(((int64_t)lg * stretch) >> 16) + S->sink;
        f64 = ((uint64_t)m->inc * px_exp2(lg)) >> 16;
        if (f64 >= 0x73333333u) {                        /* at or above 0.45 FS: silent */
            S->a1[k] = S->r2[k] = S->gi[k] = 0;
            S->y1[k] = S->y2[k] = 0;
            continue;
        }
        f = (uint32_t)f64;
        a = a0 + a0 * k / 3u;                            /* the upper modes die sooner */
        if (a > 0x04000000u)
            a = 0x04000000u;
        r = (1 << 30) - (int32_t)(a >> 2) + (int32_t)(((uint64_t)(a >> 2) * (a >> 2)) >> 31);
        cs = (uint32_t)px_sin(f + 0x40000000u);
        sn = (uint32_t)px_sin(f);
        lv = px_sin((uint32_t)((k + 1u) * pos)) >> 15;
        lv = (lv < 0 ? -lv : lv) + 2500;
        lv = lv * 6 / (int32_t)(6 + k);
        S->a1[k] = px_m(r, (int32_t)cs, 30);
        S->r2[k] = px_m(r, r, 30);
        S->gi[k] = px_m((int32_t)sn, lv, 15);
        nm = k + 1u;
    }
    S->nm = nm;
}

/* one sample of the exciter (Q20), amp the envelope (Q15) */
static inline int32_t reso_excite(rs_slot_t *S, uint32_t ex, int32_t rate, int32_t grit, int32_t amp)
{
    int32_t x = 0, nz;
    switch (ex) {
    case RX_FRIC: {                                      /* stick (a ramp) and slip (a drop), jittered */
        S->ph += rate + (px_m(sx_noise(&S->rng), rate, 20) * grit >> 8);
        if (S->ph >= 1 << 20) {
            S->ph -= 1 << 20;
            S->xhp = sx_noise(&S->rng) >> 2;             /* (each slip a little different) */
        }
        x = px_m(S->ph - (1 << 19) + S->xhp, amp, 15);
        break;
    }
    case RX_BALL:                                        /* hits ever closer, ever softer */
        if (S->cnt <= 0 && S->gap > 40) {
            x = S->amp;
            S->cnt = S->gap;
            S->gap = (S->gap * 13) >> 4;                 /* x 0.81 */
            S->amp = (S->amp * 7) >> 3;
        }
        S->cnt--;
        break;
    case RX_SHAKE:                                       /* hits at random */
        if ((int32_t)(px_rand(&S->rng) >> 16) < rate)
            x = px_m(sx_noise(&S->rng), amp, 14);
        break;
    case RX_SPARK:                                       /* crackling bursts */
        if ((int32_t)(px_rand(&S->rng) >> 18) < (rate >> 2) + 1)
            S->cnt = 20 + (int32_t)(px_rand(&S->rng) >> 26) * 8;
        if (S->cnt > 0) {
            S->cnt--;
            nz = sx_noise(&S->rng);
            x = px_m(nz - S->xhp, amp, 14);              /* (high-passed: GRIT's harshness below) */
            S->xhp = nz;
        }
        break;
    default:                                             /* SCRAPE: noise over ridges */
        S->ph += rate;
        nz = sx_noise(&S->rng);
        x = px_m(nz, (S->ph & (1 << 19)) ? amp : amp >> 3, 15);
        break;
    }
    /* GRIT: the exciter's low-pass, dull .. raw */
    S->xlp += px_m(x - S->xlp, 1200 + grit * 250, 15);
    return S->xlp;
}

static void reso_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    rs_slot_t *S = rs_slot(t, v);
    uint32_t ex = (uint32_t)clamp(p[P_E0], 0, RX_N - 1), res = (uint32_t)clamp(p[P_E1], 0, RR_N - 1), i, k;
    int32_t grit = p[P_E3], dirt = p[P_E7], rate;
    if (!S)
        return;
    if (n > CTL)
        n = CTL;
    /* RATE: FRIC / SCRAPE a frequency (~20 .. 800 Hz as a Q20 turn a sample), SHAKE / SPARK a density */
    rate = ex == RX_FRIC || ex == RX_SCRAPE ? (int32_t)(sx_hz_inc(sx_expmap(p[P_E2], 20u << 16, 5)) >> 12)
                                            : 2 + p[P_E2] * p[P_E2] / 24;
    if (res == RR_GONG && S->sink > 0)                   /* GONG: settling, its modes sink */
        S->sink -= 1 + (S->sink >> 7);
    if (res <= RR_BOX) {                                 /* the modal bodies */
        uint32_t ch = m->inc != S->kinc || S->sink != S->ksink;
        for (k = 0; k < 8u; k++)
            ch |= (uint32_t)(p[P_E0 + k] != S->kp[k]);
        if (ch) {
            reso_coef(S, t, m);
            S->kinc = m->inc;
            S->ksink = S->sink;
            for (k = 0; k < 8u; k++)
                S->kp[k] = p[P_E0 + k];
        }
        for (i = 0; i < n; i++) {
            int32_t x = reso_excite(S, ex, rate, grit, amp_at(m, i)) << 3, s = 0, y;   /* (Q23: the modes' scale) */
            for (k = 0; k < S->nm; k++) {
                y = (int32_t)((((int64_t)S->a1[k] * S->y1[k] << 1) - (int64_t)S->r2[k] * S->y2[k] +
                               (int64_t)S->gi[k] * x) >> 30);
                if (dirt > 64)                           /* DIRT: the body driven, it buzzes */
                    y = sx_sat(y);
                S->y2[k] = S->y1[k];
                S->y1[k] = y;
                s += y >> 3;
            }
            s += px_m(x, dirt * 64, 15) >> 5;            /* (the exciter heard through) */
            s = clamp(soft_knee(s >> 5, 24000), -32767, 32767);   /* Q20 / 8 / 32 -> Q15 */
            if (MF_E8(p))                                /* DRIV (MONOFM's ninth): the sound into a tanh, up to ~6 x */
                s = softclip(s + px_m(s, MF_E8(p) * 1290, 15)) - (softclip(s + px_m(s, MF_E8(p) * 1290, 15)) >> 2);
            out[i] += voice_amp(s, m, i);
        }
        return;
    }
    {                                                    /* TUBE / WIRE: a waveguide */
        uint32_t d16 = sx_period(m->inc), w = S->ww, tap;
        int32_t fb, klp = 9000 + grit * 150, c = 6000 + p[P_E5] * 180;   /* the all-passes' coefficient (Q15) */
        int32_t t60 = (int32_t)sx_expmap(p[P_E4], 1323u, 9);
        if (res == RR_TUBE)
            d16 >>= 1;                                   /* closed at one end: half the line, inverted */
        d16 -= 2u << 16;                                 /* (the all-passes' and the low-pass' delay) */
        if (d16 < (4u << 16))
            d16 = 4u << 16;
        if (d16 > ((RS_WG - 4u) << 16))
            d16 = (RS_WG - 4u) << 16;
        /* the loop's gain for a T60 of DAMP: 1 - 6.9 x period / T60 (Q15) */
        fb = 32767 - (int32_t)((d16 >> 16) * 226361u / (uint32_t)(t60 > 1 ? t60 : 1));   /* (32-bit: no 64 / 64 divide) */
        fb = clamp(fb, 16000, 32700);
        tap = 1u + (d16 >> 16) * (uint32_t)p[P_E6] / 128u;
        for (i = 0; i < n; i++) {
            int32_t x = reso_excite(S, ex, rate, grit, amp_at(m, i)), y = sx_read(S->wg, RS_WMASK, w, d16), z, s;
            uint32_t a;
            for (a = 0; a < (res == RR_WIRE ? 4u : 2u); a++) {   /* the dispersion */
                z = px_m(c, y - S->ap[a], 15) + S->apx[a];
                S->apx[a] = y;
                S->ap[a] = z;
                y = z;
            }
            S->wlp += px_m(y - S->wlp, klp, 15);
            y = px_m(S->wlp, fb, 15);
            if (res == RR_TUBE)
                y = -y;
            if (dirt > 40)
                y = sx_sat(y + px_m(y, dirt * 96, 15));
            S->wg[w & RS_WMASK] = y + (ex == RX_FRIC || ex == RX_SCRAPE ? x : x << 2);   /* (hits: louder; 26.10: rubbing
                                                          * was x / 4, 12 dB under) */
            w++;
            s = (S->wg[(w - tap) & RS_WMASK] + y) >> 1;
            s += px_m(x, dirt * 64, 15) >> 2;
            s = clamp(soft_knee(s >> 5, 24000), -32767, 32767);
            if (MF_E8(p))
                s = softclip(s + px_m(s, MF_E8(p) * 1290, 15)) - (softclip(s + px_m(s, MF_E8(p) * 1290, 15)) >> 2);
            out[i] += voice_amp(s, m, i);
        }
        S->ww = w;
    }
}

#define STRANGE_CAT_RESO "FFFFFPFFPF"
static const preset_t RESO_PRESETS[] = {
    /* name, {EXCT, RES, RATE, GRIT, DAMP, STIFF, POS, DIRT}, {A D S R}, fenv, mono */
    {"WET GLASS", {RX_FRIC, RR_TUBE, 50, 40, 90, 30, 40, 10}, {60, 64, 127, 90}, 0, 0, FX(0, 0, 10, 70)},
    {"BOUNCING TIN", {RX_BALL, RR_SHEET, 80, 90, 70, 60, 30, 20}, {0, 64, 127, 100}, 0, 0, FX(0, 0, 20, 60)},
    {"BOLT BOX", {RX_SHAKE, RR_BOX, 70, 80, 40, 50, 50, 40}, {10, 64, 127, 60}, 0, 0, FX(0, 0, 0, 40)},
    {"TESLA GONG", {RX_SPARK, RR_GONG, 50, 100, 110, 70, 40, 30}, {20, 64, 127, 110}, 0, 0, FX(0, 0, 20, 80)},
    {"CABLE SCRAPE", {RX_SCRAPE, RR_WIRE, 60, 70, 100, 90, 30, 30}, {40, 64, 127, 100}, 0, 0, FX(0, 0, 10, 70)},
    {"DOOR CREAK", {RX_FRIC, RR_BOX, 20, 30, 50, 30, 60, 50}, {50, 64, 127, 60}, 0, 0, FX(0, 0, 0, 40)},
    {"RAIN ON TIN", {RX_SHAKE, RR_SHEET, 50, 110, 80, 70, 20, 0}, {80, 64, 127, 110}, 0, 0, FX(0, 0, 30, 100)},
    {"SINKING GONG", {RX_BALL, RR_GONG, 100, 60, 120, 110, 40, 10}, {0, 64, 127, 127}, 0, 0, FX(0, 0, 0, 90)},
    {"SPRING WIRE", {RX_BALL, RR_WIRE, 60, 80, 110, 127, 20, 20}, {0, 64, 127, 110}, 0, 0, FX(0, 0, 20, 80)},
    {"ARC FIELD", {RX_SPARK, RR_TUBE, 90, 120, 100, 50, 50, 70}, {90, 64, 127, 120}, 0, 0, FX(10, 0, 30, 100)},
};

static const engine_t ENG_RESO = {
    .name = "RESO",
    .page_title = {"EXCITE", "BODY"},
    .edit = {
        {"EXCT", F_ENUM, 0, RX_N - 1, 0, N_RS_EXCT, 0},
        {"RES", F_ENUM, 0, RR_N - 1, 0, N_RS_RES, 0},
        {"RATE", F_PCT, 0, 127, 60, 0, 0},
        {"GRIT", F_PCT, 0, 127, 64, 0, 0},
        {"DAMP", F_PCT, 0, 127, 90, 0, 0},
        {"STIFF", F_PCT, 0, 127, 50, 0, 0},
        {"POS", F_PCT, 0, 127, 40, 0, 0},
        {"DIRT", F_PCT, 0, 127, 20, 0, 0},
    },
    .presets = RESO_PRESETS,
    .npresets = NELEM(RESO_PRESETS),
    .note_on = reso_note_on,
    .render = reso_render,
    .knob = {P_E2, P_E3, P_E4, P_E5},
    .poly = RS_POLY,
    .mem = sizeof(rs_slot_t) * RS_POLY,
    .xyd = {{XY_E(2), XY_E(3), XY_E(7)}, {XY_E(4), XY_E(5), XY_E(6)}},   /* X: RATE GRIT DIRT (the exciting), Y: DAMP STIFF POS (the body) */
    .xya = {{40, 40, 30}, {35, 40, 40}},
#if FELUCCA_MONOFM
    .e8 = {"DRIV", F_PCT, 0, 127, 0, 0, 0},
#endif
};
