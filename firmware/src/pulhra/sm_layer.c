/* SPDX-License-Identifier: GPL-3.0-only */
/* PULHRA button holds, stage 2: what a family button does when it is tapped and what when it is held (after
 * Felucca's quick layers, ui_layer.c, and its HOLD setting: HOLD_MS[settings_hold], 300 .. 600 ms).
 *   EDIT, GLO  tap: the family's next page (the page changes when the button is let go: a hold must not change it)
 *   SEQ        tap: the STEP page; ENV: its page at once (no layer)
 *   HOME       tap: the family's next page (MIXER, CLOCK); a hold has no layer yet (the menu is a later stage): it does
 *              nothing, and the page does not change when the button is let go
 *   hold       the button down for HOLD_MS, or a key pressed / a knob turned while it is down (even at 50 ms): the layer is
 *              open until the button is let go; the page under it stays as it was
 *   EDIT       white keys from F3: the machines of the page (MONO 13, SOMA 9, SLICE 5: engines.c MF_PAGE, in the order of
 *              eng_vis), OCT+ the next page, G5 INIT (the machine's preset 0). Loads as the PRESETS knob does
 *   GLO        black keys 1..4 (F#3 G#3 A#3 C#4) toggle the mute of T1..T4 (smp.mute); the white keys F3 .. B3 solo T1..T4
 *              while they are held (sm_solo); the banks and patterns (sm_bank.c sm_glo_poll): the black G#4 A#4 C#5 D#5 F#5 put
 *              bank A..E in view, the white C4 D4 E4 F4 G4 A4 B4 C5 are its patterns 1..8: a tap on a used one asks for it (as
 *              SEQ + ALGORITHM), on an empty one saves the current pattern into it (kit and all), a hold (0.8 s) saves the
 *              current pattern into it again (no switch)
 *   SEQ        K1 the selected track's length 1..32, K2 the pattern's master length (AUTO, 1..128); the white keys (MONOFM's
 *              tools, sm_edit.c): F3 CLEAR the track's trigs and locks (held 0.8 s: all four tracks; the kit stays), G3 COPY
 *              the pattern, A3 COPY the track (its sound, steps, locks), B3 PASTE the track, D4 PASTE the pattern, E4 / F4
 *              SHIFT the track's steps < >, B4 / C5 TRN its notes - +; F#3 the pattern change's mode round (SEQUENTIAL, DIRECT START, DIRECT JUMP); ALGORITHM picks a
 *              pattern of all 40 (from the one playing), and letting go of SEQ asks for it (sm_bank.c sm_ask: its bank into RAM,
 *              then smp.next: sm_seq.c switches); F#5 held 0.8 s SAVE (sm_bank.c: at STOP while playing);
 *              OCT+ adds the picked pattern (none picked: the one playing) to the chain being built (up to 16, repeats too),
 *              OCT- throws away what was built (and the pick); letting go of SEQ with two links or more: that chain plays
 *              (smp.chain, sm_chain_set), one link: that pattern is asked for; G#3 turns the chain off (the pattern
 *              playing plays on). Asking for one pattern turns a chain off too (MONOFM's)
 *   GLO + G5   FILL while held (sm_fill: COND FILL / !FILL)
 *   FX, ARP    tap: the family's next page (FX, DIST, INSERT; ARP, ARP 2). A hold has no layer (as HOME's)
 *   HOME held  white keys F3 .. B3: the selected track T1 .. T4 (sm_hold_keys); ARP held: F3 the selected track's note LATCH
 *   SHIFT (OCT- held, outside the keyboard mode) + EDIT / FX / SEQ / HOME: the VOICE page / the deep FX pages DLY REV CHORUS in turn /
 *              the EUCLID page / the MENU page (sm_shift_combo); no layer opens, the button is no tap
 * The keys and knobs of an open layer are the layer's alone (the page's knobs and the trigs do not see them); PLAY, the
 * track knob and PRESETS work as ever. The main loop is the only writer of all of it (sm_input.c's ownership). */

