/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Tim (646timur-cell), on Felucca by Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* DRIFT (STRANGE): a drifting memory, after SOMA's COSMOS (not its circuit: its way). The notes are simple sounds; what
 * they leave goes into two loops of memory whose heads drift: each pass comes back a little slower or faster, a little
 * duller, mixed with the other loop and with what plays now, and slowly wears away (or never, ERAS at the top: it
 * grows into a texture of its own). The memory is lo-fi on purpose: a quarter of the rate (11,025 Hz) and 8 bits
 * (companded), 1.5 s and 1.1 s long.
 *   SRC    the notes' sound: SINE (with its octave), SAW (two, detuned), NOISE (a band at the note), PLUCK (a sine that
 *          dies away by itself)
 *   TONE   their brightness
 *   DRFT   how far the heads drift (their speed wanders: a tape's wow, then a seasick pitch)
 *   MOTN   how fast they wander
 *   ERAS   how long the memory keeps: a few passes .. for ever
 *   BLUR   each pass duller and smeared
 *   SIZE   the loops' lengths, 1/4 .. all of them (short: a flutter, long: whole phrases)
 *   MIX    the memory against the notes
 * The memory is the part's (engine_t.post): it plays on after the last note. Poly 4. */

#define DR_POLY 4
#define DR_LA 16384u                                     /* loop A: 1.49 s at FS / 4 */
#define DR_LB 12288u                                     /* loop B: 1.11 s */
enum { DS_SINE, DS_SAW, DS_NOISE, DS_PLUCK, DS_N };
static const char *const N_DR_SRC[] = {"SINE", "SAW", "NOISE", "PLUCK"};

typedef struct {
    uint32_t wa, wb, rng;                                /* the write indices; random numbers */
    int32_t walk_a, walk_b;                              /* the heads' drift (Q16 samples) */
    int32_t da, db;                                      /* their read distances, smoothed (Q16) */
    int32_t lpa, lpb, acc, wprev;                        /* the loops' blur; the input's sum (4 to 1); the last wet */
    int32_t live;
    int8_t la[DR_LA], lb[DR_LB];
} drift_mem_t;
ENG_MEM_FITS("DRIFT", sizeof(drift_mem_t));

/* the 8-bit companding: x (Q15) -> code (sign, then the square root of the size, 7 bits); back through a table */
static int16_t dr_dec[128];
static int8_t dr_enc(int32_t x)
{
    uint32_t a = (uint32_t)(x < 0 ? -x : x), lo = 0, hi = 127;
    if (!dr_dec[127]) {
        uint32_t i;
        for (i = 0; i < 128u; i++)
            dr_dec[i] = (int16_t)(i * i * 32767u / (127u * 127u));
    }
    if (a > 32767u)
        a = 32767u;
    while (lo < hi) {                                    /* the nearest code at or below a, then the closer one */
        uint32_t mid = (lo + hi + 1u) >> 1;
        if ((uint32_t)dr_dec[mid] <= a)
            lo = mid;
        else
            hi = mid - 1u;
    }
    if (lo < 127u && a - (uint32_t)dr_dec[lo] > (uint32_t)dr_dec[lo + 1u] - a)
        lo++;
    return (int8_t)(x < 0 ? -(int32_t)lo : (int32_t)lo);
}
static inline int32_t dr_get(const int8_t *b, uint32_t mask_len, uint32_t i)
{
    int32_t c = b[i % mask_len];
    return c < 0 ? -dr_dec[-c] : dr_dec[c];
}

static drift_mem_t *drift_mem(const track_t *t) { return (drift_mem_t *)ENG_MEM(t - trk); }

static void drift_note_on(track_t *t, voice_t *v)
{
    (void)t;
    v->s[2] = 1 << 15;                                   /* PLUCK's own decay starts */
}

static void drift_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t src = (uint32_t)clamp(p[P_E0], 0, DS_N - 1), i, pa = v->ph[0], pb = v->ph[1], inc = m->inc;
    int32_t tone = p[P_E1], lp = v->s[0], bp = v->s[1], dec = v->s[2], k = 1500 + tone * 240;
    uint32_t seed = (uint32_t)v->s[3] | 1u;
    if (n > CTL)
        n = CTL;
    for (i = 0; i < n; i++) {
        int32_t s;
        switch (src) {
        case DS_SINE:                                    /* a sine, its octave (TONE: louder), a touch of FM */
            s = px_sin(pa + (uint32_t)(px_sin(pb) >> 2) * (uint32_t)tone / 64u) >> 15;
            s += (px_sin(pb) >> 15) * tone / 254;
            pa += inc;
            pb += inc << 1;
            break;
        case DS_SAW:                                     /* two saws a few cents apart, low-passed */
            s = (osc_saw(pa, inc) + osc_saw(pb, inc + (inc >> 9))) >> 1;
            pa += inc;
            pb += inc + (inc >> 9);
            lp += px_m(s - lp, k, 15);
            s = lp;
            break;
        case DS_NOISE: {                                 /* a band of noise at the note: a 2-pole resonance */
            int32_t nz = (int32_t)px_rand(&seed) >> 17, f = (int32_t)((inc >> 14) * 25u >> 5) + 8;   /* (f ~ 2 pi f0 / FS, Q15) */
            f = f > 24000 ? 24000 : f;
            lp += px_m(f, bp, 15);                       /* (the Chamberlin state variable: lp, bp) */
            bp += px_m(f, nz - lp - px_m(bp, 600 + (127 - tone) * 40, 15), 15);
            s = clamp(bp << 1, -32767, 32767);
            break;
        }
        default:                                         /* PLUCK: a sine dying away by itself */
            s = px_m(px_sin(pa) >> 15, dec, 15);
            s += px_m(px_sin(pa << 1) >> 16, px_m(dec, dec, 15), 15) * tone / 64;
            pa += inc;
            dec -= (dec >> 13) + 1;                      /* (~0.2 s) */
            if (dec < 0)
                dec = 0;
            break;
        }
        out[i] += voice_amp(s, m, i);
    }
    v->ph[0] = pa;
    v->ph[1] = pb;
    v->s[0] = lp;
    v->s[1] = bp;
    v->s[2] = dec;
    v->s[3] = (int32_t)seed;
}

