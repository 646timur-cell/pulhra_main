/* SPDX-License-Identifier: GPL-3.0-only */
/* PULHRA's boot splash (Tim, 24.10: slower, smoother, starting minimal). t: 0..1000 over SM_SPLASH_MS (sm_main.c; the emulator,
 * web/emu/pulhra_web.c, the same). The story:
 *   0 ..  140  nothing but a block cursor blinking at the left of the screen's middle (a terminal waking up);
 *  90 ..  230  a thin line drawn out from the middle to both edges (a CRT coming on), then gone;
 * 180 ..  620  the rain: pixel characters falling in twenty columns over the top 150 rows, each column fading in at its own
 *              moment, falling smoothly by the pixel (35 .. 95 px a second), head white, a trail that greys out, the
 *              characters changing slowly; CRT scanlines over it;
 * 330 ..  600  "pulhra" (scale 4) decoded over the rain on a black shadow: each letter a run of noise characters that settles,
 *              one after another; afterwards a rare, slight glitch (a frame of the word knocked two pixels sideways);
 * 480 ..  960  the terminal below: the systems coming up line by line (dots typed, then OK, or a figure), a bar under the
 *              title filling as they do; the block cursor on the line being typed;
 * 960 .. 1000  everything held.
 * Drawn on the canvas in two bands of 240 x 120 (gfx.c CV_MAX), each blitted whole. Deterministic in t (a hash, no state):
 * any frame can be drawn alone (the first at power-on, before the rest is up). */
#ifndef FELUCCA_VERSION
#define FELUCCA_VERSION "v1.5.1"
#endif
#define SM_SPLASH_MS 2600u                                /* (sm_main.c, the emulator) */
#define SPL_COLS 20u                                      /* the rain: 12 px columns */
#define SPL_RAIN_H 150
#define SPL_TERM_Y 150
#define SPL_GREY1 0xB596u                                 /* (SM_WHITE_DIM) */
#define SPL_GREY2 0x8410u
#define SPL_GREY3 0x4208u                                 /* (SM_DIM_INK) */

