/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Tim (646timur-cell), on Felucca by Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* GLITCH (STRANGE): a slice machine on its own kit of broken digital sound (tools/gen_strange_samples.py: 16 slices,
 * 11,025 Hz: crushed noise, click trains, FM chirps, stutters, dropouts, zaps, a rate sweep, crackle, a modem, a thump,
 * a CD skip, LFSR data, a faulty beep, a tick, a reversed swell, a grain smear).
 * Each key plays a slice: (note + SLCE) mod 16, the next 16 keys the same slices an octave up. The slice is decoded
 * into the voice's buffer as it starts, so it can be read anywhere, either way:
 *   SLCE   which slice the lowest key plays
 *   PTCH   the slices' pitch, semitones
 *   STUT   a stutter: a window of the slice (170 ms .. 5 ms) played again and again (0: off, the slice once)
 *   REP    how often each window repeats before the next one (16: for ever: a buzz, a loop)
 *   REV    the chance a window (or the slice) plays backwards
 *   CRSH   bits and rate thrown away (a bitcrusher: 16 bits .. 2, the rate held up to 16 samples)
 *   CHOP   the chance a window is dropped (silence: an underrun)
 *   JUMP   the chance a window jumps somewhere else in the slice
 * Poly 3, its buffers in the part's engine region (gl_slot_t). */

#define GL_POLY 3
#define GL_LEN 2048u

typedef struct {
    int16_t buf[GL_LEN];
    uint32_t len, rng;                                   /* the slice's samples; random numbers */
    int32_t pos, step;                                   /* the read position, its step (Q16, negative: backwards) */
    int32_t w0, wn, rep, chop;                           /* the window: its start, length (samples), repeats so far,
                                                          * dropped */
    int32_t hold, held;                                  /* CRSH's sample hold: samples left, the sample held */
} gl_slot_t;
ENG_MEM_FITS("GLITCH", sizeof(gl_slot_t) * GL_POLY);

static gl_slot_t *gl_slot(track_t *t, voice_t *v)
{
    uint32_t i = (uint32_t)(v - t->v);
    if (t < &trk[0] || t >= &trk[NLIVE] || i >= GL_POLY)
        return 0;
    return (gl_slot_t *)ENG_MEM(t - trk) + i;
}

/* the zone's IMA ADPCM into the buffer (as eng_sample.c sample_next decodes it) */
static __attribute__((noinline)) uint32_t gl_decode(const smp_zone_t *z, int16_t *b)
{
    uint32_t n = z->n < GL_LEN ? z->n : GL_LEN, i;
    int32_t pred = 0, idx = 0;
    for (i = 0; i < n; i++) {
        uint32_t c = SMP_DATA[z->off + (i >> 1)], code = (i & 1u) ? c >> 4 : c & 15u;
        int32_t step = IMA_STEP[idx], vd = step >> 3;
        if (code & 4u)
            vd += step;
        if (code & 2u)
            vd += step >> 1;
        if (code & 1u)
            vd += step >> 2;
        pred = clamp(pred + ((code & 8u) ? -vd : vd), -32768, 32767);
        idx = clamp(idx + IMA_IDX[code & 7u], 0, 88);
        b[i] = (int16_t)pred;
    }
    return n;
}

static int gl_roll(gl_slot_t *S, int32_t p) { return (int32_t)(px_rand(&S->rng) >> 25) < p; }   /* p / 128 */

/* a window begins: the dice for reversing, dropping, jumping */
static void gl_window(gl_slot_t *S, const int16_t *p, int first)
{
    int32_t wn = S->wn;
    if (!first && S->rep < (p[P_E3] >= 16 ? 0x7FFFFFFF : p[P_E3])) {
        S->rep++;                                        /* the same window again */
        S->pos = S->step >= 0 ? S->w0 << 16 : (S->w0 + wn - 1) << 16;
        return;
    }
    S->rep = 1;
    if (!first)
        S->w0 += wn;
    if (gl_roll(S, p[P_E7]))
        S->w0 = (int32_t)(px_rand(&S->rng) % (S->len > (uint32_t)wn ? S->len - (uint32_t)wn : 1u));
    if (S->w0 + wn > (int32_t)S->len)
        S->w0 = 0;                                       /* (round the slice again) */
    if (gl_roll(S, p[P_E4]))
        S->step = -S->step;
    S->step = p[P_E4] ? S->step : (S->step < 0 ? -S->step : S->step);
    S->chop = gl_roll(S, p[P_E6]);
    S->pos = S->step >= 0 ? S->w0 << 16 : (S->w0 + wn - 1) << 16;
}

static void glitch_note_on(track_t *t, voice_t *v)
{
    const int16_t *p = t->p;
    gl_slot_t *S = gl_slot(t, v);
    uint32_t sl = ((uint32_t)v->note + (uint32_t)p[P_E0]) % GLT_NZ;
    int32_t oct = (int32_t)v->note / 16 - 3, d16 = (p[P_E1] + 12 * oct) * 16;
    const smp_zone_t *z = &GLT_ZONES[sl];
    if (!S)
        return;
    S->len = gl_decode(z, S->buf);
    if (!S->rng)
        S->rng = (uint32_t)rng() | 1u;
    S->step = (int32_t)(((uint64_t)z->rate * pow2_q16(clamp(d16, -1536, 576))) >> 16);   /* Q16 samples a sample */
    S->wn = p[P_E2] ? (int32_t)(sx_expmap(127 - p[P_E2], 55u << 16, 5) >> 16) : (int32_t)S->len;   /* 5 .. 170 ms (at
                                                                                              * 11,025 Hz: 55 .. 1760) */
    if (S->wn > (int32_t)S->len)
        S->wn = (int32_t)S->len;
    if (S->wn < 8)
        S->wn = 8;
    S->w0 = 0;
    S->rep = 0;
    S->hold = 0;
    gl_window(S, p, 1);
}

