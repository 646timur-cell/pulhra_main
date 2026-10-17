/* SPDX-License-Identifier: GPL-3.0-only */
/* SWAMP MACHINE edits of whole tracks and patterns: the SEQ layer's tools (sm_layer.c: CLEAR, COPY / PASTE a pattern or a
 * track, SHIFT, TRN) and UNDO (SAVE held, sm_input.c). The main loop's, as every write of the model.
 *  - the clipboard (sm_clip): a pattern (everything of it), or a track (its sound - the kit's, its length too - its steps and
 *    its locks, kept as track 0 of the clipboard). PASTE track takes the track copied, or, a pattern copied, its track of the
 *    same number; PASTE pattern needs a pattern copied;
 *  - UNDO: one snapshot (sm_undo_buf) of the current pattern, taken before each CLEAR, PASTE, SHIFT, TRN (CLEAR held on to all
 *    four tracks keeps the one its tap took); UNDO puts it back once; a pattern switch forgets it.
 * A track rewritten while the audio stream plays it (sm_put_begin, then sm_put_track): every step of the track first made
 * invisible (n = 0: the audio stream reads n first, sm_seq_fire / sm_seq_part / sm_ps_begin), its locks removed (tombstones,
 * one byte each; stopped: the pool compacted), SMC_UNLOCK pushed (a lock of it on the core goes back to the base); then the
 * new sound (the interrupt off for the copy, then the LOAD, tagged with the pattern, by sm_flush), the new locks (sm_lock_set:
 * each whole before it counts), the steps' fields (n still 0), SM_BARRIER(), and only then the n of each step. So whenever
 * the interrupt comes the track has either no trig at all or every trig with all its locks: no lock of a wiped step, none
 * half written. All the tracks of an edit are emptied before any is filled, so the pool never runs out midway on locks the
 * edit itself is removing. A block that comes in the middle plays the track with no trig (a trig due in those few hundred
 * microseconds is skipped: the edit is happening then). TRN writes the note bytes in place (a fire in between plays a chord
 * half moved: no harm). */
#define SM_CLEAR_ALL_US 800000u                           /* SEQ + F3 held this long: every track */
enum { SM_CLIP_NONE, SM_CLIP_TRK, SM_CLIP_PAT };

static sm_pattern_t sm_clip;                              /* COPY / PASTE */
static uint8_t sm_clip_kind;                              /* SM_CLIP_* */
static sm_pattern_t sm_undo_buf;                          /* UNDO: the pattern before the last edit */
static uint8_t sm_undo_ok, sm_undo_pat;                   /* ... there is one, of this pattern */

static void sm_edit_init(void)
{
    sm_clip_kind = SM_CLIP_NONE;
    sm_undo_ok = 0;
    sm_undo_pat = 0;
}

/* sm_input.c sm_pat_poll: another pattern plays now (the snapshot is of the one before) */
static void sm_edit_pat_changed(void) { sm_undo_ok = 0; }

static void sm_undo_snap(void)
{
    sm_undo_buf = smp.pat[smp.cur % SM_NPAT];
    sm_undo_pat = (uint8_t)(smp.cur % SM_NPAT);
    sm_undo_ok = 1;
}

/* the tracks of mask (bit per track) emptied in the current pattern for a rewrite: no trig (n first), no lock (tombstones),
 * the core's locks of them back to the base; stopped, the pool compacted */
static void sm_put_begin(uint32_t mask)
{
    sm_pattern_t *p = &smp.pat[smp.cur % SM_NPAT];
    uint32_t t, s, i, n;
    for (t = 0; t < SM_NTRK; t++)
        if ((mask >> t) & 1u)
            for (s = 0; s < SM_NSTEP; s++)
                p->step[t][s].n = 0;                      /* (one byte: the audio stream sees the trig or nothing) */
    SM_BARRIER();
    n = p->nlock > SM_NLOCK ? SM_NLOCK : p->nlock;
    for (i = 0; i < n; i++)
        if (p->lock[i].param != SM_LOCK_DEAD && ((mask >> (p->lock[i].ts >> 5)) & 1u)) {
            p->lock[i].param = SM_LOCK_DEAD;              /* (one byte) */
            SM_LOCK_PREEMPT();
        }
    SM_BARRIER();
    for (t = 0; t < SM_NTRK; t++)
        if ((mask >> t) & 1u)
            sm_unlock_req(t, 0xFFu);
    if (!sm_playing)
        sm_lock_compact(p);
}

/* step s of a track of length len moved by rot (-1, 0, +1) within the length; a step past the length stays */
static uint32_t sm_rot(uint32_t s, int32_t rot, uint32_t len)
{
    if (!rot || s >= len)
        return s;
    return (uint32_t)(((int32_t)s + rot + (int32_t)len) % (int32_t)len);
}

/* track t of the current pattern (emptied by sm_put_begin) filled from track st of src (0: left empty, a CLEAR), its steps
 * moved by rot within the track's length (SHIFT); snd: the sound too (a LOAD if it is another). The locks, then the steps'
 * fields, a barrier, then each n */
