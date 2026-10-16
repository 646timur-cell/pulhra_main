/* SPDX-License-Identifier: GPL-3.0-only */
/* SWAMP MACHINE sequencer: the audio stream's side of a block (fx.c mix_block calls events_block first). The only writer
 * of trk[]. One clock of 1/16 steps for the four tracks, each track wrapping at its own length (the kit sound's len,
 * 1..32). A trig FIRES (sm_seq_fire) on its step's start, or NUDGE x 1/16 step off it (-8..+7: an early one fires in the
 * step before, and its own start then plays nothing): its COND first (a chance from this file's LFSR, or a condition: the
 * track's passes, the track's last result PRE, FILL; a failed one fires nothing, no locks), then its locks, then its notes
 * (none for a REST trig: its locks only, a note held keeps sounding) at VEL (ACCENT: 127). They end after the trig's LEN
 * (MONOFM's list; INF: at the next trig); a SLIDE trig holds them to the next trig, which then plays legato with the
 * glide (trk.slide_glide, voice.c) and lets go of the old notes after its own are on, or, if no note comes on the next
 * step, they end there. A ratchet x2..x4 plays the step's notes that many times in equal parts of the step (each part
 * the track's GATE of the part; no NUDGE, no LEN, no slide out). PSLIDE: the values the trig locks (and those the next
 * trig locks) move in straight lines to the next trig's (its locks, else the base) over the step, in 1/32 steps.
 *
 * Timing: a block (n = CTL samples) is the unit; an event (a step's start, a nudged fire, a ratchet part) whose time
 * falls inside a block plays at the block's start (0 .. CTL-1 samples early). The step is FS x 60 / (BPM x 4) =
 * FS x 150 / bpm10 samples, kept in q8 (1/256 sample): every pending time is a q8 count from this block's start, added
 * to, never restarted, so steps do not drift; the q8 floor loses < 1/256 sample a step (< 4 samples in 64 bars). The
 * product FS x 150 x 256 = 1693440000 fits 32 bits; a step is at most 5644800 q8 (bpm10 300, 30 BPM), so a pending time
 * (< 2 steps) and PSLIDE's elapsed x 32 (< 2^28) fit too. */
/* (sm_playing: sm_cmd.c defines it, volatile uint8_t: the transport; this file's alone to write) */
static volatile uint8_t sm_solo;                          /* bit per track soloed (the GLO layer's held keys; the main loop writes) */
static volatile uint8_t sm_fill;                          /* FILL held (Task 6: GLO + G5); COND FILL / !FILL read it */
static uint32_t sm_next_q8;                               /* the next step starts this many q8 samples after this block's start */
static uint8_t sm_pos[SM_NTRK], sm_next[SM_NTRK];         /* the step playing, the step to play next */
static uint8_t sm_held[SM_NTRK][4], sm_nheld[SM_NTRK];    /* the notes the sequencer holds, per track */
static int32_t sm_gate[SM_NTRK];                          /* samples left of their LEN (SM_GATE_INF: to the next trig) */
#define SM_GATE_INF 0x7FFFFFFF
#define SM_Q8_NONE 0xFFFFFFFFu                            /* no event pending */
static uint32_t sm_lfsr = 0xACE1u;                        /* COND's chance (32-bit Galois LFSR; the test seeds it) */
static uint16_t sm_loop[SM_NTRK];                         /* the track's passes since PLAY, 0 the first */
static uint8_t sm_pre[SM_NTRK];                           /* the result of the track's last condition (PRE) */
static uint32_t sm_late_q8[SM_NTRK], sm_early_q8[SM_NTRK];   /* a nudged fire pending: late (this step's), early (the next's) */
static uint8_t sm_late_st[SM_NTRK], sm_early_st[SM_NTRK], sm_done[SM_NTRK];   /* their steps; the step fired early */
static uint32_t sm_rat_q8[SM_NTRK], sm_rat_d[SM_NTRK];    /* the next ratchet part, the length of a part (q8) */
static uint8_t sm_rat_left[SM_NTRK], sm_rat_vel[SM_NTRK];
static uint8_t sm_slid[SM_NTRK];                          /* the notes held come from a SLIDE trig (held to the next trig) */
#define SM_NPS 8u                                         /* PSLIDE: parameters gliding per track */
static struct {
    uint8_t n, p[SM_NPS];
    int16_t a[SM_NPS], b[SM_NPS];                         /* from, to */
    int32_t at;                                           /* the fire, in q8 from this block's start (<= 0 once past) */
    uint32_t len;                                         /* the step (q8) */
} sm_ps[SM_NTRK];

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
    sm_lk_step[t] = (uint8_t)st;
}

