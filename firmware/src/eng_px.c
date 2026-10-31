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

/* ==================================================================== CLOUD === */
/* a granular cloud (Tim, 31.10: textures, pads, drones for the shows) over the built-in material: SRC a source (GLASS, THROAT,
 * METAL: the bowed glass, the throat drone, the struck plate) or a slice (SLCE) of a SLICE machine's kit (GLITCH RITUAL MECH
 * BIO ERROR) or USER, the sample slot the web editor fills (SLCE its zone). The slice is decoded whole into the part's engine memory (a block's share at a time, the block hook), then
 * grains read it anywhere, either way: POS the centre (the ninth, SCAN, moves it on by itself, either way), SIZE a grain's
 * length (10 .. 400 ms), DENS how many a second (1 .. 100), SPRD the random spread around POS (0: frozen) and of the
 * starts, RAND a random detune (up to +-1 octave) and the share of grains reversed (up to half), PTCH the pitch
 * (semitones; a note: its own against the slice's root). Each grain a Hann window, the level 1 / sqrt(the overlap). */
#define CL_NBUF 12288u                                   /* the slice, decoded (0.55 s at 22,050 Hz) */
#define CL_NG 16u                                        /* the grains of a part, its voices' */
#define CL_POLY 3
typedef struct {
    uint32_t pos;                                        /* Q16, in the buffer */
    int32_t step;                                        /* Q16 a sample (negative: reversed) */
    uint32_t wph, winc;                                  /* the window: one turn */
    int16_t lvl;
    uint8_t on, vo;                                      /* sounding; its voice */
} cl_grain_t;
typedef struct {
    int16_t buf[CL_NBUF];
    cl_grain_t g[CL_NG];
    uint32_t n, len, rate;                               /* decoded so far, the slice's length, its rate (Q16 of 44.1 kHz) */
    int32_t pred, idx, root16;                           /* the decoder's state; the slice's root (1/16 semitone) */
    uint8_t src, sl, have;                               /* what the buffer holds (have: 1) */
    uint32_t gen;                                        /* the slot's scan it holds (USER: an upload decodes anew) */
    uint32_t rs;                                         /* random numbers */
} cl_part_t;
ENG_MEM_FITS("CLOUD", sizeof(cl_part_t));
enum { CL_GLASS, CL_THROAT, CL_METAL, CL_GLT, CL_RIT, CL_MEC, CL_BIO, CL_ERR, CL_USER, CL_NSRC };
static const char *const N_CL_SRC[CL_NSRC] = {"GLASS", "THROAT", "METAL", "GLITCH", "RITUAL", "MECH", "BIO", "ERROR", "USER"};
static cl_part_t *cl_part(track_t *t) { return (cl_part_t *)ENG_MEM(t - trk); }
static const smp_zone_t *cl_zone(uint32_t src, uint32_t sl)
{
    static const uint8_t KIT[6] = {0, GL_RIT, GL_MEC, GL_BIO, GL_ERR, 1u};   /* (USER: the slot USR1, the web editor's) */
    return src < CL_GLT ? &SMP_ZONES[src % 3u] : gl_zone(KIT[(src - CL_GLT) % 6u], sl & 15u);
}
static uint32_t cl_rnd(cl_part_t *P) { P->rs = P->rs * 1664525u + 1013904223u; return P->rs >> 8; }   /* 24 bits */
/* the block hook: the source asked for into the buffer, 1024 samples a block */
static void cloud_block(track_t *t)
{
    cl_part_t *P = cl_part(t);
    uint32_t src = (uint32_t)clamp(t->p[P_E0], 0, CL_NSRC - 1), sl = (uint32_t)clamp(t->p[P_E1], 0, 15), k;
    const smp_zone_t *z;
    if (t < &trk[0] || t >= &trk[NLIVE])
        return;
    z = cl_zone(src, sl);
    if (!P->have || P->src != src || P->sl != (src < CL_GLT ? 0u : sl) || (src == CL_USER && P->gen != smp_user_gen)) {
        P->have = 1;
        P->gen = smp_user_gen;
        P->src = (uint8_t)src;
        P->sl = (uint8_t)(src < CL_GLT ? 0u : sl);
        P->n = 0;
        P->len = z->n < CL_NBUF ? z->n : CL_NBUF;
        P->rate = z->rate;
        P->root16 = z->root16;
        P->pred = P->idx = 0;
        for (k = 0; k < CL_NG; k++)
            P->g[k].on = 0;
        if (!P->rs)
            P->rs = rng() | 1u;
    }
    for (k = 0; k < CL_NG; k++)                          /* (a grain of a voice gone: free) */
        if (P->g[k].on && (P->g[k].vo >= NVOICE || !t->v[P->g[k].vo].active))
            P->g[k].on = 0;
    for (k = 0; k < 1024u && P->n < P->len; k++, P->n++)
        P->buf[P->n] = (int16_t)gl_dec(z, P->n, &P->pred, &P->idx);
}
static void cloud_note_on(track_t *t, voice_t *v)
{
    uint32_t k, vi = (uint32_t)(v - t->v);
    if (t >= &trk[0] && t < &trk[NLIVE])
        for (k = 0; k < CL_NG; k++)                      /* (this voice's grains of before: done) */
            if (cl_part(t)->g[k].vo == vi)
                cl_part(t)->g[k].on = 0;
    v->s[0] = 0;                                         /* (the first grain at once) */
    if (mf_fresh(v))
        v->s[1] = 0;                                     /* (SCAN's offset from POS) */
}
static void cloud_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    cl_part_t *P;
    const int16_t *p = t->p;
    uint32_t vi = (uint32_t)(v - t->v), i, k, size, dens;
    int32_t pos = clamp(p[P_E2], 0, 127), sprd = clamp(p[P_E5], 0, 127), rnd = clamp(p[P_E6], 0, 127), scan = clamp(p[P_E8], -64, 63);
    int32_t d16, ov, lvl;
    uint32_t ratio, period;
    if (t < &trk[0] || t >= &trk[NLIVE] || vi >= CL_POLY)
        return;
    P = cl_part(t);
    if (!P->have || P->n < 64u)
        return;
    size = (uint32_t)(441u * dv_exp2((uint32_t)(clamp(p[P_E3], 0, 127) * 4096 * 532 / 127 / 100)) >> 16);   /* (10 ms x 40^(v/127):
                                                          * 10 .. 400 ms, in samples) */
    size = size < 441u ? 441u : size;
    dens = dv_exp2((uint32_t)(clamp(p[P_E4], 0, 127) * 4096 * 664 / 127 / 100));   /* (1 .. 100 a second, Q16) */
    period = (uint32_t)(((uint64_t)FS << 16) / dens);
    ov = (int32_t)(((uint64_t)size * dens) / ((uint32_t)FS << 16));   /* (grains at once) */
    lvl = ov <= 1 ? 32767 : ov >= 16 ? 8192 : (int32_t)(32767 * 4 / (2 + (ov > 4 ? ov / 2 + 2 : ov)));
    d16 = m->pitch16 - P->root16 + clamp(p[P_E7], -24, 24) * 16;
    ratio = (uint32_t)(((uint64_t)P->rate * (dv_exp2((uint32_t)clamp(d16 * 4096 / 192 + 4 * 4096, 0, 7 * 4096)) >> 4)) >> 16);
    ratio = ratio > (3u << 16) ? 3u << 16 : ratio;
    v->s[1] += scan * (int32_t)n * 4;                    /* SCAN: POS moves (+-1 the slice in ~6 s at the ends) */
    v->s[0] -= (int32_t)n;
    while (v->s[0] <= 0) {                               /* a grain starts */
        cl_grain_t *g = 0;
        uint32_t span, c, r = cl_rnd(P), rlen, st;
        int32_t jit = sprd ? (int32_t)((r & 0xFFFu) * (uint32_t)sprd / 127u) - (int32_t)((0x800u * (uint32_t)sprd) / 127u) : 0;
        v->s[0] += (int32_t)period + (int32_t)(((int64_t)period * jit) >> 12) + 1;   /* (period: samples) */
        for (k = 0; k < CL_NG && !g; k++)
            if (!P->g[k].on)
                g = &P->g[k];
        if (!g)
            continue;
        st = ratio;
        if (rnd) {                                       /* RAND: a detune (quadratic, up to an octave), some reversed */
            int32_t dt = (int32_t)(cl_rnd(P) & 0xFFFFu) - 32768;
            dt = (int32_t)(((int64_t)dt * rnd * rnd) >> 14) / 127 * 4096 / 32768;
            st = (uint32_t)(((uint64_t)st * (dv_exp2((uint32_t)(4 * 4096 + dt)) >> 4)) >> 16);
        }
        rlen = (uint32_t)(((uint64_t)size * st) >> 16) + 2u;   /* (the source samples the grain reads) */
        if (rlen >= P->n)
            rlen = P->n - 1u;
        span = P->n - rlen;
        c = (uint32_t)(((uint64_t)pos * span / 127u + ((((uint32_t)v->s[1] >> 8) & 0xFFFFu) * (uint64_t)span >> 16)) %
                       (span + 1u));                     /* (POS, SCAN's offset round the slice) */
        if (sprd) {
            int32_t o = (int32_t)(((int64_t)((int32_t)(cl_rnd(P) & 0xFFFFu) - 32768) * (int32_t)span * sprd) >> 22);
            c = (uint32_t)clamp((int32_t)c + o, 0, (int32_t)span);
        }
        g->on = 1;
        g->vo = (uint8_t)vi;
        g->wph = 0;
        g->winc = 0xFFFFFFFFu / size;
        g->lvl = (int16_t)lvl;
        if (rnd && (cl_rnd(P) & 0xFFu) < (uint32_t)rnd) {   /* (up to half of them reversed) */
            g->pos = (c + rlen - 1u) << 16;
            g->step = -(int32_t)st;
        } else {
            g->pos = c << 16;
            g->step = (int32_t)st;
        }
    }
    {
    int32_t acc[CTL];
    for (i = 0; i < n && i < CTL; i++)
        acc[i] = 0;
    for (k = 0; k < CL_NG; k++) {
        cl_grain_t *g = &P->g[k];
        if (!g->on || g->vo != vi)
            continue;
        for (i = 0; i < n && i < CTL; i++) {
            uint32_t a = g->pos >> 16, f = (g->pos >> 1) & 0x7FFFu, wi = g->wph >> 24, wf = (g->wph >> 9) & 0x7FFFu;
            int32_t s0 = P->buf[a < P->n ? a : P->n - 1u], s1 = P->buf[a + 1u < P->n ? a + 1u : P->n - 1u];
            int32_t s = s0 + (((s1 - s0) * (int32_t)f) >> 15);
            int32_t w = GR_HANN[wi] + (((GR_HANN[wi + 1u] - GR_HANN[wi]) * (int32_t)wf) >> 15);
            acc[i] += (((s * w) >> 15) * g->lvl) >> 15;
            g->pos = (uint32_t)((int32_t)g->pos + g->step);
            if (g->wph + g->winc < g->wph) {             /* (the window's end) */
                g->on = 0;
                break;
            }
            g->wph += g->winc;
        }
    }
    for (i = 0; i < n && i < CTL; i++)                   /* (x 2.5: as loud as the other machines; a knee over the sum) */
        out[i] += voice_amp(clamp(soft_knee((acc[i] * 5) >> 1, 26000), -32767, 32767), m, i);
    }
}
#define PX_CAT_CLOUD "PPPPFPFF"
static const preset_t CLOUD_PRESETS[] = {
    /* SRC SLCE POS SIZE DENS SPRD RAND PTCH (SCAN) */
    {"GLASS PAD", {CL_GLASS, 0, 40, 90, 70, 40, 10, 0}, {80, 64, 127, 110}, 0, 0, FX(0, 20, 20, 90)},
    {"THROAT CHOIR", {CL_THROAT, 0, 50, 95, 80, 30, 20, 0}, {70, 64, 127, 100}, 0, 0, FX(0, 30, 20, 80)},
    {"METAL DRONE", {CL_METAL, 0, 20, 80, 60, 20, 5, -12}, {90, 64, 127, 120}, 0, 0, FX(0, 0, 30, 90)},
    {"FROZEN", {CL_GLASS, 0, 64, 60, 100, 0, 0, 0}, {60, 64, 127, 100}, 0, 0, FX(0, 20, 0, 70)},
    {"SHIMMER", {CL_METAL, 0, 60, 40, 110, 50, 60, 12}, {40, 64, 127, 90}, 0, 0, FX(0, 30, 40, 100)},
    {"RITUAL FOG", {CL_RIT, 3, 40, 90, 70, 80, 30, 0}, {80, 64, 127, 110}, 0, 0, FX(0, 0, 30, 90)},
    {"BIO SWARM", {CL_BIO, 5, 50, 20, 115, 100, 40, 0}, {20, 64, 127, 60}, 0, 0, FX(0, 0, 20, 60)},
    {"ERROR DUST", {CL_ERR, 8, 50, 10, 80, 127, 80, 0}, {10, 64, 127, 50}, 0, 0, FX(10, 0, 30, 50)},
};
static const engine_t ENG_CLOUD = {
    .name = "CLOUD", .page_title = {"SOURCE", "GRAINS"}, .presets = CLOUD_PRESETS, .npresets = NELEM(CLOUD_PRESETS),
    .note_on = cloud_note_on, .render = cloud_render, .block = cloud_block, .knob = {P_E2, P_E3, P_E4, P_E5}, .poly = CL_POLY,
    .mem = sizeof(cl_part_t),
    .edit = {{"SRC", F_ENUM, 0, CL_NSRC - 1, 0, N_CL_SRC, 0}, {"SLCE", F_INT, 0, 15, 0, 0, 0}, {"POS", F_PCT, 0, 127, 40, 0, 0},
             {"SIZE", F_PCT, 0, 127, 90, 0, 0}, {"DENS", F_PCT, 0, 127, 70, 0, 0}, {"SPRD", F_PCT, 0, 127, 40, 0, 0},
             {"RAND", F_PCT, 0, 127, 10, 0, 0}, {"PTCH", F_INT, -24, 24, 0, 0, "st"}},
    .e8 = {"SCAN", F_BIPCT, -64, 63, 0, 0, 0},
    .xyd = {{XY_E(2), XY_E(5), 0}, {XY_E(3), XY_E(4), 0}},   /* X: the position, the spread; Y: the size, the density */
    .xya = {{50, 40, 0}, {40, 40, 0}},
};

