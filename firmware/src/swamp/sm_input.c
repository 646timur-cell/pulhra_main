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
 * still queued from a turn a block ago must not drain half a sound.
 * Locks (sm_lock.c): a white key down holds its step; a knob or SELECT turned while it is down locks that parameter on the
 * step (from the lock's value, else the base; the base stays) and the key's release then does not toggle; a release with
 * no turn toggles the trig (taking it off removes its locks). Held on an EMPTY step a turn makes a REST trig with that
 * lock (a trigless lock; no toggle). REC on and playing, no key down: a turn locks the selected track's playing step,
 * only if it has a trig; the base stays.
 * The STEP page (SEQ tapped): the knobs edit a step's fields, not the sound (sm_step_page): the step of the white key
 * down on the selected track (the last pressed), else the step selected last (sm_ui.sel_step); none: nothing. Field
 * writes are single bytes the audio stream reads at a fire (a step edited between two fires is whole by bytes). */
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
static uint8_t sm_key_trk[SM_NKEYS];                      /* ... and the track */
static uint32_t sm_key_used;                              /* bit per key: a knob turned while it was down (a lock, no toggle) */

static void sm_input_init(void)
{
    memset(&sm_ui, 0, sizeof sm_ui);
    sm_ui.held_step = sm_ui.sel_step = 0xFFu;
    memset(sm_pdirty, 0, sizeof sm_pdirty);
    sm_ld_pend = sm_tr_pend = 0;
    sm_key_arm = sm_key_used = 0;
    sm_unl_pend = 0;
    sm_rec = 0;
    sm_lock_msg = 0;
    sm_dirty = 1;
}

static void sm_layer_begin(uint32_t pressed, uint32_t now, uint32_t used);   /* sm_layer.c (after this file) */
static void sm_layer_keys(uint32_t pressed, uint32_t notes, const int32_t *k);
static void sm_layer_end(void);

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

/* a knob with white keys down: the lock of every held step (the last one pressed is the one the screen shows, at once) */
static void sm_turn_held(uint32_t id, int32_t s)
{
    uint32_t k, last = SM_NKEYS;
    for (k = 0; k < SM_NKEYS; k++) {
        if (!((sm_key_arm >> k) & 1u))
            continue;
        sm_key_used |= 1u << k;
        (void)sm_lock_turn(sm_key_trk[k], sm_key_step[k], id, s, 1);
        if (last == SM_NKEYS || (int32_t)(sm_key_t0[k] - sm_key_t0[last]) >= 0)
            last = k;
    }
    if (sm_key_trk[last] == sm_ui.trk && sm_ui.held_step != sm_key_step[last]) {
        sm_ui.held_step = sm_ui.sel_step = sm_key_step[last];
        sm_dirty = 1;
    }
}

/* a knob: the model's value turned by s detents (param_turn: the descriptor's own steps and range), then the command.
 * A white key down: a lock of the held step instead; REC while playing: a lock of the playing step (a trig there only) */
static void sm_turn(uint32_t id, int32_t s)
{
    sm_sound_t *sd = sm_ui_sound();
    const param_desc_t *d = sm_desc_safe(sm_engine_of(sd->machine), id);
    int32_t v;
    if (!s || !d)
        return;
    if (sm_key_arm) {
        sm_turn_held(id, s);
        return;
    }
    if (sm_rec && sm_playing) {                           /* (the playing step: the audio stream's byte) */
        uint32_t t = sm_ui.trk % SM_NTRK, st = *(volatile uint8_t *)&sm_pos[t];
        if (sm_lock_turn(t, st, id, s, 0)) {                 /* heard at once: the lock onto the core (a refused preview: */
            sm_cmd_t c = {SMC_PREVIEW, (uint8_t)t, 0, (uint8_t)id, 0};   /* the step's next fire plays it) */
            c.val = smp.pat[smp.cur % SM_NPAT].lock[sm_lock_find(&smp.pat[smp.cur % SM_NPAT], t, st, id)].val;
            (void)sm_cmd_push(c);
        }
        return;
    }
    v = param_turn(d, sd->p[id], s);
    sd->p[id] = (int8_t)clamp(v, d->min, d->max);        /* (a byte write: the audio stream sees the old or the new) */
    sm_param_push(sm_ui.trk, id);
    sm_dirty = 1;
}

/* the selected track's whole sound from a machine's preset, then the LOAD: built here, off to the side, then copied into the
 * model with the audio interrupt off for the copy only (a LOAD still queued must not drain half a sound). The track's
 * length is the sequencer's, not the sound's: it stays. Used by the PRESETS knob and the EDIT layer */
static void sm_sound_load(uint32_t machine, uint32_t preset)
{
    sm_sound_t *sd = sm_ui_sound(), n;
    sm_sound_from_preset(&n, machine, preset);
    n.len = sd->len;
    fm1_irq_off();
    *sd = n;
    fm1_irq_on();
    sm_ld_pend |= (uint8_t)(1u << sm_ui.trk);
    sm_dirty = 1;
}

/* PRESETS: the next / previous preset of the track's machine (wraps), the whole sound from it, then the LOAD */
static void sm_preset_turn(int32_t s)
{
    sm_sound_t *sd = sm_ui_sound();
    uint32_t np = ENGINES[sm_engine_of(sd->machine)]->npresets;
    int32_t p;
    if (!s || !np)
        return;
    p = ((int32_t)sd->preset + s) % (int32_t)np;
    if (p < 0)
        p += (int32_t)np;
    sm_sound_load(sd->machine, (uint32_t)p);
}

/* the keys: a white key is a step of the bank (F3 .. G5, the 16 white keys), D#5 / F#5 the bank. Pressed: armed (before
 * the knobs of the poll, so a turn in the same poll is a lock of it). Let go: the trig toggles only if it was down at most
 * SM_HOLD_US and no knob or SELECT turned meanwhile (a hold is a look at the step's locks, not a toggle); a key held past
 * SM_HOLD_US, or turned, is the step shown (held_step) */
static int32_t sm_white_of(uint32_t key)
{
    uint32_t k, w = 0;
    if (key >= SM_NKEYS || ((SM_BLACK_KEYS >> key) & 1u))
        return -1;
    for (k = 0; k < key; k++)
        w += !((SM_BLACK_KEYS >> k) & 1u);
    return (int32_t)w;
}

static void sm_keys_press(uint32_t notes, uint32_t now)
{
    uint32_t k;
    for (k = 0; k < SM_NKEYS; k++) {
        int32_t w;
        if (!((notes >> k) & 1u))
            continue;
        if (k == SM_KEY_BANK0 || k == SM_KEY_BANK1) {
            if (smp.pat[smp.cur].kit.snd[sm_ui.trk].len <= 16u)
                continue;                                 /* (no second page of steps while the track is 16 long) */
            sm_ui.bank = (uint8_t)(k == SM_KEY_BANK1);
            sm_dirty = 1;
            continue;
        }
        w = sm_white_of(k);
        if (w < 0)
            continue;
        if ((sm_key_arm >> k) & 1u)                       /* (down again before its release was seen: that release first) */
            continue;
        sm_key_step[k] = (uint8_t)((uint32_t)sm_ui.bank * 16u + (uint32_t)w);
        sm_key_trk[k] = sm_ui.trk;
        sm_key_t0[k] = now;
        sm_key_arm |= 1u << k;
        sm_key_used &= ~(1u << k);
    }
}

static void sm_keys_release(uint32_t now)
{
    uint32_t k, held = fm1_in.notes, hs = 0xFFu;
    for (k = 0; k < SM_NKEYS; k++) {
        if (!((sm_key_arm >> k) & 1u))
            continue;
        if (!((held >> k) & 1u)) {
            sm_key_arm &= ~(1u << k);
            if (!((sm_key_used >> k) & 1u) && (uint32_t)(now - sm_key_t0[k]) <= SM_HOLD_US * FM1_TICKS_PER_US)
                sm_step_toggle(sm_key_trk[k], sm_key_step[k]);   /* (off: its locks go too) */
            sm_key_used &= ~(1u << k);
            sm_dirty = 1;
            continue;
        }
        if (sm_key_trk[k] == sm_ui.trk &&
            (((sm_key_used >> k) & 1u) || (uint32_t)(now - sm_key_t0[k]) > SM_HOLD_US * FM1_TICKS_PER_US))
            hs = sm_key_step[k];
    }
    if (hs != sm_ui.held_step) {
        sm_ui.held_step = (uint8_t)hs;
        if (hs < SM_NSTEP)
            sm_ui.sel_step = (uint8_t)hs;
        sm_dirty = 1;
    }
}

/* the step the STEP page edits on the selected track: the white key down on it pressed last, else the step selected last;
 * -1 none */
static int32_t sm_step_edit(void)
{
    uint32_t k, last = SM_NKEYS;
    for (k = 0; k < SM_NKEYS; k++)
        if (((sm_key_arm >> k) & 1u) && sm_key_trk[k] == sm_ui.trk &&
            (last == SM_NKEYS || (int32_t)(sm_key_t0[k] - sm_key_t0[last]) >= 0))
            last = k;
    if (last < SM_NKEYS)
        return sm_key_step[last];
    return sm_ui.sel_step < SM_NSTEP ? (int32_t)sm_ui.sel_step : -1;
}

/* the STEP page's knobs onto the step edited: K1 NOTE 0..127 (C-1 .. G9), K2 VEL 1..127, K3 LEN (the list's index),
 * K4 COND (its walk), SELECT NUDGE -8..+7, PRESETS the FLAG list round. An empty step: PRESETS makes it a REST trig
 * (sm_step_toggle's order: fields, barrier, n; the place REST), the rest nothing */
static void sm_step_page(const int32_t *k, int32_t sel, int32_t pre)
{
    int32_t st = sm_step_edit();
    uint32_t t = sm_ui.trk % SM_NTRK;
    sm_step_t *s;
    if (st < 0 || (!pre && !sel && !k[0] && !k[1] && !k[2] && !k[3]))
        return;
    s = &smp.pat[smp.cur % SM_NPAT].step[t][st];
    sm_dirty = 1;
    if (!s->n) {
        if (pre) {
            sm_lock_clear_step(t, (uint32_t)st);
            sm_step_fresh(t, (uint32_t)st, SMF_REST);
            SM_BARRIER();
            s->n = 1;
        }
        return;
    }
    if (pre) {
        int32_t q = ((int32_t)sm_flag_place(s->flags) + pre) % (int32_t)SM_NFLAG;
        s->flags = SM_FLAGV[q < 0 ? q + (int32_t)SM_NFLAG : q];
    }
    s->note[0] = (uint8_t)clamp((int32_t)s->note[0] + k[0], 0, 127);
    s->vel = (uint8_t)clamp((int32_t)s->vel + k[1], 1, 127);
    s->len = (uint8_t)clamp((int32_t)s->len + k[2], 0, (int32_t)MF_NLEN - 1);
    if (k[3])
        s->cond = (uint8_t)sm_cond_of_place((uint32_t)clamp((int32_t)sm_cond_place(s->cond) + k[3], 0, (int32_t)SM_COND_PLACES - 1));
    s->nudge = (int8_t)clamp((int32_t)s->nudge + sel, -8, 7);
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
    sm_unlock_flush();
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
    /* the family buttons (EDIT GLO SEQ: a tap is the page, a hold the layer; ENV: the page): a key or a knob used while one is
     * down is a layer too; in an open layer the keys and the four knobs and SELECT are the layer's, not the page's */
    sm_layer_begin(pressed, now, notes || sel || alg || pre || k[0] || k[1] || k[2] || k[3]);
    if (sm_layer) {
        sm_layer_keys(pressed, notes, k);
        notes = 0;
        sel = 0;
        for (i = 0; i < 4; i++)
            k[i] = 0;
    }
    sm_layer_end();
    sm_lock_msg_poll(now);
    if ((pressed >> panel.btn[B_REC]) & 1u) {
        sm_rec = (uint8_t)!sm_rec;
        sm_dirty = 1;
    }
    if ((pressed >> panel.btn[B_PLAY]) & 1u)              /* (against a PLAY / STOP not pushed yet, if there is one) */
        sm_tr_pend = (uint8_t)(sm_tr_pend ? (sm_tr_pend == 1u ? 2u : 1u) : (sm_playing ? 2u : 1u));
    if (alg) {
        int32_t t = ((int32_t)sm_ui.trk + alg) % (int32_t)SM_NTRK;
        sm_ui.trk = (uint8_t)(t < 0 ? t + (int32_t)SM_NTRK : t);
        sm_ui.held_step = sm_ui.sel_step = 0xFFu;
        sm_dirty = 1;
    }
    sm_keys_press(notes, now);
    if (sel || k[0] || k[1] || k[2] || k[3] || (pre && sm_page_is_step()))   /* (any of them, a knob without a parameter */
        sm_key_used |= sm_key_arm;                                           /* too: no toggle) */
    if (sm_page_is_step()) {
        sm_step_page(k, sel, pre);
    } else {
        sm_preset_turn(pre);
        for (i = 0; i < 4; i++)
            if (sm_cur_page()->id[i] != SM_NOPARAM)
                sm_turn(sm_cur_page()->id[i], k[i]);
        if (sm_cur_page()->sel != SM_NOPARAM)
            sm_turn(sm_cur_page()->sel, sel);
    }
    sm_keys_release(now);
    sm_flush();
}
