/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Tim (646timur-cell), on Felucca by Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* STRING (STRANGE): a bidirectional digital waveguide string (J. O. Smith; the topology of STK's Bowed, P. Cook and
 * G. Scavone). Two delay lines carry the velocity waves either side of the point where the string is played: the
 * NECK line from there to the nut (a fixed end: it reflects inverted) and the BRIDGE line from there to the bridge,
 * whose reflection loses energy through a low-pass (BRIT), a gain (DAMP) and an all-pass (STIF: a stiff string's
 * partials run sharp). What the bridge receives is the output, through a body (three resonances: BODY, BMIX).
 * MODE plays it three ways:
 *   PLUCK   a burst of noise, half a period long, through a low-pass (PRES: the pick, soft .. hard)
 *   STRIKE  a raised-cosine hammer (PRES: felt .. steel, ~3 ms .. 0.1 ms)
 *   BOW     a bow: the friction of STK's BowTable between the bow's velocity (the track's envelope x velocity: a slow
 *           ATK is a bow stroke) and the string's, PRES the bow's pressure (light: flautando, whistling harmonics;
 *           heavy: a scratchy, pressed tone). The bow lifts as the envelope ends: the string rings on, damped by DAMP
 * POS is where it is played, from the bridge (sul ponticello, glassy) to a third of the string (sul tasto, round).
 * BODY SITAR adds the jawari: the bridge's reflection clipped on one side, a buzzing bright ring.
 * Lines of Q20 (phys_dsp.c's format); 2048 + 1024 samples (the lowest note ~22 Hz); poly 2 per part. */

#define SR_NLEN 2048u
#define SR_BLEN 1024u
#define SR_POLY 2
enum { SR_PLUCK, SR_STRIKE, SR_BOW, SR_NMODE };
enum { SB_OFF, SB_GUITAR, SB_VIOLIN, SB_CELLO, SB_SITAR, SB_N };
static const char *const N_SR_MODE[] = {"PLUCK", "STRIKE", "BOW"};
static const char *const N_SR_BODY[] = {"OFF", "GUITR", "VIOLN", "CELLO", "SITAR"};
/* the body's three resonances (CUTOFF_HZ's scale, 0..127) and their levels (Q15): a guitar's air and top, a violin's
 * air, main wood and bridge hill, a cello's, a sitar's gourd */
static const uint8_t SR_BODY_F[SB_N][3] = {{0, 0, 0}, {24, 38, 52}, {45, 55, 92}, {26, 41, 79}, {33, 58, 84}};
static const int16_t SR_BODY_G[SB_N][3] = {{0, 0, 0}, {26000, 20000, 12000}, {16000, 26000, 14000},
                                           {26000, 22000, 10000}, {22000, 16000, 12000}};

typedef struct {
    int32_t neck[SR_NLEN];
    int32_t bridge[SR_BLEN];
    uint32_t w, rng;
    int32_t lp, apx, apy;                                /* the bridge's low-pass, its all-pass */
    int32_t b[6];                                        /* the body's three SVFs */
    int32_t dcx, dcy;                                    /* the output's DC block */
    int32_t xn, xl, xlp;                                 /* the exciter: samples left, its length, its low-pass */
    uint8_t mode;
} sr_slot_t;
ENG_MEM_FITS("STRING", sizeof(sr_slot_t) * SR_POLY);

static sr_slot_t *sr_slot(track_t *t, voice_t *v)
{
    uint32_t i = (uint32_t)(v - t->v);
    if (t < &trk[0] || t >= &trk[NLIVE] || i >= SR_POLY)
        return 0;
    return (sr_slot_t *)ENG_MEM(t - trk) + i;
}

static void string_note_on(track_t *t, voice_t *v)
{
    sr_slot_t *S = sr_slot(t, v);
    uint32_t i;
    if (!S)
        return;
    if (!v->env && !v->env_out) {                        /* from rest: a still string */
        for (i = 0; i < SR_NLEN; i++)
            S->neck[i] = 0;
        for (i = 0; i < SR_BLEN; i++)
            S->bridge[i] = 0;
        S->lp = S->apx = S->apy = S->dcx = S->dcy = S->xlp = 0;
        for (i = 0; i < 6u; i++)
            S->b[i] = 0;
        S->rng = (uint32_t)rng() | 1u;
    }
    S->mode = (uint8_t)clamp(t->p[P_E0], 0, SR_NMODE - 1);
    S->xn = -1;                                          /* the exciter starts at the render (it knows the period) */
}

static void string_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    sr_slot_t *S = sr_slot(t, v);
    uint32_t i, w, mode = (uint32_t)clamp(p[P_E0], 0, SR_NMODE - 1), body = (uint32_t)clamp(p[P_E6], 0, SB_N - 1);
    uint32_t L, Lb, Ln, comp;
    int32_t pres = p[P_E1], kb, g, c, slope, bmix = p[P_E7] * 258, t60, jaw = body == SB_SITAR ? SQ_ONE / 48 : 0;
    int32_t lp, apx, apy, xn, xl, xlp, kx, gb[3], xf;
    uint32_t xd;
    tsvf_t bf[3];
    if (!S)
        return;
    if (n > CTL)
        n = CTL;
    /* the loop: its period (Q16), split at POS (0.02 .. 0.33 of it from the bridge) */
    L = sx_period(m->inc);
    if (L > (2000u << 16))
        L = 2000u << 16;
    if (L < (8u << 16))
        L = 8u << 16;
    kb = clamp(5000 + p[P_E4] * 216 + m->cutoff, 3000, 32767);   /* BRIT: the bridge's low-pass (ENV / LFO -> FLT) */
    c = p[P_E5] * 160;                                   /* STIF: the all-pass, 0 .. 0.62 (Q15) */
    /* their delays at low frequencies: (1 - k) / k, (1 - c) / (1 + c) samples (Q16), taken off the neck line */
    comp = ((uint32_t)(32768 - kb) << 16) / (uint32_t)kb + ((uint32_t)(32768 - c) << 16) / (uint32_t)(32768 + c);
    Lb = (uint32_t)(((uint64_t)L * (uint32_t)(1311 + p[P_E2] * 160)) >> 32);   /* whole samples */
    if (Lb < 2u)
        Lb = 2u;
    Ln = L - (Lb << 16) - comp;
    if ((int32_t)Ln < (2 << 16))
        Ln = 2u << 16;
    /* DAMP: the time the string rings, 0.08 .. ~20 s; per round trip the loss of 6.9 (60 dB in nepers) x L / T60 */
    t60 = (int32_t)sx_expmap(p[P_E3], 3528u, 8);   /* in samples (3528 = 80 ms) */
    g = 32767 - (int32_t)((((uint64_t)226357 * (L >> 8)) / (uint32_t)t60) >> 8);   /* (a 64 / 32 divide: the target has
                                                                                 * no 64 / 64 one) */
    g = clamp(g, 20000, 32760);
    if (mode == SR_BOW && !v->gate)                      /* (a bow lifted: the string is free, as damped as it is) */
        g = g > 32000 ? g - 300 : g;
    slope = (5 << 12) - pres * 129;                      /* the bow's pressure: STK's slope 5 - 4 pressure (Q12) */
    for (i = 0; i < 3u; i++) {
        tsvf_coef(&bf[i], SR_BODY_F[body][i] << 8, 96);
        gb[i] = SR_BODY_G[body][i];
    }
    if (S->xn < 0) {                                     /* a note: its exciter */
        S->xl = mode == SR_PLUCK ? (int32_t)clamp((int32_t)(L >> 17), 8, 900) : 4 + (127 - pres) * 1;
        S->xn = mode == SR_BOW ? 0 : S->xl;
    }
    kx = 1500 + pres * 240;                              /* PLUCK: the pick's low-pass */
    xd = S->xl > 0 ? 0xFFFFFFFFu / (uint32_t)S->xl : 0;  /* the exciter's steps: a turn (STRIKE), 1.0 (PLUCK's fade) */
    xf = S->xl > 0 ? 32767 / S->xl : 0;
    w = S->w;
    lp = S->lp;
    apx = S->apx;
    apy = S->apy;
    xn = S->xn;
    xl = S->xl;
    xlp = S->xlp;
    for (i = 0; i < n; i++) {
        int32_t bo = S->bridge[(w - Lb) & (SR_BLEN - 1u)], no = sx_read(S->neck, SR_NLEN - 1u, w, Ln);
        int32_t br, nr, sv, nv = 0, o, s, amp = amp_at(m, i);
        lp += px_m(bo - lp, kb, 15);                     /* the bridge: low-pass, loss, all-pass, then inverted */
        br = px_m(lp, g, 15);
        s = px_m(c, br - apy, 15) + apx;                 /* y = c (x - y1) + x1 */
        apx = br;
        apy = s;
        br = -s;
        if (jaw && br > jaw)                             /* SITAR: the jawari, one side of the bridge's swing */
            br = jaw + ((br - jaw) >> 3);
        nr = -no;                                        /* the nut */
        sv = br + nr;                                    /* the string's velocity where it is played */
        if (mode == SR_BOW) {
            int32_t bv = amp << 3, dv = bv - sv, contact = clamp(amp << 4, 0, 32767);   /* bow: 0 .. 0.25 */
            nv = px_m(px_m(dv, sx_bow(px_m(dv, slope, 12)), 15), contact, 15);
        } else if (xn > 0) {
            int32_t e;
            if (mode == SR_PLUCK) {                      /* noise, fading over the burst */
                e = px_m(sx_noise(&S->rng), xn * xf, 15);
                xlp += px_m(e - xlp, kx, 15);
                e = xlp * 3;
            } else {                                     /* a raised cosine: 1 - cos over the hammer's contact */
                e = (SQ_ONE >> 1) - (px_sin((uint32_t)xn * xd + 0x40000000u) >> 11);
            }
            nv = e >> 1;
            xn--;
        }
        S->neck[w & (SR_NLEN - 1u)] = br + nv;
        S->bridge[w & (SR_BLEN - 1u)] = nr + nv;
        w++;
        o = bo - S->dcx + px_m(S->dcy, 32604, 15);       /* DC block (~25 Hz) */
        S->dcx = bo;
        S->dcy = o;
        o = mode == SR_BOW ? o >> 3 : (o >> 4) + (o >> 5);   /* Q20 -> Q15 (a bowed string swings less: more gain) */
        if (body) {                                      /* the body: its resonances against the string */
            int32_t r = mulq15(gurdy_bp(&bf[0], o, &S->b[0], &S->b[1]), gb[0]) +
                        mulq15(gurdy_bp(&bf[1], o, &S->b[2], &S->b[3]), gb[1]) +
                        mulq15(gurdy_bp(&bf[2], o, &S->b[4], &S->b[5]), gb[2]);
            o += mulq15(r - o, bmix);
        }
        o = clamp(soft_knee(o, 24000), -32767, 32767);
        if (mode == SR_BOW)                              /* the bow sets the level; the release fades its ring */
            out[i] += mulq15(mulq15(o, clamp(amp << 2, 0, 32767)), VOICE_FS);
        else
            out[i] += voice_amp(o, m, i);
    }
    S->w = w;
    S->lp = lp;
    S->apx = apx;
    S->apy = apy;
    S->xn = xn;
    S->xlp = xlp;
}