static void sm_put_track(uint32_t t, const sm_pattern_t *src, uint32_t st, int32_t rot, int snd)
{
    sm_pattern_t *p = &smp.pat[smp.cur % SM_NPAT];
    uint32_t s, i, n, len;
    t %= SM_NTRK;
    st %= SM_NTRK;
    if (src && snd && memcmp(&p->kit.snd[t], &src->kit.snd[st], sizeof p->kit.snd[t])) {
        fm1_irq_off();                                    /* (a LOAD still queued must not drain half a sound) */
        p->kit.snd[t] = src->kit.snd[st];
        fm1_irq_on();
        sm_ld_pend |= (uint8_t)(1u << t);                 /* (sm_flush pushes it, with the pattern) */
    }
    len = (uint32_t)clamp(p->kit.snd[t].len, 1, SM_NSTEP);
    if (src) {
        n = src->nlock > SM_NLOCK ? SM_NLOCK : src->nlock;
        for (i = 0; i < n; i++) {
            const sm_lock_t *l = &src->lock[i];
            if (l->param >= P_COUNT || (uint32_t)(l->ts >> 5) != st)
                continue;
            if (!sm_lock_set(t, sm_rot(l->ts & 31u, rot, len), l->param, l->val))
                break;                                    /* (the pool full: LOCKS FULL, the rest not placed) */
        }
    }
    for (s = 0; s < SM_NSTEP; s++) {
        sm_step_t x;
        if (src)
            x = src->step[st][sm_rot(s, -rot, len)];
        else
            memset(&x, 0, sizeof x);
        x.n = 0;
        p->step[t][s] = x;                                /* (n stays 0: not read yet) */
    }
    SM_BARRIER();
    if (src)
        for (s = 0; s < SM_NSTEP; s++) {
            uint8_t k = src->step[st][sm_rot(s, -rot, len)].n;
            if (k) {
                SM_LOCK_PREEMPT();
                p->step[t][s].n = k;
            }
        }
    sm_dirty = 1;
}

/* CLEAR: the trigs and locks of the tracks of mask (the kit stays); snap: take the UNDO snapshot first */
static void sm_edit_clear(uint32_t mask, int snap)
{
    uint32_t t;
    if (snap)
        sm_undo_snap();
    sm_put_begin(mask);
    for (t = 0; t < SM_NTRK; t++)
        if ((mask >> t) & 1u)
            sm_put_track(t, 0, 0, 0, 0);
    sm_msg_show(SM_MSG_CLEARED);
}

/* COPY: the pattern; a track (as track 0 of the clipboard: its sound, steps, live locks) */
static void sm_copy_pat(void)
{
    sm_clip = smp.pat[smp.cur % SM_NPAT];
    sm_clip_kind = SM_CLIP_PAT;
}
static void sm_copy_trk(uint32_t t)
{
    const sm_pattern_t *p = &smp.pat[smp.cur % SM_NPAT];
    uint32_t i, k = 0, n = p->nlock > SM_NLOCK ? SM_NLOCK : p->nlock;
    t %= SM_NTRK;
    memset(&sm_clip, 0, sizeof sm_clip);
    sm_clip.kit.snd[0] = p->kit.snd[t];
    memcpy(sm_clip.step[0], p->step[t], sizeof sm_clip.step[0]);
    for (i = 0; i < n; i++)
        if (p->lock[i].param != SM_LOCK_DEAD && (uint32_t)(p->lock[i].ts >> 5) == t) {
            sm_clip.lock[k] = p->lock[i];
            sm_clip.lock[k++].ts = (uint8_t)(p->lock[i].ts & 31u);
        }
    sm_clip.nlock = (uint16_t)k;
    sm_clip_kind = SM_CLIP_TRK;
}

/* PASTE onto track t: the track copied (or, a pattern copied, its track t); nothing copied: nothing */
static void sm_paste_trk(uint32_t t)
{
    if (sm_clip_kind == SM_CLIP_NONE)
        return;
    t %= SM_NTRK;
    sm_undo_snap();
    sm_put_begin(1u << t);
    sm_put_track(t, &sm_clip, sm_clip_kind == SM_CLIP_TRK ? 0u : t, 0, 1);
}

/* PASTE the pattern copied onto the current one: the kit, the steps, the locks, the master length */
static void sm_paste_pat(void)
{
    uint32_t t;
    if (sm_clip_kind != SM_CLIP_PAT)
        return;
    sm_undo_snap();
    sm_put_begin((1u << SM_NTRK) - 1u);
    for (t = 0; t < SM_NTRK; t++)
        sm_put_track(t, &sm_clip, t, 0, 1);
    smp.pat[smp.cur % SM_NPAT].mlen = sm_clip.mlen;       /* (a byte) */
}

/* SHIFT: track t's steps (and their locks) one step left (-1) or right (+1), wrapping at its length */
static void sm_shift(uint32_t t, int32_t dir)
{
    sm_undo_snap();
    sm_put_begin(1u << (t % SM_NTRK));
    sm_put_track(t, &sm_undo_buf, t, dir < 0 ? -1 : 1, 0);   /* (the snapshot is the track as it was) */
}

/* TRN: the notes of track t's trigs d semitones (0 .. 127), in place */
static void sm_trn(uint32_t t, int32_t d)
{
    sm_pattern_t *p = &smp.pat[smp.cur % SM_NPAT];
    uint32_t s, k;
    t %= SM_NTRK;
    sm_undo_snap();
    for (s = 0; s < SM_NSTEP; s++) {
        sm_step_t *x = &p->step[t][s];
        for (k = 0; k < x->n && k < 4u; k++)
            x->note[k] = (uint8_t)clamp((int32_t)x->note[k] + d, 0, 127);
    }
    sm_dirty = 1;
}

/* UNDO: the snapshot back onto its pattern (it is the current one: a switch forgot it), once */
static void sm_undo(void)
{
    uint32_t t;
    if (!sm_undo_ok || sm_undo_pat != smp.cur % SM_NPAT)
        return;
    sm_put_begin((1u << SM_NTRK) - 1u);
    for (t = 0; t < SM_NTRK; t++)
        sm_put_track(t, &sm_undo_buf, t, 0, 1);
    smp.pat[smp.cur % SM_NPAT].mlen = sm_undo_buf.mlen;
    sm_undo_ok = 0;
    sm_msg_show(SM_MSG_UNDO);
}
