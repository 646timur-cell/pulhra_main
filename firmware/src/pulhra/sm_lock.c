/* SPDX-License-Identifier: GPL-3.0-only */
/* PULHRA parameter locks: the pattern's pool of SM_NLOCK entries {ts = trk << 6 | step, param, val}, nlock of them
 * in use. Ownership (sm_model.h): the main loop is the ONLY writer of the pool; the audio stream reads it when a trig
 * fires (sm_seq.c sm_seq_locks) and is never preempted by the main loop, so it sees the pool between two stores of the
 * main loop, never inside one. Every write below keeps each of those in-between states whole:
 *  - a new value of an existing lock: one 16-bit store of val;
 *  - a new lock in a removed slot (param 0xFF): ts and val first, SM_BARRIER(), then the param byte (until that store
 *    the slot is still a removed one, skipped);
 *  - a new lock at the end: ts, param, val, SM_BARRIER(), then nlock + 1 (the reader never looks past nlock);
 *  - a removed lock: its param byte 0xFF (a tombstone; one byte store; never matches, sm_lock_find, sm_seq_locks);
 *  - compaction (the tombstones squeezed out, nlock down): many stores, so only while stopped AND with the audio
 *    interrupt off for the copy (<= 192 x 4 bytes, a few microseconds; a block is 0.73 ms): a PLAY still in the queue
 *    could otherwise start the sequencer between two of its stores. While playing the tombstones stay and are reused.
 * A lock lives only on a step with a trig (n > 0): REC writes none elsewhere; the gesture on an EMPTY step makes a REST
 * trig (Elektron's trigless lock: n 1 with SMF_REST, its locks fire, no note) after the lock is written; taking a trig
 * off removes its locks. */
#define SM_LOCK_DEAD 0xFFu
#define SM_MSG_US 1000000u                                /* a message: 1 s in the header */
#define SM_MSG_BLINK_US 125000u                           /* CLEAR (SEQ + F3): inverted / plain every 125 ms of its second */
#ifndef SM_LOCK_PREEMPT
#define SM_LOCK_PREEMPT() ((void)0)                       /* (the host test runs an audio block here: the interrupt's moment) */
#endif

/* the header's messages (the only ones: LOCKS FULL, CLEARED, UNDO; SAVED and the flash's error come with the storage), each
 * for SM_MSG_US, the header inverted */
enum { SM_MSG_NONE, SM_MSG_LOCKS, SM_MSG_CLEARED, SM_MSG_UNDO, SM_MSG_SAVED, SM_MSG_FLASH, SM_MSG_SHIFT, SM_MSG_TRN, SM_MSG_SHT, SM_MSG_TR,
       SM_MSG_NUDGE, SM_MSG_SAVEPEND, SM_MSG_PTNCLR, SM_MSG_FILL, SM_MSG_TCOPY, SM_MSG_TPASTE, SM_MSG_TCOPYP, SM_MSG_TPASTEP, SM_MSG_CHORDIN, SM_MSG_CHORDSET, SM_MSG_GCOPY, SM_MSG_GPASTE, SM_MSG_GEMPTY, SM_MSG_PCOPY, SM_MSG_PPASTE, SM_MSG_LOADED, SM_MSG_T4ONLY, SM_MSG_PTCOPY, SM_MSG_PTPASTE, SM_MSG_PTCOPYP, SM_MSG_PTPASTEP, SM_MSG_PTEMPTY, SM_MSG_COUNT };
static const char *const SM_MSG_TEXT[SM_MSG_COUNT] = {"", "LOCKS FULL", "CLEAR", "UNDO", "SAVED", "FLASH ERROR", "SHIFT", "TRN", "SHT", "TR", "NUDGE", "SAVE AT STOP", "PTN CLEAR", "FILL", "TRACK COPIED", "TRACK PASTED", "HOLD: COPY TRACK", "HOLD: PASTE TRACK", "PLAY CHORD", "CHORD SET", "TRIG COPIED", "TRIG PASTED", "NO TRIG COPIED", "PAGE COPIED", "PAGE PASTED", "LOADED", "T4 ONLY", "PTN COPIED", "PTN PASTED", "HOLD: COPY PTN", "HOLD: PASTE PTN", "NO PTN COPIED"};
/* NUDGE: the STEP page's OCT- / OCT+ with a trig key held: NUDGE+3, shown only then (a second) */
/* SHT, TR: SEQ held + A#3 / C#4 (all the tracks shifted), D#4 / F#4 (transposed); the net count follows with its sign: SHT+2, TR-1, SHT+0 */
static int16_t sm_msg_val;                                /* SHIFT, TRN: the signed value shown after the text ("SHIFT +2") */
static uint8_t sm_msg;                                    /* the message in the header (since sm_msg_t0), SM_MSG_NONE none */
static uint32_t sm_msg_t0;

