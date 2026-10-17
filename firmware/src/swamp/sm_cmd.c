/* SPDX-License-Identifier: GPL-3.0-only */
/* SWAMP MACHINE command queue: the main loop -> the audio stream. Ownership: the main loop is the only writer of the
 * model (smp: parameters, steps, lengths, tempo, everything); the audio stream only reads it and owns trk[], the
 * transport and the sequencer's state. So the main loop writes the model, then pushes a command for what the core must
 * take (a value, a sound, PLAY / STOP); events_block (sm_seq.c) drains the queue at the start of each audio block,
 * before the engines and the notes of the block, so the core never sees half an edit. A step is not a command: the
 * sequencer reads it from the model (sm_step_toggle below writes it so that a half-written step never plays).
 *
 * One producer, one consumer: sm_cmd_push from the main loop only (never from an interrupt), sm_cmd_drain from the audio
 * interrupt only. The target (pi32v2) has one core and the audio interrupt preempts the main loop, never the other way
 * round: a drain runs whole between two instructions of a push, a push never runs inside a drain. Lock-free:
 *  - a ring's head is written by the producer only, its tail by the consumer only (volatile uint8_t, free-running, the
 *    size a power of 2 that divides 256); the producer writes the slot, then a compiler barrier, then the head (one byte
 *    store: atomic); the consumer reads the head, a barrier, the slot, a barrier, then moves the tail;
 *  - SMC_PARAM when the ring is full: the newest pending value of the same {trk, param} is overwritten (coalescing).
 *    The drain may run between finding that slot and storing into it (it then takes the old value and the store lands in
 *    a free slot): after the store the producer reads the tail again, and if the slot is no longer pending it pushes the
 *    value once more (the ring has room then). A value applied twice is the same value: no harm. No interrupt masking.
 *  - SMC_PARAM with the ring full and no pending value of its {trk, param}: a second small ring of values only (sm_cov),
 *    drained after the main one and coalesced the same way. While it holds anything the main ring takes nothing (a later
 *    command must not overtake an earlier value), the values go on into sm_cov; anything else is refused (0). */
enum { SMC_PLAY, SMC_STOP, SMC_PARAM, SMC_LOAD, SMC_UNLOCK, SMC_PREVIEW };
/* SMC_UNLOCK {trk, step, val = gen}: the trig of step (0xFF: every step, SEQ CLEAR) was taken off; if the values locked on
 *   the core came from it, they go back to the model's base (a removed trig's lock must not stay on the core till the next
 *   trig). gen: the track's fire count (sm_gen) when the trig was taken off; a later fire (the step trigged again and
 *   fired before a refused unlock got through) makes it stale: ignored (8 bits: 256 fires of the track while the queue
 *   stays full, seconds; never in practice).
 * SMC_PREVIEW {trk, param, val}: a lock REC just wrote, onto the core at once (playing only; stopped: ignored); the
 *   parameter counts as locked, so the next unlocked trig of the track takes the base again. */
/* SMC_PARAM, SMC_LOAD, SMC_PREVIEW carry in `step` the pattern (smp.cur) whose model they come from: one of a pattern the audio
 *   stream no longer plays (it switched before the main loop saw it: sm_play_pat) is dropped, never a value or a sound of the
 *   old pattern onto the new one's tracks (the switch loaded the new kit whole) */
typedef struct { uint8_t kind, trk, step, param; int16_t val; } sm_cmd_t;

#define SM_NCMD 64u                                       /* the ring */
#define SM_NCOV 8u                                        /* values past the full ring (distinct {trk, param}) */
#define SM_BARRIER() __asm__ volatile("" ::: "memory")    /* (core.h RING_PUBLISH: slot store before the index update) */
#ifndef SM_CMD_PREEMPT
#define SM_CMD_PREEMPT() ((void)0)                        /* (the host test runs a drain here: the interrupt's moment) */
#endif

typedef struct {
    volatile uint8_t head, tail;                          /* head: the producer's, tail: the consumer's */
    uint8_t size;
    sm_cmd_t *slot;
} sm_ring_t;

static sm_cmd_t sm_cq_slot[SM_NCMD], sm_cov_slot[SM_NCOV];
static sm_ring_t sm_cq = {0, 0, SM_NCMD, sm_cq_slot}, sm_cov = {0, 0, SM_NCOV, sm_cov_slot};

static uint32_t sm_ring_count(const sm_ring_t *r) { return (uint8_t)(r->head - r->tail); }

/* producer: append, 0 = full */
static int sm_ring_put(sm_ring_t *r, sm_cmd_t c)
{
    uint8_t h = r->head;
    if ((uint8_t)(h - r->tail) >= r->size)
        return 0;
    r->slot[h & (r->size - 1u)] = c;
    SM_BARRIER();
    r->head = (uint8_t)(h + 1u);
    return 1;
}

/* producer: the newest pending SMC_PARAM of c's {trk, param} takes c's value. 1 = done (still pending after the store),
 * 0 = none pending, -1 = the consumer took that slot meanwhile (push again: there is room now) */
