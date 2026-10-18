/* SPDX-License-Identifier: GPL-3.0-only */
/* SWAMP MACHINE pages: four knobs and SELECT per page, grouped in the families of the buttons EDIT / GLO / ENV / SEQ
 * (a second press of a family's button goes to its next page). The screen (sm_draw) reads sm_ui and sm_cur_page().
 * STEP (SEQ tapped) is the one page whose knobs edit a step, not the sound (sm_input.c sm_step_page): K1 NOTE, K2 VEL,
 * K3 LEN, K4 COND, SELECT NUDGE, PRESETS the FLAG; with no step selected (a SEQ tap drops the selection) K1..K4 are the
 * track's trig defaults and SELECT the track's own SWING. HOME (a family: a tap goes to its next page; sm_layer.c): MIXER, K1..K4 the
 * LEVEL of T1..T4, and CLOCK, K1 BPM (30.0 .. 300.0), K2 SWING (50 .. 75 %); SELECT is the tempo on both */
#define SM_NOPARAM 0xFFu
#define SM_GLB 0x80u                                      /* a page's parameter id with this bit is a GLOBAL (song.g[id & 0x7F]): the
                                                             FX page's SELECT and the deep FX pages; no locks, the project's */

enum { SMFAM_EDIT, SMFAM_GLO, SMFAM_ENV, SMFAM_SEQ, SMFAM_HOME, SMFAM_ARP, SMFAM_FX, SMFAM_COUNT };
enum { SMH_NONE, SMH_MIXER, SMH_CLOCK };                  /* a HOME page's kind (sm_page_t.home) */

typedef struct {
    const char *name;
    uint8_t fam;
    uint8_t id[4];                                        /* the parameters on K1..K4, SM_NOPARAM: none */
    uint8_t sel;                                          /* the parameter on SELECT, SM_NOPARAM: none */
    uint8_t home;                                         /* SMH_*: the HOME pages, whose knobs are not the selected track's sound */
} sm_page_t;

static const sm_page_t SM_PAGES[] = {
    {"EDIT 1", SMFAM_EDIT, {P_E0, P_E1, P_E2, P_E3}, P_E8},
    {"EDIT 2", SMFAM_EDIT, {P_E4, P_E5, P_E6, P_E7}, P_E8},
    {"VOICE", SMFAM_EDIT, {P_VOICE, P_GLIDE, P_DETUNE, P_PRIO}, SM_NOPARAM},
    {"AMP", SMFAM_GLO, {P_ATK, P_DEC, P_SUS, P_REL}, P_ESYNC},
    {"MIX", SMFAM_GLO, {P_LEVEL, P_PAN, P_DIST, P_REV}, SM_NOPARAM},
    {"FILTER", SMFAM_ENV, {P_FLT, P_FCUT, P_FRES, P_ED_FLT}, P_LD_FLT},   /* SELECT: LFO>FLT (HYB: WIDTH) */
    {"F.ENV", SMFAM_ENV, {P_FATK, P_FDEC, P_ED_FLT, SM_NOPARAM}, SM_NOPARAM},
    {"STEP", SMFAM_SEQ, {SM_NOPARAM, SM_NOPARAM, SM_NOPARAM, SM_NOPARAM}, SM_NOPARAM, SMH_NONE},
    {"MIXER", SMFAM_HOME, {P_LEVEL, P_LEVEL, P_LEVEL, P_LEVEL}, SM_NOPARAM, SMH_MIXER},   /* K1..K4: the LEVEL of T1..T4 */
    {"CLOCK", SMFAM_HOME, {SM_NOPARAM, SM_NOPARAM, SM_NOPARAM, SM_NOPARAM}, SM_NOPARAM, SMH_CLOCK},   /* K1 BPM, K2 SWING */
    {"ARP", SMFAM_ARP, {P_AMODE, P_ARATE, P_AOCT, P_AGATE}, P_AORDER},                  /* the arpeggiator (sm_arp.c): the ARP tap */
    {"ARP 2", SMFAM_ARP, {P_ASWING, P_APROB, P_AHOLD, SM_NOPARAM}, SM_NOPARAM},         /* K3 is LATCH (params.c names it HOLD) */
    /* the FX button's family (a tap goes to the next page: FX, DIST, INSERT). FX: the four sends, SELECT the delay's MIX (the
     * core has no separate dry level: the dry is the track's LEVEL); DIST: TYPE DRIVE (the send P_DIST is the drive) TONE OUT,
     * SELECT LOW CUT; INSERT: TYPE (OFF CRUSH PHASR FLANG CHOR: the drives are DIST's), A B C, SELECT MIX */
    {"FX", SMFAM_FX, {P_DIST, P_CHOR, P_DLY, P_REV}, SM_GLB | G_DMIX},
    {"DIST", SMFAM_FX, {P_DTYPE, P_DIST, P_DTONE, P_DOUT}, P_DLCUT},
    {"INSERT", SMFAM_FX, {P_ITYPE, P_IA, P_IB, P_IC}, P_IMIX},
};

/* the deep global pages: the FX button held and SELECT turned (sm_layer.c SML_FX) cycles them; they show while FX is held and
 * the knobs edit song.g (the project's: sm_bank.c). Not in the table above (no button of theirs) */
