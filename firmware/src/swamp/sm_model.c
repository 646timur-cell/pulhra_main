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

/* power-on: 120.0 BPM, swing 50, every step empty, 16 steps; the kit T1 SAW, T2 FM-ST, T3 STRING, T4 BBOX (their first
 * presets) in all 16 patterns */
static void sm_project_init(void)
{
    static const uint8_t KIT[SM_NTRK] = {ENGI_MSAW, ENGI_MFMST, ENGI_STRING, ENGI_MBBOX};
    sm_kit_t kit;
    uint32_t t, pp, m;
    memset(&smp, 0, sizeof smp);
    for (t = 0; t < SM_NTRK; t++) {
        m = eng_rank(KIT[t]);
        sm_sound_from_preset(&kit.snd[t], m, 0);
    }
    for (pp = 0; pp < SM_NPAT; pp++) {
        smp.pat[pp].kit = kit;
        smp.pat[pp].mlen = 0;
    }
    smp.bpm10 = 1200u;
    smp.swing = 50u;
    smp.mode = SM_PM_SEQ;
    smp.next = SM_PAT_NONE;
}
