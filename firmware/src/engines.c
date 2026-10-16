/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Engine table (order = PRESETS browsing order and the engine numbers of the editor protocol), the
 * factory patterns (SAVE > PHRASES) and the parts' sounds at power-on. */
#include "dsp.c"

/* The parts' engine memory (1.5): a part plays one engine at a time, so the engines with large per-part state
 * (engine_t.mem: PHYS's slots, GRAIN's grains and index, DRUM's lanes, FM6's notes, SLICE's reverse windows)
 * keep it in one region per part instead of an array each. The region holds the state of the engine that
 * last claimed it (eng_mem_owner); a switch to another such engine zeroes its engine_t.mem bytes, which is
 * the state each had at power-on, so a switch sounds as it did with the arrays (tests/union_test.c). State
 * that runs on across notes and switches stays outside (WHEEL's wheels, GRAIN's random numbers).
 * Sized by the largest, PHYS (3 x SYMP's slot); an engine that needs more than ENG_MEM_PART grows it for
 * every part (x NPART of the pool). Zero-initialised pool section: main.c fm1_cstart clears it. */
#if FELUCCA_MONOFM
/* MONOFM: four parts of 28 KiB (the global delay gave up 32 KiB of the pool for it, fx.c DLY_LEN): LYRA's delay and DRIFT's
 * second loop a little shorter than STRANGE's (eng_lyra.c, eng_drift.c), the rest as there */
#define ENG_MEM_PART 28672u
#elif FELUCCA_STRANGE
/* STRANGE: two parts render (NLIVE), so the pool of four regions of 12888 goes to two of 36 KiB (+20 KiB in all): the
 * delay of LYRA, the strings of STRING, the bores of BLOW */
#define ENG_MEM_PART 36864u
#else
#define ENG_MEM_PART 12888u
#endif
static union {
    uint64_t align;
    uint8_t b[ENG_MEM_PART];
} eng_mem[NLIVE] __attribute__((section(".pool")));
static uint8_t eng_mem_owner[NLIVE];       /* engine index + 1 whose state is in the region, 0 = none (zeros) */
#define ENG_MEM(p) ((void *)eng_mem[(p) < NLIVE ? (p) : 0].b)  /* part p's region (p < NLIVE; a part that never
                                                                * renders never asks) */
#define ENG_MEM_FITS(name, size) _Static_assert((size) <= ENG_MEM_PART, name " state over ENG_MEM_PART")

#include "eng_analog.c"
#include "eng_phase.c"
#include "eng_lofi.c"
#include "eng_sample.c"
#include "eng_formant.c"
#if !FELUCCA_DARK                     /* (STRANGE: TRIO, DRUM, SLICE dropped; their numbers stay, played by none) */
#include "eng_trio.c"
#endif
#include "eng_wheel.c"
#include "eng_grain.c"
#include "eng_phys.c"           /* PHYS: DaisySP physical models (phys_dsp.c, MIT) */
#include "eng_drum.c"           /* DRUM: the 8-lane kit (drum_voice.c) */
#include "eng_noise.c"
#include "eng_fm6.c"            /* FM6: 6-operator FM, msfa ported (fm6_core.c, Apache-2.0) */
#include "fm4_convert.c"        /* DIGITAL's tables, and its sounds -> FM6 */
#if FELUCCA_FM4
#include "eng_digital.c"        /* DIGITAL: four-operator FM (retired; FELUCCA_FM4=1 builds it) */
#endif
#if FELUCCA_SLICE
#include "eng_slice.c"
#endif
#include "eng_gurdy.c"          /* GURDY: the hurdy-gurdy (from GHOULBOX, GPL-3.0) */
#if FELUCCA_DARK
#include "strange_dsp.c"        /* STRANGE: the shared pieces of its engines */
#include "eng_lyra.c"
#include "eng_chaos.c"
#include "eng_string.c"
#include "eng_modal.c"
#include "eng_blow.c"
#include "eng_reso.c"
#include "eng_glitch.c"
#include "eng_junk.c"
#include "eng_drift.c"
#include "eng_plant.c"
#endif
#if FELUCCA_MONOFM
#include "eng_mono.c"           /* MONOFM: the Monomachine-style machines */
#endif