#define SM_NDEEP 3u
static const sm_page_t SM_DEEP[SM_NDEEP] = {
    {"DLY", SMFAM_FX, {SM_GLB | G_DTIME, SM_GLB | G_DFDBK, SM_GLB | G_DCOLOR, SM_GLB | G_DMIX}, SM_NOPARAM},
    {"REV", SMFAM_FX, {SM_GLB | G_RTYPE, SM_GLB | G_RSIZE, SM_GLB | G_RDAMP, SM_NOPARAM}, SM_NOPARAM},
    {"CHORUS", SMFAM_FX, {SM_GLB | G_CRATE, SM_GLB | G_CDEPTH, SM_NOPARAM, SM_NOPARAM}, SM_NOPARAM},
};

/* what the screen and the keys share (the main loop's alone; sm_draw only reads): the page, the selected track, the step
 * held on a white key (0..31, 0xFF: none; its knobs show and edit the step's locks), the bank of 16 steps, the step
 * selected last (a key held past SM_HOLD_US or turned on; 0xFF none; the STEP page edits it when no key is down), the keyboard
 * mode (the keys play the selected track: sm_input.c) and its octave -3 .. +3 */
static struct { uint8_t page, trk, held_step, bank, sel_step, kbd; int8_t oct; } sm_ui;
enum { SML_NONE, SML_EDIT, SML_GLO, SML_SEQ, SML_FX };
static uint8_t sm_lay_fpage;                              /* FX held: the deep page in view (0 DLY, 1 REV, 2 CHORUS) */
static uint8_t sm_layer;                                  /* the layer open while its button is held (sm_layer.c; the screen and the LEDs read it) */
static volatile uint8_t sm_rec;                           /* recording (the REC key): grid record (the trig keys edit the steps only
                                                             then), knob turns while playing lock the playing trig, live record */
static uint8_t sm_dirty;                                  /* the screen must redraw (sm_draw clears it) */

static uint32_t sm_page_count(void) { return sizeof SM_PAGES / sizeof SM_PAGES[0]; }
/* the page shown: the deep page while FX is held, else the page of sm_ui.page */
static const sm_page_t *sm_cur_page(void)
{
    if (sm_layer == SML_FX)
        return &SM_DEEP[sm_lay_fpage % SM_NDEEP];
    return &SM_PAGES[sm_ui.page % sm_page_count()];
}
/* a layer with its map over the knobs (EDIT, GLO, SEQ); FX held shows the deep pages' knobs, not a map */
static uint32_t sm_map_open(void) { return sm_layer && sm_layer != SML_FX; }
/* the deep page in view + 1 while FX is held, else 0 (the screen's signatures) */
static uint32_t sm_deep_idx(void) { return sm_layer == SML_FX ? 1u + sm_lay_fpage % SM_NDEEP : 0u; }

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

/* the parameter on knob k (0..3; 4: SELECT) of the page shown. FILTER is MONOFM's twin page (params.c): FILT HYB (5) on
 * the selected track's sound makes K2 / K3 HYBRID's BASE / RES (CUT / RES do nothing then: voice.c mf_filter_voice) and
 * SELECT its WIDTH (the low-pass above BASE). In OFF / LP / BP / HP / NOTCH SELECT is the LFO's depth onto the cutoff
 * (P_LD_FLT, LFO DEST's FLT): mf_filter_voice has no key tracking and no drive, its cutoff moves by ENV (K4), the LFO
 * and the accent, so LFO>FLT is the one filter control not on K1..K4 (SWAMP has no LFO page; the LFO runs at its
 * default RATE). Hardware 02:43: SELECT on FILTER was WIDTH in every FILT and did nothing outside HYB */
static uint32_t sm_page_param(uint32_t k)
{
    const sm_page_t *pg = sm_cur_page();
    uint32_t id = k < 4u ? pg->id[k] : pg->sel;
    int hyb = pg->id[0] == P_FLT && smp.pat[smp.cur % SM_NPAT].kit.snd[sm_ui.trk % SM_NTRK].p[P_FLT] == 5;
    if (hyb && id == P_FCUT)
        return P_HBASE;
    if (hyb && id == P_FRES)
        return P_HRES;
    if (hyb && k == 4u)
        return P_HWIDTH;
    return id;
}

/* the ARP button's pages (ARP, ARP 2): the first page of the family, and the page after it */
static uint32_t sm_arp_page(uint32_t second)
{
    uint32_t i, n = sm_page_count(), seen = 0;
    for (i = 0; i < n; i++)
        if (SM_PAGES[i].fam == SMFAM_ARP && seen++ == (second ? 1u : 0u))
            return i;
    return 0;
}
static int sm_page_is_arp(void) { return sm_cur_page()->fam == SMFAM_ARP; }

static int sm_page_is_step(void) { return sm_cur_page()->fam == SMFAM_SEQ; }
static uint32_t sm_page_home(void) { return sm_cur_page()->home; }   /* SMH_*: 0 not a HOME page */

/* the STEP page's FLAG list (the PRESETS knob, round): the flags byte of each place, its name */
#define SM_NFLAG 12u
static const uint8_t SM_FLAGV[SM_NFLAG] = {
    0, SMF_ACCENT, SMF_SLIDE, SMF_ACCENT | SMF_SLIDE, 1u << 4, 2u << 4, 3u << 4,
    SMF_ACCENT | (1u << 4), SMF_ACCENT | (2u << 4), SMF_ACCENT | (3u << 4), SMF_REST, SMF_PSLIDE,
};
static const char *const SM_FLAGN[SM_NFLAG] = {"-", "ACC", "SLD", "A+S", "X2", "X3", "X4", "AX2", "AX3", "AX4", "REST", "PSLD"};
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
