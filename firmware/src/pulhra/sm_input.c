/* SPDX-License-Identifier: GPL-3.0-only */
/* PULHRA input, stage 1: buttons, knobs and keys into the model. Ownership: the main loop (this file) is the ONLY
 * writer of the model (smp); the audio stream owns trk[] and takes from the model only what a command names. So a knob
 * turn writes the model first (clamped to the parameter's range), then pushes SMC_PARAM; a preset turn writes the whole
 * sound, then pushes SMC_LOAD; a trig is sm_step_toggle (no command). sm_cmd_push returns 0 when the queue is full:
 *  - a refused SMC_PARAM is in the model already: its {trk, param} stays marked (sm_pdirty) and every poll pushes the
 *    marked values again until they are taken (the value is read from the model then, the newest);
 *  - a refused SMC_LOAD / PLAY / STOP is kept as a flag (sm_ld_pend, sm_tr_pend) and pushed again on every poll.
 * sm_input_poll is called without a pause (the wait at the end of the frame), so a retry is a few hundred microseconds
 * away. A sound is written to the model with the audio interrupt off for the copy (upreset.c does the same): a LOAD
 * still queued from a turn a block ago must not drain half a sound.
 * REC is grid record (Elektron's): REC off the white keys on the trig pages do nothing to the steps (no toggle, no lock, no
 * step held or selected; the LEDs still show the trigs) and the knobs are the track's base. REC on:
 * Locks (sm_lock.c): a white key down holds its step; a knob or SELECT turned while it is down locks that parameter on the
 * step (from the lock's value, else the base; the base stays) and the key's release then does not toggle; a release with
 * no turn toggles the trig (taking it off removes its locks). Held on an EMPTY step a turn makes a REST trig with that
 * lock (a trigless lock; no toggle). No key down a turn is the base, always; REC on and playing it is also recorded: the
 * selected track's playing step, only if it has a trig, locks the new base value.
 * LIVE RECORD (REC on, playing, the keyboard mode): a key plays its note and is written as a trig of the selected track at the
 * nearest step (the audio stream picks it: sm_seq.c sm_rec_on) with its note, the keyboard's velocity and, at its release,
 * the LEN of the PULHRA list nearest to how long it was held; notes within one step make a chord (up to 4). MIDI notes on the
 * selected track's channel record the same way (REC on and playing, any mode). sm_lrec_poll writes them (sm_step_rec).
 * The STEP page (SEQ tapped): the knobs edit a step's fields, not the sound (sm_step_page): the step of the white key
 * down on the selected track (the last pressed), else the step selected last (sm_ui.sel_step); none (a SEQ tap drops the
 * selection): the track's trig defaults, which every field a step never had edited (SM_INH) plays. Field writes are single
 * bytes the audio stream reads at a fire (a step edited between two fires is whole by bytes).
 * SHIFT (OCT- held outside the keyboard mode: sm_shift) + a trig key: that trig copied (sm_edit.c sm_trig_copy; no toggle, no
 * hold; REC on or off); a trig held (REC on) + OCT+: the copy pasted onto it. SHIFT + a knob: the turn accelerated x4 / x8 (sm_accel); SHIFT + a family button / SAVE / D#4 F#4 D#5 F#5: the combos (sm_layer.c sm_shift_combo,
 * sm_save_poll, sm_shift_keys). ARP held + F3: the keyboard notes' LATCH (sm_latch_toggle); HOME held + F3 .. B3: the track (sm_layer.c sm_hold_keys).
 * The STEP page: OCT- / OCT+ tapped the NUDGE of the step edited; SELECT the track's LEVEL.
 * HOME (sm_home_page): MIXER's knobs are the LEVEL of T1..T4 (a base value, no locks), CLOCK's K1 the tempo (SHIFT: 1.0 a detent) and K2 the
 * current pattern's swing; SELECT the selected track's LEVEL on both (never the tempo). smp.bpm10 and the pattern's swing are written
 * here, in the main loop, 16 and 8 bits at once; the audio stream reads each once per block (sm_seq.c). */
#define SM_HOLD_US 350000u                                /* a key held longer is a select, not a tap */
#define SM_BLACK_KEYS 0x252A52Au                          /* the black keys of F3 .. G5, bit n = key n (ui_input.c) */
#define SM_NKEYS 27u
#define SM_OCT_PAIR_US 300000u                            /* OCT- and OCT+ pressed within this: both together (keyboard mode) */
#define SM_KEY_NOTE0 53u                                  /* F3, the first key's MIDI note (C4 = 60) */
#define SM_KEY_VEL 100u

#define SM_PDW ((SM_NPID + 31u) / 32u)
static uint32_t sm_pdirty[SM_NTRK][SM_PDW];               /* bit per parameter id (< SM_NPID: the KIT's lanes too): pushed again */
static uint8_t sm_ld_pend;                                /* bit per track: a LOAD still to push */
static uint8_t sm_tr_pend;                                /* 0 none, 1 PLAY, 2 STOP still to push */
static uint8_t sm_save_st;                                /* SAVE: 0 up, 1 down (since sm_save_t0), 2 held: UNDO done */
static uint32_t sm_save_t0;
static uint32_t sm_key_t0[SM_NKEYS], sm_key_arm;          /* press time of the white keys down, bit per key */
static uint8_t sm_key_step[SM_NKEYS];                     /* the step each of them was pressed on (a bank key may move on) */
static uint8_t sm_key_trk[SM_NKEYS];                      /* ... and the track */
static uint32_t sm_key_used;                              /* bit per key: a knob turned while it was down (a lock, no toggle) */
/* the chord entry (Tim): ARM (REC on), a trig held + D#5 (POLY on the panel): the keys a keyboard until a chord is played (up to
 * four keys down together; it sounds); when every key is up again its notes are the trig's (an empty step: a new trig), the
 * keyboard mode back as it was. sm_che 0 off, 1 waiting for every key up (the trig, D#5), 2 the chord being played */
#define SM_CHE_KEY 22u                                    /* D#5 */
static uint8_t sm_che, sm_che_step, sm_che_trk, sm_che_kbd, sm_che_n, sm_che_note[4];
static uint8_t sm_oct_pend;                               /* an OCT press waiting for the other one: 0 none, 1 OCT-, 2 OCT+ */
static uint32_t sm_oct_t0;                                /* ... when it went down */
static uint8_t sm_kb_note[SM_NKEYS], sm_kb_trk[SM_NKEYS]; /* keyboard mode: the note each key sounds (0xFF none) and its track */
static uint32_t sm_kb_pend;                               /* bit per key: pressed, its note-on not pushed yet */
static uint32_t sm_kb_arp;                                /* bit per key: it went to the track's arpeggiator (sm_arp.c), not the voices */
static uint8_t sm_arp_lseen[SM_NTRK], sm_arp_mseen[SM_NTRK];   /* each track's LATCH and MODE as the main loop last saw them */
/* sm_midi_ch (the MIDI page's channel per track) and sm_midi_src (the SETUP page's source) live in sm_midi.c, which the
 * audio stream reads; the MIDI and SETUP pages here write them, the screen reads them */
