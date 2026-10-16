/* SPDX-License-Identifier: GPL-3.0-only */
/* SWAMP MACHINE button holds, stage 2: what a family button does when it is tapped and what when it is held (after
 * Felucca's quick layers, ui_layer.c, and its HOLD setting: HOLD_MS[settings_hold], 300 .. 600 ms).
 *   EDIT, GLO  tap: the family's next page (the page changes when the button is let go: a hold must not change it)
 *   SEQ        tap: nothing (no page); ENV: its page at once (no layer)
 *   hold       the button down for HOLD_MS, or a key pressed / a knob turned while it is down (even at 50 ms): the layer is
 *              open until the button is let go; the page under it stays as it was
 *   EDIT       white keys from F3: the machines of the page (MONO 13, SOMA 9, SLICE 5: engines.c MF_PAGE, in the order of
 *              eng_vis), OCT+ the next page, G5 INIT (the machine's preset 0). Loads as the PRESETS knob does
 *   GLO        black keys 1..4 (F#3 G#3 A#3 C#4) toggle the mute of T1..T4 (smp.mute); the white keys F3 .. B3 solo T1..T4
 *              while they are held (sm_solo)
 *   SEQ        K1 the selected track's length 1..32; the white key F3 clears the track's steps
 * The keys and knobs of an open layer are the layer's alone (the page's knobs and the trigs do not see them); PLAY, the
 * track knob and PRESETS work as ever. The main loop is the only writer of all of it (sm_input.c's ownership). */

static uint8_t sm_lay_arm;                                /* the layer button down: 0 none, else its index + 1 in SM_LBTN */
static uint32_t sm_lay_t0;                                /* when it went down (fm1_ticks) */
static uint8_t sm_lay_epage;                              /* EDIT: the page of machines in view */
static uint8_t sm_lay_cleared;                            /* SEQ: the track was cleared since the layer opened (CLEARED) */

static const struct { uint8_t btn, layer, fam; } SM_LBTN[3] = {
    {B_EDIT, SML_EDIT, SMFAM_EDIT}, {B_GLO, SML_GLO, SMFAM_GLO}, {B_SEQ, SML_SEQ, SMFAM_COUNT},
};
#define SM_LAY_INIT_KEY 15u                               /* the white key of INIT: G5 */
#define SM_NEPAGE 3u

static void sm_layer_init(void)
{
    sm_layer = SML_NONE;
    sm_lay_arm = 0;
    sm_lay_t0 = 0;
    sm_lay_epage = 0;
    sm_lay_cleared = 0;
    sm_solo = 0;
}

/* the button of layer l */
static uint32_t sm_layer_button(uint32_t l)
{
    return l == SML_EDIT ? B_EDIT : l == SML_GLO ? B_GLO : B_SEQ;
}

/* the page of machines (0..2) a machine is on, its first machine, how many it has */
static uint32_t sm_epage_of(uint32_t machine) { return machine >= MF_PAGE[2] ? 2u : machine >= MF_PAGE[1] ? 1u : 0u; }
static uint32_t sm_epage_first(uint32_t pg) { return MF_PAGE[pg % SM_NEPAGE]; }
static uint32_t sm_epage_n(uint32_t pg) { return (uint32_t)MF_PAGE[pg % SM_NEPAGE + 1u] - MF_PAGE[pg % SM_NEPAGE]; }

static void sm_layer_open(uint32_t l)
{
    sm_layer = (uint8_t)l;
    sm_lay_cleared = 0;
    if (l == SML_EDIT)                                    /* (opens on the page of the selected track's machine) */
        sm_lay_epage = (uint8_t)sm_epage_of(sm_ui_sound()->machine);
    sm_dirty = 1;
}

static void sm_layer_close(void)
{
    sm_layer = SML_NONE;
    sm_solo = 0;
    sm_lay_cleared = 0;
    sm_dirty = 1;
}

/* every poll, first: a layer button going down is remembered (let go again already: a tap at once); the one down is a layer
 * once HOLD_MS has gone or something was used (a key, a knob); ENV is its page at once. used: a key or a knob this poll */
