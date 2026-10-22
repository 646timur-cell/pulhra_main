/* SPDX-License-Identifier: GPL-3.0-only */
/* PULHRA arpeggiator. A track whose ARP MODE (the kit's p[P_AMODE], so it travels with the pattern) is not OFF turns
 * the notes it is given into a stream of notes: the keyboard mode's keys of the track and MIDI IN's notes on its channel go
 * to the arp (sm_input.c sm_kbd_poll, sm_midi.c), not to the voices. The parameters are the track's (params.c TP): MODE
 * (N_AMODE: OFF UP DN UPDN RND ORD REPEAT DNUP UP+8 CONV DIVG PINKY THUMB WALK CHORD), RATE (N_DIV: 1/4 1/8 1/16 1/32 8T 16T ..),
 * OCT 1..4 (the notes again an octave up, that many times), GATE (the note's length in 1/128 of the step: 64 is 50 %), SWING
 * (0..100: every second step starts that percent of half a step late), PROB (0..127: the chance a step plays; 127 always,
 * 0 never), LATCH (P_AHOLD: the notes stay after the keys are let go; a key pressed with none down replaces them) and ORDER
 * (NOTE: low to high; PLAY: as pressed; the ORD mode is always as pressed). Read from the pattern playing, the base value (a
 * lock of these is not followed).
 *
 * Ownership. The note SETS are the input: the keyboard's (sm_ak) the main loop is the only writer of; MIDI's (sm_am) the audio
 * stream is the only writer of (the MIDI notes arrive on its side). Each is written fields first, then SM_BARRIER(), then the
 * count (sm_aset_*): the audio stream reads the count once, then the notes below it, and so sees a set that is whole, or one
 * note short or one over for the moment (a removal shifts the notes down before the count goes down: the set it sees holds
 * every note that stays). Everything else here (the clock, the note book, the position) is the audio stream's alone.
 *
 * The clock. Each track has its own step clock of RATE, in q8 samples from the start of the block as sm_seq.c's (the tempo is
 * smp.bpm10; a step plays at the start of the block it falls in). PLAY and STOP put every arp back to its step 0 at the
 * sequencer's step 0 (sm_arp_sync), so while playing a rate that divides the 1/16 grid is on the sequencer's grid; stopped it
 * runs on, from the tempo alone. A key pressed while stopped with the arp idle starts it at once (step 0 now); playing, at the
 * next step of the grid. The list: the notes of both sets, once each, ordered, over OCT octaves; the step picks from it by
 * the MODE (the position counts steps since the set was empty; a step that rolls PROB out still counts).
 *
 * The notes. The arp keeps its own book (sm_ar: up to 4, CHORD's) next to the sequencer's (sm_held), MIDI's (sm_mn) and the
 * keyboard's (sm_kn): it ends only its own, and only where no other side holds the same note (and the others leave a note
 * the arp holds: sm_midi_has). A note ends after its GATE; a new step, a muted track, a MODE change (any), STOP and the panic
 * end whatever the arp still sounds, so nothing hangs. MODE OFF empties the sets (the keyboard's: sm_input.c). A muted track
 * plays no arp note; its clock runs on. REC + playing on the selected track (sm_rec_mtrk): every arp note is recorded as the
 * keys are (sm_rec_on at the note, sm_rec_off at its end), so the pattern gets the trigs the arp played. */
#define SM_AK_N 16u                                       /* notes a set holds */
#define SM_AR_N 4u                                        /* the arp's book (CHORD: 4 notes) */
#define SM_AR_LIST 64u
#define SM_AR_NONE 0xFFFFFFFFu
enum { SM_AM_OFF, SM_AM_UP, SM_AM_DN, SM_AM_UPDN, SM_AM_RND, SM_AM_ORD, SM_AM_REPEAT, SM_AM_DNUP, SM_AM_UP8, SM_AM_CONV,
       SM_AM_DIVG, SM_AM_PINKY, SM_AM_THUMB, SM_AM_WALK, SM_AM_CHORD, SM_AM_COUNT };

typedef struct { uint8_t note[SM_AK_N]; uint8_t vel, n; } sm_aset_t;
static sm_aset_t sm_ak[SM_NTRK];                          /* the keyboard mode's keys given to the arp (the main loop writes) */
static sm_aset_t sm_am[SM_NTRK];                          /* MIDI IN's notes given to the arp (the audio stream writes) */
static uint8_t sm_klatch;                                 /* bit per track: the keyboard's LATCH (ARP held + F3, sm_input.c); the main loop's */
static uint32_t sm_amdn[SM_NTRK][4];                      /* MIDI notes down now, bit per note (LATCH's "none down") */