/* the index in p->lock[] of the lock of (track t, step s, parameter param), -1: none */
static int32_t sm_lock_find(const sm_pattern_t *p, uint32_t t, uint32_t s, uint32_t param)
{
    uint32_t i, ts = SM_TS(t, s), n = p->nlock;
    if (param >= SM_LOCK_DEAD)
        return -1;
    if (n > SM_NLOCK)
        n = SM_NLOCK;
    for (i = 0; i < n; i++)
        if (p->lock[i].ts == ts && p->lock[i].param == param)
            return (int32_t)i;
    return -1;
}

/* is the parameter locked on any step of the track (the dot in the centre of its ring) */
static int sm_param_locked_any(uint32_t t, uint32_t param)
{
    const sm_pattern_t *p = &smp.pat[smp.cur % SM_NPAT];
    uint32_t i, n = p->nlock;
    if (param >= SM_LOCK_DEAD)
        return 0;
    if (n > SM_NLOCK)
        n = SM_NLOCK;
    for (i = 0; i < n; i++)
        if (SM_TS_T(p->lock[i].ts) == t && p->lock[i].param == param)
            return 1;
    return 0;
}

/* a message for a second (a newer one takes its place) */
static void sm_msg_show(uint32_t id)
{
    sm_msg = (uint8_t)(id < SM_MSG_COUNT ? id : SM_MSG_NONE);
    sm_msg_t0 = fm1_ticks();
    sm_dirty = 1;
}
static void sm_msg_show_val(uint32_t id, int32_t v)
{
    sm_msg_val = (int16_t)v;
    sm_msg_show(id);
}
static void sm_lock_full_msg(void) { sm_msg_show(SM_MSG_LOCKS); }   /* (the only message of the locks) */

/* every poll: the message gone after its second */
/* the messages that blink (inverted / plain every SM_MSG_BLINK_US): CLEAR, PTN CLEAR, and the save's: SAVED, SAVE AT STOP, FLASH ERROR */
static uint32_t sm_msg_blinks(uint32_t m)
{
    return m == SM_MSG_CLEARED || m == SM_MSG_PTNCLR || m == SM_MSG_CHORDIN || m == SM_MSG_CHORDSET || m == SM_MSG_GCOPY || m == SM_MSG_GPASTE || m == SM_MSG_PCOPY || m == SM_MSG_PPASTE || m == SM_MSG_TCOPY || m == SM_MSG_TPASTE || m == SM_MSG_SAVED || m == SM_MSG_SAVEPEND || m == SM_MSG_FLASH;
}
static void sm_msg_poll(uint32_t now)
{
    /* (signed: a message shown later in this poll than `now` was read, as every one the poll's own handlers show, is
     * younger than now - that must not wrap round to "four billion ticks old" and end it at once: on the FM-1 the ticks run
     * on within a poll, CLEAR and SAVED were never seen. SAVED / SAVE AT STOP / FLASH ERROR: two seconds) */
    uint32_t dur = (sm_msg == SM_MSG_SAVED || sm_msg == SM_MSG_SAVEPEND || sm_msg == SM_MSG_FLASH) ? 2u * SM_MSG_US : SM_MSG_US;
    if (sm_msg && (int32_t)(now - sm_msg_t0) >= (int32_t)(dur * FM1_TICKS_PER_US)) {
        sm_msg = SM_MSG_NONE;
        sm_dirty = 1;
    } else if (sm_msg_blinks(sm_msg)) {
        sm_dirty = 1;                                     /* (CLEAN, SAVED .. blink: the header's signature has the phase) */
    }
}

/* the removed entries squeezed out (the order of the live ones kept). Stopped only, the interrupt off for the copy */
static void sm_lock_compact(sm_pattern_t *p)
{
    uint32_t i, j, n;
    fm1_irq_off();
    if (!sm_playing) {
        n = p->nlock > SM_NLOCK ? SM_NLOCK : p->nlock;
        for (i = j = 0; i < n; i++)
            if (p->lock[i].param != SM_LOCK_DEAD)
                p->lock[j++] = p->lock[i];
        p->nlock = (uint16_t)j;
    }
    fm1_irq_on();
}

/* the main loop: the lock of (t, s, param) is v (the caller clamps it). 1 = set; 0 = the pool is full (LOCKS FULL, nothing
 * changed). An existing lock takes the value; else a removed slot is taken again; else one at the end */