/* the memory, on the part's sum (after the voices), at a quarter of the rate; returns 1 while it sounds */
static uint32_t drift_post(track_t *t, int32_t *out, uint32_t n)
{
    const int16_t *p = t->p;
    drift_mem_t *M = drift_mem(t);
    uint32_t i, j, size = 32u + (uint32_t)p[P_E6] * 96u / 127u;   /* the loops' share in 1/128: 1/4 .. 1 */
    int32_t fb = p[P_E4] * p[P_E4] * 228 / 100, kb = 32000 - p[P_E5] * 150, mix = p[P_E7] * 258, peak = 0;   /* fb Q15: 0 .. 1.12 (the
                                                                              * tanh holds it: at the top it keeps) */
    int32_t drft = p[P_E2], step = (40 + p[P_E3] * p[P_E3] * 3) << 8;
    int32_t ta = (int32_t)(DR_LA * size / 128u - 4u) << 16, tb = (int32_t)(DR_LB * size / 128u - 4u) << 16;
    if (!M->rng)
        M->rng = (uint32_t)rng() | 1u;
    /* the heads' drift, once a block: a leaky random walk each, as far as DRFT lets them (up to ~3 % of the loop) */
    M->walk_a = sx_walk(M->walk_a, &M->rng, step, 1 << 28);
    M->walk_b = sx_walk(M->walk_b, &M->rng, step, 1 << 28);
    ta += (int32_t)(((int64_t)(ta >> 5) * px_m(M->walk_a, drft * 258, 15)) >> 28);
    tb += (int32_t)(((int64_t)(tb >> 5) * px_m(M->walk_b, drft * 258, 15)) >> 28);
    if (!M->da)
        M->da = ta, M->db = tb;
    M->da += (ta - M->da) >> 4;                          /* (smoothed: the pitch glides, never jumps) */
    M->db += (tb - M->db) >> 4;
    for (i = 0; i + 3u < n; i += 4u) {
        int32_t x = (out[i] + out[i + 1u] + out[i + 2u] + out[i + 3u]) >> 2, ya, yb, f, wet, in;
        uint32_t a;
        a = M->wa - ((uint32_t)M->da >> 16);             /* loop A's head, interpolated */
        f = (M->da >> 1) & 0x7FFF;
        ya = dr_get(M->la, DR_LA, a);
        ya += ((dr_get(M->la, DR_LA, a - 1u) - ya) * f) >> 15;
        a = M->wb - ((uint32_t)M->db >> 16);             /* .. B's */
        f = (M->db >> 1) & 0x7FFF;
        yb = dr_get(M->lb, DR_LB, a);
        yb += ((dr_get(M->lb, DR_LB, a - 1u) - yb) * f) >> 15;
        /* into the loops: what plays now, the loop's own past and some of the other's, blurred and worn */
        in = (x >> 1) + px_m(px_m(ya, 22000, 15) + px_m(yb, 10000, 15), fb, 15);
        M->lpa += px_m(in - M->lpa, kb, 15);
        M->la[M->wa % DR_LA] = dr_enc(softclip(M->lpa));
        in = (x >> 1) + px_m(px_m(yb, 22000, 15) + px_m(ya, 10000, 15), fb, 15);
        M->lpb += px_m(in - M->lpb, kb, 15);
        M->lb[M->wb % DR_LB] = dr_enc(softclip(M->lpb));
        M->wa++;
        M->wb++;
        wet = px_m(ya + yb, mix, 16);
        for (j = 0; j < 4u; j++)                         /* a quarter rate -> full: the samples between, straight */
            out[i + j] += M->wprev + (((wet - M->wprev) * (int32_t)(j + 1u)) >> 2);
        M->wprev = wet;
        if ((wet < 0 ? -wet : wet) > peak)
            peak = wet < 0 ? -wet : wet;
    }
    M->live = peak > 8 ? 48 : M->live > 0 ? M->live - 1 : 0;
    return M->live > 0;
}

