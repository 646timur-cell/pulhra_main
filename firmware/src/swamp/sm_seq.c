/* SPDX-License-Identifier: GPL-3.0-only */
/* SWAMP MACHINE sequencer, stage 1: the audio stream's side of a block (fx.c mix_block calls events_block first). The
 * only writer of trk[]. One clock of 1/16 steps for the four tracks, each track wrapping at its own length (the kit
 * sound's len, 1..32); a trig plays its notes at the start of its step, they end after the trig's LEN.
 *
 * Timing: a block (n = CTL samples) is the unit; a step whose start falls inside a block plays at the block's start
 * (0 .. CTL-1 samples early). The step is FS x 60 / (BPM x 4) = FS x 150 / bpm10 samples, kept in q8 (1/256 sample:
 * <= 1693440000 at bpm10 300, fits): the next step's start is a q8 count from this block's start, added to, never
 * restarted, so steps do not drift; the q8 floor loses < 1/256 sample a step (< 4 samples in 64 bars). */
static volatile uint8_t sm_playing;
static volatile uint8_t sm_solo;                          /* bit per track soloed (the GLO layer's held keys; the main loop writes) */
static uint32_t sm_next_q8;                               /* the next step starts this many q8 samples after this block's start */
static uint8_t sm_pos[SM_NTRK], sm_next[SM_NTRK];         /* the step playing, the step to play next */
static uint8_t sm_held[SM_NTRK][4], sm_nheld[SM_NTRK];    /* the notes the sequencer holds, per track */
static int32_t sm_gate[SM_NTRK];                          /* samples left of their LEN (SM_GATE_INF: to the next trig) */
#define SM_GATE_INF 0x7FFFFFFF
#define SM_LEN_INF 255u                                   /* a step's len: 0 the track's GATE, 1..254 in 1/8 steps, INF */

/* the tracks that do not start notes, bit per track: smp.mute (the GLO layer's toggles), or, while a solo key is held, every
 * track but the soloed. The main loop writes both bytes; events_block reads this once per block, so a block never plays
 * half of a change. A note already sounding is not cut: at the next block it is let go (note-off: its own RELEASE rings out) */
static uint32_t sm_mute_eff(void)
{
    uint32_t solo = sm_solo, m = smp.mute;
    return (solo ? ~solo : m) & ((1u << SM_NTRK) - 1u);
}

/* the step playing on track t (the screen's playhead) */
static uint32_t sm_seq_pos(uint32_t t) { return sm_pos[t % SM_NTRK]; }

static uint32_t sm_step_q8(void)
{
    uint32_t b = (uint32_t)clamp(smp.bpm10, 300, 3000);   /* 30.0 .. 300.0 BPM */
    return (uint32_t)FS * 150u * 256u / b;
}

static void sm_seq_release(uint32_t t)
{
    uint32_t i;
    for (i = 0; i < sm_nheld[t]; i++)
        trk_note_off(&trk[t], sm_held[t][i]);
    sm_nheld[t] = 0;
}

#ifndef SM_LOCK_APPLIED
#define SM_LOCK_APPLIED(t, s, p, v) ((void)0)             /* (the host test checks every lock applied against its model) */
#endif

/* a value onto track t's core, clamped to the parameter's range on the engine the track is on its way to (as SMC_PARAM) */
static void sm_seq_pset(uint32_t t, uint32_t p, int32_t v, uint32_t st, int lock)
{
    const param_desc_t *d = sm_desc_safe(eng_idx(trk[t].eng_req), p);
    (void)st;
    if (!d)
        return;
    v = clamp(v, d->min, d->max);
    trk[t].p[p] = (int16_t)v;
    if (lock)
        SM_LOCK_APPLIED(t, st, p, v);
}

/* track t's trig on step st fires: its locks onto the core; what the trig before locked and this one does not goes back
 * to the model's base. The pool is the main loop's (sm_lock.c): nlock read once, the entries below it are whole (an
 * entry is published before nlock counts it, a reused slot before its param byte stops being 0xFF); 0xFF: a removed
 * one, skipped. This interrupt is never preempted by the main loop, so an entry is read whole */
static void sm_seq_locks(uint32_t t, uint32_t st)
{
    const sm_pattern_t *pt = &smp.pat[smp.cur % SM_NPAT];
    uint8_t now[SM_LKB];
    uint32_t i, n = pt->nlock, ts = (t << 5) | st, b;
    memset(now, 0, sizeof now);
    if (n > SM_NLOCK)
        n = SM_NLOCK;
    SM_BARRIER();
    for (i = 0; i < n; i++) {
        const sm_lock_t *l = &pt->lock[i];
        uint32_t p = l->param;
        if (l->ts != ts || p >= P_COUNT)                  /* (0xFF >= P_COUNT: a removed entry) */
            continue;
        now[p >> 3] |= (uint8_t)(1u << (p & 7u));
        sm_seq_pset(t, p, l->val, st, 1);
    }
    for (b = 0; b < SM_LKB; b++) {
        uint32_t back = sm_lk_on[t][b] & ~now[b], k;
        for (k = 0; back; k++, back >>= 1)
            if (back & 1u)
                sm_seq_pset(t, b * 8u + k, pt->kit.snd[t].p[b * 8u + k], st, 0);
        sm_lk_on[t][b] = now[b];
    }
}

