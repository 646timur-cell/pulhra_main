/* SPDX-License-Identifier: GPL-3.0-only */
/* PULHRA's drum machines (the EDIT layer's DRUM page; FELUCCA_SWAMP builds only), each a synthesized drum of its own after
 * the analog drum machines (Elektron's Analog Rhythm, the 808 / 909 circuits): a hit is the whole sound (engine_t.oneshot:
 * the ADSR does nothing, velocity does), one voice a track that rings (a new hit chokes the old one in ~2 ms: no pile-up),
 * every value lockable as any machine's. Velocity twice (a square law, dr_amp): 64 is -12 dB; KICK's click, noise and sweep
 * and SNARE's snap follow it too. Fixed point, 32-bit products only; the slow envelopes per control block
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

enum { DR_KTY_SINE, DR_KTY_TRI, DR_KTY_ROUND, DR_KTY_SQR, DR_KTY_FM, DR_KTY_FOLD, DR_KTY_N };
static const char *const N_DR_KTYPE[DR_KTY_N] = {"SINE", "TRI", "ROUND", "SQR", "FM", "FOLD"};

#define DR_Q30 (1 << 30)
#define DR_CLICK_N 64u                                    /* the click: 64 samples (1.45 ms) */
#define DR_CHOKE_US 2000u                                 /* a choked hit's time constant */

/* a choke: the track's other hits of this engine let go fast (a new hit replaces them: the drum is one voice) */
static uint8_t dr_lane_choke;                             /* (KIT: a hit chokes only its own lane's: the same note) */
static void dr_choke_others(track_t *t, const voice_t *v)
{
    uint32_t i;
    for (i = 0; i < NVOICE; i++)
        if (&t->v[i] != v && t->v[i].active && (!dr_lane_choke || t->v[i].note == v->note))
            t->v[i].s[6] = 1;
}
/* a machine's nine values (E1 .. E8, the ninth) from a track's parameters */
static void dr_e9(const int16_t *p, int16_t *e)
{
    uint32_t k;
    for (k = 0; k < 8u; k++)
        e[k] = p[P_E0 + k];
    e[8] = p[P_E8];
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
    return v->vel * 516;                                  /* (velocity twice: the core's and this, a square law: 64 = -12 dB; x2
                                                             (+6 dB): a hit is short, it must stand up to the held machines.
                                                             Never past 65532: voice_amp's s x amp stays below 2^31) */
}

/* ===================================================================== KICK === */
/* the master's ducking (fx.c duck_prep, PULHRA's COMP page): a kick hit since the last block, the kick's peak in it */
static volatile uint8_t dr_kick_hit;
static volatile uint8_t dr_beat_hit;                       /* a beat (a quarter) of the sequencer since the last block (COMP SC 1/4) */
static int32_t dr_kick_pk;
static void dkick_start(track_t *t, voice_t *v, const int16_t *e)
{
    dr_kick_hit = 1;
    dr_choke_others(t, v);
    v->ph[0] = 0x40000000u;                               /* (the sine from its peak: the first half-cycle a thump) */
    v->ph[1] = v->ph[2] = 0;
    v->s[0] = DR_Q30;
    v->s[1] = 32767;
    v->s[2] = (int32_t)((uint32_t)e[4] * (FS / 635u));   /* HOLD: 0 .. 200 ms */
    v->s[3] = 32767;
    v->s[4] = 0;
    v->s[5] = (int32_t)DR_CLICK_N;
    v->s[6] = 0;
    if (!v->s[7])
        v->s[7] = (int32_t)(rng() | 1u);
}

static void dkick_run(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m, const int16_t *e)
{
    int32_t note = clamp(m->pitch16 - 48 * 16, -48 * 16, 48 * 16);   /* (C3: the trigs' default note) */
    int32_t base = 23 * 16 + clamp(e[0], 0, 127) * 28 * 16 / 127 + note;   /* TUNE: 26 .. 130 Hz at C3 */
    uint32_t inc0 = pitch_inc((uint32_t)clamp(base, 0, 2047));
    uint32_t top = pitch_inc((uint32_t)clamp(base + clamp(e[1], 0, 127) * 6, 0, 2047));   /* SWEEP: up to 48 st */
    uint32_t span = ((top - inc0) >> 15) * (uint32_t)(64 + v->vel / 2) / 127u, ks = dv_k(TIME_MS_X10[clamp(e[2], 0, 127) >> 1] * 100u);   /* STIME: 1 .. 96 ms */
    uint32_t kb = dv_kb(TIME_MS_X10[36 + clamp(e[3], 0, 127) * 60 / 127] * 100u);   /* DECAY: 14 ms .. 1.06 s */
    uint32_t kn = dv_k(4000u), ph = v->ph[0], cph = v->ph[1], wph = v->ph[2];
    uint32_t cinc = pitch_inc(107 * 16), winc = 0xFFFFFFFFu / DR_CLICK_N;   /* (B7 ~ 3.95 kHz; the window's one cycle) */
    int32_t vel = v->vel, drv = clamp(e[5], 0, 127), click = clamp(e[6], 0, 127) * 2 * vel, nlev = clamp(e[7], 0, 127) * 200 * vel / 127;
    /* (velocity: the click and the noise with it, the sweep half its depth at 0: a harder hit snaps more, as the circuit) */
    int32_t type = clamp(e[8], 0, DR_KTY_N - 1), g = 4096 + drv * 228, comp = 32767 - drv * 110;
    int32_t a, d, sw = v->s[1], hold = v->s[2], nl = v->s[3], lp = v->s[4], cn = v->s[5], kp = dr_kick_pk;
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
        switch (type) {
        case DR_KTY_TRI: b = osc_tri(ph); break;
        case DR_KTY_ROUND: b = (sine_i(ph) * 22000 + sine_i(ph << 1) * 8000 + sine_i(ph * 3u) * 4000) >> 15; break;
        case DR_KTY_SQR: b = softclip(sine_i(ph) * 4); break;   /* (a sine driven square: rounded corners) */
        case DR_KTY_FM:                                   /* (an octave above modulating it, the index falling with the sweep) */
            b = sine_i(ph + ((uint32_t)(sine_i(ph << 1) * ((sw >> 4) + 300)) << 4));
            break;
        case DR_KTY_FOLD:                                 /* (the sine x3 folded back: odd, metallic partials) */
            b = sine_i(ph) * 3;
            while (b > 32767 || b < -32767)
                b = b > 32767 ? 65534 - b : -65534 - b;
            break;
        default: b = sine_i(ph); break;
        }
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
        x = voice_amp(x, m, i);
        out[i] += x;
        x = x < 0 ? -x : x;
        kp = x > kp ? x : kp;
    }
    dr_kick_pk = kp;
    v->ph[0] = ph;
    v->ph[1] = cph;
    v->ph[2] = wph;
    v->s[1] = sw;
    v->s[2] = hold;
    v->s[3] = nl;
    v->s[4] = lp;
    v->s[5] = cn;
}

