/* SPDX-License-Identifier: GPL-3.0-only */
/* PULHRA edits of whole tracks and patterns: the SEQ layer's tools (sm_layer.c: CLEAR, COPY / PASTE a pattern or a
 * track, SHIFT, TRN) and UNDO (SAVE held, sm_input.c). The main loop's, as every write of the model.
 *  - the clipboard (sm_clip): a pattern (everything of it), or a track (its sound - the kit's, its length too - its steps and
 *    its locks, kept as track 0 of the clipboard). PASTE track takes the track copied, or, a pattern copied, its track of the
 *    same number; PASTE pattern needs a pattern copied;
 *  - UNDO: one level, one snapshot (sm_undo_buf) of the current pattern, taken anew before each CLEAR, PASTE, SHIFT, TRN (each
 *    replaces the one before; CLEAR held on to all four tracks keeps the one its tap took); UNDO puts it back once; a pattern
 *    switch forgets it.
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
#define SM_CLEAR_ALL_US 800000u                           /* SEQ + F5 held this long: the pattern (every track) */
enum { SM_CLIP_NONE, SM_CLIP_TRK, SM_CLIP_PAT };

/* the two buffers in .noinit (app.ld NOINIT: RAM the .bss does not pay for; sm_main.c checks they fit with the rest of it):
 * garbage after a power-on or a reset, so each is read only while its flag (zeroed by sm_edit_init) says it was written */
static sm_pattern_t sm_clip __attribute__((section(".noinit")));        /* COPY / PASTE */
static uint8_t sm_clip_kind;                              /* SM_CLIP_* */
static sm_pattern_t sm_undo_buf __attribute__((section(".noinit")));    /* UNDO: the pattern before the last edit */
static uint8_t sm_undo_ok, sm_undo_pat;                   /* ... there is one, of this pattern */

static uint8_t sm_pg_ok;
static void sm_edit_init(void)
{
    sm_pg_ok = 0;
    sm_clip_kind = SM_CLIP_NONE;
    sm_undo_ok = 0;
    sm_undo_pat = 0;
}

/* sm_input.c sm_pat_poll: another pattern plays now (the snapshot is of the one before) */
static void sm_acc_reset(uint32_t mask)                   /* the net SHIFT / TRN shown of the tracks of mask: rewritten, back to 0 */
{
    uint32_t t;
    for (t = 0; t < SM_NTRK; t++)
        if ((mask >> t) & 1u)
            sm_sh_acc[t] = sm_trn_acc[t] = 0;
}
static void sm_edit_pat_changed(void)
{
    sm_undo_ok = 0;
    sm_acc_reset(0xFu);                                   /* (the net shown is of the pattern that was) */
}

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
        if (p->lock[i].param != SM_LOCK_DEAD && ((mask >> SM_TS_T(p->lock[i].ts)) & 1u)) {
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
            if (l->param >= P_COUNT || SM_TS_T(l->ts) != st)
                continue;
            if (!sm_lock_set(t, sm_rot(SM_TS_S(l->ts), rot, len), l->param, l->val))
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
    sm_acc_reset(mask);
    sm_msg_show(SM_MSG_CLEARED);
}

/* CLEAR on the KIT (Tim): the lane in edit alone: its hits off every step (a step no lane hits any more: no trig, its locks
 * gone), its own locks (its values, mix, filter, condition) gone; the other lanes as they were. One UNDO */
static void sm_clear_lane(uint32_t L)
{
    sm_pattern_t *p = &smp.pat[smp.cur % SM_NPAT];
    uint32_t s, i, n, t = SM_KIT_TRK;
    L %= 4u;
    sm_undo_snap();
    for (s = 0; s < SM_NSTEP; s++) {
        sm_step_t *x = &p->step[t][s];
        if (!x->n || !x->note[L])
            continue;
        if (!(x->note[(L + 1u) & 3u] | x->note[(L + 2u) & 3u] | x->note[(L + 3u) & 3u]) && !(x->flags & SMF_REST)) {
            x->n = 0;                                     /* (one byte: the audio stream sees the trig or nothing) */
            SM_BARRIER();
            sm_lock_clear_step(t, s);
        }
        x->note[L] = 0;
    }
    n = p->nlock > SM_NLOCK ? SM_NLOCK : p->nlock;
    for (i = 0; i < n; i++) {
        uint32_t q = p->lock[i].param;
        if (q == SM_LOCK_DEAD || SM_TS_T(p->lock[i].ts) != t)
            continue;
        if ((sm_is_lp(q) && (q - SM_LP0) / 9u == L) || (sm_is_lm(q) && (q - SM_LM0) / 4u == L) ||
            (sm_is_lf(q) && (q - SM_LF0) / 3u == L) || q == SM_LC(L) ||
            (sm_is_le(q) && (q - SM_LE0) / 4u == L))
            p->lock[i].param = SM_LOCK_DEAD;
    }
    if (!sm_playing)
        sm_lock_compact(p);
    sm_msg_show(SM_MSG_CLEARED);
}

