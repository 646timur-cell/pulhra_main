/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Tim (646timur-cell), on Felucca by Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* LYRA (STRANGE): an organismic drone synth after SOMA's Lyra-8. Not a copy of its circuit: its way of playing.
 *
 * Each note is a PAIR of oscillators, as the Lyra's voices come in pairs: A on the note, B at the pair's interval
 * (TUNE). Both drift a little on their own (the oscillators of an organism are never quite still), so a pair beats.
 * Four notes play at once (eight oscillators: the Lyra's eight voices), the two halves of the part's notes are its
 * two quads.
 *   SHRP   each oscillator's sharpness: a sine driven into a tanh, round -> almost square (the Lyra's SHARP)
 *   MOD    how deep the oscillators modulate each other's phase (FM), from nothing to wild
 *   ROUT   what modulates them: SELF the pair each other (A by B, B by A: the Lyra's TOTAL FB), LFO the hyper-LFO,
 *          CROSS every other note of the part (the other quad), ALL the three together
 *   HLFO   the hyper-LFO: two triangles at a rate and that rate x sqrt 2, summed; from a slow sway (0.05 Hz) to
 *          audio rate (~400 Hz). It modulates the voices (ROUT LFO, ALL) and the delay's time (DMOD)
 *   TIME   the delay (the Lyra's is half its sound): two taps, TIME and TIME x 0.618, ~10 .. 740 ms (0: off)
 *   FDBK   the delay's feedback through a tanh: above ~3/4 it saturates, near the top it sings on its own
 *   DMOD   how much the hyper-LFO moves the delay's time (a tape's wow, then a warble, then a FM of the echoes)
 * The track's ATK / REL are the Lyra's FAST / SLOW; the INSERT or DIST the drive of its VOLUME.
 *
 * CROSS hears the other notes one block late (CTL = 32 samples, 0.7 ms): the voices render one after another. The
 * delay runs at half the rate (a darker line, twice as long in the memory) and is the part's (engine_t.post): it rings
 * on after the last note. Memory: the part's engine region (lyra_mem_t). */

#define LY_POLY 4
static uint8_t spread_on;                                /* (voice.c: SPREAD of the part being rendered) */
static int32_t spread_buf[CTL];
#if FELUCCA_MONOFM
#define LY_DLEN 12288u                                   /* MONOFM (four parts' memory): 0.56 s at FS / 2, TIME tops out there */
#define LY_AT(w, a) ((w) >= (a) ? (w) - (a) : (w) + LY_DLEN - (a))   /* (a ring of compares: not a power of two) */
#define LY_NEXT(w) ((w) + 1u >= LY_DLEN ? 0u : (w) + 1u)
#define LY_W(w) (w)
#else
#define LY_DLEN 16384u                                   /* the delay line, at FS / 2: 0.74 s */
#define LY_AT(w, a) (((w) - (a)) & (LY_DLEN - 1u))
#define LY_NEXT(w) ((w) + 1u)
#define LY_W(w) ((w) & (LY_DLEN - 1u))
#endif

typedef struct {
    int16_t own[NVOICE][CTL];                            /* each voice's block before (CROSS: the others' share) */
    int32_t sum_prev[CTL], sum_cur[CTL];                 /* every voice of the block before, of this one */
    uint32_t la, lb, lia, lib;                           /* the hyper-LFO's phases at this block's start, increments */
    int16_t hl[CTL];                                     /* .. and its value over the block (lyra_block) */
    uint32_t dw;                                         /* the delay's write index */
    int32_t dtime;                                       /* its time, smoothed (half-rate samples, Q16) */
    int32_t dlp, wlp, wprev;                             /* the loop's low-pass, the wet's, the last wet sample */
    int32_t live;                                        /* blocks the tail still rings (post) */
    int16_t dl[LY_DLEN];
} lyra_mem_t;
ENG_MEM_FITS("LYRA", sizeof(lyra_mem_t));

static const char *const N_LY_TUNE[] = {"UNI", "OCT", "5TH", "4TH", "MAJ3", "MIN3", "TRIT", "9TH", "SUB", "CLST"};
static const int16_t LY_TUNE_CT[] = {9, 1200, 702, 498, 386, 316, 600, 1404, -1200, 100};   /* B over A, cents */
static const char *const N_LY_ROUT[] = {"SELF", "LFO", "CROSS", "ALL"};
enum { LR_SELF, LR_LFO, LR_CROSS, LR_ALL };

static lyra_mem_t *lyra_mem(const track_t *t) { return (lyra_mem_t *)ENG_MEM(t - trk); }

/* the hyper-LFO's two increments: HLFO 0..127 -> 0.05 Hz .. ~410 Hz (13 octaves), the second x sqrt 2 */
static void lyra_lfo_inc(const track_t *t, uint32_t *ia, uint32_t *ib)
{
    *ia = sx_hz_inc(sx_expmap(t->p[P_E4], 3277u, 13));
    *ib = *ia + (uint32_t)(((uint64_t)*ia * 27146u) >> 16);   /* x 1.414 */
}

static void lyra_note_on(track_t *t, voice_t *v)
{
    (void)t;
    if (!v->s[6])
        v->s[6] = (int32_t)(0x1A2B3C4Du ^ (v->age * 2654435761u)) | 1;
}

/* once a block, before the voices: this block's sums start, a voice that stopped has no share; the hyper-LFO */
static void lyra_block(track_t *t)
{
    lyra_mem_t *L = lyra_mem(t);
    uint32_t i, k, la = L->la, lb = L->lb;
    lyra_lfo_inc(t, &L->lia, &L->lib);
    for (i = 0; i < CTL; i++) {
        L->sum_prev[i] = L->sum_cur[i];
        L->sum_cur[i] = 0;
        L->hl[i] = (int16_t)((osc_tri(la) + osc_tri(lb)) >> 1);
        la += L->lia;
        lb += L->lib;
    }
    for (k = 0; k < NVOICE; k++)
        if (!t->v[k].active)
            for (i = 0; i < CTL; i++)
                L->own[k][i] = 0;
}

static void lyra_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    static const int16_t WSELF[4] = {32767, 0, 0, 10923}, WEXT[4] = {0, 32767, 32767, 10923};   /* SELF LFO CROSS ALL */
    const int16_t *p = t->p;
    lyra_mem_t *L = lyra_mem(t);
    uint32_t k = (uint32_t)(v - t->v) % NVOICE, i, rout = (uint32_t)clamp(p[P_E3], 0, 3);
    uint32_t tune = (uint32_t)clamp(p[P_E0], 0, (int32_t)NELEM(LY_TUNE_CT) - 1);
    uint32_t pa = v->ph[0], pb = v->ph[1], ia, ib;
    int32_t sh = p[P_E1], md = p[P_E2], g, dk, ya = v->s[0], yb = v->s[1], da, db, ws = WSELF[rout], we = WEXT[rout];
    int32_t ext[CTL];
    uint32_t seed = (uint32_t)v->s[6] | 1u;
    if (n > CTL)
        n = CTL;
    /* the drift: a slow random walk per oscillator, +-7 cents (in TUNE's 1/4096 steps: 2.367 a cent) */
    da = v->s[4] = sx_walk(v->s[4], &seed, 3, 16);
    db = v->s[5] = sx_walk(v->s[5], &seed, 3, 16);
    ia = m->inc + (uint32_t)((int32_t)(m->inc >> 12) * da);
    ib = cents_inc(m->pitch16, LY_TUNE_CT[tune], m->fine + db);
    g = 4096 + sh * sh * 3;                              /* SHRP: x1 .. x12 into the tanh (Q12) */
    dk = (md * md * 10431) >> 14;                        /* MOD: up to ~8 radians of phase per full-scale source */
    if (m->cutoff > 0)                                   /* the track's ENV / LFO -> FLT open the sharpness too */
        g += m->cutoff >> 1;
    /* what modulates both oscillators from outside the pair: the hyper-LFO (LFO), the other notes of the block before
     * (CROSS), a third of each (ALL; the other third is the pair's own: SELF) */
    for (i = 0; i < n; i++) {
        int32_t x = rout == LR_LFO ? L->hl[i] : clamp((L->sum_prev[i] - L->own[k][i]) >> 1, -32767, 32767);
        if (rout == LR_ALL)
            x = (x + L->hl[i]) >> 1;
        ext[i] = mulq15(x, we) * dk;
    }
    for (i = 0; i < n; i++) {
        int32_t a, b, s;
        a = sine_i(pa + ((uint32_t)(mulq15(yb, ws) * dk + ext[i]) << 4));
        b = sine_i(pb + ((uint32_t)(mulq15(ya, ws) * dk + ext[i]) << 4));
        ya = softclip((a * g) >> 12);
        yb = softclip((b * g) >> 12);
        pa += ia;
        pb += ib;
        s = (ya + yb) >> 1;
        L->own[k][i] = (int16_t)s;
        L->sum_cur[i] += s;
        out[i] += voice_amp(s, m, i);
    }
    v->ph[0] = pa;
    v->ph[1] = pb;
    v->s[0] = ya;
    v->s[1] = yb;
    v->s[6] = (int32_t)seed;
}