/* the machines on a track of their own: their values the track's */
#define DR_WRAP(nm) \
    static void nm##_note_on(track_t *t, voice_t *v) { int16_t e[9]; dr_e9(t->p, e); dr_lane_choke = 0; nm##_start(t, v, e); } \
    static void nm##_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m) \
    { int16_t e[9]; dr_e9(t->p, e); nm##_run(t, v, out, n, m, e); }
DR_WRAP(dkick)

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
    int32_t le;                  /* KIT: the lane's envelope (dkit_env), Q24 */
    uint16_t lg;                 /* .. its gain at the last block's end, Q15 */
    uint8_t lst;                 /* .. its stage: 1 attack, 2 decay, 3 sustain, 4 release, 5 done (the hit ends) */
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
static void dsnare_start(track_t *t, voice_t *v, const int16_t *e)
{
    (void)e;
    dr_slot_t *S;
    dr_start(t, v);
    S = dr_slot(t, v);
    S->ph[0] = S->ph[1] = 0x40000000u;
}
static void dsnare_run(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m, const int16_t *e)
{
    dr_slot_t *S = dr_slot(t, v);
    int32_t note = clamp(m->pitch16 - 48 * 16, -48 * 16, 48 * 16);
    int32_t b16 = (47 << 4) + clamp(e[0], 0, 127) * (20 << 4) / 127 + note;   /* TUNE: 123 .. 392 Hz at C3 */
    uint32_t inc1 = pitch_inc((uint32_t)clamp(b16, 0, 2047));
    uint32_t inc2 = pitch_inc((uint32_t)clamp(b16 + clamp(e[1], 0, 127) * (12 << 4) / 127, 0, 2047));
    uint32_t kb = dv_kb(dr_tau(e[2], 30, 40)), kn = dv_kb(dr_tau(e[4], 32, 45));
    uint32_t ksn = dv_k(2500u), ksw = dv_k(15000u);
    int32_t nlev = clamp(e[3], 0, 127) * 258, snap = clamp(e[6], 0, 127) * 2 * v->vel, drv = clamp(e[7], 0, 127);   /* (SNAP with the velocity) */
    int32_t swd = clamp(e[8], 0, 127);                 /* SWEEP: up to an octave above, gliding down */
    int32_t ab, db, an, dn, sn = S->e[0], sw = S->e[1], x1 = S->x1;
    tsvf_t c;
    uint32_t i;
    tsvf_coef(&c, dr_cut((83 << 4) + clamp(e[5], 0, 127) * (24 << 4) / 127), 50);   /* NTON: 1 .. 4 kHz .. 8 kHz */
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
static void dclap_start(track_t *t, voice_t *v, const int16_t *e)
{
    dr_slot_t *S;
    dr_start(t, v);
    S = dr_slot(t, v);
    S->n = clamp(e[2], 1, 6) - 1;                   /* (the bursts after the first) */
    S->cnt = 0;
    v->s[2] = 1;
}
static void dclap_run(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m, const int16_t *e)
{
    dr_slot_t *S = dr_slot(t, v);
    int32_t note = clamp(m->pitch16 - 48 * 16, -24 * 16, 24 * 16) + clamp(e[8], -64, 63) * (12 << 4) / 64;
    int32_t gap = (int32_t)(FS / 250u) + clamp(e[3], 0, 127) * (int32_t)(FS / 63u) / 127;   /* SPRD: 4 .. 20 ms */
    int32_t hum = clamp(e[6], 0, 127), tail = clamp(e[5], 0, 127) * 258, drv = clamp(e[7], 0, 127);
    uint32_t kb = dv_kb(dr_tau(e[4], 40, 45)), kf = dv_k(3500u);
    int32_t at, dt, be = S->e[0];
    tsvf_t c;
    uint32_t i;
    tsvf_coef(&c, dr_cut((75 << 4) + clamp(e[0], 0, 127) * (20 << 4) / 127 + note), clamp(e[1], 0, 127));
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
static void dhat_start(track_t *t, voice_t *v, const int16_t *e) { (void)e; dr_start(t, v); }
static void dhat_run(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m, const int16_t *e)
{
    dr_slot_t *S = dr_slot(t, v);
    uint32_t inc[6], i, k, mode = (uint32_t)clamp(e[0], 0, DR_HM_N - 1), mt = (uint32_t)clamp(e[7], 0, DR_MT_N - 1);
    uint32_t kb = mode == DR_HM_OP ? dv_kb(dr_tau(e[3], 45, 40)) : dv_kb(dr_tau(e[2], 18, 34));   /* CL 3.7 .. 36 ms,
                                                                                                         * OP 28 .. 470 ms */
    int32_t note = clamp(m->pitch16 - 48 * 16, -24 * 16, 24 * 16), siz = clamp(e[6], 0, 127) * 200;
    int32_t drv = clamp(e[8], 0, 127), a, d, x1 = S->x1, reso = clamp(e[5], 0, 127);
    tsvf_t c;
    dr_metal_inc(inc, DR_METAL_HZ, e[1], note, 256);
    tsvf_coef(&c, dr_cut((96 << 4) + clamp(e[4], 0, 127) * (28 << 4) / 127), reso);   /* TONE: ~4 .. 16 kHz */
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
/* after the Analog Rhythm's CY (an 808's cymbal under it): the metal of HAT on wider ratios (SPRD: closer .. wider; TYPE:
 * CRASH, RIDE: lower and with a bell, CHINA: the squares ring-modulated in pairs: trashy), plus a bell of two sines (BELL)
 * and noise (NOIS), split in three bands each its own decay: the low (a band-pass ~3.4 kHz) short, the middle (~6.8 kHz)
 * half the ring, the high (a high-pass above it) the whole ring (DCAY: 0.2 .. 3 s): the strike's clang dies into the
 * sizzle. TONE tilts low .. high, the ninth HP moves the upper split up and takes the low away; a strike (STRK, 12 ms) */
static const uint32_t DR_CYM_HZ[3][6] = {
    {420, 593, 818, 1030, 1342, 1587},                  /* CRASH */
    {311, 452, 593, 755, 1012, 1215},                   /* RIDE */
    {367, 545, 712, 1001, 1290, 1720},                  /* CHINA */
};
enum { DR_CY_CRASH, DR_CY_RIDE, DR_CY_CHINA, DR_CY_N };
static const char *const N_DR_CTYPE[DR_CY_N] = {"CRSH", "RIDE", "CHNA"};
static void dcym_start(track_t *t, voice_t *v, const int16_t *e)
{
    (void)e;
    dr_start(t, v);
    dr_slot(t, v)->cnt = 32767;                           /* (the strike) */
}
static void dcym_run(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m, const int16_t *e)
{
    dr_slot_t *S = dr_slot(t, v);
    uint32_t inc[6], i, ty = (uint32_t)clamp(e[7], 0, DR_CY_N - 1), tau = dr_tau(e[1], 62, 40), kb = dv_kb(tau);
    uint32_t klo = dv_k(tau / 6u + 1u), kmid = dv_k(tau / 2u + 1u), ks = dv_k(12000u);
    int32_t note = clamp(m->pitch16 - 48 * 16, -24 * 16, 24 * 16), spread = 128 + clamp(e[3], 0, 127) * 2;
    int32_t tone = clamp(e[2], 0, 127), hp = clamp(e[8], 0, 127), strk = clamp(e[4], 0, 127) * 258;
    int32_t bell = clamp(e[5], 0, 127) * 200, nz = clamp(e[6], 0, 127) * 150;
    int32_t wl = ((127 - tone) * 300 * (127 - hp)) / 127, wm = 20000, wh = 12000 + tone * 160;
    int32_t a, d, st = S->cnt, el = S->e[0], em = S->e[1];
    uint32_t binc = (uint32_t)(((DR_CYM_HZ[ty][0] * 2u * 97392u) >> 8) << 8), binc2 = binc + (binc >> 1) + (binc >> 4);
    tsvf_t cl, cm;
    dr_metal_inc(inc, DR_CYM_HZ[ty], e[0], note, spread);
    tsvf_coef(&cl, dr_cut(1674 + note / 2), 50);                   /* the low band: ~3.4 kHz, Q ~1.4 */
    tsvf_coef(&cm, dr_cut(1866 + note / 2 + hp * 2), 40);          /* the split: ~6.8 kHz (HP: up to ~11 kHz) */
    if (v->s[6]) {
        kb = dv_kb(DR_CHOKE_US);
        st = el = em = 0;
    }
    dv_slow(&S->q[0], kb, &a, &d);
    for (i = 0; i < n; i++) {
        int32_t x = 0, z = DR_NOISE(S), lo, bp, lpm, hi, y;
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
        {                                                 /* the low band */
            int32_t v3 = x - S->f[1];
            int32_t v1 = (cl.a1 * S->f[0] + cl.a2 * v3) >> 13;
            int32_t v2 = S->f[1] + ((cl.a2 * S->f[0] + cl.a3 * v3) >> 13);
            S->f[0] = clamp(2 * v1 - S->f[0], -150000, 150000);
            S->f[1] = clamp(2 * v2 - S->f[1], -150000, 150000);
            lo = v1;
        }
        {                                                 /* the split: its band the middle, its high-pass the high */
            int32_t v3 = x - S->f[3];
            int32_t v1 = (cm.a1 * S->f[2] + cm.a2 * v3) >> 13;
            int32_t v2 = S->f[3] + ((cm.a2 * S->f[2] + cm.a3 * v3) >> 13);
            S->f[2] = clamp(2 * v1 - S->f[2], -150000, 150000);
            S->f[3] = clamp(2 * v2 - S->f[3], -150000, 150000);
            bp = v1;
            lpm = v2;
        }
        hi = x - lpm - ((bp * 6700) >> 12);
        y = ((((clamp(lo, -65535, 65535) * wl) >> 15) * el) >> 15) + ((((clamp(bp, -65535, 65535) * wm) >> 15) * em) >> 15)
            + (((clamp(hi, -65535, 65535) * wh) >> 15) * (a >> 5) >> 15);
        y = (clamp(y * 2, -65535, 65535) * clamp(32767 + ((st * strk) >> 15), 0, 65535)) >> 15;
        el = (el * (int32_t)klo) >> 15;
        em = (em * (int32_t)kmid) >> 15;
        st = (st * (int32_t)ks) >> 15;
        a += d;
        out[i] += voice_amp(dr_out(y, 0), m, i);
    }
    S->cnt = st;
    S->e[0] = el;
    S->e[1] = em;
    v->s[0] = S->q[0];
}

DR_WRAP(dsnare)
DR_WRAP(dclap)
DR_WRAP(dhat)
DR_WRAP(dcym)

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

/* ==================================================================== CONGA === */
/* the hand drums (Tim, 26.10: PERC split; RIM and the cowbell machines of their own): MODE CONGA, BONGO or TOM, each a sine
 * shell swept down BEND (BTIM, the ninth, its time) with its partial at TONE's level, skin noise (NOIS), the slap's spike
 * (SLAP, with the velocity), DRIV. STRK the stroke (lockable: a pattern of them), as a player's: OPEN (the shell rings),
 * MUTE (the palm on it: short, dull), SLAP (the crack: a snap of noise, the shell a breath), BASS (the heel: an octave
 * down, a thud). TUNE moves each mode inside its range, DCAY its decay */
enum { DR_PM_CONGA, DR_PM_BONGO, DR_PM_TOM, DR_PM_N };
static const char *const N_DR_PMODE[DR_PM_N] = {"CONGA", "BONGO", "TOM"};
static const uint8_t DR_PM_LO[DR_PM_N] = {50, 62, 36};         /* TUNE's range, MIDI notes: lo .. lo + span */
static const uint8_t DR_PM_SPAN[DR_PM_N] = {24, 22, 36};
static const uint16_t DR_PM_IV[DR_PM_N] = {130, 150, 112};     /* the second partial above, 1/16 semitone */
static const uint8_t DR_PM_DLO[DR_PM_N] = {26, 20, 34};        /* DCAY: dr_tau's place lo, span */
static const uint8_t DR_PM_DSP[DR_PM_N] = {44, 40, 50};
enum { DR_ST_OPEN, DR_ST_MUTE, DR_ST_SLAP, DR_ST_BASS, DR_ST_N };
static const char *const N_DR_STRK[DR_ST_N] = {"OPEN", "MUTE", "SLAP", "BASS"};
static const struct {
    int8_t dlo, oct;                                     /* DCAY's place moved, the pitch (1/16 semitone x 16) */
    uint8_t part, nois, slap, crack;                     /* the partial's level (x/8), the skin noise (x/8), the spike (x/8), a
                                                          * snap of high noise (0..127) */
} DR_STRK[DR_ST_N] = {
    {0, 0, 8, 8, 8, 0},                                  /* OPEN */
    {-14, 0, 3, 5, 6, 0},                                /* MUTE */
    {-18, 2, 10, 14, 14, 110},                           /* SLAP */
    {-6, -12, 2, 3, 3, 0},                               /* BASS */
};
static void dperc_start(track_t *t, voice_t *v, const int16_t *e)
{
    dr_slot_t *S;
    (void)e;
    dr_start(t, v);
    S = dr_slot(t, v);
    S->ph[0] = S->ph[1] = 0x40000000u;
}
static void dperc_run(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m, const int16_t *e)
{
    dr_slot_t *S = dr_slot(t, v);
    uint32_t md = (uint32_t)clamp(e[0], 0, DR_PM_N - 1), st = (uint32_t)clamp(e[4], 0, DR_ST_N - 1);
    int32_t note = clamp(m->pitch16 - 48 * 16, -48 * 16, 48 * 16);
    int32_t b16 = (DR_PM_LO[md] << 4) + clamp(e[1], 0, 127) * (DR_PM_SPAN[md] << 4) / 127 + note + DR_STRK[st].oct * 16;
    uint32_t inc1 = pitch_inc((uint32_t)clamp(b16, 0, 2047)), inc2 = pitch_inc((uint32_t)clamp(b16 + DR_PM_IV[md], 0, 2047));
    uint32_t kb = dv_kb(dr_tau(e[2], (uint32_t)clamp(DR_PM_DLO[md] + DR_STRK[st].dlo, 2, 100), DR_PM_DSP[md]));
    uint32_t kn = dv_k(st == DR_ST_SLAP ? 9000u : 18000u), ksn = dv_k(1800u), kcr = dv_k(6000u), ksw = dv_k(dr_tau(e[8], 12, 50));
    int32_t bend = clamp(e[3], 0, 127), tone = clamp(e[5], 0, 127), nlev = clamp(e[6], 0, 127) * 25 * DR_STRK[st].nois;
    int32_t slap = clamp(e[6], 0, 127) * v->vel * DR_STRK[st].slap / 4, crack = DR_STRK[st].crack * 258, drv = clamp(e[7], 0, 127);
    int32_t lev2 = tone * 25 * DR_STRK[st].part, ab, db, nz = S->e[0], sw = S->e[1];
    int32_t sn = S->cnt ? S->cnt : (S->n ? 0 : 32767), x1 = S->x1, cr = S->n ? (int32_t)S->ph[2] : 32767;
    if (!DR_STRK[st].crack)
        cr = 0;
    tsvf_t cn, ch;
    uint32_t i;
    S->n = 1;                                             /* (the slap started: once a hit) */
    tsvf_coef(&cn, dr_cut(b16 + (19 << 4)), 40);          /* the skin noise around two octaves and a half up */
    tsvf_coef(&ch, dr_cut(1840), 30);                     /* SLAP's crack: ~6 kHz */
    if (v->s[6]) {
        kb = dv_kb(DR_CHOKE_US);
        nz = sn = cr = 0;
    }
    dv_slow(&S->q[0], kb, &ab, &db);
    for (i = 0; i < n; i++) {
        int32_t z = DR_NOISE(S), x;
        uint32_t ext = (uint32_t)((sw * bend) >> 7);
        S->ph[0] += inc1 + (uint32_t)((inc1 >> 15) * ext);
        S->ph[1] += inc2 + (uint32_t)((inc2 >> 15) * ext);
        sw = (sw * (int32_t)ksw) >> 15;
        x = sine_i(S->ph[0]) + ((sine_i(S->ph[1]) * clamp(lev2, 0, 32767)) >> 15);
        x = (((x * 3) >> 2) * (ab >> 5)) >> 15;
        ab += db;
        if (nz > 64) {
            x += (((clamp(dv_bp(&cn, z, &S->f[0]) * 2, -32767, 32767) * nz) >> 15) * clamp(nlev, 0, 32767)) >> 15;
            nz = (nz * (int32_t)kn) >> 15;
        }
        if (sn > 64) {
            x += ((((z - x1) >> 1) * sn >> 15) * clamp(slap, 0, 65535)) >> 15;
            sn = (sn * (int32_t)ksn) >> 15;
        }
        if (crack && cr > 64) {
            x += (((clamp(dv_bp(&ch, z, &S->f[2]) * 2, -32767, 32767) * cr) >> 15) * crack) >> 15;
            cr = (cr * (int32_t)kcr) >> 15;
        }
        x1 = z;
        out[i] += voice_amp(dr_out(x, drv), m, i);
    }
    S->e[0] = nz;
    S->e[1] = sw;
    S->cnt = sn;
    S->x1 = x1;
    S->ph[2] = (uint32_t)cr;
    v->s[0] = S->q[0];
    v->s[2] = cr > 64;
}
DR_WRAP(dperc)
#define DR_CAT_PERC "DDDDDD"
static const preset_t DPERC_PRESETS[] = {
    /* MODE TUNE DCAY BEND STRK TONE NOIS/SLAP DRIV */
    {"CG OPEN", {DR_PM_CONGA, 50, 60, 20, DR_ST_OPEN, 60, 50, 0}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 10)},
    {"CG SLAP", {DR_PM_CONGA, 60, 60, 20, DR_ST_SLAP, 60, 70, 10}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 10)},
    {"BG HI", {DR_PM_BONGO, 80, 50, 25, DR_ST_OPEN, 60, 60, 0}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 10)},
    {"BG MUTE", {DR_PM_BONGO, 50, 50, 20, DR_ST_MUTE, 50, 60, 0}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 10)},
    {"TOM LO", {DR_PM_TOM, 30, 70, 50, DR_ST_OPEN, 40, 40, 10}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 10)},
    {"TOM HI", {DR_PM_TOM, 80, 55, 45, DR_ST_OPEN, 40, 50, 10}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 10)},
};
static const engine_t ENG_DPERC = {
    .name = "CONGA", .page_title = {"SHELL", "STROKE"}, .presets = DPERC_PRESETS, .npresets = NELEM(DPERC_PRESETS),
    .note_on = dperc_note_on, .render = dperc_render, DR_MEM, .knob = {P_E0, P_E1, P_E2, P_E4},
    .edit = {{"MODE", F_ENUM, 0, DR_PM_N - 1, 0, N_DR_PMODE, 0}, {"TUNE", F_PCT, 0, 127, 64, 0, 0},
             {"DCAY", F_PCT, 0, 127, 50, 0, 0}, {"BEND", F_PCT, 0, 127, 20, 0, 0},
             {"STRK", F_ENUM, 0, DR_ST_N - 1, 0, N_DR_STRK, 0}, {"TONE", F_PCT, 0, 127, 50, 0, 0},
             {"SLAP", F_PCT, 0, 127, 50, 0, 0}, {"DRIV", F_PCT, 0, 127, 0, 0, 0}},
    .e8 = {"BTIM", F_PCT, 0, 127, 40, 0, 0},
    .xyd = {{XY_E(1), XY_E(3), 0}, {XY_E(2), XY_E(6), 0}},
    .xya = {{40, 40, 0}, {40, 40, 0}},
};

