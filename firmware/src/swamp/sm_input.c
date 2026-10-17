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
 * writes are single bytes the audio stream reads at a fire (a step edited between two fires is whole by bytes).
 * HOME (sm_home_page): MIXER's knobs are the LEVEL of T1..T4 (a base value, no locks), CLOCK's K1 the tempo and K2 the swing,
 * SELECT the tempo on both. smp.bpm10 and smp.swing are written here, in the main loop, 16 and 8 bits at once; the audio
 * stream reads each once per block (sm_seq.c). */
#define SM_HOLD_US 350000u                                /* a key held longer is a select, not a tap */
#define SM_BLACK_KEYS 0x252A52Au                          /* the black keys of F3 .. G5, bit n = key n (ui_input.c) */
#define SM_NKEYS 27u
#define SM_OCT_PAIR_US 100000u                            /* OCT- and OCT+ pressed within this: both together (keyboard mode) */
#define SM_KEY_NOTE0 53u                                  /* F3, the first key's MIDI note (C4 = 60) */
#define SM_KEY_VEL 100u

static uint32_t sm_pdirty[SM_NTRK][4];                    /* bit per parameter (P_COUNT <= 127): pushed again */
static uint8_t sm_ld_pend;                                /* bit per track: a LOAD still to push */
static uint8_t sm_tr_pend;                                /* 0 none, 1 PLAY, 2 STOP still to push */
static uint8_t sm_save_st;                                /* SAVE: 0 up, 1 down (since sm_save_t0), 2 held: UNDO done */
static uint32_t sm_save_t0;
static uint32_t sm_key_t0[SM_NKEYS], sm_key_arm;          /* press time of the white keys down, bit per key */
static uint8_t sm_key_step[SM_NKEYS];                     /* the step each of them was pressed on (a bank key may move on) */
static uint8_t sm_key_trk[SM_NKEYS];                      /* ... and the track */
static uint32_t sm_key_used;                              /* bit per key: a knob turned while it was down (a lock, no toggle) */
static uint8_t sm_oct_pend;                               /* an OCT press waiting for the other one: 0 none, 1 OCT-, 2 OCT+ */
static uint32_t sm_oct_t0;                                /* ... when it went down */
static uint8_t sm_kb_note[SM_NKEYS], sm_kb_trk[SM_NKEYS]; /* keyboard mode: the note each key sounds (0xFF none) and its track */
static uint32_t sm_kb_pend;                               /* bit per key: pressed, its note-on not pushed yet */

#define SM_FAST_US 80000u
#define SM_FAST_N 4u
static uint32_t sm_fast_t0, sm_fast_n;                    /* K1's BPM turn: its window (sm_bpm_accel) */
static int8_t sm_fast_dir;

static void sm_input_init(void)
{
    sm_fast_n = 0;
    sm_fast_dir = 0;
    memset(&sm_ui, 0, sizeof sm_ui);
    sm_ui.held_step = sm_ui.sel_step = 0xFFu;
    memset(sm_pdirty, 0, sizeof sm_pdirty);
    sm_ld_pend = sm_tr_pend = 0;
    sm_save_st = 0;
    sm_key_arm = sm_key_used = 0;
    sm_oct_pend = 0;
    sm_oct_t0 = 0;
    memset(sm_kb_note, 0xFF, sizeof sm_kb_note);
    memset(sm_kb_trk, 0, sizeof sm_kb_trk);
    sm_kb_pend = 0;
    sm_unl_pend = 0;
    sm_rec = 0;
    sm_msg = 0;
    sm_dirty = 1;
}

static void sm_layer_begin(uint32_t pressed, uint32_t now, uint32_t used);   /* sm_layer.c (after this file) */
static void sm_layer_keys(uint32_t pressed, uint32_t notes, const int32_t *k, int32_t alg);
static void sm_layer_end(void);
static uint32_t sm_layer_busy(void);                      /* a layer open, or its button down */
static void sm_undo(void);                                /* sm_edit.c (after this file) */
static void sm_edit_pat_changed(void);

static sm_sound_t *sm_ui_sound(void) { return &smp.pat[smp.cur % SM_NPAT].kit.snd[sm_ui.trk % SM_NTRK]; }

