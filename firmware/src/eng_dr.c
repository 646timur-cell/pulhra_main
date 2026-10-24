/* SPDX-License-Identifier: GPL-3.0-only */
/* PULHRA's drum machines (the EDIT layer's DRUM page; FELUCCA_SWAMP builds only), each a synthesized drum of its own after
 * the analog drum machines (Elektron's Analog Rhythm, the 808 / 909 circuits): a hit is the whole sound (engine_t.oneshot:
 * the ADSR does nothing, velocity does), one voice a track that rings (a new hit chokes the old one in ~2 ms: no pile-up),
 * every value lockable as any machine's. Fixed point, 32-bit products only; the slow envelopes per control block
 * (drum_voice.c's helpers: dv_kb, dv_k, dv_slow).
 *
 * KICK: a body oscillator (TYPE: SINE, TRI, ROUND: the sine with its 2nd and 3rd harmonics, phase-locked) swept down
 * from SWEEP semitones above TUNE (an exponential in frequency, STIME its time constant: the analog circuit's pitch
 * envelope), held at full level for HOLD then decaying (DECAY's time constant, 14 ms .. 1 s), a click (a windowed burst
 * of 3 kHz, 1.5 ms, CLICK's level) and a burst of noise (NOISE's level, 4 ms), into a soft clip (DRIVE, up to x8, the
 * level kept). The note moves TUNE (C3, the trigs' default, = TUNE as set): a melodic kick from the keys.
 *
 * Voice state: ph[0] the body's phase, ph[1] the click's, ph[2] the click's window; s[0] the amplitude (Q30, dv_slow),
 * s[1] the sweep (Q15, per sample), s[2] the hold left (samples), s[3] the noise's level (Q15), s[4] the noise's
 * low-pass, s[5] the click's samples left, s[6] choked (1), s[7] the noise's state */

enum { DR_KTY_SINE, DR_KTY_TRI, DR_KTY_ROUND, DR_KTY_N };
static const char *const N_DR_KTYPE[DR_KTY_N] = {"SINE", "TRI", "ROUND"};

#define DR_Q30 (1 << 30)
#define DR_CLICK_N 64u                                    /* the click: 64 samples (1.45 ms) */
#define DR_CHOKE_US 2000u                                 /* a choked hit's time constant */

/* a choke: the track's other hits of this engine let go fast (a new hit replaces them: the drum is one voice) */
static void dr_choke_others(track_t *t, const voice_t *v)
{
    uint32_t i;
    for (i = 0; i < NVOICE; i++)
        if (&t->v[i] != v && t->v[i].active)
            t->v[i].s[6] = 1;
}

/* the amplitude of a hit: 1.0 until its own envelope has rung out (-84 dB), then the voice ends (the ADSR never does) */
static int32_t dr_amp(track_t *t, voice_t *v, int32_t adsr)
{
    (void)t;
    (void)adsr;
    if (!v->active)
        return 0;
    if (v->s[0] < (1 << 16) && v->s[2] <= 0) {
        v->active = v->gate = 0;
        v->stage = 0;
        v->env = 0;
        return 0;
    }
    v->env = 1 << 24;                                     /* (the ADSR's release never ends a hit) */
    return 32767;
}

/* ===================================================================== KICK === */
static void dkick_note_on(track_t *t, voice_t *v)
{
    dr_choke_others(t, v);
    v->ph[0] = 0x40000000u;                               /* (the sine from its peak: the first half-cycle a thump) */
    v->ph[1] = v->ph[2] = 0;
    v->s[0] = DR_Q30;
    v->s[1] = 32767;
    v->s[2] = (int32_t)((uint32_t)t->p[P_E4] * (FS / 635u));   /* HOLD: 0 .. 200 ms */
    v->s[3] = 32767;
    v->s[4] = 0;
    v->s[5] = (int32_t)DR_CLICK_N;
    v->s[6] = 0;
    if (!v->s[7])
        v->s[7] = (int32_t)(rng() | 1u);
}