/* ===================================================================== BELL === */
/* the cowbell, after the Analog Rhythm's CB (and the 808's under it): two square oscillators (TUNE: 540 Hz +-12 semitones;
 * DTUN the second's interval, 0 .. 12 semitones, 6.8 the 808's 540 : 800), BAL between them, through a resonant band-pass
 * (TONE 830 Hz .. 6.6 kHz, RES); the amplitude in two stages: a short loud one (PNCH: how much of the hit, ~6 ms) over the
 * ring (DCAY 9 ms .. 0.7 s); DRIV the clip; the ninth, SWEP, a pitch drop at the strike */
static void dbell_start(track_t *t, voice_t *v, const int16_t *e) { (void)e; dr_start(t, v); }
static void dbell_run(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m, const int16_t *e)
{
    dr_slot_t *S = dr_slot(t, v);
    int32_t note = clamp(m->pitch16 - 48 * 16, -24 * 16, 24 * 16);
    int32_t b16 = 1161 + clamp(e[0], -64, 63) * 192 / 64 + note, i16 = clamp(e[3], 0, 127) * 192 / 127;
    uint32_t inc1 = pitch_inc((uint32_t)clamp(b16, 0, 2047)), inc2 = pitch_inc((uint32_t)clamp(b16 + i16, 0, 2047));
    uint32_t kb = dv_kb(dr_tau(e[1], 30, 60)), kp = dv_k(6000u), ksw = dv_k(9000u), i;
    int32_t pn = clamp(e[4], 0, 127), tl = 32767 - pn * 150, bal = clamp(e[6], 0, 127), drv = clamp(e[7], 0, 127);
    int32_t bend = clamp(e[8], 0, 127) / 4, ab, db, fp = S->e[0], sw = S->e[1];
    tsvf_t c;
    tsvf_coef(&c, dr_cut((80 << 4) + clamp(e[2], 0, 127) * (32 << 4) / 127), 30 + clamp(e[5], 0, 127) * 90 / 127);
    if (v->s[6]) {
        kb = dv_kb(DR_CHOKE_US);
        fp = 0;
    }
    dv_slow(&S->q[0], kb, &ab, &db);
    for (i = 0; i < n; i++) {
        uint32_t ext = (uint32_t)((sw * bend) >> 7);
        int32_t o1 = (S->ph[0] >> 31) ? -16000 : 16000, o2 = (S->ph[1] >> 31) ? -16000 : 16000, x, g;
        S->ph[0] += inc1 + (uint32_t)((inc1 >> 15) * ext);
        S->ph[1] += inc2 + (uint32_t)((inc2 >> 15) * ext);
        sw = (sw * (int32_t)ksw) >> 15;
        x = (o1 * (127 - bal) + o2 * bal) >> 6;
        x = clamp(dv_bp(&c, x, &S->f[0]), -150000, 150000);
        S->f[0] = clamp(S->f[0], -150000, 150000);
        S->f[1] = clamp(S->f[1], -150000, 150000);
        g = (((ab >> 5) * tl) >> 15) + ((fp * pn * 258) >> 15);
        x = (clamp(x * 3 / 2, -65535, 65535) * clamp(g, 0, 32767)) >> 15;
        fp = (fp * (int32_t)kp) >> 15;
        ab += db;
        out[i] += voice_amp(dr_out(x, drv), m, i);
    }
    S->e[0] = fp;
    S->e[1] = sw;
    v->s[0] = S->q[0];
}

/* ====================================================================== RIM === */
/* the rimshot, after the Analog Rhythm's RS: two resonators struck (TUNE: 455 Hz +-12 semitones; DTUN the second's
 * interval, 12 .. 36 semitones, 22.5 the 808's 455 : 1667, ringing half as long), BAL between them, a click of noise on the
 * strike (CLIK, 1.5 ms), a noise rattle under the ring (NOIS), all high-passed at TONE (80 Hz .. 2.6 kHz) and driven (DRIV:
 * the 808's is clipped); DCAY 3 .. 110 ms; the ninth, SWEP, a pitch drop at the strike */
static void drim_start(track_t *t, voice_t *v, const int16_t *e) { (void)e; dr_start(t, v); }
static void drim_run(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m, const int16_t *e)
{
    dr_slot_t *S = dr_slot(t, v);
    int32_t note = clamp(m->pitch16 - 48 * 16, -24 * 16, 24 * 16);
    int32_t b16 = 1117 + clamp(e[0], -64, 63) * 192 / 64 + note, i16 = 192 + clamp(e[3], 0, 127) * 384 / 127;
    uint32_t inc1 = pitch_inc((uint32_t)clamp(b16, 0, 2047)), inc2 = pitch_inc((uint32_t)clamp(b16 + i16, 0, 2047));
    uint32_t tau = dr_tau(e[1], 14, 40), kb = dv_kb(tau), kb2 = dv_kb(tau / 2u + 1u), kc = dv_k(1500u), ksw = dv_k(6000u), i;
    int32_t bal = clamp(e[5], 0, 127), clk = clamp(e[4], 0, 127) * 2 * (64 + v->vel / 2) / 127, nl = clamp(e[6], 0, 127) * 120;
    int32_t drv = clamp(e[7], 0, 127), bend = clamp(e[8], 0, 127) / 4, a1, d1, a2, d2, cs = S->e[0], sw = S->e[1], x1 = S->x1;
    tsvf_t c;
    if (!S->n) {                                          /* (the strike: once a hit) */
        S->n = 1;
        S->ph[0] = S->ph[1] = 0x40000000u;                /* (from the peak: the struck resonator's edge) */
    }
    tsvf_coef(&c, dr_cut((40 << 4) + clamp(e[2], 0, 127) * (60 << 4) / 127), 20);
    if (v->s[6]) {
        kb = kb2 = dv_kb(DR_CHOKE_US);
        cs = 0;
    }
    dv_slow(&S->q[0], kb, &a1, &d1);
    dv_slow(&S->q[1], kb2, &a2, &d2);
    for (i = 0; i < n; i++) {
        uint32_t ext = (uint32_t)((sw * bend) >> 7);
        int32_t z = DR_NOISE(S), x, hp;
        S->ph[0] += inc1 + (uint32_t)((inc1 >> 15) * ext);
        S->ph[1] += inc2 + (uint32_t)((inc2 >> 15) * ext);
        sw = (sw * (int32_t)ksw) >> 15;
        x = ((((sine_i(S->ph[0]) * (127 - bal)) >> 7) * (a1 >> 5)) >> 15) + ((((sine_i(S->ph[1]) * bal) >> 7) * (a2 >> 5)) >> 15);
        x += (((z * nl) >> 15) * (a2 >> 5)) >> 15;
        if (cs > 64) {
            x += ((((z - x1) >> 1) * cs >> 15) * clk) >> 8;
            cs = (cs * (int32_t)kc) >> 15;
        }
        x1 = z;
        {
            int32_t v3 = x * 2 - S->f[1];
            int32_t v1 = (c.a1 * S->f[0] + c.a2 * v3) >> 13;
            int32_t v2 = S->f[1] + ((c.a2 * S->f[0] + c.a3 * v3) >> 13);
            S->f[0] = clamp(2 * v1 - S->f[0], -150000, 150000);
            S->f[1] = clamp(2 * v2 - S->f[1], -150000, 150000);
            hp = x * 2 - v2 - ((v1 * 7000) >> 12);
        }
        a1 += d1;
        a2 += d2;
        out[i] += voice_amp(dr_out(hp, drv + 20), m, i);
    }
    S->e[0] = cs;
    S->e[1] = sw;
    S->x1 = x1;
    v->s[0] = S->q[0];
}
DR_WRAP(dbell)
DR_WRAP(drim)
#define DR_CAT_BELL "DDDD"
#define DR_CAT_RIM "DDDD"
static const preset_t DBELL_PRESETS[] = {
    /* TUNE DCAY TONE DTUN PNCH RES BAL DRIV */
    {"CB 808", {0, 60, 79, 72, 70, 50, 64, 10}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 10)},
    {"CB SHORT", {6, 30, 85, 72, 100, 70, 64, 30}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 0)},
    {"CB LOW", {-30, 70, 60, 60, 50, 40, 50, 20}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 10)},
    {"CB AGOGO", {20, 50, 95, 100, 40, 90, 80, 0}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 20)},
};
static const preset_t DRIM_PRESETS[] = {
    /* TUNE DCAY TONE DTUN CLIK BAL NOIS DRIV */
    {"RS 808", {0, 40, 60, 56, 60, 70, 10, 40}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 10)},
    {"RS CLICK", {10, 20, 90, 70, 100, 90, 0, 20}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 0)},
    {"RS WOOD", {-20, 50, 30, 40, 30, 30, 0, 10}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 10)},
    {"RS NOISE", {0, 35, 70, 56, 80, 60, 70, 50}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 10)},
};
static const engine_t ENG_DBELL = {
    .name = "BELL", .page_title = {"BELL", "COLOR"}, .presets = DBELL_PRESETS, .npresets = NELEM(DBELL_PRESETS),
    .note_on = dbell_note_on, .render = dbell_render, DR_MEM, .knob = {P_E0, P_E1, P_E2, P_E3},
    .edit = {{"TUNE", F_INT, -64, 63, 0, 0, 0}, {"DCAY", F_PCT, 0, 127, 60, 0, 0}, {"TONE", F_PCT, 0, 127, 79, 0, 0},
             {"DTUN", F_PCT, 0, 127, 72, 0, 0}, {"PNCH", F_PCT, 0, 127, 70, 0, 0}, {"RES", F_PCT, 0, 127, 50, 0, 0},
             {"BAL", F_PCT, 0, 127, 64, 0, 0}, {"DRIV", F_PCT, 0, 127, 10, 0, 0}},
    .e8 = {"SWEP", F_PCT, 0, 127, 0, 0, 0},
    .xyd = {{XY_E(2), XY_E(3), 0}, {XY_E(1), XY_E(4), 0}},
    .xya = {{40, 40, 0}, {50, 40, 0}},
};
static const engine_t ENG_DRIM = {
    .name = "RIM", .page_title = {"RIM", "CLICK"}, .presets = DRIM_PRESETS, .npresets = NELEM(DRIM_PRESETS),
    .note_on = drim_note_on, .render = drim_render, DR_MEM, .knob = {P_E0, P_E1, P_E2, P_E4},
    .edit = {{"TUNE", F_INT, -64, 63, 0, 0, 0}, {"DCAY", F_PCT, 0, 127, 40, 0, 0}, {"TONE", F_PCT, 0, 127, 60, 0, 0},
             {"DTUN", F_PCT, 0, 127, 56, 0, 0}, {"CLIK", F_PCT, 0, 127, 60, 0, 0}, {"BAL", F_PCT, 0, 127, 70, 0, 0},
             {"NOIS", F_PCT, 0, 127, 10, 0, 0}, {"DRIV", F_PCT, 0, 127, 40, 0, 0}},
    .e8 = {"SWEP", F_PCT, 0, 127, 10, 0, 0},
    .xyd = {{XY_E(0), XY_E(2), 0}, {XY_E(1), XY_E(4), 0}},
    .xya = {{40, 40, 0}, {40, 40, 0}},
};