static uint8_t sm_ar[SM_NTRK][SM_AR_N], sm_arn[SM_NTRK]; /* the arp's own note book */
static uint32_t sm_ar_next[SM_NTRK], sm_ar_off[SM_NTRK]; /* the next step, the gate's end: q8 from this block's start */
static int32_t sm_ar_g[SM_NTRK];                          /* the next step's place on the grid (without swing) */
static uint32_t sm_ar_k[SM_NTRK], sm_ar_idx[SM_NTRK];     /* the step counted since step 0 (swing's parity), the position in the list */
static uint8_t sm_ar_mode[SM_NTRK], sm_ar_walk[SM_NTRK], sm_ar_idle[SM_NTRK];   /* the mode seen; WALK's place; no notes were given */
static uint8_t sm_ar_base[2u * SM_AK_N], sm_ar_lst[SM_AR_LIST];   /* (the list under construction: the interrupt never nests) */

static int32_t sm_arp_param(uint32_t t, uint32_t id)      /* the pattern playing's base value (the audio stream's view) */
{
    return smp.pat[sm_play_pat % SM_NPAT].kit.snd[t % SM_NTRK].p[id];
}
static uint32_t sm_arp_mode(uint32_t t)
{
    int32_t m = sm_arp_param(t, P_AMODE);
    return m > 0 && m < (int32_t)SM_AM_COUNT ? (uint32_t)m : 0u;
}

/* ---- the sets: one writer each, fields -> SM_BARRIER() -> count ---- */
static void sm_aset_add(sm_aset_t *s, uint32_t note, uint32_t vel)
{
    uint32_t i, n = s->n;
    s->vel = (uint8_t)(vel < 1u ? 1u : vel > 127u ? 127u : vel);
    for (i = 0; i < n; i++)
        if (s->note[i] == note)
            return;                                       /* (a repeated note-on is not a new note) */
    if (n >= SM_AK_N)
        return;
    s->note[n] = (uint8_t)note;
    SM_BARRIER();
    s->n = (uint8_t)(n + 1u);
}
static void sm_aset_remove(sm_aset_t *s, uint32_t note)
{
    uint32_t i, j, n = s->n;
    for (i = 0; i < n && s->note[i] != note; i++)
        ;
    if (i == n)
        return;
    for (j = i; j + 1u < n; j++)
        s->note[j] = s->note[j + 1u];
    SM_BARRIER();
    s->n = (uint8_t)(n - 1u);
}
static void sm_aset_clear(sm_aset_t *s) { s->n = 0; }

/* the keyboard keys' LATCH of track t: the arp's own (P_AHOLD, ARP 2) or the keyboard's (sm_klatch): the same semantics either way */
static uint32_t sm_arp_latch(uint32_t t)
{
    t %= SM_NTRK;
    return smp.pat[smp.cur % SM_NPAT].kit.snd[t].p[P_AHOLD] != 0 || ((sm_klatch >> t) & 1u);
}

/* the main loop (sm_input.c): a keyboard key for track t. others_down: another key of the arp is down on the track */
static void sm_arp_key_on(uint32_t t, uint32_t note, uint32_t vel, uint32_t others_down)
{
    t %= SM_NTRK;
    if (sm_arp_latch(t) && !others_down)
        sm_aset_clear(&sm_ak[t]);                         /* (LATCH: a new chord replaces the latched one) */
    sm_aset_add(&sm_ak[t], note & 0x7Fu, vel);
}
static void sm_arp_key_off(uint32_t t, uint32_t note, uint32_t force)
{
    t %= SM_NTRK;
    if (force || !sm_arp_latch(t))
        sm_aset_remove(&sm_ak[t], note & 0x7Fu);
}
/* the main loop: track t's keyboard set replaced by these c notes (LATCH turned off: only the keys down stay) */
static void sm_arp_key_set(uint32_t t, const uint8_t *notes, uint32_t c)
{
    sm_aset_t *s = &sm_ak[t % SM_NTRK];
    uint32_t i;
    s->n = 0;
    SM_BARRIER();
    for (i = 0; i < c && i < SM_AK_N; i++)
        s->note[i] = notes[i];
    SM_BARRIER();
    s->n = (uint8_t)(c < SM_AK_N ? c : SM_AK_N);
}

