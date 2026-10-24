/* SPDX-License-Identifier: GPL-3.0-only */
/* PULHRA pages: four knobs and SELECT per page, grouped in the families of the buttons EDIT / GLO / ENV / LFO / SEQ / HOME / ARP /
 * FX / SCL (a second press of a family's button goes to its next page). The screen (sm_draw) reads sm_ui and sm_cur_page().
 * STEP (SEQ tapped) is the one page whose knobs edit a step, not the sound (sm_input.c sm_step_page): K1 NOTE, K2 VEL, K3 LEN,
 * K4 COND, PRESETS the FLAG, OCT- / OCT+ tapped the NUDGE -1 / +1 (sm_oct_single); with no step selected K1..K4 are the
 * track's trig defaults. SELECT on STEP and on HOME's MIXER and CLOCK is the selected track's LEVEL (never the tempo).
 * HOME: MIXER, K1..K4 the LEVEL of T1..T4; CLOCK, K1 BPM (30.0 .. 300.0), K2 SWING (50 .. 75 %).
 * SHIFT + HOME (sm_layer.c sm_shift_combo), in turn: MIDI (K1..K4 the channel of T1..T4, OFF + 1..16) and SETUP (K1 the
 * pattern change mode, K2 the MIDI source); the project saves both (sm_bank.c, version 3). */
#define SM_NOPARAM 0xFFu
#define SM_GLB 0x100u                                     /* a page's parameter id with this bit is a GLOBAL (song.g[id & 0xFF]): the
                                                             FX page's SELECT and the deep FX pages; no locks, the project's */

/* the families of the buttons (a tap of the button: the family's next page) and, after FX, the families of the SHIFT combos (no
 * button of their own: SHIFT + EDIT the VOICE page, SHIFT + FX the deep FX pages DLY / REV / CHORUS in turn, SHIFT + SEQ the
 * EUCLID page, SHIFT + HOME the SETUP pages MIDI / SETUP; sm_layer.c) */
enum { SMFAM_EDIT, SMFAM_GLO, SMFAM_ENV, SMFAM_LFO, SMFAM_SEQ, SMFAM_HOME, SMFAM_ARP, SMFAM_FX, SMFAM_SCL, SMFAM_VOICE, SMFAM_DEEP, SMFAM_EUCL, SMFAM_MENU,
       SMFAM_SETUP, SMFAM_COUNT };
enum { SMH_NONE, SMH_MIXER, SMH_CLOCK, SMH_EUCLID, SMH_MENU, SMH_MIDI, SMH_SETUP };   /* the kind of a page whose knobs are not the sound's (sm_page_t.home) */

typedef struct {
    const char *name;
    uint8_t fam;
    uint16_t id[4];                                       /* the parameters on K1..K4, SM_NOPARAM: none (16 bits: SM_GLB above the
                                                             KIT's lane ids, P_COUNT + 36 > 0x80) */
    uint16_t sel;                                         /* the parameter on SELECT, SM_NOPARAM: none */
    uint8_t home;                                         /* SMH_*: the HOME pages, whose knobs are not the selected track's sound */
} sm_page_t;

