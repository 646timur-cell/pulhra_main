/* SPDX-License-Identifier: GPL-3.0-only */
/* PULHRA panel LEDs, after the mechanism of Felucca's ui_leds (ui_input.c): each frame the picture is built off-line,
 * one byte per column (lit / glow / breath / mid), and copied glow first, lit last, so an LED going from lit to glow never
 * has a dark frame for the 10 kHz scan to catch. The LEDs carry the step information (the screen strip stays ascetic):
 *  - white keys, the steps of the visible bank, three states told at a glance: a TRIG (n > 0, no REST) is lit; a REST
 *    (SMF_REST: locks only, no note) breathes; an EMPTY step is fully dark, no glow in any LEDS mode (INV turns it round,
 *    as it turns round every LED: the active ones dark, the rest lit);
 *  - the step playing is a soft running light: the MID level, never a full flash: on an empty step mid, a trig comes down
 *    to mid, a REST has mid in place of its breath; a held step is mid too (and a frame on the screen);
 *  - REC: steady while armed (stopped), breathing while recording (playing); a HOME / ARP hold: F3 .. B3 the tracks / F3 the LATCH
 *  - OCT- / OCT+ (outside a layer): OCT+ lit on the page of steps 17..32 (a track longer than 16; 1..16: it glows), OCT- (SHIFT)
 *    lit while it is held; the keyboard mode: the white keys show no steps, and OCT- / OCT+ are lit while the octave is below /
 *    above 0 (the header says KEYS +n);
 *  - buttons: the family of the page in view lit (HOME's too), REC lit while recording, PLAY's own LED glows (dark while playing, its
 *    green one lit), the rest glow, as settings_leds (OFF / DIM LO / DIM HI / INV) says;
 *  - a layer open (sm_layer.c): its button breathes (not lit, no glow), and the keys show what they do in it instead of the
 *    steps: EDIT the machines of the page glow, the track's own lit; GLO the mutes (black keys 1..4, lit = muted), the
 *    solo keys F3 .. B3 (lit while held), the patterns C4 .. C5 of the bank in view (the current one lit, the used ones
 *    breathe, the empty dark) and the banks G#4 A#4 C#5 D#5 F#5 (the one in view lit, the others breathe); SEQ F3 (CLEAR)
 *    glows. */
#define SM_GLO_PAT0 4u                                    /* GLO: the white key of pattern 1 (C4) */
#define SM_GLO_BANK0 6u                                   /* GLO: the black key of bank A (G#4), among the black keys */
#define LED_PLAY_GREEN ((8u << 3) | 1u)                   /* PLAY's second, green LED: column 8, row bit 1 (ui_input.c) */

static uint8_t sm_led_pos[41];                            /* (col << 3) | row bit of a key id (0..13 buttons, 14.. keys) */

static void sm_led_pos_init(void)
{
    uint32_t id, p, r;
    for (id = 0; id < 41u; id++) {
        sm_led_pos[id] = 0xFF;
        for (p = 0; p < FM1_NCOL; p++)
            for (r = 1; r < 5u; r++)
                if (FM1_KEYMAP[r][p] == (int8_t)id)
                    sm_led_pos[id] = (uint8_t)((p << 3) | r);
    }
}
static void sm_led_put(uint8_t *nl, uint32_t id, int on)
{
    uint8_t q = id < 41u ? sm_led_pos[id] : 0xFF;
    if (q != 0xFF && on)
        nl[q >> 3] |= (uint8_t)(1u << (q & 7u));
}
static void sm_led_clear(uint8_t *nl, uint32_t id)
{
    uint8_t q = id < 41u ? sm_led_pos[id] : 0xFF;
    if (q != 0xFF)
        nl[q >> 3] &= (uint8_t)~(1u << (q & 7u));
}

