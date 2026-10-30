/* SPDX-License-Identifier: GPL-3.0-only */
/* PULHRA's machines added since the review (Tim, 31.10: what the palette was missing). Each appended to ENGINES (core.h
 * ENGI_*) and to ENGINE_ORDER (a stored machine is its place: the places of before keep theirs); the EDIT layer shows it
 * on its family's page (pulhra/sm_layer.c SM_EPG_X). Included after eng_mono.c and eng_dr.c: their helpers. */

/* ===================================================================== ACID === */
/* a 303-style bass: a saw, a square or between (WAVE), a sub an octave down (the ninth, SUB), into its own four-pole ladder
 * (CUT, RES: up to the edge of ringing; the bass it loses made up), swept by its own envelope (ENV the depth, DEC its fall,
 * 0.2 .. 2.5 s), and the accent (a step's ACCENT or a velocity of 120 and up: ACC): a harder, shorter sweep that builds up
 * over accents in a row (the 303's accent capacitor) and louder. DRIV into a soft clip after the filter. The slide: a
 * step's SLIDE on a LEGATO track (the presets are), the note glides, the envelope not struck again (note_on is not called).
 * ph[0] the oscillator, ph[1] the sub; s[0..3] the ladder, s[4] the sweep (Q15), s[5] the accent's charge (Q15), s[6] the
 * note accented (1) */
