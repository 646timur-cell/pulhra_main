/* SPDX-License-Identifier: GPL-3.0-only */
/* SWAMP MACHINE project model: 16 patterns of 4 tracks x 32 steps, the kit (the four sounds) in every pattern, parameter
 * locks per pattern. Ownership: the main loop is the ONLY writer of the model (smp: parameters, steps, lengths, tempo,
 * the current pattern, everything); the audio stream only reads it (the steps it plays, a sound on SMC_LOAD) and owns
 * trk[], the transport and the sequencer's state (sm_cmd.c, sm_seq.c). */
#define SM_NTRK 4u
#define SM_NPAT 16u
#define SM_NSTEP 32u
#define SM_NLOCK 192u                                     /* parameter locks per pattern */
#define SM_NCHAIN 16u
#define SM_NMACHINE NENG_SHOWN                            /* stage 1: MONOFM's machines, in the order of eng_vis() */

#define SMF_ACCENT 1u
#define SMF_SLIDE 2u
#define SMF_REST 4u
#define SMF_PSLIDE 8u
#define SMF_RAT(f) (((f) >> 4) & 3u)                      /* ratchet: 0..3 = x1..x4 */

/* a step's cond: 0 100 % (no condition), 1..99 that chance; then the conditions (MONOFM's, without NEI): 1ST !1ST (the
 * track's first pass since PLAY or not), PRE !PRE (the track's last condition passed or not), FILL !FILL, and A:B (the
 * A-th of every B passes of the track) 1:2 2:2 1:3 .. 8:8 */
#define SM_COND_1ST 100u
#define SM_COND_N1ST 101u
#define SM_COND_PRE 102u
#define SM_COND_NPRE 103u
#define SM_COND_FILL 104u
#define SM_COND_NFILL 105u
#define SM_COND_AB0 106u                                  /* 1:2; 35 of them, B = 2..8 */
#define SM_COND_END (SM_COND_AB0 + 35u)
/* a step's len: an index into MONOFM's LEN list (core.h MF_LEN8 / MF_LEN_N: TRK 1/8 1/4 1/2 3/4 1 1.5 2 3 4 6 8 16 INF) */
#define SM_LEN_INF MF_LEN_INF

typedef struct {
    uint8_t note[4], n, vel, len, cond, flags;            /* n == 0: the trig is off; n with SMF_REST: a lock trig, no note */
    int8_t nudge;                                         /* -8..+7, in 1/16 step */
} sm_step_t;

typedef struct {
    uint8_t ts;                                           /* trk << 5 | step */
    uint8_t param;
    int16_t val;
} sm_lock_t;

typedef struct {
    uint8_t machine, preset, len, rsv;
    int8_t p[P_COUNT];                                    /* as mfp_trk_t: -64..127 */
} sm_sound_t;

typedef struct { sm_sound_t snd[SM_NTRK]; } sm_kit_t;     /* (master inserts and the compressor: stage 4) */

typedef struct {
    sm_kit_t kit;
    sm_step_t step[SM_NTRK][SM_NSTEP];
    uint16_t nlock;
    uint8_t mlen, mode;
    sm_lock_t lock[SM_NLOCK];
} sm_pattern_t;

typedef struct {
    sm_pattern_t pat[SM_NPAT];
    uint16_t bpm10;                                       /* 1200 = 120.0 */
    uint8_t swing, cur, sel, nchain;
    uint8_t chain[SM_NCHAIN];
    uint8_t mute;                                         /* bit per track */
} sm_project_t;