/* the delay, on the part's sum (after the voices), at half rate; returns 1 while it rings */
static uint32_t lyra_post(track_t *t, int32_t *out, uint32_t n)
{
    const int16_t *p = t->p;
    lyra_mem_t *L = lyra_mem(t);
    uint32_t i, w = L->dw;
    int32_t time = p[P_E5], fb = p[P_E6] * 147, dmod = p[P_E7], tgt, d0, d1, dd, hl, peak = 0;   /* fb Q14: 0 .. 1.14 */
    int32_t klp = 9000 - p[P_E6] * 30;                   /* the loop darker as it feeds back more */
    hl = L->hl[0];                                       /* the hyper-LFO at the block's start (Q15) */
    L->la += L->lia * n;                                 /* .. its phases on to the next block */
    L->lb += L->lib * n;
    if (!time) {                                         /* TIME 0: off (the line empties as it is read: none) */
        L->live = 0;
        return 0;
    }
    /* 10 ms .. 740 ms (6.2 octaves) in half-rate samples (22.05 a ms), Q16; DMOD: up to +-45 % by the hyper-LFO */
    tgt = (int32_t)(((uint64_t)sx_expmap(time, 10u << 16, 6) * 1445069u) >> 16);   /* x 22.05 (Q16) */
    tgt += (int32_t)((((int64_t)tgt * hl >> 15) * dmod * 232) >> 16);              /* x DMOD / 282 */
    tgt = clamp(tgt, 4 << 16, (int32_t)(LY_DLEN - 4u) << 16);
    if (!L->dtime)
        L->dtime = tgt;
    d0 = L->dtime;
    d1 = d0 + ((tgt - d0) >> 3);                         /* smoothed: no zipper on a turn or a fast LFO */
    dd = (d1 - d0) / (int32_t)(n >> 1);
    L->dtime = d1;
    for (i = 0; i + 1u < n; i += 2u) {
        int32_t wa, wb;
        int32_t x = (out[i] + out[i + 1u]) >> 2, y1, y2, y, d = d0, wet;
        uint32_t a, f;
        a = (uint32_t)d >> 16;                           /* tap 1: TIME */
        f = ((uint32_t)d >> 1) & 0x7FFFu;
        y1 = L->dl[LY_AT(w, a)];
        y1 += ((L->dl[LY_AT(w, a + 1u)] - y1) * (int32_t)f) >> 15;
        d = (int32_t)(((int64_t)d * 40501) >> 16);       /* tap 2: TIME x 0.618 */
        a = (uint32_t)d >> 16;
        f = ((uint32_t)d >> 1) & 0x7FFFu;
        y2 = L->dl[LY_AT(w, a)];
        y2 += ((L->dl[LY_AT(w, a + 1u)] - y2) * (int32_t)f) >> 15;
        y = (y1 + y2) >> 1;
        L->dlp += mulq15(y - L->dlp, klp);
        L->dl[LY_W(w)] = (int16_t)softclip((x >> 1) + ((L->dlp * fb) >> 14));   /* the drive in the loop */
        w = LY_NEXT(w);
        d0 += dd;
        L->wlp += mulq15(y - L->wlp, 14000);             /* the wet, smoothed (half rate: its images) */
        wet = L->wlp;
        wa = (wet + L->wprev) * 3 >> 2;                  /* (half rate -> full: the sample between, interpolated) */
        wb = (wet * 3) >> 1;
        out[i] += wa;
        out[i + 1u] += wb;
        if (spread_on) {                                 /* SPREAD: the echoes in the middle (half in the left sum) */
            spread_buf[i] += wa >> 1;
            spread_buf[i + 1u] += wb >> 1;
        }
        L->wprev = wet;
        if ((wet < 0 ? -wet : wet) > peak)
            peak = wet < 0 ? -wet : wet;
    }
    L->dw = w;
    L->live = peak > 12 ? 24 : L->live > 0 ? L->live - 1 : 0;
    return L->live > 0;
}

