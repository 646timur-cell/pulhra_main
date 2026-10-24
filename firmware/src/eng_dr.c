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