/* the editor protocol, user presets and projects store these indices: append, never reorder. MONOFM: Felucca's own
 * engines are not in it (FEL: the reserved stub plays their numbers, silent; their code drops out unreferenced) */
#if FELUCCA_MONOFM
#define FEL(e) (&ENG_FM4_GONE)
#define FELREF(e) ((const engine_t *)0)  /* (a test for one of them elsewhere: never, and no reference keeps its code) */
#else
#define FEL(e) (&(e))
#define FELREF(e) (&(e))
#endif
static const engine_t *const ENGINES[NENGINES] = {
    FEL(ENG_ANALOG),                 /* 0 */
#if FELUCCA_FM4
    &ENG_DIGITAL,                /* 1 (ENGI_DIGITAL) */
#else
    &ENG_FM4_GONE,               /* 1: reserved (DIGITAL, retired: its sounds convert to FM6, fm4_convert.c) */
#endif
    FEL(ENG_PHASE),                  /* 2 */
    FEL(ENG_LOFI),                   /* 3 */
    FEL(ENG_SAMPLE),                 /* 4 */
    FEL(ENG_FORMANT),                /* 5 VOICE (eng_formant.c: "voice" is a sounding note in voice.c) */
#if FELUCCA_DARK
    &ENG_FM4_GONE,               /* 6: TRIO (not in STRANGE) */
#else
    &ENG_TRIO,                   /* 6 */
#endif
    FEL(ENG_WHEEL),                  /* 7 */
    FEL(ENG_GRAIN),                  /* 8 */
    FEL(ENG_PHYS),                   /* 9 (ENGI_PHYS) */
#if FELUCCA_DARK
    &ENG_FM4_GONE,               /* 10: DRUM (not in STRANGE; its code drops out unreferenced) */
#else
    &ENG_DRUM,                   /* 10 (ENGI_DRUM) */
#endif
    FEL(ENG_NOISE),                  /* 11 */
    FEL(ENG_FM6),                    /* 12 (ENGI_FM6) */
#if FELUCCA_SLICE
    &ENG_SLICE,                  /* 13 (FELUCCA_SLICE=0 builds without it) */
#elif FELUCCA_DARK
    &ENG_FM4_GONE,               /* 13: SLICE (not in STRANGE; GURDY's 14 and STRANGE's 15..19 stay) */
#endif
    FEL(ENG_GURDY),                  /* 14 (ENGI_GURDY) */
#if FELUCCA_DARK
    &ENG_LYRA,                   /* 15 (ENGI_LYRA) */
    &ENG_CHAOS,                  /* 16 (ENGI_CHAOS) */
    &ENG_STRING,                 /* 17 (ENGI_STRING) */
    &ENG_MODAL,                  /* 18 (ENGI_MODAL) */
    &ENG_BLOW,                   /* 19 (ENGI_BLOW) */
    &ENG_RESO,                   /* 20 (ENGI_RESO) */
    &ENG_GLITCH,                 /* 21 (ENGI_GLITCH) */
    &ENG_JUNK,                   /* 22 (ENGI_JUNK) */
    &ENG_DRIFT,                  /* 23 (ENGI_DRIFT) */
    &ENG_RITUAL,                 /* 24 (ENGI_RITUAL) */
    &ENG_MECH,                   /* 25 (ENGI_MECH) */
    &ENG_BIO,                    /* 26 (ENGI_BIO) */
    &ENG_ERROR,                  /* 27 (ENGI_ERROR) */
    &ENG_PLANT,                  /* 28 (ENGI_PLANT) */
#endif
#if FELUCCA_MONOFM
    &ENG_MSAW,                   /* 29 (ENGI_MSAW) */
    &ENG_MPULSE,                 /* 30 */
    &ENG_MENS,                   /* 31 */
    &ENG_MSID,                   /* 32 */
    &ENG_MWAVE,                  /* 33 */
    &ENG_MDDRAW,                 /* 34 */
    &ENG_MBBOX,                  /* 35 (eng_glitch.c) */
    &ENG_MFMST,                  /* 36 */
    &ENG_MFMPAR,                 /* 37 */
    &ENG_MFMDYN,                 /* 38 */
    &ENG_MVO,                    /* 39 */
    &ENG_MSINE,                  /* 40 */
    &ENG_MNOISE,                 /* 41 (ENGI_MNOISE) */
#endif
};