/* the keys of the open layer: the white keys by their number (F3 = 0), the black keys 1..4 for the mutes */
static void sm_leds_layer(uint8_t *nl, uint8_t *ng, uint8_t *ni, uint8_t *nbr)
{
    uint32_t k, w = 0, nb = 0, t = sm_ui.trk % SM_NTRK, cur = sm_mach_shown(&smp.pat[smp.cur % SM_NPAT], t);
    uint32_t gcur = sm_gid_of(smp.cur);
    for (k = 0; k < SM_NKEYS; k++) {
        if ((SM_BLACK_KEYS >> k) & 1u) {
            if (sm_layer == SML_SEQ && (k == SM_LAY_MODE_KEY || k == SM_TCOPY_KEY || k == SM_TPASTE_KEY)) {
                                                          /* (SEQ: F#3 the change mode; D#5 F#5 held the pattern's COPY / PASTE) */
                sm_led_put(ni, 14u + k, 1);
                sm_led_put(ng, 14u + k, 1);
            }
            if (sm_layer == SML_FX && (nb < SM_NTRK || k == SM_FX_TGT_KEY)) {   /* FX: the mutes, the target */
                sm_led_put(ni, 14u + k, 1);
                sm_led_put((sm_fx_keys >> k) & 1u ? nl : ng, 14u + k, 1);
            }
            if (sm_layer == SML_GLO && nb < SM_NTRK) {
                sm_led_put(ni, 14u + k, 1);
                sm_led_put((smp.mute >> nb) & 1u ? nl : ng, 14u + k, 1);
            }
            if (sm_layer == SML_GLO && nb >= SM_GLO_BANK0 && nb < SM_GLO_BANK0 + SM_NBANK) {   /* the banks: the one in view lit */
                sm_led_put(ni, 14u + k, 1);
                sm_led_put(nb - SM_GLO_BANK0 == sm_glo_bank ? nl : nbr, 14u + k, 1);
            }
            nb++;
            continue;
        }
        if (sm_layer == SML_FX) {                         /* FX: a key with an effect glows, held lit */
            uint32_t e = w < PF_KEYS ? perf_map[w] : PF_N;
            sm_led_put(ni, 14u + k, 1);
            if ((sm_fx_keys >> k) & 1u)
                sm_led_put(nl, 14u + k, 1);
            else if (e < PF_N)
                sm_led_put(ng, 14u + k, 1);
            w++;
            continue;
        }
        if (sm_layer == SML_SEQ) {                        /* (SEQ held: the white keys keep the trigs, sm_leds; the black keys the tools) */
            w++;
            continue;
        }
        sm_led_put(ni, 14u + k, 1);
        if (sm_layer == SML_EDIT) {
            if (w < sm_epage_n(sm_lay_epage) && sm_mach_ok(sm_epage_mach(sm_lay_epage, w), sm_ui.trk))
                sm_led_put(sm_epage_on(sm_lay_epage, w, cur) ? nl : ng, 14u + k, 1);   /* (the DRUM page: dark off T4) */
        } else if (sm_layer == SML_GLO && w < SM_NTRK) {
            sm_led_put((sm_solo >> w) & 1u ? nl : ng, 14u + k, 1);
        } else if (sm_layer == SML_GLO && w >= SM_GLO_PAT0 && w < SM_GLO_PAT0 + SM_BPAT) {   /* the patterns of the bank in view: */
            uint32_t g = sm_glo_bank * SM_BPAT + w - SM_GLO_PAT0;                            /* the current lit, used ones breathe */
            if (g == gcur)
                sm_led_put(nl, 14u + k, 1);
            else if (sm_pat_used(g))
                sm_led_put(nbr, 14u + k, 1);
        }
        w++;
    }
}

/* HOME held: F3 .. B3 are T1 .. T4 (the selected one lit); ARP held: F3 is LATCH (lit while the selected track's is on) */
static void sm_leds_hold(uint8_t *nl, uint8_t *ng, uint8_t *ni)
{
    uint32_t k, w = 0, b = sm_hold_btn(), t = sm_ui.trk % SM_NTRK;
    for (k = 0; k < SM_NKEYS; k++) {
        if ((SM_BLACK_KEYS >> k) & 1u)
            continue;
        if (b == B_HOME && w < SM_NTRK) {
            sm_led_put(ni, 14u + k, 1);
            sm_led_put(w == t ? nl : ng, 14u + k, 1);
        } else if (b == B_ARP && w == 0u) {
            sm_led_put(ni, 14u + k, 1);
            sm_led_put((sm_klatch >> t) & 1u ? nl : ng, 14u + k, 1);
        }
        w++;
    }
}

