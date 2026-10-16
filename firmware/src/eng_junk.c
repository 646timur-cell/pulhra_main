/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Tim (646timur-cell), on Felucca by Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* JUNK (STRANGE): a box of junk and springs. A spring is a delay with a chain of all-passes in its loop: the highs run
 * slower than the lows, so a hit comes back as a falling chirp, the spring tank's "boing", again and again. The junk:
 * three loose things (a screw, a can, a washer: three inharmonic modes each) that rattle when the box is shaken, and
 * ring into the spring.
 *   TYPE   TANK    a spring reverb's tank kicked: one long spring, a little junk
 *          COIL    a slinky: a long chirp (twice the all-passes), slow
 *          BOX     a box of junk: the loose things first, a short spring behind them
 *          RATTLE  shaken all the time: springs and junk together while the note lasts
 *   SHAKE  how hard and how often it is shaken (the note's hit, then rattles at random while it is held)
 *   SIZE   the spring's length (the note tunes it, SIZE moves it down up to 2 octaves)
 *   CHIRP  the all-passes' coefficient: a soft wobble .. a laser zap
 *   DAMP   how long the spring rings; TONE its brightness; JUNK how much of the loose things; DRIVE the loop driven
 *          (a tanh: the spring rattles against itself, a fuzzed boing)
 * Poly 2, the springs in the part's engine region (jk_slot_t). */

#define JK_POLY 2
#define JK_LEN 4096u                                     /* the spring's line: down to ~11 Hz */
#define JK_MASK (JK_LEN - 1u)
#define JK_NAP 16                                        /* all-passes (COIL all of them, the rest 8) */
enum { JT_TANK, JT_COIL, JT_BOX, JT_RATTLE, JT_N };
static const char *const N_JK_TYPE[] = {"TANK", "COIL", "BOX", "RATTLE"};
/* the loose things' modes, log2 of their ratio to the note (Q16): a screw, a can, a washer */
static const int32_t JK_MODE[9] = {98304, 150733, 189399, 52429, 112460, 141558, 170394, 222822, 258212};

typedef struct {
    int16_t dl[JK_LEN];                                  /* the spring (Q15) */
    uint32_t w, rng;
    int32_t ap[JK_NAP], apx[JK_NAP];                     /* the all-passes: last out, last in (Q15) */
    int32_t lp;                                          /* the loop's low-pass */
    int32_t y1[9], y2[9], a1[9], r2[9];                  /* the junk's modes (Q24; coefficients Q30) */
    uint32_t kinc;
    int16_t kp[8];
    int32_t kick;                                        /* the note's hit, samples left */
} jk_slot_t;
ENG_MEM_FITS("JUNK", sizeof(jk_slot_t) * JK_POLY);

static jk_slot_t *jk_slot(track_t *t, voice_t *v)
{
    uint32_t i = (uint32_t)(v - t->v);
    if (t < &trk[0] || t >= &trk[NLIVE] || i >= JK_POLY)
        return 0;
    return (jk_slot_t *)ENG_MEM(t - trk) + i;
}

static void junk_note_on(track_t *t, voice_t *v)
{
    jk_slot_t *S = jk_slot(t, v);
    if (!S)
        return;
    if (!v->env && !v->env_out) {
        memset(S, 0, sizeof *S);
        S->rng = (uint32_t)rng() | 1u;
    }
    S->kick = 400;                                       /* the hit: ~9 ms of noise, falling away */
}

/* the junk's modes: three per thing, short (~60 .. 250 ms), above the note */
static __attribute__((noinline)) void junk_coef(jk_slot_t *S, const vmod_t *m)
{
    uint32_t k;
    for (k = 0; k < 9u; k++) {
        uint64_t f64 = ((uint64_t)m->inc * px_exp2(JK_MODE[k])) >> 16;
        int32_t r = (1 << 30) - (int32_t)(100000u + k * 40000u);   /* decay: ~240 ms .. ~55 ms down the list */
        uint32_t f = f64 >= 0x73333333u ? 0x73333333u : (uint32_t)f64;
        S->a1[k] = px_m(r, px_sin(f + 0x40000000u), 30);
        S->r2[k] = px_m(r, r, 30);
    }
}

static void junk_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    jk_slot_t *S = jk_slot(t, v);
    uint32_t type = (uint32_t)clamp(p[P_E0], 0, JT_N - 1), i, k, nap = type == JT_COIL ? JK_NAP : 8u, d16, w;
    int32_t shake = p[P_E1], chirp = 16000 + p[P_E3] * 120, fb, klp = 4000 + p[P_E5] * 220, junk = p[P_E6];
    int32_t drive = p[P_E7], t60, cont = type == JT_RATTLE ? shake : shake >> 5;
    if (!S)
        return;
    if (n > CTL)
        n = CTL;
    {
        uint32_t ch = m->inc != S->kinc;
        for (k = 0; k < 8u; k++)
            ch |= (uint32_t)(p[P_E0 + k] != S->kp[k]);
        if (ch) {
            junk_coef(S, m);
            S->kinc = m->inc;
            for (k = 0; k < 8u; k++)
                S->kp[k] = p[P_E0 + k];
        }
    }
    /* the spring's line: a period of the note, SIZE up to 2 octaves longer, the all-passes' delay taken off */
    d16 = sx_period(m->inc);
    d16 = (uint32_t)(((uint64_t)d16 * px_exp2(p[P_E2] * 1032)) >> 16);   /* x 2^(SIZE / 63.5) */
    if (type == JT_COIL)
        d16 <<= 1;
    if (type == JT_BOX)
        d16 >>= 1;
    if (d16 > ((JK_LEN - 4u) << 16))
        d16 = (JK_LEN - 4u) << 16;
    if (d16 < (8u << 16))
        d16 = 8u << 16;
    t60 = (int32_t)sx_expmap(p[P_E4], 2205u, 8);         /* 50 ms .. 13 s */
    fb = 32767 - (int32_t)((d16 >> 16) * 226361u / (uint32_t)t60);   /* (32-bit: no 64 / 64 divide) */
    fb = clamp(fb, 12000, 32600);
    if (MF_E8(p) && !v->gate)                            /* MUTE (MONOFM's ninth): let go, the spring held still */
        fb = mulq15(fb, 32767 - MF_E8(p) * 200);
    w = S->w;
    for (i = 0; i < n; i++) {
        int32_t amp = amp_at(m, i), x = 0, j = 0, y, z;
        uint32_t a;
        if (S->kick > 0) {                               /* the note's hit */
            x = px_m(px_m(sx_noise(&S->rng) >> 5, 8000 + shake * 194, 15), S->kick * 82, 15);
            S->kick--;
        }
        if ((int32_t)(px_rand(&S->rng) >> 18) < cont)    /* rattles while it lasts (RATTLE: all the time) */
            x += px_m(sx_noise(&S->rng) >> 5, amp, 15);
        if (junk) {                                      /* the loose things, struck by the same hits */
            int32_t jx = x << 4;
            for (k = 0; k < 9u; k++) {
                int32_t yy = (int32_t)((((int64_t)S->a1[k] * S->y1[k] << 1) - (int64_t)S->r2[k] * S->y2[k]) >> 30) + jx;
                S->y2[k] = S->y1[k];
                S->y1[k] = clamp(yy, -(1 << 26), 1 << 26);
                j += yy >> 10;
            }
            j = px_m(j, junk * 258, 15);                 /* (Q20 -> ~Q15 at JUNK 127) */
        }
        /* the spring: the line, the all-passes (the dispersion), the low-pass, the loss, the drive */
        y = S->dl[(w - (d16 >> 16)) & JK_MASK];
        for (a = 0; a < nap; a++) {
            z = px_m(chirp, y - S->ap[a], 15) + S->apx[a];
            S->apx[a] = y;
            S->ap[a] = z;
            y = z;
        }
        S->lp += px_m(y - S->lp, klp, 15);
        y = px_m(S->lp, fb, 15);
        if (drive)
            y = softclip(y + px_m(y, drive * 160, 15));
        S->dl[w & JK_MASK] = (int16_t)clamp(y + x + (j >> 2), -32767, 32767);
        w++;
        z = (type == JT_BOX ? (y >> 1) + j : y + (y >> 1) + (j >> 1));
        out[i] += voice_amp(clamp(soft_knee(z, 24000), -32767, 32767), m, i);
    }
    S->w = w;
}

