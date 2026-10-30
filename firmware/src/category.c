/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Sound categories (1.2, Discussion #90): every sound in the PRESETS list belongs to one, so KNOB 4 (LIST) can show
 * only the basses, the pads, .. (ui.c list_mode). Factory presets: a static table here (a letter per preset, in the
 * engine's preset order); user presets: a byte of their record (upreset.c up_cat, set on the NAME screen when saving
 * or renaming, ui_name.c). Included by ui.c and upreset.c (the host build of the user presets needs the numbers only).
 * The numbers are stored (a user preset's record, the editor's UP_LIST / UP_GET / UP_PUT): append-only. CAT_NONE is a
 * record of before (or the editor's without one): its category follows from its engine (cat_of_engine) */
#ifndef FELUCCA_CATEGORY
#define FELUCCA_CATEGORY 1
enum { CAT_NONE, CAT_BASS, CAT_LEAD, CAT_PAD, CAT_PLUCK, CAT_KEYS, CAT_DRUM, CAT_FX, CAT_OTHER, CAT_N };
static const char *const CAT_NAME[CAT_N] = {"-", "BASS", "LEAD", "PAD", "PLUCK", "KEYS", "DRUM", "FX", "OTHER"};
/* the category a sound without one is listed under: a kit (DRUM, SLICE: engines 10, 13) DRUM, any other OTHER */
static uint32_t cat_of_engine(uint32_t e) { return e == 10u || e == 13u ? CAT_DRUM : CAT_OTHER; }

#ifndef UP_HOST
/* the factory presets' categories: per engine number (engines.c ENGINES[]), a letter per preset in its order (B BASS,
 * L LEAD, P PAD, U PLUCK, K KEYS, D DRUM, F FX, O OTHER); a preset past its string (DIGITAL's, FELUCCA_FM4 builds
 * only; SAMPLE's retired PERC, past SMP_NPRESETS): its engine's (cat_of_engine). tests/ui_test.c checks every string
 * against its engine's preset count. A preset's category is a hint for browsing: nothing else reads it */
#if FELUCCA_DARK                                      /* STRANGE's texture sounds after the stock ones */
#define XCAT_ANALOG "PP"
#define XCAT_GRAIN "PPFP"
#define XCAT_PHYS "PPF"
#define XCAT_NOISE "FF"
#define XCAT_GURDY "PP"
#else
#define XCAT_ANALOG ""
#define XCAT_GRAIN ""
#define XCAT_PHYS ""
#define XCAT_NOISE ""
#define XCAT_GURDY ""
#endif
static const char *const PRESET_CAT[NENGINES > 16 ? NENGINES : 16] = {
    "LPBPBKLBULLP" XCAT_ANALOG,              /* 0 ANALOG: SAW LEAD, SOFT PAD, SQR BASS, PWM STR, ACID, SINE KEY, RAVE, SUB BASS,
                                  * PLUCK, BRASS, WIND, STRINGS */
    "",                          /* 1 DIGITAL (retired) */
    "LKPLUU",                    /* 2 PHASE: BRASS, ORGAN, STRING, RESO, BELL, WIRE */
    "LBLLL",                     /* 3 LOFI: PULSE LD, WAVE BASS, ARP 8BIT, WAVE LEAD, STEP LEAD */
    "KKLL",                      /* 4 SAMPLE: PIANO, (PIANO: the retired TRANH's alias), FLUTE, SAX */
    "PLBF",                      /* 5 VOICE: CHOIR AAH, VOX LEAD, WOW BASS, WHISPER */
    "BLLUP",                     /* 6 TRIO: FAT BASS, ARP LEAD, SYNC LEAD, RING BELL, CHIP CHOIR */
    "KKKKK",                     /* 7 WHEEL: the organs */
    "PFPP" XCAT_GRAIN,                      /* 8 GRAIN: CLOUD PAD, GLITCH, FROZEN, SHIMMER */
    "UUUPUDDPU" XCAT_PHYS,                 /* 9 PHYS: BELL TREE, MARIMBA, PLUCK, BOWED METAL, KALIMBA, HAND DRUM, TOMS, DRONE
                                  * STRING, HARP */
    "D",                         /* 10 DRUM: DRUM KIT */
    "FFFF" XCAT_NOISE,                      /* 11 NOISE: WIND, RAIN, ARCADE, METAL */
    "KUBLPUKU",                  /* 12 FM6: TINE EP, BELL, FM BASS, BRASS, PAD, MARIMBA, ORGAN, PLUCK */
    "DD",                        /* 13 SLICE: CHOP, STUTTER */
    "LPLLLL" XCAT_GURDY,                    /* 14 GURDY: HURDY GURDY, DRONE WHEEL, DANCE GURDY, TROMPETTE, VIELLE, BAGPIPE */
#if FELUCCA_DARK
    STRANGE_CAT_LYRA, STRANGE_CAT_CHAOS, STRANGE_CAT_STRING, STRANGE_CAT_MODAL, STRANGE_CAT_BLOW,   /* 15..23 (the engine
                                                                                                * files: next to their presets) */
    STRANGE_CAT_RESO, STRANGE_CAT_GLITCH, STRANGE_CAT_JUNK, STRANGE_CAT_DRIFT, STRANGE_CAT_RITUAL, STRANGE_CAT_MECH, STRANGE_CAT_BIO, STRANGE_CAT_ERROR, STRANGE_CAT_PLANT,
#endif
#if FELUCCA_MONOFM
    MONO_CAT_MSAW, MONO_CAT_MPULSE, MONO_CAT_MENS, MONO_CAT_MSID, MONO_CAT_MWAVE, MONO_CAT_MDDRAW, MONO_CAT_MBBOX,   /* 29.. */
    MONO_CAT_MFMST, MONO_CAT_MFMPAR, MONO_CAT_MFMDYN, MONO_CAT_MVO, MONO_CAT_MSINE, MONO_CAT_MNOISE,                 /* ..41 */
#endif
#if FELUCCA_SWAMP
    DR_CAT_KICK, DR_CAT_SNARE, DR_CAT_CLAP, DR_CAT_HAT, DR_CAT_CYM, DR_CAT_KIT, DR_CAT_PERC, DR_CAT_BELL, DR_CAT_RIM, DR_CAT_SHKR, DR_CAT_BELS, DR_CAT_TAMB,   /* 42..53 */
    PX_CAT_MACID, DR_CAT_ZAP, PX_CAT_CLOUD, PX_CAT_WEST, PX_CAT_ORGAN, DR_CAT_TOM, DR_CAT_SMP,                                                        /* 54.. (eng_px.c, eng_dr.c) */
#endif
};
static uint32_t preset_cat(uint32_t e, uint32_t k)    /* factory preset k of engine e */
{
    static const char L[] = "?BLPUKDFO";               /* (a letter's index is its CAT_*) */
    const char *s = e < NELEM(PRESET_CAT) && PRESET_CAT[e] ? PRESET_CAT[e] : "";
    uint32_t i, c;
    for (i = 0; i < k && s[i]; i++)
        ;
    for (c = 1; s[i] && L[c] && L[c] != s[i]; c++)
        ;
    return s[i] && L[c] ? c : cat_of_engine(e);
}
#endif
#endif
