/* SPDX-License-Identifier: GPL-3.0-only */
/* PULHRA project model (sm_model.h): the one project, the power-on state, a sound from a machine's preset. */

static sm_project_t smp;

/* machine (0..SM_NMACHINE-1) -> the engine's ENGI_* */
static uint32_t sm_engine_of(uint32_t machine) { return eng_vis(machine); }

/* KIT (eng_dr.c): the DRUM page's sixth machine, on T4 only (the EDIT layer loads it nowhere else); on it the track's steps
 * are four lanes: a step's note[k] the hit of lane k (0 none, SM_INH the track's velocity, 1..127 its own), n 1 while
 * any lane hits (or a lock trig) */
#define SM_KIT_MACH (MF_PAGE_4 + 5u)
/* the DRUM places (ENGINE_ORDER): KICK SNARE CLAP HAT CYM, the KIT (+5, not on the page), PERC (+6); a lane machine (DR_KM_*)
 * <-> its place */
static uint32_t sm_kmach_place(uint32_t km) { return MF_PAGE_4 + (km < 5u ? km : km + 1u); }
static uint32_t sm_place_kmach(uint32_t m) { uint32_t o = m - MF_PAGE_4; return o < 5u ? o : o - 1u; }
#define SM_KIT_TRK 3u
#define SM_NLANE 4u
static uint8_t sm_kit_lane;                               /* the KIT lane in edit (the trig keys, the strip): ALGORITHM picks it */
/* machine m may go on track t: the DRUM page's (MF_PAGE_4 on: KICK .. KIT) on T4 alone (Tim: the drums are T4's) */
static int sm_mach_ok(uint32_t m, uint32_t t) { return m < MF_PAGE_4 || t % SM_NTRK == SM_KIT_TRK; }
static int sm_snd_kit(const sm_sound_t *d);
/* the machine the EDIT layer shows as track t's (inverted, lit): the KIT's lane in edit's machine (KICK .. CYM), else its own */
static uint32_t sm_mach_shown(const sm_pattern_t *pt, uint32_t t);
static int sm_snd_kit(const sm_sound_t *d) { return d->machine == SM_KIT_MACH; }
/* track t of pattern pt plays the KIT */
static int sm_trk_kit(uint32_t t, const sm_pattern_t *pt) { return t == SM_KIT_TRK && sm_snd_kit(&pt->kit.snd[t]); }
/* step s of track t in pattern pt as the trig keys, their LEDs and the strip show it: on the KIT, the lane in edit's (a trig
 * where that lane hits; a lock trig as it is) */
static const sm_step_t *sm_step_view(const sm_pattern_t *pt, uint32_t t, uint32_t s, sm_step_t *tmp)
{
    const sm_step_t *st = &pt->step[t % SM_NTRK][s % SM_NSTEP];
    if (!sm_trk_kit(t, pt) || (st->flags & SMF_REST))
        return st;
    *tmp = *st;
    tmp->n = (uint8_t)(st->n && st->note[sm_kit_lane % 4u] != 0);
    return tmp;
}
static uint32_t sm_mach_shown(const sm_pattern_t *pt, uint32_t t)
{
    const sm_sound_t *s = &pt->kit.snd[t % SM_NTRK];
    return sm_trk_kit(t % SM_NTRK, pt) ? sm_kmach_place(dr_kmach(s->p[P_E0 + sm_kit_lane % 4u])) : s->machine;
}
/* a KIT step's lanes that hit (bit per lane) */
static uint32_t sm_kit_mask(const sm_step_t *s)
{
    uint32_t k, m = 0;
    for (k = 0; k < SM_NLANE; k++)
        m |= (s->note[k] != 0) << k;
    return m;
}

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
/* the KIT's lanes as parameters (Tim: a whole machine a lane, every value lockable): ids past P_COUNT, lane L's value k
 * (0..8: E1 .. E8, the ninth) SM_LP(L, k); their value the pattern's (sm_pattern_t.lane), the core's dr_lp (eng_dr.c) */
