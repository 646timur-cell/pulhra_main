/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Tim (646timur-cell), on Felucca by Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* PLANT (STRANGE): a sound that grows, after the idea of Sonic Charge's Synplant (not its code). A seed (GENE) is a
 * patch of a small synth: a sine carrier, a sine modulator (RATIO, INDEX, its own feedback), a wavefolder, noise, a
 * resonant low-pass and a wobble. Every key is a branch out of the seed: the twelve notes of an octave twelve
 * directions of mutation (a fixed random vector per GENE and note, an octave a second, smaller root of its own), and
 * while the note is held the branch grows out along its direction (GROW: how far its tip is from the seed, RATE: how
 * fast it gets there, 0 at once): the modulators grow on it, the FM deepens, the fold and the wobble come in, the
 * filter opens or closes as that branch has it. Let go and play again: the branch starts at the seed again.
 *   GENE   which plant (0..127)        GROW  how far the branches reach    RATE  how fast they grow (0: at once)
 *   RATIO  the modulator's ratio (0.5 .. 8, before the branch moves it)    INDEX the FM depth
 *   BLOOM  the filter                  NOISE breath and spores            WIND  the wobble's depth
 * Poly 4; per voice ph[0] the carrier, ph[1] the modulator, ph[2] the wobble; s[0] the blocks held, s[1] s[2] the
 * filter, s[3] the modulator's last output, s[4] the noise. */

#define PL_POLY 4
enum { PD_RATIO, PD_INDEX, PD_FB, PD_FOLD, PD_CUT, PD_RES, PD_NOISE, PD_WOB, PD_N };

/* branch b (0..11, the note in its octave) of plant g, root r (the octave): dimension d's direction, -32768..32767 */
static int32_t pl_dir(uint32_t g, uint32_t b, uint32_t r, uint32_t d)
{
    uint32_t h = (g * 2654435761u) ^ (b * 40503u + 1u) * 2246822519u ^ (d * 3266489917u);
    uint32_t h2 = (g * 2654435761u) ^ ((r + 101u) * 668265263u) ^ (d * 374761393u);
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h2 ^= h2 >> 13;
    h2 *= 0x297A2D39u;
    h2 ^= h2 >> 15;
    return (int32_t)(h >> 16) - 32768 + (((int32_t)(h2 >> 16) - 32768) >> 2);
}

static void plant_note_on(track_t *t, voice_t *v)
{
    (void)t;
    if (!v->env && !v->env_out) {
        v->ph[0] = v->ph[1] = 0;
        v->s[1] = v->s[2] = v->s[3] = 0;
    }
    v->s[0] = 0;                                         /* the branch from the seed again */
    if (!v->s[4])
        v->s[4] = (int32_t)(rng() | 1u);
}

static void plant_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t i, b = (uint32_t)v->note % 12u, r = (uint32_t)v->note / 12u, g = (uint32_t)clamp(p[P_E0], 0, 127);
    int32_t pd[PD_N], grow, k, q, idx, fb, fold, nz, wob, lo = v->s[1], bd = v->s[2], prev = v->s[3];
    uint32_t mrat, minc, wph = v->ph[2], seed = (uint32_t)v->s[4];
    if (n > CTL)
        n = CTL;
    /* the growth: GROW's share reached after RATE's time (~20 ms .. 10 s), then held there */
    {
        int32_t held = v->s[0] < 0x7FFFFFFF ? v->s[0]++ : v->s[0], full = p[P_E2] ? (int32_t)sx_expmap(p[P_E2], 28u, 9) : 1;   /* blocks */
        grow = p[P_E1] * 258;                            /* Q15 */
        if (held < full)
            grow = (int32_t)((int64_t)grow * held / full);
    }
    /* the patch: the seed's knobs, moved along the branch */
    {
        uint32_t d;
        for (d = 0; d < PD_N; d++)
            pd[d] = px_m(pl_dir(g, b, r, d), grow, 15);  /* -32768 .. 32767 times the growth */
    }
    mrat = px_exp2(clamp(p[P_E3] * 4 * 65536 / 127 - 65536 + pd[PD_RATIO] * 3 / 2, -2 * 65536, 4 * 65536));   /* Q16 */
    minc = (uint32_t)(((uint64_t)m->inc * mrat) >> 16);
    idx = clamp(p[P_E4] * 200 + pd[PD_INDEX] / 2, 0, 32767);   /* FM depth (Q15 of 2 turns) */
    fb = clamp(pd[PD_FB] / 2, 0, 24000);
    fold = clamp(pd[PD_FOLD] / 3, 0, 32767);
    nz = clamp(p[P_E6] * 180 + pd[PD_NOISE] / 6, 0, 32767);
    wob = clamp(p[P_E7] * 40 + (pd[PD_WOB] > 0 ? pd[PD_WOB] / 12 : 0), 0, 8000);
    {                                                    /* the filter: BLOOM + the branch + the track's FLT routings */
        int32_t ci = clamp(p[P_E5] * 256 + pd[PD_CUT] / 3 + m->cutoff, 0, 127 * 256);
        uint32_t hz = (uint32_t)(((uint64_t)px_exp2(ci * 18) * (40u << 16)) >> 16);   /* 40 Hz .. 20 kHz, 9 octaves (Q16) */
        k = (int32_t)(((uint64_t)hz * 3u) >> 16);         /* ~2 pi f / fs, Q15: f * 4.67 */
        k = clamp(k + (k >> 1), 40, 28000);
        q = clamp(32767 - 18000 - pd[PD_RES] / 3, 3000, 32767);   /* damping: lower is more resonance */
    }
    for (i = 0; i < n; i++) {
        uint32_t wi = (uint32_t)((int64_t)m->inc * px_m(sine_i(wph), wob, 15) >> 19);   /* (up to ~25 cents) */
        int32_t mo, c, y, hi;
        wph += 2800000u + (uint32_t)(seed & 0xFFFFu) * 8u;   /* the wobble: ~3 .. 6 Hz */
        v->ph[1] += minc;
        mo = sine_i(v->ph[1] + (uint32_t)(prev * fb >> 15) * 65536u);
        prev = mo;
        v->ph[0] += m->inc + wi;
        c = sine_i(v->ph[0] + (uint32_t)((mo * idx) >> 15) * 262144u);
        if (fold) {                                      /* the fold: up to 4 x, folded back into range */
            y = c + px_m(c, fold * 3, 15);
            while (y > 32767 || y < -32767)
                y = y > 32767 ? 65534 - y : -65534 - y;
            c = y;
        }
        c += px_m(sx_noise(&seed) >> 5, nz, 15);
        hi = c - lo - px_m(bd, q, 15);                   /* a state-variable low-pass (Chamberlin) */
        bd = clamp(bd + px_m(hi, k, 15), -98304, 98304);
        lo = clamp(lo + px_m(bd, k, 15), -98304, 98304);
        out[i] += voice_amp(clamp(soft_knee(lo, 26000), -32767, 32767), m, i);
    }
    v->s[1] = lo;
    v->s[2] = bd;
    v->s[3] = prev;
    v->s[4] = (int32_t)seed;
    v->ph[2] = wph;
}