/* the page clipboard (Tim: OCT+ held + REC copies the page of steps in view of the selected track, OCT+ held + PLAY pastes it
 * onto the page in view, any track): its 16 steps and their locks (up to SM_PG_NL). In .noinit (RAM has no room): valid only
 * with sm_pg_ok, which the boot clears */
#define SM_PG_NL 96u
static struct {
    sm_step_t st[16];
    uint8_t nl, ofs[SM_PG_NL], par[SM_PG_NL];
    int16_t val[SM_PG_NL];
} sm_pg __attribute__((section(".noinit")));
static void sm_page_copy(void)
{
    const sm_pattern_t *p = &smp.pat[smp.cur % SM_NPAT];
    uint32_t t = sm_ui.trk % SM_NTRK, b = sm_ui.bank * 16u, i, n = p->nlock > SM_NLOCK ? SM_NLOCK : p->nlock;
    for (i = 0; i < 16u; i++)
        sm_pg.st[i] = p->step[t][(b + i) % SM_NSTEP];
    sm_pg.nl = 0;
    for (i = 0; i < n && sm_pg.nl < SM_PG_NL; i++) {
        uint32_t s = SM_TS_S(p->lock[i].ts);
        if (p->lock[i].param == SM_LOCK_DEAD || SM_TS_T(p->lock[i].ts) != t || s < b || s >= b + 16u)
            continue;
        sm_pg.ofs[sm_pg.nl] = (uint8_t)(s - b);
        sm_pg.par[sm_pg.nl] = p->lock[i].param;
        sm_pg.val[sm_pg.nl++] = p->lock[i].val;
    }
    sm_pg_ok = 1;
    sm_msg_show(SM_MSG_PCOPY);
}
static void sm_page_paste(void)
{
    sm_pattern_t *p = &smp.pat[smp.cur % SM_NPAT];
    uint32_t t = sm_ui.trk % SM_NTRK, b = sm_ui.bank * 16u, i, j;
    if (!sm_pg_ok) {
        sm_msg_show(SM_MSG_GEMPTY);
        return;
    }
    sm_undo_snap();
    for (i = 0; i < 16u && b + i < SM_NSTEP; i++) {       /* (the order of a trig's paste, sm_trig_put: n 0, the locks, the fields, n) */
        sm_step_t *x = &p->step[t][b + i], y = sm_pg.st[i];
        uint32_t had = x->n;
        x->n = 0;
        SM_BARRIER();
        sm_lock_clear_step(t, b + i);
        if (had)
            sm_unlock_req(t, b + i);
        for (j = 0; j < sm_pg.nl; j++)
            if (sm_pg.ofs[j] == i && !sm_lock_set(t, b + i, sm_pg.par[j], sm_pg.val[j]))
                break;
        y.n = 0;
        *x = y;
        SM_LOCK_PREEMPT();
        SM_BARRIER();
        x->n = sm_pg.st[i].n;
    }
    sm_acc_reset(1u << t);
    sm_msg_show(SM_MSG_PPASTE);
    sm_dirty = 1;
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
        if (p->lock[i].param != SM_LOCK_DEAD && SM_TS_T(p->lock[i].ts) == t) {
            sm_clip.lock[k] = p->lock[i];
            sm_clip.lock[k++].ts = SM_TS(0, SM_TS_S(p->lock[i].ts));
        }
    sm_clip.nlock = (uint16_t)k;
    memcpy(sm_clip.lane, p->lane, sizeof sm_clip.lane);  /* (a KIT's lanes go with T4's sound) */
    memcpy(sm_clip.lmix, p->lmix, sizeof sm_clip.lmix);
    memcpy(sm_clip.lflt, p->lflt, sizeof sm_clip.lflt);
    memcpy(sm_clip.lenv, p->lenv, sizeof sm_clip.lenv);
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
    if (t == SM_KIT_TRK && sm_snd_kit(&smp.pat[smp.cur % SM_NPAT].kit.snd[t]))   /* (a KIT pasted on T4: its lanes too; the LOAD */
    {
        memcpy(smp.pat[smp.cur % SM_NPAT].lane, sm_clip.lane, sizeof sm_clip.lane);   /* brings them) */
        memcpy(smp.pat[smp.cur % SM_NPAT].lmix, sm_clip.lmix, sizeof sm_clip.lmix);
        memcpy(smp.pat[smp.cur % SM_NPAT].lflt, sm_clip.lflt, sizeof sm_clip.lflt);
        memcpy(smp.pat[smp.cur % SM_NPAT].lenv, sm_clip.lenv, sizeof sm_clip.lenv);
    }
    sm_acc_reset(1u << t);
}

