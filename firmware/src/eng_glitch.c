/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Tim (646timur-cell), on Felucca by Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* GLITCH (STRANGE): a slice machine on its own kit (tools/gen_strange_samples.py: 16 slices, 22,050 Hz, 0.3 .. 0.6 s:
 * glass (a tap, a crack, a rubbed rim, shards), short-wave air (a heterodyne tuning through zero beat, modulated data
 * clicks, static, a duck voice, morse, RTTY) and current (a neon lamp speeding up on its choke, choke hum, an arc, a
 * relay, a valve warming, a degauss).
 * Each key plays a slice: (note + SLCE) mod 16, the next 16 keys the same slices an octave up. Every slice is read
 * from flash through a seek index, so it can be read anywhere, either way:
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
 * into CUTS (OP1's LVL, 2..16) equal slices. Every slice, the kit's and a slot's, is read through a seek index (as GRAIN's: the
 * ADPCM state every 256 samples of every zone, built two entries a block in the block hook, the zones of the
 * sounding voices first) into a 257-sample window of the voice: any length the slot holds, any direction, any jump.
 * The windows (STUT) are in time, whatever the slot's rate.
 * Poly 3, its buffers and the index in the part's engine region (gl_part_t). */

#define GL_POLY 3
#define GL_LEN 257u                                      /* a window: 256 samples and the next one */

typedef struct {
    int16_t buf[GL_LEN];
    uint32_t len, rng;                                   /* the slice's samples; random numbers */
    int32_t pos, step;                                   /* the read position, its step (Q16, negative: backwards) */
    int32_t w0, wn, rep, chop;                           /* the window: its start, length (samples), repeats so far,
                                                          * dropped */
    int32_t hold, held;                                  /* CRSH's sample hold: samples left, the sample held */
    int32_t dcy;                                         /* MONOFM: DCAY's level (Q15), full at each hit */
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
    uint16_t zbase[16], zcnt[16], zdone[16];             /* a zone's entries: first, all, built (<= 16 zones a kit) */
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
/* the source a part plays (sc): 1..3 a user slot (KIT), else the machine's own built-in kit: 0 GLITCH's, 4 RITUAL's,
 * 5 MECH's, 6 BIO's (the four machines are this one engine) */
#define GL_RIT 4u
#define GL_MEC 5u
#define GL_BIO 6u
#define GL_ERR 7u
#define GL_BBX 8u                                        /* MONOFM's BBOX (DIGIPRO BEAT BOX): its drum kit */
static uint32_t gl_src(const track_t *t)
{
    uint32_t kit = gl_kit(t->p);
#if FELUCCA_MONOFM
    if (!kit && t->engine == ENGI_MBBOX)
        return GL_BBX;
#endif
    return kit ? kit : t->engine == ENGI_RITUAL ? GL_RIT : t->engine == ENGI_MECH ? GL_MEC : t->engine == ENGI_BIO ? GL_BIO : t->engine == ENGI_ERROR ? GL_ERR : 0u;
}
static uint32_t gl_nz(uint32_t sc)
{
#if FELUCCA_MONOFM
    if (sc == GL_BBX)
        return BBX_NZ;
#endif
    return sc - 1u < SMP_USER_SLOTS ? usr_nz[sc - 1u] : sc == GL_RIT ? RIT_NZ : sc == GL_MEC ? MEC_NZ : sc == GL_BIO ? BIO_NZ : sc == GL_ERR ? ERR_NZ : GLT_NZ;
}
static const smp_zone_t *gl_zone(uint32_t sc, uint32_t z)
{
#if FELUCCA_MONOFM
    if (sc == GL_BBX)
        return &BBX_ZONES[z % BBX_NZ];
#endif
    return sc - 1u < SMP_USER_SLOTS ? &usr_zone[sc - 1u][z & 15u] : sc == GL_RIT ? &RIT_ZONES[z % RIT_NZ]
                                                                 : sc == GL_MEC ? &MEC_ZONES[z % MEC_NZ]
                                                                 : sc == GL_BIO ? &BIO_ZONES[z % BIO_NZ]
                                                                 : sc == GL_ERR ? &ERR_ZONES[z % ERR_NZ] : &GLT_ZONES[z % GLT_NZ];
}

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

/* the index for source sc (kit = sc + 1): every zone from the state (0, 0), its entries to build */
static void gl_reset(gl_part_t *P, uint32_t kit, uint32_t stamp)
{
    uint32_t i, tot = 0, nz = gl_nz(kit), had = P->kit;
    P->kit = (uint8_t)(kit + 1u);
    P->stamp = stamp;
    P->nz = (uint8_t)nz;
    for (i = 0; i < nz; i++) {
        uint32_t c = (gl_zone(kit, i)->n + GL_SEG - 1u) / GL_SEG;
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
    z = gl_zone(P->kit - 1u, zl);
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
    uint32_t kit = gl_src(t), st;
    if (t < &trk[0] || t >= &trk[NLIVE])
        return;
    P = gl_part(t);
    st = kit - 1u >= SMP_USER_SLOTS ? 0u : usr_nz[kit - 1u] ? smp_user_gen : 0xFFFFFFFFu;
    if (P->kit != kit + 1u || P->stamp != st)
        gl_reset(P, kit, st);
    gl_build(P, t);
    gl_build(P, t);
}

/* a user slice's window: chunk c of its zone (257 samples, the next chunk's first too), 0 = its entry not built yet */
static __attribute__((noinline)) int gl_chunk(gl_part_t *P, gl_slot_t *S, uint32_t c)
{
    const smp_zone_t *z = gl_zone(P->kit - 1u, S->zone);
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

static int gl_roll(gl_slot_t *S, int32_t p) { return (int32_t)(px_rand(&S->rng) >> 25) < p; }   /* p / 128 */

/* a window begins: the dice for reversing, dropping, jumping */
static void gl_window(gl_slot_t *S, const int16_t *p, int first)
{
    int32_t wn = S->wn;
    if (!first && S->rep < (p[P_E3] >= 16 ? 0x7FFFFFFF : p[P_E3])) {
        S->rep++;                                        /* the same window again */
        S->pos = S->step >= 0 ? S->w0 << 16 : (S->w0 + wn - 2) << 16;
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
    S->pos = S->step >= 0 ? S->w0 << 16 : (S->w0 + wn - 2) << 16;   /* (backwards: from the sample before the last, the
                                                             interpolation's pair inside the slice: from its last one, REV at 127
                                                             was silent, the read never moved) */
}

static void glitch_note_on(track_t *t, voice_t *v)
{
    const int16_t *p = t->p;
    gl_slot_t *S = gl_slot(t, v);
    uint32_t kit = gl_src(t), nz = gl_nz(kit), ns = kit - 1u < SMP_USER_SLOTS && nz == 1u ? (FELUCCA_MONOFM ? 16u : (uint32_t)clamp(p[P_FM1_LEVEL], 2, 16)) : nz;   /* (MONOFM: CUTS's id is the FILTER's RES) */
    uint32_t sl = ns ? ((uint32_t)v->note + (uint32_t)p[P_E0]) % ns : 0u;
    int32_t dn = (int32_t)v->note - 48, oct = ns ? (dn >= 0 ? dn / (int32_t)ns : -((-dn + (int32_t)ns - 1) / (int32_t)ns)) : 0;
    int32_t d16 = (p[P_E1] + 12 * oct) * 16;
    const smp_zone_t *z;
    if (!S)
        return;
    if (!S->rng)
        S->rng = (uint32_t)rng() | 1u;
    S->cc = -1;
    S->user = 1;                                         /* (every kit through the index: the built-in one too) */
    S->len = 0;
    if (!nz)
        return;                                          /* (an empty slot: silent, the UI says NO SAMPLE) */
    S->zone = (uint8_t)(nz > 1u ? sl : 0u);              /* a zone a key, or one zone cut in CUTS */
    z = gl_zone(kit, S->zone);
    S->len = nz > 1u ? z->n : z->n / ns;
    S->base = nz > 1u ? 0 : (int32_t)(sl * S->len);
    if (S->len < 16u)
        S->len = 0;
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
    S->dcy = 32767;
    gl_window(S, p, 1);
}

static void glitch_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    gl_slot_t *S = gl_slot(t, v);
    uint32_t i, bits = (uint32_t)p[P_E5] / 9u, hn = 1u + (uint32_t)p[P_E5] / 8u, stut = p[P_E2] != 0;
    int32_t mask = -(1 << bits);
    int32_t dk = 32767, bb = 0;                          /* bb: BBOX's own kit, louder (26.10, Tim: too quiet) */
    if (!S || !S->len)
        return;
#if FELUCCA_MONOFM
    bb = gl_src(t) == GL_BBX;
#endif
    if (n > CTL)
        n = CTL;
#if FELUCCA_MONOFM
    if (p[P_E8] > 0) {                                   /* DCAY (the ninth): the hit falls, 1 the shortest (~5 ms) .. 127
                                                          * ~3 s to a tenth; 0: its whole length, as before */
        int32_t k = (int32_t)(32768u - 32768u * n / (uint32_t)(sx_expmap(p[P_E8], 220u << 16, 9) >> 16));
        S->dcy = mulq15(S->dcy, clamp(k, 0, 32767));
    }
    dk = S->dcy;
#endif
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
            {                                            /* the window over the flash: its chunk, the next sample in it */
                uint32_t ab = (uint32_t)(S->base + ip), c = ab / GL_SEG;
                if ((int32_t)c != S->cc && !gl_chunk(gl_part(t), S, c))
                    S->cc = -1;
                a = S->cc >= 0 ? S->buf[ab % GL_SEG] : 0;
                b = S->cc >= 0 ? S->buf[ab % GL_SEG + 1u] : 0;
            }
            s = a + (((b - a) * f) >> 15);
            S->pos += S->step;
        } else if (ip >= 0 && S->step) {
            S->pos += S->step;                            /* (at the slice's edge: on, to the window's end) */
        }
        if (S->chop)
            s = 0;
        if (dk < 32767)
            s = mulq15(s, dk);
        if (hn > 1u) {                                   /* CRSH: the rate held, the bits dropped */
            if (S->hold-- <= 0) {
                S->hold = (int32_t)hn - 1;
                S->held = s & mask;
            }
            s = S->held;
        }
        if (bb)                                          /* (x1.8 into a knee: ~5 dB more, the peaks held) */
            s = clamp(soft_knee((s * 29) >> 4, 24000), -32767, 32767);
        out[i] += voice_amp(s, m, i);
    }
}

#define STRANGE_CAT_GLITCH "FFFFFFFFFF"
static const preset_t GLITCH_PRESETS[] = {
    /* name, {SLCE, PTCH, STUT, REP, REV, CRSH, CHOP, JUMP}, {A D S R}, fenv, mono */
    {"GLASS & AIR", {0, 0, 0, 1, 0, 0, 0, 0}, {0, 64, 127, 30}, 0, 0, FX(0, 0, 0, 20)},
    {"STUTTER", {0, 0, 70, 3, 0, 0, 0, 0}, {0, 64, 127, 30}, 0, 0, FX(0, 0, 10, 20)},
    {"CHOKE BUZZ", {11, 0, 110, 16, 0, 20, 0, 0}, {0, 64, 127, 40}, 0, 0, FX(0, 0, 0, 30)},
    {"BACKWARDS", {0, -5, 40, 2, 100, 0, 0, 0}, {0, 64, 127, 40}, 0, 0, FX(0, 0, 20, 40)},
    {"DEAD AIR", {4, 0, 90, 2, 20, 30, 50, 30}, {0, 64, 127, 40}, 0, 0, FX(0, 0, 10, 30)},
    {"SW DATA", {5, 7, 60, 4, 60, 60, 30, 90}, {40, 64, 127, 90}, 0, 0, FX(0, 0, 40, 90)},
    {"CRUSH ARC", {12, -12, 30, 2, 30, 110, 10, 20}, {0, 64, 127, 30}, 0, 0, FX(0, 0, 0, 20)},
    {"RTTY CHOIR", {9, -12, 100, 16, 40, 20, 0, 50}, {90, 64, 127, 110}, 0, 0, FX(0, 30, 30, 110)},
    {"NEON STRIKE", {10, 0, 80, 6, 0, 10, 20, 60}, {0, 64, 127, 60}, 0, 0, FX(0, 0, 30, 60)},
    {"GHOST VALVE", {14, -24, 50, 3, 80, 40, 30, 70}, {40, 64, 127, 120}, 0, 0, FX(0, 40, 40, 120)},
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
#if FELUCCA_MONOFM
    .e8 = {"DCAY", F_PCT, 0, 127, 0, 0, 0},
#endif
};

/* RITUAL, MECH and BIO (STRANGE): GLITCH with kits of their own (tools/gen_strange_samples.py, 11,025 Hz, C3 .. D#4):
 *   RITUAL  BELL, GONG, CHAINS, STONE GRIND, STONE KNOCK, CANDLE, CHOIR BREATH, CAVE DRIP, COFFIN, BONES, FRAME DRUM,
 *           BOWL, THROAT, CRYPT WIND, DOOR CREAK, TOLLING
 *   BIO     HEARTBEAT, BREATH IN, BREATH OUT, TONGUE CLICK, KNUCKLES, SQUELCH, TWIG SNAP, LEAVES, BUBBLES, RAIN ON
 *           LEAVES, CICADA, FROG, BIRD, WOOD CREAK, PURR, GULP
 *   MECH    CLOCK TICK, TOCK, RATCHET, TYPEWRITER, CARRIAGE, REEL UP, TAPE STOP, PROJECTOR, PISTON, WIND-UP, SHUTTER,
 *           SERVO, HAMMER, CHAIN DRIVE, STEAM, MUSIC BOX
 * the same knobs, KIT too (a user slot instead of the kit) */
#define STRANGE_CAT_RITUAL "FFFFFFFFFF"
static const preset_t RITUAL_PRESETS[] = {
    /* name, {SLCE, PTCH, STUT, REP, REV, CRSH, CHOP, JUMP}, {A D S R}, fenv, mono */
    {"CRYPT KIT", {0, 0, 0, 1, 0, 0, 0, 0}, {0, 64, 127, 50}, 0, 0, FX(0, 0, 10, 50)},
    {"LOW TOLL", {15, -12, 0, 1, 0, 0, 0, 0}, {0, 64, 127, 110}, 0, 0, FX(0, 20, 20, 110)},
    {"CHAIN DRAG", {2, -5, 60, 3, 30, 0, 10, 40}, {0, 64, 127, 60}, 0, 0, FX(0, 0, 20, 70)},
    {"BONE RAIN", {9, 0, 40, 2, 20, 20, 20, 70}, {0, 64, 127, 50}, 0, 0, FX(0, 0, 10, 60)},
    {"STONE ALTAR", {3, -7, 90, 6, 40, 10, 0, 30}, {30, 64, 127, 90}, 0, 0, FX(10, 0, 0, 80)},
    {"CANDLE VIGIL", {5, 0, 110, 16, 30, 0, 20, 60}, {60, 64, 127, 110}, 0, 0, FX(0, 20, 30, 100)},
    {"BREATH CHOIR", {6, -12, 100, 16, 50, 0, 0, 40}, {80, 64, 127, 120}, 0, 0, FX(0, 40, 30, 120)},
    {"DRIP CAVE", {7, 5, 0, 1, 10, 0, 0, 20}, {0, 64, 127, 90}, 0, 0, FX(0, 0, 40, 110)},
    {"BURIAL", {8, -12, 50, 2, 40, 30, 10, 30}, {0, 64, 127, 80}, 0, 0, FX(20, 0, 10, 90)},
    {"THROAT RITE", {12, -12, 60, 16, 20, 10, 0, 20}, {70, 64, 127, 120}, 0, 0, FX(10, 30, 20, 110)},
};
#define STRANGE_CAT_MECH "FFFFFFFFFF"
static const preset_t MECH_PRESETS[] = {
    {"WORKSHOP", {4, 0, 0, 1, 0, 0, 0, 0}, {0, 64, 127, 40}, 0, 0, FX(0, 0, 0, 30)},
    {"CLOCKWORK", {2, 0, 80, 4, 0, 0, 10, 20}, {0, 64, 127, 50}, 0, 0, FX(0, 0, 10, 40)},
    {"TYPE POOL", {3, 0, 30, 2, 10, 10, 20, 70}, {0, 64, 127, 40}, 0, 0, FX(0, 0, 10, 40)},
    {"REEL TO REEL", {5, 0, 100, 16, 0, 0, 0, 0}, {20, 64, 127, 80}, 0, 0, FX(0, 20, 20, 60)},
    {"TAPE DEATH", {6, -5, 60, 2, 60, 30, 20, 30}, {0, 64, 127, 80}, 0, 0, FX(10, 0, 20, 70)},
    {"PROJECTOR", {7, 0, 90, 16, 0, 20, 10, 10}, {10, 64, 127, 70}, 0, 0, FX(0, 0, 10, 50)},
    {"STEAM WORKS", {8, -7, 50, 3, 20, 20, 10, 40}, {0, 64, 127, 70}, 0, 0, FX(20, 0, 10, 60)},
    {"FORGE", {12, -12, 20, 2, 0, 40, 0, 20}, {0, 64, 127, 70}, 0, 0, FX(30, 0, 0, 70)},
    {"SERVO SWARM", {11, 7, 70, 4, 30, 10, 20, 60}, {30, 64, 127, 90}, 0, 0, FX(0, 20, 20, 80)},
    {"BROKEN BOX", {15, 0, 40, 3, 40, 20, 30, 50}, {0, 64, 127, 90}, 0, 0, FX(0, 20, 30, 90)},
};
#define STRANGE_CAT_BIO "FFFFFFFFFF"
static const preset_t BIO_PRESETS[] = {
    /* name, {SLCE, PTCH, STUT, REP, REV, CRSH, CHOP, JUMP}, {A D S R}, fenv, mono */
    {"BODY KIT", {0, 0, 0, 1, 0, 0, 0, 0}, {0, 64, 127, 50}, 0, 0, FX(0, 0, 10, 40)},
    {"HEART LOOP", {0, -5, 90, 16, 0, 0, 0, 0}, {0, 64, 127, 80}, 0, 0, FX(10, 0, 10, 60)},
    {"BREATHING", {1, 0, 60, 3, 50, 0, 0, 30}, {40, 64, 127, 110}, 0, 0, FX(0, 20, 20, 100)},
    {"PULHRA NIGHT", {11, 0, 50, 4, 20, 0, 20, 60}, {30, 64, 127, 110}, 0, 0, FX(0, 20, 30, 110)},
    {"CICADA HEAT", {10, 0, 90, 3, 0, 20, 10, 20}, {60, 64, 127, 110}, 0, 0, FX(0, 0, 20, 90)},
    {"FOREST RAIN", {9, 0, 70, 6, 30, 0, 10, 80}, {50, 64, 127, 110}, 0, 0, FX(0, 0, 30, 110)},
    {"CRUNCH", {4, -7, 30, 2, 20, 60, 20, 40}, {0, 64, 127, 50}, 0, 0, FX(20, 0, 0, 40)},
    {"WET LAB", {5, -5, 50, 3, 40, 20, 10, 50}, {0, 64, 127, 80}, 0, 0, FX(0, 20, 20, 70)},
    {"DAWN BIRDS", {12, 0, 40, 2, 20, 0, 20, 70}, {30, 64, 127, 100}, 0, 0, FX(0, 20, 30, 110)},
    {"PURR DRONE", {14, -12, 80, 16, 10, 10, 0, 10}, {60, 64, 127, 120}, 0, 0, FX(10, 20, 20, 110)},
};
#define GL_EDIT {{"SLCE", F_INT, 0, 15, 0, 0, 0}, {"PTCH", F_SEMI, -24, 24, 0, 0, 0}, {"STUT", F_PCT, 0, 127, 0, 0, 0}, \
                 {"REP", F_INT, 1, 16, 1, 0, 0}, {"REV", F_PCT, 0, 127, 0, 0, 0}, {"CRSH", F_PCT, 0, 127, 0, 0, 0}, \
                 {"CHOP", F_PCT, 0, 127, 0, 0, 0}, {"JUMP", F_PCT, 0, 127, 0, 0, 0}}
static const engine_t ENG_RITUAL = {
    .name = "RITUAL",
    .page_title = {"SLICE", "BREAK"},
    .edit = GL_EDIT,
    .presets = RITUAL_PRESETS,
    .npresets = NELEM(RITUAL_PRESETS),
    .note_on = glitch_note_on,
    .render = glitch_render,
    .block = glitch_block,
    .knob = {P_E2, P_E3, P_E5, P_E7},
    .poly = GL_POLY,
    .sampled = 1,
    .mem = sizeof(gl_part_t),
    .xyd = {{XY_E(2), XY_E(5), XY_E(4)}, {XY_E(7), XY_E(6), XY_E(1)}},
    .xya = {{40, 40, 30}, {50, 40, 20}},
#if FELUCCA_MONOFM
    .e8 = {"DCAY", F_PCT, 0, 127, 0, 0, 0},
#endif
};
static const engine_t ENG_MECH = {
    .name = "MECH",
    .page_title = {"SLICE", "BREAK"},
    .edit = GL_EDIT,
    .presets = MECH_PRESETS,
    .npresets = NELEM(MECH_PRESETS),
    .note_on = glitch_note_on,
    .render = glitch_render,
    .block = glitch_block,
    .knob = {P_E2, P_E3, P_E5, P_E7},
    .poly = GL_POLY,
    .sampled = 1,
    .mem = sizeof(gl_part_t),
    .xyd = {{XY_E(2), XY_E(5), XY_E(4)}, {XY_E(7), XY_E(6), XY_E(1)}},
    .xya = {{40, 40, 30}, {50, 40, 20}},
#if FELUCCA_MONOFM
    .e8 = {"DCAY", F_PCT, 0, 127, 0, 0, 0},
#endif
};
static const engine_t ENG_BIO = {
    .name = "BIO",
    .page_title = {"SLICE", "BREAK"},
    .edit = GL_EDIT,
    .presets = BIO_PRESETS,
    .npresets = NELEM(BIO_PRESETS),
    .note_on = glitch_note_on,
    .render = glitch_render,
    .block = glitch_block,
    .knob = {P_E2, P_E3, P_E5, P_E7},
    .poly = GL_POLY,
    .sampled = 1,
    .mem = sizeof(gl_part_t),
    .xyd = {{XY_E(2), XY_E(5), XY_E(4)}, {XY_E(7), XY_E(6), XY_E(1)}},
    .xya = {{40, 40, 30}, {50, 40, 20}},
#if FELUCCA_MONOFM
    .e8 = {"DCAY", F_PCT, 0, 127, 0, 0, 0},
#endif
};
/* ERROR: the classic broken digital (the first GLITCH kit, 11,025 Hz): crushed noise, click trains, an FM chirp, a
 * stutter, dropouts, a zap, a rate sweep, crackle, a modem, a thump, a CD skip, LFSR data, a faulty beep, a tick, a
 * reversed swell, a grain smear */
#define STRANGE_CAT_ERROR "FFFFFFFFFF"
static const preset_t ERROR_PRESETS[] = {
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
static const engine_t ENG_ERROR = {
    .name = "ERROR",
    .page_title = {"SLICE", "BREAK"},
    .edit = GL_EDIT,
    .presets = ERROR_PRESETS,
    .npresets = NELEM(ERROR_PRESETS),
    .note_on = glitch_note_on,
    .render = glitch_render,
    .block = glitch_block,
    .knob = {P_E2, P_E3, P_E5, P_E7},
    .poly = GL_POLY,
    .sampled = 1,
    .mem = sizeof(gl_part_t),
    .xyd = {{XY_E(2), XY_E(5), XY_E(4)}, {XY_E(7), XY_E(6), XY_E(1)}},
    .xya = {{40, 40, 30}, {50, 40, 20}},
#if FELUCCA_MONOFM
    .e8 = {"DCAY", F_PCT, 0, 127, 0, 0, 0},
#endif
};
#if FELUCCA_MONOFM
/* BBOX (MONOFM, after the Monomachine's DIGIPRO BEAT BOX): GLITCH with a 12-bit drum kit (tools/gen_strange_samples.py
 * bbox_kit, 12,000 Hz, C3 .. D#4): BD, BD LONG, SD, SD SNAP, CLAP, RIM, HH CL, HH OP, TOM LO, TOM MID, TOM HI, COWBELL,
 * CLAVE, CYMBAL, SHAKER, ZAP. STUT and REP are its retrigs; poly: a kit plays its hits over each other */
#define MONO_CAT_MBBOX "DDDDDD"
static const preset_t MBBOX_PRESETS[] = {
    {"BEAT BOX", {0, 0, 0, 1, 0, 0, 0, 0}, {0, 64, 127, 30}, 0, 0, FX(0, 0, 0, 10)},
    {"12 BIT KIT", {0, 0, 0, 1, 0, 40, 0, 0}, {0, 64, 127, 30}, 0, 0, FX(10, 0, 0, 10)},
    {"LOW KIT", {0, -5, 0, 1, 0, 20, 0, 0}, {0, 64, 127, 30}, 0, 0, FX(10, 0, 0, 20)},
    {"ROLLS", {0, 0, 60, 4, 0, 0, 0, 0}, {0, 64, 127, 30}, 0, 0, FX(0, 0, 10, 10)},
    {"TRASH KIT", {0, 3, 30, 2, 20, 90, 20, 20}, {0, 64, 127, 30}, 0, 0, FX(20, 0, 10, 20)},
    {"REVERSE KIT", {0, 0, 20, 1, 127, 0, 0, 0}, {0, 64, 127, 30}, 0, 0, FX(0, 0, 20, 40)},
};
static const engine_t ENG_MBBOX = {
    .name = "BBOX",
    .page_title = {"SLICE", "BREAK"},
    .edit = GL_EDIT,
    .presets = MBBOX_PRESETS,
    .npresets = NELEM(MBBOX_PRESETS),
    .note_on = glitch_note_on,
    .render = glitch_render,
    .block = glitch_block,
    .knob = {P_E1, P_E2, P_E3, P_E5},
    .poly = GL_POLY,
    .sampled = 1,
    .mem = sizeof(gl_part_t),
    .xyd = {{XY_E(2), XY_E(5), XY_E(4)}, {XY_E(1), XY_E(6), XY_E(7)}},
    .xya = {{40, 40, 30}, {30, 40, 40}},
#if FELUCCA_MONOFM
    .e8 = {"DCAY", F_PCT, 0, 127, 0, 0, 0},
#endif
};
#endif
#undef GL_EDIT
