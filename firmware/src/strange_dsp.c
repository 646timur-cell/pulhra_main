/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Tim (646timur-cell), on Felucca by Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* STRANGE build: the pieces its engines share (eng_lyra.c, eng_chaos.c, eng_string.c, eng_modal.c, eng_blow.c).
 * Fixed point as phys_dsp.c (whose px_m, px_sin, px_exp2, px_rand they use): waveguide signals Q20 (1.0 = 2^20),
 * frequencies as phase increments (cycles Q32 per sample), coefficients Q15 / Q30, 64-bit products (one MAC on the
 * pi32v2), divisions only once a block. */

#define SQ_ONE (1 << 20)                                 /* a waveguide signal of 1.0 */
#define MD_NMAT 8                                        /* MODAL's materials */
#define MD_NMODE 24                                      /* .. and its modes */

/* the partials of each MATR over the fundamental, as log2 of the ratio, Q16 (generated: STRANGE.md):
 * BAR a free bar (1 : 2.76 : 5.40 ..), MRMB a tuned bar (1 : 3.99 : 9.94, then a bar's), BELL a church bell (hum 1/2,
 * prime, minor third, fifth, nominal ..), GLASS a wine glass, PLATE a 1 : 1.41 plate, DRUM a circular membrane,
 * BOWL a ring (a singing bowl), HARM the harmonic series */
static const int32_t MODAL_LOG2[MD_NMAT][MD_NMODE] = {
    {0, 95870, 159515, 207037, 244983, 276573, 303633, 327301, 348333, 367259, 384461, 400228,   /* BAR */
     414781, 428294, 440905, 452728, 463854, 474362, 484317, 493774, 502780, 511377, 519600, 527480},
    {0, 130835, 217137, 264659, 302605, 334195, 361255, 384923, 405955, 424881, 442083, 457850,   /* MRMB */
     472403, 485916, 498527, 510350, 521476, 531984, 541939, 551396, 560402, 568999, 577222, 585102},
    {-65536, 0, 15889, 38714, 65536, 87162, 92570, 104218, 134917, 160022, 181186, 199115,   /* BELL */
     216559, 233436, 249854, 265896, 281627, 297100, 312357, 327432, 342355, 357148, 371831, 386421},
    {0, 79569, 136804, 178848, 211654, 237662, 259493, 278501, 295477, 310927, 325188, 338502,   /* GLASS */
     351045, 362949, 374318, 385231, 395755, 405942, 415836, 425473, 434884, 444094, 453127, 462001},
    {0, 65724, 103746, 123119, 131072, 164115, 169721, 174361, 188296, 196796, 207744, 208078,   /* PLATE */
     226546, 226957, 229617, 234818, 240402, 247176, 251884, 254191, 262144, 267699, 268230, 273134},
    {0, 44083, 71756, 78586, 92250, 101252, 108665, 118474, 121110, 122466, 132480, 134644,   /* DRUM */
     144307, 148938, 150318, 154615, 159656, 164043, 168330, 169140, 170411, 172630, 177353, 179090},
    {0, 98304, 159853, 205303, 241534, 271738, 297671, 320411, 340668, 358938, 375579, 390861,   /* BOWL */
     404990, 418130, 430410, 441938, 452799, 463068, 472806, 482065, 490890, 499321, 507391, 515129},
    {0, 65536, 103872, 131072, 152170, 169408, 183983, 196608, 207744, 217706, 226717, 234944,   /* HARM */
     242512, 249519, 256042, 262144, 267876, 273280, 278392, 283242, 287855, 292253, 296456, 300480},
};
/* the bow's friction: min(1, (|x| + 0.75)^-4) at |x| = 4 i / 256, Q15 (STK's BowTable, slope applied before) */
static const int16_t SX_BOW_TAB[257] = {
    32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
    32767, 30797, 28972, 27281, 25711, 24253, 22896, 21633, 20456, 19358, 18333, 17374, 16478, 15638, 14852, 14114,
    13421, 12771, 12159, 11584, 11042, 10531, 10050, 9596, 9167, 8762, 8379, 8017, 7674, 7349, 7041, 6749,
    6472, 6210, 5960, 5723, 5497, 5283, 5079, 4884, 4699, 4523, 4354, 4194, 4041, 3894, 3755, 3621,
    3494, 3372, 3255, 3143, 3036, 2934, 2835, 2741, 2651, 2565, 2482, 2402, 2325, 2252, 2181, 2113,
    2048, 1985, 1925, 1867, 1811, 1757, 1705, 1655, 1607, 1561, 1516, 1473, 1431, 1391, 1352, 1315,
    1279, 1244, 1210, 1177, 1146, 1115, 1086, 1057, 1030, 1003, 977, 952, 928, 905, 882, 860,
    839, 818, 798, 779, 760, 742, 724, 707, 690, 674, 658, 643, 628, 614, 600, 586,
    573, 560, 548, 535, 524, 512, 501, 490, 480, 469, 459, 450, 440, 431, 422, 413,
    405, 396, 388, 380, 373, 365, 358, 351, 344, 337, 330, 324, 317, 311, 305, 299,
    294, 288, 283, 277, 272, 267, 262, 257, 253, 248, 243, 239, 235, 230, 226, 222,
    218, 214, 211, 207, 203, 200, 196, 193, 190, 187, 183, 180, 177, 174, 171, 168,
    166, 163, 160, 158, 155, 153, 150, 148, 145, 143, 141, 139, 136, 134, 132, 130,
    128, 126, 124, 122, 120, 118, 117, 115, 113, 111, 110, 108, 107, 105, 103, 102,
    100, 99, 98, 96, 95, 93, 92, 91, 89, 88, 87, 86, 85, 83, 82, 81,
    80, 79, 78, 77, 76, 75, 74, 73, 72, 71, 70, 69, 68, 67, 66, 65,
    64,
};