static void dkick_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    int32_t note = clamp(m->pitch16 - 48 * 16, -48 * 16, 48 * 16);   /* (C3: the trigs' default note) */
    int32_t base = 23 * 16 + clamp(p[P_E0], 0, 127) * 28 * 16 / 127 + note;   /* TUNE: 26 .. 130 Hz at C3 */
    uint32_t inc0 = pitch_inc((uint32_t)clamp(base, 0, 2047));
    uint32_t top = pitch_inc((uint32_t)clamp(base + clamp(p[P_E1], 0, 127) * 6, 0, 2047));   /* SWEEP: up to 48 st */
    uint32_t span = (top - inc0) >> 15, ks = dv_k(TIME_MS_X10[clamp(p[P_E2], 0, 127) >> 1] * 100u);   /* STIME: 1 .. 96 ms */
    uint32_t kb = dv_kb(TIME_MS_X10[36 + clamp(p[P_E3], 0, 127) * 60 / 127] * 100u);   /* DECAY: 14 ms .. 1.06 s */
    uint32_t kn = dv_k(4000u), ph = v->ph[0], cph = v->ph[1], wph = v->ph[2];
    uint32_t cinc = pitch_inc(107 * 16), winc = 0xFFFFFFFFu / DR_CLICK_N;   /* (B7 ~ 3.95 kHz; the window's one cycle) */
    int32_t drv = clamp(p[P_E5], 0, 127), click = clamp(p[P_E6], 0, 127) * 258, nlev = clamp(p[P_E7], 0, 127) * 200;
    int32_t type = clamp(p[P_E8], 0, DR_KTY_N - 1), g = 4096 + drv * 228, comp = 32767 - drv * 110;
    int32_t a, d, sw = v->s[1], hold = v->s[2], nl = v->s[3], lp = v->s[4], cn = v->s[5];
    uint32_t i;
    if (v->s[6]) {                                        /* choked: out fast, no click or noise any more */
        kb = dv_kb(DR_CHOKE_US);
        hold = 0;
        cn = 0;
        nl = 0;
    }
    if (hold > 0) {                                       /* (HOLD: the amplitude flat this block) */
        hold -= (int32_t)n;
        a = v->s[0] >> 10;
        d = 0;
    } else {
        dv_slow(&v->s[0], kb, &a, &d);
    }
    for (i = 0; i < n; i++) {
        int32_t b, x;
        ph += inc0 + span * (uint32_t)sw;
        sw = (sw * (int32_t)ks) >> 15;
        if (type == DR_KTY_TRI)
            b = osc_tri(ph);
        else if (type == DR_KTY_ROUND)
            b = (sine_i(ph) * 22000 + sine_i(ph << 1) * 8000 + sine_i(ph * 3u) * 4000) >> 15;
        else
            b = sine_i(ph);
        x = (b * (a >> 5)) >> 15;                         /* (a: Q20 -> Q15) */
        a += d;
        if (cn > 0) {                                     /* the click: a windowed burst, mean 0 */
            int32_t w = (32768 - sine_i(wph + 0x40000000u)) >> 1;
            x += (((sine_i(cph) * w) >> 15) * click) >> 15;
            cph += cinc;
            wph += winc;
            cn--;
        }
        if (nl > 64) {                                    /* the noise burst, a little low-passed */
            int32_t z = (int32_t)noise32(&v->s[7]) >> 16;
            lp += (z - lp) >> 1;
            x += (((lp * nl) >> 15) * nlev) >> 15;
            nl = (nl * (int32_t)kn) >> 15;
        }
        x = clamp(x, -65535, 65535);
        if (drv)
            x = (softclip(((x >> 1) * g) >> 11) * comp) >> 15;
        else
            x = clamp(soft_knee(x, 28000), -32767, 32767);  /* (the click and the noise on a full body: a knee, no wrap) */
        out[i] += voice_amp(x, m, i);
    }
    v->ph[0] = ph;
    v->ph[1] = cph;
    v->ph[2] = wph;
    v->s[1] = sw;
    v->s[2] = hold;
    v->s[3] = nl;
    v->s[4] = lp;
    v->s[5] = cn;
}

