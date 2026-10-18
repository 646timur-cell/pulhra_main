/* SPDX-License-Identifier: GPL-3.0-only */
/* SWAMP MACHINE project model (sm_model.h): the one project, the power-on state, a sound from a machine's preset. */

static sm_project_t smp;

/* machine (0..SM_NMACHINE-1) -> the engine's ENGI_* */
static uint32_t sm_engine_of(uint32_t machine) { return eng_vis(machine); }

/* the descriptor of parameter id on engine e, 0 when there is none: P_E8 of a machine without a ninth value (param_desc_of
 * would read edit[8], past the array), an id out of range */
static const param_desc_t *sm_desc_safe(uint32_t e, uint32_t id)
{
    if (id >= P_COUNT || (id == P_E8 && !ENGINES[e % NENGINES]->e8.label))
        return 0;
    return param_desc_of(e % NENGINES, id);
}

/* the descriptor of parameter id on the sound s: sm_desc_safe, and the INSERT's three values A B C as its TYPE names them
 * (params.c ins_desc: DRIVE .. for the swept ones, BITS RATE LPF for CRUSH; the ranges are 0..127 in every type) */
static const param_desc_t *sm_desc_trk(const sm_sound_t *s, uint32_t id)
{
    if (id >= P_IA && id <= P_IC)
        return ins_desc(s->p[P_ITYPE], id - P_IA);
    return sm_desc_safe(sm_engine_of(s->machine), id);
}

/* the INSERT TYPEs a track offers, in the order of the knob: OFF CRUSH PHASR FLANG CHOR (fx.c IT_*, the values stored). The
 * drives (SOFT HARD FOLD FUZZ) are DIST's own on a track; the master inserts of stage 4 offer all of them */
static const uint8_t SM_ITYPES[5] = {IT_OFF, IT_CRUSH, IT_PHASER, IT_FLANGER, IT_CHORUS};

/* value v of parameter id (descriptor d) turned by s detents, in its range. INSERT TYPE walks SM_ITYPES only (a value not in the
 * list, from an older kit, turns from the first listed one above it) */
static int32_t sm_val_turn(const param_desc_t *d, uint32_t id, int32_t v, int32_t s)
{
    if (id == P_ITYPE) {
        int32_t i = 0, n = (int32_t)(sizeof SM_ITYPES / sizeof SM_ITYPES[0]);
        while (i < n - 1 && (int32_t)SM_ITYPES[i] < v)
            i++;
        return SM_ITYPES[clamp(i + s, 0, n - 1)];
    }
    return clamp(param_turn(d, clamp(v, d->min, d->max), s), d->min, d->max);
}

/* a machine's preset as the sound ui.c apply_preset_to + set_engine_of leave on a track (the sound only, nothing of the
 * track's own, no undo, no UI): the parameter defaults, the engine's eight values and its ninth, the ADSR, F.ENV, the
 * voice mode, the sends */
static void sm_sound_from_preset(sm_sound_t *s, uint32_t machine, uint32_t preset)
{
    static const uint8_t FX_DEF[4] = {0, 24, 28, 36};
    const engine_t *e;
    const preset_t *pr;
    uint32_t i;
    machine %= SM_NMACHINE;
    e = ENGINES[sm_engine_of(machine)];
    memset(s, 0, sizeof *s);
    s->machine = (uint8_t)machine;
    s->len = 16u;
    for (i = 0; i < P_E0; i++)
        s->p[i] = (int8_t)TP[i].def;
    s->dnote = (uint8_t)(48 + clamp(s->p[P_ROOT], 0, 11)); /* the trig defaults: C3 + the ROOT, VEL 100, LEN TRK, 100 % */
    s->dvel = 100u;
    s->dlen = 0;
    s->dcond = 0;
    if (!e->npresets)
        return;
    preset %= e->npresets;
    pr = &e->presets[preset];
    s->preset = (uint8_t)preset;
    for (i = 0; i < 8u; i++)
        s->p[P_E0 + i] = (int8_t)pr->e[i];
    s->p[P_E8] = (int8_t)(e->e8.label ? e->e8.def : 0);       /* the ninth: the default; a preset may set it (mf_preset_e8) */
    if (e->presets == MSAW_PRESETS && preset == 6u)           /* SAW SYNC LEAD */
        s->p[P_E8] = 80;
    s->p[P_ATK] = (int8_t)pr->env[0];
    s->p[P_DEC] = (int8_t)pr->env[1];
    s->p[P_SUS] = (int8_t)pr->env[2];
    s->p[P_REL] = (int8_t)pr->env[3];
    s->p[P_ED_FLT] = pr->fenv;
    if (pr->fenv) {                                           /* F.ENV: the sweep is the ADSR's attack and decay */
        s->p[P_FATK] = (int8_t)pr->env[0];
        s->p[P_FDEC] = (int8_t)pr->env[1];
    }
    s->p[P_VOICE] = (int8_t)(pr->mono ? V_LEGATO : V_POLY);
    for (i = 0; i < 4u; i++)
        s->p[P_DIST + i] = (int8_t)(pr->fx[i] ? pr->fx[i] - 1 : FX_DEF[i]);
}