static void macid_note_on(track_t *t, voice_t *v)
{
    (void)t;
    if (mf_fresh(v)) {
        v->ph[0] = v->ph[1] = 0;
        v->s[0] = v->s[1] = v->s[2] = v->s[3] = 0;
        v->s[5] = 0;
    }
    v->s[4] = 32767;                                     /* (the sweep from its top at each struck note) */
    v->s[6] = v->vel >= 120u;
    if (v->s[6])                                         /* (an accent charges the capacitor: accents in a row build up) */
        v->s[5] += (32767 - v->s[5]) >> 1;
}
static void macid_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t i, b = mf_inc(t, m), ph = v->ph[0], ps = v->ph[1], gi;
    int32_t wave = clamp(p[P_E0], 0, 127), cut = clamp(p[P_E1], 0, 127), res = clamp(p[P_E2], 0, 127);
    int32_t envd = clamp(p[P_E3], 0, 127), acc = clamp(p[P_E5], 0, 127), drv = clamp(p[P_E6], 0, 127);
    int32_t sub = clamp(p[P_E8], 0, 127) * 160, ac = v->s[6] ? acc : 0;
    int32_t y1 = v->s[0], y2 = v->s[1], y3 = v->s[2], y4 = v->s[3], fe = v->s[4], cap = v->s[5];
    int32_t kq = res * 118, mk = 4096 + (res * 118) / 2;   /* (feedback up to ~3.7, Q12; the bass made up: 1 + k / 2) */
    uint32_t kf = dv_k(ac ? 200000u : 200000u + (uint32_t)clamp(p[P_E4], 0, 127) * 18110u);   /* (DEC 0.2 .. 2.5 s; an
                                                          * accent's: 0.2 s, as the 303's) */
    uint32_t kc = dv_k(400000u);                         /* (the accent's charge leaks away over ~0.4 s) */
    int32_t g, gq, q8;
    /* the cutoff, in CUTOFF_HZ's steps (Q8): CUT, the sweep's depth (ENV: up to ~5 octaves, more on an accent), the charge */
    q8 = cut * 256 + 17 * 256 + ((fe * (envd * 200 + ac * 120)) >> 15) + ((cap * ac) >> 15) * 50;   /* (+17: the four stages'
                                                          * own fall, ~1.2 octaves under each one's cutoff) */   /* (14 steps an octave: ENV up
                                                          * to 7 octaves, an accent 4 more, the charge 2) */
    q8 = clamp(q8, 0, 127 * 256);
    gi = (uint32_t)q8 >> 8;
    g = SVF_G[gi] + (((SVF_G[gi < 127u ? gi + 1u : 127u] - SVF_G[gi]) * (q8 & 255)) >> 8);   /* tan, Q12 */
    gq = (g << 15) / (4096 + g);                          /* a one-pole's coefficient t / (1 + t), Q15 */
    for (i = 0; i < n; i++) {
        int32_t sw = osc_saw(ph, b), x, u;
        x = sw + (((osc_pulse(ph, b, 0x80000000u) - sw) * wave) >> 7);   /* SAW .. SQR */
        x >>= 1;
        if (sub)
            x += ((int32_t)(ps >> 31) ? -sub : sub) >> 1;
        u = softclip(x - ((y4 * kq) >> 12));             /* the ladder: its input against the fourth stage, saturated */
        y1 += ((u - y1) * gq) >> 15;
        y2 += ((y1 - y2) * gq) >> 15;
        y3 += ((y2 - y3) * gq) >> 15;
        y4 += ((y3 - y4) * gq) >> 15;
        y4 = clamp(y4, -60000, 60000);
        x = (y4 * mk) >> 11;                            /* (+6 dB: as loud as the other machines) */
        if (drv)                                         /* DRIV: up to ~9 x into the soft clip */
            x = softclip(x + ((x * drv) >> 4));
        else
            x = soft_knee(x, 26000);
        if (ac)                                          /* the accent: louder (up to ~+6 dB) */
            x = clamp(x + ((x * ((ac * (fe + cap)) >> 8)) >> 15), -32767, 32767);
        ph += b;
        ps += b >> 1;
        fe = (int32_t)(((uint32_t)fe * kf) >> 15);
        cap = (int32_t)(((uint32_t)cap * kc) >> 15);
        out[i] += voice_amp(clamp(x, -32767, 32767), m, i);
    }
    v->ph[0] = ph;
    v->ph[1] = ps;
    v->s[0] = y1;
    v->s[1] = y2;
    v->s[2] = y3;
    v->s[3] = y4;
    v->s[4] = fe;
    v->s[5] = cap;
}
#define PX_CAT_MACID "BBBBBLBF"
static const preset_t MACID_PRESETS[] = {
    /* WAVE CUT RES ENV DEC ACC DRIV TUNE (SUB) */
    {"ACID 303", {0, 40, 90, 70, 50, 70, 20, 0}, {0, 64, 127, 20}, 0, 1, FX(0, 0, 20, 10)},
    {"SQUELCH", {0, 30, 115, 90, 40, 90, 40, 0}, {0, 64, 127, 20}, 0, 1, FX(0, 0, 25, 10)},
    {"SQR ACID", {127, 45, 85, 60, 55, 60, 10, 0}, {0, 64, 127, 20}, 0, 1, FX(0, 0, 20, 10)},
    {"DEEP ACID", {30, 40, 50, 35, 60, 40, 30, 0}, {0, 64, 127, 25}, 0, 1, FX(0, 0, 0, 10)},
    {"RUBBER", {64, 35, 70, 50, 30, 50, 30, 0}, {0, 64, 127, 20}, 0, 1, FX(0, 0, 15, 10)},
    {"ACID LEAD", {0, 60, 100, 60, 70, 60, 70, 0}, {0, 64, 127, 40}, 0, 1, FX(10, 0, 35, 25)},
    {"DIST ACID", {0, 45, 105, 80, 45, 80, 110, 0}, {0, 64, 127, 20}, 0, 1, FX(20, 0, 20, 10)},
    {"BLIP", {127, 20, 120, 110, 15, 60, 20, 0}, {0, 50, 60, 20}, 0, 1, FX(0, 0, 40, 30)},
};
static const engine_t ENG_MACID = {
    .name = "ACID", .page_title = {"FILTER", "ACCENT"}, .presets = MACID_PRESETS, .npresets = NELEM(MACID_PRESETS),
    .note_on = macid_note_on, .render = macid_render, .knob = {P_E1, P_E2, P_E3, P_E4}, .poly = 1,
    .edit = {{"WAVE", F_PCT, 0, 127, 0, 0, 0}, {"CUT", F_PCT, 0, 127, 40, 0, 0}, {"RES", F_PCT, 0, 127, 90, 0, 0},
             {"ENV", F_PCT, 0, 127, 70, 0, 0}, {"DEC", F_PCT, 0, 127, 50, 0, 0}, {"ACC", F_PCT, 0, 127, 70, 0, 0},
             {"DRIV", F_PCT, 0, 127, 20, 0, 0}, MF_TUNE},
    .e8 = {"SUB", F_PCT, 0, 127, 0, 0, 0},
    .xyd = {{XY_E(1), XY_E(3), 0}, {XY_E(2), XY_E(4), 0}},   /* X: the cutoff, the sweep; Y: the resonance, the decay */
    .xya = {{50, 40, 0}, {40, 40, 0}},
};