static void sm_layer_begin(uint32_t pressed, uint32_t now, uint32_t used)
{
    uint32_t i, held = fm1_in.buttons;
    if (!sm_lay_arm) {
        for (i = 0; i < 3u; i++) {
            uint32_t b = panel.btn[SM_LBTN[i].btn];
            if (!((pressed >> b) & 1u))
                continue;
            if (!((held >> b) & 1u)) {
                if (SM_LBTN[i].fam < SMFAM_COUNT)
                    sm_page_press(SM_LBTN[i].fam);        /* (down and up between two polls) */
            } else {
                sm_lay_arm = (uint8_t)(i + 1u);
                sm_lay_t0 = now;
            }
            break;
        }
        if (!sm_lay_arm && ((pressed >> panel.btn[B_ENV]) & 1u))
            sm_page_press(SMFAM_ENV);
    }
    if (sm_lay_arm && !sm_layer &&
        (used || (uint32_t)(now - sm_lay_t0) >= (uint32_t)HOLD_MS[settings_hold % 4u] * 1000u * FM1_TICKS_PER_US))
        sm_layer_open(SM_LBTN[sm_lay_arm - 1u].layer);
}

/* every poll, last: the layer button let go: a tap (no layer opened) is the page; an open layer closes */
static void sm_layer_end(void)
{
    if (sm_lay_arm && !((fm1_in.buttons >> panel.btn[SM_LBTN[sm_lay_arm - 1u].btn]) & 1u)) {
        if (sm_layer)
            sm_layer_close();
        else if (SM_LBTN[sm_lay_arm - 1u].fam < SMFAM_COUNT)
            sm_page_press(SM_LBTN[sm_lay_arm - 1u].fam);
        sm_lay_arm = 0;
    }
}

/* the selected track's steps all off (n first: the audio stream reads n before the rest of a step) and no REST. Task 4: the
 * track's locks of the pattern go here too (sm_lock_*) */
static void sm_track_clear(uint32_t t)
{
    sm_pattern_t *pt = &smp.pat[smp.cur % SM_NPAT];
    uint32_t s;
    for (s = 0; s < SM_NSTEP; s++) {
        pt->step[t % SM_NTRK][s].n = 0;
        pt->step[t % SM_NTRK][s].flags = 0;
    }
}

/* the open layer's keys (the white key number is its place among the white keys, F3 = 0) and knobs, this poll's edges */
static void sm_layer_keys(uint32_t pressed, uint32_t notes, const int32_t *k)
{
    uint32_t key, t = sm_ui.trk % SM_NTRK;
    if (sm_layer == SML_EDIT && ((pressed >> panel.btn[B_OCTUP]) & 1u)) {
        sm_lay_epage = (uint8_t)((sm_lay_epage + 1u) % SM_NEPAGE);
        sm_dirty = 1;
    }
    if (sm_layer == SML_SEQ && k[0]) {
        sm_sound_t *sd = sm_ui_sound();
        int32_t len = clamp((int32_t)sd->len + k[0], 1, (int32_t)SM_NSTEP);
        if ((uint8_t)len != sd->len) {
            sd->len = (uint8_t)len;                       /* (a byte: the audio stream reads it, sm_seq_step) */
            if (len <= 16)
                sm_ui.bank = 0;
            sm_dirty = 1;
        }
    }
    if (sm_layer == SML_GLO) {                            /* solo: the white keys F3 .. B3 held */
        uint32_t held = fm1_in.notes, solo = 0, w;
        for (key = 0, w = 0; key < SM_NKEYS && w < SM_NTRK; key++) {
            if ((SM_BLACK_KEYS >> key) & 1u)
                continue;
            solo |= ((held >> key) & 1u) << w++;
        }
        if (solo != sm_solo) {
            sm_solo = (uint8_t)solo;
            sm_dirty = 1;
        }
    }
    for (key = 0; key < SM_NKEYS; key++) {
        int32_t w;
        if (!((notes >> key) & 1u))
            continue;
        w = sm_white_of(key);
        if (sm_layer == SML_EDIT) {
            if (w == (int32_t)SM_LAY_INIT_KEY)
                sm_sound_load(sm_ui_sound()->machine, 0);
            else if (w >= 0 && (uint32_t)w < sm_epage_n(sm_lay_epage))
                sm_sound_load(sm_epage_first(sm_lay_epage) + (uint32_t)w, 0);
        } else if (sm_layer == SML_GLO) {
            uint32_t b, n = 0;                            /* the black key's place among the black keys, 0 .. 3: T1 .. T4 */
            if (w >= 0)
                continue;
            for (b = 0; b < key; b++)
                n += (SM_BLACK_KEYS >> b) & 1u;
            if (n < SM_NTRK) {
                smp.mute ^= (uint8_t)(1u << n);
                sm_dirty = 1;
            }
        } else if (sm_layer == SML_SEQ && w == 0) {
            sm_track_clear(t);
            sm_lay_cleared = 1;
            sm_dirty = 1;
        }
    }
}