/* ===================================================================== WEST === */
/* a west-coast complex oscillator (Tim, 31.10): a principal oscillator (SHAP: sine .. triangle) frequency-modulated through
 * zero by a modulator (RAT its ratio, MOD the depth; the ninth, FMEN, how much the low-pass gate's strike opens it more),
 * into a wavefolder (TIMB: up to 6 folds; SYM its offset: the even partials), into a low-pass gate: struck at each note
 * (a vactrol: ~2 ms in, LPG its fall 20 ms .. 3 s, 127 held while the note is), opening a two-pole low-pass (CTRL: 0 the
 * gate a VCA alone, up: the filter too: the "bongo"). ph[0] the principal, ph[1] the modulator; s[0] the gate (Q15), s[1]
 * s[2] the filter, s[3] the gate's rise (Q15) */
static const char *const N_WEST_RAT[10] = {"0.5", "1", "1.41", "1.5", "2", "2.76", "3", "3.5", "5.19", "7.13"};
static const uint16_t WEST_RAT_Q8[10] = {128, 256, 361, 384, 512, 707, 768, 896, 1329, 1825};
static void west_note_on(track_t *t, voice_t *v)
{
    (void)t;
    if (mf_fresh(v)) {
        v->ph[0] = v->ph[1] = 0;
        v->s[1] = v->s[2] = 0;
        v->s[0] = 0;
    }
    v->s[3] = 1;                                         /* (struck: the gate rises) */
}
static void west_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t i, b = mf_inc(t, m), bm = (b >> 8) * WEST_RAT_Q8[clamp(p[P_E3], 0, 9)], gi;
    int32_t timb = clamp(p[P_E0], 0, 127), sym = clamp(p[P_E1], -64, 63) * 256, mod = clamp(p[P_E2], 0, 127);
    int32_t shap = clamp(p[P_E4], 0, 127), lpg = clamp(p[P_E5], 0, 127), ctrl = clamp(p[P_E6], 0, 127);
    int32_t fmen = clamp(p[P_E8], 0, 127), e = v->s[0], f1 = v->s[1], f2 = v->s[2], ph_k, g, gq, q8;
    uint32_t kd = lpg >= 127 ? 32768u : dv_k(20000u + (uint32_t)lpg * (uint32_t)lpg * 184u);   /* (20 ms .. ~3 s) */
    if (v->s[3]) {                                       /* the strike: up over ~2 ms (two blocks), then the fall */
        e += 16384;
        if (e >= 32767) {
            e = 32767;
            v->s[3] = 0;
        }
    } else if (lpg >= 127) {
        e = v->gate ? 32767 : e;                         /* (held: the gate open while the note is) */
    }
    /* the gate's filter: from ~60 Hz closed to fully open, by the gate's level squared (a vactrol's curve), CTRL how much */
    q8 = 127 * 256 - (((127 - ((e * e) >> 15) * 127 / 32767) * 256 * ctrl) >> 7);
    q8 = clamp(q8, 12 * 256, 127 * 256);
    gi = (uint32_t)q8 >> 8;
    g = SVF_G[gi] + (((SVF_G[gi < 127u ? gi + 1u : 127u] - SVF_G[gi]) * (q8 & 255)) >> 8);
    gq = (g << 15) / (4096 + g);
    ph_k = mod * 4 + ((fmen * e) >> 13);                 /* (the FM depth: MOD, opened by the strike with FMEN) */
    for (i = 0; i < n; i++) {
        int32_t mo = sine_i(v->ph[1]), x, y, fx;
        uint32_t inc = (uint32_t)((int32_t)b + (int32_t)(((int64_t)(int32_t)(b >> 4) * mo * ph_k) >> 19));   /* (through zero) */
        x = sine_i(v->ph[0]);
        if (shap)                                        /* SHAP: the sine towards the triangle */
            x += ((osc_tri(v->ph[0]) - x) * shap) >> 7;
        if (timb || sym) {                               /* the folder: up to 6 folds, SYM's offset first */
            y = x + ((x * timb * 5) >> 7) + sym;
            while (y > 32767 || y < -32767)
                y = y > 32767 ? 65534 - y : -65534 - y;
            x = y;
        }
        f1 += (((x - f1) * gq) >> 15);
        f2 += (((f1 - f2) * gq) >> 15);
        fx = ctrl ? f2 : x;
        fx = (fx * e) >> 15;                             /* the gate's VCA */
        v->ph[0] += inc;
        v->ph[1] += bm;
        if (!v->s[3] && lpg < 127)                       /* the gate's fall */
            e = (int32_t)(((uint32_t)e * kd) >> 15);
        out[i] += voice_amp(soft_knee(fx, 26000), m, i);
    }
    v->s[0] = e;
    v->s[1] = f1;
    v->s[2] = f2;
}
#define PX_CAT_WEST "UBUULLPF"
static const preset_t WEST_PRESETS[] = {
    /* TIMB SYM MOD RAT SHAP LPG CTRL TUNE (FMEN) */
    {"WEST BONGO", {20, 0, 40, 2, 0, 20, 127, 0}, {0, 64, 127, 40}, 0, 0, FX(0, 0, 20, 30)},
    {"FOLD BASS", {70, 0, 0, 1, 0, 127, 60, 0}, {0, 64, 127, 20}, 0, 1, FX(0, 0, 0, 10)},
    {"BUCHLA BELL", {30, 0, 90, 7, 0, 70, 90, 0}, {0, 64, 127, 70}, 0, 0, FX(0, 0, 30, 50)},
    {"WOOD PLUCK", {40, 10, 20, 1, 60, 25, 127, 0}, {0, 64, 127, 30}, 0, 0, FX(0, 0, 20, 30)},
    {"FOLD LEAD", {90, 20, 10, 4, 0, 127, 40, 0}, {0, 64, 127, 40}, 0, 1, FX(10, 0, 30, 30)},
    {"ZING LEAD", {60, -20, 60, 3, 30, 127, 70, 0}, {5, 64, 127, 50}, 0, 1, FX(0, 20, 30, 40)},
    {"DRONE FOLD", {50, 30, 30, 5, 40, 127, 30, 0}, {90, 64, 127, 110}, 0, 0, FX(0, 30, 30, 90)},
    {"METAL PING", {10, 0, 110, 8, 0, 45, 110, 0}, {0, 64, 127, 50}, 0, 0, FX(0, 0, 30, 40)},
};
static const engine_t ENG_WEST = {
    .name = "WEST", .page_title = {"FOLD", "GATE"}, .presets = WEST_PRESETS, .npresets = NELEM(WEST_PRESETS),
    .note_on = west_note_on, .render = west_render, .knob = {P_E0, P_E2, P_E5, P_E6}, .poly = MF_POLY,
    .edit = {{"TIMB", F_PCT, 0, 127, 20, 0, 0}, {"SYM", F_BIPCT, -64, 63, 0, 0, 0}, {"MOD", F_PCT, 0, 127, 40, 0, 0},
             {"RAT", F_ENUM, 0, 9, 2, N_WEST_RAT, 0}, {"SHAP", F_PCT, 0, 127, 0, 0, 0}, {"LPG", F_PCT, 0, 127, 30, 0, 0},
             {"CTRL", F_PCT, 0, 127, 127, 0, 0}, MF_TUNE},
    .e8 = {"FMEN", F_PCT, 0, 127, 0, 0, 0},
    .xyd = {{XY_E(0), XY_E(1), 0}, {XY_E(2), XY_E(5), 0}},   /* X: the folds, SYM; Y: the FM, the gate's fall */
    .xya = {{50, 30, 0}, {50, 40, 0}},
};