/* a delay of one period of inc (cycles Q32 per sample), in samples Q16: 2^48 / inc (a divide: once a block) */
static inline uint32_t sx_period(uint32_t inc)
{
    return inc > 0x8000u ? (uint32_t)(((uint64_t)1 << 48) / inc) : 0xFFFFFFFFu;
}

/* a read d samples (Q16, >= 1) behind the write index w of a circular line (mask + 1 a power of two), linearly
 * interpolated */
static inline int32_t sx_read(const int32_t *b, uint32_t mask, uint32_t w, uint32_t d16)
{
    uint32_t di = d16 >> 16, f = (d16 >> 1) & 0x7FFFu;
    int32_t a = b[(w - di) & mask], c = b[(w - di - 1u) & mask];
    return a + (int32_t)(((int64_t)(c - a) * (int32_t)f) >> 15);
}

/* the bow's friction for x (velocity difference x slope, Q20): Q15, 1.0 sticking .. ~0 slipping */
static inline int32_t sx_bow(int32_t x)
{
    uint32_t a = (uint32_t)(x < 0 ? -x : x), i = a >> 14;
    int32_t y0, y1;
    if (i >= 256u)
        return SX_BOW_TAB[256];
    y0 = SX_BOW_TAB[i];
    y1 = SX_BOW_TAB[i + 1u];
    return y0 + (((y1 - y0) * (int32_t)(a & 0x3FFFu)) >> 14);
}

/* tanh for Q20 signals (softclip's table: Q20 1.0 -> tanh(1)), the result Q20 */
static inline int32_t sx_sat(int32_t x)
{
    return softclip(clamp(x >> 5, -(1 << 24), 1 << 24)) << 5;
}

/* white noise, Q20 (-1 .. 1) */
static inline int32_t sx_noise(uint32_t *s)
{
    return (int32_t)px_rand(s) >> 11;
}

/* a one-pole low-pass of k (Q15: 32767 = through) */
static inline int32_t sx_lp(int32_t *y, int32_t x, int32_t k)
{
    *y += px_m(x - *y, k, 15);
    return *y;
}

/* a slow random walk (+-limit), once a block: leaky, so it wanders and comes back */
static inline int32_t sx_walk(int32_t w, uint32_t *s, int32_t step, int32_t limit)
{
    w += px_m((int32_t)px_rand(s), step, 31);
    w -= w >> 7;
    return clamp(w, -limit, limit);
}

/* 2^(v / 127 * oct) x lo, for a knob v 0..127 spread exponentially over oct octaves from lo (both Q16): Q16 */
static uint32_t sx_expmap(int32_t v, uint32_t lo, int32_t oct)
{
    return (uint32_t)(((uint64_t)px_exp2(clamp(v, 0, 127) * oct * 65536 / 127) * lo) >> 16);
}

/* Hz (Q16) -> phase increment (cycles Q32 per sample) */
static inline uint32_t sx_hz_inc(uint32_t hz16)
{
    return (uint32_t)(((uint64_t)hz16 * 97392u) >> 16);  /* 2^32 / 44100 */
}