#define STRANGE_CAT_JUNK "FFFFFFFFFF"
static const preset_t JUNK_PRESETS[] = {
    /* name, {TYPE, SHAKE, SIZE, CHIRP, DAMP, TONE, JUNK, DRIVE}, {A D S R}, fenv, mono */
    {"SPRING TANK", {JT_TANK, 80, 40, 70, 90, 70, 20, 0}, {0, 64, 127, 100}, 0, 0, FX(0, 0, 0, 40)},
    {"SLINKY", {JT_COIL, 70, 30, 100, 100, 60, 0, 0}, {0, 64, 127, 110}, 0, 0, FX(0, 0, 10, 50)},
    {"JUNK DRAWER", {JT_BOX, 90, 20, 50, 50, 90, 110, 20}, {0, 64, 127, 70}, 0, 0, FX(0, 0, 0, 30)},
    {"SHAKEN BOX", {JT_RATTLE, 60, 30, 60, 70, 80, 90, 10}, {10, 64, 127, 80}, 0, 0, FX(0, 0, 10, 50)},
    {"LASER BOING", {JT_TANK, 100, 60, 127, 100, 100, 0, 30}, {0, 64, 127, 100}, 0, 0, FX(0, 0, 20, 40)},
    {"RUSTY COIL", {JT_COIL, 90, 50, 90, 80, 40, 40, 70}, {0, 64, 127, 100}, 0, 0, FX(20, 0, 10, 50)},
    {"SCRAP METAL", {JT_BOX, 110, 10, 40, 40, 110, 127, 50}, {0, 64, 127, 60}, 0, 0, FX(10, 0, 0, 30)},
    {"TANK DRONE", {JT_RATTLE, 30, 80, 80, 120, 50, 30, 40}, {100, 64, 127, 120}, 0, 0, FX(0, 20, 20, 100)},
    {"CHAIN RAIN", {JT_RATTLE, 90, 10, 50, 60, 100, 100, 0}, {60, 64, 127, 110}, 0, 0, FX(0, 0, 30, 90)},
    {"BROKEN AMP", {JT_TANK, 70, 50, 90, 110, 60, 20, 110}, {0, 64, 127, 110}, 0, 0, FX(30, 0, 0, 50)},
};