/* a track's engine number as an index (the audio paths: a compare, cheaper than % NENGINES; a bad number: 0) */
static inline uint32_t eng_idx(uint32_t e) { return e < NENGINES ? e : 0u; }

/* part t's engine memory goes to its engine (t->engine; voice.c engine_block, once a block: on a switch, after
 * the old engine's fade, before any note on the new one): zeroed (its engine_t.mem bytes) when another
 * engine's state is in it. An engine without memory leaves it as it is: a switch back finds its state. */
static void eng_mem_claim(const track_t *t)
{
    uint32_t p = (uint32_t)(t - trk), e, k, n;
    uint32_t *w;
    if (p >= NLIVE)
        return;
    e = eng_idx(t->engine);
    n = ENGINES[e]->mem;
    if (!n || eng_mem_owner[p] == e + 1u)
        return;
    w = (uint32_t *)ENG_MEM(p);
    for (k = 0; k < (n + 3u) / 4u; k++)
        w[k] = 0;
    eng_mem_owner[p] = (uint8_t)(e + 1u);
}

/* the sample source a track's sound plays that has no data (SAMPLE's SET, GRAIN's SRC, SLICE's SRC: an empty or
 * invalid user slot, a set or PIANO this build lacks): its name (it plays a sine: eng_sample.c smp_sine), 0 = none;
 * *code a small number for it (1.., per engine and source) so the UI says it once (ui_input.c sample_notice) */
static uint8_t snd_said[NTRK];   /* per track: the code last said, 0 = none (a project load clears them: said again) */
static const char *snd_missing(const track_t *t, uint32_t *code)
{
    const engine_t *e = ENGINES[eng_idx(t->eng_req)];
    uint32_t si = (uint32_t)t->p[P_E0] % SMP_NALL;
#if FELUCCA_SLICE
    if (e == &ENG_SLICE) {
        si = slc_src_of(t->p);
        *code = 64u + si;
        return slc_get(si) ? 0 : N_SLC_SRC[si];
    }
#endif
#if FELUCCA_DARK
    if (e == &ENG_GLITCH || e == &ENG_RITUAL || e == &ENG_MECH || e == &ENG_BIO || e == &ENG_ERROR
#if FELUCCA_MONOFM
        || e == &ENG_MBBOX
#endif
        ) {   /* KIT: a user slot */
        uint32_t k = (uint32_t)clamp(t->p[P_FM3_REL], 0, SMP_USER_SLOTS);
        *code = 96u + k;
        return k && !usr_nz[k - 1u] ? SMP_ALL_NAMES[SMP_NSETS + k - 1u] : 0;
    }
#endif
    if (e != FELREF(ENG_SAMPLE) && e != FELREF(ENG_GRAIN))
        return 0;
    *code = (e == FELREF(ENG_GRAIN) ? 32u : 1u) + si % 32u;
    return smp_set_missing(si) ? SMP_ALL_NAMES[si] : 0;
}

/* the order the engines are shown in (PRESETS browsing and its ENG knob, the EDIT layer's keys, the editor's list):
 * engine indices, never DIGITAL's reserved 1 (with FELUCCA_FM4 it follows FM6). The indices stay as they are (the
 * stores and the protocol hold them); only this table orders them */