static uint8_t sm_shift;                                  /* SHIFT: OCT- held outside the keyboard mode and the layers (sm_oct_poll) */
#define SM_SHK_FILL 26u                                   /* SHIFT + G5 (REC off): FILL while held (sm_fill_poll) */
static int8_t sm_sh_acc[SM_NTRK], sm_trn_acc[SM_NTRK];    /* SHIFT + D#4 / F#4, D#5 / F#5: the net shift and transpose of a track since the pattern came (the header's value) */
#define SM_KL_N 8u
static uint8_t sm_kl_note[SM_NTRK][SM_KL_N];              /* LATCH (ARP held + F3, sm_klatch): the notes held on after their keys, 0xFF none */
static uint8_t sm_kl_rel[SM_NTRK];                        /* bit per place: let go (its KEYOFF not pushed yet) */
static uint8_t sm_eu_steps, sm_eu_hits, sm_eu_rot;        /* the EUCLID page's settings */
/* LIVE RECORD, the main loop's side (sm_lrec_poll): the recorded notes still held (their trig's LEN comes at the release) and,
 * per track, the step the last note went to and when (a note within a step of it on the same step: the chord) */
#define SM_NRO 8u
static struct { uint8_t on, trk, step, note, pat; uint32_t at; } sm_ro[SM_NRO];
static uint8_t sm_rc_ok[SM_NTRK], sm_rc_step[SM_NTRK], sm_rc_pat[SM_NTRK];
static uint32_t sm_rc_at[SM_NTRK];

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
    sm_kb_arp = 0;
    memset(sm_arp_lseen, 0, sizeof sm_arp_lseen);
    memset(sm_arp_mseen, 0, sizeof sm_arp_mseen);
    sm_unl_pend = 0;
    sm_rec = 0;
    memset(sm_ro, 0, sizeof sm_ro);
    memset(sm_rc_ok, 0, sizeof sm_rc_ok);
    sm_shift = 0;
    memset(sm_sh_acc, 0, sizeof sm_sh_acc);
    memset(sm_trn_acc, 0, sizeof sm_trn_acc);
    memset(sm_kl_note, 0xFF, sizeof sm_kl_note);
    memset(sm_kl_rel, 0, sizeof sm_kl_rel);
    sm_klatch = 0;
    sm_eu_steps = 16u;
    sm_eu_hits = 4u;
    sm_eu_rot = 0;
    sm_msg = 0;
    sm_dirty = 1;
}

static void sm_layer_begin(uint32_t pressed, uint32_t now, uint32_t used);
static void sm_fx_poll(uint32_t notes, const int32_t *k);   /* sm_layer.c (after this file) */
static void sm_layer_keys(uint32_t pressed, uint32_t notes, const int32_t *k, int32_t alg, int32_t sel);
static void sm_layer_end(void);
static uint32_t sm_hold_btn(void);                        /* sm_layer.c: HOME / ARP / FX held (no layer) */
static void sm_hold_keys(uint32_t notes);
static uint32_t sm_layer_busy(void);                      /* a layer open, or its button down */
static void sm_undo(void);                                /* sm_edit.c (after this file) */
static int sm_trig_copy(uint32_t t, uint32_t st);
static void sm_paste_held(void);
static uint8_t sm_tclip_ok;                               /* (sm_edit.c: a trig copied) */
static void sm_edit_pat_changed(void);
static void sm_eu_gen(void);
static void sm_acc_reset(uint32_t mask);
static void sm_undo_snap(void);
static void sm_put_begin(uint32_t mask);
static void sm_put_track(uint32_t t, const sm_pattern_t *src, uint32_t st, int32_t rot, int snd);
static void sm_shift_trk(uint32_t t, int32_t dir);
static void sm_trn(uint32_t t, int32_t d);

static sm_sound_t *sm_ui_sound(void) { return &smp.pat[smp.cur % SM_NPAT].kit.snd[sm_ui.trk % SM_NTRK]; }

static void sm_lane_machine(uint32_t L);
static void sm_param_push(uint32_t t, uint32_t id)
{
    sm_cmd_t c;
    c.kind = SMC_PARAM;
    c.trk = (uint8_t)t;
    c.step = smp.cur;                                     /* (the pattern: sm_cmd.c) */
    c.param = (uint8_t)id;
    c.val = (int16_t)sm_pget(&smp.pat[smp.cur % SM_NPAT], t, id);
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
    sm_pattern_t *pt = &smp.pat[smp.cur % SM_NPAT];
    sm_sound_t *sd = &pt->kit.snd[t % SM_NTRK];
    const param_desc_t *d = sm_desc_trk(sd, id);
    if (!s || !d)
        return;
    sm_pset(pt, t, id, sm_val_turn(d, id, sm_pget(pt, t, id), s));   /* (a byte write: the audio stream sees the old or the new) */
    sm_param_push(t % SM_NTRK, id);
    if (sm_snd_kit(sd) && t % SM_NTRK == SM_KIT_TRK && id >= P_E0 && id < P_E0 + 4u)
        sm_lane_machine(id - P_E0);                       /* (a lane's machine turned: its values that machine's first sound) */
    sm_dirty = 1;
}

/* a global effect parameter (song.g[g]: the delay, the reverb, the chorus) turned by s detents in its range. The core reads
 * song.g once a block (fx.c); the main loop is the only writer and a store is one aligned int16, so a block sees the old or
 * the new value. The project object (sm_bank.c) carries it; sm_proj_hash sees it, so the autosave takes it */
static void sm_turn_glob(uint32_t g, int32_t s)
{
    const param_desc_t *d;
    if (!s || g >= G_COUNT)
        return;
    d = &GP[g];
    song.g[g] = (int16_t)clamp(param_turn(d, clamp(song.g[g], d->min, d->max), s), d->min, d->max);
    sm_dirty = 1;
}

/* a knob: the model's value turned by s detents, then the command.
 * A white key down: a lock of the held step instead. No key down the knob is always the track's base (the step selected
 * on STEP is STEP's alone); REC while playing it also records: the playing step's trig (a trig there only) locks the
 * value the base now has, and the value goes onto the core at once (a parameter a lock holds there would not take the
 * base's SMC_PARAM until the next unlocked trig). Hardware 02:43: the knobs "do not turn at all without a step" was REC
 * on while playing, where a turn used to move a lock only, never the base: no trig under the playhead, nothing at all */
static void sm_turn(uint32_t id, int32_t s)
{
    const param_desc_t *d;
    uint32_t t = sm_ui.trk % SM_NTRK;
    if (id & SM_GLB) {                                    /* a global (FX's SELECT, the deep pages): song.g, the project's; */
        sm_turn_glob(id & ~SM_GLB, s);                    /* no lock, no REC (the base of the whole project) */
        return;
    }
    d = sm_desc_trk(sm_ui_sound(), id);
    if (!s || !d)
        return;
    if (sm_key_arm) {
        sm_turn_held(id, s);
        return;
    }
    sm_turn_base(t, id, s);
    if (sm_rec && sm_live && sm_playing) {               /* REC (live), playing: the knob records onto the trig playing (ARM: never; a held trig locks) */
        uint32_t st = *(volatile uint8_t *)&sm_pos[t];
        sm_cmd_t c = {SMC_PREVIEW, (uint8_t)t, smp.cur, (uint8_t)id, 0};
        c.val = (int16_t)sm_pget(&smp.pat[smp.cur % SM_NPAT], t, id);
        if (smp.pat[smp.cur % SM_NPAT].step[t][st % SM_NSTEP].n)
            (void)sm_lock_set(t, st % SM_NSTEP, id, c.val);   /* (the pool full: LOCKS FULL, the base moved all the same) */
        (void)sm_cmd_push(c);                             /* (refused: the base's SMC_PARAM, or the next trig, brings it) */
    }
}

/* FILT OFF (the sound's default) makes CUT, RES, the filter envelope's ENV and its attack and decay, and FILTER's SELECT
 * (LFO>FLT) do nothing: turning one of them on a track whose FILT is OFF first sets FILT to LP (the model, then the
 * command), so the knob is heard at once. Another FILT (LP BP HP, HYB: K2 / K3 are BASE / RES then) is left as it is */