#define DR_CAT_KICK "DDDD"
static const preset_t DKICK_PRESETS[] = {
    /* TUNE SWEEP STIME DECAY HOLD DRIVE CLICK NOISE */
    {"BD ANALOG", {40, 60, 40, 60, 10, 20, 40, 10}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 0)},
    {"BD PUNCH", {48, 80, 24, 40, 6, 50, 90, 30}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 0)},
    {"BD LONG", {30, 50, 50, 100, 30, 10, 20, 0}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 0)},
    {"BD HARD", {44, 90, 30, 50, 4, 110, 100, 50}, {0, 64, 127, 30}, 0, 1, FX(20, 0, 0, 0)},
};
static const engine_t ENG_DKICK = {
    .name = "KICK", .page_title = {"BODY", "HIT"}, .presets = DKICK_PRESETS, .npresets = NELEM(DKICK_PRESETS),
    .note_on = dkick_note_on, .render = dkick_render, .amp = dr_amp, .oneshot = 1, .poly = 2,
    .knob = {P_E0, P_E1, P_E3, P_E5},
    .edit = {{"TUNE", F_PCT, 0, 127, 40, 0, 0}, {"SWEP", F_PCT, 0, 127, 60, 0, 0}, {"STIM", F_PCT, 0, 127, 40, 0, 0},
             {"DCAY", F_PCT, 0, 127, 60, 0, 0}, {"HOLD", F_PCT, 0, 127, 10, 0, 0}, {"DRIV", F_PCT, 0, 127, 20, 0, 0},
             {"CLIK", F_PCT, 0, 127, 40, 0, 0}, {"NOIS", F_PCT, 0, 127, 10, 0, 0}},
    .e8 = {"TYPE", F_ENUM, 0, DR_KTY_N - 1, 0, N_DR_KTYPE, 0},
    .xyd = {{XY_E(1), XY_E(5), 0}, {XY_E(3), XY_E(6), 0}},   /* X: the sweep, the drive; Y: the decay, the click */
    .xya = {{40, 40, 0}, {50, 30, 0}},
};

/* ======================================================= the other drums: shared === */
/* SNARE CLAP HAT CYM keep their state in the part's engine memory (engines.c ENG_MEM), a slot a voice. The voice's own:
 * s[0] the main amplitude (Q30: the hit ends below -84 dB, dr_amp), s[2] samples still to come before it may end (CLAP's
 * bursts), s[6] choked */
typedef struct {
    uint32_t ph[8];              /* oscillators: the shells, the metal's six squares, CYM's bell */
    int32_t q[2];                /* slow envelopes, Q30 (dv_slow) */
    int32_t e[2];                /* fast envelopes, Q15 per sample */
    int32_t f[4];                /* filter states */
    int32_t rng, cnt, n, x1;     /* noise; CLAP: samples to the next burst, bursts left; a high-pass's last input */
} dr_slot_t;
ENG_MEM_FITS("DRUM machines", sizeof(dr_slot_t) * NVOICE);
static dr_slot_t *dr_slot(track_t *t, voice_t *v)
{
    return (dr_slot_t *)ENG_MEM(t - trk) + (uint32_t)(v - t->v) % NVOICE;
}
static void dr_start(track_t *t, voice_t *v)
{
    dr_slot_t *S = dr_slot(t, v);
    int32_t r = S->rng;
    dr_choke_others(t, v);
    memset(S, 0, sizeof *S);
    S->rng = r ? r : (int32_t)(rng() | 1u);
    S->q[0] = S->q[1] = DR_Q30;
    S->e[0] = S->e[1] = 32767;
    v->s[0] = DR_Q30;
    v->s[2] = 0;
    v->s[6] = 0;
}
#define DR_NOISE(S) ((int32_t)noise32(&(S)->rng) >> 16)  /* white, Q15 */
/* the end of a hit's block: drive (0..127: up to x8, the level kept) or a knee, never past full scale */
static inline int32_t dr_out(int32_t x, int32_t drv)
{
    x = clamp(x, -65535, 65535);
    if (drv)
        return (softclip(((x >> 1) * (4096 + drv * 228)) >> 11) * (32767 - drv * 110)) >> 15;
    return clamp(soft_knee(x, 28000), -32767, 32767);
}
/* a time constant from a knob: TIME_MS_X10's place lo + v (0..127) * span / 127, in us */
static uint32_t dr_tau(int32_t v, uint32_t lo, uint32_t span)
{
    return TIME_MS_X10[clamp((int32_t)lo + clamp(v, 0, 127) * (int32_t)span / 127, 0, 127)] * 100u;
}
/* a pitch (1/16 semitone) -> the SVF's cutoff index */
static int32_t dr_cut(int32_t p16) { return dv_cut(clamp(p16, 0, 2047)); }

/* ==================================================================== SNARE === */
/* two shells (TUNE, the second DTUN semitones above, at 0.6), a pitch drop on them (SWEEP, ~15 ms), the body's decay;
 * noise through a band-pass at NTON (1 .. 8 kHz) with its own level and decay; SNAP: a high-passed spike of noise (3 ms) */
