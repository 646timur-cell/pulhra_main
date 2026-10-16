/* SPDX-License-Identifier: GPL-3.0-only */
/* SWAMP MACHINE input, stage 1: buttons, knobs and keys into the model. Ownership: the main loop (this file) is the ONLY
 * writer of the model (smp); the audio stream owns trk[] and takes from the model only what a command names. So a knob
 * turn writes the model first (clamped to the parameter's range), then pushes SMC_PARAM; a preset turn writes the whole
 * sound, then pushes SMC_LOAD; a trig is sm_step_toggle (no command). sm_cmd_push returns 0 when the queue is full:
 *  - a refused SMC_PARAM is in the model already: its {trk, param} stays marked (sm_pdirty) and every poll pushes the
 *    marked values again until they are taken (the value is read from the model then, the newest);
 *  - a refused SMC_LOAD / PLAY / STOP is kept as a flag (sm_ld_pend, sm_tr_pend) and pushed again on every poll.
 * sm_input_poll is called without a pause (the wait at the end of the frame), so a retry is a few hundred microseconds
 * away. A sound is written to the model with the audio interrupt off for the copy (upreset.c does the same): a LOAD
 * still queued from a turn a block ago must not drain half a sound. */
#define SM_HOLD_US 350000u                                /* a key held longer is a select, not a tap */
#define SM_BLACK_KEYS 0x252A52Au                          /* the black keys of F3 .. G5, bit n = key n (ui_input.c) */
#define SM_KEY_BANK0 22u                                  /* D#5: steps 0..15 */
#define SM_KEY_BANK1 25u                                  /* F#5: steps 16..31 */
#define SM_NKEYS 27u

static uint32_t sm_pdirty[SM_NTRK][4];                    /* bit per parameter (P_COUNT <= 127): pushed again */
static uint8_t sm_ld_pend;                                /* bit per track: a LOAD still to push */
static uint8_t sm_tr_pend;                                /* 0 none, 1 PLAY, 2 STOP still to push */
static uint32_t sm_key_t0[SM_NKEYS], sm_key_arm;          /* press time of the white keys down, bit per key */
static uint8_t sm_key_step[SM_NKEYS];                     /* the step each of them was pressed on (a bank key may move on) */

static void sm_input_init(void)
{
    memset(&sm_ui, 0, sizeof sm_ui);
    sm_ui.held_step = 0xFFu;
    memset(sm_pdirty, 0, sizeof sm_pdirty);
    sm_ld_pend = sm_tr_pend = 0;
    sm_key_arm = 0;
    sm_dirty = 1;
}

static sm_sound_t *sm_ui_sound(void) { return &smp.pat[smp.cur % SM_NPAT].kit.snd[sm_ui.trk % SM_NTRK]; }

static void sm_param_push(uint32_t t, uint32_t id)
{
    sm_cmd_t c;
    c.kind = SMC_PARAM;
    c.trk = (uint8_t)t;
    c.step = 0;
    c.param = (uint8_t)id;
    c.val = smp.pat[smp.cur % SM_NPAT].kit.snd[t].p[id];
    if (sm_cmd_push(c))
        sm_pdirty[t][id >> 5] &= ~(1u << (id & 31u));
    else
        sm_pdirty[t][id >> 5] |= 1u << (id & 31u);
}

/* a knob: the model's value turned by s detents (param_turn: the descriptor's own steps and range), then the command */
static void sm_turn(uint32_t id, int32_t s)
{
    sm_sound_t *sd = sm_ui_sound();
    const param_desc_t *d = sm_desc_safe(sm_engine_of(sd->machine), id);
    int32_t v;
    if (!s || !d)
        return;
    v = param_turn(d, sd->p[id], s);
    sd->p[id] = (int8_t)clamp(v, d->min, d->max);        /* (a byte write: the audio stream sees the old or the new) */
    sm_param_push(sm_ui.trk, id);
    sm_dirty = 1;
}

/* PRESETS: the next / previous preset of the track's machine (wraps), the whole sound from it, then the LOAD */
static void sm_preset_turn(int32_t s)
{
    sm_sound_t *sd = sm_ui_sound(), n;
    uint32_t np = ENGINES[sm_engine_of(sd->machine)]->npresets;
    int32_t p;
    if (!s || !np)
        return;
    p = ((int32_t)sd->preset + s) % (int32_t)np;
    if (p < 0)
        p += (int32_t)np;
    sm_sound_from_preset(&n, sd->machine, (uint32_t)p);
    fm1_irq_off();
    *sd = n;
    fm1_irq_on();
    sm_ld_pend |= (uint8_t)(1u << sm_ui.trk);
    sm_dirty = 1;
}

/* the keys: a white key is a step of the bank (F3 .. G5, the 16 white keys), D#5 / F#5 the bank. A tap toggles the trig
 * at once; a key held past SM_HOLD_US only records the step (held_step) */