/* PASTE the pattern copied onto the current one: the kit, the steps, the locks, the master length */
static int sm_paste_pat(void)
{
    uint32_t t;
    if (sm_clip_kind != SM_CLIP_PAT)
        return 0;
    sm_undo_snap();
    sm_put_begin((1u << SM_NTRK) - 1u);
    for (t = 0; t < SM_NTRK; t++)
        sm_put_track(t, &sm_clip, t, 0, 1);
    smp.pat[smp.cur % SM_NPAT].mlen = sm_clip.mlen;       /* (a byte) */
    smp.pat[smp.cur % SM_NPAT].speed = sm_clip.speed;
    smp.pat[smp.cur % SM_NPAT].swing = sm_clip.swing;
    sm_acc_reset(0xFu);
    return 1;
}

/* PASTE the pattern copied into pattern id g (SEQ held: the one picked with ALGORITHM; Tim, 31.10: no need to go there first):
 * none picked, or the current one: onto the current pattern (sm_paste_pat, with its UNDO); another: written into it (its bank
 * brought into RAM first, waiting if need be: sm_bank.c), no UNDO (the current pattern is untouched). 0 nothing copied, 1 pasted
 * (or on its way), 2 the flash failed */
static int sm_paste_pat_into(uint32_t g)
{
    int r;
    if (sm_clip_kind != SM_CLIP_PAT)
        return 0;
    if (g >= SM_NGPAT || g == sm_gid_of(smp.cur))
        return sm_paste_pat();
    sm_flash_hold = 0;
    r = sm_pat_store(g, &sm_clip);
    if (r == 1) {                                         /* (its bank waits for a free half) */
        sm_gop_kind = 1;
        sm_gop_g = (uint8_t)g;
        sm_gop_src = &sm_clip;
    }
    return r == 2 ? 2 : 1;
}

/* lock param q is KIT lane L's own (its values, mix, filter, envelope, condition) */
static int sm_lock_of_lane(uint32_t q, uint32_t L)
{
    return (sm_is_lp(q) && (q - SM_LP0) / 9u == L) || (sm_is_lm(q) && (q - SM_LM0) / 4u == L) ||
           (sm_is_lf(q) && (q - SM_LF0) / 3u == L) || q == SM_LC(L) || (sm_is_le(q) && (q - SM_LE0) / 4u == L);
}

/* SHIFT on the KIT (Tim: it moved every lane): lane L's hits and its own locks one step left (-1) or right (+1), wrapping at the
 * track's length; the other lanes, and the steps' shared fields, stay. A step L leaves that no lane hits any more: no trig (its
 * other locks gone); a step L comes to without a trig: a fresh one. The UNDO snapshot first */