static void dsnare_note_on(track_t *t, voice_t *v)
{
    dr_slot_t *S;
    dr_start(t, v);
    S = dr_slot(t, v);
    S->ph[0] = S->ph[1] = 0x40000000u;
}
static void dsnare_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    dr_slot_t *S = dr_slot(t, v);
    int32_t note = clamp(m->pitch16 - 48 * 16, -48 * 16, 48 * 16);
    int32_t b16 = (47 << 4) + clamp(p[P_E0], 0, 127) * (20 << 4) / 127 + note;   /* TUNE: 123 .. 392 Hz at C3 */
    uint32_t inc1 = pitch_inc((uint32_t)clamp(b16, 0, 2047));
    uint32_t inc2 = pitch_inc((uint32_t)clamp(b16 + clamp(p[P_E1], 0, 127) * (12 << 4) / 127, 0, 2047));
    uint32_t kb = dv_kb(dr_tau(p[P_E2], 30, 40)), kn = dv_kb(dr_tau(p[P_E4], 32, 45));
    uint32_t ksn = dv_k(2500u), ksw = dv_k(15000u);
    int32_t nlev = clamp(p[P_E3], 0, 127) * 258, snap = clamp(p[P_E6], 0, 127) * 258, drv = clamp(p[P_E7], 0, 127);
    int32_t swd = clamp(p[P_E8], 0, 127);                 /* SWEEP: up to an octave above, gliding down */
    int32_t ab, db, an, dn, sn = S->e[0], sw = S->e[1], x1 = S->x1;
    tsvf_t c;
    uint32_t i;
    tsvf_coef(&c, dr_cut((83 << 4) + clamp(p[P_E5], 0, 127) * (24 << 4) / 127), 50);   /* NTON: 1 .. 4 kHz .. 8 kHz */
    if (v->s[6]) {
        kb = kn = dv_kb(DR_CHOKE_US);
        sn = 0;
    }
    dv_slow(&S->q[0], kb, &ab, &db);
    dv_slow(&S->q[1], kn, &an, &dn);
    for (i = 0; i < n; i++) {
        int32_t z = DR_NOISE(S), x, bp;
        uint32_t ext = (uint32_t)((sw * swd) >> 7);       /* (Q15 x 0..127 -> Q15 share of an octave) */
        x = (sine_i(S->ph[0]) * 3 + sine_i(S->ph[1]) * 2) / 5;
        S->ph[0] += inc1 + (uint32_t)(((inc1 >> 15) * ext));
        S->ph[1] += inc2 + (uint32_t)(((inc2 >> 15) * ext));
        sw = (sw * (int32_t)ksw) >> 15;
        x = (x * (ab >> 5)) >> 15;
        ab += db;
        bp = dv_bp(&c, z, &S->f[0]);
        x += (((clamp(bp * 2, -32767, 32767) * (an >> 5)) >> 15) * nlev) >> 15;
        an += dn;
        if (sn > 64) {
            x += ((((z - x1) >> 1) * sn >> 15) * snap) >> 15;
            sn = (sn * (int32_t)ksn) >> 15;
        }
        x1 = z;
        out[i] += voice_amp(dr_out(x, drv), m, i);
    }
    S->e[0] = sn;
    S->e[1] = sw;
    S->x1 = x1;
    v->s[0] = S->q[0] > S->q[1] ? S->q[0] : S->q[1];
}

/* ===================================================================== CLAP === */
/* noise through a band-pass (TONE, RES), struck COUNT times SPRD apart (HUMN: each gap a little off), each strike a fast
 * decay (3.5 ms), the last one followed by the tail (DCAY, TAIL's level) */