/* every track's locked values back to the base (STOP) */
static void sm_seq_unlock_all(void)
{
    const sm_pattern_t *pt = &smp.pat[smp.cur % SM_NPAT];
    uint32_t t, p;
    for (t = 0; t < SM_NTRK; t++) {
        for (p = 0; p < P_COUNT; p++)
            if (sm_lk_is(t, p))
                sm_seq_pset(t, p, pt->kit.snd[t].p[p], 0, 0);
        memset(sm_lk_on[t], 0, SM_LKB);
    }
}

/* power-on (before the audio starts: nothing runs concurrently): stopped, at step 0, the queue empty */
static void sm_seq_init(void)
{
    sm_playing = 0;
    sm_next_q8 = 0;
    memset(sm_pos, 0, sizeof sm_pos);
    memset(sm_next, 0, sizeof sm_next);
    memset(sm_nheld, 0, sizeof sm_nheld);
    memset(sm_lk_on, 0, sizeof sm_lk_on);
    sm_solo = 0;
    sm_cq.head = sm_cq.tail = 0;
    sm_cov.head = sm_cov.tail = 0;
}

/* SMC_PLAY: from step 0 of every track, in this block (playing already: nothing) */
static void sm_seq_play(void)
{
    if (sm_playing)
        return;
    sm_next_q8 = 0;
    memset(sm_pos, 0, sizeof sm_pos);
    memset(sm_next, 0, sizeof sm_next);
    sm_playing = 1;
}

/* SMC_STOP: every note of every track let go within this block, LEN INF too (the core's one-block fade, voice_kill:
 * a long RELEASE does not ring on), and the position back to 0 */
static void sm_seq_stop(void)
{
    uint32_t t, i;
    for (t = 0; t < SM_NTRK; t++) {
        trk_all_off(&trk[t]);
        for (i = 0; i < NVOICE; i++)
            if (trk[t].v[i].active)
                voice_kill(&trk[t].v[i]);
        sm_nheld[t] = 0;
    }
    sm_seq_unlock_all();
    memset(sm_pos, 0, sizeof sm_pos);
    memset(sm_next, 0, sizeof sm_next);
    sm_playing = 0;
}

/* the start of track t's next step: its trig (n > 0, not REST) lets go of the notes before and plays its own */
static void sm_seq_step(uint32_t t, uint32_t step_q8, uint32_t muted)
{
    const sm_pattern_t *pt = &smp.pat[smp.cur % SM_NPAT];
    uint32_t len = (uint32_t)clamp(pt->kit.snd[t].len, 1, SM_NSTEP), i = sm_next[t], k;
    const sm_step_t *s;
    if (i >= len)                                         /* (the length got shorter) */
        i = 0;
    sm_pos[t] = (uint8_t)i;
    sm_next[t] = (uint8_t)(i + 1u < len ? i + 1u : 0u);
    s = &pt->step[t][i];
    if (!s->n || (s->flags & SMF_REST) || muted)          /* (n first: the main loop writes it last, sm_step_toggle) */
        return;                                           /* (no trig, or a muted track: the notes before run out their LEN) */
    sm_seq_release(t);
    sm_seq_locks(t, i);                                   /* (before the notes: they start with the step's values) */
    for (k = 0; k < s->n && k < 4u; k++) {
        sm_held[t][k] = s->note[k];
        trk_note_on(&trk[t], s->note[k], (uint32_t)clamp(s->vel, 1, 127));
    }
    sm_nheld[t] = (uint8_t)k;
    if (s->len == SM_LEN_INF)
        sm_gate[t] = SM_GATE_INF;
    else if (s->len)
        sm_gate[t] = (int32_t)(step_q8 / 8u * s->len >> 8);
    else
        sm_gate[t] = (int32_t)((step_q8 >> 8) * (uint32_t)clamp(trk[t].p[P_SGATE], 1, 127) / 128u);
}

/* the audio block's events: the commands first (before engine_block: a machine change applied after it would render the
 * old engine with the new engine's values in this block, voice.c track_render), then panic_req, the engines' switches,
 * and the steps */
static void events_block(uint32_t n)
{
    uint32_t t, pr;
    sm_cmd_drain();
    pr = panic_req;                                       /* (the main loop sets bits; this interrupt is never preempted */
    panic_req = 0;                                        /* by it between the read and the clear) */
    for (t = 0; t < SM_NTRK; t++) {
        if ((pr >> t) & 1u) {
            trk_all_off(&trk[t]);
            sm_nheld[t] = 0;
        }
        engine_block(&trk[t]);                            /* engine switch: fade, then switch (voice.c) */
    }
    if (!sm_playing)
        return;
    {
        uint32_t step_q8 = sm_step_q8(), nq8 = n << 8, mute = sm_mute_eff();   /* (the mask: once per block) */
        for (t = 0; t < SM_NTRK; t++)                     /* LEN run out at this block's start; a muted track lets go (a LEN INF */
            if (sm_nheld[t] && (((mute >> t) & 1u) || (sm_gate[t] != SM_GATE_INF && sm_gate[t] <= 0)))   /* note would ring on) */
                sm_seq_release(t);
        while (sm_next_q8 < nq8) {
            for (t = 0; t < SM_NTRK; t++)
                sm_seq_step(t, step_q8, (mute >> t) & 1u);
            sm_next_q8 += step_q8;
        }
        sm_next_q8 -= nq8;
        for (t = 0; t < SM_NTRK; t++)
            if (sm_nheld[t] && sm_gate[t] != SM_GATE_INF)
                sm_gate[t] -= (int32_t)n;
    }
}