static void sm_shift_lane(uint32_t L, int32_t dir)
{
    sm_pattern_t *p = &smp.pat[smp.cur % SM_NPAT];
    uint32_t t = SM_KIT_TRK, len = (uint32_t)clamp(p->kit.snd[t].len, 1, (int32_t)SM_NSTEP), s, i, n;
    uint8_t v[SM_NSTEP];
    L %= 4u;
    sm_undo_snap();
    for (s = 0; s < len; s++)
        v[s] = p->step[t][s].n ? p->step[t][s].note[L] : 0u;
    n = p->nlock > SM_NLOCK ? SM_NLOCK : p->nlock;
    for (s = 0; s < len; s++) {
        sm_step_t *x = &p->step[t][s];
        uint32_t w = v[(s + len - (uint32_t)(dir < 0 ? len - 1u : 1u)) % len], other;
        other = x->n ? (x->note[(L + 1u) & 3u] | x->note[(L + 2u) & 3u] | x->note[(L + 3u) & 3u]) : 0u;
        if (w && !x->n) {
            for (i = 0; i < n; i++)                       /* (a stale lock of an empty step: gone; L's own move below) */
                if (p->lock[i].ts == SM_TS(t, s) && p->lock[i].param != SM_LOCK_DEAD && !sm_lock_of_lane(p->lock[i].param, L))
                    p->lock[i].param = SM_LOCK_DEAD;
            sm_step_fresh(t, s, 0);
            x->note[0] = 0;
            x->note[L] = (uint8_t)w;
            SM_BARRIER();
            x->n = 1;
        } else if (w) {
            x->note[L] = (uint8_t)w;
            x->flags &= (uint8_t)~SMF_REST;
        } else if (x->n && x->note[L]) {
            if (!other && !(x->flags & SMF_REST)) {
                x->n = 0;                                 /* (one byte: the audio stream sees the trig or nothing) */
                SM_BARRIER();
                for (i = 0; i < n; i++)
                    if (p->lock[i].ts == SM_TS(t, s) && p->lock[i].param != SM_LOCK_DEAD && !sm_lock_of_lane(p->lock[i].param, L)) {
                        SM_LOCK_PREEMPT();
                        p->lock[i].param = SM_LOCK_DEAD;
                    }
            }
            x->note[L] = 0;
        }
    }
    for (i = 0; i < n; i++) {                             /* L's own locks with its hits */
        uint32_t st = SM_TS_S(p->lock[i].ts);
        if (p->lock[i].param == SM_LOCK_DEAD || SM_TS_T(p->lock[i].ts) != t || st >= len || !sm_lock_of_lane(p->lock[i].param, L))
            continue;
        SM_LOCK_PREEMPT();
        p->lock[i].ts = (uint8_t)SM_TS(t, (st + len + (uint32_t)(dir < 0 ? len - 1u : 1u)) % len);
    }
    if (!sm_playing)
        sm_lock_compact(p);
    sm_dirty = 1;
}

/* SHIFT: track t's steps (and their locks) one step left (-1) or right (+1), wrapping at its length; the KIT: the lane in edit
 * alone (sm_shift_lane) */
static void sm_shift_trk(uint32_t t, int32_t dir)
{
    if (sm_trk_kit(t % SM_NTRK, &smp.pat[smp.cur % SM_NPAT])) {
        sm_shift_lane(sm_kit_lane, dir);
        return;
    }
    sm_undo_snap();
    sm_put_begin(1u << (t % SM_NTRK));
    sm_put_track(t, &sm_undo_buf, t, dir < 0 ? -1 : 1, 0);   /* (the snapshot is the track as it was) */
}

/* SEQ held + K3 / A#3 / C#4 (Tim: the selected track alone): its steps (and locks) one step left (-1) or right (+1), wrapping at its
 * length; the net under K3 (SHT+N, the track's: sm_sh_acc, as SHIFT + D#4 / F#4) */
static void sm_shift_all(int32_t dir)
{
    uint32_t t = sm_ui.trk % SM_NTRK, len = (uint32_t)clamp(smp.pat[smp.cur % SM_NPAT].kit.snd[t].len, 1, (int32_t)SM_NSTEP);
    int32_t v;
    dir = dir < 0 ? -1 : 1;
    sm_shift_trk(t, dir);
    v = ((int32_t)sm_sh_acc[t] + dir) % (int32_t)len;
    if (v > (int32_t)len / 2)
        v -= (int32_t)len;
    else if (v < -(int32_t)len / 2)
        v += (int32_t)len;
    sm_sh_acc[t] = (int8_t)clamp(v, -99, 99);
    sm_dirty = 1;
}
static void sm_trn(uint32_t t, int32_t d);
/* SEQ held + K4 / D#4 / F#4: the selected track's trigs a semitone down / up (sm_trn; a KIT: nothing); TR+N under K4 */
static void sm_trn_all(int32_t d)
{
    uint32_t t = sm_ui.trk % SM_NTRK;
    d = d < 0 ? -1 : 1;
    if (sm_trk_kit(t, &smp.pat[smp.cur % SM_NPAT]))
        return;
    sm_trn(t, d);
    sm_trn_acc[t] = (int8_t)clamp((int32_t)sm_trn_acc[t] + d, -99, 99);
    sm_dirty = 1;
}

/* TRN: the notes of track t's trigs d semitones (0 .. 127), in place; the track's default NOTE too, so the trigs that inherit it
 * (SM_INH, left as they are) move with it */