static int sm_ring_coalesce(sm_ring_t *r, sm_cmd_t c)
{
    uint8_t h = r->head, tl = r->tail, k = h;
    while (k != tl) {
        sm_cmd_t *e = &r->slot[--k & (r->size - 1u)];
        if (e->kind == SMC_PARAM && e->trk == c.trk && e->param == c.param && e->step == c.step) {
            SM_CMD_PREEMPT();
            e->val = c.val;
            SM_BARRIER();
            tl = r->tail;                                 /* (re-read: the drain may have run since) */
            return (uint8_t)(k - tl) < (uint8_t)(h - tl) ? 1 : -1;
        }
    }
    return 0;
}

/* consumer: the oldest command, 0 = empty */
static int sm_ring_get(sm_ring_t *r, sm_cmd_t *c)
{
    uint8_t t = r->tail;
    if (t == r->head)
        return 0;
    SM_BARRIER();
    *c = r->slot[t & (r->size - 1u)];
    SM_BARRIER();
    r->tail = (uint8_t)(t + 1u);
    return 1;
}

/* the main loop: 1 = queued (or coalesced), 0 = refused (full). A refused SMC_PARAM: its value is in the model already
 * (the caller wrote it there first), the caller pushes it again later (its next push of that parameter, or its next
 * poll); a refused PLAY / STOP / LOAD is not kept anywhere: the caller retries it on its next poll. Loops again only if
 * the drain ran during this push (at most once a block, and a push is far shorter than a block) */
static int sm_cmd_push(sm_cmd_t c)
{
    for (;;) {
        int r;
        if (!sm_ring_count(&sm_cov)) {
            if (sm_ring_put(&sm_cq, c))
                return 1;
            if (c.kind != SMC_PARAM)
                return 0;
            r = sm_ring_coalesce(&sm_cq, c);
            if (r > 0)
                return 1;
            if (r < 0)
                continue;
        } else if (c.kind != SMC_PARAM) {
            return 0;                                     /* (it would overtake the values waiting in sm_cov) */
        }
        r = sm_ring_coalesce(&sm_cov, c);
        if (r > 0)
            return 1;
        if (r < 0)
            continue;
        return sm_ring_put(&sm_cov, c);
    }
}

static void sm_seq_play(void);
static void sm_seq_stop(void);
static void sm_lock_clear_step(uint32_t t, uint32_t s);  /* sm_lock.c */

/* the sequencer's (the audio stream's) own: bit per parameter, the parameters the last trig fired on each track locked
 * (sm_seq.c sm_seq_locks). While a bit is set, trk[t].p holds the lock's value, not the model's base */
#define SM_LKB ((P_COUNT + 7u) / 8u)
static uint8_t sm_lk_on[SM_NTRK][SM_LKB];
static uint8_t sm_lk_step[SM_NTRK];                       /* the step whose trig set sm_lk_on (0xFF: none) */
static uint8_t sm_gen[SM_NTRK];                           /* the track's trig fires (sm_seq_fire bumps it; the main loop reads) */
static volatile uint8_t sm_playing;                       /* the transport (sm_seq.c writes it) */
static volatile uint8_t sm_play_pat;                      /* the pattern playing: the audio stream's own (sm_seq.c sm_pat_switch);
                                                             everything the audio stream reads of the model is of this one */
static void sm_seq_unlock(uint32_t t);                    /* sm_seq.c: the locked values of track t back to the base */
static void sm_ps_stop(uint32_t t);                       /* sm_seq.c: the PSLIDE of track t ends where it is */
static void sm_seq_pset(uint32_t t, uint32_t p, int32_t v, uint32_t st, int lock);
static int sm_lk_is(uint32_t t, uint32_t p) { return p < P_COUNT && ((sm_lk_on[t % SM_NTRK][p >> 3] >> (p & 7u)) & 1u); }

/* one command onto the core (the audio stream's side; the model is the main loop's and was written before the push).
 * A value is clamped to its parameter's range (P_E0..P_E8: the range of the engine the track is on its way to) */