/* =================================================================== SHAKER === */
/* white noise (Tim: a shaker on white noise) through a state-variable filter at TONE (1.5 .. 12 kHz, RES), its band or its
 * high-pass (WIDE: narrow .. open); GRN: the noise cut into beads (0 a smooth hiss, up: fewer, harder grains); ATK the
 * swoosh in (0 .. 40 ms), DCAY its fall (10 .. 150 ms), BITE a click at the strike, DRIV; the ninth, DBL: the shake back,
 * a second stroke DBL later (Tim, 31.10: the knob its time: OFF, 1 ms (a flam) .. ~680 ms, as the times of the envelopes) */
static const char *const N_DR_DBL[2] = {"OFF", 0};
static int32_t dr_dbl_gap(int32_t v) { return (int32_t)(TIME_MS_X10[clamp(v, 0, 127)] * 441u / 100u); }   /* samples */
static void dshk_start(track_t *t, voice_t *v, const int16_t *e)
{
    dr_slot_t *S;
    dr_start(t, v);
    S = dr_slot(t, v);
    S->e[0] = 0;                                          /* (the swoosh from silence) */
    S->e[1] = 0;
    S->q[1] = 32767;                                      /* (the click) */
    v->s[2] = clamp(e[8], 0, 127) > 0;                    /* (DBL: alive till the second stroke) */
}
static void dshk_run(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m, const int16_t *e)
{
    dr_slot_t *S = dr_slot(t, v);
    int32_t note = clamp(m->pitch16 - 48 * 16, -24 * 16, 24 * 16), grn = clamp(e[3], 0, 127), gm = grn * 258;
    int32_t wide = clamp(e[5], 0, 127), bite = clamp(e[6], 0, 127) * 2 * (64 + v->vel / 2) / 127, drv = clamp(e[7], 0, 127);
    int32_t dbl = clamp(e[8], 0, 127), rise = 32767 / (1 + clamp(e[2], 0, 127) * 14), a, d, at = S->e[0], g = S->e[1];
    int32_t ck = S->q[1], x1 = S->x1, gc = 32767 + grn * 600;   /* (the beads' level made up) */
    uint32_t kb = dv_kb(dr_tau(e[1], 28, 42)), kg = dv_k(400u), kc = dv_k(1200u), i;
    uint32_t thr = grn ? (dv_exp2((uint32_t)(4096 * 12 - grn * 4096 * 5 / 127)) >> 16) * 97391u : 0u;   /* (beads a second:
                                                          ~4000 at 1 .. ~130 at 127; x 2^32 / fs) */
    tsvf_t c;
    tsvf_coef(&c, dr_cut((90 << 4) + clamp(e[0], 0, 127) * (37 << 4) / 127 + note), clamp(e[4], 0, 127) * 100 / 127);
    if (dbl && !S->n && S->cnt >= dr_dbl_gap(dbl)) {      /* DBL: the shake back */
        S->n = 1;
        S->q[0] = DR_Q30 / 4 * 3;
        at = 0;
        v->s[2] = 0;
    }
    if (v->s[6]) {
        kb = dv_kb(DR_CHOKE_US);
        ck = 0;
        S->n = 1;
        v->s[2] = 0;
    }
    dv_slow(&S->q[0], kb, &a, &d);
    for (i = 0; i < n; i++) {
        int32_t z = DR_NOISE(S), x, bp, lp;
        if (gm) {                                         /* GRN: beads gate the noise */
            if ((uint32_t)noise32(&S->rng) < thr)
                g = 32767;
            x = (z * (32767 - gm + ((g * gm) >> 15))) >> 15;
            x = (x * (gc >> 2)) >> 13;
            g = (g * (int32_t)kg) >> 15;
        } else {
            x = z;
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
        x = (((bp * 2) * (127 - wide)) >> 7) + (((x - lp - bp) * wide) >> 7);
        if (ck > 64) {
            x += ((((z - x1) >> 1) * ck >> 15) * bite) >> 7;
            ck = (ck * (int32_t)kc) >> 15;
        }
        x1 = z;
        x = (clamp(x, -65535, 65535) * (((a >> 5) * at) >> 15)) >> 15;
        at = at + rise > 32767 ? 32767 : at + rise;
        a += d;
        out[i] += voice_amp(dr_out(x, drv), m, i);
    }
    S->cnt = S->cnt + (int32_t)n < (1 << 30) ? S->cnt + (int32_t)n : S->cnt;
    S->e[0] = at;
    S->e[1] = g;
    S->q[1] = ck;
    S->x1 = x1;
    v->s[0] = S->q[0];
}
DR_WRAP(dshk)
#define DR_CAT_SHKR "DDDD"
static const preset_t DSHK_PRESETS[] = {
    /* TONE DCAY ATK GRN RES WIDE BITE DRIV */
    {"SH SHAKER", {70, 40, 30, 20, 20, 60, 20, 0}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 10)},
    {"SH CABASA", {95, 30, 8, 50, 30, 90, 50, 10}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 10)},
    {"SH MARACA", {60, 25, 15, 75, 40, 40, 40, 0}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 10)},
    {"SH EGG", {80, 55, 55, 10, 10, 70, 10, 0}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 20)},
};
static const engine_t ENG_DSHK = {
    .name = "SHKR", .page_title = {"SHAKE", "GRAIN"}, .presets = DSHK_PRESETS, .npresets = NELEM(DSHK_PRESETS),
    .note_on = dshk_note_on, .render = dshk_render, DR_MEM, .knob = {P_E0, P_E1, P_E2, P_E3},
    .edit = {{"TONE", F_PCT, 0, 127, 70, 0, 0}, {"DCAY", F_PCT, 0, 127, 40, 0, 0}, {"ATK", F_PCT, 0, 127, 30, 0, 0},
             {"GRN", F_PCT, 0, 127, 20, 0, 0}, {"RES", F_PCT, 0, 127, 20, 0, 0}, {"WIDE", F_PCT, 0, 127, 60, 0, 0},
             {"BITE", F_PCT, 0, 127, 20, 0, 0}, {"DRIV", F_PCT, 0, 127, 0, 0, 0}},
    .e8 = {"DBL", F_TIME, 0, 90, 0, N_DR_DBL, 0},
    .xyd = {{XY_E(0), XY_E(3), 0}, {XY_E(1), XY_E(2), 0}},
    .xya = {{40, 40, 0}, {50, 40, 0}},
};

/* ==================================================================== BELLS === */
/* small bells and tuned metal (Tim, 26.10: «колокольчики»): four sine partials at the MODE's ratios, struck (STRK: a click
 * of noise), each its own decay: the first DCAY's (30 ms .. 4 s), the others shorter by DAMP; HARD lifts the upper partials
 * (a hard mallet), SPRD stretches their ratios (inharmonic), SHIM a beating copy of the lower two; DRIV. MODE: TRI (the
 * triangle), GLOK (glockenspiel), CHIM (a chime), HBEL (a hand bell), AGGO (agogo), CLAV (the clave) */
enum { DR_BM_TRI, DR_BM_GLOK, DR_BM_CHIM, DR_BM_HBEL, DR_BM_AGGO, DR_BM_CLAV, DR_BM_N };
static const char *const N_DR_BMODE[DR_BM_N] = {"TRI", "GLOK", "CHIM", "HBEL", "AGGO", "CLAV"};
static const struct {
    uint8_t lo, span;                                    /* TUNE's range, MIDI notes */
    uint16_t r[4];                                       /* the partials' ratios to the first, Q8 */
    uint8_t lev[4];                                      /* their levels (x/32) */
    uint8_t dlo, dsp;                                    /* DCAY: dr_tau's place lo, span */
    uint8_t g;                                           /* the output's gain (x/16): the modes as loud */
} DR_BEL[DR_BM_N] = {
    {86, 20, {256, 727, 1283, 1918}, {32, 22, 18, 14}, 50, 50, 4},   /* TRI: a bright, long ring, near-harmonic */
    {79, 24, {256, 706, 1382, 2286}, {32, 10, 5, 2}, 40, 46, 7},     /* GLOK: a bar's 1 : 2.76 : 5.4 : 8.93 */
    {72, 24, {256, 594, 1088, 1697}, {32, 20, 12, 8}, 52, 50, 5},    /* CHIM: a tube's 1 : 2.32 : 4.25 : 6.63 */
    {67, 24, {256, 515, 725, 1037}, {32, 24, 14, 10}, 46, 50, 4},    /* HBEL: a bell's 1 : 2.01 : 2.83 : 4.05 */
    {68, 18, {256, 595, 1065, 1418}, {32, 18, 10, 6}, 24, 40, 6},    /* AGGO: short, a little clangy */
    {88, 12, {256, 1015, 1530, 2080}, {32, 6, 3, 2}, 8, 30, 11},      /* CLAV: wood, a very short knock */
};
static void dbels_start(track_t *t, voice_t *v, const int16_t *e)
{
    dr_slot_t *S;
    (void)e;
    dr_start(t, v);
    S = dr_slot(t, v);
    S->f[2] = S->f[3] = DR_Q30;                           /* (the upper partials' envelopes, as q[]) */
}
static void dbels_run(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m, const int16_t *e)
{
    dr_slot_t *S = dr_slot(t, v);
    uint32_t md = (uint32_t)clamp(e[0], 0, DR_BM_N - 1), inc[4], i, k;
    int32_t note = clamp(m->pitch16 - 48 * 16, -36 * 16, 36 * 16);
    int32_t b16 = (DR_BEL[md].lo << 4) + clamp(e[1], 0, 127) * (DR_BEL[md].span << 4) / 127 + note;
    uint32_t inc0 = pitch_inc((uint32_t)clamp(b16, 0, 2047)), tau = dr_tau(e[2], DR_BEL[md].dlo, DR_BEL[md].dsp);
    int32_t hard = clamp(e[3], 0, 127), damp = clamp(e[4], 0, 127), sprd = clamp(e[5], 0, 127), shim = clamp(e[6], 0, 127);
    int32_t drv = clamp(e[7], 0, 127), strk = clamp(e[8], 0, 127) * 2 * (64 + v->vel / 2) / 127, lv[4], a[4], d[4];
    int32_t cs = S->n ? S->e[0] : 32767, x1 = S->x1;
    uint32_t kd = 256u - (uint32_t)damp * 200u / 127u, kc = dv_k(1000u);   /* (DAMP: the upper partials' tau x 1 .. 0.22, each
                                                          * step up) */
    uint32_t kb[4];
    S->n = 1;
    for (k = 0; k < 4u; k++) {
        uint32_t r = DR_BEL[md].r[k];
        uint64_t ii;
        if (k)
            r += (uint32_t)((int32_t)(r - 256u) * (sprd - 40) / 300);   /* SPRD: 40 as designed, up stretches, down squeezes */
        ii = ((uint64_t)inc0 * r) >> 8;
        inc[k] = ii > 0x7F000000u ? 0u : (uint32_t)ii;   /* (over Nyquist: silent) */
        lv[k] = k ? DR_BEL[md].lev[k] * (256 + hard * 14) : 32767;   /* HARD: the uppers x 1/4 .. 2 (60: as designed) */
        lv[k] = clamp(lv[k], 0, 32767);
    }
    kb[0] = dv_kb(tau);
    kb[1] = dv_kb(tau * kd >> 8);
    kb[2] = dv_kb((tau * kd >> 8) * kd >> 8);
    kb[3] = dv_kb(((tau * kd >> 8) * kd >> 8) * kd >> 8);
    if (v->s[6]) {
        kb[0] = kb[1] = kb[2] = kb[3] = dv_kb(DR_CHOKE_US);
        cs = 0;
    }
    dv_slow(&S->q[0], kb[0], &a[0], &d[0]);
    dv_slow(&S->q[1], kb[1], &a[1], &d[1]);
    dv_slow(&S->f[2], kb[2], &a[2], &d[2]);
    dv_slow(&S->f[3], kb[3], &a[3], &d[3]);
    for (i = 0; i < n; i++) {
        int32_t z = DR_NOISE(S), x = 0;
        for (k = 0; k < 4u; k++) {
            x += (((sine_i(S->ph[k]) * lv[k]) >> 15) * (a[k] >> 5)) >> 15;
            S->ph[k] += inc[k];
            a[k] += d[k];
        }
        if (shim) {                                       /* SHIM: the lower two again, ~3 Hz and ~5 Hz off */
            x += (((((sine_i(S->ph[4]) + ((sine_i(S->ph[5]) * lv[1]) >> 15)) * shim) >> 7) * (a[1] >> 5)) >> 15) >> 1;
            S->ph[4] += inc[0] + 292000u;
            S->ph[5] += inc[1] + 487000u;
        }
        if (cs > 64) {
            x += ((((z - x1) >> 1) * cs >> 15) * strk) >> 8;
            cs = (cs * (int32_t)kc) >> 15;
        }
        x1 = z;
        out[i] += voice_amp(dr_out((x * DR_BEL[md].g) >> 4, drv), m, i);
    }
    S->e[0] = cs;
    S->x1 = x1;
    v->s[0] = S->q[0] > S->q[1] ? S->q[0] : S->q[1];
}

