/* SPDX-License-Identifier: GPL-3.0-only */
/* SWAMP MACHINE pixel primitives on the canvas (gfx.c cv_rect): the 5 x 7 text of sm_font.c and the knob ring. One ink on
 * the ground, no smoothing; an inverted text is the ground colour on an ink box. Coordinates are the canvas's. */
#ifndef SM_PX_HOOK
#define SM_PX_HOOK(x0, y0, x1, y1, s, flags) ((void)0)    /* (the host test records the ink boxes) */
#endif
#define SM_DIM_INK 0x4208u                                /* the unlit dots, the empty steps */

static uint32_t sm_px_glyph(uint8_t c)
{
    if (c >= 'a' && c <= 'z')
        c = (uint8_t)(c - 32u);
    if (c >= 32u && c <= 95u)
        return c - 32u;
    if (c >= 0x10u && c <= 0x12u)
        return 64u + (c - 0x10u);
    return 0;
}

/* s at x, y (the glyph's top left), scale sc: glyph 5 x 7, pitch 6 x sc; inv: the ground colour on an ink box (one sc of margin
 * each side and above / below); returns the width, 6 * sc * length */
static int32_t sm_px_text(int32_t x, int32_t y, uint32_t sc, const char *s, uint16_t ink, int inv)
{
    int32_t n = 0, i, c, r, k = (int32_t)sc, w;
    uint16_t fg = inv ? T_BG : ink;
    while (s[n])
        n++;
    w = 6 * k * n;
    if (inv)
        cv_rect(x - k, y - k, w + k, 9 * k, ink);
    for (i = 0; i < n; i++) {
        const uint8_t *gl = SM_FONT[sm_px_glyph((uint8_t)s[i])];
        for (c = 0; c < 5; c++) {
            for (r = 0; r < 7; r++) {                     /* a vertical run of set bits is one rect */
                int32_t r0 = r;
                if (!((gl[c] >> r) & 1u))
                    continue;
                while (r + 1 < 7 && ((gl[c] >> (r + 1)) & 1u))
                    r++;
                cv_rect(x + (i * 6 + c) * k, y + r0 * k, k, (r - r0 + 1) * k, fg);
            }
        }
    }
    SM_PX_HOOK(x, y, x + w, y + 7 * k, s, (x < 0 || y < 0 || x + w > (int32_t)cv_w || y + 7 * k > (int32_t)cv_h) ? 1u : 0u);
    return w;
}

/* the ring: 16 dots of 3 x 3 on an arc of 270 degrees from 225 to -45 (lower left, over the top, lower right) around cx, cy
 * at radius r; the first 1 + pos256 * 15 / 256 of them lit (pos256 0..256), the rest dim. filled: a solid disc inside the
 * dots (a locked value); lockdot: a 3 x 3 dot in the centre (the parameter is locked on some other step); dim (a muted
 * track's level on MIXER): the lit dots dim, the rest not drawn, a dotted ring with no ink */
static const int16_t SM_RING_COS[16] = {-181, -228, -253, -253, -228, -181, -116, -40, 40, 116, 181, 228, 253, 253, 228, 181};
static const int16_t SM_RING_SIN[16] = {-181, -116, -40, 40, 116, 181, 228, 253, 253, 228, 181, 116, 40, -40, -116, -181};
static int32_t sm_ring_off(int32_t r, int32_t t)          /* r * t / 256, rounded away from zero */
{
    return (r * t + (t < 0 ? -128 : 128)) / 256;
}
static void sm_px_ring(int32_t cx, int32_t cy, int32_t r, uint32_t pos256, int filled, int lockdot, int dim)
{
    int32_t i, lit, dy, rr = r - 5;
    if (pos256 > 256u)
        pos256 = 256u;
    lit = 1 + (int32_t)(pos256 * 15u / 256u);
    if (filled)
        for (dy = -rr; dy <= rr; dy++) {                  /* the disc, row by row */
            int32_t hw = 0;
            while ((hw + 1) * (hw + 1) + dy * dy <= rr * rr)
                hw++;
            cv_rect(cx - hw, cy + dy, 2 * hw + 1, 1, T_TEXT);
        }
    for (i = 0; i < 16 && (!dim || i < lit); i++)
        cv_rect(cx + sm_ring_off(r, SM_RING_COS[i]) - 1, cy - sm_ring_off(r, SM_RING_SIN[i]) - 1, 3, 3,
                i < lit && !dim ? T_TEXT : (uint16_t)SM_DIM_INK);
    if (lockdot)
        cv_rect(cx - 1, cy - 1, 3, 3, filled ? T_BG : T_TEXT);
}