/* the audio stream: MIDI IN's note for track t's arp (sm_midi.c) */
static void sm_arp_midi_on(uint32_t t, uint32_t note, uint32_t vel)
{
    uint32_t any = sm_amdn[t][0] | sm_amdn[t][1] | sm_amdn[t][2] | sm_amdn[t][3];
    if (sm_arp_param(t, P_AHOLD) && !any)
        sm_aset_clear(&sm_am[t]);
    sm_amdn[t][note >> 5] |= 1u << (note & 31u);
    sm_aset_add(&sm_am[t], note, vel);
}
/* ... its note-off: 1 if the arp had the note down (nothing else is to be done with it), 0 if not (a note from before the MODE) */
static uint32_t sm_arp_midi_off(uint32_t t, uint32_t note)
{
    if (!((sm_amdn[t][note >> 5] >> (note & 31u)) & 1u))
        return 0;
    sm_amdn[t][note >> 5] &= ~(1u << (note & 31u));
    if (!sm_arp_param(t, P_AHOLD))
        sm_aset_remove(&sm_am[t], note);
    return 1;
}

/* ---- the notes ---- */
static uint32_t sm_arp_has(uint32_t t, uint32_t note)
{
    uint32_t i;
    t %= SM_NTRK;
    for (i = 0; i < sm_arn[t]; i++)
        if (sm_ar[t][i] == note)
            return 1;
    return 0;
}

/* the arp's notes on track t end: each on the core unless another side holds that note too; a recorded one is let go */
static void sm_ar_end(uint32_t t)
{
    uint32_t i, n = sm_arn[t];
    uint8_t b[SM_AR_N];
    memcpy(b, sm_ar[t], sizeof b);
    sm_arn[t] = 0;
    sm_ar_off[t] = SM_AR_NONE;
    for (i = 0; i < n; i++) {
        sm_rec_off(t, b[i]);
        if (!sm_seq_has(t, b[i]) && !sm_mn_has(t, b[i]) && !sm_kn_has(t, b[i]))
            trk_note_off(&trk[t], b[i]);
    }
}
static void sm_ar_start(uint32_t t, uint32_t note, uint32_t vel)
{
    if (sm_arn[t] >= SM_AR_N)
        return;
    sm_ar[t][sm_arn[t]++] = (uint8_t)note;
    if (t == (uint32_t)sm_rec_mtrk && sm_playing)
        sm_rec_on(t, note, vel);                          /* (REC + playing: the arp's note is a trig) */
    trk_note_on(&trk[t], note, vel);
}

/* the list of track t: both sets' notes once each (*cnt of them, before the octaves), ordered, over OCT octaves; its length.
 * *last: the note pressed last (REPEAT), *vel: the velocity of the newest set */
static uint32_t sm_ar_build(uint32_t t, uint32_t mode, uint32_t *cnt, uint32_t *last, uint32_t *vel)
{
    const sm_aset_t *s[2] = {&sm_ak[t], &sm_am[t]};
    uint32_t i, j, n = 0, len = 0, o, x, oct = (uint32_t)sm_arp_param(t, P_AOCT), w;
    oct = oct < 1u ? 1u : oct > 4u ? 4u : oct;
    *last = 0;
    *vel = 100u;
    for (w = 0; w < 2u; w++) {
        uint32_t c = s[w]->n, v;
        c = c > SM_AK_N ? SM_AK_N : c;
        SM_BARRIER();
        v = s[w]->vel;
        for (i = 0; i < c; i++) {
            x = s[w]->note[i] & 0x7Fu;
            for (j = 0; j < n && sm_ar_base[j] != x; j++)
                ;
            if (j == n && n < 2u * SM_AK_N)
                sm_ar_base[n++] = (uint8_t)x;
            *last = x;
        }
        if (c)
            *vel = v;
    }
    *cnt = n;
    if (!sm_arp_param(t, P_AORDER) && mode != SM_AM_ORD)  /* NOTE: low to high (ORD: as pressed) */
        for (i = 1; i < n; i++)
            for (j = i; j > 0 && sm_ar_base[j - 1u] > sm_ar_base[j]; j--) {
                x = sm_ar_base[j];
                sm_ar_base[j] = sm_ar_base[j - 1u];
                sm_ar_base[j - 1u] = (uint8_t)x;
            }
    for (o = 0; o < oct; o++)
        for (i = 0; i < n && len < SM_AR_LIST; i++) {
            int32_t v = (int32_t)sm_ar_base[i] + 12 * (int32_t)o;
            sm_ar_lst[len++] = (uint8_t)clamp(v, 0, 127);
        }
    if (n && mode == SM_AM_REPEAT) {
        sm_ar_lst[0] = (uint8_t)*last;
        len = 1;
    }
    return len;
}