#define STRANGE_CAT_DRIFT "PPPPPPPPPP"
static const preset_t DRIFT_PRESETS[] = {
    /* name, {SRC, TONE, DRFT, MOTN, ERAS, BLUR, SIZE, MIX}, {A D S R}, fenv, mono */
    {"COSMOS", {DS_SINE, 30, 50, 30, 100, 40, 127, 90}, {40, 64, 127, 80}, 0, 0, FX(0, 0, 0, 50)},
    {"WORN TAPE", {DS_SAW, 50, 90, 40, 96, 70, 110, 80}, {30, 64, 127, 70}, 0, 0, FX(0, 0, 0, 40)},
    {"MEMORY DUST", {DS_NOISE, 80, 60, 50, 110, 60, 127, 100}, {60, 64, 127, 100}, 0, 0, FX(0, 0, 0, 60)},
    {"PLUCK ECHOES", {DS_PLUCK, 60, 30, 20, 90, 30, 90, 90}, {0, 64, 127, 60}, 0, 0, FX(0, 0, 0, 40)},
    {"SEASICK", {DS_SINE, 60, 127, 70, 104, 50, 100, 100}, {20, 64, 127, 80}, 0, 0, FX(0, 20, 0, 50)},
    {"FOREVER", {DS_SINE, 20, 40, 10, 127, 50, 127, 110}, {80, 64, 127, 100}, 0, 0, FX(0, 0, 0, 70)},
    {"FLUTTER", {DS_PLUCK, 90, 80, 120, 80, 20, 20, 90}, {0, 64, 127, 50}, 0, 0, FX(0, 0, 0, 30)},
    {"GHOST CHOIR", {DS_NOISE, 40, 70, 30, 118, 90, 127, 110}, {100, 64, 127, 120}, 0, 0, FX(0, 30, 0, 90)},
    {"DECAYING SAW", {DS_SAW, 90, 50, 30, 104, 100, 127, 100}, {40, 64, 127, 100}, 0, 0, FX(10, 0, 0, 60)},
    {"STAR FIELD", {DS_PLUCK, 110, 60, 50, 120, 40, 127, 110}, {0, 64, 127, 90}, 0, 0, FX(0, 0, 0, 90)},
};

static const engine_t ENG_DRIFT = {
    .name = "DRIFT",
    .page_title = {"SOURCE", "MEMORY"},
    .edit = {
        {"SRC", F_ENUM, 0, DS_N - 1, 0, N_DR_SRC, 0},
        {"TONE", F_PCT, 0, 127, 40, 0, 0},
        {"DRFT", F_PCT, 0, 127, 50, 0, 0},
        {"MOTN", F_PCT, 0, 127, 30, 0, 0},
        {"ERAS", F_PCT, 0, 127, 100, 0, 0},
        {"BLUR", F_PCT, 0, 127, 40, 0, 0},
        {"SIZE", F_PCT, 0, 127, 127, 0, 0},
        {"MIX", F_PCT, 0, 127, 90, 0, 0},
    },
    .presets = DRIFT_PRESETS,
    .npresets = NELEM(DRIFT_PRESETS),
    .note_on = drift_note_on,
    .render = drift_render,
    .post = drift_post,
    .knob = {P_E2, P_E4, P_E5, P_E7},
    .poly = DR_POLY,
    .keep = 0x0B,                                        /* the filters and the random numbers */
    .mem = sizeof(drift_mem_t),
    .xyd = {{XY_E(2), XY_E(3), XY_E(1)}, {XY_E(4), XY_E(5), XY_E(6)}},   /* X: DRFT MOTN TONE (the wobble), Y: ERAS BLUR SIZE (the memory) */
    .xya = {{40, 40, 30}, {25, 40, -40}},
};