static void glitch_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    gl_slot_t *S = gl_slot(t, v);
    uint32_t i, bits = (uint32_t)p[P_E5] / 9u, hn = 1u + (uint32_t)p[P_E5] / 8u, stut = p[P_E2] != 0;
    int32_t mask = -(1 << bits);
    if (!S || !S->len)
        return;
    if (n > CTL)
        n = CTL;
    for (i = 0; i < n; i++) {
        int32_t s = 0, ip = S->pos >> 16, end = stut ? S->w0 + S->wn : (int32_t)S->len;
        if (S->step >= 0 ? ip >= end : ip < (stut ? S->w0 : 0)) {
            if (!stut) {                                 /* the slice once: done, silent */
                S->step = 0;
                ip = -1;
            } else {
                gl_window(S, p, 0);
                ip = S->pos >> 16;
            }
        }
        if (ip >= 0 && ip + 1 < (int32_t)S->len && S->step) {
            int32_t a = S->buf[ip], b = S->buf[ip + 1], f = (S->pos >> 1) & 0x7FFF;
            s = a + (((b - a) * f) >> 15);
            S->pos += S->step;
        }
        if (S->chop)
            s = 0;
        if (hn > 1u) {                                   /* CRSH: the rate held, the bits dropped */
            if (S->hold-- <= 0) {
                S->hold = (int32_t)hn - 1;
                S->held = s & mask;
            }
            s = S->held;
        }
        out[i] += voice_amp(s, m, i);
    }
}

#define STRANGE_CAT_GLITCH "FFFFFFFFFF"
static const preset_t GLITCH_PRESETS[] = {
    /* name, {SLCE, PTCH, STUT, REP, REV, CRSH, CHOP, JUMP}, {A D S R}, fenv, mono */
    {"BROKEN KIT", {0, 0, 0, 1, 0, 0, 0, 0}, {0, 64, 127, 30}, 0, 0, FX(0, 0, 0, 20)},
    {"STUTTER", {0, 0, 70, 3, 0, 0, 0, 0}, {0, 64, 127, 30}, 0, 0, FX(0, 0, 10, 20)},
    {"BUZZ LOOP", {3, 0, 110, 16, 0, 20, 0, 0}, {0, 64, 127, 40}, 0, 0, FX(0, 0, 0, 30)},
    {"BACKWARDS", {0, -5, 40, 2, 100, 0, 0, 0}, {0, 64, 127, 40}, 0, 0, FX(0, 0, 20, 40)},
    {"UNDERRUN", {4, 0, 90, 2, 20, 30, 50, 30}, {0, 64, 127, 40}, 0, 0, FX(0, 0, 10, 30)},
    {"DATA RAIN", {11, 7, 60, 4, 60, 60, 30, 90}, {40, 64, 127, 90}, 0, 0, FX(0, 0, 40, 90)},
    {"CRUSH BOX", {0, -12, 30, 2, 30, 110, 10, 20}, {0, 64, 127, 30}, 0, 0, FX(0, 0, 0, 20)},
    {"MODEM CHOIR", {8, -12, 100, 16, 40, 20, 0, 50}, {90, 64, 127, 110}, 0, 0, FX(0, 30, 30, 110)},
    {"SKIPPING CD", {10, 0, 80, 6, 0, 10, 20, 60}, {0, 64, 127, 60}, 0, 0, FX(0, 0, 30, 60)},
    {"GHOST GLITCH", {14, -24, 50, 3, 80, 40, 30, 70}, {40, 64, 127, 120}, 0, 0, FX(0, 40, 40, 120)},
};

static const engine_t ENG_GLITCH = {
    .name = "GLITCH",
    .page_title = {"SLICE", "BREAK"},
    .edit = {
        {"SLCE", F_INT, 0, 15, 0, 0, 0},
        {"PTCH", F_SEMI, -24, 24, 0, 0, 0},
        {"STUT", F_PCT, 0, 127, 0, 0, 0},
        {"REP", F_INT, 1, 16, 1, 0, 0},
        {"REV", F_PCT, 0, 127, 0, 0, 0},
        {"CRSH", F_PCT, 0, 127, 0, 0, 0},
        {"CHOP", F_PCT, 0, 127, 0, 0, 0},
        {"JUMP", F_PCT, 0, 127, 0, 0, 0},
    },
    .presets = GLITCH_PRESETS,
    .npresets = NELEM(GLITCH_PRESETS),
    .note_on = glitch_note_on,
    .render = glitch_render,
    .knob = {P_E2, P_E3, P_E5, P_E7},
    .poly = GL_POLY,
    .sampled = 1,
    .mem = sizeof(gl_slot_t) * GL_POLY,
    .xyd = {{XY_E(2), XY_E(5), XY_E(4)}, {XY_E(7), XY_E(6), XY_E(1)}},   /* X: STUT CRSH REV (the break), Y: JUMP CHOP PTCH (the chaos) */
    .xya = {{40, 40, 30}, {50, 40, 20}},
};