static uint8_t sm_lay_arm;                                /* the layer button down: 0 none, else its index + 1 in SM_LBTN */
static uint32_t sm_lay_t0;                                /* when it went down (fm1_ticks) */
static uint8_t sm_lay_epage;                              /* EDIT: the page of machines in view */
static uint8_t sm_lay_cleared;                            /* SEQ: the track was cleared since the layer opened (CLEARED) */
static uint8_t sm_lay_void;                               /* a button with no layer (HOME) was held: its release is no tap */
static uint8_t sm_lay_pick = SM_PAT_NONE;                 /* SEQ: the pattern ALGORITHM picked (SM_PAT_NONE: none yet) */
static uint8_t sm_lay_chain[SM_NCHAIN], sm_lay_nch;       /* SEQ: the chain being built (OCT+), its links */
static uint8_t sm_lay_clr;                                /* SEQ: F3 down since sm_lay_clr_t0 (1), its hold cleared all (2) */
static uint32_t sm_lay_clr_t0;

#define SM_NLBTN 8u
static const struct { uint8_t btn, layer, fam; } SM_LBTN[SM_NLBTN] = {
    {B_EDIT, SML_EDIT, SMFAM_EDIT}, {B_GLO, SML_GLO, SMFAM_GLO}, {B_SEQ, SML_SEQ, SMFAM_SEQ}, {B_HOME, SML_NONE, SMFAM_HOME},
    {B_FX, SML_NONE, SMFAM_FX}, {B_ARP, SML_NONE, SMFAM_ARP}, {B_LFO, SML_NONE, SMFAM_LFO}, {B_SCL, SML_NONE, SMFAM_SCL},
};
#define SM_LAY_INIT_KEY 15u                               /* the white key of INIT: G5 */
#define SM_LAY_MODE_KEY 1u                                /* SEQ: F#3 (key 1), the change mode (MONOFM's) */
#define SM_LAY_CHOFF_KEY 3u                               /* SEQ: G#3 (key 3), the chain off */
#define SM_LAY_SHL_KEY 5u                                 /* SEQ: A#3 (key 5), every track's trigs a step back */
#define SM_LAY_SHR_KEY 8u                                 /* SEQ: C#4 (key 8), ... a step forward */
#define SM_LAY_TDN_KEY 10u                                /* SEQ: D#4 (key 10), every track's trigs a semitone down */
#define SM_LAY_TUP_KEY 13u                                /* SEQ: F#4 (key 13), ... up */
#define SM_LAY_F3_KEY 0u                                  /* SEQ: F3 (key 0), CLEAR (held: all) */
#define SM_LAY_FILL_KEY 26u                               /* GLO: G5 (key 26), FILL while held */
enum { SM_W_CLEAR = 0, SM_W_COPY_PAT = 1, SM_W_COPY_TRK = 4, SM_W_PASTE_TRK = 5, SM_W_PASTE_PAT = 2, SM_W_SHL = 6, SM_W_SHR = 7,
       SM_W_TRN_DN = 10, SM_W_TRN_UP = 11 };             /* SEQ: the white keys of the tools (F3 = 0); C4=4 copy, D4=5 paste */
#define SM_NEPAGE 3u

static void sm_layer_init(void)
{
    sm_layer = SML_NONE;
    sm_lay_arm = 0;
    sm_lay_t0 = 0;
    sm_lay_epage = 0;
    sm_lay_cleared = 0;
    sm_lay_void = 0;
    sm_lay_pick = SM_PAT_NONE;
    sm_lay_nch = 0;
    sm_lay_clr = 0;
    sm_solo = 0;
    sm_fill = 0;
}

static uint32_t sm_layer_busy(void) { return sm_layer || sm_lay_arm; }

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
    sm_key_arm = sm_key_used = 0;                         /* (a trig key down before the layer never toggles inside it) */
    sm_layer = (uint8_t)l;
    sm_lay_cleared = 0;
    sm_lay_pick = SM_PAT_NONE;
    sm_lay_nch = 0;
    sm_lay_clr = 0;
    if (l == SML_EDIT)                                    /* (opens on the page of the selected track's machine) */
        sm_lay_epage = (uint8_t)sm_epage_of(sm_ui_sound()->machine);
    if (l == SML_GLO)
        sm_glo_open();                                    /* (the grid: the current pattern's bank) */
    sm_fsave_dn = 0;
    sm_dirty = 1;
}

/* the chain c[0 .. n-1] (2 .. 16 links, pattern ids 0..39) published for the audio stream: the old one off first (nchain 0:
 * the audio stream reads no link), the links, a new generation (the link playing is link 1 again), link 1 asked for (its
 * RAM slot: sm_bank.c publishes a chain once link 1's bank is in RAM), then nchain. Each a byte store with a barrier between:
 * whatever block runs in between sees no chain, or this whole one */