/* ===================================================================== TAMB === */
/* the tambourine (Tim, 26.10: its states and its sounds; 27.10: it sounded a snare): the jingles, metal (five squares at a
 * pair of zils' inharmonic modes, 2.4 .. 12.5 kHz, two of them ring-modulated: the clash) struck bead by bead (DENS), each
 * bead a click of high noise and the metal, the metal ringing on under DCAY; two band-passes over it (TONE, RES; the ninth
 * METL the zils: BRAS, STEL brighter, DARK, MINI a small one), and the skin (a short sine knock ~220 Hz, SKIN; no body). STAT how it is played (lockable: a pattern of them): HIT (struck: the skin, the jingles at once), SLAP (the
 * palm: more skin, a crack), SHAK (shaken: the jingles swoosh in and back, no skin), ROLL (the thumb roll: the jingles
 * re-struck RATE times a second for the decay: 4 .. 40; SHAK: RATE the back stroke's time, 250 .. 25 ms), TAP (a light tap: the jingles alone, short). DCAY the jingles' ring, DRIV */
enum { DR_TS_HIT, DR_TS_SLAP, DR_TS_SHAK, DR_TS_ROLL, DR_TS_TAP, DR_TS_N };
static const char *const N_DR_TSTAT[DR_TS_N] = {"HIT", "SLAP", "SHAK", "ROLL", "TAP"};
enum { DR_TM_BRAS, DR_TM_STEL, DR_TM_DARK, DR_TM_MINI, DR_TM_N };
static const char *const N_DR_TMETL[DR_TM_N] = {"BRAS", "STEL", "DARK", "MINI"};
static const int16_t DR_TM_P16[DR_TM_N][2] = {{1712, 1580}, {1770, 1610}, {1640, 1530}, {1810, 1660}};   /* the band, the
                                                             high-pass under the zils, p16 */
static const uint32_t DR_TM_HZ[DR_TM_N][5] = {{2340, 3320, 4400, 5410, 6790}, {2700, 3830, 5190, 6330, 7970},
                                              {1810, 2610, 3470, 4430, 5540}, {3230, 4610, 6080, 7650, 9380}};   /* the zils */
static void dtamb_start(track_t *t, voice_t *v, const int16_t *e)
{
    dr_slot_t *S;
    uint32_t st = (uint32_t)clamp(e[0], 0, DR_TS_N - 1);
    dr_start(t, v);
    S = dr_slot(t, v);
    S->e[0] = st == DR_TS_SHAK ? 0 : 32767;               /* (the jingles' rise: SHAK swooshes in) */
    S->e[1] = st == DR_TS_SHAK || st == DR_TS_TAP ? 0 : 32767;   /* (the skin) */
    S->ph[1] = 32767;                                     /* (the burst: struck) */
    S->ph[0] = 0x40000000u;
    S->ph[3] = 0x12345678u;                               /* (the zils apart from the start) */
    S->ph[4] = 0x9ABCDEF0u;
    S->ph[5] = 0x2468ACE0u;
    S->ph[6] = 0x13579BDFu;
    S->ph[7] = 0xFEDCBA98u;
    S->q[1] = 0;                                          /* (ROLL: the last strike's sample; dr_start leaves it full scale) */
    v->s[2] = st == DR_TS_SHAK;                           /* (SHAK: alive till the back stroke) */
}
static int32_t dtamb_zils(const uint32_t *ph)          /* five squares, two pairs ring-modulated (the clash) */
{
    return ((int32_t)(ph[3] >> 31) ? 3000 : -3000) + ((int32_t)((ph[4] ^ ph[5]) >> 31) ? 3000 : -3000) +
           ((int32_t)(ph[6] >> 31) ? 2400 : -2400) + ((int32_t)((ph[7] ^ ph[3]) >> 31) ? 2400 : -2400) +
           ((int32_t)(ph[5] >> 31) ? 1800 : -1800);
}
static void dtamb_run(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m, const int16_t *e)
{
    dr_slot_t *S = dr_slot(t, v);
    uint32_t st = (uint32_t)clamp(e[0], 0, DR_TS_N - 1), mt = (uint32_t)clamp(e[8], 0, DR_TM_N - 1), i;
    int32_t note = clamp(m->pitch16 - 48 * 16, -24 * 16, 24 * 16), tone = clamp(e[1], 0, 127) * 3 - 190 + note / 2;
    int32_t skin = clamp(e[3], 0, 127) * (st == DR_TS_SLAP ? 150 : 90), dens = clamp(e[4], 0, 127);
    int32_t per = (int32_t)FS / (4 + clamp(e[5], 0, 127) * 36 / 127), reso = clamp(e[6], 0, 127) * 100 / 127;
    int32_t drv = clamp(e[7], 0, 127) + (st == DR_TS_SLAP ? 30 : 0), a, d, rise = S->e[0], sk = S->e[1];
    int32_t bu = (int32_t)S->ph[1], g = S->cnt, x1 = S->x1;
    uint32_t dlo = st == DR_TS_TAP ? 18u : st == DR_TS_ROLL ? 40u : 30u;
    uint32_t kb = dv_kb(dr_tau(e[2], dlo, 40)), kg = dv_k(900u), ksk = dv_k(st == DR_TS_SLAP ? 9000u : 6000u);
    uint32_t kbu = dv_k(st == DR_TS_SLAP ? 2500u : 6000u), inc = pitch_inc((uint32_t)clamp(57 * 16 + note, 0, 2047));
    uint32_t zi[5], k, tmul = dv_exp2((uint32_t)clamp(4096 + tone * 4096 / 384, 0, 3 * 4096)) >> 9;   /* (TONE: the zils
                                                             +-~half an octave; Q8, 256 = as listed) */
    uint32_t thr = (dv_exp2((uint32_t)(4096 * 9 + dens * 4096 * 4 / 127)) >> 16) * 97391u;   /* (beads ~500 .. 8000 a second) */
    tsvf_t c1, c2;
    for (k = 0; k < 5u; k++)
        zi[k] = (((DR_TM_HZ[mt][k] * 97392u) >> 8) * tmul) >> 8;
    tsvf_coef(&c1, dr_cut(DR_TM_P16[mt][0] + tone), 20 + reso / 2);
    tsvf_coef(&c2, dr_cut(DR_TM_P16[mt][1] + tone), 0);
    if (st == DR_TS_SHAK && !S->n && (int32_t)S->ph[2] >= per) {   /* SHAK: the back stroke, RATE's period on (250 .. 25 ms) */
        S->n = 1;
        S->q[0] = DR_Q30 / 4 * 3;
        rise = 0;
        v->s[2] = 0;
    }
    if (st == DR_TS_ROLL && (int32_t)S->ph[2] - S->q[1] >= per) {   /* ROLL: struck again */
        S->q[1] = (int32_t)S->ph[2];
        bu = 22000 + (DR_NOISE(S) >> 3);
    }
    if (v->s[6]) {
        kb = dv_kb(DR_CHOKE_US);
        sk = bu = 0;
        v->s[2] = 0;
    }
    dv_slow(&S->q[0], kb, &a, &d);
    for (i = 0; i < n; i++) {
        int32_t z = DR_NOISE(S), x, j, env, mtl;
        if ((uint32_t)noise32(&S->rng) < thr)            /* a bead (a jingle against the next) */
            g = 32767;
        mtl = dtamb_zils(S->ph);
        for (k = 0; k < 5u; k++)
            S->ph[3 + k] += zi[k];
        x = ((mtl * (9000 + ((g * 23767) >> 15))) >> 13) + ((z * (g >> 2)) >> 15);   /* the metal ringing, each bead
                                                             a click of it and of high noise */
        g = (g * (int32_t)kg) >> 15;
        j = x + clamp(dv_bp(&c1, x, &S->f[0]), -150000, 150000);   /* the zils and their band (RES) */
        S->f[0] = clamp(S->f[0], -150000, 150000);
        S->f[1] = clamp(S->f[1], -150000, 150000);
        {                                                 /* a high-pass under them (~1.7 .. 2.5 kHz): no body, no hum */
            int32_t v3 = j - S->f[3];
            int32_t v1 = (c2.a1 * S->f[2] + c2.a2 * v3) >> 13;
            int32_t v2 = S->f[3] + ((c2.a2 * S->f[2] + c2.a3 * v3) >> 13);
            S->f[2] = clamp(2 * v1 - S->f[2], -150000, 150000);
            S->f[3] = clamp(2 * v2 - S->f[3], -150000, 150000);
            j = j - v2 - v1;
        }
        env = (((a >> 5) * rise) >> 15) + (st == DR_TS_ROLL ? (bu >> 1) : (bu >> 2));
        x = (clamp(j * 3, -65535, 65535) * clamp(env, 0, 32767)) >> 15;
        if (sk > 64) {                                    /* the skin: a short knock ~220 Hz */
            x += (((sine_i(S->ph[0]) * sk) >> 15) * skin) >> 15;
            S->ph[0] += inc;
            sk = (sk * (int32_t)ksk) >> 15;
        }
        if (st == DR_TS_SLAP && bu > 64)
            x += ((((z - x1) >> 1) * bu) >> 15);
        x1 = z;
        bu = (bu * (int32_t)kbu) >> 15;
        rise = rise + 110 > 32767 ? 32767 : rise + 110;   /* (SHAK: in over ~7 ms) */
        a += d;
        out[i] += voice_amp(dr_out(x, drv), m, i);
    }
    S->ph[1] = (uint32_t)bu;
    S->ph[2] += n;
    S->cnt = g;
    S->e[0] = rise;
    S->e[1] = sk;
    S->x1 = x1;
    v->s[0] = S->q[0];
}
DR_WRAP(dbels)
DR_WRAP(dtamb)
#define DR_CAT_BELS "DDDDDD"
#define DR_CAT_TAMB "DDDDD"
static const preset_t DBELS_PRESETS[] = {
    /* MODE TUNE DCAY HARD DAMP SPRD SHIM DRIV */
    {"BL TRIANGLE", {DR_BM_TRI, 64, 80, 70, 30, 40, 20, 0}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 20)},
    {"BL GLOCK", {DR_BM_GLOK, 64, 60, 60, 60, 40, 0, 0}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 20)},
    {"BL CHIME", {DR_BM_CHIM, 64, 90, 50, 40, 40, 30, 0}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 10, 30)},
    {"BL HANDBELL", {DR_BM_HBEL, 64, 80, 60, 40, 40, 40, 0}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 20)},
    {"BL AGOGO", {DR_BM_AGGO, 64, 60, 70, 60, 40, 0, 10}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 10)},
    {"BL CLAVE", {DR_BM_CLAV, 64, 50, 40, 60, 40, 0, 0}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 10)},
};
static const preset_t DTAMB_PRESETS[] = {
    /* STAT TONE DCAY SKIN DENS RATE RES DRIV */
    {"TB HIT", {DR_TS_HIT, 64, 60, 50, 64, 50, 40, 0}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 10)},
    {"TB SLAP", {DR_TS_SLAP, 64, 50, 80, 64, 50, 40, 10}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 10)},
    {"TB SHAKE", {DR_TS_SHAK, 70, 50, 0, 80, 40, 40, 0}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 10)},
    {"TB ROLL", {DR_TS_ROLL, 64, 90, 0, 70, 60, 40, 0}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 20)},
    {"TB TAP", {DR_TS_TAP, 80, 40, 0, 50, 50, 50, 0}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 10)},
};
static const engine_t ENG_DBELS = {
    .name = "BELLS", .page_title = {"BELLS", "PARTS"}, .presets = DBELS_PRESETS, .npresets = NELEM(DBELS_PRESETS),
    .note_on = dbels_note_on, .render = dbels_render, DR_MEM, .knob = {P_E0, P_E1, P_E2, P_E3},
    .edit = {{"MODE", F_ENUM, 0, DR_BM_N - 1, 0, N_DR_BMODE, 0}, {"TUNE", F_PCT, 0, 127, 64, 0, 0},
             {"DCAY", F_PCT, 0, 127, 70, 0, 0}, {"HARD", F_PCT, 0, 127, 60, 0, 0}, {"DAMP", F_PCT, 0, 127, 50, 0, 0},
             {"SPRD", F_PCT, 0, 127, 40, 0, 0}, {"SHIM", F_PCT, 0, 127, 0, 0, 0}, {"DRIV", F_PCT, 0, 127, 0, 0, 0}},
    .e8 = {"STRK", F_PCT, 0, 127, 40, 0, 0},
    .xyd = {{XY_E(1), XY_E(5), 0}, {XY_E(2), XY_E(3), 0}},
    .xya = {{40, 40, 0}, {50, 40, 0}},
};
static const engine_t ENG_DTAMB = {
    .name = "TAMB", .page_title = {"TAMB", "JINGLE"}, .presets = DTAMB_PRESETS, .npresets = NELEM(DTAMB_PRESETS),
    .note_on = dtamb_note_on, .render = dtamb_render, DR_MEM, .knob = {P_E0, P_E1, P_E2, P_E3},
    .edit = {{"STAT", F_ENUM, 0, DR_TS_N - 1, 0, N_DR_TSTAT, 0}, {"TONE", F_PCT, 0, 127, 64, 0, 0},
             {"DCAY", F_PCT, 0, 127, 60, 0, 0}, {"SKIN", F_PCT, 0, 127, 50, 0, 0}, {"DENS", F_PCT, 0, 127, 64, 0, 0},
             {"RATE", F_PCT, 0, 127, 50, 0, 0}, {"RES", F_PCT, 0, 127, 40, 0, 0}, {"DRIV", F_PCT, 0, 127, 0, 0, 0}},
    .e8 = {"METL", F_ENUM, 0, DR_TM_N - 1, 0, N_DR_TMETL, 0},
    .xyd = {{XY_E(1), XY_E(4), 0}, {XY_E(2), XY_E(3), 0}},
    .xya = {{40, 40, 0}, {50, 40, 0}},
};

