/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Tim (646timur-cell), on Felucca by Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* CHAOS (STRANGE): unstable oscillators in the spirit of SOMA's Enner and Terra: systems that hold a pitch while they
 * are gentle and fall into subharmonics, period doubling and noise as they are pushed. ALGO picks the system:
 *   FBFM   two sines that frequency-modulate each other (cross-coupled feedback FM): WARP their ratio (1/4 .. 4),
 *          CHAO the depth (up to ~6 radians each way: past ~2 it no longer settles)
 *   DUFF   a Duffing oscillator (a spring that stiffens as it stretches) shaken at the note: WARP the stiffening,
 *          CHAO how hard it is shaken. Gentle, the note; harder, bifurcations; hardest, a chaotic roar
 *   LOGI   the logistic map x -> r x (1 - x), one step per cycle of the note (twice): CHAO r from 3.5 (a cycle of four:
 *          two octaves down) to 4.0 (chaos, still pitched by the clock), WARP how much the steps glide into each other
 *   VDP    a van der Pol oscillator (a relaxation oscillator: a valve, a heart) forced at the note: WARP its
 *          nonlinearity mu (sine .. sharp pulses), CHAO the forcing (it locks, beats, then slips)
 * Then, per voice: FOLD a wavefolder, TONE a low-pass (the track's ENV / LFO -> FLT move it), SUB a square an octave
 * down, DRFT a slow random wander of CHAO (an organism: never twice the same), NOIS noise into the system itself (it
 * is sensitive to it as Terra's plates are to a finger: VEL, AT, MODW onto NOIS in the matrix are touch).
 * State per voice in voice_t: ph[0] the note's phase (the forcing, the clock, FM's A), ph[1] FM's B, ph[2] the SUB;
 * s[0], s[1] the system (Q24, or FM's last outputs), s[2], s[3] TONE's SVF, s[4] the drift, s[5] the random numbers,
 * s[6] LOGI's glide. No engine memory. */

enum { CH_FBFM, CH_DUFF, CH_LOGI, CH_VDP, CH_N };
static const char *const N_CH_ALGO[] = {"FBFM", "DUFF", "LOGI", "VDP"};
#define CH_Q 24                                          /* the systems' state: Q24 */
#define CH_ONE (1 << CH_Q)

static void chaos_note_on(track_t *t, voice_t *v)
{
    (void)t;
    if (!v->s[5])
        v->s[5] = (int32_t)(0x9E3779B9u ^ (v->age * 2246822519u)) | 1;
    if (!v->env && !v->env_out) {                        /* from rest: a small push, so the system starts moving */
        v->s[0] = CH_ONE / 8;
        v->s[1] = 0;
        v->s[6] = 0;
    }
}

/* a triangle wavefolder: y (Q15) x gain (Q12), folded back at +-1 */
static inline int32_t ch_fold(int32_t y, int32_t g)
{
    return osc_tri((uint32_t)((((int64_t)y * g) >> 12) + 32768) << 16);
}

static void chaos_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t algo = (uint32_t)clamp(p[P_E0], 0, CH_N - 1), i, seed = (uint32_t)v->s[5] | 1u;
    uint32_t ph = v->ph[0], pb = v->ph[1], ps = v->ph[2], inc = m->inc, ib;
    int32_t x = v->s[0], y = v->s[1], c1 = v->s[2], c2 = v->s[3], gl = v->s[6];
    int32_t chao, warp = p[P_E2], fold = 4096 + p[P_E4] * p[P_E4] / 2, sub = p[P_E6] * 200, nois = p[P_E7];
    int32_t w, k1, k2, dr;
    tsvf_t f;
    if (n > CTL)
        n = CTL;
    dr = v->s[4] = sx_walk(v->s[4], &seed, p[P_E5] * 3, p[P_E5] * 24);   /* DRFT: CHAO wanders by up to ~24 */
    chao = clamp(p[P_E1] * 32 + dr, 0, 127 * 32);       /* 0 .. 4064 (CHAO x 32) */
    tsvf_coef(&f, clamp((p[P_E3] << 8) + m->cutoff * 2, 0, 127 << 8), 30);
    /* w = 2 pi f / FS (Q24) of the note, for the integrators; at most ~0.9 (stable: the step stays short of a cycle) */
    w = (int32_t)(((uint64_t)inc * 105414357u) >> 32);   /* 2 pi / 2^32 in Q24 x 2^32 */
    w = w > 15099494 ? 15099494 : w;
    switch (algo) {
    case CH_FBFM:
        ib = (uint32_t)(((uint64_t)inc * sx_expmap(warp, 16384u, 4)) >> 16);   /* B at 1/4 .. 4 x the note */
        k1 = (chao * chao) >> 10;                        /* depth: 0 .. 16128 (<< 3: ~1 cycle, 6 radians, at full scale) */
        for (i = 0; i < n; i++) {
            int32_t a, b, nz = nois ? px_m(sx_noise(&seed), nois * 64, 20) : 0, s;
            a = sine_i(ph + ((uint32_t)((y + nz) * k1) << 3));
            b = sine_i(pb + ((uint32_t)((x + nz) * k1) << 3));
            x = a;                                       /* (s[0] A's output, s[1] B's: Q15 here) */
            y = b;
            ph += inc;
            pb += ib;
            s = ch_fold(a, fold);
            s += ps < 0x80000000u ? sub : -sub;
            ps += inc >> 1;
            s = tsvf_lp(&f, s, &c1, &c2);
            out[i] += voice_amp(clamp(soft_knee(s, 24000), -32767, 32767), m, i);
        }
        break;
    case CH_DUFF:
    case CH_VDP:
        /* DUFF: y += w (-d y - x - b x^3 + g cos), x += w y; VDP: y += w (mu (1 - x^2) y - x + g cos), x += w y
         * (symplectic Euler, Q24; x held to +-3.5) */
        if (algo == CH_DUFF) {
            k1 = warp * 132104;                          /* b: 0 .. 1 */
        } else {                                         /* mu: 0 .. 4, mu w at most 1.5 (stiff: unstable above) */
            uint32_t r = 0xFFFFFFFFu / (uint32_t)(w | 1);  /* 2^32 / w: 256 x (1 / w) */
            k1 = warp * 528416;
            if (r < 683u && k1 > (int32_t)(98304u * r))   /* 1.5 x 2^24 x (1 / w) (Q24) */
                k1 = (int32_t)(98304u * r);
        }
        k2 = chao * 4128;                                /* g: 0 .. ~1 (Q24) */
        for (i = 0; i < n; i++) {
            int32_t fx = px_m(sine_i(ph + 0x40000000u), k2, 15), x2 = px_m(x, x, CH_Q), acc, s;
            if (nois)
                fx += px_m(sx_noise(&seed), nois * 1024, 20);
            if (algo == CH_DUFF)
                acc = -(y >> 3) - x - px_m(px_m(x2, x, CH_Q), k1, CH_Q) + fx;   /* d = 1/8 */
            else
                acc = px_m(clamp(px_m(CH_ONE - x2, y, CH_Q), -30 * CH_ONE, 30 * CH_ONE), k1, CH_Q) - x + fx;
            y += px_m(acc, w, CH_Q);
            y = clamp(y, -6 * CH_ONE, 6 * CH_ONE);
            x += px_m(y, w, CH_Q);
            x = clamp(x, -(7 * CH_ONE) / 2, (7 * CH_ONE) / 2);
            ph += inc;
            s = ch_fold(x >> 10, fold);                   /* (x 1.0 -> Q15 / 2: room for the swing) */
            s += ps < 0x80000000u ? sub : -sub;
            ps += inc >> 1;
            s = tsvf_lp(&f, s, &c1, &c2);
            out[i] += voice_amp(clamp(soft_knee(s, 24000), -32767, 32767), m, i);
        }
        break;
    default:                                             /* LOGI */
        k1 = 3 * CH_ONE + CH_ONE / 2 + chao * 2064;      /* r: 3.5 .. 4.0 (Q24) */
        k2 = 32767 - warp * 254;                         /* the glide: a step each cycle .. a slow slur */
        if (k2 < 400)
            k2 = 400;
        for (i = 0; i < n; i++) {
            int32_t s;
            uint32_t prev = ph;
            ph += inc << 1;
            if (ph < prev) {                             /* the clock: one step of the map */
                int32_t r = k1 + (nois ? px_m(sx_noise(&seed), nois * 2048, 20) : 0);
                x = px_m(px_m(clamp(r, 0, 4 * CH_ONE), x, CH_Q), CH_ONE - x, CH_Q);
                if (x <= 0 || x >= CH_ONE)               /* (left the interval: back in, somewhere) */
                    x = (int32_t)(px_rand(&seed) >> 8) | 1;
            }
            gl += px_m(((x >> 8) - 32768) - gl, k2, 15);   /* x 0..1 -> -1..1 (Q15), glided */
            s = ch_fold(gl, fold);
            s += ps < 0x80000000u ? sub : -sub;
            ps += inc >> 1;
            s = tsvf_lp(&f, s, &c1, &c2);
            out[i] += voice_amp(clamp(soft_knee(s, 24000), -32767, 32767), m, i);
        }
        break;
    }
    v->ph[0] = ph;
    v->ph[1] = pb;
    v->ph[2] = ps;
    v->s[0] = x;
    v->s[1] = y;
    v->s[2] = c1;
    v->s[3] = c2;
    v->s[5] = (int32_t)seed;
    v->s[6] = gl;
}

/* per ALGO, CHAO and WARP under their own names */
static const param_desc_t CH_D_RATIO = {"RATIO", F_PCT, 0, 127, 64, 0, 0};
static const param_desc_t CH_D_STIF = {"STIF", F_PCT, 0, 127, 64, 0, 0};
static const param_desc_t CH_D_GLID = {"GLID", F_PCT, 0, 127, 64, 0, 0};
static const param_desc_t CH_D_MU = {"MU", F_PCT, 0, 127, 64, 0, 0};
static const param_desc_t *chaos_desc(const track_t *t, uint32_t k)
{
    static const param_desc_t *const W[CH_N] = {&CH_D_RATIO, &CH_D_STIF, &CH_D_GLID, &CH_D_MU};
    return k == 2u ? W[(uint32_t)clamp(t->p[P_E0], 0, CH_N - 1)] : 0;
}

#define STRANGE_CAT_CHAOS "LBFPLFB"
static const preset_t CHAOS_PRESETS[] = {
    /* name, {ALGO, CHAO, WARP, TONE, FOLD, DRFT, SUB, NOIS}, {A D S R}, fenv, mono */
    {"ENNER LEAD", {CH_FBFM, 40, 64, 100, 20, 30, 0, 0}, {2, 64, 127, 40}, 0, 0, FX(0, 0, 0, 30)},
    {"DUFF BASS", {CH_DUFF, 52, 70, 70, 0, 20, 60, 0}, {0, 64, 127, 30}, 0, 1, FX(10, 0, 0, 10)},
    {"BIFURCATE", {CH_LOGI, 80, 30, 96, 30, 60, 0, 10}, {0, 64, 127, 50}, 0, 0, FX(0, 0, 0, 40)},
    {"TERRA PAD", {CH_VDP, 40, 50, 76, 10, 70, 0, 6}, {80, 64, 127, 90}, 0, 0, FX(0, 30, 0, 70)},
    {"VALVE LEAD", {CH_VDP, 70, 110, 88, 40, 20, 20, 0}, {4, 64, 127, 40}, 0, 1, FX(10, 0, 0, 30)},
    {"ROAR", {CH_DUFF, 127, 127, 110, 60, 90, 0, 30}, {10, 64, 127, 60}, 0, 0, FX(20, 0, 0, 40)},
    {"LOGIC BASS", {CH_LOGI, 20, 90, 60, 0, 10, 80, 0}, {0, 64, 127, 20}, 0, 1, FX(0, 0, 0, 10)},
};

static const engine_t ENG_CHAOS = {
    .name = "CHAOS",
    .page_title = {"SYSTEM", "BODY"},
    .edit = {
        {"ALGO", F_ENUM, 0, CH_N - 1, 0, N_CH_ALGO, 0},
        {"CHAO", F_PCT, 0, 127, 40, 0, 0},
        {"WARP", F_PCT, 0, 127, 64, 0, 0},
        {"TONE", F_PCT, 0, 127, 96, 0, 0},
        {"FOLD", F_PCT, 0, 127, 0, 0, 0},
        {"DRFT", F_PCT, 0, 127, 30, 0, 0},
        {"SUB", F_PCT, 0, 127, 0, 0, 0},
        {"NOIS", F_PCT, 0, 127, 0, 0, 0},
    },
    .presets = CHAOS_PRESETS,
    .npresets = NELEM(CHAOS_PRESETS),
    .note_on = chaos_note_on,
    .render = chaos_render,
    .desc = chaos_desc,
    .knob = {P_E1, P_E2, P_E3, P_E4},
    .poly = 4,
    .keep = 0x33,                                        /* the system and the drift run on over a retrigger */
    .xyd = {{XY_E(1), XY_E(5), XY_E(7)}, {XY_E(4), XY_E(3), XY_E(2)}},   /* X: CHAO DRFT NOIS (into chaos), Y: FOLD TONE WARP (the edge) */
    .xya = {{50, 40, 25}, {55, 25, 30}},
};
