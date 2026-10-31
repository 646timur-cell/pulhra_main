/* SPDX-License-Identifier: GPL-3.0-only */
/* DIST's shaper (fx.c track_dist; PULHRA: each KIT lane too, eng_dr.c dkit_render). MONOFM v4's TYPE: SOFT (the clip,
 * asymmetric: a little bias, even harmonics) / HARD (flat at the rails) / FOLD (the peaks folded back) / FUZZ (hard,
 * lopsided: a strong bias, the gaps of a dying fuzz) */
#if FELUCCA_MONOFM
static inline int32_t dist_shape(uint32_t ty, int32_t u)
{
    if (ty == 1u)
        return clamp(u, -26000, 26000);
    if (ty == 2u) {                                     /* fold: the triangle of period 4 L through 0 */
        int32_t L = 24000, p = 4 * L, v = (u + L) % p;
        v = v < 0 ? v + p : v;
        return v < 2 * L ? v - L : 3 * L - v;
    }
    if (ty == 3u) {
#if FELUCCA_SWAMP
        int32_t v = u + 9000;                           /* PULHRA: the gap of a dying fuzz, a dead zone around the crossing */
        int32_t a = v < 0 ? -v : v;
        v = (int32_t)(((int64_t)v * a) / (a + 6000));    /* (a soft gate: the small swings sink, the big ones pass: no corner) */
        return clamp(v * 2, -14000, 30000) - 8000;
#else
        return clamp(u + 9000, -14000, 30000) - 8000;
#endif
    }
    return softclip(u);
}
#endif

#if FELUCCA_SWAMP
/* PULHRA (Tim, 31.10: "the distortions sound doubtful"; measured: a 1.76 kHz sine through HARD / FUZZ at full drive kept its
 * aliases at -36 dB, FOLD at -19 dB, an offset of up to -2400 from the lopsided clips, FOLD 11 dB quieter at full drive).
 * Now: the low cut (LOW CUT) -> the drive (1x..9x; HARD 1.5x that) -> the shaper at twice the rate (the input
 * between two samples their mean, the shaped pair through a 1-2-1 before the decimation: HARD / FUZZ ~10 dB cleaner,
 * FOLD ~9 dB) -> the offset out (~14 Hz) -> the tone low-pass that closes with the drive (TONE) -> the make-up (OUT),
 * each TYPE as loud as SOFT across the drive. p: the track's TYPE TONE OUT LOW CUT; d: the drive (DIST, or a lane's) */
static __attribute__((noinline)) void dist_run(dist_st_t *s, const int16_t *p, int32_t d, int32_t *b, uint32_t n)
{
    static const uint8_t SQ[128] = {   /* sqrt(d / 127) x 128 */
        0, 11, 16, 20, 23, 25, 28, 30, 32, 34, 36, 37, 39, 41, 42, 44, 45, 46, 48, 49, 50, 52, 53, 54, 55, 56, 57, 59, 60, 61,
        62, 63, 64, 65, 66, 67, 68, 69, 70, 70, 71, 72, 73, 74, 75, 76, 77, 77, 78, 79, 80, 81, 81, 82, 83, 84, 84, 85, 86, 87,
        87, 88, 89, 90, 90, 91, 92, 92, 93, 94, 94, 95, 96, 96, 97, 98, 98, 99, 100, 100, 101, 102, 102, 103, 103, 104, 105, 105,
        106, 106, 107, 108, 108, 109, 109, 110, 111, 111, 112, 112, 113, 113, 114, 115, 115, 116, 116, 117, 117, 118, 118, 119,
        119, 120, 121, 121, 122, 122, 123, 123, 124, 124, 125, 125, 126, 126, 127, 128};
    uint32_t ty = (uint32_t)clamp(p[P_DTYPE], 0, 3), i;
    int32_t g, k, mk, bias = 2400, b0, hc = 256 + p[P_DLCUT] * p[P_DLCUT] / 2;
    d = clamp(d, 0, 127);
    if (!d)
        return;
    g = 4096 + d * d * 2;                                /* Q12: 1x .. ~9x, gentle at first */
    if (ty == 1u)
        g += g >> 1;                                     /* (HARD: half again: flat-topped sooner, brighter than SOFT) */
    k = clamp(32000 - d * 95 * (ty == 3u ? 2 : 1) + p[P_DTONE] * 300, 1500, 32700);   /* (FUZZ: woollier, its tone closes twice as far) */
    mk = (30000 - d * 120) * (64 + clamp(p[P_DOUT], -64, 63)) / 64;
    mk = mk * (128 + (ty == 1u ? 26 * SQ[d] / 128 : ty == 3u ? 54 * SQ[d] / 128 : ty == 2u ? 154 * SQ[d] / 128 : 0)) / 128;
    b0 = dist_shape(ty, bias);
    for (i = 0; i < n; i++) {
        int32_t x = b[i], acc, y, j, xi;
        s->hp += p[P_DLCUT] ? mulq15(clamp(x - s->hp, -250000, 250000), hc) : (x - s->hp + 64) >> 7;   /* (LOW CUT) */
        x = clamp(x - s->hp, -230000, 230000);
        for (j = 0, acc = 0; j < 2; j++) {
            xi = j ? x : (s->x1 + x) >> 1;
            y = dist_shape(ty, (((xi >> 2) * g) >> 10) + bias) - b0;
            acc += (s->d2 + 2 * s->d1 + y) >> 2;
            s->d2 = s->d1;
            s->d1 = y;
        }
        s->x1 = x;
        y = acc >> 1;
        s->dc += (y - s->dc) >> 9;                       /* (the lopsided clips' offset out, ~14 Hz) */
        y -= s->dc;
        s->lp1 += mulq15(y - s->lp1, k);                /* two poles: tames the fizz */
        s->lp2 += mulq15(s->lp1 - s->lp2, k);
        b[i] = mulq15(s->lp2, mk);
    }
}
#endif
