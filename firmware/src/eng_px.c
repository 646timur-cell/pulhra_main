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
enum { CL_GLASS, CL_THROAT, CL_METAL, CL_GLT, CL_RIT, CL_MEC, CL_BIO, CL_ERR, CL_USER, CL_USER2, CL_USER3, CL_NSRC };
static const char *const N_CL_SRC[CL_NSRC] = {"GLASS", "THROAT", "METAL", "GLITCH", "RITUAL", "MECH", "BIO", "ERROR", "USR1", "USR2", "USR3"};
static cl_part_t *cl_part(track_t *t) { return (cl_part_t *)ENG_MEM(t - trk); }
static const smp_zone_t *cl_zone(uint32_t src, uint32_t sl)
{
    static const uint8_t KIT[8] = {0, GL_RIT, GL_MEC, GL_BIO, GL_ERR, 1u, 2u, 3u};   /* (USR1..3: the slots, the web editor's) */
    return src < CL_GLT ? &SMP_ZONES[src % 3u] : gl_zone(KIT[(src - CL_GLT) % 8u], sl & 15u);
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
    if (!P->have || P->src != src || P->sl != (src < CL_GLT ? 0u : sl) || (src >= CL_USER && P->gen != smp_user_gen)) {
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

/* ======================================================================= CZ === */
/* Casio CZ-style phase distortion (Tim, 1.11): the PD core of PHASE (eng_phase.c: CrispyZebra's, the CZ's eight waves,
 * WAVE and WAVE2 alternating each cycle), as a CZ-1000 plays it: DCW the bend, ENV its envelope's depth (the envelope is
 * F.ENV's ADSR: the DCW envelope; FILTER's ENV>FLT and LFO>FLT move DCW too), LINE 1 (one line), 1+1' (a second line DTN
 * cents up, mixed: the CZ's detuned doubling), RING (the two lines multiplied) or NOIS (the line through noise: the CZ's
 * noise modulation), SUB a sine an octave down, KF the DCW key follow (higher notes less bent, a CZ's darker top).
 * ph[0] line 1, ph[1] line 2 (NOIS: the noise's state), ph[2] the sub; s[0] s[1] the WAVE / WAVE2 toggles; s[2] the
 * noise held (NOIS: a sample and hold at 4 x the pitch) */
static const char *const N_CZ_LINE[] = {"1", "1+1'", "RING", "NOIS"};
static void cz_note_on(track_t *t, voice_t *v)
{
    (void)t;
    if (mf_fresh(v)) {
        v->ph[0] = 0;
        v->ph[2] = 0;
        v->s[0] = v->s[1] = v->s[2] = 0;
    }
    v->ph[1] = v->ph[0];                                 /* (line 2 starts with line 1: the doubling beats from there) */
    if (!v->ph[1])
        v->ph[1] = 1u;
}
static void cz_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t w1 = (uint32_t)p[P_E0] & 7u, w2 = (uint32_t)p[P_E1], line = (uint32_t)clamp(p[P_E4], 0, 3), i, dcw, inc = m->inc, inc2 = 0;
    int32_t depth = (p[P_E2] << 8) + m->cutoff + mulq15(m->envq15, p[P_E3] * 256), sub = p[P_E6] * 200, kf;
    uint32_t ph0 = v->ph[0], ph1 = v->ph[1], ph2 = v->ph[2];
    int32_t tg0 = v->s[0], tg1 = v->s[1], nz = v->s[2];
    pd_t b1, b2;
    kf = (m->pitch16 - 48 * 16) * p[P_E7];               /* (above C3: 3 octaves at KF 127 take the bend about away) */
    depth -= kf > 0 ? (kf * 3) >> 3 : 0;
    depth = clamp(depth, 0, 127 << 8);
    dcw = (uint32_t)depth * 65535u / (127u << 8);
    dcw = (dcw * 56000u) >> 16;                          /* (PHASE's classic range of the bend) */
    if (line == 1u || line == 2u)
        inc2 = cents_inc(m->pitch16, p[P_E5], m->fine);  /* (DTN: 0 .. 127 cents up) */
    pd_setup(&b1, w1, dcw);
    pd_setup(&b2, w2 ? w2 - 1u : w1, dcw);
    for (i = 0; i < n; i++) {
        uint32_t old = ph0;
        int32_t s = pd_wave(tg0 ? &b2 : &b1, ph0 >> 16);
        ph0 += inc;
        if (ph0 < old)
            tg0 ^= 1;
        if (line == 1u || line == 2u) {
            uint32_t old2 = ph1;
            int32_t s2 = pd_wave(tg1 ? &b2 : &b1, ph1 >> 16);
            ph1 += inc2;
            if (ph1 < old2)
                tg1 ^= 1;
            s = line == 2u ? mulq15(s, s2) : (s + s2) >> 1;
        } else if (line == 3u) {
            if ((ph0 & 0x3FFFFFFFu) < (old & 0x3FFFFFFFu) || ph0 < old) {   /* (a new noise value 4 x a cycle) */
                ph1 = ph1 * 1664525u + 1013904223u;
                nz = (int32_t)(ph1 >> 17) - 16384;       /* (+-0.5) */
            }
            s = mulq15(s, 24576 + nz);                   /* (the wave roughened, not drowned) */
        }
        if (sub) {
            ph2 += inc >> 1;
            s += mulq15(osc_sine(ph2), sub);
        }
        out[i] += voice_amp(s, m, i) << 1;
    }
    v->ph[0] = ph0;
    v->ph[1] = ph1;
    v->ph[2] = ph2;
    v->s[0] = tg0;
    v->s[1] = tg1;
    v->s[2] = nz;
}
/* the presets: WAVE WAV2 DCW ENV LINE DTN SUB KF; their DCW envelope (F.ENV: ATK DEC SUS REL) in CZ_DCW below. In the
 * spirit of the CZ-1000's factory sounds (made by ear: no factory data), the two basses first (Tim) */
static const preset_t CZ_PRESETS[] = {
    {"CZ SYN BASS", {0, 2, 10, 110, 1, 6, 40, 30}, {0, 70, 70, 20}, 0, 1, FX(0, 0, 0, 5)},
    {"CZ FAT BASS", {0, 0, 20, 90, 1, 14, 70, 20}, {0, 64, 100, 20}, 0, 1, FX(10, 10, 0, 5)},
    {"CZ BRASS", {0, 0, 15, 95, 1, 8, 0, 40}, {12, 80, 100, 35}, 0, 0, FX(0, 20, 10, 35)},
    {"CZ STRINGS", {0, 4, 30, 50, 1, 12, 0, 50}, {50, 90, 110, 70}, 0, 0, FX(0, 50, 20, 60)},
    {"CZ E.PIANO", {3, 0, 10, 80, 0, 0, 0, 60}, {0, 85, 40, 50}, 0, 0, FX(0, 30, 15, 40)},
    {"CZ CLAV", {2, 0, 20, 100, 0, 0, 0, 40}, {0, 55, 20, 15}, 0, 0, FX(10, 0, 15, 20)},
    {"CZ BELL", {6, 0, 40, 70, 2, 9, 0, 30}, {0, 100, 0, 90}, 0, 0, FX(0, 0, 30, 60)},
    {"CZ RESO SWEEP", {5, 0, 0, 120, 0, 0, 0, 20}, {0, 64, 110, 40}, 0, 0, FX(0, 0, 30, 40)},
    {"CZ ORGAN", {3, 1, 50, 0, 1, 4, 0, 0}, {2, 64, 127, 20}, 0, 0, FX(0, 40, 0, 30)},
    {"CZ PERCUSSION", {4, 7, 20, 110, 3, 0, 0, 30}, {0, 60, 0, 40}, 0, 0, FX(0, 0, 25, 40)},
};
static const uint8_t CZ_DCW[NELEM(CZ_PRESETS)][4] = {   /* F.ENV for each: the DCW envelope (ATK DEC SUS REL) */
    {0, 45, 15, 20},   /* SYN BASS: a quick fall, the CZ bass's snap */
    {0, 60, 30, 25},   /* FAT BASS: slower, rounder */
    {25, 70, 70, 40},  /* BRASS: the bend swells in */
    {60, 90, 80, 70},  /* STRINGS */
    {0, 70, 20, 50},   /* E.PIANO: the bright strike, darker after */
    {0, 40, 10, 15},   /* CLAV */
    {0, 95, 0, 90},    /* BELL */
    {40, 110, 0, 50},  /* RESO SWEEP: up, then the long fall */
    {0, 64, 127, 20},  /* ORGAN (ENV 0: no sweep) */
    {0, 30, 0, 30},    /* PERCUSSION */
};
static const engine_t ENG_CZ = {
    .name = "CZ", .page_title = {"PD", "LINE"}, .presets = CZ_PRESETS, .npresets = NELEM(CZ_PRESETS),
    .note_on = cz_note_on, .render = cz_render, .knob = {P_E2, P_E3, P_E5, P_E7}, .poly = MF_POLY,
    .edit = {{"WAVE", F_ENUM, 0, 7, 0, N_PD_WAVE, 0}, {"WAV2", F_ENUM, 0, 8, 0, N_PD_WAVE2, 0},
             {"DCW", F_PCT, 0, 127, 30, 0, 0}, {"ENV", F_PCT, 0, 127, 80, 0, 0},
             {"LINE", F_ENUM, 0, 3, 0, N_CZ_LINE, 0}, {"DTN", F_INT, 0, 127, 8, 0, "ct"},
             {"SUB", F_PCT, 0, 127, 0, 0, 0}, {"KF", F_PCT, 0, 127, 30, 0, 0}},
    .xyd = {{XY_E(2), XY_E(3), 0}, {XY_E(5), XY_E(7), 0}},
    .xya = {{50, 40, 0}, {40, 40, 0}},
};

/* ====================================================================== EMS === */
/* Formanta EMS-01 style (Tim, 1.11; by the descriptions of it: a Soviet synth and organ of 1980-82, two oscillators, ring,
 * noise, a filter after the Polivoks' (low-pass / band-pass, "aggressive", its cutoff 100 .. 1500 Hz), a delayed vibrato and
 * a stereo phaser over it all). WAV1 SAW / RSAW (falling) / SQR / PLS (20 %), WAV2 the same or RING (oscillator 1 times a
 * saw at oscillator 2's pitch) or NOIS; DTN oscillator 2 -12 .. +12 semitones (at 0 it runs 6 cents up: the two beat, as
 * two analogue oscillators do); MIX 1 .. 2. Its own 12 dB state-variable filter, its integrators saturated (an op-amp
 * filter driven: the Polivoks' scream): CUT over ~80 .. 1800 Hz, RES up to the edge of ringing, MODE LP / BP, ENV the
 * depth of F.ENV on it (FILTER's ENV>FLT and LFO>FLT move it too). The ninth, VIB: the vibrato's depth (5.5 Hz, ~0.3 s
 * before it fades in over ~0.3 s, as the EMS's delayed vibrato). The presets put INSERT's PHASR on.
 * ph[0] ph[1] the oscillators, ph[2] the vibrato's phase; s[0] s[1] the filter, s[2] the noise, s[3] the blocks since the note */
static const char *const N_EMS_W1[] = {"SAW", "RSAW", "SQR", "PLS"};
static const char *const N_EMS_W2[] = {"SAW", "RSAW", "SQR", "PLS", "RING", "NOIS"};
static const char *const N_EMS_MODE[] = {"LP", "BP"};
static void ems_note_on(track_t *t, voice_t *v)
{
    (void)t;
    if (mf_fresh(v)) {
        v->ph[0] = 0;
        v->ph[1] = 0x40000000u;
        v->s[0] = v->s[1] = 0;
        v->s[2] = 0x1234567;
    }
    if (mf_fresh(v) || v->gate)                          /* (a struck note: the vibrato's delay from here; legato keeps it) */
        v->s[3] = 0;
}
static inline int32_t ems_osc(uint32_t w, uint32_t ph, uint32_t inc)
{
    return w == 0u ? osc_saw(ph, inc) : w == 1u ? -osc_saw(ph, inc) : w == 2u ? osc_pulse(ph, inc, 0x80000000u)
                                                                    : osc_pulse(ph, inc, 0x33333333u);
}
static void ems_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t w1 = (uint32_t)clamp(p[P_E0], 0, 3), w2 = (uint32_t)clamp(p[P_E1], 0, 5), i, inc1, inc2;
    uint32_t ph0 = v->ph[0], ph1 = v->ph[1], bp = (uint32_t)clamp(p[P_E6], 0, 1);
    int32_t dtn = clamp(p[P_E2], -12, 12), mix = clamp(p[P_E3], 0, 127), res = clamp(p[P_E5], 0, 127);
    int32_t vib = clamp(MF_E8(p), 0, 127), ic1 = v->s[0], ic2 = v->s[1], nz = v->s[2], age = v->s[3], ct = 0, cut, k, mk;
    tsvf_t c;
    if (vib) {                                           /* the delayed vibrato: 5.5 Hz, up to +-50 cents */
        int32_t fade = clamp((age - 420) * 32767 / 420, 0, 32767);   /* (~0.3 s still, then ~0.3 s in: 0.73 ms a block) */
        v->ph[2] += 535690u * CTL;                       /* (5.5 Hz) */
        ct = mulq15(mulq15(osc_sine(v->ph[2]), fade), vib * 50 * 258 / 127);
    }
    v->s[3] = age < 100000 ? age + 1 : age;
    inc1 = ct ? cents_inc(m->pitch16, ct, m->fine) : m->inc;
    inc2 = cents_inc(clamp(m->pitch16 + dtn * 16, 0, 2047), ct + (dtn ? 0 : 6), m->fine);
    /* the cutoff: CUT over CUTOFF_HZ's 19 .. 83 (~80 .. 1800 Hz), ENV x F.ENV up to 50 steps more (~3.5 octaves), FILTER's mods */
    cut = (19 << 8) + clamp(p[P_E4], 0, 127) * 129 + ((m->envq15 * clamp(p[P_E7], 0, 127) * 101) >> 15) + m->cutoff;
    k = 8192 - res * 8000 / 127;                         /* (damping 2.0 .. 0.05: the edge of ringing) */
    tsvf_coef_k(&c, cut, k);
    mk = bp ? 4096 + res * 24 : 4096 + res * 12;         /* (the level the resonance takes made up a little) */
    for (i = 0; i < n; i++) {
        int32_t a = ems_osc(w1, ph0, inc1), b, x, v1, v2, v3;
        if (w2 < 4u)
            b = ems_osc(w2, ph1, inc2);
        else if (w2 == 4u)
            b = (int32_t)(((int64_t)a * osc_saw(ph1, inc2)) >> 15);   /* RING (the blep may pass 2^15) */
        else
            b = (int32_t)(noise32(&nz) >> 17) - 16384;   /* NOIS (+-0.5) */
        x = ((a * (127 - mix) + b * mix) >> 7);
        /* the filter (Simper's SVF), its two integrators through a soft clip: driven, it screams and squashes */
        v3 = x - ic2;
        v1 = (c.a1 * ic1 + c.a2 * v3) >> 13;
        v2 = ic2 + ((c.a2 * ic1 + c.a3 * v3) >> 13);
        ic1 = softclip((2 * v1 - ic1) >> 1) << 1;
        ic2 = clamp(2 * v2 - ic2, -150000, 150000);
        x = ((bp ? v1 : v2) * mk) >> 12;
        ph0 += inc1;
        ph1 += inc2;
        out[i] += voice_amp(soft_knee(x, 26000), m, i) << 1;   /* (+6 dB: as loud as the other machines) */
    }
    v->ph[0] = ph0;
    v->ph[1] = ph1;
    v->s[0] = ic1;
    v->s[1] = ic2;
    v->s[2] = nz;
}
/* WAV1 WAV2 DTN MIX CUT RES MODE ENV; the F.ENV, VIB and the phaser of each in EMS_X below */
static const preset_t EMS_PRESETS[] = {
    {"EMS BASS", {0, 2, -12, 50, 30, 70, 0, 70}, {0, 70, 90, 20}, 0, 1, FX(10, 0, 0, 5)},
    {"EMS LEAD", {0, 0, 0, 60, 60, 50, 0, 50}, {5, 70, 110, 35}, 0, 1, FX(0, 0, 30, 30)},
    {"EMS SPACE", {2, 3, 7, 64, 50, 80, 0, 60}, {40, 90, 100, 80}, 0, 0, FX(0, 20, 40, 70)},
    {"EMS QUACK", {3, 0, 0, 40, 20, 105, 1, 100}, {0, 50, 60, 20}, 0, 1, FX(0, 0, 20, 10)},
    {"EMS STRINGS", {0, 1, 0, 64, 70, 20, 0, 20}, {60, 90, 110, 80}, 0, 0, FX(0, 50, 20, 60)},
    {"EMS NOISE SWEEP", {0, 5, 0, 110, 10, 110, 1, 120}, {30, 100, 50, 80}, 0, 0, FX(0, 0, 40, 60)},
    {"EMS RING BELL", {2, 4, 7, 100, 80, 40, 0, 40}, {0, 95, 0, 80}, 0, 0, FX(0, 0, 30, 60)},
    {"EMS SCREAM", {0, 2, 12, 64, 40, 120, 0, 90}, {0, 64, 100, 30}, 0, 1, FX(25, 0, 20, 20)},
};
static const uint8_t EMS_X[NELEM(EMS_PRESETS)][6] = {   /* F.ENV ATK DEC SUS REL, VIB, the phaser's MIX (0: off) */
    {0, 50, 20, 20, 0, 0},
    {10, 70, 60, 40, 60, 40},
    {50, 100, 70, 80, 30, 90},
    {0, 45, 10, 20, 0, 0},
    {60, 90, 90, 70, 40, 80},
    {70, 110, 30, 80, 0, 70},
    {0, 80, 0, 60, 0, 50},
    {0, 60, 40, 30, 70, 30},
};
static const engine_t ENG_EMS = {
    .name = "EMS", .page_title = {"VCO", "VCF"}, .presets = EMS_PRESETS, .npresets = NELEM(EMS_PRESETS),
    .note_on = ems_note_on, .render = ems_render, .knob = {P_E3, P_E4, P_E5, P_E7}, .poly = MF_POLY,
    .edit = {{"WAV1", F_ENUM, 0, 3, 0, N_EMS_W1, 0}, {"WAV2", F_ENUM, 0, 5, 0, N_EMS_W2, 0},
             {"DTN", F_SEMI, -12, 12, 0, 0, 0}, {"MIX", F_PCT, 0, 127, 64, 0, 0},
             {"CUT", F_PCT, 0, 127, 60, 0, 0}, {"RES", F_PCT, 0, 127, 40, 0, 0},
             {"MODE", F_ENUM, 0, 1, 0, N_EMS_MODE, 0}, {"ENV", F_PCT, 0, 127, 60, 0, 0}},
    .e8 = {"VIB", F_PCT, 0, 127, 0, 0, 0},
    .xyd = {{XY_E(4), XY_E(7), 0}, {XY_E(5), XY_E(3), 0}},
    .xya = {{60, 40, 0}, {50, 40, 0}},
};