#define SM_LP0 ((uint32_t)P_COUNT)
#define SM_LM0 (SM_LP0 + 36u)                             /* the lanes' mix (Tim): lane L's LVL CHO DLY REV, SM_LM(L, j) */
#define SM_LF0 (SM_LM0 + 16u)                             /* the lanes' filter (Tim): lane L's FILT CUT RES, SM_LF(L, j) */
#define SM_LC0 (SM_LF0 + 12u)                             /* a lane's trig condition (Tim): only a lock (a KIT step's lane L,
                                                             SM_LC(L)); no base, no core value (sm_desc_core: none) */
#define SM_LC(L) (SM_LC0 + (uint32_t)(L))
#define SM_NPID (SM_LC0 + 4u)                            /* every id a lock, a command, a push may carry */
#define SM_LF(L, j) (SM_LF0 + (uint32_t)(L) * 3u + (uint32_t)(j))
#define SM_LP(L, k) (SM_LP0 + (uint32_t)(L) * 9u + (uint32_t)(k))
#define SM_LM(L, j) (SM_LM0 + (uint32_t)(L) * 4u + (uint32_t)(j))
_Static_assert(SM_NPID < 0xFFu, "a parameter id is a byte (0xFF: a removed lock)");
static int sm_is_lp(uint32_t id) { return id >= SM_LP0 && id < SM_LM0; }
static int sm_is_lm(uint32_t id) { return id >= SM_LM0 && id < SM_LF0; }
static int sm_is_lf(uint32_t id) { return id >= SM_LF0 && id < SM_LC0; }
static const char *const SM_LFN[5] = {"OFF", "LP", "BP", "HP", "NOTCH"};
static const param_desc_t SM_LFD[3] = {                   /* (the track's FILT CUT RES, a lane's; no HYB) */
    {"FILT", F_ENUM, 0, 4, 0, SM_LFN, 0}, {"CUT", F_CUTOFF, 0, 127, 127, 0, 0}, {"RES", F_PCT, 0, 127, 0, 0, 0},
};
static const param_desc_t SM_LMD[4] = {                   /* (the track's LEVEL and sends, a lane's) */
    {"LVL", F_DB, 0, 127, 112, 0, 0}, {"CHO", F_PCT, 0, 127, 0, 0, 0}, {"DLY", F_PCT, 0, 127, 0, 0, 0}, {"REV", F_PCT, 0, 127, 0, 0, 0},
};
/* the value of id on track t of pattern pt: a lane's, else the sound's */
static int32_t sm_pget(const sm_pattern_t *pt, uint32_t t, uint32_t id)
{
    if (sm_is_lp(id))
        return pt->lane[(id - SM_LP0) / 9u][(id - SM_LP0) % 9u];
    if (sm_is_lm(id))
        return pt->lmix[(id - SM_LM0) / 4u][(id - SM_LM0) % 4u];
    if (sm_is_lf(id))
        return pt->lflt[(id - SM_LF0) / 3u][(id - SM_LF0) % 3u];
    return id < P_COUNT ? pt->kit.snd[t % SM_NTRK].p[id] : 0;
}
static void sm_pset(sm_pattern_t *pt, uint32_t t, uint32_t id, int32_t v)
{
    if (sm_is_lp(id))
        pt->lane[(id - SM_LP0) / 9u][(id - SM_LP0) % 9u] = (int8_t)v;
    else if (sm_is_lm(id))
        pt->lmix[(id - SM_LM0) / 4u][(id - SM_LM0) % 4u] = (int8_t)v;
    else if (sm_is_lf(id))
        pt->lflt[(id - SM_LF0) / 3u][(id - SM_LF0) % 3u] = (int8_t)v;
    else if (id < P_COUNT)
        pt->kit.snd[t % SM_NTRK].p[id] = (int8_t)v;
}
/* the descriptor of a lane's value on a KIT whose lane machines are p[P_E0 .. P_E3] (the model's or the core's) */
static const param_desc_t *sm_desc_lp16(int32_t mach, uint32_t id) { return dr_lane_desc(dr_kmach(mach), (id - SM_LP0) % 9u); }
static const param_desc_t *sm_desc_trk(const sm_sound_t *s, uint32_t id)
{
    if (sm_is_lp(id))
        return sm_snd_kit(s) ? sm_desc_lp16(s->p[P_E0 + (id - SM_LP0) / 9u], id) : 0;
    if (sm_is_lm(id))
        return sm_snd_kit(s) ? &SM_LMD[(id - SM_LM0) % 4u] : 0;
    if (sm_is_lf(id))
        return sm_snd_kit(s) ? &SM_LFD[(id - SM_LF0) % 3u] : 0;
    if (id >= P_IA && id <= P_IC)
        return ins_desc(s->p[P_ITYPE], id - P_IA);
    return sm_desc_safe(sm_engine_of(s->machine), id);
}
/* the core's: track t (trk[], the engine on its way) */
static const param_desc_t *sm_desc_core(uint32_t t, uint32_t id)
{
    if (sm_is_lp(id))
        return trk[t % SM_NTRK].eng_req == ENGI_DKIT ? sm_desc_lp16(trk[t % SM_NTRK].p[P_E0 + (id - SM_LP0) / 9u], id) : 0;
    if (sm_is_lm(id))
        return trk[t % SM_NTRK].eng_req == ENGI_DKIT ? &SM_LMD[(id - SM_LM0) % 4u] : 0;
    if (sm_is_lf(id))
        return trk[t % SM_NTRK].eng_req == ENGI_DKIT ? &SM_LFD[(id - SM_LF0) % 3u] : 0;
    return sm_desc_safe(eng_idx(trk[t % SM_NTRK].eng_req), id);
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
/* ENS on PULHRA: semitones only (Tim). Its factory sounds' just ratios (25 .. 28: 6/5 5/4 4/3 3/2) become the nearest
 * semitones (+3 +4 +5 +7), a stored one too (sm_bank.c) */
static void sm_ens_semi(sm_sound_t *s)
{
    static const int8_t JI_SEMI[4] = {15, 16, 17, 19};
    uint32_t i;
    if (sm_engine_of(s->machine) != ENGI_MENS)
        return;
    for (i = 0; i < 3u; i++)
        if (s->p[P_E0 + i] > 24)
            s->p[P_E0 + i] = JI_SEMI[(s->p[P_E0 + i] - 25) & 3];
}
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
    sm_ens_semi(s);
}