static const sm_page_t SM_PAGES[] = {
    {"EDIT 1", SMFAM_EDIT, {P_E0, P_E1, P_E2, P_E3}, P_E8},
    {"EDIT 2", SMFAM_EDIT, {P_E4, P_E5, P_E6, P_E7}, P_E8},
    {"AMP", SMFAM_GLO, {P_ATK, P_DEC, P_SUS, P_REL}, P_LEVEL},   /* SELECT: track volume (ESYNC removed) */
    {"MIX", SMFAM_GLO, {P_LEVEL, P_PAN, P_DIST, P_REV}, SM_NOPARAM},
    {"FILTER", SMFAM_ENV, {P_FRES, P_ED_FLT, P_LD_FLT, P_FLT}, P_FCUT},   /* SELECT: CUTOFF, K1 RES, K2 ENV>FLT, K3 LFO>FLT, K4 TYPE */
    {"F.ENV", SMFAM_ENV, {P_FATK, P_FDEC, P_ED_FLT, SM_NOPARAM}, SM_NOPARAM},
    {"LFO", SMFAM_LFO, {P_LWAVE, P_LPHASE, P_LSYNC, P_LTRIG}, P_LRATE},   /* SELECT: RATE, K1 WAVE, K2 PHASE, K3 SYNC, K4 TRIG */
    {"LFO DEST", SMFAM_LFO, {P_LD_PIT, P_LD_FLT, P_LD_SHP, P_LD_AMP}, P_LFADE},
    {"STEP", SMFAM_SEQ, {SM_NOPARAM, SM_NOPARAM, SM_NOPARAM, SM_NOPARAM}, SM_NOPARAM, SMH_NONE},
    {"MIXER", SMFAM_HOME, {P_LEVEL, P_LEVEL, P_LEVEL, P_LEVEL}, P_LEVEL, SMH_MIXER},   /* K1..K4: the LEVEL of T1..T4, SELECT: selected track's volume */
    {"CLOCK", SMFAM_HOME, {SM_NOPARAM, SM_NOPARAM, SM_NOPARAM, SM_NOPARAM}, SM_NOPARAM, SMH_CLOCK},   /* K1 BPM, K2 SWING */
    {"ARP", SMFAM_ARP, {P_AGATE, P_AOCT, P_AMODE, P_AORDER}, P_ARATE},                  /* SELECT: RATE, K1 GATE, K2 OCT, K3 MODE, K4 ORDER */
    {"ARP 2", SMFAM_ARP, {P_ASWING, P_APROB, P_AHOLD, SM_NOPARAM}, SM_NOPARAM},         /* K3 is LATCH (params.c names it HOLD) */
    /* the FX button's family (a tap goes to the next page: FX, DIST, INSERT). FX: the four sends, SELECT the delay's MIX (the
     * core has no separate dry level: the dry is the track's LEVEL); DIST: TYPE DRIVE (the send P_DIST is the drive) TONE OUT,
     * SELECT LOW CUT; INSERT: TYPE (OFF CRUSH PHASR FLANG CHOR: the drives are DIST's), A B C, SELECT MIX */
    {"FX", SMFAM_FX, {P_DIST, P_CHOR, P_DLY, P_REV}, SM_GLB | G_DMIX},
    {"DIST", SMFAM_FX, {P_DTYPE, P_DIST, P_DTONE, P_DOUT}, P_DLCUT},
    {"INSERT", SMFAM_FX, {P_ITYPE, P_IA, P_IB, P_IC}, P_IMIX},
    /* the pages of the SHIFT combos (SHIFT + EDIT, FX, SEQ, HOME). VOICE: the sound's voicing (out of EDIT's cycle); DLY / REV /
     * CHORUS: the deep global effect pages (song.g, the project's: sm_bank.c; no locks), in turn on SHIFT + FX; EUCLID: K1 STEPS,
     * K2 HITS, K3 ROT, OCT+ generates the trigs into the selected track (sm_input.c sm_eu_gen); MENU: a stub until the menu */
    {"VOICE", SMFAM_VOICE, {P_VOICE, P_GLIDE, P_DETUNE, P_PRIO}, SM_NOPARAM},
    {"DLY", SMFAM_DEEP, {SM_GLB | G_DTIME, SM_GLB | G_DFDBK, SM_GLB | G_DCOLOR, SM_GLB | G_DMIX}, SM_NOPARAM},
    {"REV", SMFAM_DEEP, {SM_GLB | G_RTYPE, SM_GLB | G_RSIZE, SM_GLB | G_RTONE, SM_GLB | G_RPRE}, SM_NOPARAM},   /* K3 TONE (the damping
                                                                                                   and a high shelf, Tim), K4 PRE-DELAY */
    {"CHORUS", SMFAM_DEEP, {SM_GLB | G_CRATE, SM_GLB | G_CDEPTH, SM_NOPARAM, SM_NOPARAM}, SM_NOPARAM},
    /* COMP (Tim): SELECT the threshold, K1 RATIO, K2 ATTACK, K3 RELEASE, K4 SC: OFF the master's own level, KICK the kick's (the
     * gain on T1..T3), 1/4 each beat (the same: a pump) */
    {"COMP", SMFAM_DEEP, {SM_GLB | G_CRAT, SM_GLB | G_CATK, SM_GLB | G_CREL, SM_GLB | G_CSC}, SM_GLB | G_CTHR},
    {"EUCLID", SMFAM_EUCL, {SM_NOPARAM, SM_NOPARAM, SM_NOPARAM, SM_NOPARAM}, SM_NOPARAM, SMH_EUCLID},
    {"SCL", SMFAM_SCL, {P_ROOT, P_SCALE, P_QUANT, P_TRANS}, SM_NOPARAM},
    {"MICRO", SMFAM_SCL, {P_FM4_LEVEL, P_MAMT, P_ROOT, SM_NOPARAM}, SM_NOPARAM},
    {"MIDI", SMFAM_SETUP, {SM_NOPARAM, SM_NOPARAM, SM_NOPARAM, SM_NOPARAM}, SM_NOPARAM, SMH_MIDI},
    {"SETUP", SMFAM_SETUP, {SM_NOPARAM, SM_NOPARAM, SM_NOPARAM, SM_NOPARAM}, SM_NOPARAM, SMH_SETUP},
};