static void dclap_note_on(track_t *t, voice_t *v)
{
    dr_slot_t *S;
    dr_start(t, v);
    S = dr_slot(t, v);
    S->n = clamp(t->p[P_E2], 1, 6) - 1;                   /* (the bursts after the first) */
    S->cnt = 0;
    v->s[2] = 1;
}
static void dclap_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    dr_slot_t *S = dr_slot(t, v);
    int32_t note = clamp(m->pitch16 - 48 * 16, -24 * 16, 24 * 16) + clamp(p[P_E8], -64, 63) * (12 << 4) / 64;
    int32_t gap = (int32_t)(FS / 250u) + clamp(p[P_E3], 0, 127) * (int32_t)(FS / 63u) / 127;   /* SPRD: 4 .. 20 ms */
    int32_t hum = clamp(p[P_E6], 0, 127), tail = clamp(p[P_E5], 0, 127) * 258, drv = clamp(p[P_E7], 0, 127);
    uint32_t kb = dv_kb(dr_tau(p[P_E4], 40, 45)), kf = dv_k(3500u);
    int32_t at, dt, be = S->e[0];
    tsvf_t c;
    uint32_t i;
    tsvf_coef(&c, dr_cut((75 << 4) + clamp(p[P_E0], 0, 127) * (20 << 4) / 127 + note), clamp(p[P_E1], 0, 127));
    if (v->s[6]) {
        kb = dv_kb(DR_CHOKE_US);
        S->n = 0;
        be = 0;
    }
    if (S->n > 0)                                         /* (the tail waits for the last burst) */
        at = S->q[0] >> 10, dt = 0;
    else
        dv_slow(&S->q[0], kb, &at, &dt);
    for (i = 0; i < n; i++) {
        int32_t z = DR_NOISE(S), x, env;
        if (S->n > 0 && ++S->cnt >= gap) {                /* the next burst */
            int32_t j = hum ? (DR_NOISE(S) * hum) >> 14 : 0;   /* (up to +-1/4 gap off) */
            S->cnt = j * gap >> 15;
            S->n--;
            be = 32767;
        }
        x = dv_bp(&c, z, &S->f[0]);
        x = clamp(x * 2, -32767, 32767);
        env = be + (S->n > 0 ? 0 : (((at >> 5) * tail) >> 15));
        x = (x * clamp(env, 0, 32767)) >> 15;
        be = (be * (int32_t)kf) >> 15;
        if (S->n == 0)
            at += dt;
        out[i] += voice_amp(dr_out(x, drv), m, i);
    }
    S->e[0] = be;
    v->s[0] = S->q[0];
    v->s[2] = S->n > 0 || be > 64;
}

/* ====================================================================== HAT === */
/* the metal: six square waves at the 808's ratios (205 304 370 523 540 800 Hz, TUNE +-12 semitones; METL: the squares,
 * noise or both), through a resonant state-variable filter (TONE: its cutoff 4 .. 16 kHz, RES), high-pass with a little of
 * its band; a sizzle of high-passed noise (SIZZ). MODE: CL / OP (lockable: a step its own), each its decay */
static const uint32_t DR_METAL_HZ[6] = {205, 304, 370, 523, 540, 800};
enum { DR_HM_CL, DR_HM_OP, DR_HM_N };
static const char *const N_DR_HMODE[DR_HM_N] = {"CL", "OP"};
enum { DR_MT_SQ, DR_MT_NOISE, DR_MT_MIX, DR_MT_N };
static const char *const N_DR_METAL[DR_MT_N] = {"6SQ", "NOIS", "MIX"};
/* the six squares' increments at TUNE tn (-64..63: +-12 semitones) and a spread (Q8 multiplier of the ratios' distance
 * from the first, 256 = as designed) on the base frequencies hz */
static void dr_metal_inc(uint32_t *inc, const uint32_t *hz, int32_t tn, int32_t note16, int32_t spread)
{
    uint32_t mul = dv_exp2((uint32_t)clamp(4096 + (clamp(tn, -64, 63) * 4096 / 64 + note16 * 4096 / 192) , 0, 3 * 4096)) >> 1;
    uint32_t k;
    for (k = 0; k < 6u; k++) {
        uint32_t h = hz[0] + (uint32_t)(((int32_t)(hz[k] - hz[0]) * spread) >> 8);
        inc[k] = ((h * 97392u) >> 8) * (mul >> 8);       /* (2^32 / 44100 = 97391.5; Q16 mul) */
    }
}
static void dhat_note_on(track_t *t, voice_t *v) { dr_start(t, v); }
static void dhat_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    dr_slot_t *S = dr_slot(t, v);
    uint32_t inc[6], i, k, mode = (uint32_t)clamp(p[P_E0], 0, DR_HM_N - 1), mt = (uint32_t)clamp(p[P_E7], 0, DR_MT_N - 1);
    uint32_t kb = mode == DR_HM_OP ? dv_kb(dr_tau(p[P_E3], 45, 40)) : dv_kb(dr_tau(p[P_E2], 18, 34));   /* CL 3.7 .. 36 ms,
                                                                                                         * OP 28 .. 470 ms */
    int32_t note = clamp(m->pitch16 - 48 * 16, -24 * 16, 24 * 16), siz = clamp(p[P_E6], 0, 127) * 200;
    int32_t drv = clamp(p[P_E8], 0, 127), a, d, x1 = S->x1, reso = clamp(p[P_E5], 0, 127);
    tsvf_t c;
    dr_metal_inc(inc, DR_METAL_HZ, p[P_E1], note, 256);
    tsvf_coef(&c, dr_cut((96 << 4) + clamp(p[P_E4], 0, 127) * (28 << 4) / 127), reso);   /* TONE: ~4 .. 16 kHz */
    if (v->s[6])
        kb = dv_kb(DR_CHOKE_US);
    dv_slow(&S->q[0], kb, &a, &d);
    for (i = 0; i < n; i++) {
        int32_t sq = 0, z = DR_NOISE(S), x, bp, lp;
        if (mt != DR_MT_NOISE)
            for (k = 0; k < 6u; k++) {
                sq += (int32_t)(S->ph[k] >> 31) ? 4096 : -4096;
                S->ph[k] += inc[k];
            }
        x = mt == DR_MT_SQ ? sq : mt == DR_MT_NOISE ? z : (sq + z) >> 1;
        {                                                 /* the SVF: high-pass = in - band * k - low */
            int32_t v3 = x - S->f[1];
            int32_t v1 = (c.a1 * S->f[0] + c.a2 * v3) >> 13;
            int32_t v2 = S->f[1] + ((c.a2 * S->f[0] + c.a3 * v3) >> 13);
            S->f[0] = clamp(2 * v1 - S->f[0], -150000, 150000);
            S->f[1] = clamp(2 * v2 - S->f[1], -150000, 150000);
            bp = v1;
            lp = v2;
        }
        x = x - lp - ((bp * (128 - reso)) >> 7) + (bp >> 1);
        x += ((((z - x1) >> 1) * siz) >> 15);              /* the sizzle */
        x1 = z;
        x = (clamp(x * 2, -65535, 65535) * (a >> 5)) >> 15;   /* (x2: the high-passed squares as loud as the other drums) */
        a += d;
        out[i] += voice_amp(dr_out(x, drv), m, i);
    }
    S->x1 = x1;
    v->s[0] = S->q[0];
}