/* ===================================================================== PREP === */
/* a prepared piano (Tim, 1.11): STRING's waveguide struck by a hammer (eng_string.c, STRIKE; its DTUN the piano's second
 * string), the preparation on it. PREP: NONE, BOLT (a bolt between the strings: the partials pushed apart, a metallic
 * ring of FM partials over the strike and the string's sound ring-modulated off its harmonics), RUBBR (rubber: the string
 * damped, darker, a short dull thud), SCREW (a screw that rattles: the string's reflection clipped on one side, as SITAR's
 * jawari, a buzz), PAPER (paper on the strings: a rasp, noise shaped by the string's own swing). AMT how much of it; HAMR
 * the hammer (felt .. steel); DAMP how long it rings; BRIT its brightness; STIF the stiffness (a piano's sharp partials);
 * POS where it is struck; FM the metallic FM partials' level (any PREP: the tine of an electric piano too, BOLT more).
 * The ninth: UNIS, the second string's detune. Poly 2 (STRING's slots: two strings of 2048 + 1024 samples a part).
 * STRING's knobs are set from these for the render and put back (the audio stream's alone: no command between). ph[0]
 * ph[1] the FM pair, ph[2] the bolt's ring, s[0] the FM's envelope (Q15), s[1] s[2] the rasp's band-pass, s[3] the noise, s[4] the swing followed */
