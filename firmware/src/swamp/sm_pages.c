/* SPDX-License-Identifier: GPL-3.0-only */
/* SWAMP MACHINE pages, stage 1: four knobs and SELECT per page, grouped in the families of the buttons EDIT / GLO / ENV
 * (a second press of a family's button goes to its next page). The screen (sm_draw) reads sm_ui and sm_cur_page(). */
#define SM_NOPARAM 0xFFu

enum { SMFAM_EDIT, SMFAM_GLO, SMFAM_ENV, SMFAM_COUNT };

typedef struct {
    const char *name;
    uint8_t fam;
    uint8_t id[4];                                        /* the parameters on K1..K4, SM_NOPARAM: none */
    uint8_t sel;                                          /* the parameter on SELECT, SM_NOPARAM: none */
} sm_page_t;

static const sm_page_t SM_PAGES[] = {
    {"EDIT 1", SMFAM_EDIT, {P_E0, P_E1, P_E2, P_E3}, P_E8},
    {"EDIT 2", SMFAM_EDIT, {P_E4, P_E5, P_E6, P_E7}, P_E8},
    {"VOICE", SMFAM_EDIT, {P_VOICE, P_GLIDE, P_DETUNE, P_PRIO}, SM_NOPARAM},
    {"AMP", SMFAM_GLO, {P_ATK, P_DEC, P_SUS, P_REL}, P_ESYNC},
    {"MIX", SMFAM_GLO, {P_LEVEL, P_PAN, P_DIST, P_REV}, SM_NOPARAM},
    {"FILTER", SMFAM_ENV, {P_FLT, P_FCUT, P_FRES, P_ED_FLT}, P_HWIDTH},
    {"F.ENV", SMFAM_ENV, {P_FATK, P_FDEC, P_ED_FLT, SM_NOPARAM}, SM_NOPARAM},
};

/* what the screen and the keys share (the main loop's alone; sm_draw only reads): the page, the selected track, the step
 * held on a white key (0..31, 0xFF: none; stage 2 turns the held step's knobs into locks), the bank of 16 steps */
static struct { uint8_t page, trk, held_step, bank; } sm_ui;
enum { SML_NONE, SML_EDIT, SML_GLO, SML_SEQ };
static uint8_t sm_layer;                                  /* the layer open while its button is held (sm_layer.c; the screen and the LEDs read it) */
static uint8_t sm_dirty;                                  /* the screen must redraw (sm_draw clears it) */

static uint32_t sm_page_count(void) { return sizeof SM_PAGES / sizeof SM_PAGES[0]; }
static const sm_page_t *sm_cur_page(void) { return &SM_PAGES[sm_ui.page % sm_page_count()]; }

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
    sm_dirty = 1;
}