static uint32_t spl_hash(uint32_t a, uint32_t b)
{
    uint32_t h = a * 2654435761u ^ (b + 0x9E3779B9u + (a << 6) + (a >> 2));
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    return h;
}
static const char SPL_CH[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ@#$%&*+=<>/?";
static char spl_noise(uint32_t a, uint32_t b) { return SPL_CH[spl_hash(a, b) % (sizeof SPL_CH - 1u)]; }

/* the first moments: the cursor alone, then the line out from the middle */
static void spl_wake(uint32_t t, int32_t y0)
{
    if (t < 140u && ((t / 35u) & 1u) == 0u)
        cv_rect(8, 113 - y0, 12, 16, T_TEXT);
    if (t >= 90u && t < 230u) {
        int32_t w = t < 160u ? (int32_t)(t - 90u) * 240 / 70 : 240, a = t < 160u ? 0 : (int32_t)(t - 160u);
        uint16_t ink = (uint16_t)(a < 25 ? T_TEXT : a < 45 ? SPL_GREY1 : SPL_GREY3);
        cv_rect(120 - w / 2, 120 - y0, w, 1, ink);
    }
}

/* the rain: column c fades in from its own start, its head at a pixel row (smooth), the trail 14 px a character */
static void spl_rain(uint32_t t, int32_t y0)
{
    uint32_t c, i;
    char b[2] = {0, 0};
    if (t < 180u)
        return;
    for (c = 0; c < SPL_COLS; c++) {
        uint32_t hc = spl_hash(c, 77u), start = 180u + hc % 260u, sp = 35u + (hc >> 8) % 61u, len = 3u + (hc >> 16) % 6u;
        uint32_t cyc = (uint32_t)SPL_RAIN_H + len * 14u + 30u, el, age;
        int32_t head;
        if (t < start)
            continue;
        el = t - start;                                   /* (t in thousandths of SM_SPLASH_MS: px = el * sp * MS / 1e6) */
        head = (int32_t)((el * sp * (SM_SPLASH_MS / 100u) / 10000u + (hc >> 20) % 40u) % cyc) - 14;
        age = el;
        for (i = 0; i <= len; i++) {
            int32_t y = head - (int32_t)i * 14;
            uint16_t ink;
            if (y < -13 || y + 14 > SPL_RAIN_H - 4)
                continue;
            ink = (uint16_t)(i == 0u ? T_TEXT : i <= 1u ? SPL_GREY1 : i <= 3u ? SPL_GREY2 : SPL_GREY3);
            if (age < 60u)                                /* (fading in: a column starts dim) */
                ink = (uint16_t)(i == 0u ? SPL_GREY2 : SPL_GREY3);
            b[0] = spl_noise(c * 31u + (uint32_t)((y + 200) / 14), i == 0u ? t / 12u : t / 45u);
            sm_px_text((int32_t)(c * 12u), y - y0, 2u, b, ink, 0);
        }
    }
    for (i = 0; i < (uint32_t)SPL_RAIN_H; i += 3u)       /* the scanlines */
        cv_rect(0, (int32_t)i - y0, 240, 1, T_BG);
}

/* the title, decoded letter by letter over the rain, on a black shadow; then a rare slight glitch */
static void spl_title(uint32_t t, int32_t y0)
{
    static const char *const W = "pulhra";
    static const int8_t SH[8][2] = {{-2, 0}, {2, 0}, {0, -2}, {0, 2}, {-2, -2}, {2, 2}, {-2, 2}, {2, -2}};
    char b[8];
    uint32_t i, n = 0, g = spl_hash(t / 8u, 5u);
    int32_t x = (240 - 24 * 6) / 2, y = 52 - y0, dx = 0;
    uint16_t ink = T_TEXT;
    if (t < 330u)
        return;
    for (i = 0; i < 6u; i++) {                            /* letter i: noise from 330 + 30 i, settled at 420 + 30 i */
        uint32_t a = 330u + 30u * i, s = a + 90u;
        if (t < a)
            break;
        b[i] = t >= s ? W[i] : spl_noise(i, t / 10u);
        n = i + 1u;
    }
    b[n] = 0;
    if (t < 380u)
        ink = SPL_GREY1;
    if (t >= 600u && g % 23u == 0u)                       /* (a glitch: two pixels sideways for a frame, a dim ghost) */
        dx = (g >> 8) & 1u ? 2 : -2;
    for (i = 0; i < 8u; i++)                              /* (a black shadow round the letters: the rain behind the word) */
        sm_px_text(x + dx + SH[i][0], y + SH[i][1], 4u, b, T_BG, 0);
    if (dx)
        sm_px_text(x - dx, y, 4u, b, SPL_GREY3, 0);
    sm_px_text(x + dx, y, 4u, b, ink, 0);
    if (t >= 480u) {                                      /* the bar under the word: the systems coming up */
        int32_t w = t >= 940u ? 144 : (int32_t)(t - 480u) * 144 / 460;
        cv_rect(x - 2, y + 36, 148, 6, T_BG);
        cv_rect(x, y + 38, 144, 2, SPL_GREY3);
        cv_rect(x, y + 38, w, 2, T_TEXT);
    }
}

/* the terminal: the systems line by line, dots typed then the answer; the block cursor on the line being typed */
static void spl_term(uint32_t t, int32_t y0)
{
    static const char *const L[5] = {"PULHRA " FELUCCA_VERSION, "VOICES ......", "DRUMS .......", "BANKS A-D ...", "MIDI ........"};
    static const char *const R[5] = {"", " 4", " OK", " OK", " OK"};
    static const uint16_t T0[5] = {480, 580, 680, 780, 870};
    char b[24];
    uint32_t l, i, n;
    for (l = 0; l < 5u; l++) {
        int32_t y = SPL_TERM_Y + (int32_t)l * 17 - y0;
        uint32_t len, j;
        if (t < T0[l])
            break;
        n = (t - T0[l]) / 4u;                             /* (a character every 4 thousandths: ~10 ms) */
        b[0] = '>';
        b[1] = ' ';
        for (i = 0; L[l][i] && i < n && i < 17u; i++)
            b[2 + i] = L[l][i];
        len = 2u + i;
        if (!L[l][i] && n > i + 6u)                       /* (all typed, a beat later: the answer) */
            for (j = 0; R[l][j] && len < 21u; j++)
                b[len++] = R[l][j];
        b[len] = 0;
        sm_px_text(2, y, 2u, b, l == 0u ? T_TEXT : (L[l][i] ? SPL_GREY2 : SPL_GREY1), 0);
        if ((L[l][i] || n <= i + 6u) && (l == 4u || t < T0[l + 1u]) && ((t / 25u) & 1u))
            cv_rect(2 + 12 * (int32_t)len, y, 10, 14, T_TEXT);
    }
}

static void sm_splash_band(uint32_t t, int32_t y0)
{
    smd_begin(0, (uint32_t)y0, 240u, 120u, T_BG);
    spl_wake(t, y0);
    spl_rain(t, y0);
    spl_title(t, y0);
    spl_term(t, y0);
    smd_end();
}
void sm_splash_paint(uint32_t t)
{
    t = t > 1000u ? 1000u : t;
    sm_splash_band(t, 0);
    sm_splash_band(t, 120);
    lcd_sync();
}