/* what the screen and the keys share (the main loop's alone; sm_draw only reads): the page, the selected track, the step
 * held on a white key (0..31, 0xFF: none; its knobs show and edit the step's locks), the bank of 16 steps, the step
 * selected last (a key held past SM_HOLD_US or turned on; 0xFF none; the STEP page edits it when no key is down), the keyboard
 * mode (the keys play the selected track: sm_input.c) and its octave -3 .. +3 */
static struct { uint8_t page, trk, held_step, bank, sel_step, kbd; int8_t oct; } sm_ui;
enum { SML_NONE, SML_EDIT, SML_GLO, SML_SEQ, SML_FX };
static uint8_t sm_layer;                                  /* the layer open while its button is held (sm_layer.c; the screen and the LEDs read it) */
static volatile uint8_t sm_rec;                           /* recording (the REC key): grid record (the trig keys edit the steps only
                                                             then), knob turns while playing lock the playing trig, live record */
static volatile uint8_t sm_live;                          /* live record (REC + PLAY): the keyboard and MIDI notes record. A REC
                                                             press while live exits to ARM (grid record only), the transport keeps
                                                             running; the header shows REC while live, ARM while armed */
static uint8_t sm_dirty;                                  /* the screen must redraw (sm_draw clears it) */

static uint32_t sm_page_count(void) { return sizeof SM_PAGES / sizeof SM_PAGES[0]; }
static const sm_page_t *sm_cur_page(void) { return &SM_PAGES[sm_ui.page % sm_page_count()]; }
/* a layer with its map over the knobs (EDIT, GLO, SEQ) */
static uint32_t sm_map_open(void) { return sm_layer != 0; }
/* the button whose LED a family lights: its own, or (the SHIFT combos' families) the button of the combo */
static uint32_t sm_fam_btn(uint32_t fam)
{
    return fam == SMFAM_EDIT || fam == SMFAM_VOICE ? B_EDIT : fam == SMFAM_GLO ? B_GLO : fam == SMFAM_SEQ || fam == SMFAM_EUCL ? B_SEQ :
           fam == SMFAM_HOME || fam == SMFAM_MENU || fam == SMFAM_SETUP ? B_HOME : fam == SMFAM_ARP ? B_ARP : fam == SMFAM_FX || fam == SMFAM_DEEP ? B_FX :
           fam == SMFAM_LFO ? B_LFO : fam == SMFAM_SCL ? B_SCL : B_ENV;
}