static void sm_param_push(uint32_t t, uint32_t id)
{
    sm_cmd_t c;
    c.kind = SMC_PARAM;
    c.trk = (uint8_t)t;
    c.step = smp.cur;                                     /* (the pattern: sm_cmd.c) */
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

/* track t's base value of parameter id turned by s detents (param_turn: the descriptor's own steps and range) in the model,
 * then the command */
static void sm_turn_base(uint32_t t, uint32_t id, int32_t s)
{
    sm_sound_t *sd = &smp.pat[smp.cur % SM_NPAT].kit.snd[t % SM_NTRK];
    const param_desc_t *d = sm_desc_safe(sm_engine_of(sd->machine), id);
    int32_t v;
    if (!s || !d)
        return;
    v = param_turn(d, sd->p[id], s);
    sd->p[id] = (int8_t)clamp(v, d->min, d->max);        /* (a byte write: the audio stream sees the old or the new) */
    sm_param_push(t % SM_NTRK, id);
    sm_dirty = 1;
}

/* a knob: the model's value turned by s detents, then the command.
 * A white key down: a lock of the held step instead; REC while playing: a lock of the playing step (a trig there only) */
static void sm_turn(uint32_t id, int32_t s)
{
    const param_desc_t *d = sm_desc_safe(sm_engine_of(sm_ui_sound()->machine), id);
    if (!s || !d)
        return;
    if (sm_key_arm) {
        sm_turn_held(id, s);
        return;
    }
    if (sm_rec && sm_playing) {                           /* (the playing step: the audio stream's byte) */
        uint32_t t = sm_ui.trk % SM_NTRK, st = *(volatile uint8_t *)&sm_pos[t];
        if (sm_lock_turn(t, st, id, s, 0)) {                 /* heard at once: the lock onto the core (a refused preview: */
            sm_cmd_t c = {SMC_PREVIEW, (uint8_t)t, smp.cur, (uint8_t)id, 0};   /* the step's next fire plays it) */
            c.val = smp.pat[smp.cur % SM_NPAT].lock[sm_lock_find(&smp.pat[smp.cur % SM_NPAT], t, st, id)].val;
            (void)sm_cmd_push(c);
        }
        return;
    }
    sm_turn_base(sm_ui.trk, id, s);
}

/* FILT OFF (the sound's default) makes CUT, RES, the filter envelope's ENV and its attack and decay do nothing: turning one
 * of them on a track whose FILT is OFF first sets FILT to LP (the model, then the command), so the knob is heard at once.
 * Another FILT (LP BP HP, HYB: K2 / K3 are BASE / RES then) is left as it is */
static void sm_filt_auto(uint32_t id)
{
    sm_sound_t *sd = sm_ui_sound();
    if (sd->p[P_FLT] || (id != P_FCUT && id != P_FRES && id != P_ED_FLT && id != P_FATK && id != P_FDEC))
        return;
    sd->p[P_FLT] = 1;
    sm_param_push(sm_ui.trk % SM_NTRK, P_FLT);
    sm_dirty = 1;
}

/* the main loop's mirror of the pattern playing (the audio stream switches: sm_seq.c sm_pat_switch): smp.cur follows
 * sm_play_pat (the edits go to the pattern playing from here on), and the request is done once it plays */
static uint8_t sm_cpos_seen;                              /* the chain's link last drawn (a link of the same pattern: no switch seen) */
static void sm_pat_poll(void)
{
    uint8_t p = *(volatile uint8_t *)&sm_play_pat, c = sm_cpos;
    if (c != sm_cpos_seen) {
        sm_cpos_seen = c;
        sm_dirty = 1;
    }
    if (p != smp.cur) {
        smp.cur = (uint8_t)(p % SM_NPAT);
        sm_edit_pat_changed();                            /* (UNDO's snapshot was of the pattern before) */
        if (smp.pat[smp.cur].kit.snd[sm_ui.trk % SM_NTRK].len <= 16u)
            sm_ui.bank = 0;
        sm_dirty = 1;
    }
    if (smp.next != SM_PAT_NONE && smp.next == p) {
        smp.next = SM_PAT_NONE;
        sm_dirty = 1;
    }
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

/* the keys: a white key is a step of the bank (F3 .. G5, the 16 white keys; OCT- / OCT+ the bank: sm_oct_poll). Pressed: armed (before
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

/* OCT- / OCT+ outside the layers. Alone: on the pages where the white keys are trigs, the page of steps 1..16 / 17..32 (only
 * on a track longer than 16); in the keyboard mode the octave -1 / +1 (-3 .. +3). Both within SM_OCT_PAIR_US: the keyboard mode
 * on or off; ARP alone does the same. So a single press waits that long for the other before it acts (a page must not flip under a chord press). */
static void sm_oct_single(uint32_t up)
{
    if (sm_ui.kbd) {
        int32_t o = clamp((int32_t)sm_ui.oct + (up ? 1 : -1), -3, 3);
        if ((int8_t)o != sm_ui.oct) {
            sm_ui.oct = (int8_t)o;                        /* (notes sounding keep their pitch: the key's own note-off) */
            sm_dirty = 1;
        }
    } else if (smp.pat[smp.cur % SM_NPAT].kit.snd[sm_ui.trk % SM_NTRK].len > 16u && sm_ui.bank != (uint8_t)up) {
        sm_ui.bank = (uint8_t)up;
        sm_dirty = 1;
    }
}

static void sm_oct_toggle(void)
{
    sm_ui.kbd = (uint8_t)!sm_ui.kbd;
    sm_key_arm = sm_key_used = 0;                         /* (a trig key down at the switch never toggles a trig) */
    sm_ui.held_step = sm_ui.sel_step = 0xFFu;
    sm_dirty = 1;
}

static void sm_oct_poll(uint32_t pressed, uint32_t now)
{
    uint32_t dn = (pressed >> panel.btn[B_OCTDN]) & 1u, up = (pressed >> panel.btn[B_OCTUP]) & 1u;
    if (!sm_layer_busy() && ((pressed >> panel.btn[B_ARP]) & 1u))
        sm_oct_toggle();                                  /* ARP: the keyboard mode too (the arpeggiator pages come in stage 5) */
    if (sm_layer_busy()) {                         /* (the layers use OCT: EDIT's next page; a layer button down may open one) */
        sm_oct_pend = 0;
        return;
    }
    if (sm_oct_pend && (uint32_t)(now - sm_oct_t0) > SM_OCT_PAIR_US * FM1_TICKS_PER_US) {
        sm_oct_single(sm_oct_pend == 2u);                 /* (nobody came with it) */
        sm_oct_pend = 0;
    }
    if (dn && up) {
        sm_oct_toggle();
        sm_oct_pend = 0;
    } else if (dn || up) {
        uint8_t which = (uint8_t)(dn ? 1u : 2u);
        if (sm_oct_pend && sm_oct_pend != which) {
            sm_oct_toggle();
            sm_oct_pend = 0;
        } else {
            if (sm_oct_pend)
                sm_oct_single(sm_oct_pend == 2u);         /* (the same button twice inside the window) */
            sm_oct_pend = which;
            sm_oct_t0 = now;
        }
    }
}

/* the keyboard mode: a key pressed plays its note (F3 + the key + 12 octaves) on the track selected then, released lets it go;
 * out of the mode every note still sounding is let go. The commands are pushed from the state each poll, so a refused one
 * (queue full) goes again on the next. edges: this poll's presses (none in a layer) */
static void sm_kbd_poll(uint32_t edges)
{
    uint32_t k, held = fm1_in.notes;
    for (k = 0; k < SM_NKEYS; k++) {
        uint32_t bit = 1u << k;
        if (sm_ui.kbd && (edges & bit))
            sm_kb_pend |= bit;
        if (!sm_ui.kbd)
            sm_kb_pend &= ~bit;
        if (sm_kb_note[k] == 0xFFu) {
            if (sm_kb_pend & bit) {
                sm_cmd_t c = {SMC_KEYON, (uint8_t)(sm_ui.trk % SM_NTRK), 0, SM_KEY_VEL, 0};
                c.step = (uint8_t)clamp((int32_t)(SM_KEY_NOTE0 + k) + 12 * sm_ui.oct, 0, 127);
                if (sm_cmd_push(c)) {
                    sm_kb_note[k] = c.step;
                    sm_kb_trk[k] = c.trk;
                    sm_kb_pend &= ~bit;
                }
            }
        } else if (!sm_ui.kbd || !(held & bit)) {
            sm_cmd_t c = {SMC_KEYOFF, sm_kb_trk[k], sm_kb_note[k], 0, 0};
            uint32_t j, same = 0;
            for (j = 0; j < SM_NKEYS; j++)               /* (another key sounds the same note on the track: its off is the last) */
                same |= j != k && sm_kb_note[j] == c.step && sm_kb_trk[j] == c.trk;
            if (same || sm_cmd_push(c))
                sm_kb_note[k] = 0xFFu;
        }
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

/* the FLAG list turned by pre places on step st of track t (the STEP page, and a held trig on any other sound page). An empty
 * step: PRESETS makes it a REST trig (sm_step_toggle's order: fields, barrier, n; the place REST). Returns 1 if the step is a trig */
static int sm_step_flag(uint32_t t, int32_t st, int32_t pre)
{
    sm_step_t *s = &smp.pat[smp.cur % SM_NPAT].step[t % SM_NTRK][st];
    if (!s->n) {
        if (pre) {
            sm_lock_clear_step(t, (uint32_t)st);
            sm_step_fresh(t, (uint32_t)st, SMF_REST);
            SM_BARRIER();
            s->n = 1;
        }
        return 0;
    }
    if (pre) {
        int32_t q = ((int32_t)sm_flag_place(s->flags) + pre) % (int32_t)SM_NFLAG;
        s->flags = SM_FLAGV[q < 0 ? q + (int32_t)SM_NFLAG : q];
    }
    return 1;
}

/* a trig held on this track (the pressed last): its step, -1 none. Not the selected step: only a key down */
static int32_t sm_trig_held(void)
{
    uint32_t k, last = SM_NKEYS;
    for (k = 0; k < SM_NKEYS; k++)
        if (((sm_key_arm >> k) & 1u) && sm_key_trk[k] == sm_ui.trk &&
            (last == SM_NKEYS || (int32_t)(sm_key_t0[k] - sm_key_t0[last]) >= 0))
            last = k;
    return last < SM_NKEYS ? (int32_t)sm_key_step[last] : -1;
}

/* the STEP page's knobs onto the step edited: K1 NOTE 0..127 (C-1 .. G9), K2 VEL 1..127, K3 LEN (the list's index),
 * K4 COND (its walk), SELECT NUDGE -8..+7, PRESETS the FLAG list round (sm_step_flag). An empty step: PRESETS a REST trig, the
 * rest nothing */
static void sm_step_page(const int32_t *k, int32_t sel, int32_t pre)
{
    int32_t st = sm_step_edit();
    uint32_t t = sm_ui.trk % SM_NTRK;
    sm_step_t *s;
    if (st < 0 || (!pre && !sel && !k[0] && !k[1] && !k[2] && !k[3]))
        return;
    s = &smp.pat[smp.cur % SM_NPAT].step[t][st];
    sm_dirty = 1;
    if (!sm_step_flag(t, st, pre))
        return;
    s->note[0] = (uint8_t)clamp((int32_t)s->note[0] + k[0], 0, 127);
    s->vel = (uint8_t)clamp((int32_t)s->vel + k[1], 1, 127);
    s->len = (uint8_t)clamp((int32_t)s->len + k[2], 0, (int32_t)SM_NLEN - 1);
    if (k[3])
        s->cond = (uint8_t)sm_cond_of_place((uint32_t)clamp((int32_t)sm_cond_place(s->cond) + k[3], 0, (int32_t)SM_COND_PLACES - 1));
    s->nudge = (int8_t)clamp((int32_t)s->nudge + sel, -8, 7);
}

/* the tempo turned: s units of 0.1 BPM, 30.0 .. 300.0 (the main loop's alone to write) */
static void sm_bpm_turn(int32_t s)
{
    int32_t v = clamp((int32_t)smp.bpm10 + s, 300, 3000);
    if (s && (uint16_t)v != smp.bpm10) {
        smp.bpm10 = (uint16_t)v;
        sm_dirty = 1;
    }
}

/* K1's BPM turn is timed (the poll runs back to back, so detents seldom pile up inside one poll): the detents of a turn in
 * one direction are counted in a window that opens at the first of them and lasts SM_FAST_US; from the 4th detent in it
 * each detent is 1.0 BPM (x10, a flick); before that, and in a slow turn (the window runs out between detents), 0.1.
 * A reversal opens a new window. */
static int32_t sm_bpm_accel(int32_t s, uint32_t now)
{
    int8_t dir = (int8_t)(s < 0 ? -1 : 1);
    uint32_t a = (uint32_t)(s < 0 ? -s : s), slow;
    if (!s)
        return 0;
    if (dir != sm_fast_dir || (uint32_t)(now - sm_fast_t0) > SM_FAST_US * FM1_TICKS_PER_US) {
        sm_fast_dir = dir;
        sm_fast_t0 = now;
        sm_fast_n = 0;
    }
    slow = sm_fast_n < SM_FAST_N - 1u ? SM_FAST_N - 1u - sm_fast_n : 0u;   /* (the detents that bring the count to 3 stay 0.1) */
    slow = slow > a ? a : slow;
    sm_fast_n += a;
    return dir * (int32_t)(slow + (a - slow) * 10u);
}

/* the HOME pages' knobs. MIXER: K1..K4 the LEVEL of T1..T4. CLOCK: K1 BPM (0.1 a detent, 1.0 in a quick turn:
 * sm_bpm_accel), K2 SWING 50 .. 75 %. SELECT: the tempo, 0.1 a detent. */
static void sm_home_page(const int32_t *k, int32_t sel, uint32_t now)
{
    uint32_t i;
    if (sm_page_home() == SMH_MIXER) {
        for (i = 0; i < SM_NTRK; i++)
            sm_turn_base(i, P_LEVEL, k[i]);
    } else {
        sm_bpm_turn(sm_bpm_accel(k[0], now));
        if (k[1]) {
            int32_t v = clamp((int32_t)smp.swing + k[1], 50, 75);
            if ((uint8_t)v != smp.swing) {
                smp.swing = (uint8_t)v;
                sm_dirty = 1;
            }
        }
    }
    sm_bpm_turn(sel);
}

/* SAVE held (HOLD_MS, as a layer's hold): UNDO, once a hold (a tap: the SAVE page, a later stage) */
static void sm_save_poll(uint32_t pressed, uint32_t now)
{
    uint32_t b = panel.btn[B_SAVE];
    if ((pressed >> b) & 1u) {
        sm_save_st = 1;
        sm_save_t0 = now;
    }
    if (!((fm1_in.buttons >> b) & 1u)) {
        sm_save_st = 0;
    } else if (sm_save_st == 1u &&
               (uint32_t)(now - sm_save_t0) >= (uint32_t)HOLD_MS[settings_hold % 4u] * 1000u * FM1_TICKS_PER_US) {
        sm_save_st = 2;
        sm_undo();
    }
}

/* everything the queue refused so far, again */
static void sm_flush(void)
{
    uint32_t t, w, b;
    for (t = 0; t < SM_NTRK; t++) {
        if ((sm_ld_pend >> t) & 1u) {
            sm_cmd_t c = {SMC_LOAD, (uint8_t)t, smp.cur, 0, 0};
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
    sm_pat_poll();
    /* the family buttons (EDIT GLO SEQ: a tap is the page, a hold the layer; ENV: the page): a key or a knob used while one is
     * down is a layer too; in an open layer the keys and the four knobs and SELECT are the layer's, not the page's */
    sm_layer_begin(pressed, now, notes || sel || alg || pre || k[0] || k[1] || k[2] || k[3]);
    if (sm_layer) {
        sm_layer_keys(pressed, notes, k, alg);
        if (sm_layer == SML_SEQ)
            alg = 0;                                      /* (SEQ: ALGORITHM picks a pattern, not the track) */
        notes = 0;
        sel = 0;
        for (i = 0; i < 4; i++)
            k[i] = 0;
    }
    sm_layer_end();
    sm_msg_poll(now);
    sm_save_poll(pressed, now);
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
    sm_oct_poll(pressed, now);
    sm_kbd_poll(sm_ui.kbd ? notes : 0);
    if (!sm_ui.kbd)                                       /* (the keyboard mode: the keys play, they are no trigs) */
        sm_keys_press(notes, now);
    if (pre && !sm_page_is_step() && !sm_page_home() && sm_trig_held() >= 0) {   /* a trig held on a sound page: PRESETS is its FLAG */
        int32_t hs = sm_trig_held();
        sm_key_used |= sm_key_arm;
        sm_step_flag(sm_ui.trk % SM_NTRK, hs, pre);
        sm_ui.held_step = sm_ui.sel_step = (uint8_t)hs;
        sm_dirty = 1;
        pre = 0;
    }
    if (sel || k[0] || k[1] || k[2] || k[3] || (pre && sm_page_is_step()))   /* (any of them, a knob without a parameter */
        sm_key_used |= sm_key_arm;                                           /* too: no toggle) */
    if (sm_page_is_step()) {
        sm_step_page(k, sel, pre);
    } else if (sm_page_home()) {
        sm_preset_turn(pre);
        sm_home_page(k, sel, now);
    } else {
        sm_preset_turn(pre);
        for (i = 0; i < 4; i++)
            if (sm_page_param(i) != SM_NOPARAM && k[i]) {
                sm_filt_auto(sm_page_param(i));
                sm_turn(sm_page_param(i), k[i]);
            }
        if (sm_page_param(4) != SM_NOPARAM)
            sm_turn(sm_page_param(4), sel);
    }
    sm_keys_release(now);
    sm_flush();
}
