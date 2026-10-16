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
enum { SMC_PLAY, SMC_STOP, SMC_PARAM, SMC_LOAD };
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
        if (e->kind == SMC_PARAM && e->trk == c.trk && e->param == c.param) {
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

/* one command onto the core (the audio stream's side; the model is the main loop's and was written before the push).
 * A value is clamped to its parameter's range (P_E0..P_E8: the range of the engine the track is on its way to) */
static void sm_cmd_apply(const sm_cmd_t *c)
{
    uint32_t t = c->trk;
    const sm_pattern_t *pt = &smp.pat[smp.cur % SM_NPAT];
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
            if (d)                                        /* (none: P_E8 of a machine without one, an id out of range) */
                trk[t].p[c->param] = (int16_t)clamp(c->val, d->min, d->max);
        }
        break;
    case SMC_LOAD:
        if (t < SM_NTRK)
            sm_track_load(t, &pt->kit.snd[t]);
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

/* the main loop: toggle the trig of step st on track t in the current pattern. On: C3 + the track's ROOT, vel 100, LEN 0
 * (the track's GATE), no flags; every field first, then a compiler barrier, then n (one byte store): the audio stream
 * (sm_seq_step reads n first) sees either no trig or the whole one. Off: n = 0 (one byte). */
static void sm_step_toggle(uint32_t t, uint32_t st)
{
    sm_pattern_t *pt = &smp.pat[smp.cur % SM_NPAT];
    sm_step_t *s;
    if (t >= SM_NTRK || st >= SM_NSTEP)
        return;
    s = &pt->step[t][st];
    if (s->n) {
        s->n = 0;
        return;
    }
    s->note[0] = (uint8_t)(48 + clamp(pt->kit.snd[t].p[P_ROOT], 0, 11));
    s->note[1] = s->note[2] = s->note[3] = 0;
    s->vel = 100;
    s->len = 0;
    s->cond = 0;
    s->flags = 0;
    s->nudge = 0;
    SM_BARRIER();
    s->n = 1;
}