#if FELUCCA_MONOFM
static const uint8_t ENGINE_ORDER[] = {                 /* MONOFM: the EDIT layer's three pages (MF_PAGE: a page a
                                                         * family, its machines on the white keys from F3; none of
                                                         * Felucca's engines) */
    ENGI_MSAW, ENGI_MPULSE, ENGI_MENS, ENGI_MSID,              /* 1 MONO: SWAVE, SID, */
    ENGI_MWAVE, ENGI_MDDRAW, ENGI_MBBOX,                       /* DIGIPRO, */
    ENGI_MFMST, ENGI_MFMPAR, ENGI_MFMDYN,                      /* FM+, */
    ENGI_MVO, ENGI_MSINE, ENGI_MNOISE,                         /* VO-6, GND */
    ENGI_STRING, ENGI_MODAL, ENGI_BLOW, ENGI_RESO,             /* 2 SOMA: physical models, */
    ENGI_JUNK, ENGI_LYRA, ENGI_CHAOS, ENGI_DRIFT, ENGI_PLANT,  /* springs, drones, the organic */
    ENGI_GLITCH, ENGI_ERROR, ENGI_RITUAL, ENGI_MECH, ENGI_BIO, /* 3 SLICE: the slice machines */
};
#define MF_PAGE_2 13u                                    /* the pages' first places in ENGINE_ORDER, then its end */
#define MF_PAGE_3 22u
static const uint8_t MF_PAGE[4] = {0, MF_PAGE_2, MF_PAGE_3, NENG_SHOWN};
_Static_assert(sizeof ENGINE_ORDER == NENG_SHOWN && MF_PAGE_2 <= 15u && MF_PAGE_3 - MF_PAGE_2 <= 15u &&
               NENG_SHOWN - MF_PAGE_3 <= 15u, "MONOFM: three pages of at most 15 machines (the white keys)");
static const char *const MF_PAGE_NAME[3] = {"MONO", "SOMA", "SLICE"};
/* MONOFM v4: the SELECT knob on EDIT, each machine's most used value (its E index; the machine's own desc names it):
 * SAW's unison level, PULSE's width, ENS's wave, SID's width, WAVE's morph, DDRAW's mix, the FM machines' first depth,
 * VO-6's vowel morph, SINE's fold, NOISE's colour, the physical models' brightness or breath, the slice machines'
 * slice. 0xFF: none */
static uint8_t mf_sel_e(uint32_t e)
{
    switch (e % NENGINES) {
    case ENGI_MSAW: return 0;
    case ENGI_MPULSE: return 4;
    case ENGI_MENS: return 3;
    case ENGI_MSID: return 1;
    case ENGI_MWAVE: return 1;
    case ENGI_MDDRAW: return 2;
    case ENGI_MBBOX: return 0;
    case ENGI_MFMST: return 1;
    case ENGI_MFMPAR: return 1;
    case ENGI_MFMDYN: return 0;
    case ENGI_MVO: return 5;
    case ENGI_MSINE: return 0;
    case ENGI_MNOISE: return 1;
    case ENGI_STRING: return 4;
    case ENGI_MODAL: return 2;
    case ENGI_BLOW: return 1;
    case ENGI_RESO: return 1;
    case ENGI_JUNK: return 1;
    case ENGI_LYRA: return 2;
    case ENGI_CHAOS: return 1;
    case ENGI_DRIFT: return 2;
    case ENGI_PLANT: return 1;
    case ENGI_GLITCH: case ENGI_ERROR: case ENGI_RITUAL: case ENGI_MECH: case ENGI_BIO: return 0;
    default: return 0xFFu;
    }
}
#elif FELUCCA_STRANGE
static const uint8_t ENGINE_ORDER[] = {                 /* STRANGE: the EDIT layer's two pages (ui_layer.c SE_P1), the
                                                         * machines on the keys from F3, four a row */
    ENGI_STRING, ENGI_MODAL, ENGI_BLOW, ENGI_RESO,             /* page 1, darkmachine's own: physical models, */
    ENGI_JUNK, ENGI_LYRA, ENGI_CHAOS, ENGI_DRIFT,              /* springs and drones, */
    ENGI_GLITCH, ENGI_ERROR, ENGI_RITUAL, ENGI_MECH,           /* the slice machines, */
    ENGI_BIO, ENGI_PLANT,                                      /* the organic */
    ENGI_PHYS, ENGI_GURDY, 8, 11,                              /* page 2, Felucca's: PHYS GURDY GRAIN NOISE, */
    0, 2, ENGI_FM6, 7,                                         /* ANALOG PHASE FM6 WHEEL, */
    5, 3,                                                      /* VOICE LOFI (no SAMPLE, TRIO, DRUM, SLICE) */
};
#else
static const uint8_t ENGINE_ORDER[NENG_SHOWN] = {
    0,                           /* ANALOG */
    12,                          /* FM6 */
#if FELUCCA_FM4
    1,                           /* DIGITAL */
#endif
    2, 3, 4, 5, 6, 7, 8, 9,      /* PHASE LOFI SAMPLE VOICE TRIO WHEEL GRAIN PHYS */
    11,                          /* NOISE */
#if FELUCCA_SLICE
    13,                          /* SLICE */
#endif
    14,                          /* GURDY */
    10,                          /* DRUM */
};
#endif