/* track t's locked values back to the model's base, the mask cleared (SMC_UNLOCK, STOP) */
static void sm_seq_unlock(uint32_t t)
{
    const sm_pattern_t *pt = &smp.pat[smp.cur % SM_NPAT];
    uint32_t p;
    t %= SM_NTRK;
    for (p = 0; p < P_COUNT; p++)
        if (sm_lk_is(t, p))
            sm_seq_pset(t, p, pt->kit.snd[t].p[p], 0, 0);
    memset(sm_lk_on[t], 0, SM_LKB);
    sm_lk_step[t] = 0xFFu;
    sm_ps[t].n = 0;                                       /* (a glide must not write over the base again) */
}
/* PSLIDE of track t stopped where it is (SMC_LOAD: the sound's values stay the sound's) */
static void sm_ps_stop(uint32_t t) { sm_ps[t % SM_NTRK].n = 0; }
static void sm_seq_unlock_all(void)
{
    uint32_t t;
    for (t = 0; t < SM_NTRK; t++)
        sm_seq_unlock(t);
}

/* the sequencer's own state of track t back to rest: nothing pending, no ratchet, no slide, no PSLIDE */
static void sm_seq_idle(uint32_t t)
{
    sm_late_q8[t] = sm_early_q8[t] = sm_rat_q8[t] = SM_Q8_NONE;
    sm_done[t] = 0xFFu;
    sm_rat_left[t] = 0;
    sm_slid[t] = 0;
    sm_ps[t].n = 0;
    trk[t].slide_glide = 0;
}

/* power-on (before the audio starts: nothing runs concurrently): stopped, at step 0, the queue empty */
static void sm_seq_init(void)
{
    uint32_t t;
    sm_playing = 0;
    sm_fill = 0;
    sm_next_q8 = 0;
    memset(sm_pos, 0, sizeof sm_pos);
    memset(sm_next, 0, sizeof sm_next);
    memset(sm_nheld, 0, sizeof sm_nheld);
    memset(sm_lk_on, 0, sizeof sm_lk_on);
    memset(sm_lk_step, 0xFF, sizeof sm_lk_step);
    memset(sm_gen, 0, sizeof sm_gen);
    memset(sm_pre, 0, sizeof sm_pre);
    for (t = 0; t < SM_NTRK; t++)
        sm_seq_idle(t);
    sm_solo = 0;
    sm_cq.head = sm_cq.tail = 0;
    sm_cov.head = sm_cov.tail = 0;
}

/* SMC_PLAY: from step 0 of every track, in this block (playing already: nothing); the passes from 0 */
static void sm_seq_play(void)
{
    uint32_t t;
    if (sm_playing)
        return;
    sm_next_q8 = 0;
    memset(sm_pos, 0, sizeof sm_pos);
    memset(sm_next, 0, sizeof sm_next);
    memset(sm_pre, 0, sizeof sm_pre);
    for (t = 0; t < SM_NTRK; t++) {
        sm_loop[t] = 0xFFFFu;                             /* (step 0's start counts it to 0) */
        sm_seq_idle(t);
    }
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
        sm_seq_idle(t);
    }
    sm_seq_unlock_all();
    memset(sm_pos, 0, sizeof sm_pos);
    memset(sm_next, 0, sizeof sm_next);
    sm_playing = 0;
}

/* COND: a chance rolls on the LFSR; a condition is checked. PRE / !PRE read the track's last result (and keep it), the
 * others set it; 100 % (no condition) keeps it */