/* ====================================================================== ZAP === */
/* FM hits (Tim, 31.10: lasers, blips, metallic clicks): a sine carrier phase-modulated by a sine (RAT its ratio, FM the depth
 * falling over FMDC, FB the modulator on itself: noisier), the pitch swept down from SWEP above TUNE (30 Hz .. 2.5 kHz) over
 * STIM, the amplitude falling over DCAY; the ninth, DRIV. ph[0] the carrier, ph[1] the modulator; e[0] the sweep, e[1] the
 * depth's fall (Q15), x1 the modulator's last output */
static const char *const N_DR_ZRAT[8] = {"0.5", "1", "1.41", "2", "2.76", "3.5", "5.19", "7.13"};
static const uint16_t DR_ZRAT_Q8[8] = {128, 256, 361, 512, 707, 896, 1329, 1825};
static void dzap_start(track_t *t, voice_t *v, const int16_t *e) { (void)e; dr_start(t, v); }
static void dzap_run(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m, const int16_t *e)
{
    dr_slot_t *S = dr_slot(t, v);
    int32_t note = clamp(m->pitch16 - 48 * 16, -24 * 16, 24 * 16), pe = S->e[0], me = S->e[1], x1 = S->x1, a, d;
    int32_t b16 = (23 + clamp(e[0], 0, 127) * 76 / 127) * 16 + note, sw16 = clamp(e[1], 0, 127) * 72 * 16 / 127;
    int32_t fm = clamp(e[5], 0, 127) * 82, fb = clamp(e[7], 0, 127) * 16, drv = clamp(e[8], 0, 127);
    uint32_t ks = dv_k(dr_tau(e[2], 0, 80)), km = dv_k(dr_tau(e[6], 10, 80)), kb = dv_kb(dr_tau(e[3], 20, 70));
    uint32_t rq = DR_ZRAT_Q8[clamp(e[4], 0, 7)], i, inc0, inc1;
    int32_t pe1;
    if (v->s[6])
        kb = dv_kb(DR_CHOKE_US);
    inc0 = pitch_inc((uint32_t)clamp(b16 + ((sw16 * pe) >> 15), 0, 2047));   /* (the sweep's pitch at the block's start, end) */
    for (i = 0, pe1 = pe; i < n; i++)
        pe1 = (pe1 * (int32_t)ks) >> 15;
    inc1 = pitch_inc((uint32_t)clamp(b16 + ((sw16 * pe1) >> 15), 0, 2047));
    dv_slow(&S->q[0], kb, &a, &d);
    for (i = 0; i < n; i++) {
        uint32_t inc = inc0 + (uint32_t)(((int32_t)inc1 - (int32_t)inc0) / (int32_t)(n ? n : 1u) * (int32_t)i);
        int32_t mo, x;
        mo = sine_i(S->ph[1] + ((uint32_t)(x1 * fb) << 4));
        x1 = mo;
        x = sine_i(S->ph[0] + ((uint32_t)(mo * ((fm * me) >> 15)) << 4));
        S->ph[0] += inc;
        S->ph[1] += (inc >> 8) * rq;                      /* (wraps: the same frequency, folded) */
        me = (me * (int32_t)km) >> 15;
        x = (x * (a >> 5)) >> 15;
        a += d;
        out[i] += voice_amp(dr_out(x, drv), m, i);
    }
    S->e[0] = pe1;
    S->e[1] = me;
    S->x1 = x1;
    v->s[0] = S->q[0];
}
DR_WRAP(dzap)
#define DR_CAT_ZAP "DDDDDD"
static const preset_t DZAP_PRESETS[] = {
    /* TUNE SWEP STIM DCAY RAT FM FMDC FB */
    {"ZP LASER", {70, 110, 50, 70, 1, 40, 40, 0}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 20, 10)},
    {"ZP BLIP", {90, 20, 10, 40, 3, 30, 20, 0}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 20, 10)},
    {"ZP FM PERC", {45, 30, 20, 55, 4, 80, 30, 10}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 10)},
    {"ZP METAL", {60, 0, 0, 70, 6, 100, 60, 30}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 20)},
    {"ZP ZAP KICK", {20, 90, 35, 75, 1, 20, 15, 0}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 0)},
    {"ZP NOISE HIT", {60, 40, 20, 45, 7, 120, 30, 110}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 10)},
};
static const engine_t ENG_DZAP = {
    .name = "ZAP", .page_title = {"PITCH", "FM"}, .presets = DZAP_PRESETS, .npresets = NELEM(DZAP_PRESETS),
    .note_on = dzap_note_on, .render = dzap_render, DR_MEM, .knob = {P_E0, P_E1, P_E3, P_E5},
    .edit = {{"TUNE", F_PCT, 0, 127, 70, 0, 0}, {"SWEP", F_PCT, 0, 127, 110, 0, 0}, {"STIM", F_PCT, 0, 127, 50, 0, 0},
             {"DCAY", F_PCT, 0, 127, 70, 0, 0}, {"RAT", F_ENUM, 0, 7, 1, N_DR_ZRAT, 0}, {"FM", F_PCT, 0, 127, 40, 0, 0},
             {"FMDC", F_PCT, 0, 127, 40, 0, 0}, {"FB", F_PCT, 0, 127, 0, 0, 0}},
    .e8 = {"DRIV", F_PCT, 0, 127, 0, 0, 0},
    .xyd = {{XY_E(1), XY_E(2), 0}, {XY_E(5), XY_E(7), 0}},
    .xya = {{40, 40, 0}, {50, 40, 0}},
};

/* ====================================================================== TOM === */
/* a tuned tom (Tim, 31.10; after the Analog Rhythm's tom): a sine body (TUNE 40 .. 400 Hz) bent down from SWEP above it over
 * STIM, a second membrane mode at 1.59 x (TONE its level, its own shorter fall), a burst of skin noise band-passed at the
 * body's second octave (NOIS), a click (HIT); DCAY the body's fall, DRIV; the ninth, TYPE: TOM (both modes, a soft click),
 * SYN (a pure sine, a longer sweep: the Simmons), 909 (a triangle-ish body, a hard click). ph[0] the body, ph[1] the mode;
 * e[0] the sweep, e[1] the mode's level (Q15), f[0..1] the noise's band-pass */
enum { DR_TT_TOM, DR_TT_SYN, DR_TT_909, DR_TT_N };
static const char *const N_DR_TTYPE[DR_TT_N] = {"TOM", "SYN", "909"};
static void dtom_start(track_t *t, voice_t *v, const int16_t *e) { (void)e; dr_start(t, v); }
static void dtom_run(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m, const int16_t *e)
{
    dr_slot_t *S = dr_slot(t, v);
    uint32_t ty = (uint32_t)clamp(e[8], 0, DR_TT_N - 1), i;
    int32_t note = clamp(m->pitch16 - 48 * 16, -24 * 16, 24 * 16), pe = S->e[0], me = S->e[1], a, d, pe1;
    int32_t b16 = (28 + clamp(e[0], 0, 127) * 40 / 127) * 16 + note, sw16 = clamp(e[1], 0, 127) * 18 * 16 / 127;
    int32_t tone = ty == DR_TT_SYN ? 0 : clamp(e[4], 0, 127) * 200, nois = clamp(e[5], 0, 127) * 180;
    int32_t hit = clamp(e[6], 0, 127) * (ty == DR_TT_909 ? 300 : 160), drv = clamp(e[7], 0, 127);
    uint32_t ks = dv_k(dr_tau(e[2], ty == DR_TT_SYN ? 40 : 20, 60)), km = dv_k(dr_tau(e[3], 10, 50));
    uint32_t kb = dv_kb(dr_tau(e[3], 30, 60)), inc0, inc1;
    tsvf_t c;
    if (v->s[6])
        kb = dv_kb(DR_CHOKE_US);
    tsvf_coef(&c, dr_cut(b16 + 24 * 16), 40);
    inc0 = pitch_inc((uint32_t)clamp(b16 + ((sw16 * pe) >> 15), 0, 2047));
    for (i = 0, pe1 = pe; i < n; i++)
        pe1 = (pe1 * (int32_t)ks) >> 15;
    inc1 = pitch_inc((uint32_t)clamp(b16 + ((sw16 * pe1) >> 15), 0, 2047));
    dv_slow(&S->q[0], kb, &a, &d);
    for (i = 0; i < n; i++) {
        uint32_t inc = inc0 + (uint32_t)(((int32_t)inc1 - (int32_t)inc0) / (int32_t)(n ? n : 1u) * (int32_t)i);
        int32_t x, body = ty == DR_TT_909 ? (sine_i(S->ph[0]) + osc_tri(S->ph[0])) >> 1 : sine_i(S->ph[0]), z;
        x = (body * (a >> 5)) >> 15;
        if (tone)                                         /* the second mode, 1.59 x */
            x += (((sine_i(S->ph[1]) * me) >> 15) * tone) >> 15;
        z = DR_NOISE(S);
        if (nois && S->cnt < 2400)                        /* the skin: noise, ~50 ms, band-passed */
            x += ((clamp(dv_bp(&c, z, &S->f[0]), -60000, 60000) * nois) >> 15) * (2400 - S->cnt) / 2400;
        if (hit && S->cnt < 90)                           /* the click */
            x += (((z * hit) >> 15) * (90 - S->cnt)) / 90;
        S->f[0] = clamp(S->f[0], -150000, 150000);
        S->f[1] = clamp(S->f[1], -150000, 150000);
        S->ph[0] += inc;
        S->ph[1] += inc + (inc >> 1) + (inc >> 4) + (inc >> 5);   /* (1.59) */
        me = (me * (int32_t)km) >> 15;
        S->cnt++;
        a += d;
        out[i] += voice_amp(dr_out(x, drv), m, i);
    }
    S->e[0] = pe1;
    S->e[1] = me;
    v->s[0] = S->q[0];
}
DR_WRAP(dtom)
#define DR_CAT_TOM "DDDDDD"
static const preset_t DTOM_PRESETS[] = {
    /* TUNE SWEP STIM DCAY TONE NOIS HIT DRIV (TYPE) */
    {"TM LOW TOM", {30, 40, 40, 70, 50, 40, 30, 0}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 20)},
    {"TM MID TOM", {60, 40, 40, 60, 50, 40, 30, 0}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 20)},
    {"TM HI TOM", {90, 40, 35, 55, 50, 40, 30, 0}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 20)},
    {"TM FLOOR", {15, 30, 50, 85, 40, 30, 20, 10}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 30)},
    {"TM SIMMONS", {55, 110, 70, 60, 0, 60, 20, 0}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 10, 30)},
    {"TM 909 TOM", {50, 50, 30, 55, 20, 20, 90, 10}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 20)},
};
static const engine_t ENG_DTOM = {
    .name = "TOM", .page_title = {"BODY", "SKIN"}, .presets = DTOM_PRESETS, .npresets = NELEM(DTOM_PRESETS),
    .note_on = dtom_note_on, .render = dtom_render, DR_MEM, .knob = {P_E0, P_E1, P_E3, P_E5},
    .edit = {{"TUNE", F_PCT, 0, 127, 60, 0, 0}, {"SWEP", F_PCT, 0, 127, 40, 0, 0}, {"STIM", F_PCT, 0, 127, 40, 0, 0},
             {"DCAY", F_PCT, 0, 127, 60, 0, 0}, {"TONE", F_PCT, 0, 127, 50, 0, 0}, {"NOIS", F_PCT, 0, 127, 40, 0, 0},
             {"HIT", F_PCT, 0, 127, 30, 0, 0}, {"DRIV", F_PCT, 0, 127, 0, 0, 0}},
    .e8 = {"TYPE", F_ENUM, 0, DR_TT_N - 1, 0, N_DR_TTYPE, 0},
    .xyd = {{XY_E(0), XY_E(1), 0}, {XY_E(3), XY_E(5), 0}},
    .xya = {{40, 40, 0}, {50, 40, 0}},
};

