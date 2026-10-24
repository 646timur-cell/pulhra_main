/* SPDX-License-Identifier: GPL-3.0-only */
/* PULHRA kit -> the sound core: a kit sound onto a track (trk[]). */

/* A kit sound onto track t. ISR ONLY (the command queue calls it from events_block): the audio stream is the only
 * writer of trk[]. Within a block it must come BEFORE engine_block (events_block drains the queue first): applied after
 * it, xf_on is still 0 and track_render plays the old engine with the new engine's P_E0..P_E7 for that block (voice.c
 * pe_old is taken only by engine_block), values that index the old engine's tables out of range. The sound's machine
 * becomes eng_req and its preset and every parameter go into trk[t].p[] (the model's p[] is int8_t, trk[].p int16_t:
 * the copy sign-extends). Nothing else of the track is touched.
 *
 * A machine change is the core's own fade, no crossfade of ours: engine_block (voice.c, at each block start, before
 * any note of the block) sees eng_req != engine, and with voices sounding sets xf = XF_BLOCKS; track_render then
 * renders the old engine with its own values (pe_old) and a linear gain (xf-1)/XF_BLOCKS per block, xf-- per block,
 * and when xf reaches 0 the voices are dropped, the engine switches (eng_mem_claim) and the notes that came during
 * the fade (xp_note) start on the new one. The old voices' fade is XF_BLOCKS = 4 blocks of CTL = 32 samples = 128
 * samples = 2.9 ms at 44.1 kHz (gain 3/4, 2/4, 1/4, 0: the amplitude ramps inside a block, amp_at, so the first
 * block already falls from 1 to 3/4), plus the block in which engine_block starts it (<= 0.73 ms): 2.9 .. 3.6 ms from
 * the change to silence of the old engine, a new note after it. The spec asks ~5 ms; the core's is 2.9 ms (about 60 %
 * of it); voice.c is left as it is. A track with nothing sounding switches in the same block. */
static void sm_track_load(uint32_t t, const sm_sound_t *s)
{
    uint32_t i;
    track_t *k = &trk[t];
    k->eng_req = (uint8_t)sm_engine_of(s->machine);
    k->preset = s->preset;
    for (i = 0; i < P_COUNT; i++)
        k->p[i] = (int16_t)s->p[i];                       /* (int8_t -> int16_t: sign-extended) */
}

/* the KIT's lanes of pattern pt onto the core (eng_dr.c dr_lp, T4's): ISR only, as sm_track_load (or before the audio starts) */
static void sm_lanes_core(const sm_pattern_t *pt)
{
    uint32_t L, k;
    for (L = 0; L < 4u; L++)
        for (k = 0; k < 9u; k++)
            dr_lp[SM_KIT_TRK][L][k] = pt->lane[L][k];
    for (L = 0; L < 4u; L++)
        for (k = 0; k < 4u; k++)
            dr_lm[SM_KIT_TRK][L][k] = pt->lmix[L][k];
}

/* power-on, before the audio starts (nothing runs concurrently): the four sounds onto the tracks, the engines
 * switched at once and their state regions claimed */
static void sm_kit_boot(const sm_kit_t *k)
{
    uint32_t t;
    for (t = 0; t < SM_NTRK; t++) {
        sm_track_load(t, &k->snd[t]);
        trk[t].engine = trk[t].eng_req;
        eng_mem_claim(&trk[t]);
    }
}