static const engine_t ENG_JUNK = {
    .name = "JUNK",
    .page_title = {"SPRING", "JUNK"},
    .edit = {
        {"TYPE", F_ENUM, 0, JT_N - 1, 0, N_JK_TYPE, 0},
        {"SHAKE", F_PCT, 0, 127, 80, 0, 0},
        {"SIZE", F_PCT, 0, 127, 40, 0, 0},
        {"CHIRP", F_PCT, 0, 127, 70, 0, 0},
        {"DAMP", F_PCT, 0, 127, 90, 0, 0},
        {"TONE", F_PCT, 0, 127, 70, 0, 0},
        {"JUNK", F_PCT, 0, 127, 30, 0, 0},
        {"DRIVE", F_PCT, 0, 127, 0, 0, 0},
    },
    .presets = JUNK_PRESETS,
    .npresets = NELEM(JUNK_PRESETS),
    .note_on = junk_note_on,
    .render = junk_render,
    .knob = {P_E1, P_E3, P_E4, P_E6},
    .poly = JK_POLY,
    .mem = sizeof(jk_slot_t) * JK_POLY,
    .xyd = {{XY_E(1), XY_E(6), XY_E(7)}, {XY_E(3), XY_E(2), XY_E(4)}},   /* X: SHAKE JUNK DRIVE (the rattle), Y: CHIRP SIZE DAMP (the spring) */
    .xya = {{40, 40, 30}, {40, 30, 30}},
#if FELUCCA_MONOFM
    .e8 = {"MUTE", F_PCT, 0, 127, 0, 0, 0},
#endif
};