static int sm_lock_set(uint32_t t, uint32_t s, uint32_t param, int32_t v)
{
    sm_pattern_t *p = &smp.pat[smp.cur % SM_NPAT];
    int32_t i;
    uint32_t k, n;
    if (t >= SM_NTRK || s >= SM_NSTEP || param >= SM_NPID)
        return 0;
    i = sm_lock_find(p, t, s, param);
    if (i >= 0) {
        SM_LOCK_PREEMPT();
        p->lock[i].val = (int16_t)v;                      /* (one 16-bit store) */
        return 1;
    }
    n = p->nlock > SM_NLOCK ? SM_NLOCK : p->nlock;
    for (k = 0; k < n; k++)
        if (p->lock[k].param == SM_LOCK_DEAD)
            break;
    if (k < n) {                                          /* a removed slot: still skipped until its param byte lands */
        p->lock[k].ts = SM_TS(t, s);
        p->lock[k].val = (int16_t)v;
        SM_LOCK_PREEMPT();
        SM_BARRIER();
        p->lock[k].param = (uint8_t)param;
        return 1;
    }
    if (n >= SM_NLOCK) {
        sm_lock_full_msg();
        return 0;
    }
    p->lock[n].ts = SM_TS(t, s);
    p->lock[n].param = (uint8_t)param;
    p->lock[n].val = (int16_t)v;
    SM_LOCK_PREEMPT();
    SM_BARRIER();
    p->nlock = (uint16_t)(n + 1u);                        /* (counted last: the entry is whole before the reader sees it) */
    return 1;
}

/* the main loop: every lock of step s of track t removed (tombstones); stopped, the pool compacted at once */
static void sm_lock_clear_step(uint32_t t, uint32_t s)
{
    sm_pattern_t *p = &smp.pat[smp.cur % SM_NPAT];
    uint32_t i, n = p->nlock > SM_NLOCK ? SM_NLOCK : p->nlock, ts = SM_TS(t, s), any = 0;
    for (i = 0; i < n; i++)
        if (p->lock[i].ts == ts && p->lock[i].param != SM_LOCK_DEAD) {
            SM_LOCK_PREEMPT();
            p->lock[i].param = SM_LOCK_DEAD;
            any = 1;
        }
    if (any && !sm_playing)
        sm_lock_compact(p);
}

/* the main loop: the locks of KIT lane L on step s of track t gone (its condition, its values, mix, filter, envelope) */
static void sm_lock_clear_lane(uint32_t t, uint32_t s, uint32_t L)
{
    sm_pattern_t *p = &smp.pat[smp.cur % SM_NPAT];
    uint32_t i, n = p->nlock > SM_NLOCK ? SM_NLOCK : p->nlock, ts = SM_TS(t, s), any = 0;
    for (i = 0; i < n; i++) {
        uint32_t q = p->lock[i].param;
        if (p->lock[i].ts != ts || q == SM_LOCK_DEAD)
            continue;
        if ((sm_is_lp(q) && (q - SM_LP0) / 9u == L) || (sm_is_lm(q) && (q - SM_LM0) / 4u == L) ||
            (sm_is_lf(q) && (q - SM_LF0) / 3u == L) || q == SM_LC(L) || (sm_is_le(q) && (q - SM_LE0) / 4u == L)) {
            SM_LOCK_PREEMPT();
            p->lock[i].param = SM_LOCK_DEAD;
            any = 1;
        }
    }
    if (any && !sm_playing)
        sm_lock_compact(p);
}

/* the main loop: the lock of (t, s, id) turned by d detents from its value (none yet: from the base), clamped. A step
 * without a trig: 0 (REC), or, mk (the held-key gesture), a REST trig made for it: the lock first, then the step's
 * fields, a barrier, n (sm_step_toggle's order); the pool full: neither */
static int sm_lock_turn(uint32_t t, uint32_t s, uint32_t id, int32_t d, int mk)
{
    sm_pattern_t *p = &smp.pat[smp.cur % SM_NPAT];
    const sm_sound_t *sd = &p->kit.snd[t % SM_NTRK];
    const param_desc_t *dc = sm_desc_trk(sd, id);
    int32_t i, v;
    uint32_t make;
    if (!d || !dc || t >= SM_NTRK || s >= SM_NSTEP)
        return 0;
    make = !p->step[t][s].n;
    if (make && !mk)
        return 0;
    i = sm_lock_find(p, t, s, id);
    v = i >= 0 ? p->lock[i].val : sm_pget(p, t, id);
    v = sm_val_turn(dc, id, v, d);
    if (!sm_lock_set(t, s, id, v))
        return 0;
    if (make) {                                           /* (n 0: the audio stream reads none of it yet) */
        sm_step_fresh(t, s, SMF_REST);
        SM_BARRIER();
        p->step[t][s].n = 1;
    }
    sm_dirty = 1;
    return 1;
}