static void sm_chain_set(const uint8_t *c, uint32_t n)
{
    uint32_t i;
    smp.nchain = 0;
    SM_BARRIER();
    for (i = 0; i < n && i < SM_NCHAIN; i++)
        smp.chain[i] = (uint8_t)(c[i] % SM_NGPAT);
    SM_BARRIER();
    sm_chain_gen = (uint8_t)(sm_chain_gen + 1u);
    SM_BARRIER();
    smp.next = (uint8_t)sm_slot_of(smp.chain[0]);
    SM_BARRIER();
    smp.nchain = (uint8_t)(n < SM_NCHAIN ? n : SM_NCHAIN);
}

/* SEQ let go: the chain built (two links or more), else its one link or the pattern picked, asked for (the chain off) */
static void sm_seq_commit(void)
{
    uint32_t p = sm_lay_nch == 1u ? sm_lay_chain[0] : sm_lay_pick;
    if (sm_lay_nch >= 2u)
        sm_chain_ask(sm_lay_chain, sm_lay_nch);          /* (published once link 1's bank is in RAM: sm_bank.c) */
    else if (p < SM_NGPAT)
        sm_ask(p);                                        /* (its bank brought in, then smp.next: sm_bank.c) */
}

static void sm_layer_close(void)
{
    if (sm_layer == SML_SEQ)
        sm_seq_commit();
    sm_lay_pick = SM_PAT_NONE;
    sm_lay_nch = 0;
    sm_layer = SML_NONE;
    sm_solo = 0;
    sm_fill = 0;
    sm_lay_clr = 0;
    sm_lay_cleared = 0;
    sm_glo_key = 0xFFu;
    sm_fsave_dn = 0;
    sm_dirty = 1;
}

/* SHIFT (OCT- held, outside the keyboard mode) + a family button: EDIT the VOICE page, FX the deep effect pages DLY / REV / CHORUS in
 * turn, SEQ the EUCLID page, HOME the MENU page (a stub until the menu); GLO and ARP have no combo (nothing happens). SAVE: sm_save_poll */
static void sm_shift_combo(uint32_t btn)
{
    if (btn == B_EDIT)
        sm_page_press(SMFAM_VOICE);
    else if (btn == B_FX)
        sm_page_press(SMFAM_DEEP);
    else if (btn == B_SEQ)
        sm_page_press(SMFAM_EUCL);
    else if (btn == B_HOME)
        sm_page_open_setup();
}

/* HOME or ARP held long enough or with a key / knob used (sm_lay_void: a hold with no layer): the button, else 0xFF */
static uint32_t sm_hold_btn(void)
{
    return sm_lay_arm && !sm_layer && sm_lay_void ? SM_LBTN[sm_lay_arm - 1u].btn : 0xFFu;
}

/* the keys of such a hold, this poll's presses: HOME + the white keys F3 .. B3 select T1 .. T4; ARP + F3 the selected track's LATCH
 * (sm_latch_toggle). The other keys do nothing (the trig keys never see them: sm_input_poll) */
static void sm_hold_keys(uint32_t notes)
{
    uint32_t b = sm_hold_btn(), key;
    if (b == 0xFFu)
        return;
    for (key = 0; key < SM_NKEYS; key++) {
        int32_t w;
        if (!((notes >> key) & 1u))
            continue;
        w = sm_white_of(key);
        if (b == B_HOME && w >= 0 && (uint32_t)w < SM_NTRK) {
            sm_ui.trk = (uint8_t)w;
            sm_ui.held_step = sm_ui.sel_step = 0xFFu;
            sm_key_arm = sm_key_used = 0;
            sm_dirty = 1;
        } else if (b == B_ARP && w == 0) {
            sm_latch_toggle();
        }
    }
}

/* every poll, first: a layer button going down is remembered (let go again already: a tap at once); the one down is a layer
 * once HOLD_MS has gone or something was used (a key, a knob); ENV is its page at once. used: a key or a knob this poll */