/* a family's button: its first page, or, when the page is of that family already, the next one (the last wraps) */
static void sm_page_press(uint32_t fam)
{
    uint32_t n = sm_page_count(), cur = sm_ui.page % n, i, first = n, next = n;
    for (i = 0; i < n; i++) {
        if (SM_PAGES[i].fam != fam)
            continue;
        if (first == n)
            first = i;
        if (i > cur && next == n && SM_PAGES[cur].fam == fam)
            next = i;
    }
    if (first == n)
        return;
    sm_ui.page = (uint8_t)(SM_PAGES[cur].fam == fam ? (next != n ? next : first) : first);
    if (fam == SMFAM_SEQ)                                 /* SEQ tapped: no step selected, STEP shows the track's trig defaults */
        sm_ui.sel_step = 0xFFu;
    sm_dirty = 1;
}

/* SHIFT + HOME: the SETUP family's next page (MIDI / SETUP), wraps */
void sm_page_open_setup(void)
{
    uint32_t n = sm_page_count(), cur = sm_ui.page % n, i, first = n, next = n;
    for (i = 0; i < n; i++) {
        if (SM_PAGES[i].fam != SMFAM_SETUP)
            continue;
        if (first == n)
            first = i;
        if (i > cur && next == n && SM_PAGES[cur].fam == SMFAM_SETUP)
            next = i;
    }
    if (first == n)
        return;
    sm_ui.page = (uint8_t)(SM_PAGES[cur].fam == SMFAM_SETUP ? (next != n ? next : first) : first);
    sm_dirty = 1;
}

/* the parameter on knob k (0..3; 4: SELECT) of the page shown. FILTER (SELECT CUTOFF, K1 RES, K2 ENV>FLT, K3 LFO>FLT, K4
 * TYPE) with FILT HYB (5) on the selected track's sound: SELECT is HYBRID's BASE (the high-pass) and K1 its RES (CUT and
 * RES do nothing then: voice.c mf_filter_voice; ENV and the LFO move BASE as they move CUT), and F.ENV's free K4 its WIDTH
 * (the low-pass above BASE) */
static uint32_t sm_page_param(uint32_t k)
{
    const sm_page_t *pg = sm_cur_page();
    uint32_t id = k < 4u ? pg->id[k] : pg->sel;
    if (pg->fam == SMFAM_EDIT && sm_trk_kit(sm_ui.trk % SM_NTRK, &smp.pat[smp.cur % SM_NPAT])) {
        /* the KIT (Tim): EDIT 1 / EDIT 2 the lane in edit's machine, its eight values; SELECT on EDIT 1 that machine, on EDIT 2
         * its ninth value */
        uint32_t L = sm_kit_lane % 4u, two = pg->id[0] == P_E4;
        if (k < 4u)
            return SM_LP(L, (two ? 4u : 0u) + k);
        return two ? SM_LP(L, 8u) : P_E0 + L;
    }
    if (sm_trk_kit(sm_ui.trk % SM_NTRK, &smp.pat[smp.cur % SM_NPAT])) {
        /* the KIT (Tim): each lane its own LVL and sends, the lane in edit's: AMP's K1..K4 LVL CHO DLY REV (the ADSR does nothing
         * on a hit), the sends of FX / MIX its own (SELECT on AMP, MIX's K1: the track's LEVEL, all the lanes) */
        uint32_t L = sm_kit_lane % 4u;
        if (pg->id[0] == P_ATK && k < 4u)
            return SM_LM(L, k);
        if (id == P_CHOR)
            return SM_LM(L, 1u);
        if (id == P_DLY)
            return SM_LM(L, 2u);
        if (id == P_REV)
            return SM_LM(L, 3u);
    }
    if (smp.pat[smp.cur % SM_NPAT].kit.snd[sm_ui.trk % SM_NTRK].p[P_FLT] == 5 && pg->fam == SMFAM_ENV) {
        if (id == P_FCUT)
            return P_HBASE;
        if (id == P_FRES)
            return P_HRES;
        if (pg->id[0] == P_FATK && k == 3u)
            return P_HWIDTH;
    }
    return id;
}

/* the ARP button's pages (ARP, ARP 2): the first page of the family, and the page after it (the tests') */
static uint32_t __attribute__((unused)) sm_arp_page(uint32_t second)
{
    uint32_t i, n = sm_page_count(), seen = 0;
    for (i = 0; i < n; i++)
        if (SM_PAGES[i].fam == SMFAM_ARP && seen++ == (second ? 1u : 0u))
            return i;
    return 0;
}

