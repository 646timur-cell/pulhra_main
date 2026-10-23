/* SPDX-License-Identifier: GPL-3.0-only */
/* PULHRA's SCL page (ROOT, SCALE, QNT, TRN; sm_pages.c) in the notes: Felucca's own rules (seq.c kb_pitch, scale_snap,
 * QNT SEQ), which PULHRA does not build, done again on PULHRA's notes. The MICRO page (TUNE, AMT) is the core's already
 * (voice.c micro_cents, from ROOT).
 *   the keyboard mode (sm_input.c sm_kbd_poll): QNT SNAP and SEQ round every key down onto ROOT's SCALE, WHITE walks the
 *   scale on the white keys from C4 = ROOT (a black key silent), OFF plays the key; TRN on top of all (semitones)
 *   the steps (sm_seq.c sm_seq_notes): QNT SEQ rounds a trig's notes down onto the scale as they play (two snapping
 *   together play once); never on a machine that maps the keys itself (DRUM, the slices)
 * MIDI notes play as they come (Felucca's too). */
#define SM_KEY_SILENT 0xFFu
enum { SM_QN_OFF, SM_QN_SNAP, SM_QN_WHITE, SM_QN_SEQ };   /* P_QUANT (params.c N_QUANT) */

static const uint16_t SM_SCALE_MASK[] = {                 /* params.c N_SCALE's order (seq.c SCALE_MASK, the same) */
    0xFFF,                                                                          /* CHR */
    (1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 11),   /* MAJ */
    (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 10),   /* MIN */
    (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 10),   /* DOR */
    (1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 10),   /* MIX */
    (1 << 0) | (1 << 2) | (1 << 4) | (1 << 7) | (1 << 9),                          /* PEN */
    (1 << 0) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 10),                         /* MPEN */
    (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 11),   /* HARM */
    (1 << 0) | (1 << 1) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 10),   /* PHRY */
    (1 << 0) | (1 << 2) | (1 << 4) | (1 << 6) | (1 << 7) | (1 << 9) | (1 << 11),   /* LYD */
    (1 << 0) | (1 << 1) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 8) | (1 << 10),   /* LOC */
    (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 11),   /* MEL (ascending) */
    (1 << 0) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 7) | (1 << 10),              /* BLUES (minor) */
    (1 << 0) | (1 << 2) | (1 << 4) | (1 << 6) | (1 << 8) | (1 << 10),              /* WHOLE */
    (1 << 0) | (1 << 1) | (1 << 3) | (1 << 4) | (1 << 6) | (1 << 7) | (1 << 9) | (1 << 10), /* DIMHW */
    (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 8) | (1 << 9) | (1 << 11), /* DIMWH */
};
_Static_assert(sizeof SM_SCALE_MASK / sizeof SM_SCALE_MASK[0] == sizeof N_SCALE / sizeof N_SCALE[0], "a mask per SCALE name");

static uint32_t sm_scale_mask(int32_t scale)
{
    return SM_SCALE_MASK[clamp(scale, 0, (int32_t)(sizeof SM_SCALE_MASK / sizeof SM_SCALE_MASK[0]) - 1)];
}
/* note n moved down onto ROOT's SCALE */
static int32_t sm_scale_snap(int32_t root, int32_t scale, int32_t n)
{
    uint32_t mask = sm_scale_mask(scale), guard = 12;
    root = clamp(root, 0, 11);
    while (guard-- && !((mask >> (uint32_t)((n - root + 120) % 12)) & 1u))
        n--;
    return n;
}

/* the keyboard mode's key k (0 = F3) at octave oct on a sound p: its note, SM_KEY_SILENT a black key under WHITE */
static uint32_t sm_key_pitch(const int8_t *p, uint32_t k, int32_t oct)
{
    static const int8_t DEGREE[12] = {0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6};
    int32_t n = 53 + (int32_t)k, tr = clamp(p[P_TRANS], -24, 24);
    if (p[P_QUANT] == SM_QN_SNAP || p[P_QUANT] == SM_QN_SEQ)
        return (uint32_t)clamp(sm_scale_snap(p[P_ROOT], p[P_SCALE], n + 12 * oct + tr), 0, 127);
    if (p[P_QUANT] == SM_QN_WHITE) {                     /* white keys walk the scale from C4 = ROOT, black keys silent */
        uint32_t mask = sm_scale_mask(p[P_SCALE]), i;
        int32_t count = 0, degree = DEGREE[n % 12], wo;
        if (degree < 0)
            return SM_KEY_SILENT;
        degree += (n / 12 - 5) * 7;
        for (i = 0; i < 12u; i++)
            count += (mask >> i) & 1u;
        wo = degree / count;
        degree %= count;
        if (degree < 0) {
            degree += count;
            wo--;
        }
        for (i = 0; i < 12u; i++)
            if ((mask >> i) & 1u) {
                if (!degree)
                    break;
                degree--;
            }
        n = 60 + clamp(p[P_ROOT], 0, 11) + 12 * wo + (int32_t)i;
    }
    return (uint32_t)clamp(n + 12 * oct + tr, 0, 127);
}

/* the audio stream: track t's QNT is SEQ on a machine that leaves the keys alone (its trigs' notes snap as they play) */
static uint32_t sm_qseq(uint32_t t)
{
    const engine_t *e = ENGINES[eng_idx(trk[t].eng_req)];
    return trk[t].p[P_QUANT] == SM_QN_SEQ && !(e->keys && e->keys(&trk[t], 0) >= 0);
}