#define STRANGE_CAT_LYRA "PPPLFPFPPFPP"
static const preset_t LYRA_PRESETS[] = {
    /* name, {TUNE, SHRP, MOD, ROUT, HLFO, TIME, FDBK, DMOD}, {A D S R}, fenv, mono */
    {"ORGANISM", {0, 30, 40, LR_SELF, 30, 78, 70, 24}, {70, 64, 127, 90}, 0, 0, FX(0, 10, 0, 50)},
    {"FIFTH SWARM", {2, 55, 58, LR_CROSS, 22, 92, 78, 40}, {84, 64, 127, 100}, 0, 0, FX(0, 0, 0, 60)},
    {"WARM BEAT", {0, 10, 12, LR_LFO, 18, 60, 52, 18}, {60, 64, 127, 84}, 0, 0, FX(0, 20, 0, 40)},
    {"SQUARE LEAD", {1, 110, 30, LR_SELF, 44, 50, 60, 10}, {10, 64, 127, 50}, 0, 0, FX(10, 0, 0, 30)},
    {"HYPER FM", {6, 64, 90, LR_LFO, 100, 70, 74, 60}, {40, 64, 127, 70}, 0, 0, FX(0, 0, 0, 40)},
    {"DRONE OCT", {8, 40, 46, LR_ALL, 12, 104, 92, 34}, {100, 64, 127, 110}, 0, 0, FX(0, 0, 0, 70)},
    {"FEEDBACK", {9, 80, 70, LR_ALL, 60, 84, 118, 70}, {50, 64, 127, 90}, 0, 0, FX(8, 0, 0, 40)},
    /* textures */
    {"DUST VEIL", {0, 20, 30, LR_LFO, 8, 110, 96, 60}, {110, 64, 127, 120}, 0, 0, FX(0, 30, 0, 100)},
    {"GLASS CHOIR", {1, 8, 18, LR_SELF, 14, 100, 88, 30}, {100, 64, 127, 115}, 0, 0, FX(0, 40, 0, 110)},
    {"SWARM CLOUD", {9, 60, 70, LR_CROSS, 40, 120, 104, 90}, {90, 64, 127, 120}, 0, 0, FX(0, 0, 0, 90)},
    {"SLOW TIDE", {2, 30, 24, LR_ALL, 4, 124, 108, 100}, {120, 64, 127, 127}, 0, 0, FX(0, 20, 0, 100)},
    {"RUST ORGAN", {8, 90, 50, LR_SELF, 20, 70, 100, 40}, {80, 64, 127, 110}, 0, 0, FX(20, 0, 0, 80)},
};