/* ===================================================================== SAMP === */
/* a slice as a hit (Tim, 31.10: the SLICE machines' kits on a KIT lane): KIT (GLITCH RITUAL MECH BIO ERROR, BBOX's drums),
 * or USER, the sample slot the web editor fills; SLCE its slice, PTCH semitones (the note too), DCAY its fall (127: the slice whole), TONE a low-pass (127 open), CRSH bits
 * and rate thrown away, DRIV; the ninth, GAIN (64: as the slice is). Read from the flash as it plays (one ADPCM decode a source sample, forward). ph[0] the read
 * position's fraction (Q16), cnt the next source sample, e[0] e[1] the two around it, f[0] the low-pass, f[2] f[3] the
 * decoder's state, n the hold's countdown, x1 the sample held */
enum { DR_SK_GLT, DR_SK_RIT, DR_SK_MEC, DR_SK_BIO, DR_SK_ERR, DR_SK_BBX, DR_SK_USER, DR_SK_N };
static const char *const N_DR_SKIT[DR_SK_N] = {"GLITCH", "RITUAL", "MECH", "BIO", "ERROR", "BBOX", "USER"};
static const smp_zone_t *dsmp_zone(const int16_t *e)
{
    static const uint8_t SC[DR_SK_N] = {0, GL_RIT, GL_MEC, GL_BIO, GL_ERR, GL_BBX, 1u};   /* (USER: the slot USR1) */
    return gl_zone(SC[clamp(e[0], 0, DR_SK_N - 1)], (uint32_t)clamp(e[1], 0, 15));
}
static void dsmp_start(track_t *t, voice_t *v, const int16_t *e)
{
    dr_slot_t *S;
    const smp_zone_t *z = dsmp_zone(e);
    int32_t p = 0, ix = 0;
    dr_start(t, v);
    S = dr_slot(t, v);
    S->e[0] = gl_dec(z, 0, &p, &ix);
    S->e[1] = z->n > 1u ? gl_dec(z, 1, &p, &ix) : S->e[0];
    S->cnt = 2;
    S->f[2] = p;
    S->f[3] = ix;
    v->s[2] = 1;                                          /* (alive till the slice's end or the fall's) */
}
static void dsmp_run(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m, const int16_t *e)
{
    dr_slot_t *S = dr_slot(t, v);
    const smp_zone_t *z = dsmp_zone(e);
    int32_t note = clamp(m->pitch16 - 48 * 16, -24 * 16, 24 * 16), d16 = clamp(e[2], -24, 24) * 16 + note, a, d;
    int32_t dc = clamp(e[3], 0, 127), tone = clamp(e[4], 0, 127), cr = clamp(e[5], 0, 127), drv = clamp(e[6], 0, 127);
    uint32_t step = (uint32_t)(((uint64_t)z->rate * (dv_exp2((uint32_t)clamp(d16 * 4096 / 192 + 4 * 4096, 0, 7 * 4096)) >> 4)) >> 16);
    uint32_t kb = dc >= 127 ? 65536u : dv_kb(dr_tau(dc, 30, 80)), i, gi = (uint32_t)(20 + tone * 107 / 127);
    int32_t g = SVF_G[gi], gq = (g << 15) / (4096 + g), bits = cr ? 16 - cr * 12 / 127 : 16, hold = cr ? 1 + cr * 15 / 127 : 1;
    int32_t pr = S->f[2], ix = S->f[3], gn = clamp(e[8], 0, 127);
    step = step > (4u << 16) ? 4u << 16 : step;
    if (v->s[6])
        kb = dv_kb(DR_CHOKE_US);
    dv_slow(&S->q[0], kb, &a, &d);
    for (i = 0; i < n; i++) {
        int32_t x;
        if ((uint32_t)S->cnt > z->n) {                    /* (the slice played out) */
            S->q[0] = 0;
            v->s[2] = 0;
            break;
        }
        x = S->e[0] + (int32_t)(((int64_t)(S->e[1] - S->e[0]) * (int32_t)(S->ph[0] & 0xFFFFu)) >> 16);
        S->ph[0] += step;
        while (S->ph[0] >= 65536u) {                      /* (on to the next source sample) */
            S->ph[0] -= 65536u;
            S->e[0] = S->e[1];
            S->e[1] = (uint32_t)S->cnt < z->n ? gl_dec(z, (uint32_t)S->cnt, &pr, &ix) : 0;
            S->cnt++;
        }
        if (cr) {                                         /* CRSH: the rate held, the bits dropped */
            if (--S->n <= 0) {
                S->n = hold;
                S->x1 = (x >> (16 - bits)) << (16 - bits);
            }
            x = S->x1;
        }
        if (tone < 127)
            S->f[0] += ((x - S->f[0]) * gq) >> 15, x = S->f[0];
        x = (((x * (a >> 5)) >> 15) * gn) >> 6;
        a += d;
        out[i] += voice_amp(dr_out(x, drv), m, i);
    }
    S->f[2] = pr;
    S->f[3] = ix;
    v->s[0] = S->q[0];
}
DR_WRAP(dsmp)
#define DR_CAT_SMP "DDDDDDD"
static const preset_t DSMP_PRESETS[] = {
    /* KIT SLCE PTCH DCAY TONE CRSH DRIV */
    {"SP GLASS TAP", {DR_SK_GLT, 0, 0, 127, 127, 0, 0, 0}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 20)},
    {"SP RELAY", {DR_SK_GLT, 14, 0, 127, 127, 0, 0, 0}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 10)},
    {"SP BELL", {DR_SK_RIT, 0, 0, 127, 127, 0, 0, 0}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 10, 30)},
    {"SP CLOCK", {DR_SK_MEC, 0, 0, 100, 127, 0, 0, 0}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 10)},
    {"SP DRIP", {DR_SK_BIO, 2, 0, 127, 127, 0, 0, 0}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 10, 20)},
    {"SP BIT HIT", {DR_SK_ERR, 4, 0, 90, 127, 60, 20, 0}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 10)},
    {"SP BBOX KICK", {DR_SK_BBX, 0, 0, 127, 127, 0, 10, 0}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 0)},
};
static const engine_t ENG_DSMP = {
    .name = "SAMP", .page_title = {"SLICE", "TONE"}, .presets = DSMP_PRESETS, .npresets = NELEM(DSMP_PRESETS),
    .note_on = dsmp_note_on, .render = dsmp_render, DR_MEM, .knob = {P_E0, P_E1, P_E2, P_E3},
    .edit = {{"KIT", F_ENUM, 0, DR_SK_N - 1, 0, N_DR_SKIT, 0}, {"SLCE", F_INT, 0, 15, 0, 0, 0}, {"PTCH", F_INT, -24, 24, 0, 0, "st"},
             {"DCAY", F_PCT, 0, 127, 127, 0, 0}, {"TONE", F_PCT, 0, 127, 127, 0, 0}, {"CRSH", F_PCT, 0, 127, 0, 0, 0},
             {"DRIV", F_PCT, 0, 127, 0, 0, 0}, {"-", F_INT, 0, 0, 0, 0, 0}},
    .e8 = {"GAIN", F_PCT, 0, 127, 64, 0, 0},              /* (64: as the slice is, 127: x2) */
    .xyd = {{XY_E(1), XY_E(2), 0}, {XY_E(4), XY_E(5), 0}},
    .xya = {{40, 40, 0}, {50, 40, 0}},
};

/* ====================================================================== KIT === */
/* four lanes on one track (T4 only: pulhra/sm_layer.c), each a whole drum machine of the above (Tim): L1 .. L4 its machine
 * (KICK SNARE CLAP HAT CYM), its own nine values (the pattern's sm_pattern_t.lane; in the core dr_lp, set by the sequencer's
 * loads and its parameter commands, pulhra/sm_kit.c), lockable each. A lane is a note: 36 + lane (C2 .. D#2; any other note its
 * lane by note % 4): the sequencer's lanes (sm_seq.c), the keyboard's F3 G3 A3 B3 (keys). A hit chokes only its own lane's last
 * one. Velocity scales it */
enum { DR_KM_KICK, DR_KM_SNARE, DR_KM_CLAP, DR_KM_HAT, DR_KM_CYM, DR_KM_PERC, DR_KM_BELL, DR_KM_RIM, DR_KM_SHKR, DR_KM_BELS, DR_KM_TAMB, DR_KM_ZAP, DR_KM_TOM, DR_KM_SMP, DR_KM_N };
static const char *const N_DR_KMACH[DR_KM_N] = {"KICK", "SNARE", "CLAP", "HAT", "CYM", "CONGA", "BELL", "RIM", "SHKR", "BELLS", "TAMB", "ZAP", "TOM", "SAMP"};
static const engine_t *const DR_KENG[DR_KM_N] = {&ENG_DKICK, &ENG_DSNARE, &ENG_DCLAP, &ENG_DHAT, &ENG_DCYM, &ENG_DPERC,
                                                  &ENG_DBELL, &ENG_DRIM, &ENG_DSHK, &ENG_DBELS, &ENG_DTAMB, &ENG_DZAP, &ENG_DTOM, &ENG_DSMP};
static int16_t dr_lp[NPART][4][9] __attribute__((section(".pool")));   /* the core's lanes: each part's (only T4 plays a KIT); the audio stream's (the pool: RAM keeps its 4 KiB headroom) */
static int16_t dr_lm[NPART][4][4];                        /* ... each lane's mix: LVL CHO DLY REV (pulhra sm_pattern_t.lmix) */
static int16_t dr_lf[NPART][4][3];                        /* ... each lane's filter: FILT CUT RES (voice.c mf_filter_voice) */
static int8_t dr_le[NPART][4][4] = {[0 ... NPART - 1] = {[0 ... 3] = {0, 64, 127, 127}}};   /* ... each lane's envelope: ATK DEC SUS
                                                             REL (pulhra sm_pattern_t.lenv; these, the defaults: the hit as it is) */