/* step s as it plays on a track with sound d: its inherited fields (SM_INH) the track's defaults. The audio stream's at a
 * fire (each field a byte, read once into *o); the main loop's for the screen and the STEP page's edits */
static void sm_step_eff(const sm_step_t *s, const sm_sound_t *d, sm_step_t *o)
{
    *o = *s;
    if (o->note[0] == SM_INH)
        o->note[0] = d->dnote;
    if (o->vel == SM_INH)
        o->vel = d->dvel;
    if (o->len == SM_INH)
        o->len = d->dlen;
    if (o->cond == SM_INH)
        o->cond = d->dcond;
}

/* the slot in RAM of pattern id g (0..39), SM_PAT_NONE when its bank is in neither half; the audio stream's too (a chain's link:
 * each hbank byte read once) */
static uint32_t sm_slot_of(uint32_t g)
{
    uint32_t h, b = g / SM_BPAT;
    if (g >= SM_NGPAT)
        return SM_PAT_NONE;
    for (h = 0; h < SM_NHALF; h++)
        if (*(volatile uint8_t *)&smp.hbank[h] == b)
            return h * SM_BPAT + g % SM_BPAT;
    return SM_PAT_NONE;
}
/* the pattern id of RAM slot s (SM_PAT_NONE: its half holds no bank) */
static uint32_t sm_gid_of(uint32_t s)
{
    uint32_t b = smp.hbank[(s % SM_NPAT) / SM_BPAT];
    return b < SM_NBANK ? b * SM_BPAT + s % SM_BPAT : SM_PAT_NONE;
}

/* the patterns that hold something (sm_bank.c, the GLO grid): bit per place, per bank. A bit is set by a save into the place
 * (GLO), and when its bank is written to the flash for every pattern with a trig; kept in the bank's flash header */
static uint8_t sm_used[SM_NBANK];

/* the power-on kit: T1 SAW, T2 FM-ST, T3 STRING, T4 BBOX (their first presets) */
static void sm_kit_default(sm_kit_t *kit)
{
    static const uint8_t KIT[SM_NTRK] = {ENGI_MSAW, ENGI_MFMST, ENGI_STRING, ENGI_MBBOX};
    uint32_t t;
    for (t = 0; t < SM_NTRK; t++)
        sm_sound_from_preset(&kit->snd[t], eng_rank(KIT[t]), 0);
}
/* an empty pattern with kit k: no trig, no lock, master length AUTO */
static void sm_pat_blank(sm_pattern_t *p, const sm_kit_t *k)
{
    memset(p, 0, sizeof *p);
    p->kit = *k;
}

/* power-on: 120.0 BPM, swing 50, every step empty, 16 steps; the default kit in every pattern; banks A and B in RAM, the
 * current pattern A01 (the only one used) */
static void sm_project_init(void)
{
    sm_kit_t kit;
    uint32_t pp;
    memset(&smp, 0, sizeof smp);
    sm_kit_default(&kit);
    for (pp = 0; pp < SM_NPAT; pp++)
        sm_pat_blank(&smp.pat[pp], &kit);
    smp.hbank[0] = 0;
    smp.hbank[1] = 1;
    memset(sm_used, 0, sizeof sm_used);
    sm_used[0] = 1u;
    smp.bpm10 = 1200u;
    smp.swing = 50u;
    smp.mode = SM_PM_SEQ;
    smp.next = SM_PAT_NONE;
}