enum { PP_NONE, PP_BOLT, PP_RUBBER, PP_SCREW, PP_PAPER, PP_N };
static const char *const N_PP[] = {"NONE", "BOLT", "RUBBR", "SCREW", "PAPER"};
static void prep_note_on(track_t *t, voice_t *v)
{
    int16_t keep = t->p[P_E0];
    t->p[P_E0] = SR_STRIKE;
    string_note_on(t, v);
    t->p[P_E0] = keep;
    v->ph[0] = v->ph[1] = v->ph[2] = 0;
    v->s[0] = 32767;
    if (mf_fresh(v))
        v->s[1] = v->s[2] = v->s[4] = 0, v->s[3] = 0x2545F49;
}
static void prep_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    int16_t *p = t->p, sv[9];
    uint32_t pp = (uint32_t)clamp(p[P_E0], 0, PP_N - 1), i, j, fm1, fm2, fm3;
    int32_t amt = clamp(p[P_E1], 0, 127), buf[CTL], env = v->s[0], b1 = v->s[1], b2 = v->s[2], nz = v->s[3], fol = v->s[4];
    int32_t fml = clamp(p[P_E7], 0, 127) + (pp == PP_BOLT ? amt : 0), ring = pp == PP_BOLT ? amt * 180 : 0, rasp = pp == PP_PAPER ? amt * 400 : 0;
    tsvf_t c;
    if (n > CTL)
        n = CTL;
    for (j = 0; j < 8u; j++)
        sv[j] = p[P_E0 + j];
    sv[8] = p[P_E8];                                     /* (the ninth is not P_E0 + 8) */
    /* STRING's knobs: MODE STRIKE, PRES the hammer, POS, DAMP, BRIT, STIF, BODY (SITAR's jawari for the screw), BMIX, DTUN */
    p[P_E0] = SR_STRIKE;
    p[P_E1] = sv[2];
    p[P_E2] = sv[6];
    p[P_E3] = (int16_t)clamp(sv[3] - (pp == PP_RUBBER ? amt * 3 / 4 : 0), 0, 127);
    p[P_E4] = (int16_t)clamp(sv[4] - (pp == PP_RUBBER ? amt / 2 : 0), 0, 127);
    p[P_E5] = (int16_t)clamp(sv[5] + (pp == PP_BOLT ? amt / 2 : 0), 0, 127);
    p[P_E6] = pp == PP_SCREW && amt ? SB_SITAR : SB_OFF;
    p[P_E7] = (int16_t)(pp == PP_SCREW ? amt : 0);
    p[P_E8] = sv[8];
    for (i = 0; i < n; i++)
        buf[i] = 0;
    string_render(t, v, buf, n, m);
    for (j = 0; j < 8u; j++)
        p[P_E0 + j] = sv[j];
    p[P_E8] = sv[8];
    /* the FM partials: a carrier at 2.76 x the note, its modulator at 3.91 x (a bar's, a bolt's inharmonic ring), falling away
     * fast (~0.3 s; slower with DAMP) */
    fm1 = (uint32_t)(((uint64_t)m->inc * 181u) >> 6);   /* (2.83) */
    fm2 = (uint32_t)(((uint64_t)m->inc * 250u) >> 6);   /* (3.91) */
    fm3 = (uint32_t)(((uint64_t)m->inc * 154u) >> 6);   /* (2.41: the ring) */
    tsvf_coef(&c, 100 << 8, 30);                         /* (the rasp's band: ~5 kHz, wide) */
    for (i = 0; i < n; i++) {
        int32_t y = buf[i], a;
        if (pp == PP_RUBBER)                             /* (the damped string made up: up to x 2.4) */
            y += (y * amt) / 90;
        a = y < 0 ? -y : y;
        fol += (a - fol) >> 6;                           /* (the string's swing, followed: ~1.5 ms) */
        if (ring) {                                      /* BOLT: the string ring-modulated at 2.41 x (off its harmonics) */
            y += (int32_t)((((int64_t)y * osc_sine(v->ph[2])) >> 15) * ring >> 15);   /* (y may pass 2^16) */
            v->ph[2] += fm3;
        }
        if (fml && env > 8) {
            int32_t mo = osc_sine(v->ph[1]);
            int32_t s = osc_sine(v->ph[0] + (uint32_t)(mo * (env >> 4)) * 64u);
            y += voice_amp(mulq15(s, mulq15(env, fml * 120)), m, i);
            v->ph[0] += fm1;
            v->ph[1] += fm2;
        }
        if (rasp) {                                      /* PAPER: noise shaped by the swing, band-passed */
            int32_t nn = (int32_t)(noise32(&nz) >> 17) - 16384, v1, v2, v3;
            nn = (int32_t)(((int64_t)nn * fol) >> 13);
            v3 = nn - b2;
            v1 = (c.a1 * b1 + c.a2 * v3) >> 13;
            v2 = b2 + ((c.a2 * b1 + c.a3 * v3) >> 13);
            b1 = clamp(2 * v1 - b1, -150000, 150000);
            b2 = clamp(2 * v2 - b2, -150000, 150000);
            y += (int32_t)(((int64_t)v1 * rasp) >> 15);
        }
        out[i] += y;
    }
    env = (int32_t)(((int64_t)env * (32768 - 80 + clamp(p[P_E3], 0, 127) * 60 / 127)) >> 15);   /* (a block's fall: ~0.3 s .. ~1.2 s) */
    v->s[0] = env;
    v->s[1] = b1;
    v->s[2] = b2;
    v->s[3] = nz;
    v->s[4] = fol;
}
/* PREP AMT HAMR DAMP BRIT STIF POS FM (UNIS) */
static const preset_t PREP_PRESETS[] = {
    {"PP BOLTS", {PP_BOLT, 70, 80, 80, 70, 40, 40, 30}, {0, 100, 127, 40}, 0, 0, FX(0, 0, 10, 40)},
    {"PP RUBBER", {PP_RUBBER, 90, 60, 60, 60, 20, 40, 0}, {0, 80, 127, 30}, 0, 0, FX(0, 0, 0, 25)},
    {"PP SCREWS", {PP_SCREW, 80, 85, 75, 75, 30, 35, 10}, {0, 100, 127, 40}, 0, 0, FX(0, 0, 10, 35)},
    {"PP PAPER", {PP_PAPER, 70, 70, 70, 65, 25, 40, 0}, {0, 100, 127, 35}, 0, 0, FX(0, 0, 0, 35)},
    {"PP CAGE", {PP_BOLT, 110, 100, 85, 80, 60, 30, 60}, {0, 100, 127, 50}, 0, 0, FX(0, 0, 20, 50)},
    {"PP MUTED KEYS", {PP_RUBBER, 127, 70, 40, 50, 10, 50, 10}, {0, 70, 127, 20}, 0, 0, FX(0, 0, 25, 30)},
    {"PP PIANO", {PP_NONE, 0, 75, 90, 80, 45, 35, 0}, {0, 100, 127, 45}, 0, 0, FX(0, 10, 0, 45)},
    {"PP TINE", {PP_NONE, 0, 90, 80, 70, 30, 40, 70}, {0, 100, 127, 40}, 0, 0, FX(0, 20, 10, 35)},
};
static const engine_t ENG_PREP = {
    .name = "PREP", .page_title = {"PREP", "STRING"}, .presets = PREP_PRESETS, .npresets = NELEM(PREP_PRESETS),
    .note_on = prep_note_on, .render = prep_render, .knob = {P_E1, P_E2, P_E3, P_E7}, .poly = SR_POLY,
    .mem = sizeof(sr_slot_t) * SR_POLY, .sampled = 0,
    .edit = {{"PREP", F_ENUM, 0, PP_N - 1, PP_BOLT, N_PP, 0}, {"AMT", F_PCT, 0, 127, 70, 0, 0},
             {"HAMR", F_PCT, 0, 127, 80, 0, 0}, {"DAMP", F_PCT, 0, 127, 80, 0, 0},
             {"BRIT", F_PCT, 0, 127, 70, 0, 0}, {"STIF", F_PCT, 0, 127, 40, 0, 0},
             {"POS", F_PCT, 0, 127, 40, 0, 0}, {"FM", F_PCT, 0, 127, 20, 0, 0}},
    .e8 = {"UNIS", F_PCT, 0, 127, 30, 0, 0},
    .xyd = {{XY_E(1), XY_E(7), 0}, {XY_E(2), XY_E(3), 0}},
    .xya = {{50, 40, 0}, {40, 40, 0}},
};

