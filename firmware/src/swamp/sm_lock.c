/* SPDX-License-Identifier: GPL-3.0-only */
/* SWAMP MACHINE parameter locks, the read side. sm_lock_find is a pure read of the pattern's pool (the main loop is the only
 * writer, sm_model.h); an entry of param 0xFF is a removed one (a tombstone) and never matches. Task 4 adds the writers. */

/* the index in p->lock[] of the lock of (track t, step s, parameter param), -1: none */
static int32_t sm_lock_find(const sm_pattern_t *p, uint32_t t, uint32_t s, uint32_t param)
{
    uint32_t i, ts = (t << 5) | s, n = p->nlock;
    if (param >= 0xFFu)
        return -1;
    if (n > SM_NLOCK)
        n = SM_NLOCK;
    for (i = 0; i < n; i++)
        if (p->lock[i].ts == ts && p->lock[i].param == param)
            return (int32_t)i;
    return -1;
}

/* is the parameter locked on any step of the track (the dot in the centre of its ring); Task 4 fills it */
static int sm_param_locked_any(uint32_t t, uint32_t param)
{
    (void)t;
    (void)param;
    return 0;
}