static uint32_t sm_rand(void)
{
    uint32_t x = sm_lfsr ? sm_lfsr : 1u;
    x = (x >> 1) ^ (-(x & 1u) & 0xA3000000u);              /* (mask bits 31 29 25 24: x^32 + x^30 + x^26 + x^25 + 1, */
    sm_lfsr = x;                                          /* period 2^32 - 1, checked by brute force) */
    return x;
}
static uint32_t sm_cond_ab_of(uint32_t c, uint32_t *b)    /* condition c (>= SM_COND_AB0): its A, *b its B */
{
    uint32_t i = c - SM_COND_AB0, bb = 2u;
    while (i >= bb) {
        i -= bb;
        bb++;
    }
    *b = bb;
    return i + 1u;
}
static int sm_cond_ok(uint32_t t, uint32_t c, uint32_t next_pass)
{
    uint32_t r, b, a, loop = (sm_loop[t] == 0xFFFFu ? 0u : sm_loop[t]) + next_pass;
    if (c < SM_COND_1ST) {
        if (!c)
            return 1;
        r = (sm_rand() >> 8) % 100u < c;
    } else if (c == SM_COND_PRE) {
        return sm_pre[t];
    } else if (c == SM_COND_NPRE) {
        return !sm_pre[t];
    } else if (c == SM_COND_1ST || c == SM_COND_N1ST) {
        r = (loop == 0u) == (c == SM_COND_1ST);
    } else if (c == SM_COND_FILL || c == SM_COND_NFILL) {
        r = (sm_fill != 0u) == (c == SM_COND_FILL);
    } else if (c < SM_COND_END) {
        a = sm_cond_ab_of(c, &b);
        r = loop % b == a - 1u;
    } else {
        return 1;                                         /* (a byte from nowhere: plays) */
    }
    sm_pre[t] = (uint8_t)r;
    return (int)r;
}

static int32_t sm_lock_find(const sm_pattern_t *p, uint32_t t, uint32_t s, uint32_t param);   /* sm_lock.c */

/* PSLIDE (the trig of step st just fired at `at`, its locks on the core): the values it locks, and those the next trig of
 * the track locks, from what they are now to the next trig's (its lock, else the base). The next trig: the first step
 * after st, wrapping at the track's length, with n > 0 (st itself if no other) */
static void sm_ps_begin(uint32_t t, uint32_t st, int32_t at, uint32_t step_q8)
{
    const sm_pattern_t *pt = &smp.pat[smp.cur % SM_NPAT];
    uint32_t len = (uint32_t)clamp(pt->kit.snd[t].len, 1, SM_NSTEP), j = st, k, i, n = pt->nlock, pass, m = 0;
    for (k = 1; k <= len; k++) {
        j = (st + k) % len;
        if (pt->step[t][j].n)
            break;
    }
    if (n > SM_NLOCK)
        n = SM_NLOCK;
    SM_BARRIER();
    for (pass = 0; pass < 2u; pass++)                     /* this trig's locks, then the next trig's not among them */
        for (i = 0; i < n && m < SM_NPS; i++) {
            const sm_lock_t *l = &pt->lock[i];
            uint32_t p = l->param, q;
            int32_t to;
            if (p >= P_COUNT || l->ts != ((t << 5) | (pass ? j : st)))
                continue;
            for (q = 0; q < m && sm_ps[t].p[q] != p; q++) {}
            if (q < m)
                continue;
            to = pass ? l->val : pt->kit.snd[t].p[p];
            if (!pass) {
                int32_t jl = sm_lock_find(pt, t, j, p);
                if (jl >= 0)
                    to = pt->lock[jl].val;
            }
            sm_ps[t].p[m] = (uint8_t)p;
            sm_ps[t].a[m] = trk[t].p[p];
            sm_ps[t].b[m] = (int16_t)to;
            sm_lk_on[t][p >> 3] |= (uint8_t)(1u << (p & 7u));   /* (gliding: a base turn waits for the next trig) */
            m++;
        }
    sm_ps[t].n = (uint8_t)m;
    sm_ps[t].at = at;
    sm_ps[t].len = step_q8 ? step_q8 : 1u;
}