static const engine_t ENG_LYRA = {
    .name = "LYRA",
    .page_title = {"PAIRS", "DELAY"},
    .edit = {
        {"TUNE", F_ENUM, 0, (int16_t)(NELEM(N_LY_TUNE) - 1), 0, N_LY_TUNE, 0},
        {"SHRP", F_PCT, 0, 127, 30, 0, 0},
        {"MOD", F_PCT, 0, 127, 40, 0, 0},
        {"ROUT", F_ENUM, 0, 3, 0, N_LY_ROUT, 0},
        {"HLFO", F_PCT, 0, 127, 30, 0, 0},
        {"TIME", F_PCT, 0, 127, 78, 0, 0},
        {"FDBK", F_PCT, 0, 127, 70, 0, 0},
        {"DMOD", F_PCT, 0, 127, 24, 0, 0},
    },
    .presets = LYRA_PRESETS,
    .npresets = NELEM(LYRA_PRESETS),
    .note_on = lyra_note_on,
    .render = lyra_render,
    .block = lyra_block,
    .post = lyra_post,
    .knob = {P_E2, P_E1, P_E5, P_E6},
    .poly = LY_POLY,
    .keep = 0x73,                                        /* the last outputs, the drifts and their random numbers */
    .mem = sizeof(lyra_mem_t),
    .xyd = {{XY_E(2), XY_E(1), XY_E(4)}, {XY_E(5), XY_E(6), XY_E(7)}},   /* X: MOD SHRP HLFO (the chaos of the pairs), Y: TIME FDBK DMOD (the space) */
    .xya = {{40, 30, 30}, {30, 35, 40}},
};