static void sm_trn(uint32_t t, int32_t d)
{
    sm_pattern_t *p = &smp.pat[smp.cur % SM_NPAT];
    uint32_t s, k;
    t %= SM_NTRK;
    sm_undo_snap();
    if (sm_trk_kit(t, p))                                 /* (KIT: no pitch to move) */
        return;
    p->kit.snd[t].dnote = (uint8_t)clamp((int32_t)p->kit.snd[t].dnote + d, 0, 127);   /* (a byte) */
    for (s = 0; s < SM_NSTEP; s++) {
        sm_step_t *x = &p->step[t][s];
        for (k = 0; k < x->n && k < 4u; k++)
            if (x->note[k] != SM_INH)
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
    smp.pat[smp.cur % SM_NPAT].speed = sm_undo_buf.speed;
    smp.pat[smp.cur % SM_NPAT].swing = sm_undo_buf.swing;
    sm_undo_ok = 0;
    sm_acc_reset(0xFu);
    sm_msg_show(SM_MSG_UNDO);
}

/* the trig clipboard (sm_input.c: REC + a trig key copies, a trig held + OCT+ pastes): one step as it is - its fields (an
 * inherited one stays SM_INH: the trig pasted takes the defaults of the track it lands on), flags, nudge, notes - and its
 * live locks. A copy of an empty step copies nothing (the clipboard stays) */
static sm_step_t sm_tclip;
static uint8_t sm_tclip_ok, sm_tclip_nl, sm_tclip_p[P_COUNT];
static int16_t sm_tclip_v[P_COUNT];

static int sm_trig_copy(uint32_t t, uint32_t st)
{
    const sm_pattern_t *p = &smp.pat[smp.cur % SM_NPAT];
    uint32_t i, n = p->nlock > SM_NLOCK ? SM_NLOCK : p->nlock, ts;
    t %= SM_NTRK;
    st %= SM_NSTEP;
    if (!p->step[t][st].n)
        return 0;
    ts = SM_TS(t, st);
    sm_tclip = p->step[t][st];
    sm_tclip_nl = 0;
    for (i = 0; i < n && sm_tclip_nl < P_COUNT; i++)
        if (p->lock[i].ts == ts && p->lock[i].param < SM_NPID) {   /* (the KIT's lanes' values and conditions too) */
            sm_tclip_p[sm_tclip_nl] = p->lock[i].param;
            sm_tclip_v[sm_tclip_nl++] = p->lock[i].val;
        }
    sm_tclip_ok = 1;
    return 1;
}

/* the trig copied onto step st of track t (a trig there or none: replaced whole), the order of a PASTE (sm_put_*): n = 0 first
 * (the audio stream fires nothing of the step from here), its locks removed (tombstones; stopped: compacted) and the core's
 * of them back to the base, the clipboard's locks (each whole before it counts), the fields (n still 0), SM_BARRIER(), n. So
 * the interrupt sees the old trig, no trig, or the new one with all its locks. The caller takes the UNDO snapshot */
static void sm_trig_put(uint32_t t, uint32_t st)
{
    sm_step_t *x, y;
    uint32_t i, had;
    if (!sm_tclip_ok || t >= SM_NTRK || st >= SM_NSTEP)
        return;
    x = &smp.pat[smp.cur % SM_NPAT].step[t][st];
    had = x->n;
    x->n = 0;
    SM_BARRIER();
    sm_lock_clear_step(t, st);
    if (had)
        sm_unlock_req(t, st);                             /* (its locks may be on the core) */
    for (i = 0; i < sm_tclip_nl; i++)
        if (!sm_lock_set(t, st, sm_tclip_p[i], sm_tclip_v[i]))
            break;                                        /* (the pool full: LOCKS FULL, the rest not placed) */
    y = sm_tclip;
    y.n = 0;
    *x = y;
    SM_LOCK_PREEMPT();
    SM_BARRIER();
    x->n = sm_tclip.n;
    sm_dirty = 1;
}

/* a trig held + OCT+: the trig copied (sm_trig_copy) onto every trig key down, one UNDO snapshot for them; the keys do not
 * toggle at their release. Nothing copied yet: nothing */
static void sm_paste_held(void)
{
    uint32_t k;
    if (!sm_tclip_ok)
        return;
    sm_undo_snap();
    for (k = 0; k < SM_NKEYS; k++)
        if ((sm_key_arm >> k) & 1u) {
            sm_trig_put(sm_key_trk[k], sm_key_step[k]);
            sm_key_used |= 1u << k;
        }
}