/* ====================================================================== CYM === */
/* the metal of HAT on wider ratios (SPRD: closer .. wider; TYPE: CRASH, RIDE: lower and with a bell, CHINA: the squares
 * ring-modulated in pairs: trashy), plus a bell of two sines (BELL), noise (NOIS), high-passed at TONE; a strike (STRK,
 * 12 ms) over the ring (DCAY: 0.2 .. 3 s) */
static const uint32_t DR_CYM_HZ[3][6] = {
    {420, 593, 818, 1030, 1342, 1587},                  /* CRASH */
    {311, 452, 593, 755, 1012, 1215},                   /* RIDE */
    {367, 545, 712, 1001, 1290, 1720},                  /* CHINA */
};
enum { DR_CY_CRASH, DR_CY_RIDE, DR_CY_CHINA, DR_CY_N };
static const char *const N_DR_CTYPE[DR_CY_N] = {"CRSH", "RIDE", "CHNA"};
static void dcym_note_on(track_t *t, voice_t *v) { dr_start(t, v); }
static void dcym_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    dr_slot_t *S = dr_slot(t, v);
    uint32_t inc[6], i, ty = (uint32_t)clamp(p[P_E7], 0, DR_CY_N - 1), kb = dv_kb(dr_tau(p[P_E1], 62, 40)), ks = dv_k(12000u);
    int32_t note = clamp(m->pitch16 - 48 * 16, -24 * 16, 24 * 16), spread = 128 + clamp(p[P_E3], 0, 127) * 2;
    int32_t strk = clamp(p[P_E4], 0, 127) * 258, bell = clamp(p[P_E5], 0, 127) * 200, nz = clamp(p[P_E6], 0, 127) * 150;
    int32_t a, d, st = S->e[0];
    uint32_t binc = (uint32_t)(((DR_CYM_HZ[ty][0] * 2u * 97392u) >> 8) << 8), binc2 = binc + (binc >> 1) + (binc >> 4);
    tsvf_t c;
    dr_metal_inc(inc, DR_CYM_HZ[ty], p[P_E0], note, spread);
    tsvf_coef(&c, dr_cut((88 << 4) + clamp(p[P_E2], 0, 127) * (34 << 4) / 127 + clamp(p[P_E8], 0, 127) * 2), 20);   /* TONE */
    if (v->s[6]) {
        kb = dv_kb(DR_CHOKE_US);
        st = 0;
    }
    dv_slow(&S->q[0], kb, &a, &d);
    for (i = 0; i < n; i++) {
        int32_t x = 0, z = DR_NOISE(S), lp, bp;
        uint32_t k;
        if (ty == DR_CY_CHINA) {                          /* the pairs multiplied: a ring modulator of squares */
            for (k = 0; k < 6u; k += 2u)
                x += ((S->ph[k] ^ S->ph[k + 1u]) >> 31) ? 5000 : -5000;
        } else {
            for (k = 0; k < 6u; k++)
                x += (S->ph[k] >> 31) ? 4096 : -4096;
        }
        for (k = 0; k < 6u; k++)
            S->ph[k] += inc[k];
        x += (z * nz) >> 15;
        if (bell || ty == DR_CY_RIDE) {
            int32_t bl = ty == DR_CY_RIDE ? bell + 8000 : bell;
            x += ((sine_i(S->ph[6]) + (sine_i(S->ph[7]) >> 1)) * bl) >> 15;
            S->ph[6] += binc;
            S->ph[7] += binc2;
        }
        {
            int32_t v3 = x - S->f[1];
            int32_t v1 = (c.a1 * S->f[0] + c.a2 * v3) >> 13;
            int32_t v2 = S->f[1] + ((c.a2 * S->f[0] + c.a3 * v3) >> 13);
            S->f[0] = clamp(2 * v1 - S->f[0], -150000, 150000);
            S->f[1] = clamp(2 * v2 - S->f[1], -150000, 150000);
            bp = v1;
            lp = v2;
        }
        x = x - lp - (bp >> 2);
        x = (clamp(x, -65535, 65535) * clamp((a >> 5) + ((st * strk) >> 15), 0, 40000)) >> 15;
        st = (st * (int32_t)ks) >> 15;
        a += d;
        out[i] += voice_amp(dr_out(x, 0), m, i);
    }
    S->e[0] = st;
    v->s[0] = S->q[0];
}