static void sm_layer_begin(uint32_t pressed, uint32_t now, uint32_t used)
{
    uint32_t i, held = fm1_in.buttons;
    if (!sm_lay_arm) {
        for (i = 0; i < SM_NLBTN; i++) {
            uint32_t b = panel.btn[SM_LBTN[i].btn];
            if (!((pressed >> b) & 1u))
                continue;
            if (sm_shift_held() && !sm_layer) {           /* SHIFT + the button: its combo, never a tap or a layer */
                sm_shift_combo(SM_LBTN[i].btn);
                break;
            }
            if (!((held >> b) & 1u)) {
                if (SM_LBTN[i].fam < SMFAM_COUNT)
                    sm_page_press(SM_LBTN[i].fam);        /* (down and up between two polls) */
            } else {
                sm_lay_arm = (uint8_t)(i + 1u);
                sm_lay_t0 = now;
                sm_lay_void = 0;
            }
            break;
        }
        if (!sm_lay_arm && ((pressed >> panel.btn[B_ENV]) & 1u))
            sm_page_press(SMFAM_ENV);
    }
    if (sm_lay_arm && !sm_layer && !sm_lay_void &&
        (used || (uint32_t)(now - sm_lay_t0) >= (uint32_t)HOLD_MS[settings_hold % 4u] * 1000u * FM1_TICKS_PER_US)) {
        if (SM_LBTN[sm_lay_arm - 1u].layer == SML_NONE)
            sm_lay_void = 1;                              /* (a hold of a button with no layer: nothing, and no tap at its release) */
        else
            sm_layer_open(SM_LBTN[sm_lay_arm - 1u].layer);
    }
}

/* every poll, last: the layer button let go: a tap (no layer opened) is the page; an open layer closes */
static void sm_layer_end(void)
{
    if (sm_lay_arm && !((fm1_in.buttons >> panel.btn[SM_LBTN[sm_lay_arm - 1u].btn]) & 1u)) {
        if (sm_layer)
            sm_layer_close();
        else if (!sm_lay_void && SM_LBTN[sm_lay_arm - 1u].fam < SMFAM_COUNT)
            sm_page_press(SM_LBTN[sm_lay_arm - 1u].fam);
        sm_lay_void = 0;
        sm_lay_arm = 0;
    }
}

/* SEQ + a white key w (F3 = 0): the tools on the selected track t / the pattern */
static void sm_seq_tool(uint32_t w, uint32_t t)
{
    switch (w) {
    case SM_W_CLEAR:
        sm_edit_clear(1u << t, 1);
        sm_lay_cleared = 1;
        sm_lay_clr = 1;                                   /* (held on: all four tracks) */
        sm_lay_clr_t0 = fm1_ticks();
        break;
    case SM_W_COPY_PAT: sm_copy_pat(); break;
    case SM_W_COPY_TRK: sm_copy_trk(t); break;
    case SM_W_PASTE_TRK: sm_paste_trk(t); break;
    case SM_W_PASTE_PAT: sm_paste_pat(); break;
    case SM_W_SHL: sm_shift_trk(t, -1); break;
    case SM_W_SHR: sm_shift_trk(t, 1); break;
    case SM_W_TRN_DN: sm_trn(t, -1); break;
    case SM_W_TRN_UP: sm_trn(t, 1); break;
    default: return;
    }
    sm_dirty = 1;
}