/* step s as it plays on a track with sound d: its inherited fields (SM_INH) the track's defaults. The audio stream's at a
 * fire (each field a byte, read once into *o); the main loop's for the screen and the STEP page's edits */
static void sm_step_eff(const sm_step_t *s, const sm_sound_t *d, sm_step_t *o)
{
    *o = *s;
    if (o->note[0] == SM_INH && !sm_snd_kit(d))             /* (KIT: note[] are the lanes' hits, nothing inherited) */
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
/* the KIT's four lanes from T4's sound: each its machine (L1 .. L4) at its factory sound (S1 .. S4); a T4 of another machine:
 * KIT's own defaults (KICK SNARE HAT CLAP, their first sounds) */
static void sm_lmix_default(sm_pattern_t *p)             /* the lanes' mix: LVL at the track's default, no sends */
{
    uint32_t L, j;
    for (L = 0; L < 4u; L++)
        for (j = 0; j < 4u; j++)
            p->lmix[L][j] = (int8_t)SM_LMD[j].def;
}
static void sm_lflt_default(sm_pattern_t *p)             /* the lanes' filters: off, open */
{
    uint32_t L, j;
    for (L = 0; L < 4u; L++)
        for (j = 0; j < 3u; j++)
            p->lflt[L][j] = (int8_t)SM_LFD[j].def;
}
static void sm_lanes_default(sm_pattern_t *p)
{
    const sm_sound_t *s = &p->kit.snd[SM_KIT_TRK];
    uint32_t L;
    for (L = 0; L < 4u; L++)
        if (sm_snd_kit(s))
            dr_lane_init(p->lane[L], dr_kmach(s->p[P_E0 + L]), (uint32_t)clamp(s->p[P_E4 + L], 0, 15));
        else
            dr_lane_init(p->lane[L], (uint32_t)ENG_DKIT.edit[L].def, 0);
    sm_lmix_default(p);
    sm_lflt_default(p);
}
static void sm_pat_blank(sm_pattern_t *p, const sm_kit_t *k)
{
    memset(p, 0, sizeof *p);
    p->kit = *k;
    sm_lanes_default(p);
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