/* ==================================================================== ETHER === */
/* an anti-radio (Tim, 1.11; after Soma Laboratory's ETHER, which turns the electromagnetic fields around it into sound): the
 * fields of the things around us, made: SRC MAINS (the 50 Hz hum, full-wave, its harmonics: a fat buzz), SMPS (a switching
 * supply's whine, its jitter, its burst mode's ticks), LED (a dimmer's PWM, the mains' 100 Hz ripple on it), DIGI (data: random
 * bits, packets on and off), MOTOR (a brushed motor: its commutation's noise, the rotation), GSM (a phone's TDMA bursts: 217 Hz
 * frames, the "dit-dit-dit" in a speaker), STATIC (the band's noise, its pops, a heterodyne's whistle). NEAR how close the
 * antenna is (louder, dirtier, more harmonics); FLUX the field's unrest (the frequency wanders, the level drops out); RATE the
 * bursts' rate (~0.3 .. 20 Hz); TONE the antenna's low-pass; GRIT a sample-and-hold and a crush; BLEND / SRC2 a second field
 * mixed in. The ninth, KEY: ON the notes set the field's frequency (playable), OFF each field at its own (a note opens it).
 * ph[0] the carrier, ph[1] the bursts, ph[2] the second rate (GSM's frames, LED's ripple, MOTOR's commutation); s[0] the noise,
 * s[1] s[2] the antenna, s[3] FLUX's walk (cents), s[4] the burst's gate (Q15), s[5] DIGI's bit / STATIC's pop, s[6] s[7] GRIT */