static const param_desc_t SR_D_HARD = {"HARD", F_PCT, 0, 127, 64, 0, 0};
static const param_desc_t *string_desc(const track_t *t, uint32_t k)
{
    return k == 1u && t->p[P_E0] != SR_BOW ? &SR_D_HARD : 0;   /* PRES: the pick's / hammer's hardness */
}

#define STRANGE_CAT_STRING "LPLUUKPU"
static const preset_t STRING_PRESETS[] = {
    /* name, {MODE, PRES, POS, DAMP, BRIT, STIF, BODY, BMIX}, {A D S R}, fenv, mono */
    {"CELLO BOW", {SR_BOW, 60, 30, 80, 70, 4, SB_CELLO, 70}, {40, 64, 127, 60}, 0, 1, FX(0, 0, 0, 50)},
    {"VIOLIN BOW", {SR_BOW, 50, 22, 80, 96, 6, SB_VIOLIN, 64}, {30, 64, 127, 50}, 0, 0, FX(0, 0, 0, 50)},
    {"PONTICELLO", {SR_BOW, 30, 2, 90, 127, 10, SB_VIOLIN, 50}, {60, 64, 127, 70}, 0, 0, FX(0, 0, 10, 70)},
    {"NYLON", {SR_PLUCK, 40, 50, 92, 60, 2, SB_GUITAR, 70}, {0, 64, 127, 50}, 0, 0, FX(0, 0, 0, 30)},
    {"SITAR", {SR_PLUCK, 90, 40, 96, 110, 0, SB_SITAR, 60}, {0, 64, 127, 70}, 0, 0, FX(0, 0, 0, 40)},
    {"DULCIMER", {SR_STRIKE, 90, 30, 92, 100, 30, SB_GUITAR, 40}, {0, 64, 127, 60}, 0, 0, FX(0, 0, 10, 40)},
    {"DRONE BOW", {SR_BOW, 80, 40, 110, 80, 20, SB_CELLO, 90}, {90, 64, 127, 100}, 0, 0, FX(0, 20, 0, 80)},
    {"PIANO WIRE", {SR_STRIKE, 110, 16, 100, 116, 90, SB_OFF, 0}, {0, 64, 127, 70}, 0, 0, FX(0, 0, 0, 40)},
};

static const engine_t ENG_STRING = {
    .name = "STRING",
    .page_title = {"PLAY", "WOOD"},
    .edit = {
        {"MODE", F_ENUM, 0, SR_NMODE - 1, SR_BOW, N_SR_MODE, 0},
        {"PRES", F_PCT, 0, 127, 60, 0, 0},
        {"POS", F_PCT, 0, 127, 30, 0, 0},
        {"DAMP", F_PCT, 0, 127, 80, 0, 0},
        {"BRIT", F_PCT, 0, 127, 80, 0, 0},
        {"STIF", F_PCT, 0, 127, 4, 0, 0},
        {"BODY", F_ENUM, 0, SB_N - 1, SB_CELLO, N_SR_BODY, 0},
        {"BMIX", F_PCT, 0, 127, 70, 0, 0},
    },
    .presets = STRING_PRESETS,
    .npresets = NELEM(STRING_PRESETS),
    .note_on = string_note_on,
    .render = string_render,
    .desc = string_desc,
    .knob = {P_E1, P_E2, P_E3, P_E4},
    .poly = SR_POLY,
    .mem = sizeof(sr_slot_t) * SR_POLY,
};