/* PSLIDE, every block: the gliding values where they are at this block's middle, in 1/32 steps (rounded) */
static void sm_ps_block(uint32_t t, uint32_t nq8)
{
    int32_t e = (int32_t)(nq8 / 2u) - sm_ps[t].at;
    uint32_t k, i;
    if (!sm_ps[t].n || e < 0)
        return;
    k = ((uint32_t)e * 32u + sm_ps[t].len / 2u) / sm_ps[t].len;
    if (k > 32u)
        k = 32u;
    for (i = 0; i < sm_ps[t].n; i++)
        sm_seq_pset(t, sm_ps[t].p[i], sm_ps[t].a[i] + ((int32_t)sm_ps[t].b[i] - sm_ps[t].a[i]) * (int32_t)k / 32, 0, 0);
    if (k >= 32u)
        sm_ps[t].n = 0;                                   /* (there: the values stay till the next trig) */
}

/* the notes of step s onto track t at vel; slide: legato from the notes held (the glide on for these note-ons, the old
 * notes let go after the new are on), else the old ones go first */
static void sm_seq_notes(uint32_t t, const sm_step_t *s, uint32_t vel, uint32_t slide)
{
    uint8_t old[4];
    uint32_t nold = sm_nheld[t], k, j, n = s->n < 4u ? s->n : 4u;
    if (!slide)
        sm_seq_release(t);
    memcpy(old, sm_held[t], sizeof old);
    trk[t].slide_glide = (uint8_t)(slide != 0);
    for (k = 0; k < n; k++) {
        sm_held[t][k] = s->note[k];
        trk_note_on(&trk[t], s->note[k], vel);
    }
    trk[t].slide_glide = 0;                               /* (a live note after it must not glide) */
    if (slide)
        for (j = 0; j < nold; j++) {
            for (k = 0; k < n && s->note[k] != old[j]; k++) {}
            if (k == n)
                trk_note_off(&trk[t], old[j]);
        }
    sm_nheld[t] = (uint8_t)n;
}

/* track t's trig on step st fires at `at` (q8 from this block's start): COND, the locks, PSLIDE, then the notes (none for
 * a REST), their LEN, a ratchet's parts. sm_gen counts the fires (an SMC_UNLOCK carries the one it saw, sm_cmd.c).
 * next_pass: step 0 fired early, in the last step of the pass before: COND counts the pass it belongs to */
static void sm_seq_fire(uint32_t t, uint32_t st, uint32_t at, uint32_t step_q8, uint32_t next_pass)
{
    const sm_pattern_t *pt = &smp.pat[smp.cur % SM_NPAT];
    const sm_step_t *s = &pt->step[t][st];
    uint32_t hits, vel, f, li;
    if (!s->n)                                            /* (n first: the main loop writes it last, sm_step_toggle) */
        return;
    SM_BARRIER();
    f = s->flags;
    if (!sm_cond_ok(t, s->cond, next_pass)) {
        if (sm_slid[t])                                   /* (no note comes: the slide's notes end here) */
            sm_seq_release(t);
        sm_slid[t] = 0;
        return;
    }
    sm_gen[t]++;
    sm_rat_left[t] = 0;
    sm_rat_q8[t] = SM_Q8_NONE;
    sm_ps[t].n = 0;
    sm_seq_locks(t, st);                                  /* (before the notes: they start with the step's values) */
    if (f & SMF_PSLIDE)
        sm_ps_begin(t, st, (int32_t)at, step_q8);
    if (f & SMF_REST) {                                   /* a lock trig: no note; a note held sounds on (with the locks) */
        if (sm_slid[t])
            sm_seq_release(t);
        sm_slid[t] = 0;
        return;
    }
    hits = SMF_RAT(f) + 1u;
    vel = (f & SMF_ACCENT) ? 127u : (uint32_t)clamp(s->vel, 1, 127);
    sm_seq_notes(t, s, vel, sm_slid[t] && sm_nheld[t]);
    sm_slid[t] = 0;
    if (hits > 1u) {                                      /* RATCHET: the parts, the track's GATE of a part each */
        uint32_t part = step_q8 / hits;
        sm_gate[t] = (int32_t)((part >> 8) * (uint32_t)clamp(trk[t].p[P_SGATE], 1, 127) / 128u);
        sm_rat_left[t] = (uint8_t)(hits - 1u);
        sm_rat_vel[t] = (uint8_t)vel;
        sm_rat_d[t] = part;
        sm_rat_q8[t] = at + part;
        return;
    }
    if (f & SMF_SLIDE) {
        sm_slid[t] = 1;
        sm_gate[t] = SM_GATE_INF;
        return;
    }
    li = s->len < MF_NLEN ? s->len : 0u;
    if (li == MF_LEN_INF)
        sm_gate[t] = SM_GATE_INF;
    else if (li)
        sm_gate[t] = (int32_t)(step_q8 / 8u * MF_LEN8[li] >> 8);
    else
        sm_gate[t] = (int32_t)((step_q8 >> 8) * (uint32_t)clamp(trk[t].p[P_SGATE], 1, 127) / 128u);
}