static int32_t sm_white_of(uint32_t key)
{
    uint32_t k, w = 0;
    if (key >= SM_NKEYS || ((SM_BLACK_KEYS >> key) & 1u))
        return -1;
    for (k = 0; k < key; k++)
        w += !((SM_BLACK_KEYS >> k) & 1u);
    return (int32_t)w;
}

static void sm_keys(uint32_t notes, uint32_t now)
{
    uint32_t k, held = fm1_in.notes, hs = 0xFFu;
    for (k = 0; k < SM_NKEYS; k++) {
        int32_t w;
        if (!((notes >> k) & 1u))
            continue;
        if (k == SM_KEY_BANK0 || k == SM_KEY_BANK1) {
            sm_ui.bank = (uint8_t)(k == SM_KEY_BANK1);
            sm_dirty = 1;
            continue;
        }
        w = sm_white_of(k);
        if (w < 0)
            continue;
        sm_key_step[k] = (uint8_t)((uint32_t)sm_ui.bank * 16u + (uint32_t)w);
        sm_step_toggle(sm_ui.trk, sm_key_step[k]);
        sm_key_t0[k] = now;
        sm_key_arm |= 1u << k;
        sm_dirty = 1;
    }
    for (k = 0; k < SM_NKEYS; k++) {
        if (!((sm_key_arm >> k) & 1u))
            continue;
        if (!((held >> k) & 1u)) {
            sm_key_arm &= ~(1u << k);
            continue;
        }
        if ((uint32_t)(now - sm_key_t0[k]) > SM_HOLD_US * FM1_TICKS_PER_US)
            hs = sm_key_step[k];
    }
    if (hs != sm_ui.held_step) {
        sm_ui.held_step = (uint8_t)hs;
        sm_dirty = 1;
    }
}

/* everything the queue refused so far, again */
static void sm_flush(void)
{
    uint32_t t, w, b;
    for (t = 0; t < SM_NTRK; t++) {
        if ((sm_ld_pend >> t) & 1u) {
            sm_cmd_t c = {SMC_LOAD, (uint8_t)t, 0, 0, 0};
            if (sm_cmd_push(c)) {
                sm_ld_pend &= (uint8_t)~(1u << t);
                for (w = 0; w < 4u; w++)
                    sm_pdirty[t][w] = 0;                  /* (the sound carries every value of the model) */
            }
            continue;                                     /* (the values wait for the sound: a LOAD changes the engine) */
        }
        for (w = 0; w < 4u; w++)
            for (b = 0; sm_pdirty[t][w] && b < 32u; b++)
                if ((sm_pdirty[t][w] >> b) & 1u)
                    sm_param_push(t, w * 32u + b);
    }
    if (sm_tr_pend) {
        sm_cmd_t c = {sm_tr_pend == 1u ? SMC_PLAY : SMC_STOP, 0, 0, 0, 0};
        if (sm_cmd_push(c))
            sm_tr_pend = 0;
    }
}

/* (never with the interrupts masked: sm_preset_turn's fm1_irq_on is an unconditional sti) */
static void sm_input_poll(void)
{
    uint32_t pressed = fm1_input_edges(0), notes = fm1_input_note_edges(), now = fm1_ticks();
    int32_t sel = panel_enc(EN_SELECT), alg = panel_enc(EN_ALGO), pre = panel_enc(EN_PRESET), k[4], i;
    for (i = 0; i < 4; i++)
        k[i] = panel_enc(EN_K1 + (uint32_t)i);
    if ((pressed >> panel.btn[B_EDIT]) & 1u)
        sm_page_press(SMFAM_EDIT);
    if ((pressed >> panel.btn[B_GLO]) & 1u)
        sm_page_press(SMFAM_GLO);
    if ((pressed >> panel.btn[B_ENV]) & 1u)
        sm_page_press(SMFAM_ENV);
    if ((pressed >> panel.btn[B_PLAY]) & 1u)              /* (against a PLAY / STOP not pushed yet, if there is one) */
        sm_tr_pend = (uint8_t)(sm_tr_pend ? (sm_tr_pend == 1u ? 2u : 1u) : (sm_playing ? 2u : 1u));
    if (alg) {
        int32_t t = ((int32_t)sm_ui.trk + alg) % (int32_t)SM_NTRK;
        sm_ui.trk = (uint8_t)(t < 0 ? t + (int32_t)SM_NTRK : t);
        sm_ui.held_step = 0xFFu;
        sm_dirty = 1;
    }
    sm_preset_turn(pre);
    for (i = 0; i < 4; i++)
        if (sm_cur_page()->id[i] != SM_NOPARAM)
            sm_turn(sm_cur_page()->id[i], k[i]);
    if (sm_cur_page()->sel != SM_NOPARAM)
        sm_turn(sm_cur_page()->sel, sel);
    sm_keys(notes, now);
    sm_flush();
}