static int8_t dr_lv[NPART][4][5] = {[0 ... NPART - 1] = {[0 ... 3] = {0, 64, 0, 0, 0}}};   /* ... each lane's filter envelope: ATK DEC
                                                             SUS REL and ENV>FLT (pulhra sm_pattern_t.lfenv; voice.c fenv_tick) */
static int32_t dr_ls[3][CTL] __attribute__((section(".pool")));                             /* the KIT's lanes' sends this block (CHO DLY REV, at the lanes' LVL; fx.c
                                                             mix_part takes them at the track's LEVEL, the track's own sends off) */
static int32_t dr_kbuf[CTL] __attribute__((section(".pool")));   /* (the pool: RAM keeps its 4 KiB headroom) */
#define DR_KIT_NOTE 36u                                   /* lane 0's note (C2) */
static uint32_t dr_kmach(int32_t v) { return (uint32_t)clamp(v, 0, DR_KM_N - 1); }
/* a lane's nine values: machine m's factory sound pi (the ninth its default) */
static void dr_lane_init(int8_t *e9, uint32_t m, uint32_t pi)
{
    const engine_t *e = DR_KENG[m % DR_KM_N];
    const preset_t *pr = &e->presets[pi % e->npresets];
    uint32_t i;
    for (i = 0; i < 8u; i++)
        e9[i] = pr->e[i];
    e9[8] = (int8_t)e->e8.def;
}
/* the descriptor of value k (0..8) of a lane of machine m */
static const param_desc_t *dr_lane_desc(uint32_t m, uint32_t k)
{
    const engine_t *e = DR_KENG[m % DR_KM_N];
    return k < 8u ? &e->edit[k] : &e->e8;
}
static uint32_t dr_lane(uint32_t note) { return (note - DR_KIT_NOTE) & 3u; }   /* (36..39: 0..3; any other: % 4) */
static void dkit_note_on(track_t *t, voice_t *v)
{
    uint32_t L = dr_lane(v->note), m = dr_kmach(t->p[P_E0 + L]);
    const int16_t *e = dr_lp[(uint32_t)(t - trk) % NPART][L];
    dr_lane_choke = 1;
    if (m == DR_KM_KICK) {
        dr_slot_t *S = dr_slot(t, v);                     /* (KICK keeps its state in the voice: the slot clean anyway) */
        memset(S, 0, sizeof *S);
        dkick_start(t, v, e);
    } else if (m == DR_KM_SNARE) {
        dsnare_start(t, v, e);
    } else if (m == DR_KM_CLAP) {
        dclap_start(t, v, e);
    } else if (m == DR_KM_HAT) {
        dhat_start(t, v, e);
    } else if (m == DR_KM_PERC) {
        dperc_start(t, v, e);
    } else if (m == DR_KM_BELL) {
        dbell_start(t, v, e);
    } else if (m == DR_KM_RIM) {
        drim_start(t, v, e);
    } else if (m == DR_KM_SHKR) {
        dshk_start(t, v, e);
    } else if (m == DR_KM_BELS) {
        dbels_start(t, v, e);
    } else if (m == DR_KM_TAMB) {
        dtamb_start(t, v, e);
    } else if (m == DR_KM_ZAP) {
        dzap_start(t, v, e);
    } else if (m == DR_KM_TOM) {
        dtom_start(t, v, e);
    } else if (m == DR_KM_SMP) {
        dsmp_start(t, v, e);
    } else {
        dcym_start(t, v, e);
    }
    dr_lane_choke = 0;
    {                                                     /* the lane's envelope: ATK 0 at full at once (the hit's own attack) */
        dr_slot_t *S = dr_slot(t, v);
        const int8_t *le = dr_le[(uint32_t)(t - trk) % NPART][L];
        S->lst = le[0] ? 1u : 2u;
        S->le = le[0] ? 0 : 1 << 24;
        S->lg = le[0] ? 0u : 32767u;
    }
}
/* the lane's envelope (Tim: the AMP page of a KIT lane, ATK DEC SUS REL): one step a block (ATK linear, DEC to SUS and REL
 * exponential, as the track's ADSR); REL past the note's end (its LEN), REL at the top (INF) none: the hit rings out as it is.
 * At the defaults (ATK 0, SUS 127, REL INF) a gain of 1 throughout: the hit as before. Its gain Q15 at this block's end */
static uint32_t dkit_env(track_t *t, voice_t *v, uint32_t L)
{
    dr_slot_t *S = dr_slot(t, v);
    const int8_t *le = dr_le[(uint32_t)(t - trk) % NPART][L];
    int32_t sus = (int32_t)clamp(le[2], 0, 127) * 132104;    /* (127: ~1.0) */
    if (!v->gate && S->lst < 4u && le[3] < 127)
        S->lst = 4;
    if (S->lst == 1u) {
        S->le += (int32_t)ENV_LIN[le[0] & 127];
        if (S->le >= (1 << 24)) {
            S->le = 1 << 24;
            S->lst = 2;
        }
    } else if (S->lst == 2u) {
        S->le -= mulq16(S->le - sus, ENV_EXP[le[1] & 127]);
        if (S->le - sus < (1 << 10)) {
            S->le = sus;
            S->lst = 3;
        }
    } else if (S->lst == 4u) {
        S->le -= mulq16(S->le, ENV_EXP[le[3] & 127]);
        if (S->le < (1 << 10))
            S->le = 0;
    }
    if (S->lst != 1u && S->le < (1 << 10)) {           /* (decayed to SUS 0, or released: the hit is over) */
        S->le = 0;
        S->lst = 5;
    }
    return (uint32_t)(S->le >= (1 << 24) ? 32767 : S->le >> 9);
}
/* the KIT's amplitude: dr_amp, and a lane whose envelope is over ends its hit */
static int32_t dkit_amp(track_t *t, voice_t *v, int32_t adsr)
{
    if (v->active && dr_slot(t, v)->lst == 5u) {
        v->active = v->gate = 0;
        v->stage = 0;
        v->env = 0;
        return 0;
    }
    return dr_amp(t, v, adsr);
}
static void dkit_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    uint32_t L = dr_lane(v->note), mc = dr_kmach(t->p[P_E0 + L]);
    const int16_t *e = dr_lp[(uint32_t)(t - trk) % NPART][L];
    const int16_t *lm = dr_lm[(uint32_t)(t - trk) % NPART][L];
    int32_t *b = dr_kbuf, lv = LEVEL_Q12[clamp(lm[0], 0, 127)], sc = clamp(lm[1], 0, 127) * 258, sd = clamp(lm[2], 0, 127) * 258;
    int32_t sr = clamp(lm[3], 0, 127) * 258;
    vmod_t km = *m;
    uint32_t i, g0 = dr_slot(t, v)->lg, g1;
    km.pitch16 = 48 * 16;                                 /* (each lane at its machine's own TUNE: C3) */
    for (i = 0; i < n; i++)
        b[i] = 0;
    if (mc == DR_KM_KICK)
        dkick_run(t, v, b, n, &km, e);
    else if (mc == DR_KM_SNARE)
        dsnare_run(t, v, b, n, &km, e);
    else if (mc == DR_KM_CLAP)
        dclap_run(t, v, b, n, &km, e);
    else if (mc == DR_KM_HAT)
        dhat_run(t, v, b, n, &km, e);
    else if (mc == DR_KM_PERC)
        dperc_run(t, v, b, n, &km, e);
    else if (mc == DR_KM_BELL)
        dbell_run(t, v, b, n, &km, e);
    else if (mc == DR_KM_RIM)
        drim_run(t, v, b, n, &km, e);
    else if (mc == DR_KM_SHKR)
        dshk_run(t, v, b, n, &km, e);
    else if (mc == DR_KM_BELS)
        dbels_run(t, v, b, n, &km, e);
    else if (mc == DR_KM_TAMB)
        dtamb_run(t, v, b, n, &km, e);
    else if (mc == DR_KM_ZAP)
        dzap_run(t, v, b, n, &km, e);
    else if (mc == DR_KM_TOM)
        dtom_run(t, v, b, n, &km, e);
    else if (mc == DR_KM_SMP)
        dsmp_run(t, v, b, n, &km, e);
    else
        dcym_run(t, v, b, n, &km, e);
    g1 = dkit_env(t, v, L);
    dr_slot(t, v)->lg = (uint16_t)g1;
    if (g0 != 32767u || g1 != 32767u) {                   /* the envelope, ramped across the block (no step on the block's edge) */
        int32_t g = (int32_t)g0 << 8, d = (((int32_t)g1 - (int32_t)g0) << 8) / (int32_t)(n ? n : 1u);
        for (i = 0; i < n; i++) {
            g += d;
            b[i] = ((clamp(b[i], -131071, 131071) >> 2) * (g >> 8)) >> 13;   /* (< 2^30) */
        }
    }
    for (i = 0; i < n; i++) {                             /* the lane's LVL (as a track's LEVEL, 112: 0 dB) and its sends */
        int32_t x = (clamp(b[i], -131071, 131071) * lv) >> 12;
        out[i] += x;
        if (sc)
            dr_ls[0][i] += (x * (sc >> 3)) >> 12;
        if (sd)
            dr_ls[1][i] += (x * (sd >> 3)) >> 12;
        if (sr)
            dr_ls[2][i] += (x * (sr >> 3)) >> 12;
    }
}
/* the keyboard: the white F3 G3 A3 B3 (keys 0 2 4 6) the four lanes, the other keys nothing */
static int32_t dkit_keys(const track_t *t, uint32_t k)
{
    (void)t;
    return k < 7u && !(k & 1u) ? (int32_t)(DR_KIT_NOTE + k / 2u) : -1;
}
static const preset_t DKIT_PRESETS[] = {
    /* L1..L4 machine, then the factory sound of each lane's machine */
    {"KIT 808", {DR_KM_KICK, DR_KM_SNARE, DR_KM_HAT, DR_KM_CLAP, 0, 0, 0, 0}, {0, 64, 127, 30}, 0, 0, FX(0, 0, 0, 10)},
    {"KIT PUNCH", {DR_KM_KICK, DR_KM_SNARE, DR_KM_HAT, DR_KM_CLAP, 1, 1, 0, 1}, {0, 64, 127, 30}, 0, 0, FX(10, 0, 0, 10)},
    {"KIT DEEP", {DR_KM_KICK, DR_KM_SNARE, DR_KM_HAT, DR_KM_CYM, 2, 2, 1, 1}, {0, 64, 127, 30}, 0, 0, FX(0, 0, 0, 30)},
    {"KIT HARD", {DR_KM_KICK, DR_KM_SNARE, DR_KM_HAT, DR_KM_CLAP, 3, 3, 3, 2}, {0, 64, 127, 30}, 0, 0, FX(20, 0, 0, 0)},
};
#define DR_CAT_KIT "DDDD"
static const engine_t ENG_DKIT = {
    .name = "DRUM", .page_title = {"LANES", "SOUNDS"}, .presets = DKIT_PRESETS, .npresets = NELEM(DKIT_PRESETS),
    .note_on = dkit_note_on, .render = dkit_render, .keys = dkit_keys,
    .mem = sizeof(dr_slot_t) * NVOICE, .amp = dkit_amp, .oneshot = 1, .poly = 6,
    .knob = {P_E0, P_E1, P_E2, P_E3},
    .edit = {{"L1", F_ENUM, 0, DR_KM_N - 1, DR_KM_KICK, N_DR_KMACH, 0}, {"L2", F_ENUM, 0, DR_KM_N - 1, DR_KM_SNARE, N_DR_KMACH, 0},
             {"L3", F_ENUM, 0, DR_KM_N - 1, DR_KM_HAT, N_DR_KMACH, 0}, {"L4", F_ENUM, 0, DR_KM_N - 1, DR_KM_CLAP, N_DR_KMACH, 0},
             {"S1", F_INT, 0, 5, 0, 0, 0}, {"S2", F_INT, 0, 5, 0, 0, 0}, {"S3", F_INT, 0, 5, 0, 0, 0}, {"S4", F_INT, 0, 5, 0, 0, 0}},
    .xyd = {{XY_E(4), XY_E(5), 0}, {XY_E(6), XY_E(7), 0}},
    .xya = {{40, 40, 0}, {40, 40, 0}},
};