/* a ratchet part of the step playing: its notes again (the step read again: taken off or a REST meanwhile ends them) */
static void sm_seq_part(uint32_t t)
{
    const sm_pattern_t *pt = &smp.pat[smp.cur % SM_NPAT];
    const sm_step_t *s = &pt->step[t][sm_pos[t]];
    if (!sm_rat_left[t] || !s->n || (s->flags & SMF_REST)) {
        sm_rat_left[t] = 0;
        sm_rat_q8[t] = SM_Q8_NONE;
        return;
    }
    sm_seq_notes(t, s, sm_rat_vel[t], 0);
    sm_gate[t] = (int32_t)((sm_rat_d[t] >> 8) * (uint32_t)clamp(trk[t].p[P_SGATE], 1, 127) / 128u);
    sm_rat_left[t]--;
    sm_rat_q8[t] = sm_rat_left[t] ? sm_rat_q8[t] + sm_rat_d[t] : SM_Q8_NONE;
}

/* a step's nudge as it plays: -8..+7 (1/16 step); a ratchet plays on its start */
static int32_t sm_nudge_of(const sm_step_t *s)
{
    return SMF_RAT(s->flags) ? 0 : clamp(s->nudge, -8, 7);
}

/* the start of track t's next step at `at` (q8 from this block's start): its trig fires now, or NUDGE later; a trig
 * nudged early on the step after is set to fire in this one. A muted track fires nothing (the notes before run out
 * their LEN) */
static void sm_seq_step(uint32_t t, uint32_t at, uint32_t step_q8, uint32_t muted)
{
    const sm_pattern_t *pt = &smp.pat[smp.cur % SM_NPAT];
    uint32_t len = (uint32_t)clamp(pt->kit.snd[t].len, 1, SM_NSTEP), i = sm_next[t], j;
    const sm_step_t *s;
    int32_t d;
    if (i >= len)                                         /* (the length got shorter) */
        i = 0;
    sm_pos[t] = (uint8_t)i;
    sm_next[t] = (uint8_t)(i + 1u < len ? i + 1u : 0u);
    if (!i)                                               /* a pass (0xFFFF at PLAY -> 0); past 0xFFFE it goes on at 855: */
        sm_loop[t] = (uint16_t)(sm_loop[t] == 0xFFFEu ? 840u + 0xFFFFu % 840u : sm_loop[t] + 1u);   /* (same A:B, not 1ST) */
    sm_late_q8[t] = SM_Q8_NONE;
    s = &pt->step[t][i];
    if (sm_done[t] == i) {                                /* fired early already */
        sm_done[t] = 0xFFu;
    } else {
        uint32_t trig = s->n && !(s->flags & SMF_REST) && !muted;
        sm_done[t] = 0xFFu;
        if (sm_slid[t] && !trig) {                        /* a slide with no note to go to: its notes end here */
            sm_seq_release(t);
            sm_slid[t] = 0;
        }
        if (s->n && !muted) {
            d = sm_nudge_of(s);
            if (d > 0) {
                sm_late_q8[t] = at + (uint32_t)d * step_q8 / 16u;
                sm_late_st[t] = (uint8_t)i;
            } else {
                sm_seq_fire(t, i, at, step_q8, 0);        /* (on time; early but not yet fired, PLAY's first step: now) */
            }
        }
    }
    j = sm_next[t];
    s = &pt->step[t][j];
    sm_early_q8[t] = SM_Q8_NONE;
    if (!muted && s->n && (d = sm_nudge_of(s)) < 0) {
        sm_early_q8[t] = at + step_q8 - (uint32_t)(-d) * step_q8 / 16u;
        sm_early_st[t] = (uint8_t)j;
    }
}