static void sm_leds(void)
{
    uint8_t nl[FM1_NCOL] = {0}, ng[FM1_NCOL] = {0}, nb[FM1_NCOL] = {0}, nm[FM1_NCOL] = {0}, ni[FM1_NCOL] = {0};
    uint32_t mode = settings_leds, t = sm_ui.trk % SM_NTRK, k, c, w = 0, len, bank = sm_ui.bank * 16u;
    uint32_t play = *(volatile uint8_t *)&sm_playing != 0u, ph = *(volatile uint8_t *)&sm_pos[t];
    static uint8_t ready;
    const sm_pattern_t *pt = &smp.pat[smp.cur % SM_NPAT];
    if (!ready) {
        sm_led_pos_init();
        ready = 1;
    }
    len = pt->kit.snd[t].len;
    len = len < 1u ? 1u : len > SM_NSTEP ? SM_NSTEP : len;
    for (k = 0; k < SM_NKEYS && (!sm_layer || sm_layer == SML_SEQ) && !sm_ui.kbd && sm_hold_btn() == 0xFFu; k++) {   /* (SEQ held: the trigs stay lit) */
        const sm_step_t *st;
        uint32_t s;
        if ((SM_BLACK_KEYS >> k) & 1u)
            continue;
        s = bank + w++;
        {
            static sm_step_t tmp;
            st = sm_step_view(pt, t, s, &tmp);            /* (KIT: the lane in edit's) */
        }
        sm_led_put(ni, 14u + k, 1);
        if ((play && ph == s) || sm_ui.held_step == s)    /* the soft running light: mid, in place of lit and of breath */
            sm_led_put(nm, 14u + k, 1);
        else if (s < len && (st->flags & SMF_REST))
            sm_led_put(nb, 14u + k, 1);
        else if (s < len && st->n)
            sm_led_put(nl, 14u + k, 1);
    }
    if (sm_hold_btn() != 0xFFu)
        sm_leds_hold(nl, ng, ni);
    else if (sm_layer)
        sm_leds_layer(nl, ng, ni, nb);
    else if (sm_ui.kbd) {                                      /* the keyboard mode: the octave (the buttons' glow is below) */
        sm_led_put(nl, panel.btn[B_OCTDN], sm_ui.oct < 0);
        sm_led_put(nl, panel.btn[B_OCTUP], sm_ui.oct > 0);
    } else {                                                   /* OCT+ the second page of steps in view, OCT- SHIFT held */
        sm_led_put(nl, panel.btn[B_OCTUP], len > 16u && bank);
        sm_led_put(nl, panel.btn[B_OCTDN], sm_shift != 0u);
        if (t == SM_KIT_TRK && sm_trk_kit(t, pt)) {           /* the KIT: its lane keys, the lane in edit lit, the others a glow */
            uint32_t L;
            for (L = 0; L < SM_NLANE; L++)
                sm_led_put(L == sm_kit_lane % SM_NLANE ? nl : ng, 14u + SM_LANE_KEY[L], 1);
        }
    }
    for (k = 0; k < NB; k++) {
        sm_led_put(ng, panel.btn[k], 1);
        sm_led_put(ni, panel.btn[k], 1);
    }
    sm_led_put(nl, panel.btn[sm_fam_btn(SM_PAGES[sm_ui.page % sm_page_count()].fam)], 1);   /* (the family of the page in view) */
    if (sm_layer)                                         /* the open layer's button breathes: masked out of lit and glow below */
        sm_led_put(nb, panel.btn[sm_layer_button(sm_layer)], 1);
    if (*(volatile uint8_t *)&sm_rec != 0u)               /* REC on: steady while armed (even playing), breathing while live (REC + PLAY) */
        sm_led_put((play && *(volatile uint8_t *)&sm_live) ? nb : nl, panel.btn[B_REC], 1);
    if ((sm_msg == SM_MSG_SAVED || sm_msg == SM_MSG_SAVEPEND || sm_msg == SM_MSG_FLASH) && !smd_blink())
        sm_led_put(nl, panel.btn[B_SAVE], 1);              /* (the save's message: the SAVE button blinks with it) */
    if (play) {                                           /* playing: PLAY's green, its own LED dark in every mode */
        sm_led_clear(ng, panel.btn[B_PLAY]);
        sm_led_clear(ni, panel.btn[B_PLAY]);
    }
    for (c = 0; c < FM1_NCOL; c++) {
        if (mode == LEDS_INV) {                           /* INV: the active ones dark, the rest lit */
            nl[c] = (uint8_t)(ni[c] & ~nl[c]);
            ng[c] = 0;
        } else if (mode != LEDS_DIM && mode != LEDS_DIM_LO) {
            ng[c] = 0;                                    /* OFF: no glow (the keys never had one) */
        }
        nl[c] &= (uint8_t)~(nb[c] | nm[c]);               /* breathing, mid: not lit (INV's rest) */
        ng[c] &= (uint8_t)~nb[c];                         /* a breathing LED has no glow under it */
    }
    if (play)
        nl[LED_PLAY_GREEN >> 3] |= (uint8_t)(1u << (LED_PLAY_GREEN & 7u));
    for (c = 0; c < FM1_NCOL; c++) {
        fm1_led_dim[c] = ng[c];
        fm1_led_breath[c] = nb[c];
        fm1_led_mid[c] = nm[c];
    }
    for (c = 0; c < FM1_NCOL; c++)
        fm1_led[c] = nl[c];
}
