/* SPDX-License-Identifier: GPL-3.0-only */
/* SWAMP MACHINE project model: 16 patterns of 4 tracks x 32 steps, the kit (the four sounds) in every pattern, parameter
 * locks per pattern. The main loop is its only writer; the audio stream reads (Task 4 copies what it plays). */
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

typedef struct {
    uint8_t note[4], n, vel, len, cond, flags;            /* n == 0: the trig is off */
    int8_t nudge;
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