/* the place in the list (len notes) step i plays; UP+8 adds the first note an octave over the top */
static uint32_t sm_ar_pick(uint32_t t, uint32_t mode, uint32_t i, uint32_t len)
{
    uint32_t cyc, k, w;
    if (mode == SM_AM_UP8 && len < SM_AR_LIST) {
        int32_t v = (int32_t)sm_ar_lst[0] + 12 * sm_arp_param(t, P_AOCT);
        sm_ar_lst[len++] = (uint8_t)clamp(v, 0, 127);
    }
    cyc = len > 1u ? 2u * len - 2u : 1u;
    k = i % cyc;
    switch (mode) {
    case SM_AM_DN:
        return len - 1u - i % len;
    case SM_AM_UPDN:
        return k < len ? k : cyc - k;
    case SM_AM_DNUP:
        return k < len ? len - 1u - k : k - (len - 1u);
    case SM_AM_RND:
        return (sm_rand() >> 8) % len;
    case SM_AM_CONV:
    case SM_AM_DIVG:
        k = i % len;
        if (mode == SM_AM_DIVG)
            k = len - 1u - k;
        return (k & 1u) ? len - 1u - k / 2u : k / 2u;
    case SM_AM_PINKY:
        return len < 2u ? 0u : (k & 1u) ? len - 1u : k / 2u;
    case SM_AM_THUMB:
        return len < 2u ? 0u : (k & 1u) ? 1u + k / 2u : 0u;
    case SM_AM_WALK:
        w = sm_ar_walk[t];
        if (!i)
            w = 0;
        else if (w >= len)
            w = len - 1u;
        else if (len > 1u)
            w = !w ? 1u : w == len - 1u ? w - 1u : (sm_rand() >> 9) & 1u ? w + 1u : w - 1u;
        sm_ar_walk[t] = (uint8_t)w;
        return w;
    default:                                              /* UP, ORD, UP+8, REPEAT */
        return i % len;
    }
}

/* the length of RATE (N_DIV's index) at the tempo, in q8 samples */
static uint32_t sm_ar_period(uint32_t rate)
{
    static const uint8_t DEN[6] = {1, 2, 4, 8, 3, 6};
    uint32_t q = sm_step_q8() * 4u;                       /* a quarter note */
    rate = rate > 9u ? 2u : rate;
    return rate < 6u ? q / DEN[rate] : q << (rate - 5u);
}

/* step k of track t's arp starts, at `at` (q8 from the block's start), a step of `period` q8 */
static void sm_ar_fire(uint32_t t, uint32_t at, uint32_t period, uint32_t mode)
{
    uint32_t cnt, last, vel, len, i, j, prob, gate;
    int32_t pv;
    sm_ar_end(t);                                         /* (a note still sounding ends where the next step starts) */
    len = sm_ar_build(t, mode, &cnt, &last, &vel);
    if (!len) {
        sm_ar_idx[t] = 0;
        sm_ar_idle[t] = 1;
        return;
    }
    sm_ar_idle[t] = 0;
    i = sm_ar_idx[t]++;
    pv = sm_arp_param(t, P_APROB);
    prob = (uint32_t)(pv < 0 ? 0 : pv > 127 ? 127 : pv);
    if (prob < 127u && (sm_rand() >> 8) % 127u >= prob)
        return;                                           /* (the step rests; it counted) */
    pv = sm_arp_param(t, P_AGATE);
    gate = (uint32_t)(pv < 1 ? 1 : pv > 127 ? 127 : pv);
    if (mode == SM_AM_CHORD) {                            /* the held notes together (the first four), an octave up each OCT step */
        uint32_t oct = (uint32_t)sm_arp_param(t, P_AOCT), o = i % (oct < 1u ? 1u : oct > 4u ? 4u : oct);
        for (j = 0; j < cnt && j < SM_AR_N; j++)
            sm_ar_start(t, (uint32_t)clamp((int32_t)sm_ar_lst[j] + 12 * (int32_t)o, 0, 127), vel);
    } else {
        sm_ar_start(t, sm_ar_lst[sm_ar_pick(t, mode, i, len)], vel);
    }
    sm_ar_off[t] = at + (uint32_t)(((uint64_t)period * gate) >> 7);
}