static int sm_page_is_step(void) { return sm_cur_page()->fam == SMFAM_SEQ; }
/* the selected track's pages of 16 steps (1 .. 4: 64 steps) */
static uint32_t sm_npages(void) { return ((uint32_t)clamp(smp.pat[smp.cur % SM_NPAT].kit.snd[sm_ui.trk % SM_NTRK].len, 1, SM_NSTEP) + 15u) / 16u; }
static uint32_t sm_page_home(void) { return sm_cur_page()->home; }   /* SMH_*: 0 not a HOME page */

/* the STEP page's FLAG list (the PRESETS knob, round): the flags byte of each place, its name */
#define SM_NFLAG 12u
static const uint8_t SM_FLAGV[SM_NFLAG] = {
    0, SMF_ACCENT, SMF_SLIDE, SMF_ACCENT | SMF_SLIDE, 1u << 4, 2u << 4, 3u << 4,
    SMF_ACCENT | (1u << 4), SMF_ACCENT | (2u << 4), SMF_ACCENT | (3u << 4), SMF_REST, SMF_PSLIDE,
};
/* (at most 8 characters: the STEP page line STEP 05 ACC+RTX3 N+3 is 20, the line's width) */
static const char *const SM_FLAGN[SM_NFLAG] = {"-", "ACCENT", "SLIDE", "ACC+SLD", "RATCH X2", "RATCH X3", "RATCH X4", "ACC+RTX2",
                                               "ACC+RTX3", "ACC+RTX4", "REST", "P.SLIDE"};
static uint32_t sm_flag_place(uint32_t f)                 /* a flags byte's place in the list (one not in it: 0) */
{
    uint32_t i;
    for (i = 0; i < SM_NFLAG; i++)
        if (SM_FLAGV[i] == f)
            return i;
    return 0;
}

/* COND (sm_model.h): the K4 walk 1 % .. 99 %, 100 % (place 99), then the conditions; the byte of place q, the place of
 * byte v; A:B's byte; the name ("100%", "!1ST", "3:7"; b: 8 bytes) */
#define SM_COND_PLACES SM_COND_END
static uint32_t sm_cond_of_place(uint32_t q) { return q < 99u ? q + 1u : q == 99u ? 0u : q; }
static uint32_t sm_cond_place(uint32_t v) { return !v ? 99u : v < SM_COND_1ST ? v - 1u : v < SM_COND_END ? v : 99u; }
static uint32_t sm_cond_ab(uint32_t a, uint32_t b)
{
    b = b < 2u ? 2u : b > 8u ? 8u : b;
    a = a < 1u ? 1u : a > b ? b : a;
    return SM_COND_AB0 + (b * (b - 1u)) / 2u - 1u + (a - 1u);   /* (B 2 starts at 0, B 3 at 2, B 4 at 5, ..) */
}
static void sm_cond_name(uint32_t v, char *b)
{
    static const char *const N[6] = {"1ST", "!1ST", "PRE", "!PRE", "FILL", "!FILL"};
    uint32_t i = 0, k;
    if (v < SM_COND_1ST) {
        v = v ? v : 100u;
        if (v >= 100u)
            b[i++] = '1';
        if (v >= 10u)
            b[i++] = (char)('0' + v / 10u % 10u);
        b[i++] = (char)('0' + v % 10u);
        b[i++] = '%';
        b[i] = 0;
    } else if (v < SM_COND_AB0) {
        for (k = 0; N[v - SM_COND_1ST][k]; k++)
            b[k] = N[v - SM_COND_1ST][k];
        b[k] = 0;
    } else {
        uint32_t bb, a, j = (v < SM_COND_END ? v : SM_COND_AB0) - SM_COND_AB0;
        for (bb = 2u; j >= bb; bb++)
            j -= bb;
        a = j + 1u;
        b[0] = (char)('0' + a);
        b[1] = ':';
        b[2] = (char)('0' + bb);
        b[3] = 0;
    }
}