#define STRANGE_CAT_PLANT "PPPLBUPPFP"
static const preset_t PLANT_PRESETS[] = {
    /* name, {GENE, GROW, RATE, RATIO, INDEX, BLOOM, NOISE, WIND}, {A D S R}, fenv, mono */
    {"SEEDLING", {7, 50, 70, 32, 30, 80, 10, 20}, {20, 64, 127, 80}, 0, 0, FX(0, 20, 20, 70)},
    {"FERN PAD", {23, 70, 95, 48, 25, 70, 20, 40}, {90, 64, 127, 110}, 0, 0, FX(0, 40, 30, 110)},
    {"MOSS", {41, 40, 110, 16, 15, 55, 45, 30}, {100, 64, 127, 120}, 0, 0, FX(10, 30, 20, 120)},
    {"THORN LEAD", {5, 60, 40, 64, 70, 95, 5, 25}, {0, 64, 127, 50}, 0, 1, FX(20, 0, 20, 50)},
    {"ROOT BASS", {88, 45, 50, 0, 50, 50, 0, 10}, {0, 64, 127, 40}, 0, 1, FX(10, 0, 0, 20)},
    {"BLOSSOM BELL", {64, 55, 60, 90, 60, 100, 0, 15}, {0, 64, 100, 90}, 0, 0, FX(0, 20, 30, 100)},
    {"VINE", {101, 80, 85, 40, 45, 75, 10, 50}, {40, 64, 127, 100}, 0, 0, FX(0, 30, 40, 90)},
    {"SPORE CLOUD", {12, 90, 100, 70, 40, 85, 80, 60}, {80, 64, 127, 120}, 0, 0, FX(0, 40, 30, 127)},
    {"NIGHT GARDEN", {77, 65, 120, 24, 35, 60, 30, 45}, {110, 64, 127, 127}, 0, 0, FX(0, 50, 40, 127)},
    {"OVERGROWN", {119, 127, 90, 56, 80, 90, 25, 70}, {60, 64, 127, 120}, 0, 0, FX(20, 30, 30, 110)},
};

static const engine_t ENG_PLANT = {
    .name = "PLANT",
    .page_title = {"SEED", "GROWTH"},
    .edit = {
        {"GENE", F_INT, 0, 127, 7, 0, 0},
        {"GROW", F_PCT, 0, 127, 50, 0, 0},
        {"RATE", F_PCT, 0, 127, 70, 0, 0},
        {"RATIO", F_PCT, 0, 127, 32, 0, 0},
        {"INDEX", F_PCT, 0, 127, 30, 0, 0},
        {"BLOOM", F_PCT, 0, 127, 80, 0, 0},
        {"NOISE", F_PCT, 0, 127, 10, 0, 0},
        {"WIND", F_PCT, 0, 127, 20, 0, 0},
    },
    .presets = PLANT_PRESETS,
    .npresets = NELEM(PLANT_PRESETS),
    .note_on = plant_note_on,
    .render = plant_render,
    .knob = {P_E0, P_E1, P_E2, P_E5},
    .poly = PL_POLY,
    .xyd = {{XY_E(1), XY_E(4), XY_E(7)}, {XY_E(5), XY_E(3), XY_E(6)}},   /* X: GROW INDEX WIND (the growth), Y: BLOOM RATIO NOISE (the flower) */
    .xya = {{50, 40, 30}, {50, 30, 30}},
};