#define DR_CAT_SNARE "DDDD"
#define DR_CAT_CLAP "DDD"
#define DR_CAT_HAT "DDDD"
#define DR_CAT_CYM "DDD"
static const preset_t DSNARE_PRESETS[] = {
    /* TUNE DTUN DCAY NOIS NDCY NTON SNAP DRIV */
    {"SD ANALOG", {64, 64, 50, 80, 50, 64, 60, 20}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 0)},
    {"SD TIGHT", {80, 50, 30, 90, 30, 90, 90, 30}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 0)},
    {"SD FAT", {40, 70, 80, 70, 70, 50, 40, 60}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 10)},
    {"SD NOISE", {70, 64, 20, 127, 60, 100, 100, 10}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 10)},
};
static const preset_t DCLAP_PRESETS[] = {
    /* TONE RES CNT SPRD DCAY TAIL HUMN DRIV */
    {"CP ANALOG", {64, 30, 4, 50, 50, 90, 20, 10}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 10)},
    {"CP TIGHT", {80, 40, 3, 25, 30, 70, 10, 30}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 0)},
    {"CP ROOM", {50, 20, 6, 80, 90, 100, 60, 0}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 40)},
};
static const preset_t DHAT_PRESETS[] = {
    /* MODE TUNE CDCY ODCY TONE RES SIZZ METL */
    {"HH CLOSED", {0, 0, 50, 60, 64, 30, 40, 0}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 0)},
    {"HH OPEN", {1, 0, 50, 60, 64, 30, 40, 0}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 0)},
    {"HH NOISE", {0, 0, 40, 50, 80, 20, 60, 1}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 0)},
    {"HH METAL", {0, -20, 70, 80, 40, 80, 20, 2}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 0)},
};
static const preset_t DCYM_PRESETS[] = {
    /* TUNE DCAY TONE SPRD STRK BELL NOIS TYPE */
    {"CY CRASH", {0, 70, 64, 64, 80, 0, 40, 0}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 10)},
    {"CY RIDE", {0, 90, 50, 40, 50, 60, 20, 1}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 10)},
    {"CY CHINA", {10, 60, 70, 90, 90, 0, 50, 2}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 10)},
};
#define DR_MEM .mem = sizeof(dr_slot_t) * NVOICE, .amp = dr_amp, .oneshot = 1, .poly = 2
static const engine_t ENG_DSNARE = {
    .name = "SNARE", .page_title = {"BODY", "NOISE"}, .presets = DSNARE_PRESETS, .npresets = NELEM(DSNARE_PRESETS),
    .note_on = dsnare_note_on, .render = dsnare_render, DR_MEM, .knob = {P_E0, P_E2, P_E3, P_E6},
    .edit = {{"TUNE", F_PCT, 0, 127, 64, 0, 0}, {"DTUN", F_PCT, 0, 127, 64, 0, 0}, {"DCAY", F_PCT, 0, 127, 50, 0, 0},
             {"NOIS", F_PCT, 0, 127, 80, 0, 0}, {"NDCY", F_PCT, 0, 127, 50, 0, 0}, {"NTON", F_PCT, 0, 127, 64, 0, 0},
             {"SNAP", F_PCT, 0, 127, 60, 0, 0}, {"DRIV", F_PCT, 0, 127, 20, 0, 0}},
    .e8 = {"SWEP", F_PCT, 0, 127, 20, 0, 0},
    .xyd = {{XY_E(0), XY_E(5), 0}, {XY_E(3), XY_E(6), 0}},
    .xya = {{40, 40, 0}, {40, 40, 0}},
};
static const engine_t ENG_DCLAP = {
    .name = "CLAP", .page_title = {"CLAP", "TAIL"}, .presets = DCLAP_PRESETS, .npresets = NELEM(DCLAP_PRESETS),
    .note_on = dclap_note_on, .render = dclap_render, DR_MEM, .knob = {P_E0, P_E2, P_E3, P_E4},
    .edit = {{"TONE", F_PCT, 0, 127, 64, 0, 0}, {"RES", F_PCT, 0, 127, 30, 0, 0}, {"CNT", F_INT, 1, 6, 4, 0, 0},
             {"SPRD", F_PCT, 0, 127, 50, 0, 0}, {"DCAY", F_PCT, 0, 127, 50, 0, 0}, {"TAIL", F_PCT, 0, 127, 90, 0, 0},
             {"HUMN", F_PCT, 0, 127, 20, 0, 0}, {"DRIV", F_PCT, 0, 127, 10, 0, 0}},
    .e8 = {"TUNE", F_INT, -64, 63, 0, 0, 0},
    .xyd = {{XY_E(0), XY_E(3), 0}, {XY_E(4), XY_E(5), 0}},
    .xya = {{40, 40, 0}, {40, 40, 0}},
};
static const engine_t ENG_DHAT = {
    .name = "HAT", .page_title = {"HAT", "METAL"}, .presets = DHAT_PRESETS, .npresets = NELEM(DHAT_PRESETS),
    .note_on = dhat_note_on, .render = dhat_render, DR_MEM, .knob = {P_E0, P_E2, P_E3, P_E4},
    .edit = {{"MODE", F_ENUM, 0, DR_HM_N - 1, 0, N_DR_HMODE, 0}, {"TUNE", F_INT, -64, 63, 0, 0, 0},
             {"CDCY", F_PCT, 0, 127, 50, 0, 0}, {"ODCY", F_PCT, 0, 127, 60, 0, 0}, {"TONE", F_PCT, 0, 127, 64, 0, 0},
             {"RES", F_PCT, 0, 127, 30, 0, 0}, {"SIZZ", F_PCT, 0, 127, 40, 0, 0},
             {"METL", F_ENUM, 0, DR_MT_N - 1, 0, N_DR_METAL, 0}},
    .e8 = {"DRIV", F_PCT, 0, 127, 0, 0, 0},
    .xyd = {{XY_E(4), XY_E(1), 0}, {XY_E(3), XY_E(6), 0}},
    .xya = {{40, 30, 0}, {50, 40, 0}},
};
static const engine_t ENG_DCYM = {
    .name = "CYM", .page_title = {"CYMBAL", "COLOR"}, .presets = DCYM_PRESETS, .npresets = NELEM(DCYM_PRESETS),
    .note_on = dcym_note_on, .render = dcym_render, DR_MEM, .knob = {P_E0, P_E1, P_E2, P_E4},
    .edit = {{"TUNE", F_INT, -64, 63, 0, 0, 0}, {"DCAY", F_PCT, 0, 127, 70, 0, 0}, {"TONE", F_PCT, 0, 127, 64, 0, 0},
             {"SPRD", F_PCT, 0, 127, 64, 0, 0}, {"STRK", F_PCT, 0, 127, 80, 0, 0}, {"BELL", F_PCT, 0, 127, 0, 0, 0},
             {"NOIS", F_PCT, 0, 127, 40, 0, 0}, {"TYPE", F_ENUM, 0, DR_CY_N - 1, 0, N_DR_CTYPE, 0}},
    .e8 = {"HP", F_PCT, 0, 127, 0, 0, 0},
    .xyd = {{XY_E(2), XY_E(3), 0}, {XY_E(1), XY_E(5), 0}},
    .xya = {{40, 40, 0}, {50, 40, 0}},
};
