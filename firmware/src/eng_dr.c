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

/* ===================================================================== PERC === */
/* the analog percussion (Tim: the KIT's sixth machine), MODE one of: TOM (a sine shell swept down BEND, a fifth above it at
 * TONE, skin noise), CONGA (higher, a tighter shell, its partial a minor sixth up), RIM (two high sines, short, driven: the
 * 808's rim shot), BELL (two squares 540 : 800, a band-pass at TONE: the 808's cowbell), CLAVE (one high sine, a click).
 * TUNE moves each mode inside its range, DCAY its decay, NOIS a band-passed noise on the skin, SLAP the attack (a
 * high-passed spike, with the velocity), DRIV the clip; the ninth, BTIM, the bend's time */
enum { DR_PM_TOM, DR_PM_CONGA, DR_PM_RIM, DR_PM_BELL, DR_PM_CLAVE, DR_PM_N };
static const char *const N_DR_PMODE[DR_PM_N] = {"TOM", "CONGA", "RIM", "BELL", "CLAVE"};
static const uint8_t DR_PM_LO[DR_PM_N] = {36, 52, 71, 66, 88};       /* TUNE's range, MIDI notes: lo .. lo + span */
static const uint8_t DR_PM_SPAN[DR_PM_N] = {36, 30, 18, 18, 12};
static const uint16_t DR_PM_IV[DR_PM_N] = {112, 130, 340, 109, 282};  /* the second partial above, 1/16 semitone */
static const uint8_t DR_PM_DLO[DR_PM_N] = {34, 28, 8, 24, 6};        /* DCAY: dr_tau's place lo, span */
static const uint8_t DR_PM_DSP[DR_PM_N] = {50, 45, 34, 46, 34};
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
    uint32_t md = (uint32_t)clamp(e[0], 0, DR_PM_N - 1);
    int32_t note = clamp(m->pitch16 - 48 * 16, -48 * 16, 48 * 16);
    int32_t b16 = (DR_PM_LO[md] << 4) + clamp(e[1], 0, 127) * (DR_PM_SPAN[md] << 4) / 127 + note;
    uint32_t inc1 = pitch_inc((uint32_t)clamp(b16, 0, 2047)), inc2 = pitch_inc((uint32_t)clamp(b16 + DR_PM_IV[md], 0, 2047));
    uint32_t kb = dv_kb(dr_tau(e[2], DR_PM_DLO[md], DR_PM_DSP[md])), kn = dv_k(18000u), ksn = dv_k(1800u);
    uint32_t ksw = dv_k(dr_tau(e[8], 12, 50));
    int32_t bend = md <= DR_PM_CONGA ? clamp(e[3], 0, 127) : clamp(e[3], 0, 127) / 4;   /* (up to an octave: TOM, CONGA) */
    int32_t tone = clamp(e[4], 0, 127), nlev = clamp(e[5], 0, 127) * 200, slap = clamp(e[6], 0, 127) * 2 * v->vel;
    int32_t drv = clamp(e[7], 0, 127) + (md == DR_PM_RIM ? 40 : 0), lev2 = md == DR_PM_BELL ? 32767 : tone * 200;
    int32_t ab, db, nz = S->e[0], sw = S->e[1], sn = S->cnt ? S->cnt : (S->n ? 0 : 32767), x1 = S->x1;
    tsvf_t cn, cb;
    uint32_t i;
    S->n = 1;                                             /* (the slap started: once a hit) */
    tsvf_coef(&cn, dr_cut(b16 + (19 << 4)), 40);          /* the skin noise around two octaves and a half up */
    tsvf_coef(&cb, dr_cut((88 << 4) + tone * (14 << 4) / 127), 70);   /* BELL: 1.3 .. 3 kHz */
    if (v->s[6]) {
        kb = dv_kb(DR_CHOKE_US);
        nz = sn = 0;
    }
    dv_slow(&S->q[0], kb, &ab, &db);
    for (i = 0; i < n; i++) {
        int32_t z = DR_NOISE(S), x, o1, o2;
        uint32_t ext = (uint32_t)((sw * bend) >> 7);
        S->ph[0] += inc1 + (uint32_t)((inc1 >> 15) * ext);
        S->ph[1] += inc2 + (uint32_t)((inc2 >> 15) * ext);
        sw = (sw * (int32_t)ksw) >> 15;
        if (md == DR_PM_BELL) {
            o1 = S->ph[0] < 0x80000000u ? 16000 : -16000;
            o2 = S->ph[1] < 0x80000000u ? 16000 : -16000;
            x = clamp(dv_bp(&cb, o1 + o2, &S->f[2]) * 2, -32767, 32767);
        } else {
            o1 = sine_i(S->ph[0]);
            o2 = sine_i(S->ph[1]);
            x = o1 + ((o2 * lev2) >> 15);
            x = md == DR_PM_RIM ? x : (x * 3) >> 2;
        }
        x = (x * (ab >> 5)) >> 15;
        ab += db;
        if (nz > 64) {
            x += (((clamp(dv_bp(&cn, z, &S->f[0]) * 2, -32767, 32767) * nz) >> 15) * nlev) >> 15;
            nz = (nz * (int32_t)kn) >> 15;
        }
        if (sn > 64) {
            x += ((((z - x1) >> 1) * sn >> 15) * slap) >> 15;
            sn = (sn * (int32_t)ksn) >> 15;
        }
        x1 = z;
        out[i] += voice_amp(dr_out(x, drv), m, i);
    }
    S->e[0] = nz;
    S->e[1] = sw;
    S->cnt = sn;
    S->x1 = x1;
    v->s[0] = S->q[0];
}
DR_WRAP(dperc)
#define DR_CAT_PERC "DDDDDD"
static const preset_t DPERC_PRESETS[] = {
    /* MODE TUNE DCAY BEND TONE NOIS SLAP DRIV */
    {"PC TOM LO", {DR_PM_TOM, 30, 70, 50, 40, 30, 40, 10}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 10)},
    {"PC TOM HI", {DR_PM_TOM, 80, 55, 45, 40, 25, 50, 10}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 10)},
    {"PC CONGA", {DR_PM_CONGA, 60, 45, 20, 60, 15, 80, 0}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 10)},
    {"PC RIM", {DR_PM_RIM, 50, 30, 0, 70, 20, 90, 30}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 10)},
    {"PC COWBELL", {DR_PM_BELL, 64, 50, 0, 64, 0, 30, 20}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 10)},
    {"PC CLAVE", {DR_PM_CLAVE, 60, 30, 0, 30, 0, 40, 0}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 10)},
};
static const engine_t ENG_DPERC = {
    .name = "PERC", .page_title = {"SHELL", "SKIN"}, .presets = DPERC_PRESETS, .npresets = NELEM(DPERC_PRESETS),
    .note_on = dperc_note_on, .render = dperc_render, DR_MEM, .knob = {P_E0, P_E1, P_E2, P_E3},
    .edit = {{"MODE", F_ENUM, 0, DR_PM_N - 1, 0, N_DR_PMODE, 0}, {"TUNE", F_PCT, 0, 127, 64, 0, 0},
             {"DCAY", F_PCT, 0, 127, 50, 0, 0}, {"BEND", F_PCT, 0, 127, 40, 0, 0}, {"TONE", F_PCT, 0, 127, 50, 0, 0},
             {"NOIS", F_PCT, 0, 127, 20, 0, 0}, {"SLAP", F_PCT, 0, 127, 50, 0, 0}, {"DRIV", F_PCT, 0, 127, 10, 0, 0}},
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

/* ====================================================================== KIT === */
/* four lanes on one track (T4 only: pulhra/sm_layer.c), each a whole drum machine of the above (Tim): L1 .. L4 its machine
 * (KICK SNARE CLAP HAT CYM), its own nine values (the pattern's sm_pattern_t.lane; in the core dr_lp, set by the sequencer's
 * loads and its parameter commands, pulhra/sm_kit.c), lockable each. A lane is a note: 36 + lane (C2 .. D#2; any other note its
 * lane by note % 4): the sequencer's lanes (sm_seq.c), the keyboard's F3 G3 A3 B3 (keys). A hit chokes only its own lane's last
 * one. Velocity scales it */
enum { DR_KM_KICK, DR_KM_SNARE, DR_KM_CLAP, DR_KM_HAT, DR_KM_CYM, DR_KM_PERC, DR_KM_BELL, DR_KM_RIM, DR_KM_N };
static const char *const N_DR_KMACH[DR_KM_N] = {"KICK", "SNARE", "CLAP", "HAT", "CYM", "PERC", "BELL", "RIM"};
static const engine_t *const DR_KENG[DR_KM_N] = {&ENG_DKICK, &ENG_DSNARE, &ENG_DCLAP, &ENG_DHAT, &ENG_DCYM, &ENG_DPERC,
                                                  &ENG_DBELL, &ENG_DRIM};
static int16_t dr_lp[NPART][4][9] __attribute__((section(".pool")));   /* the core's lanes: each part's (only T4 plays a KIT); the audio stream's (the pool: RAM keeps its 4 KiB headroom) */
static int16_t dr_lm[NPART][4][4];                        /* ... each lane's mix: LVL CHO DLY REV (pulhra sm_pattern_t.lmix) */
static int16_t dr_lf[NPART][4][3];                        /* ... each lane's filter: FILT CUT RES (voice.c mf_filter_voice) */
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
    } else {
        dcym_start(t, v, e);
    }
    dr_lane_choke = 0;
}
static void dkit_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    uint32_t L = dr_lane(v->note), mc = dr_kmach(t->p[P_E0 + L]);
    const int16_t *e = dr_lp[(uint32_t)(t - trk) % NPART][L];
    const int16_t *lm = dr_lm[(uint32_t)(t - trk) % NPART][L];
    int32_t *b = dr_kbuf, lv = LEVEL_Q12[clamp(lm[0], 0, 127)], sc = clamp(lm[1], 0, 127) * 258, sd = clamp(lm[2], 0, 127) * 258;
    int32_t sr = clamp(lm[3], 0, 127) * 258;
    vmod_t km = *m;
    uint32_t i;
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
    else
        dcym_run(t, v, b, n, &km, e);
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
    .mem = sizeof(dr_slot_t) * NVOICE, .amp = dr_amp, .oneshot = 1, .poly = 6,
    .knob = {P_E0, P_E1, P_E2, P_E3},
    .edit = {{"L1", F_ENUM, 0, DR_KM_N - 1, DR_KM_KICK, N_DR_KMACH, 0}, {"L2", F_ENUM, 0, DR_KM_N - 1, DR_KM_SNARE, N_DR_KMACH, 0},
             {"L3", F_ENUM, 0, DR_KM_N - 1, DR_KM_HAT, N_DR_KMACH, 0}, {"L4", F_ENUM, 0, DR_KM_N - 1, DR_KM_CLAP, N_DR_KMACH, 0},
             {"S1", F_INT, 0, 5, 0, 0, 0}, {"S2", F_INT, 0, 5, 0, 0, 0}, {"S3", F_INT, 0, 5, 0, 0, 0}, {"S4", F_INT, 0, 5, 0, 0, 0}},
    .xyd = {{XY_E(4), XY_E(5), 0}, {XY_E(6), XY_E(7), 0}},
    .xya = {{40, 40, 0}, {40, 40, 0}},
};