/* every arp back to step 0 (PLAY, STOP): the clock restarted at the sequencer's step 0, the position and WALK at the first note */
static void sm_arp_sync(void)
{
    uint32_t t;
    for (t = 0; t < SM_NTRK; t++) {
        sm_ar_next[t] = 0;
        sm_ar_g[t] = 0;
        sm_ar_k[t] = sm_ar_idx[t] = 0;
        sm_ar_walk[t] = 0;
        sm_ar_idle[t] = 0;
    }
}
/* SMC_PLAY: whatever the arp sounds ends, then step 0 */
static void sm_arp_play(void)
{
    uint32_t t;
    for (t = 0; t < SM_NTRK; t++)
        sm_ar_end(t);
    sm_arp_sync();
}
/* STOP and the panic: the core has let go of everything already (trk_all_off); the books are forgotten, the clock restarts */
static void sm_arp_forget(uint32_t t)
{
    t %= SM_NTRK;
    sm_arn[t] = 0;
    sm_ar_off[t] = SM_AR_NONE;
}
static void sm_arp_stop(void)
{
    uint32_t t;
    for (t = 0; t < SM_NTRK; t++) {
        sm_arp_forget(t);
        sm_ar_idle[t] = 1;
    }
    sm_arp_sync();
    for (t = 0; t < SM_NTRK; t++)
        sm_ar_idle[t] = 1;
}
static void sm_arp_init(void)
{
    uint32_t t;
    memset(sm_ak, 0, sizeof sm_ak);
    memset(sm_am, 0, sizeof sm_am);
    memset(sm_amdn, 0, sizeof sm_amdn);
    memset(sm_ar_mode, 0, sizeof sm_ar_mode);
    for (t = 0; t < SM_NTRK; t++)
        sm_arp_forget(t);
    sm_arp_sync();
    for (t = 0; t < SM_NTRK; t++)
        sm_ar_idle[t] = 1;
}

/* the audio stream, once a block (events_block, after the engines): the mode, the gates, the steps of the block in time order */
static void sm_arp_block(uint32_t n)
{
    uint32_t t, nq8 = n << 8, mute = sm_mute_eff();
    for (t = 0; t < SM_NTRK; t++) {
        uint32_t mode = sm_arp_mode(t), muted = (mute >> t) & 1u, period, sw, latch = sm_arp_param(t, P_AHOLD) != 0;
        if (mode != sm_ar_mode[t]) {                      /* a MODE change: what sounds ends, the position starts over */
            sm_ar_end(t);
            sm_ar_mode[t] = (uint8_t)mode;
            sm_ar_idx[t] = 0;
            sm_ar_walk[t] = 0;
            if (!mode) {
                sm_aset_clear(&sm_am[t]);
                memset(sm_amdn[t], 0, sizeof sm_amdn[0]);
            }
        }
        if (!latch && sm_am[t].n) {                       /* LATCH off: only the MIDI notes still down stay */
            uint32_t i = sm_am[t].n;
            while (i--) {
                uint32_t nt = sm_am[t].note[i];
                if (!((sm_amdn[t][nt >> 5] >> (nt & 31u)) & 1u))
                    sm_aset_remove(&sm_am[t], nt);
            }
        }
        if (muted)
            sm_ar_end(t);
        if (!sm_playing && sm_ar_idle[t] && mode && !muted && (sm_ak[t].n | sm_am[t].n)) {
            sm_ar_next[t] = 0;                            /* (stopped, idle, a note given: step 0 now) */
            sm_ar_g[t] = 0;
            sm_ar_k[t] = sm_ar_idx[t] = 0;
            sm_ar_walk[t] = 0;
            sm_ar_idle[t] = 0;
        }
        period = sm_ar_period((uint32_t)sm_arp_param(t, P_ARATE));
        sw = (uint32_t)sm_arp_param(t, P_ASWING);
        sw = sw > 100u ? 100u : sw;
        for (;;) {
            if (sm_ar_off[t] != SM_AR_NONE && sm_ar_off[t] < nq8 && sm_ar_off[t] <= sm_ar_next[t]) {
                sm_ar_end(t);
            } else if (sm_ar_next[t] < nq8) {
                uint32_t at = sm_ar_next[t], k;
                if (mode && !muted)
                    sm_ar_fire(t, at, period, mode);
                else
                    sm_ar_idle[t] = 1;
                sm_ar_k[t]++;
                k = sm_ar_k[t];
                sm_ar_g[t] += (int32_t)period;
                sm_ar_next[t] = (uint32_t)(sm_ar_g[t] + (int32_t)((k & 1u) ? (uint32_t)(((uint64_t)period * sw) / 200u) : 0u));
            } else {
                break;
            }
        }
        sm_ar_next[t] -= nq8;
        sm_ar_g[t] -= (int32_t)nq8;
        if (sm_ar_off[t] != SM_AR_NONE)
            sm_ar_off[t] -= nq8;
    }
}