static void sm_cmd_apply(const sm_cmd_t *c)
{
    uint32_t t = c->trk;
    const sm_pattern_t *pt = &smp.pat[sm_play_pat % SM_NPAT];
    if ((c->kind == SMC_PARAM || c->kind == SMC_LOAD || c->kind == SMC_PREVIEW) && c->step != sm_play_pat)
        return;                                           /* (of a pattern switched away from) */
    switch (c->kind) {
    case SMC_PLAY:
        sm_seq_play();
        break;
    case SMC_STOP:
        sm_seq_stop();
        break;
    case SMC_PARAM:
        if (t < SM_NTRK) {
            const param_desc_t *d = sm_desc_safe(eng_idx(trk[t].eng_req), c->param);
            if (d && !sm_lk_is(t, c->param))              /* (none: P_E8 of a machine without one, an id out of range; */
                trk[t].p[c->param] = (int16_t)clamp(c->val, d->min, d->max);   /* locked now: the next trig takes the base) */
        }
        break;
    case SMC_UNLOCK:
        if (t < SM_NTRK && (uint8_t)c->val == sm_gen[t] && (c->step == 0xFFu || c->step == sm_lk_step[t]))
            sm_seq_unlock(t);
        break;
    case SMC_PREVIEW:
        if (t < SM_NTRK && c->param < P_COUNT && sm_playing) {
            sm_seq_pset(t, c->param, c->val, 0, 0);
            sm_lk_on[t][c->param >> 3] |= (uint8_t)(1u << (c->param & 7u));
        }
        break;
    case SMC_LOAD:
        if (t < SM_NTRK) {
            sm_track_load(t, &pt->kit.snd[t]);
            memset(sm_lk_on[t], 0, SM_LKB);               /* (every value the sound's base now) */
            sm_ps_stop(t);                                /* (no glide over the sound just loaded) */
            sm_lk_step[t] = 0xFFu;
        }
        break;
    }
}

/* the audio stream, at the start of events_block: the ring, then the values that waited past it */
static void sm_cmd_drain(void)
{
    sm_cmd_t c;
    while (sm_ring_get(&sm_cq, &c))
        sm_cmd_apply(&c);
    while (sm_ring_get(&sm_cov, &c))
        sm_cmd_apply(&c);
}

/* the main loop: SMC_UNLOCK for track t (step st, 0xFF every step), with the track's fire count read now (after the trig
 * was taken off: no fire of it can come after this read). Refused (the queue full): kept, sm_unlock_flush pushes it again
 * on every poll; one per track: a second request of the same count widens to every step, of a later count replaces it
 * (the older one is stale: a fire since has replaced the core's locks) */
static uint8_t sm_unl_pend, sm_unl_step[SM_NTRK], sm_unl_gen[SM_NTRK];
static void sm_unlock_req(uint32_t t, uint32_t st)
{
    uint8_t g = *(volatile uint8_t *)&sm_gen[t];
    sm_cmd_t c = {SMC_UNLOCK, (uint8_t)t, (uint8_t)st, 0, 0};
    c.val = g;
    if ((sm_unl_pend >> t) & 1u) {
        sm_unl_step[t] = sm_unl_gen[t] != g || sm_unl_step[t] == st ? (uint8_t)st : 0xFFu;
        sm_unl_gen[t] = g;
        return;
    }
    if (!sm_cmd_push(c)) {
        sm_unl_pend |= (uint8_t)(1u << t);
        sm_unl_step[t] = (uint8_t)st;
        sm_unl_gen[t] = g;
    }
}
static void sm_unlock_flush(void)
{
    uint32_t t;
    for (t = 0; t < SM_NTRK; t++)
        if ((sm_unl_pend >> t) & 1u) {
            sm_cmd_t c = {SMC_UNLOCK, (uint8_t)t, sm_unl_step[t], 0, 0};
            c.val = sm_unl_gen[t];
            if (sm_cmd_push(c))
                sm_unl_pend &= (uint8_t)~(1u << t);
        }
}

/* the main loop: a fresh trig's fields on step st of track t (not n): C3 + the track's ROOT, vel 100, LEN TRK (the
 * track's GATE), 100 %, the flags given, no nudge */
static void sm_step_fresh(uint32_t t, uint32_t st, uint32_t flags)
{
    sm_pattern_t *pt = &smp.pat[smp.cur % SM_NPAT];
    sm_step_t *s = &pt->step[t][st];
    s->note[0] = (uint8_t)(48 + clamp(pt->kit.snd[t].p[P_ROOT], 0, 11));
    s->note[1] = s->note[2] = s->note[3] = 0;
    s->vel = 100;
    s->len = 0;
    s->cond = 0;
    s->flags = (uint8_t)flags;
    s->nudge = 0;
}

/* the main loop: toggle the trig of step st on track t in the current pattern. On: a fresh trig (sm_step_fresh, no
 * flags); every field first, then a compiler barrier, then n (one byte store): the audio stream (sm_seq_fire reads n
 * first) sees either no trig or the whole one. Off: n = 0 (one byte), then the flags cleared (an empty step shows no
 * REST). Either way the step's locks go (after n = 0 when off: the step no longer fires while they are removed; before
 * n when on: a trig placed again starts with none) */
static void sm_step_toggle(uint32_t t, uint32_t st)
{
    sm_pattern_t *pt = &smp.pat[smp.cur % SM_NPAT];
    sm_step_t *s;
    if (t >= SM_NTRK || st >= SM_NSTEP)
        return;
    s = &pt->step[t][st];
    if (s->n) {
        s->n = 0;
        SM_BARRIER();
        s->flags = 0;
        sm_lock_clear_step(t, st);
        sm_unlock_req(t, st);                             /* (its lock may be on the core: back to the base) */
        return;
    }
    sm_lock_clear_step(t, st);
    sm_step_fresh(t, st, 0);
    SM_BARRIER();
    s->n = 1;
}
