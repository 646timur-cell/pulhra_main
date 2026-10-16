/* SPDX-License-Identifier: GPL-3.0-only */
/* SWAMP MACHINE panel LEDs, after the mechanism of Felucca's ui_leds (ui_input.c): each frame the picture is built off-line,
 * one byte per column (lit / glow / breath / mid), and copied glow first, lit last, so an LED going from lit to glow never
 * has a dark frame for the 10 kHz scan to catch. The LEDs carry the step information (the screen strip stays ascetic):
 *  - white keys, the steps of the visible bank, three states told at a glance: a TRIG (n > 0, no REST) is lit; a REST
 *    (SMF_REST: locks only, no note) breathes; an EMPTY step is fully dark, no glow in any LEDS mode (INV turns it round,
 *    as it turns round every LED: the active ones dark, the rest lit);
 *  - the step playing is a soft running light: the MID level, never a full flash: on an empty step mid, a trig comes down
 *    to mid, a REST has mid in place of its breath; a held step is mid too (and a frame on the screen);
 *  - D#5 / F#5: the page of steps in view is lit (only while the track is longer than 16), the other page glows;
 *  - buttons: the family of the page in view lit, REC lit while recording, PLAY's own LED glows (dark while playing, its
 *    green one lit), the rest glow, as settings_leds (OFF / DIM LO / DIM HI / INV) says. */
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

static void sm_leds(void)
{
    uint8_t nl[FM1_NCOL] = {0}, ng[FM1_NCOL] = {0}, nb[FM1_NCOL] = {0}, nm[FM1_NCOL] = {0}, ni[FM1_NCOL] = {0};
    uint32_t mode = settings_leds, t = sm_ui.trk % SM_NTRK, k, c, w = 0, len, bank = sm_ui.bank ? 16u : 0u;
    uint32_t play = *(volatile uint8_t *)&sm_playing != 0u, ph = *(volatile uint8_t *)&sm_pos[t];
    static uint8_t ready;
    const sm_pattern_t *pt = &smp.pat[smp.cur % SM_NPAT];
    if (!ready) {
        sm_led_pos_init();
        ready = 1;
    }
    len = pt->kit.snd[t].len;
    len = len < 1u ? 1u : len > SM_NSTEP ? SM_NSTEP : len;
    for (k = 0; k < SM_NKEYS; k++) {
        const sm_step_t *st;
        uint32_t s;
        if ((SM_BLACK_KEYS >> k) & 1u)
            continue;
        s = bank + w++;
        st = &pt->step[t][s];
        sm_led_put(ni, 14u + k, 1);
        if ((play && ph == s) || sm_ui.held_step == s)    /* the soft running light: mid, in place of lit and of breath */
            sm_led_put(nm, 14u + k, 1);
        else if (s < len && (st->flags & SMF_REST))
            sm_led_put(nb, 14u + k, 1);
        else if (s < len && st->n)
            sm_led_put(nl, 14u + k, 1);
    }
    if (len > 16u) {                                      /* the two pages of steps: the one in view lit, the other glows */
        sm_led_put(nl, 14u + (bank ? SM_KEY_BANK1 : SM_KEY_BANK0), 1);
        sm_led_put(ng, 14u + (bank ? SM_KEY_BANK0 : SM_KEY_BANK1), 1);
        sm_led_put(ni, 14u + SM_KEY_BANK0, 1);
        sm_led_put(ni, 14u + SM_KEY_BANK1, 1);
    }
    for (k = 0; k < NB; k++) {
        sm_led_put(ng, panel.btn[k], 1);
        sm_led_put(ni, panel.btn[k], 1);
    }
    sm_led_put(nl, panel.btn[sm_cur_page()->fam == SMFAM_EDIT ? B_EDIT : sm_cur_page()->fam == SMFAM_GLO ? B_GLO : B_ENV], 1);
    /* (stage 2 task 3: the button of the open layer breathes here: sm_led_put(nb, panel.btn[layer button], 1)) */
    sm_led_put(nl, panel.btn[B_REC], *(volatile uint8_t *)&sm_rec != 0u);
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