/* the open layer's keys (the white key number is its place among the white keys, F3 = 0) and knobs, this poll's edges */
static void sm_layer_keys(uint32_t pressed, uint32_t notes, const int32_t *k, int32_t alg, int32_t sel)
{
    uint32_t key, t = sm_ui.trk % SM_NTRK;
    if (sm_layer == SML_EDIT && ((pressed >> panel.btn[B_OCTUP]) & 1u)) {
        sm_lay_epage = (uint8_t)((sm_lay_epage + 1u) % SM_NEPAGE);
        sm_dirty = 1;
    }
    if (sm_layer == SML_SEQ && sel) {                     /* SELECT: the track's length */
        sm_sound_t *sd = sm_ui_sound();
        int32_t len = clamp((int32_t)sd->len + sel, 1, (int32_t)SM_NSTEP);
        if ((uint8_t)len != sd->len) {
            sd->len = (uint8_t)len;
            if (len <= 16)
                sm_ui.bank = 0;
            sm_dirty = 1;
        }
    }
    if (sm_layer == SML_SEQ && (k[0] || k[1])) {          /* K1 the pattern's speed (/8 .. *8), K2 the change mode; K3, K4 nothing */
        sm_pattern_t *pt = &smp.pat[smp.cur % SM_NPAT];
        int32_t v;
        if (k[0]) {                                       /* (/8 .. *8, no round: a byte the audio stream reads every block) */
            v = clamp((int32_t)sm_speed_place(pt->speed) + k[0], 0, (int32_t)SM_NSPEED - 1) - SM_SPEED_ONE;
            pt->speed = (int8_t)v;
        }
        if (k[1])                                         /* (SEQ, DIR.S, DIR.J, round) */
            smp.mode = (uint8_t)((((int32_t)smp.mode + k[1]) % (int32_t)SM_PM_COUNT + (int32_t)SM_PM_COUNT) % (int32_t)SM_PM_COUNT);
        sm_dirty = 1;
    }
    if (sm_layer == SML_SEQ && ((pressed >> panel.btn[B_OCTUP]) & 1u) && sm_lay_nch < SM_NCHAIN) {
        sm_lay_chain[sm_lay_nch++] = (uint8_t)(sm_lay_pick < SM_NGPAT ? sm_lay_pick : sm_gid_of(smp.cur) % SM_NGPAT);
        sm_dirty = 1;
    }
    if (sm_layer == SML_SEQ && ((pressed >> panel.btn[B_OCTDN]) & 1u)) {   /* what was built (and the pick) thrown away */
        sm_lay_nch = 0;
        sm_lay_pick = SM_PAT_NONE;
        sm_dirty = 1;
    }
    if (sm_layer == SML_SEQ && alg) {                     /* the pattern picked (all 40, A01..E08), from the one playing (wraps) */
        int32_t p = ((int32_t)(sm_lay_pick < SM_NGPAT ? sm_lay_pick : sm_gid_of(smp.cur) % SM_NGPAT) + alg) % (int32_t)SM_NGPAT;
        sm_lay_pick = (uint8_t)(p < 0 ? p + (int32_t)SM_NGPAT : p);
        sm_dirty = 1;
    }
    if (sm_layer == SML_SEQ)
        sm_fsave_poll(notes, fm1_ticks());                /* (F#5 held 0.8 s: SAVE) */
    if (sm_layer == SML_GLO)
        sm_glo_poll(notes, fm1_ticks());                  /* (the bank / pattern grid) */
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
        if (((held >> SM_LAY_FILL_KEY) & 1u) != sm_fill) {
            sm_fill = (uint8_t)((held >> SM_LAY_FILL_KEY) & 1u);   /* (a byte: the audio stream reads it at a COND) */
            sm_dirty = 1;
        }
    }
    if (sm_layer == SML_SEQ && sm_lay_clr == 1u) {       /* F3 held on: all four tracks at 0.8 s (the tap's UNDO snapshot) */
        if (!((fm1_in.notes >> SM_LAY_F3_KEY) & 1u))
            sm_lay_clr = 0;
        else if ((uint32_t)(fm1_ticks() - sm_lay_clr_t0) >= SM_CLEAR_ALL_US * FM1_TICKS_PER_US) {
            sm_edit_clear((1u << SM_NTRK) - 1u, 0);
            sm_lay_clr = 2;
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
            uint32_t b, n = 0;                            /* the black key's place among the black keys, 0 .. 3: T1 .. T4 (the
                                                             banks' keys: sm_glo_poll) */
            if (w >= 0)
                continue;
            for (b = 0; b < key; b++)
                n += (SM_BLACK_KEYS >> b) & 1u;
            if (n < SM_NTRK) {
                smp.mute ^= (uint8_t)(1u << n);
                sm_dirty = 1;
            }
        } else if (sm_layer == SML_SEQ && key == SM_LAY_MODE_KEY) {
            smp.mode = (uint8_t)((smp.mode + 1u) % SM_PM_COUNT);
            sm_dirty = 1;
        } else if (sm_layer == SML_SEQ && key == SM_LAY_CHOFF_KEY) {   /* the chain off (and the one being built) */
            smp.nchain = 0;
            sm_chain_npend = 0;
            sm_lay_nch = 0;
            sm_dirty = 1;
        } else if (sm_layer == SML_SEQ && (key == SM_LAY_SHL_KEY || key == SM_LAY_SHR_KEY)) {
            sm_shift_all(key == SM_LAY_SHR_KEY ? 1 : -1);
        } else if (sm_layer == SML_SEQ && (key == SM_LAY_TDN_KEY || key == SM_LAY_TUP_KEY)) {
            sm_trn_all(key == SM_LAY_TUP_KEY ? 1 : -1);
        } else if (sm_layer == SML_SEQ && w >= 0) {
            sm_seq_tool((uint32_t)w, t);
        }
    }
}