/* the engines one can pick (engine 1 only with FELUCCA_FM4), in ENGINE_ORDER: eng_ok(e), the n-th of them
 * eng_vis(n), e's place among them eng_rank(e), the next / previous one eng_step(e, dir) (wraps) */
#if FELUCCA_DARK
static int eng_ok(uint32_t e)                    /* STRANGE: only the engines it shows (a stored sound of another still
                                                  * plays: its engine number is kept) */
{
    uint32_t n;
    for (n = 0; n < NENG_SHOWN; n++)
        if (ENGINE_ORDER[n] == e)
            return 1;
    return 0;
}
#else
static int eng_ok(uint32_t e) { return e < NENGINES && (FELUCCA_FM4 || e != ENGI_DIGITAL); }
#endif
static uint32_t eng_vis(uint32_t n) { return ENGINE_ORDER[n % NENG_SHOWN]; }
static uint32_t eng_rank(uint32_t e)
{
    uint32_t n;
    if (!eng_ok(e))
        e = FELUCCA_DARK ? ENGINE_ORDER[0] : ENGI_FM6;   /* (DIGITAL without FELUCCA_FM4: its sounds play as FM6;
                                                             * STRANGE: a hidden engine's place is the first) */
    for (n = 0; n < NENG_SHOWN && ENGINE_ORDER[n] != e; n++)
        ;
    return n < NENG_SHOWN ? n : 0u;
}
static uint32_t eng_step(uint32_t e, int32_t dir)
{
    return eng_vis((eng_rank(e % NENGINES) + (dir > 0 ? 1u : NENG_SHOWN - 1u)) % NENG_SHOWN);
}

/* factory sequence patterns: SAVE > PHRASES loads one into the selected track (ui.c pat_load); a preset
 * only suggests one with PAT(n), loading a sound never touches the steps. Absolute notes, loaded as they
 * are (DRUM plays them as GM drums, SLICE as slices; SCL TRANS and OCT transpose): 0 = rest;
 * flags 1 = accent, 2 = slide, 4 = tie (holds the previous note). Names: at most 8 characters */