/* ==================================================================== ORGAN === */
/* a drawbar organ (Tim, 31.10: dub chords, ambient): six drawbars 0..8 (16' 8' 5 1/3' 4' 2 2/3' 2': the sub, the note, its
 * fifth, the octave, the twelfth, two octaves), sines from one phase (the sub's: the others its multiples, in tune), PERC a
 * decaying 2 2/3' at each note (~250 ms), CLIK the key click (a burst of noise, ~3 ms); the ninth, ROTR: a rotary speaker
 * (0 off, slow .. fast: its tremolo and its doppler, one rotor a part, turned by the block hook). Level: the drawbars'
 * sum made up (1 / sqrt). ph[0] the sub's phase; s[0] the percussion, s[1] the click (Q15), s[2] random numbers */
static uint32_t org_rot[NPART];                          /* the rotors' phases */
static void organ_block(track_t *t)
{
    uint32_t pi = (uint32_t)(t - trk) % NPART, r = (uint32_t)clamp(t->p[P_E8], 0, 127);
    if (r)                                               /* (0.6 .. 7 Hz) */
        org_rot[pi] += (uint32_t)((uint64_t)(60u + r * r * 640u / 16129u) * CTL * 97391u / 100u);
}
static void organ_note_on(track_t *t, voice_t *v)
{
    (void)t;
    if (mf_fresh(v))
        v->ph[0] = 0;
    v->s[0] = 32767;
    v->s[1] = 32767;
    if (!v->s[2])
        v->s[2] = (int32_t)(rng() | 1u);
}
static void organ_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    static const uint8_t MUL[6] = {1, 2, 3, 4, 6, 8};    /* (of the sub's phase) */
    const int16_t *p = t->p;
    uint32_t i, k, b = mf_inc(t, m) >> 1, rot = org_rot[(uint32_t)(t - trk) % NPART];
    int32_t db[6], sum = 0, g, perc = clamp(p[P_E6], 0, 127) * 200, clik = clamp(p[P_E7], 0, 127) * 120;
    int32_t pe = v->s[0], ce = v->s[1], rd = clamp(p[P_E8], 0, 127) ? 9000 : 0, am, fmv;
    uint32_t kp = dv_k(250000u), kc = dv_k(3000u);
    for (k = 0; k < 6u; k++) {
        db[k] = clamp(p[P_E0 + k], 0, 8) * 4096 / 8;
        sum += db[k];
    }
    g = sum ? (int32_t)(((int64_t)32767 * 4096) / (int32_t)(mf_isqrt((uint32_t)sum * 4096u) * 2u + 1u)) : 0;   /* (1 / sqrt) */
    am = rd ? 32767 - ((rd * (sine_i(rot) + 32767)) >> 16) : 32767;   /* ROTR: the horn's tremolo */
    fmv = rd ? sine_i(rot + 0x40000000u) : 0;            /* .. its doppler (a quarter turn on) */
    b = b + (uint32_t)(((int64_t)(int32_t)(b >> 8) * fmv) >> 18);   /* (+-~0.4 %) */
    for (i = 0; i < n; i++) {
        uint32_t ph = v->ph[0];
        int32_t x = 0, y;
        for (k = 0; k < 6u; k++)
            if (db[k])
                x += (sine_i(ph * MUL[k]) * db[k]) >> 12;
        x = (x * g) >> 14;                               /* (+6 dB: as loud as the others) */
        if (perc && pe > 16) {                           /* PERC: the 2 2/3' struck */
            x += (((sine_i(ph * 6u) * pe) >> 15) * perc) >> 15;
            pe = (int32_t)(((uint32_t)pe * kp) >> 15);
        }
        if (clik && ce > 16) {                           /* CLIK: the key's contacts */
            x += (((((int32_t)noise32(&v->s[2]) >> 17) * ce) >> 15) * clik) >> 15;
            ce = (int32_t)(((uint32_t)ce * kc) >> 15);
        }
        y = (x * am) >> 15;
        v->ph[0] += b;
        out[i] += voice_amp(soft_knee(y, 26000), m, i);
    }
    v->s[0] = pe;
    v->s[1] = ce;
}
#define PX_CAT_ORGAN "KKKPKKPK"
static const preset_t ORGAN_PRESETS[] = {
    /* 16' 8' 5 1/3' 4' 2 2/3' 2' PERC CLIK (ROTR) */
    {"FULL ORGAN", {8, 8, 8, 8, 8, 8, 0, 20}, {2, 64, 127, 20}, 0, 0, FX(10, 0, 0, 30)},
    {"JAZZ 888", {8, 8, 8, 0, 0, 0, 60, 40}, {0, 64, 127, 15}, 0, 0, FX(0, 0, 0, 25)},
    {"GOSPEL", {8, 8, 8, 8, 0, 6, 30, 30}, {0, 64, 127, 20}, 0, 0, FX(20, 0, 0, 30)},
    {"SOFT FLUTE", {0, 8, 0, 4, 0, 0, 0, 0}, {10, 64, 127, 40}, 0, 0, FX(0, 20, 20, 50)},
    {"DUB CHORD", {0, 8, 4, 6, 0, 0, 40, 10}, {0, 40, 60, 30}, 0, 0, FX(0, 0, 60, 50)},
    {"REED", {0, 6, 8, 4, 6, 2, 0, 10}, {0, 64, 127, 20}, 0, 0, FX(10, 0, 0, 30)},
    {"CHURCH", {8, 6, 4, 8, 4, 8, 0, 0}, {60, 64, 127, 90}, 0, 0, FX(0, 0, 0, 90)},
    {"PERC ORGAN", {0, 8, 0, 0, 0, 0, 100, 30}, {0, 64, 127, 15}, 0, 0, FX(0, 0, 20, 30)},
};
static const engine_t ENG_ORGAN = {
    .name = "ORGAN", .page_title = {"BARS", "BARS 2"}, .presets = ORGAN_PRESETS, .npresets = NELEM(ORGAN_PRESETS),
    .note_on = organ_note_on, .render = organ_render, .block = organ_block, .knob = {P_E1, P_E2, P_E3, P_E6}, .poly = MF_POLY,
    .edit = {{"16'", F_INT, 0, 8, 0, 0, 0}, {"8'", F_INT, 0, 8, 8, 0, 0}, {"5'", F_INT, 0, 8, 8, 0, 0},
             {"4'", F_INT, 0, 8, 0, 0, 0}, {"2'2", F_INT, 0, 8, 0, 0, 0}, {"2'", F_INT, 0, 8, 0, 0, 0},
             {"PERC", F_PCT, 0, 127, 0, 0, 0}, {"CLIK", F_PCT, 0, 127, 20, 0, 0}},
    .e8 = {"ROTR", F_PCT, 0, 127, 0, 0, 0},
    .xyd = {{XY_E(2), XY_E(3), 0}, {XY_E(6), XY_E(8), 0}},
    .xya = {{40, 40, 0}, {50, 50, 0}},
};