/* the audio block's events: the commands first (before engine_block: a machine change applied after it would render the
 * old engine with the new engine's values in this block, voice.c track_render), then panic_req, the engines' switches,
 * and the sequencer's events of the block in time order: the steps' starts, the nudged fires, the ratchet parts */
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
            sm_slid[t] = 0;
            sm_rat_left[t] = 0;
            sm_rat_q8[t] = SM_Q8_NONE;
        }
        engine_block(&trk[t]);                            /* engine switch: fade, then switch (voice.c) */
    }
    if (!sm_playing)
        return;
    {
        uint32_t step_q8 = sm_step_q8(), nq8 = n << 8, mute = sm_mute_eff();   /* (the mask: once per block) */
        for (t = 0; t < SM_NTRK; t++)                     /* LEN run out at this block's start; a muted track lets go (a LEN INF */
            {
            if (sm_nheld[t] && (((mute >> t) & 1u) || (sm_gate[t] != SM_GATE_INF && sm_gate[t] <= 0))) {   /* note would ring on) */
                sm_seq_release(t);
                sm_slid[t] = 0;
            }
            if ((mute >> t) & 1u) {                       /* (a ratchet's parts stop with the mute) */
                sm_rat_left[t] = 0;
                sm_rat_q8[t] = SM_Q8_NONE;
            }
        }
        for (;;) {                                        /* the earliest event before the block's end (a step's start first) */
            uint32_t best = sm_next_q8 < nq8 ? sm_next_q8 : nq8, kind = sm_next_q8 < nq8, bt = 0;
            for (t = 0; t < SM_NTRK; t++) {
                if (sm_late_q8[t] < best) { best = sm_late_q8[t]; kind = 2; bt = t; }
                if (sm_early_q8[t] < best) { best = sm_early_q8[t]; kind = 3; bt = t; }
                if (sm_rat_q8[t] < best) { best = sm_rat_q8[t]; kind = 4; bt = t; }
            }
            if (!kind)
                break;
            if (kind == 1u) {
                for (t = 0; t < SM_NTRK; t++)
                    sm_seq_step(t, best, step_q8, (mute >> t) & 1u);
                sm_next_q8 += step_q8;
            } else if (kind == 2u) {
                sm_late_q8[bt] = SM_Q8_NONE;
                if (!((mute >> bt) & 1u))
                    sm_seq_fire(bt, sm_late_st[bt], best, step_q8, 0);
            } else if (kind == 3u) {
                sm_early_q8[bt] = SM_Q8_NONE;
                if (!((mute >> bt) & 1u)) {
                    sm_done[bt] = sm_early_st[bt];        /* (its own start then plays nothing) */
                    sm_seq_fire(bt, sm_early_st[bt], best, step_q8, sm_early_st[bt] == 0u);
                }
            } else {
                if ((mute >> bt) & 1u) {
                    sm_rat_left[bt] = 0;
                    sm_rat_q8[bt] = SM_Q8_NONE;
                } else {
                    sm_seq_part(bt);
                }
            }
        }
        sm_next_q8 -= nq8;
        for (t = 0; t < SM_NTRK; t++) {
            sm_ps_block(t, nq8);
            if (sm_late_q8[t] != SM_Q8_NONE)
                sm_late_q8[t] -= nq8;
            if (sm_early_q8[t] != SM_Q8_NONE)
                sm_early_q8[t] -= nq8;
            if (sm_rat_q8[t] != SM_Q8_NONE)
                sm_rat_q8[t] -= nq8;
            if (sm_ps[t].n)
                sm_ps[t].at -= (int32_t)nq8;
            if (sm_nheld[t] && sm_gate[t] != SM_GATE_INF)
                sm_gate[t] -= (int32_t)n;
        }
    }
}