#define T_ 4
static const struct {
    const char *name;
    uint8_t note[16], flags[16];
} PATTERNS[] = {
    {"ACID", {45, 45, 57, 45, 0, 48, 45, 55, 45, 0, 57, 52, 45, 48, 0, 50},          /* 1 */
     {1, 0, 2, 0, 0, 0, 1, 2, 0, 0, 1, 0, 0, 2, 0, 1}},
    {"OFFBEAT", {0, 36, 0, 36, 0, 36, 0, 48, 0, 36, 0, 36, 0, 39, 0, 43},          /* 2 bass */
     {0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0}},
    {"MELODY", {60, 0, 67, 0, 72, 67, 0, 64, 62, 0, 69, 0, 74, 69, 0, 67},         /* 3 pluck */
     {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}},
    {"LEAD", {72, 0, 0, 74, 0, 0, 76, 0, 79, 0, 76, 0, 74, 0, 0, 0},               /* 4 */
     {1, T_, 0, 2, T_, 0, 0, 0, 1, 0, 2, 0, 0, T_, T_, 0}},
    {"PAD", {60, 0, 0, 0, 0, 0, 0, 0, 57, 0, 0, 0, 55, 0, 0, 0},                   /* 5 long notes */
     {0, T_, T_, T_, T_, T_, T_, 0, 0, T_, T_, 0, 0, T_, T_, 0}},
    {"KEYS", {0, 0, 60, 0, 0, 63, 0, 0, 0, 0, 60, 0, 0, 65, 0, 63},                /* 6 offbeat stabs */
     {0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0}},
    {"BELL", {72, 0, 0, 79, 0, 0, 84, 0, 0, 0, 76, 0, 0, 0, 0, 0},                 /* 7 sparse */
     {1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {"SUB", {36, 0, 0, 0, 0, 0, 0, 36, 0, 0, 34, 0, 0, 0, 0, 0},                   /* 8 low, held */
     {1, T_, T_, T_, 0, 0, 0, 0, 0, 0, 0, T_, T_, T_, 0, 0}},
    /* SLICE (eng_slice.c): note = C4 + slice */
    {"CHOP", {60, 61, 62, 67, 64, 65, 60, 69, 68, 70, 62, 67, 72, 72, 74, 64},     /* 9 16 slices re-ordered */
     {1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0}},
    {"STUTTER", {60, 60, 61, 61, 62, 0, 63, 63, 64, 65, 65, 0, 66, 66, 66, 67},    /* 10 8 slices, repeats */
     {1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0}},
    {"SLICES", {60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75},   /* 11 in order */
     {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}},
    /* DRUM (General MIDI: 36 kick, 38 snare, 42 closed / 46 open hi-hat) */
    {"BEAT", {36, 42, 42, 42, 38, 42, 36, 42, 36, 42, 42, 36, 38, 42, 46, 42},     /* 12 */
     {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}},
    /* 16ths up a C minor arpeggio, twice: the line the ARP presets (RAVE, ARP 8BIT, ARP LEAD) suggest
     * now that the arpeggiator is the track's and a preset no longer switches it on */
    {"ARP", {48, 51, 55, 60, 63, 67, 72, 75, 48, 51, 55, 60, 63, 67, 72, 75},       /* 13 */
     {1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0}},
};
#undef T_
#define NPATTERNS (sizeof PATTERNS / sizeof PATTERNS[0])

/* the parts at power-on (engine, preset, PATTERNS[n - 1] in the sequencer, 0 = empty: all are): bass, pad, lead, drums */
#if FELUCCA_MONOFM
/* MONOFM: T1 SAW, T2 FM-ST, T3 STRING, T4 BBOX (a kit), their first sounds, the patterns empty */
static const uint8_t TRK_DEF[NPART][3] = {{ENGI_MSAW, 0, 0}, {ENGI_MFMST, 0, 0}, {ENGI_STRING, 0, 0}, {ENGI_MBBOX, 0, 0}};
_Static_assert(sizeof ENGINE_ORDER == NENG_SHOWN, "MONOFM: NENG_SHOWN is ENGINE_ORDER's length");
#elif FELUCCA_STRANGE
/* STRANGE: T1 LYRA's first, T2 STRING's first; T3, T4 (never rendered) PHYS and GURDY, so a hidden engine is nowhere */
static const uint8_t TRK_DEF[NPART][3] = {{ENGI_LYRA, 0, 0}, {ENGI_STRING, 0, 0}, {ENGI_PHYS, 0, 0}, {ENGI_GURDY, 0, 0}};
_Static_assert(sizeof ENGINE_ORDER == NENG_SHOWN, "STRANGE: NENG_SHOWN is ENGINE_ORDER's length");
#else
static const uint8_t TRK_DEF[NPART][3] = {{0, 4, 0}, {ENGI_FM6, 4, 0}, {3, 0, 0}, {ENGI_DRUM, 0, 0}}; /* ANALOG ACID,
                                                                       * FM6 PAD (was DIGITAL PAD), LOFI PULSE LD, DRUM KIT */
#endif
static uint32_t trk_def_engine(uint32_t i) { return TRK_DEF[i % NPART][0]; }
