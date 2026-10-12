/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Tim (646timur-cell), on Felucca by Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* MODAL (STRANGE): a struck, rubbed or blown body as 24 resonant modes (two-pole resonators; PHYS's MODAL has 12).
 *   MATR   the body: BAR, MRMB (a tuned bar), BELL, GLASS, PLATE, DRUM (a membrane), BOWL (a singing bowl's ring),
 *          HARM (a string's harmonics): the partials' ratios (strange_dsp.c MODAL_LOG2)
 *   STRC   bends them: 64 as the material is, lower compresses (towards harmonic), higher stretches (more metallic)
 *   BRIT   how long the high modes ring against the low ones (dull wood .. bright metal; ENV / LFO -> FLT move it)
 *   DAMP   how long the body rings, ~20 ms .. ~25 s
 *   POS    where it is struck: the modes' levels follow sin(pi k pos) (the middle: the odd ones only)
 *   EXCT   MALLT a mallet (a half-sine contact, HARD: soft rubber .. hard plastic), STICK a click and a burst of
 *          noise, BOW a rubbing exciter (noise that follows the body's own swing: a bowl rubbed with a stick, a
 *          bowed cymbal: the track's envelope is the bowing), BLOW breath (low-passed noise; HARD its brightness)
 *   BEAT   pairs the modes: the upper 12 become a copy of the lower 12 a few cents apart (a bell's, a bowl's
 *          doublets: they beat and warble). 0: 24 different modes
 * Each mode: y = 2 r cos(w) y1 - r^2 y2 + sin(w) gain x; its coefficients once a block (64-bit: r near 1). A mode at or
 * above 0.45 FS is left out; while both voices ring, each runs its lowest 16 (the cost of 32 modes a part). The voice's
 * amplitude is the ADSR as PHYS's (the presets hold SUS 127). Poly 2. */

#define MD_POLY 2
enum { MX_MALLET, MX_STICK, MX_BOW, MX_BLOW, MX_N };
static const char *const N_MD_MATR[] = {"BAR", "MRMB", "BELL", "GLASS", "PLATE", "DRUM", "BOWL", "HARM"};
static const char *const N_MD_EXCT[] = {"MALLT", "STICK", "BOW", "BLOW"};

typedef struct {
    int32_t y1[MD_NMODE], y2[MD_NMODE];                  /* the modes (Q24) */
    int32_t a1[MD_NMODE], r2[MD_NMODE], gi[MD_NMODE];    /* 2 r cos w (Q29), r^2 (Q30), sin w x level (Q30) */
    uint32_t nm, rng;                                    /* modes in use, random numbers */
    uint32_t kinc;                                       /* what the coefficients were made for (modal_coef): the */
    int32_t kcut;                                        /* pitch, ENV / LFO -> FLT, the E values (0: none yet) */
    int16_t kp[8];
    int32_t xn, xl, xlp, last;                           /* the exciter: samples left of a hit, its length, its
                                                          * low-pass; the body's last output (BOW) */
} md_slot_t;
ENG_MEM_FITS("MODAL", sizeof(md_slot_t) * MD_POLY);

static md_slot_t *md_slot(track_t *t, voice_t *v)
{
    uint32_t i = (uint32_t)(v - t->v);
    if (t < &trk[0] || t >= &trk[NLIVE] || i >= MD_POLY)
        return 0;
    return (md_slot_t *)ENG_MEM(t - trk) + i;
}

static void modal_note_on(track_t *t, voice_t *v)
{
    md_slot_t *S = md_slot(t, v);
    uint32_t i;
    if (!S)
        return;
    if (!v->env && !v->env_out) {                        /* from rest */
        for (i = 0; i < MD_NMODE; i++)
            S->y1[i] = S->y2[i] = 0;
        S->kinc = 0;
        S->xlp = S->last = 0;
        S->rng = (uint32_t)rng() | 1u;
    }
    S->xn = -1;                                          /* a hit at the render (a retrigger strikes it again) */
}

/* the coefficients of the modes for this block */
static __attribute__((noinline)) void modal_coef(md_slot_t *S, const track_t *t, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t mat = (uint32_t)clamp(p[P_E0], 0, MD_NMAT - 1), k, nm = 0, beat = (uint32_t)p[P_E7];
    int32_t strc = (p[P_E1] - 64) * 400;                 /* the exponent's change: -0.39 .. +0.39 (Q16) */
    int32_t brit = clamp(p[P_E2] * 4 + (m->cutoff >> 6), 0, 600);   /* 0 .. ~1.17 (Q9): the high modes' decay */
    uint32_t pos = (uint32_t)p[P_E4] * 8454660u;         /* pos / 2: 0 .. 1/4 of a turn (Q32) */
    int32_t t60 = (int32_t)sx_expmap(p[P_E3], 882u, 10);   /* 20 ms .. 20 s, samples */
    /* a0: the fundamental's decay per sample, 6.9078 / T60 (Q32) */
    uint32_t a0 = (uint32_t)(((uint64_t)sx_inv40((uint32_t)t60) * 452704u) >> 24);   /* 2^40 / T60 x 6.9078 (Q16) */
    for (k = 0; k < MD_NMODE; k++) {
        uint32_t src = beat && k >= 12u ? k - 12u : k, f, a, cs, sn;
        int32_t lg = MODAL_LOG2[mat][src], r, lv;
        uint64_t f64;
        lg += (int32_t)(((int64_t)lg * strc) >> 16);
        if (beat && k >= 12u)                            /* the doublet: up to ~35 cents above (2^(cents/1200)) */
            lg += (int32_t)beat * 15;
        f64 = lg > (14 << 16) ? ~0ull : ((uint64_t)m->inc * px_exp2(lg)) >> 16;
        if (f64 >= 0x73333333u) {                        /* at or above 0.45 FS: silent */
            S->a1[k] = S->r2[k] = S->gi[k] = 0;
            S->y1[k] = S->y2[k] = 0;
            continue;
        }
        f = (uint32_t)f64;
        /* the mode's decay: a0 x ratio^(1.17 - brit) (high modes faster as BRIT goes down), at most 1/64 a sample */
        a = (uint32_t)(((uint64_t)a0 * px_exp2((int32_t)(((int64_t)lg * (600 - brit)) >> 9))) >> 16);
        if (a > 0x04000000u || a < a0)
            a = a < a0 ? a0 : 0x04000000u;
        r = (1 << 30) - (int32_t)(a >> 2) + (int32_t)(((uint64_t)(a >> 2) * (a >> 2)) >> 31);   /* e^-a ~ 1 - a + a^2/2 */
        cs = (uint32_t)px_sin(f + 0x40000000u);
        sn = (uint32_t)px_sin(f);
        lv = px_sin((uint32_t)((src + 1u) * pos)) >> 15;   /* the position: sin(pi (k + 1) pos), Q15 */
        lv = (lv < 0 ? -lv : lv) + 1500;
        lv = (lv * 28) / (int32_t)(28 + src * 3);        /* a gentle tilt: the upper modes a little quieter */
        if (beat && k >= 12u)
            lv >>= 1;                                    /* (the pair shares the mode's level) */
        else if (beat && k < 12u)
            lv -= lv >> 2;
        S->a1[k] = px_m(r, (int32_t)cs, 30);             /* r cos w (Q30) = 2 r cos w (Q29) */
        S->r2[k] = px_m(r, r, 30);
        S->gi[k] = px_m((int32_t)sn, lv, 15);
        nm = k + 1u;
    }
    S->nm = nm;                                          /* (the modes above the last one sounding: not run) */
}

static void modal_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    md_slot_t *S = md_slot(t, v);
    uint32_t exct = (uint32_t)clamp(p[P_E5], 0, MX_N - 1), i, k, nm;
    int32_t hard = p[P_E6], kx, xn, xl, xlp, last, x, s, xg;
    uint32_t xd;
    if (!S)
        return;
    if (n > CTL)
        n = CTL;
    {                                                    /* the coefficients: again only when what they follow moved */
        int32_t cut = m->cutoff >> 6;
        uint32_t ch = m->inc != S->kinc || cut != S->kcut;
        for (k = 0; k < 8u; k++)
            ch |= (uint32_t)(p[P_E0 + k] != S->kp[k]);
        if (ch) {
            modal_coef(S, t, m);
            S->kinc = m->inc;
            S->kcut = cut;
            for (k = 0; k < 8u; k++)
                S->kp[k] = p[P_E0 + k];
        }
    }
    nm = S->nm;
    if (nm > 16u && t->v[0].active && t->v[1].active)    /* two notes ringing: 16 modes each (the CPU of 32) */
        nm = 16u;
    if (S->xn < 0) {                                     /* a hit: its length (MALLT: ~8 ms soft .. 0.2 ms hard) */
        S->xl = exct == MX_MALLET ? 8 + (127 - hard) * (127 - hard) / 46 : exct == MX_STICK ? 90 : 0;
        S->xn = S->xl;
    }
    kx = 800 + hard * 250;                               /* the noise's low-pass (STICK, BOW, BLOW) */
    xd = S->xl > 0 ? 0x80000000u / (uint32_t)S->xl : 0;  /* a hit's half sine: its step (a turn / 2, Q32) */
    xg = S->xl > 0 ? (6 << 15) / S->xl : 0;              /* .. and its height: the same momentum over any contact */
    xn = S->xn;
    xl = S->xl;
    xlp = S->xlp;
    last = S->last;
    for (i = 0; i < n; i++) {
        int32_t amp = amp_at(m, i);
        x = 0;
        switch (exct) {
        case MX_MALLET:                                  /* a half sine over the contact */
            if (xn > 0) {
                x = px_m(px_sin((uint32_t)xn * xd) >> 6, xg, 15);
                xn--;
            }
            break;
        case MX_STICK:                                   /* a click, then 2 ms of noise falling away */
            if (xn > 0) {
                x = xn == xl ? 1 << 24 : px_m(sx_noise(&S->rng) << 3, xn * 364, 15);   /* (90 samples: 32768 / 90) */
                xn--;
            }
            xlp += px_m(x - xlp, kx, 15);
            x = xn == xl - 1 ? x : xlp;
            break;
        case MX_BOW: {                                   /* noise that leans on the body's own swing (stick-slip) */
            int32_t sg = last > 0 ? 1 << 19 : -(1 << 19), nz = sx_noise(&S->rng);
            xlp += px_m(nz + sg - xlp, kx, 15);          /* (the slip and the grain through HARD's low-pass) */
            x = px_m(xlp, amp, 15) >> 1;
            break;
        }
        default:                                         /* BLOW: breath */
            xlp += px_m((sx_noise(&S->rng) << 2) - xlp, kx, 15);
            x = px_m(xlp, amp, 15) >> 2;
            break;
        }
        s = 0;
        if (x) {                                         /* the modes, excited */
            for (k = 0; k < nm; k++) {
                int32_t y = (int32_t)((((int64_t)S->a1[k] * S->y1[k] << 1) - (int64_t)S->r2[k] * S->y2[k] +
                                       (int64_t)S->gi[k] * x) >> 30);
                S->y2[k] = S->y1[k];
                S->y1[k] = y;
                s += y >> 4;
            }
        } else {                                         /* .. ringing (a struck body after the hit) */
            for (k = 0; k < nm; k++) {
                int32_t y = (int32_t)((((int64_t)S->a1[k] * S->y1[k] << 1) - (int64_t)S->r2[k] * S->y2[k]) >> 30);
                S->y2[k] = S->y1[k];
                S->y1[k] = y;
                s += y >> 4;
            }
        }
        last = s;
        s = clamp(soft_knee(s >> 5, 24000), -32767, 32767);   /* Q24 / 16 / 32 -> Q15 */
        out[i] += voice_amp(s, m, i);
    }
    S->xn = xn;
    S->xlp = xlp;
    S->last = last;
}

#define STRANGE_CAT_MODAL "UUPPFUPK"
static const preset_t MODAL_PRESETS[] = {
    /* name, {MATR, STRC, BRIT, DAMP, POS, EXCT, HARD, BEAT}, {A D S R}, fenv, mono */
    {"MARIMBA", {1, 64, 30, 46, 20, MX_MALLET, 50, 0}, {0, 64, 127, 50}, 0, 0, FX(0, 0, 0, 30)},
    {"GLOCKEN", {0, 64, 100, 84, 12, MX_MALLET, 110, 0}, {0, 64, 127, 70}, 0, 0, FX(0, 0, 10, 50)},
    {"CHURCH BELL", {2, 64, 90, 110, 30, MX_MALLET, 90, 20}, {0, 64, 127, 100}, 0, 0, FX(0, 0, 0, 70)},
    {"SINGING BOWL", {6, 64, 70, 118, 26, MX_BOW, 40, 30}, {90, 64, 127, 110}, 0, 0, FX(0, 0, 0, 70)},
    {"BOWED CYMBAL", {4, 90, 110, 100, 40, MX_BOW, 100, 50}, {60, 64, 127, 90}, 0, 0, FX(0, 20, 0, 60)},
    {"TABLA", {5, 50, 40, 50, 50, MX_MALLET, 70, 0}, {0, 64, 127, 40}, 0, 0, FX(0, 0, 0, 30)},
    {"BLOWN GLASS", {3, 64, 80, 104, 34, MX_BLOW, 60, 10}, {70, 64, 127, 90}, 0, 0, FX(0, 0, 0, 70)},
    {"STEEL TONGUE", {7, 72, 60, 90, 24, MX_STICK, 80, 6}, {0, 64, 127, 80}, 0, 0, FX(0, 0, 10, 50)},
};

static const engine_t ENG_MODAL = {
    .name = "MODAL",
    .page_title = {"BODY", "EXCITE"},
    .edit = {
        {"MATR", F_ENUM, 0, MD_NMAT - 1, 0, N_MD_MATR, 0},
        {"STRC", F_PCT, 0, 127, 64, 0, 0},
        {"BRIT", F_PCT, 0, 127, 80, 0, 0},
        {"DAMP", F_PCT, 0, 127, 80, 0, 0},
        {"POS", F_PCT, 0, 127, 24, 0, 0},
        {"EXCT", F_ENUM, 0, MX_N - 1, 0, N_MD_EXCT, 0},
        {"HARD", F_PCT, 0, 127, 64, 0, 0},
        {"BEAT", F_PCT, 0, 127, 0, 0, 0},
    },
    .presets = MODAL_PRESETS,
    .npresets = NELEM(MODAL_PRESETS),
    .note_on = modal_note_on,
    .render = modal_render,
    .knob = {P_E0, P_E2, P_E3, P_E4},
    .poly = MD_POLY,
    .mem = sizeof(md_slot_t) * MD_POLY,
    .xyd = {{XY_E(1), XY_E(2), XY_E(6)}, {XY_E(3), XY_E(7), XY_E(4)}},   /* X: STRC BRIT HARD (the metal), Y: DAMP BEAT POS (the ring, the beating) */
    .xya = {{40, 40, 40}, {40, 50, 30}},
};