static void sm_filt_auto(uint32_t id)
{
    sm_sound_t *sd = sm_ui_sound();
    if (sd->p[P_FLT] || (id != P_FCUT && id != P_FRES && id != P_ED_FLT && id != P_FATK && id != P_FDEC && id != P_LD_FLT))
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
        if (sm_ui.bank >= sm_npages())
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
 * length and its trig defaults are the sequencer's, not the sound's: they stay. Used by the PRESETS knob and the EDIT layer */
static void sm_sound_load(uint32_t machine, uint32_t preset)
{
    sm_sound_t *sd = sm_ui_sound(), n;
    sm_sound_from_preset(&n, machine, preset);
    n.len = sd->len;
    n.dnote = sd->dnote;                                  /* (the trig defaults are the sequencer's too) */
    n.dvel = sd->dvel;
    n.dlen = sd->dlen;
    n.dcond = sd->dcond;
    n.speed = sd->speed;                                  /* (and its speed) */
    fm1_irq_off();
    *sd = n;
    fm1_irq_on();
    if (sm_trk_kit(sm_ui.trk % SM_NTRK, &smp.pat[smp.cur % SM_NPAT]))
        sm_lanes_default(&smp.pat[smp.cur % SM_NPAT]);    /* (the KIT: its lanes from its sound; the LOAD brings them) */
    sm_ld_pend |= (uint8_t)(1u << sm_ui.trk);
    sm_dirty = 1;
}

/* the KIT on T4: lane L's nine values its machine's factory sound pi (S1 .. S4 = pi), each pushed */
static void sm_lane_sound(uint32_t L, uint32_t pi)
{
    sm_pattern_t *pt = &smp.pat[smp.cur % SM_NPAT];
    sm_sound_t *sd = &pt->kit.snd[SM_KIT_TRK];
    uint32_t k;
    L &= 3u;
    sd->p[P_E4 + L] = (int8_t)pi;
    sm_param_push(SM_KIT_TRK, P_E4 + L);
    dr_lane_init(pt->lane[L], dr_kmach(sd->p[P_E0 + L]), pi);
    for (k = 0; k < 9u; k++)
        sm_param_push(SM_KIT_TRK, SM_LP(L, k));
    sm_dirty = 1;
}
/* lane L's machine turned (SELECT on EDIT 1, the DRUM page's keys): its first factory sound */
static void sm_lane_machine(uint32_t L) { sm_lane_sound(L, 0); }

/* PRESETS: the next / previous preset of the track's machine (wraps), the whole sound from it, then the LOAD */
static void sm_preset_turn(int32_t s)
{
    sm_sound_t *sd = sm_ui_sound();
    uint32_t np = ENGINES[sm_engine_of(sd->machine)]->npresets;
    int32_t p;
    if (s && sm_trk_kit(sm_ui.trk % SM_NTRK, &smp.pat[smp.cur % SM_NPAT])) {   /* the KIT: the lane in edit's factory sounds */
        uint32_t L = sm_kit_lane % 4u, n = DR_KENG[dr_kmach(sd->p[P_E0 + L])]->npresets;
        p = ((int32_t)clamp(sd->p[P_E4 + L], 0, 15) + s) % (int32_t)n;
        sm_lane_sound(L, (uint32_t)(p < 0 ? p + (int32_t)n : p));
        return;
    }
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
        if (sm_shift && k == SM_SHK_FILL && !sm_rec)      /* (SHIFT + G5, REC off: FILL, sm_fill_poll; REC on: the copy as ever) */
            continue;
        if (sm_shift) {                                   /* SHIFT + a trig key (Tim): a lock trig there (no note, the locks only), or
                                                             back: an empty step a new one, a trig made one, a lock trig made a trig */
            sm_step_rest(sm_ui.trk, (uint32_t)sm_ui.bank * 16u + (uint32_t)w);   /* (the page in view) */
            sm_dirty = 1;
            continue;
        }
        if (!sm_rec)                                      /* (REC off: the keys leave the steps alone) */
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

/* OCT- / OCT+ outside the layers. Alone, in the keyboard mode: the octave -1 / +1 (-3 .. +3). Outside it OCT- is SHIFT (held:
 * sm_shift, nothing on its own) and OCT+ turns the page of steps round, 1..16 / 17..32 (only on a track longer than 16; on the EUCLID page it
 * generates); a trig held + OCT+: the trig copied pastes onto it (sm_oct_poll). Both within SM_OCT_PAIR_US: the keyboard mode on or off (the only way: ARP is pages only).
 * So a single press waits that long for the other before it acts (a page must not flip under a chord press). */
static int32_t sm_step_edit(void);
static uint32_t sm_key_held_trk(void);
static void sm_oct_single(uint32_t up)
{
    if (sm_ui.kbd) {
        int32_t o = clamp((int32_t)sm_ui.oct + (up ? 1 : -1), -3, 3);
        if ((int8_t)o != sm_ui.oct) {
            sm_ui.oct = (int8_t)o;                        /* (notes sounding keep their pitch: the key's own note-off) */
            sm_dirty = 1;
        }
    } else if (sm_page_is_step() && sm_key_held_trk()) {   /* STEP, a trig key held: OCT- / OCT+ the NUDGE -1 / +1 */
        sm_step_t *s = &smp.pat[smp.cur % SM_NPAT].step[sm_ui.trk % SM_NTRK][sm_step_edit()];
        if (!up && ((fm1_in.buttons >> panel.btn[B_OCTDN]) & 1u))
            return;                                       /* (OCT- still down past the pair's window: SHIFT, not a tap) */
        if (s->n) {
            int32_t v = clamp((int32_t)s->nudge + (up ? 1 : -1), -8, 7);
            if ((int8_t)v != s->nudge) {
                s->nudge = (int8_t)v;                     /* (a byte the audio stream reads at the step's start) */
                sm_dirty = 1;
            }
            sm_msg_show_val(SM_MSG_NUDGE, s->nudge);      /* (shown only now, a second in the header: NUDGE+3) */
        }
    } else if (up && sm_page_home() == SMH_EUCLID) {
        sm_eu_gen();                                      /* (the EUCLID page: OCT+ generates) */
    } else if (smp.pat[smp.cur % SM_NPAT].kit.snd[sm_ui.trk % SM_NTRK].len > 16u) {
        uint32_t np = sm_npages();                        /* OCT- the previous page of steps, OCT+ the next (round: 1..16, 17..32, ..) */
        if (!up && ((fm1_in.buttons >> panel.btn[B_OCTDN]) & 1u))
            return;                                       /* (OCT- still down past the pair's window: SHIFT, not a tap) */
        sm_ui.bank = (uint8_t)((sm_ui.bank + (up ? 1u : np - 1u)) % np);
        sm_dirty = 1;
    }
}

static void sm_arp_clear_all(void)                        /* the keyboard sets of every arp emptied (latched notes too) */
{
    uint32_t t;
    for (t = 0; t < SM_NTRK; t++)
        sm_aset_clear(&sm_ak[t]);
}

static void sm_oct_toggle(void)
{
    sm_ui.kbd = (uint8_t)!sm_ui.kbd;
    if (!sm_ui.kbd) {
        uint32_t t, i;
        sm_arp_clear_all();                               /* (leaving the keyboard mode lets every arp go, latched or not) */
        sm_klatch = 0;                                    /* (... and every latched note: LATCH is off with the mode) */
        for (t = 0; t < SM_NTRK; t++)
            for (i = 0; i < SM_KL_N; i++)
                if (sm_kl_note[t][i] != 0xFFu)
                    sm_kl_rel[t] |= (uint8_t)(1u << i);
    }
    sm_key_arm = sm_key_used = 0;                         /* (a trig key down at the switch never toggles a trig) */
    sm_ui.held_step = sm_ui.sel_step = 0xFFu;
    sm_dirty = 1;
}

/* act: this poll another button, a key or a knob moved. OCT- held while one did is SHIFT (a combo, an accelerated turn): it is no
 * longer the first half of a pair, so an OCT+ after it is a single OCT+ (SHIFT + SEQ, then OCT+ at once on EUCLID, generates) */
static void sm_oct_poll(uint32_t pressed, uint32_t now, uint32_t act)
{
    uint32_t dn = (pressed >> panel.btn[B_OCTDN]) & 1u, up = (pressed >> panel.btn[B_OCTUP]) & 1u;
    if (act && sm_oct_pend == 1u && ((fm1_in.buttons >> panel.btn[B_OCTDN]) & 1u))
        sm_oct_pend = 0;                                  /* (OCT- used as SHIFT: no pair, no single press of its own) */
    sm_shift = (uint8_t)(!sm_ui.kbd && !sm_layer_busy() && ((fm1_in.buttons >> panel.btn[B_OCTDN]) & 1u));
    if (sm_layer_busy()) {                         /* (the layers use OCT: EDIT's next / previous page; a layer button down may open one) */
        sm_oct_pend = 0;
        return;
    }
    if (up && !sm_ui.kbd && sm_key_arm && !sm_page_is_step()) {   /* a trig held: OCT+ pastes (no page, no pair with OCT-: SHIFT;
                                                                     * STEP: OCT+ is the NUDGE, sm_oct_single) */
        sm_paste_held();
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

/* LATCH of the keyboard's notes (sm_klatch, ARP held + F3; not the arp's: a track whose arp is on gives its keys to the arp, which
 * holds them by the same latch, sm_arp_latch). A key let go on a latched track keeps its note sounding (a place of sm_kl_note: the
 * key is free again, a note-on of the same key plays anew); a new chord (the first key down, none of the track's other keys down)
 * lets the old ones go, a note the same as the new key's stays with the key; LATCH off lets them all go. The KEYOFFs are pushed from
 * the state every poll (sm_kl_flush), so a refused one (queue full) goes again on the next */
static uint32_t sm_kl_add(uint32_t t, uint32_t note)
{
    uint32_t i, free = SM_KL_N;
    for (i = 0; i < SM_KL_N; i++) {
        if (sm_kl_note[t][i] == note && !((sm_kl_rel[t] >> i) & 1u))
            return 1;                                     /* (held on already) */
        if (sm_kl_note[t][i] == 0xFFu && free == SM_KL_N)
            free = i;
    }
    if (free == SM_KL_N)
        return 0;                                         /* (full: this one goes off with its key) */
    sm_kl_note[t][free] = (uint8_t)note;
    sm_kl_rel[t] &= (uint8_t)~(1u << free);
    return 1;
}
static void sm_kl_replace(uint32_t t, uint32_t note)
{
    uint32_t i;
    for (i = 0; i < SM_KL_N; i++) {
        if (sm_kl_note[t][i] == 0xFFu)
            continue;
        if (sm_kl_note[t][i] == note)
            sm_kl_note[t][i] = 0xFFu;                     /* (the new key sounds it) */
        else
            sm_kl_rel[t] |= (uint8_t)(1u << i);
    }
}
static void sm_kl_flush(void)
{
    uint32_t t, i;
    for (t = 0; t < SM_NTRK; t++)
        for (i = 0; i < SM_KL_N && sm_kl_rel[t]; i++) {
            sm_cmd_t c = {SMC_KEYOFF, (uint8_t)t, sm_kl_note[t][i], 0, 0};
            if (!((sm_kl_rel[t] >> i) & 1u))
                continue;
            if (sm_kl_note[t][i] == 0xFFu || sm_cmd_push(c)) {
                sm_kl_note[t][i] = 0xFFu;
                sm_kl_rel[t] &= (uint8_t)~(1u << i);
            }
        }
}
/* ARP held + F3: the selected track's LATCH on / off (off: its latched notes go) */
static void sm_latch_toggle(void)
{
    uint32_t t = sm_ui.trk % SM_NTRK, i;
    sm_klatch ^= (uint8_t)(1u << t);
    if (!((sm_klatch >> t) & 1u))
        for (i = 0; i < SM_KL_N; i++)
            if (sm_kl_note[t][i] != 0xFFu)
                sm_kl_rel[t] |= (uint8_t)(1u << i);
    sm_dirty = 1;
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
                uint32_t pitch = sm_key_pitch(smp.pat[smp.cur % SM_NPAT].kit.snd[c.trk].p, k, sm_ui.oct);   /* (SCL: QNT, TRN) */
                if (sm_trk_kit(c.trk, &smp.pat[smp.cur % SM_NPAT])) {   /* KIT: F3 G3 A3 B3 its four lanes, the rest nothing */
                    int32_t kn = dkit_keys(&trk[c.trk], k);
                    pitch = kn < 0 ? SM_KEY_SILENT : (uint32_t)kn;
                }
                if (pitch == SM_KEY_SILENT) {             /* (a black key under QNT WHITE: nothing) */
                    sm_kb_pend &= ~bit;
                    continue;
                }
                c.step = (uint8_t)pitch;
                c.val = (int16_t)(sm_live && sm_playing); /* (live record; the audio stream checks PLAY again) */
                if (smp.pat[smp.cur % SM_NPAT].kit.snd[c.trk].p[P_AMODE] > 0) {   /* an arpeggiator on the track: the key is its input */
                    uint32_t j, others = 0;
                    for (j = 0; j < SM_NKEYS; j++)
                        others |= ((sm_kb_arp >> j) & 1u) && sm_kb_trk[j] == c.trk;
                    sm_arp_key_on(c.trk, c.step, SM_KEY_VEL, others);
                    sm_kb_arp |= bit;
                    sm_kb_note[k] = c.step;
                    sm_kb_trk[k] = c.trk;
                    sm_kb_pend &= ~bit;
                } else if (sm_cmd_push(c)) {
                    uint32_t j, others = 0;
                    sm_kb_note[k] = c.step;
                    sm_kb_trk[k] = c.trk;
                    sm_kb_pend &= ~bit;
                    for (j = 0; j < SM_NKEYS; j++)
                        others |= j != k && sm_kb_note[j] != 0xFFu && !((sm_kb_arp >> j) & 1u) && sm_kb_trk[j] == c.trk;
                    if (((sm_klatch >> c.trk) & 1u) && !others)
                        sm_kl_replace(c.trk, c.step);     /* (a new chord: the latched notes go) */
                }
            }
        } else if (!sm_ui.kbd || !(held & bit)) {
            sm_cmd_t c = {SMC_KEYOFF, sm_kb_trk[k], sm_kb_note[k], 0, 0};
            uint32_t j, same = 0;
            for (j = 0; j < SM_NKEYS; j++)               /* (another key sounds the same note on the track: its off is the last) */
                same |= j != k && sm_kb_note[j] == c.step && sm_kb_trk[j] == c.trk && ((sm_kb_arp >> j) & 1u) == ((sm_kb_arp >> k) & 1u);
            if ((sm_kb_arp >> k) & 1u) {                  /* (an arp key: out of the set unless LATCH holds it, or another key has the note) */
                if (!same)
                    sm_arp_key_off(c.trk, c.step, !sm_ui.kbd);
                sm_kb_arp &= ~bit;
                sm_kb_note[k] = 0xFFu;
            } else if (!same && sm_ui.kbd && ((sm_klatch >> sm_kb_trk[k]) & 1u) && sm_kl_add(sm_kb_trk[k], sm_kb_note[k])) {
                sm_kb_note[k] = 0xFFu;                    /* (LATCH: the note stays, the key is free) */
            } else if (same || sm_cmd_push(c)) {
                sm_kb_note[k] = 0xFFu;
            }
        }
    }
    sm_kl_flush();
}

/* the arpeggiators, once a poll: a track whose MODE went OFF lets its keyboard set go; LATCH turned off drops every note whose
 * key is not down (the set is rebuilt from the keys down: the audio stream sees the old set or the new, sm_arp_key_set) */
static void sm_arp_poll(void)
{
    uint32_t t, k;
    for (t = 0; t < SM_NTRK; t++) {
        const sm_sound_t *sd = &smp.pat[smp.cur % SM_NPAT].kit.snd[t];
        uint8_t m = sd->p[P_AMODE] > 0, l = (uint8_t)(sm_arp_latch(t) != 0u);
        if (m != sm_arp_mseen[t] && !m)
            sm_aset_clear(&sm_ak[t]);
        if (l != sm_arp_lseen[t] && !l && m) {
            uint8_t nt[SM_AK_N];
            uint32_t c = 0, i;
            for (k = 0; k < SM_NKEYS; k++) {
                if (!((sm_kb_arp >> k) & 1u) || sm_kb_trk[k] != t)
                    continue;
                for (i = 0; i < c && nt[i] != sm_kb_note[k]; i++)
                    ;
                if (i == c && c < SM_AK_N)
                    nt[c++] = sm_kb_note[k];
            }
            sm_arp_key_set(t, nt, c);
        }
        sm_arp_mseen[t] = m;
        sm_arp_lseen[t] = l;
    }
}

/* the step the STEP page edits on the selected track: the white key down on it pressed last, else the step selected last;
 * -1 none */
/* a trig key of the selected track is down (armed): the STEP page's OCT- / OCT+ are its NUDGE then, not the pages */
static uint32_t sm_key_held_trk(void)
{
    uint32_t k;
    for (k = 0; k < SM_NKEYS; k++)
        if (((sm_key_arm >> k) & 1u) && sm_key_trk[k] == sm_ui.trk)
            return 1;
    return 0;
}
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

/* the chord entry (sm_che, above): returns the keys pressed this poll the rest of the poll may see (D#5 taken by it) */
static uint32_t sm_che_poll(uint32_t notes)
{
    uint32_t k, t;
    sm_pattern_t *p = &smp.pat[smp.cur % SM_NPAT];
    if (sm_che && (!sm_rec || sm_layer || sm_che_trk != sm_ui.trk)) {   /* (REC off, a layer, another track: given up) */
        sm_ui.kbd = sm_che_kbd;
        sm_che = 0;
    }
    if (!sm_che) {
        int32_t hs;
        if (!sm_rec || sm_layer || !((notes >> SM_CHE_KEY) & 1u) || (hs = sm_trig_held()) < 0 ||
            sm_trk_kit(sm_ui.trk % SM_NTRK, p))
            return notes;
        sm_che = 1;                                       /* a trig held + D#5: wait for every key up */
        sm_che_step = (uint8_t)hs;
        sm_che_trk = sm_ui.trk;
        sm_che_kbd = sm_ui.kbd;
        sm_che_n = 0;
        sm_key_used |= sm_key_arm;                        /* (the trig's release toggles nothing) */
        sm_msg_show(SM_MSG_CHORDIN);
        return notes & ~(1u << SM_CHE_KEY);
    }
    t = sm_che_trk % SM_NTRK;
    if (sm_che == 1) {
        if (!fm1_in.notes) {
            sm_che = 2;                                   /* every key up: the keys a keyboard now */
            sm_ui.kbd = 1;
            sm_key_arm = 0;
        }
        return 0;
    }
    for (k = 0; k < SM_NKEYS; k++)                        /* the chord: each key down, its note (up to four) */
        if (((notes >> k) & 1u) && sm_che_n < 4u) {
            uint32_t pitch = sm_key_pitch(p->kit.snd[t].p, k, sm_ui.oct);
            if (pitch != SM_KEY_SILENT)
                sm_che_note[sm_che_n++] = (uint8_t)pitch;
        }
    if (sm_che_n && !fm1_in.notes) {                      /* played and let go: the trig's notes */
        sm_step_t *s = &p->step[t][sm_che_step % SM_NSTEP];
        uint32_t i;
        if (!s->n)
            sm_step_fresh(t, sm_che_step % SM_NSTEP, 0);
        for (i = 0; i < sm_che_n; i++)
            s->note[i] = sm_che_note[i];
        s->flags &= (uint8_t)~SMF_REST;
        SM_BARRIER();
        s->n = sm_che_n;
        sm_ui.kbd = sm_che_kbd;
        sm_che = 0;
        sm_msg_show(SM_MSG_CHORDSET);
        sm_dirty = 1;
    }
    return notes;
}

/* a STEP field f (0 NOTE 0..127, 1 VEL 1..127, 2 LEN the list's index, 3 COND its walk) from v turned by d */
static uint32_t sm_field_turn(uint32_t f, uint32_t v, int32_t d)
{
    if (f == 0u)
        return (uint32_t)clamp((int32_t)v + d, 0, 127);
    if (f == 1u)
        return (uint32_t)clamp((int32_t)v + d, 1, 127);
    if (f == 2u)
        return (uint32_t)clamp((int32_t)v + d, 0, (int32_t)SM_NLEN - 1);
    return sm_cond_of_place((uint32_t)clamp((int32_t)sm_cond_place(v) + d, 0, (int32_t)SM_COND_PLACES - 1));
}

/* the STEP page's knobs: K1 NOTE, K2 VEL, K3 LEN, K4 COND (sm_field_turn), SELECT the track's LEVEL, PRESETS the FLAG list round
 * (sm_step_flag), OCT- / OCT+ tapped the NUDGE (sm_oct_single). A step edited (sm_step_edit): a field turned starts from what the
 * trig plays (its own, else the track's default) and is the step's own from then on (no longer SM_INH); the fields not turned keep
 * inheriting. An empty step: PRESETS a REST trig, the rest nothing. No step selected: K1..K4 are the track's trig defaults
 * (sm_sound_t dnote .. dcond: the new trigs' fields, and every trig's that inherits them). Each a byte the audio stream reads at a
 * fire. (The swing is the pattern's: CLOCK K2) */
static void sm_step_page(const int32_t *k, int32_t sel, int32_t pre)
{
    int32_t st = sm_step_edit();
    uint32_t t = sm_ui.trk % SM_NTRK, i;
    sm_sound_t *sd = sm_ui_sound();
    sm_step_t *s, e;
    uint8_t *f[4];
    if (!pre && !sel && !k[0] && !k[1] && !k[2] && !k[3])
        return;
    sm_dirty = 1;
    if (st < 0) {
        f[0] = &sd->dnote; f[1] = &sd->dvel; f[2] = &sd->dlen; f[3] = &sd->dcond;
        for (i = 0; i < 4u; i++)
            if (k[i])
                *f[i] = (uint8_t)sm_field_turn(i, *f[i], k[i]);
        if (sel)                                          /* the selected track's volume */
            sm_turn_base(t, P_LEVEL, sel);
        return;
    }
    s = &smp.pat[smp.cur % SM_NPAT].step[t][st];
    if (!sm_step_flag(t, st, pre))
        return;
    sm_step_eff(s, sd, &e);
    f[0] = &s->note[0]; f[1] = &s->vel; f[2] = &s->len; f[3] = &s->cond;
    for (i = 0; i < 4u; i++)
        if (k[i]) {
            uint32_t old = i == 0u ? e.note[0] : 0u;
            *f[i] = (uint8_t)sm_field_turn(i, i == 0u ? e.note[0] : i == 1u ? e.vel : i == 2u ? e.len : e.cond, k[i]);
            if (i == 0u && s->n > 1u && !sm_trk_kit(t, &smp.pat[smp.cur % SM_NPAT])) {   /* a chord: NOTE moves every note (Tim) */
                int32_t dn = (int32_t)s->note[0] - (int32_t)old;
                uint32_t j;
                for (j = 1; j < s->n && j < 4u; j++)
                    s->note[j] = (uint8_t)clamp((int32_t)s->note[j] + dn, 0, 127);
            }
        }
    if (sel)                                              /* the selected track's volume */
        sm_turn_base(t, P_LEVEL, sel);
}

/* SHIFT + a knob: the turn accelerated. On a wide range (64 values or more, no list of names) a detent is 4 steps, on a very
 * wide one (256 or more: a time, a frequency) 8; a list, a narrow range turns as ever. The knobs of STEP (NOTE, VEL) and MIXER's
 * levels are x4; CLOCK's tempo 1.0 BPM a detent (K1 and SELECT). Without SHIFT: the detents as they are */
static int32_t sm_range_mult(const param_desc_t *d)
{
    if (!d || d->fmt == F_ENUM || d->names || d->max - d->min < 64)
        return 1;
    return d->max - d->min >= 256 ? 8 : 4;
}
/* the multiplier of knob i (0..3, 4 SELECT) of the page shown */
static int32_t sm_knob_mult(uint32_t i)
{
    uint32_t id;
    const param_desc_t *d;
    if (sm_page_is_step())
        return i < 2u ? 4 : 1;
    if (sm_page_home())
        return sm_page_home() == SMH_MIXER && i < 4u ? 4 : 1;
    id = sm_page_param(i);
    d = id == SM_NOPARAM ? 0 : (id & SM_GLB) ? &GP[(id & ~SM_GLB) % G_COUNT] : sm_desc_trk(sm_ui_sound(), id);
    return sm_range_mult(d);
}
static int32_t sm_accel(uint32_t i, int32_t s) { return sm_shift ? s * sm_knob_mult(i) : s; }

/* the EUCLID page (SHIFT + SEQ): K1 STEPS 1..32, K2 HITS 0..STEPS, K3 ROT 0..STEPS-1; OCT+ generates. HITS spread over STEPS as
 * evenly as the grid allows, a hit on step 1 (rot 0), the cycle turned right by ROT, repeated over the selected track's length */
static void sm_eu_page(const int32_t *k)
{
    int32_t st = clamp((int32_t)sm_eu_steps + k[0], 1, (int32_t)SM_NSTEP);
    int32_t h = clamp((int32_t)sm_eu_hits + k[1], 0, st);
    int32_t r = clamp((int32_t)sm_eu_rot + k[2], 0, st - 1);
    if (st != sm_eu_steps || h != sm_eu_hits || r != sm_eu_rot) {
        sm_eu_steps = (uint8_t)st;
        sm_eu_hits = (uint8_t)h;
        sm_eu_rot = (uint8_t)r;
        sm_dirty = 1;
    }
}
static uint32_t sm_eu_on(uint32_t i, uint32_t steps, uint32_t hits, uint32_t rot)   /* is place i of the cycle a hit */
{
    uint32_t j = (i + steps - rot % steps) % steps;
    return (j * hits) % steps < hits;
}
static void sm_eu_gen(void)
{
    uint32_t t = sm_ui.trk % SM_NTRK, s, len = (uint32_t)clamp(sm_ui_sound()->len, 1, (int32_t)SM_NSTEP);
    uint32_t steps = (uint32_t)clamp(sm_eu_steps, 1, (int32_t)SM_NSTEP), hits = sm_eu_hits > steps ? steps : sm_eu_hits;
    sm_undo_snap();
    sm_put_begin(1u << t);
    sm_put_track(t, 0, 0, 0, 0);                          /* (the track's trigs and locks cleared, then the hits placed) */
    for (s = 0; s < len; s++)
        if (sm_eu_on(s % steps, steps, hits, sm_eu_rot))
            sm_step_toggle(t, s);
    sm_acc_reset(1u << t);
    sm_dirty = 1;
}

/* REC: a press turns recording on / off. Off: the trig keys down are let go of (their release toggles nothing) and no step
 * stays held or selected (the knobs are the base, STEP's the track's defaults). MIDI's record track published every poll,
 * the live record's track (REC + PLAY); armed without live (REC + PLAY's REC press: ARM) it is 0xFF.
 * REC + PLAY (REC held, PLAY tapped) starts recording at once (sm_rec = 1, sm_live = 1, PLAY). A REC press while live
 * exits to ARM: live record off, REC stays armed, the transport keeps running. A REC press while armed disarms;
 * while off, arms. */
static void sm_rec_poll(uint32_t pressed)
{
    if (((pressed >> panel.btn[B_REC]) & 1u) && sm_key_arm) {   /* a trig held + REC (Tim): that trig copied; REC stays as it is */
        uint32_t k;
        for (k = 0; k < SM_NKEYS; k++)
            if ((sm_key_arm >> k) & 1u) {
                sm_msg_show(sm_trig_copy(sm_key_trk[k], sm_key_step[k]) ? SM_MSG_GCOPY : SM_MSG_GEMPTY);   /* (an empty step: nothing) */
                break;
            }
        sm_key_used |= sm_key_arm;                        /* (no toggle at the release) */
        sm_dirty = 1;
        return;
    }
    if ((pressed >> panel.btn[B_REC]) & 1u) {
        if (sm_rec && sm_live) {                            /* RECORD MODE: exit to ARM, the transport keeps running */
            sm_live = 0;
        } else {
            sm_rec = (uint8_t)!sm_rec;
            sm_live = 0;
            if (!sm_rec) {
                sm_key_arm = sm_key_used = 0;
                sm_ui.held_step = sm_ui.sel_step = 0xFFu;
            }
        }
        sm_dirty = 1;
    }
    sm_rec_mtrk = (uint8_t)(sm_live ? sm_ui.trk % SM_NTRK : 0xFFu);
}

/* the LEN list's place nearest to a hold of dt samples at step_q8 (q8 samples a step): 1/64 .. 16 (never TRK, never INF) */
static uint32_t sm_len_nearest(uint32_t dt, uint32_t step_q8)
{
    uint32_t u = step_q8 >> 6, q, i, best = 1u, bd = 0xFFFFFFFFu;
    if (dt > 0xFFFFFFu)
        dt = 0xFFFFFFu;
    q = (dt << 8) / (u ? u : 1u);                         /* (the hold in 1/64 steps) */
    for (i = 1; i < SM_LEN_INF; i++) {
        uint32_t d = q > SM_LEN64[i] ? q - SM_LEN64[i] : SM_LEN64[i] - q;
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

/* LIVE RECORD: the audio stream's events (sm_seq.c sm_rev) into the model. ON: a new trig on its step (the trig there before
 * replaced, sm_step_rec), or, the track's last note having gone to the same step less than a step ago, a note more on it (the
 * chord); OFF: the LEN of its hold on that trig. An event of a pattern no longer the current one: dropped */
static void sm_lrec_poll(void)
{
    uint8_t r = sm_rev_r;
    while (r != *(volatile uint8_t *)&sm_rev_w) {
        sm_rev_t e;
        uint32_t i, t, sq;
        SM_BARRIER();
        e = sm_rev[r % SM_NREV];
        SM_BARRIER();
        r++;
        sm_rev_r = r;
        t = e.trk % SM_NTRK;
        sq = sm_trk_step_q8(&smp.pat[smp.cur % SM_NPAT], t);   /* (the track's own step: its speed) */
        if (e.kind == SM_REV_ON) {
            uint32_t free = SM_NRO, old = 0;
            if (e.pat != smp.cur % SM_NPAT || e.step >= SM_NSTEP)
                continue;
            if (!(sm_rc_ok[t] && sm_rc_step[t] == e.step && sm_rc_pat[t] == e.pat &&
                  (uint32_t)(e.at - sm_rc_at[t]) < (sq >> 8) && sm_step_rec_add(t, e.step, e.note))) {
                sm_step_rec(t, e.step, e.note, e.vel);
                sm_rc_ok[t] = 1;
                sm_rc_step[t] = e.step;
                sm_rc_pat[t] = e.pat;
                sm_rc_at[t] = e.at;
            }
            for (i = 0; i < SM_NRO; i++) {                /* (held: a free place, else the oldest's) */
                if (!sm_ro[i].on) {
                    free = i;
                    break;
                }
                if ((int32_t)(sm_ro[i].at - sm_ro[old].at) < 0)
                    old = i;
            }
            i = free < SM_NRO ? free : old;
            sm_ro[i].on = 1;
            sm_ro[i].trk = (uint8_t)t;
            sm_ro[i].step = e.step;
            sm_ro[i].note = e.note;
            sm_ro[i].pat = e.pat;
            sm_ro[i].at = e.at;
            sm_dirty = 1;
            continue;
        }
        for (i = 0; i < SM_NRO; i++)
            if (sm_ro[i].on && sm_ro[i].trk == t && sm_ro[i].note == e.note) {
                sm_step_t *s = &smp.pat[smp.cur % SM_NPAT].step[t][sm_ro[i].step % SM_NSTEP];
                uint32_t k, has = 0;
                sm_ro[i].on = 0;
                for (k = 0; k < s->n && k < 4u; k++)
                    has |= s->note[k] == e.note;
                if (sm_ro[i].pat == smp.cur % SM_NPAT && has && !(s->flags & SMF_REST)) {
                    s->len = (uint8_t)sm_len_nearest(e.at - sm_ro[i].at, sq);   /* (one byte the audio stream reads at a fire) */
                    sm_dirty = 1;
                }
                break;
            }
    }
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
        if (sel)                                            /* SELECT: selected track's volume */
            sm_turn_base(sm_ui.trk % SM_NTRK, P_LEVEL, sel);
    } else {
        sm_bpm_turn(sm_shift ? k[0] * 10 : sm_bpm_accel(k[0], now));   /* (SHIFT: 1.0 a detent) */
        if (k[1]) {                                       /* K2: the current pattern's swing, 50 .. 75 % (a byte read once a block) */
            sm_pattern_t *pt = &smp.pat[smp.cur % SM_NPAT];
            int32_t v = clamp((int32_t)(pt->swing < 25u ? pt->swing : 25u) + k[1], 0, 25);
            if ((uint8_t)v != pt->swing) {
                pt->swing = (uint8_t)v;
                sm_dirty = 1;
            }
        }
        if (sel) sm_turn_base(sm_ui.trk % SM_NTRK, P_LEVEL, sel);  /* SELECT: selected track's volume */
    }
}

/* the KIT on T4, selected: the black F#3 G#3 A#3 C#4 pick its lane in edit, L1 .. L4 (Tim; ALGORITHM stays the track's).
 * Not in a layer, the keyboard mode, a hold (HOME / ARP) or with SHIFT (its black-key combos) */
static const uint8_t SM_LANE_KEY[SM_NLANE] = {1, 3, 5, 8};
static void sm_lane_keys(uint32_t notes)
{
    uint32_t L;
    if (sm_layer || sm_ui.kbd || sm_hold_btn() != 0xFFu || sm_ui.trk % SM_NTRK != SM_KIT_TRK ||
        !sm_trk_kit(SM_KIT_TRK, &smp.pat[smp.cur % SM_NPAT]))
        return;
    for (L = 0; L < SM_NLANE; L++)
        if ((notes >> SM_LANE_KEY[L]) & 1u) {
            sm_kit_lane = (uint8_t)L;
            sm_ui.held_step = sm_ui.sel_step = 0xFFu;
            sm_dirty = 1;
        }
}
/* FILL (Tim: SHIFT + G5, REC / ARM off): while both are held, COND FILL / !FILL play their FILL side; FILL in the header the while. (GLO
 * held + G5 too: sm_layer.c) */
static void sm_fill_poll(void)
{
    uint32_t f;
    if (sm_layer)                                         /* (a layer's own: GLO + G5) */
        return;
    f = sm_shift && !sm_rec && ((fm1_in.notes >> SM_SHK_FILL) & 1u);   /* (Tim: with REC / ARM off only) */
    if (f != sm_fill) {
        sm_fill = (uint8_t)f;                             /* (a byte: the audio stream reads it at a COND) */
        if (!f && sm_msg == SM_MSG_FILL)
            sm_msg = SM_MSG_NONE;
        sm_dirty = 1;
    }
    if (f && (sm_msg != SM_MSG_FILL || (uint32_t)(fm1_ticks() - sm_msg_t0) > SM_MSG_US * FM1_TICKS_PER_US / 2u))
        sm_msg_show(SM_MSG_FILL);                         /* (shown on while held) */
}
/* SHIFT: OCT- held outside the keyboard mode (sm_shift is this, minus a layer open: sm_oct_poll) */
static uint32_t sm_shift_held(void) { return !sm_ui.kbd && ((fm1_in.buttons >> panel.btn[B_OCTDN]) & 1u); }

/* SHIFT + a black key (outside the keyboard mode and the layers; the white keys are SHIFT's trig copy): D#4 / F#4 (keys 10, 13) the
 * track's steps one back / forward, D#5 / F#5 (keys 22, 25) its notes a semitone down / up; the net shown in the header for a second */
#define SM_SHK_BACK 10u
#define SM_SHK_FWD 13u
#define SM_SHK_TDN 22u
#define SM_SHK_TUP 25u
static void sm_shift_keys(uint32_t notes)
{
    uint32_t t = sm_ui.trk % SM_NTRK, len = (uint32_t)clamp(sm_ui_sound()->len, 1, (int32_t)SM_NSTEP);
    int32_t d = 0;
    if (((notes >> SM_SHK_BACK) & 1u) || ((notes >> SM_SHK_FWD) & 1u)) {
        int32_t v;
        d = ((notes >> SM_SHK_FWD) & 1u) ? 1 : -1;
        sm_shift_trk(t, d);
        v = ((int32_t)sm_sh_acc[t] + d) % (int32_t)len;
        if (v > (int32_t)len / 2)
            v -= (int32_t)len;
        else if (v < -(int32_t)len / 2)
            v += (int32_t)len;
        sm_sh_acc[t] = (int8_t)v;
        sm_msg_show_val(SM_MSG_SHIFT, v);
    }
    if (((notes >> SM_SHK_TDN) & 1u) || ((notes >> SM_SHK_TUP) & 1u)) {
        d = ((notes >> SM_SHK_TUP) & 1u) ? 1 : -1;
        sm_trn(t, d);
        sm_trn_acc[t] = (int8_t)clamp((int32_t)sm_trn_acc[t] + d, -99, 99);
        sm_msg_show_val(SM_MSG_TRN, sm_trn_acc[t]);
    }
}

/* SAVE held (HOLD_MS, as a layer's hold): UNDO, once a hold (a tap: the SAVE page, a later stage) */
static void sm_save_poll(uint32_t pressed, uint32_t now)
{
    uint32_t b = panel.btn[B_SAVE];
    if ((pressed >> b) & 1u) {
        sm_save_st = 1;
        sm_save_t0 = now;
        if (sm_shift_held() && !sm_layer) {               /* SHIFT + SAVE: the quick save (as SEQ + F#5); the hold is no UNDO */
            sm_save_st = 2;
            sm_save_ask();
        }
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
                for (w = 0; w < SM_PDW; w++)
                    sm_pdirty[t][w] = 0;                  /* (the sound carries every value of the model) */
            }
            continue;                                     /* (the values wait for the sound: a LOAD changes the engine) */
        }
        for (w = 0; w < SM_PDW; w++)
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
    sm_bank_poll(now);                                    /* (a bank brought in, a request served, SAVE at STOP, the autosave) */
    fx_bpm10 = (uint16_t)sm_bpm_eff();                    /* (the delay's divisions, LFO SYNC, ESYNC: fx.c beat_samples; the external clock's) */
    /* the family buttons (EDIT GLO SEQ: a tap is the page, a hold the layer; ENV: the page): a key or a knob used while one is
     * down is a layer too; in an open layer the keys and the four knobs and SELECT are the layer's, not the page's */
    sm_layer_begin(pressed, now, notes || sel || alg || pre || k[0] || k[1] || k[2] || k[3]);
    sm_fx_poll(notes, k);                                 /* (FX held: the punch-in effects; a key let go ends its effect, FX held or not) */
    if (sm_layer) {
        sm_layer_keys(pressed, notes, k, alg, sel);
        if (sm_layer == SML_SEQ)
            alg = 0;                                      /* (SEQ: ALGORITHM picks a pattern, not the track) */
        notes = 0;
        sel = 0;
        for (i = 0; i < 4; i++)
            k[i] = 0;
    }
    sm_layer_end();
    sm_hold_keys(notes);                                  /* (HOME / ARP held: F3 .. B3 the track, F3 the LATCH) */
    if (sm_hold_btn() != 0xFFu)
        notes = 0;
    sm_msg_poll(now);
    sm_save_poll(pressed, now);
    sm_rec_poll(pressed);
    sm_lrec_poll();                                       /* (live record: the notes the audio stream placed) */
    if (((pressed >> panel.btn[B_PLAY]) & 1u) && sm_key_arm) {   /* a trig held + PLAY (Tim): the trig copied pasted there, no transport */
        sm_msg_show(sm_tclip_ok ? SM_MSG_GPASTE : SM_MSG_GEMPTY);
        sm_paste_held();
        sm_dirty = 1;
    } else if (((pressed >> panel.btn[B_PLAY]) & 1u) && ((fm1_in.buttons >> panel.btn[B_REC]) & 1u)) {
        /* REC held + PLAY (Elektron's live record): recording on whatever REC's own press made of it (on from grid record,
         * that press turned it off), and playing (started; already playing: it plays on, a STOP not pushed yet dropped) */
        sm_rec = 1;
        sm_live = 1;
        sm_rec_mtrk = (uint8_t)(sm_ui.trk % SM_NTRK);
        sm_tr_pend = (uint8_t)(sm_playing ? (sm_tr_pend == 1u ? 1u : 0u) : 1u);
        sm_dirty = 1;
    } else if ((pressed >> panel.btn[B_PLAY]) & 1u) {     /* (against a PLAY / STOP not pushed yet, if there is one) */
        sm_tr_pend = (uint8_t)(sm_tr_pend ? (sm_tr_pend == 1u ? 2u : 1u) : (sm_playing ? 2u : 1u));
    }
    if (alg) {                                            /* the track (T1 .. T4 as ever; a KIT's lane: the black keys, sm_lane_keys) */
        int32_t t = ((int32_t)sm_ui.trk + alg) % (int32_t)SM_NTRK;
        sm_ui.trk = (uint8_t)(t < 0 ? t + (int32_t)SM_NTRK : t);
        sm_ui.held_step = sm_ui.sel_step = 0xFFu;
        sm_dirty = 1;
    }
    sm_oct_poll(pressed, now, (pressed & ~((1u << panel.btn[B_OCTDN]) | (1u << panel.btn[B_OCTUP]))) || notes || sel || alg ||
                               pre || k[0] || k[1] || k[2] || k[3]);
    notes = sm_che_poll(notes);                           /* (the chord entry: ARM, a trig + D#5) */
    if (!sm_layer && sm_shift_held())
        sm_shift_keys(notes);
    else
        sm_lane_keys(notes);
    sm_fill_poll();                             /* (SHIFT + D#4 F#4 D#5 F#5) */
    sm_arp_poll();
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
    for (i = 0; i < 4; i++)                               /* SHIFT: the turn accelerated (after the keys saw the detents) */
        k[i] = sm_accel((uint32_t)i, k[i]);
    sel = sm_accel(4u, sel);
    if (sm_page_is_step()) {
        sm_step_page(k, sel, pre);
    } else if (sm_page_home() == SMH_EUCLID) {
        sm_eu_page(k);
    } else if (sm_page_home() == SMH_MIDI) {
        /* K1..K4: the MIDI channel of T1..T4, 0 OFF (no external MIDI on any channel), 1..16 the channel */
        for (i = 0; i < SM_NTRK; i++)
            if (k[i]) {
                int32_t v = ((int32_t)sm_midi_ch[i] + k[i]) % 17;
                if (v < 0)
                    v += 17;
                if ((uint8_t)v != sm_midi_ch[i]) {
                    sm_midi_ch[i] = (uint8_t)v;
                    sm_dirty = 1;
                    sm_midi_release_req(i);               /* (a new channel or OFF: the drain lets the old channel's notes go) */
                }
            }
    } else if (sm_page_home() == SMH_SETUP) {             /* K1 CLK, K2 MODE, K3 TRNS, K4 SRC, each round */
        uint8_t w;
        if (k[0]) {                                       /* (round over the choices SRC allows: sm_midi_clk_ok) */
            int32_t c = sm_midi_clk, j, d = k[0] < 0 ? -1 : 1, n = k[0] < 0 ? -k[0] : k[0];
            while (n--) {
                for (j = 0; j < (int32_t)SM_CLK_COUNT; j++) {
                    c = ((c + d) % (int32_t)SM_CLK_COUNT + (int32_t)SM_CLK_COUNT) % (int32_t)SM_CLK_COUNT;
                    if (sm_midi_clk_ok((uint32_t)c))
                        break;
                }
            }
            sm_midi_clk = (uint8_t)c;
            sm_dirty = 1;
        }
        w = (uint8_t)((((int32_t)smp.mode + k[1]) % SM_PM_COUNT + SM_PM_COUNT) % SM_PM_COUNT);
        if (k[1] && w != smp.mode) {
            smp.mode = w;
            sm_dirty = 1;
        }
        if (k[2] && (k[2] & 1)) {                         /* (an odd turn flips it) */
            sm_midi_trnoff = (uint8_t)!sm_midi_trnoff;
            sm_dirty = 1;
        }
        w = (uint8_t)((((int32_t)sm_midi_src + k[3]) % 3 + 3) % 3);
        if (k[3] && w != sm_midi_src) {
            sm_midi_src = w;
            if (!sm_midi_clk_ok(sm_midi_clk))             /* (a port no longer in use: its clock gone, AUTO) */
                sm_midi_clk = SM_CLK_AUTO;
            sm_dirty = 1;
        }
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
        if (sm_page_param(4) != SM_NOPARAM && sel) {
            sm_filt_auto(sm_page_param(4));
            sm_turn(sm_page_param(4), sel);
        }
    }
    sm_keys_release(now);
    sm_flush();
}
