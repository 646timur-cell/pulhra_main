/* SPDX-License-Identifier: GPL-3.0-only */
/* PULHRA project model: 5 banks A..E of 8 patterns (40, A01..E08) of 4 tracks x 32 steps, the kit (the four sounds)
 * in every pattern, parameter locks per pattern. Two banks live in RAM (smp.pat: two halves of 8, smp.hbank says which bank
 * each half holds), the rest in the flash (sm_bank.c). Ownership: the main loop is the ONLY writer of the model (smp: parameters, steps, lengths, tempo,
 * the current pattern, everything); the audio stream only reads it (the steps it plays, a sound on SMC_LOAD) and owns
 * trk[], the transport and the sequencer's state (sm_cmd.c, sm_seq.c). */
#define SM_NTRK 4u
#define SM_NBANK 5u                                       /* banks A..E */
#define SM_BPAT 8u                                        /* patterns per bank */
#define SM_NGPAT (SM_NBANK * SM_BPAT)                     /* the pattern ids 0..39 (A01..E08: bank * 8 + place) */
#define SM_NHALF 2u                                       /* banks in RAM */
#define SM_NPAT (SM_NHALF * SM_BPAT)                      /* RAM slots of smp.pat: half * 8 + place */
#define SM_BANK_NONE 0xFFu
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
/* a step's len: an index into PULHRA's own LEN list (not core.h's: MONOFM / Felucca keep theirs): TRK 1/64 1/32 1/16 1/8 1/4
 * 1/2 3/4 1 1.5 2 3 4 6 8 16 INF. SM_LEN64: the gate in 1/64 steps (0: TRK = the track's GATE, INF = held to the next trig) */
#define SM_NLEN 17u
#define SM_LEN_INF 16u
static const uint16_t SM_LEN64[SM_NLEN] = {0, 1, 2, 4, 8, 16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 1024, 0};
static const char *const SM_LEN_N[SM_NLEN] = {"TRK", "1/64", "1/32", "1/16", "1/8", "1/4", "1/2", "3/4", "1", "1.5", "2", "3",
                                              "4", "6", "8", "16", "INF"};

/* a trig's NOTE (note[0]), VEL, LEN, COND hold SM_INH until they are edited on that step: the trig then takes the track's
 * default (sm_sound_t dnote / dvel / dlen / dcond, the STEP page with no step selected), so a default changed later changes
 * every trig that never had that field edited. 0xFF is none of their values (note 0..127, vel 1..127, len < SM_NLEN, cond <
 * SM_COND_END). The audio stream resolves them when a trig fires (sm_step_eff), each a byte read whole */
#define SM_INH 0xFFu

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
    uint8_t machine, preset, len;
    uint8_t swing;                                        /* (unused: the swing is the pattern's, sm_pattern_t.swing; kept for the bank
                                                             format) the track's own swing on top of the project's (smp.swing): 0..25
                                                             percent more, 0 none (the STEP page's SELECT with no step: SWNG
                                                             50+swing %); the sequencer's, as len (a preset load keeps it) */
    uint8_t dnote, dvel, dlen, dcond;                     /* the track's trig defaults (SM_INH above); the sequencer's, as len:
                                                             a preset load keeps them, a track copy carries them */
    int8_t p[P_COUNT];                                    /* as mfp_trk_t: -64..127 */
} sm_sound_t;

typedef struct { sm_sound_t snd[SM_NTRK]; } sm_kit_t;     /* (master inserts and the compressor: stage 4) */

typedef struct {
    sm_kit_t kit;
    sm_step_t step[SM_NTRK][SM_NSTEP];
    uint8_t nlock;                                        /* (at most SM_NLOCK = 192: a byte. It was a uint16_t, its high byte
                                                             always 0 in a saved bank: that byte is swing now) */
    uint8_t swing;                                        /* the pattern's swing (CLOCK K2): 0..25 percent over 50 (50 .. 75 %); every
                                                             track of the pattern swings by it (no swing per track); 0 = none */
    uint8_t mlen;                                         /* master length in 1/16 steps 1..SM_MLEN_MAX, 0 AUTO (the longest track) */
    int8_t speed;                                         /* the pattern's speed (the SEQ layer's K1, Elektron's SCALE): SM_SPEED_*
                                                             places as an offset from 1x, -3 .. +1 = 1/8x 1/4x 1/2x 1x 2x; 0 = 1x (the
                                                             byte was a zeroed reserve: every saved pattern plays at 1x) */
    sm_lock_t lock[SM_NLOCK];
} sm_pattern_t;

/* the pattern change (smp.mode, the project's, as MONOFM's): SEQUENTIAL at the end of the pattern playing (its master length),
 * every track from step 1; DIRECT START at once, every track from step 1; DIRECT JUMP at once, the tracks where they were.
 * Stopped: at once. The main loop asks (smp.next); the audio stream switches (sm_seq.c sm_pat_block) and says so in its own
 * sm_play_pat, which the main loop mirrors into smp.cur (sm_input.c sm_pat_poll) */
enum { SM_PM_SEQ, SM_PM_START, SM_PM_JUMP, SM_PM_COUNT };
#define SM_PAT_NONE 0xFFu
#define SM_MLEN_MAX 128u
/* the pattern speeds (sm_pattern_t.speed): a step of 8 / m grid steps, m in eighths; the place 3 is 1x */
#define SM_NSPEED 5u
#define SM_SPEED_ONE 3
static const uint8_t SM_SPEED8[SM_NSPEED] = {1, 2, 4, 8, 16};
static const char *const SM_SPEED_N[SM_NSPEED] = {"1/8", "1/4", "1/2", "1X", "2X"};
static uint32_t sm_speed_place(int32_t speed) { return (uint32_t)clamp(SM_SPEED_ONE + speed, 0, (int32_t)SM_NSPEED - 1); }

/* smp.cur, smp.next, sm_play_pat: RAM slots (0..15); smp.chain: pattern ids (0..39, any bank: the audio stream finds a link's
 * slot through smp.hbank, sm_slot_of; a link whose bank is not in RAM waits) */
typedef struct {
    sm_pattern_t pat[SM_NPAT];
    uint8_t hbank[SM_NHALF];                              /* the bank each RAM half holds (SM_BANK_NONE: none); the main loop's,
                                                             the audio stream reads it (a link's slot) */
    uint16_t bpm10;                                       /* 1200 = 120.0 */
    uint8_t swing, cur, sel, nchain;
    uint8_t chain[SM_NCHAIN];
    uint8_t mute;                                         /* bit per track */
    uint8_t mode;                                         /* SM_PM_* */
    uint8_t next;                                         /* the pattern asked for (SM_PAT_NONE none; the main loop's) */
} sm_project_t;
