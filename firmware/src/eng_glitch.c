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
 * KIT (EDIT > KIT, OP3's REL): BUILT (the kit above) or a user slot USR1..3, the web uploader's: a slot of several
 * zones (files, or one loop cut at its transients by the uploader) plays a zone a key; a slot of one zone is cut
 * into CUTS (OP1's LVL, 2..16) equal slices. A user slice is read from flash through a seek index (as GRAIN's: the
 * ADPCM state every 256 samples of every zone, built two entries a block in the block hook, the zones of the
 * sounding voices first) into a 257-sample window of the voice: any length the slot holds, any direction, any jump.
 * The windows (STUT) are in time, whatever the slot's rate.
 * Poly 3, its buffers and the index in the part's engine region (gl_part_t). */

#define GL_POLY 3
#define GL_LEN 2048u

typedef struct {
    int16_t buf[GL_LEN];
    uint32_t len, rng;                                   /* the slice's samples; random numbers */
    int32_t pos, step;                                   /* the read position, its step (Q16, negative: backwards) */
    int32_t w0, wn, rep, chop;                           /* the window: its start, length (samples), repeats so far,
                                                          * dropped */
    int32_t hold, held;                                  /* CRSH's sample hold: samples left, the sample held */
    int32_t base, cc;                                    /* a user slice: its start in the zone; the window's chunk
                                                          * (buf[0..256] = samples cc * 256 ..; -1 none) */
    uint8_t zone, user;                                  /* the slice's zone; 1 = a user slot's (read through the
                                                          * index), 0 = the kit's (decoded whole into buf) */
} gl_slot_t;
#define GL_SEG 256u
#define GL_NIDX 1024u                                    /* a full user slot: ~640 entries */
typedef struct {
    gl_slot_t v[GL_POLY];
    int16_t ipred[GL_NIDX];                              /* the seek index: the state before sample k * 256 of a zone */
    uint8_t iidx[GL_NIDX];
    uint16_t zbase[16], zcnt[16], zdone[16];             /* a zone's entries: first, all, built */
    uint32_t stamp;                                      /* the slot it was built for (smp_user_gen) */
    uint8_t kit, nz;                                     /* KIT + 1 the index holds (0: none); zones */
} gl_part_t;
ENG_MEM_FITS("GLITCH", sizeof(gl_part_t));

static gl_part_t *gl_part(track_t *t) { return (gl_part_t *)ENG_MEM(t - trk); }
static gl_slot_t *gl_slot(track_t *t, voice_t *v)
{
    uint32_t i = (uint32_t)(v - t->v);
    if (t < &trk[0] || t >= &trk[NLIVE] || i >= GL_POLY)
        return 0;
    return &gl_part(t)->v[i];
}
static uint32_t gl_kit(const int16_t *p) { return (uint32_t)clamp(p[P_FM3_REL], 0, SMP_USER_SLOTS); }

/* one ADPCM sample of zone z at pos */
static inline int32_t gl_dec(const smp_zone_t *z, uint32_t pos, int32_t *pred, int32_t *idx)
{
    uint32_t b = SMP_DATA[z->off + (pos >> 1)], code = (pos & 1u) ? (b >> 4) : (b & 15u);
    int32_t step = IMA_STEP[*idx], vd = step >> 3;
    if (code & 4u)
        vd += step;
    if (code & 2u)
        vd += step >> 1;
    if (code & 1u)
        vd += step >> 2;
    *pred = clamp(*pred + ((code & 8u) ? -vd : vd), -32768, 32767);
    *idx = clamp(*idx + IMA_IDX[code & 7u], 0, 88);
    return *pred;
}

/* the index for slot k (kit k + 1): every zone from the state (0, 0), its entries to build */
static void gl_reset(gl_part_t *P, uint32_t kit, uint32_t stamp)
{
    uint32_t i, tot = 0, nz = kit ? usr_nz[kit - 1u] : 0u, had = P->kit;
    P->kit = (uint8_t)(kit + 1u);
    P->stamp = stamp;
    P->nz = (uint8_t)nz;
    for (i = 0; i < nz; i++) {
        uint32_t c = (usr_zone[kit - 1u][i].n + GL_SEG - 1u) / GL_SEG;
        if (tot + c > GL_NIDX)
            c = 0;                                       /* past the capacity: silent */
        P->zbase[i] = (uint16_t)tot;
        P->zcnt[i] = (uint16_t)c;
        P->zdone[i] = c ? 1u : 0u;
        if (c) {
            P->ipred[tot] = 0;
            P->iidx[tot] = 0;
        }
        tot += c;
    }
    for (i = 0; i < GL_POLY && had; i++)                 /* (a first index: the voices are on this one) */
        if (P->v[i].user) {
            P->v[i].len = 0;                             /* (a voice on the old slot: silent) */
            P->v[i].cc = -1;
        }
}

/* one index entry: a sounding voice's zone first, else the first not done */
static void gl_build(gl_part_t *P, const track_t *t)
{
    uint32_t i, zl = 16u, k, e, pos;
    int32_t pred, idx;
    const smp_zone_t *z;
    for (i = 0; i < GL_POLY && zl == 16u; i++)
        if (t->v[i].active && P->v[i].user && P->v[i].zone < P->nz && P->zdone[P->v[i].zone] < P->zcnt[P->v[i].zone])
            zl = P->v[i].zone;
    for (i = 0; i < P->nz && zl == 16u; i++)
        if (P->zdone[i] < P->zcnt[i])
            zl = i;
    if (zl == 16u)
        return;
    z = &usr_zone[P->kit - 2u][zl];
    k = P->zdone[zl];
    e = P->zbase[zl] + k - 1u;
    pred = P->ipred[e];
    idx = P->iidx[e];
    pos = (k - 1u) * GL_SEG;
    for (i = 0; i < GL_SEG; i++)
        gl_dec(z, pos + i, &pred, &idx);
    P->ipred[e + 1u] = (int16_t)pred;
    P->iidx[e + 1u] = (uint8_t)idx;
    P->zdone[zl] = (uint16_t)(k + 1u);
}

/* per part and block: the index follows KIT and a new upload; two entries built */
static void glitch_block(track_t *t)
{
    gl_part_t *P;
    uint32_t kit = gl_kit(t->p), st;
    if (t < &trk[0] || t >= &trk[NLIVE] || !kit)
        return;
    P = gl_part(t);
    st = usr_nz[kit - 1u] ? smp_user_gen : 0xFFFFFFFFu;
    if (P->kit != kit + 1u || P->stamp != st)
        gl_reset(P, kit, st);
    gl_build(P, t);
    gl_build(P, t);
}

/* a user slice's window: chunk c of its zone (257 samples, the next chunk's first too), 0 = its entry not built yet */
static __attribute__((noinline)) int gl_chunk(gl_part_t *P, gl_slot_t *S, uint32_t c)
{
    const smp_zone_t *z = &usr_zone[(P->kit - 2u) % SMP_USER_SLOTS][S->zone & 15u];
    uint32_t i, n0 = c * GL_SEG, n, e;
    int32_t pred, idx;
    if (S->zone >= P->nz || c >= P->zdone[S->zone] || n0 >= z->n)
        return 0;
    e = P->zbase[S->zone] + c;
    pred = P->ipred[e];
    idx = P->iidx[e];
    n = z->n - n0 < GL_SEG + 1u ? z->n - n0 : GL_SEG + 1u;
    for (i = 0; i < n; i++)
        S->buf[i] = (int16_t)gl_dec(z, n0 + i, &pred, &idx);
    for (; i < GL_SEG + 1u; i++)
        S->buf[i] = 0;
    S->cc = (int32_t)c;
    return 1;
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
    uint32_t kit = gl_kit(p), nz = kit ? usr_nz[kit - 1u] : 0u, ns = kit ? (nz > 1u ? nz : (uint32_t)clamp(p[P_FM1_LEVEL], 2, 16))
                                                                       : GLT_NZ;
    uint32_t sl = ((uint32_t)v->note + (uint32_t)p[P_E0]) % ns;
    int32_t dn = (int32_t)v->note - 48, oct = dn >= 0 ? dn / (int32_t)ns : -((-dn + (int32_t)ns - 1) / (int32_t)ns);
    int32_t d16 = (p[P_E1] + 12 * oct) * 16;
    const smp_zone_t *z;
    if (!S)
        return;
    if (!S->rng)
        S->rng = (uint32_t)rng() | 1u;
    S->cc = -1;
    if (kit) {                                           /* a user slot: a zone a key, or one zone cut in CUTS */
        S->user = 1;
        S->len = 0;
        if (!nz)
            return;                                      /* (empty: silent, the UI says NO SAMPLE) */
        S->zone = (uint8_t)(nz > 1u ? sl : 0u);
        z = &usr_zone[kit - 1u][S->zone];
        S->len = nz > 1u ? z->n : z->n / ns;
        S->base = nz > 1u ? 0 : (int32_t)(sl * S->len);
        if (S->len < 16u)
            S->len = 0;
    } else {
        S->user = 0;
        z = &GLT_ZONES[sl];
        S->len = gl_decode(z, S->buf);
    }
    S->step = (int32_t)(((uint64_t)z->rate * pow2_q16(clamp(d16, -1536, 576))) >> 16);   /* Q16 samples a sample */
    S->wn = p[P_E2] ? (int32_t)(sx_expmap(127 - p[P_E2], 55u << 16, 5) >> 16) : (int32_t)S->len;   /* 5 .. 170 ms (at
                                                                                              * 11,025 Hz: 55 .. 1760) */
    if (p[P_E2])                                         /* (in time at any rate: 11,025 Hz is 16,384 in Q16) */
        S->wn = (int32_t)(((uint64_t)(uint32_t)S->wn * z->rate) >> 14);
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
            int32_t a, b, f = (S->pos >> 1) & 0x7FFF;
            if (S->user) {                               /* the window over the flash: its chunk, the next sample in it */
                uint32_t ab = (uint32_t)(S->base + ip), c = ab / GL_SEG;
                if ((int32_t)c != S->cc && !gl_chunk(gl_part(t), S, c))
                    S->cc = -1;
                a = S->cc >= 0 ? S->buf[ab % GL_SEG] : 0;
                b = S->cc >= 0 ? S->buf[ab % GL_SEG + 1u] : 0;
            } else {
                a = S->buf[ip];
                b = S->buf[ip + 1];
            }
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
    .block = glitch_block,
    .knob = {P_E2, P_E3, P_E5, P_E7},
    .poly = GL_POLY,
    .sampled = 1,
    .mem = sizeof(gl_part_t),
    .xyd = {{XY_E(2), XY_E(5), XY_E(4)}, {XY_E(7), XY_E(6), XY_E(1)}},   /* X: STUT CRSH REV (the break), Y: JUMP CHOP PTCH (the chaos) */
    .xya = {{40, 40, 30}, {50, 40, 20}},
};