enum { ET_MAINS, ET_SMPS, ET_LED, ET_DIGI, ET_MOTOR, ET_GSM, ET_STATIC, ET_N };
static const char *const N_ET_SRC[] = {"MAINS", "SMPS", "LED", "DIGI", "MOTOR", "GSM", "STATC"};
static const char *const N_ET_KEY[] = {"OFF", "ON"};
#define ET_HZ(h) ((uint32_t)((h) * 97391.5f))                /* Hz -> a phase increment at 44.1 kHz */
static const uint32_t ET_FREE[ET_N] = {ET_HZ(50), ET_HZ(1820), ET_HZ(240), ET_HZ(4100), ET_HZ(95), ET_HZ(1733), ET_HZ(900)};
static const uint8_t ET_MULT[ET_N] = {1, 16, 4, 32, 1, 8, 8};   /* KEY ON: the field's frequency, the note's x this */
static void ether_note_on(track_t *t, voice_t *v)
{
    (void)t;
    if (mf_fresh(v)) {
        v->ph[0] = v->ph[1] = v->ph[2] = 0;
        v->s[0] = 0x6C8E9CF5;
        v->s[1] = v->s[2] = v->s[3] = v->s[5] = v->s[6] = v->s[7] = 0;
        v->s[4] = 32767;
    }
}
/* one field's sample (Q15-ish, +-32767) at the carrier's phase c, the second rate's r; nz a fresh random number */
static int32_t et_field(uint32_t src, uint32_t c, uint32_t inc, uint32_t r, int32_t near, uint32_t nz, int32_t *bit)
{
    int32_t s, a;
    switch (src) {
    case ET_MAINS:                                       /* the hum: its fundamental, the full wave's 2nd and up, clipped near */
        s = osc_sine(c);
        a = s < 0 ? -s : s;
        s = (s >> 1) + ((a - 20860) * (32 + near)) / 96;
        return softclip(s + ((s * near) >> 6));
    case ET_SMPS:                                        /* narrow pulses (10 %), the switch's ring after each */
        return ((c >> 16) < 6554u ? 26000 : -2900) + ((int32_t)(nz >> 20) - 2048) * near / 16;
    case ET_LED:                                         /* PWM, its duty breathing on the 100 Hz ripple */
        s = (c >> 16) < (uint32_t)(24000 + (osc_sine(r) >> 3)) ? 18000 : -18000;
        return s - osc_saw(c, inc) / 4;
    case ET_DIGI:                                        /* a random bit a period, NRZ */
        if (c < inc)
            *bit = (nz >> 31) ? 22000 : -22000;
        return *bit + (c < inc * 2u ? (int32_t)(nz >> 18) - 8192 : 0);   /* (each edge a little ring) */
    case ET_MOTOR:                                       /* the rotation's tone, the brushes' noise at each commutation */
        a = (r >> 16) < 9000u ? 32767 - (int32_t)(r >> 17) * 2 : 0;
        return (osc_sine(c) >> 1) + (int32_t)(((int64_t)((int32_t)(nz >> 16) - 32768) * a) >> 15) * (40 + near) / 128;
    case ET_GSM:                                         /* TDMA: a burst 1/8 of each 4.6 ms frame, a square buzz in it */
        return (r >> 29) == 0u ? ((c >> 31) ? 24000 : -24000) : 0;
    default:                                             /* STATIC: the band's noise, a whistle drifting through it */
        return ((int32_t)(nz >> 17) - 16384) + (osc_sine(c) * near >> 8);
    }
}
static void ether_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t src = (uint32_t)clamp(p[P_E0], 0, ET_N - 1), src2 = (uint32_t)clamp(p[P_E7], 0, ET_N - 1), i, inc, inc2, rinc, binc;
    int32_t near = clamp(p[P_E1], 0, 127), flux = clamp(p[P_E2], 0, 127), grit = clamp(p[P_E5], 0, 127), bl = clamp(p[P_E6], 0, 127);
    int32_t nz = v->s[0], ic1 = v->s[1], ic2 = v->s[2], walk = v->s[3], gate = v->s[4], bit = v->s[5], hold = v->s[6], hc = v->s[7];
    int32_t bit2 = bit, gt, cut, lvl;
    uint32_t c = v->ph[0], b = v->ph[1], r = v->ph[2], key = MF_E8(p) != 0, rnd;
    tsvf_t f;
    /* FLUX: a random walk of the frequency (up to ~+-3 semitones), steered back to the middle */
    rnd = noise32(&nz);
    walk += ((int32_t)(rnd >> 21) - 1024) * flux / 64 - walk / 16;
    walk = clamp(walk, -300 * 16, 300 * 16);
    inc = key ? cents_inc(m->pitch16, walk / 16, m->fine) : ET_FREE[src];
    if (key) {
        uint64_t x = (uint64_t)inc * ET_MULT[src];
        inc = x > 0x3FFFFFFFu ? 0x3FFFFFFFu : (uint32_t)x;
    } else if (walk) {
        inc = (uint32_t)((int64_t)inc + (((int64_t)inc * walk) >> 15));
    }
    inc2 = key ? (uint32_t)(((uint64_t)inc * ET_MULT[src2]) / ET_MULT[src]) : ET_FREE[src2];
    /* the second rate: GSM's frames 217 Hz, LED's ripple 100 Hz, MOTOR's commutation 6 x the rotation */
    rinc = src == ET_GSM || src2 == ET_GSM ? ET_HZ(217) : src == ET_LED ? ET_HZ(100) : (uint32_t)(((uint64_t)inc * 6u) & 0x3FFFFFFFu);
    binc = ET_HZ(0.3f) + (uint32_t)clamp(p[P_E3], 0, 127) * (ET_HZ(20) / 127u);   /* RATE: the bursts */
    /* the bursts' gate: SMPS's burst mode, DIGI's packets, GSM's groups, STATIC's pops; FLUX drops the rest out now and then */
    if (b + binc * n < b) {                              /* (a new burst: on or off, by chance) */
        rnd = noise32(&nz);
        gt = src == ET_MAINS || src == ET_LED || src == ET_MOTOR ? (rnd >> 25) >= (uint32_t)flux / 2u : (rnd >> 31) || src == ET_GSM;
    } else
        gt = gate > 0;
    gate += ((gt ? 32767 : 0) - gate) >> 2;
    cut = (30 << 8) + clamp(p[P_E4], 0, 127) * 190 + m->cutoff;   /* TONE: the antenna's low-pass, ~120 Hz .. 16 kHz */
    tsvf_coef(&f, cut, 20);
    lvl = 9000 + near * 120;                             /* (NEAR: the level too) */
    for (i = 0; i < n; i++) {
        uint32_t z = noise32(&nz);
        int32_t x = et_field(src, c, inc, r, near, z, &bit);
        if (src == ET_GSM || src == ET_SMPS || src == ET_DIGI)
            x = (int32_t)(((int64_t)x * gate) >> 15);   /* (the bursts) */
        if (src == ET_STATIC && (z & 0x3FFFu) < (uint32_t)(4 + flux))   /* (a pop) */
            x += (z >> 31) ? 30000 : -30000;
        if (bl)
            x += ((et_field(src2, c, inc2, r, near, z ^ 0x5A5A5A5Au, &bit2) - x) * bl) >> 7;
        if (grit) {                                      /* GRIT: held 1 .. 16 samples, 12 .. 4 bits */
            if (++hc > grit / 8) {
                hc = 0;
                hold = x & ~((1 << (3 + grit / 12)) - 1);
            }
            x = hold;
        }
        x = tsvf_lp(&f, x, &ic1, &ic2);
        x = soft_knee((x * lvl) >> 14, 26000);
        out[i] += voice_amp(x, m, i);
        c += src == ET_STATIC ? inc + (uint32_t)((int32_t)(z >> 22) * flux) : inc;   /* (STATIC: the whistle wavers) */
        r += rinc;
    }
    b += binc * n;
    v->ph[0] = c;
    v->ph[1] = b;
    v->ph[2] = r;
    v->s[0] = nz;
    v->s[1] = ic1;
    v->s[2] = ic2;
    v->s[3] = walk;
    v->s[4] = gate;
    v->s[5] = bit;
    v->s[6] = hold;
    v->s[7] = hc;
}
/* SRC NEAR FLUX RATE TONE GRIT BLEND SRC2 (KEY: E8, the presets below set it in ETHER_KEY) */
static const preset_t ETHER_PRESETS[] = {
    {"ET KITCHEN", {ET_MAINS, 70, 20, 30, 70, 0, 40, ET_SMPS}, {5, 64, 127, 40}, 0, 0, FX(0, 0, 0, 20)},
    {"ET SERVER ROOM", {ET_DIGI, 60, 40, 70, 90, 20, 50, ET_SMPS}, {5, 64, 127, 40}, 0, 0, FX(0, 0, 20, 30)},
    {"ET TRAM", {ET_MOTOR, 90, 60, 20, 60, 10, 30, ET_MAINS}, {20, 64, 127, 60}, 0, 0, FX(10, 0, 0, 40)},
    {"ET PHONE ON DESK", {ET_GSM, 80, 30, 50, 80, 0, 0, ET_GSM}, {0, 64, 127, 30}, 0, 0, FX(0, 0, 0, 10)},
    {"ET OLD TV", {ET_SMPS, 60, 30, 10, 110, 30, 50, ET_STATIC}, {10, 64, 127, 50}, 0, 0, FX(0, 0, 0, 30)},
    {"ET LED STRIP", {ET_LED, 70, 20, 30, 80, 10, 30, ET_MAINS}, {5, 64, 127, 40}, 0, 0, FX(0, 20, 0, 30)},
    {"ET STORM", {ET_STATIC, 80, 90, 40, 70, 20, 30, ET_MAINS}, {30, 64, 127, 80}, 0, 0, FX(0, 0, 30, 60)},
    {"ET HUM BASS", {ET_MAINS, 100, 10, 0, 40, 0, 0, ET_MAINS}, {0, 70, 100, 20}, 0, 1, FX(10, 0, 0, 5)},
};
static const uint8_t ETHER_KEY[NELEM(ETHER_PRESETS)] = {0, 0, 0, 0, 0, 1, 0, 1};   /* KEY per preset (LED STRIP, HUM BASS: played) */
static const engine_t ENG_ETHER = {
    .name = "ETHER", .page_title = {"FIELD", "ANTENNA"}, .presets = ETHER_PRESETS, .npresets = NELEM(ETHER_PRESETS),
    .note_on = ether_note_on, .render = ether_render, .knob = {P_E1, P_E2, P_E3, P_E4}, .poly = MF_POLY,
    .edit = {{"SRC", F_ENUM, 0, ET_N - 1, ET_MAINS, N_ET_SRC, 0}, {"NEAR", F_PCT, 0, 127, 70, 0, 0},
             {"FLUX", F_PCT, 0, 127, 30, 0, 0}, {"RATE", F_PCT, 0, 127, 40, 0, 0},
             {"TONE", F_PCT, 0, 127, 80, 0, 0}, {"GRIT", F_PCT, 0, 127, 0, 0, 0},
             {"BLND", F_PCT, 0, 127, 0, 0, 0}, {"SRC2", F_ENUM, 0, ET_N - 1, ET_SMPS, N_ET_SRC, 0}},
    .e8 = {"KEY", F_ENUM, 0, 1, 0, N_ET_KEY, 0},
    .xyd = {{XY_E(1), XY_E(4), 0}, {XY_E(2), XY_E(6), 0}},
    .xya = {{50, 40, 0}, {50, 40, 0}},
};
