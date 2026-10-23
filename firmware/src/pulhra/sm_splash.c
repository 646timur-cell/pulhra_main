/* SPDX-License-Identifier: GPL-3.0-only */
/* PULHRA's boot splash: a terminal / glitch screen. Over the top 150 rows a rain of pixel characters falls in all twenty
 * columns (behind the title too): each column its own speed, offset and trail, the head white, the next two the light grey,
 * the rest the dim grey, the characters changing as they fall; CRT scanlines over it. "pulhra" (scale 4) is typed in over the
 * rain on a black shadow, and glitches now and then (the word jumps sideways for a frame with a dim ghost the other way, one
 * letter swapped for noise). Below, a terminal: four boot lines typed one after another (> PULHRA v1.5.1, VOICES, BANKS,
 * MIDI ... OK) with a blinking block cursor and an ASCII spinner. t: 0..100 the progress (sm_main.c over 1.5 s; the
 * emulator, web/emu/pulhra_web.c, the same). Drawn on the canvas in two bands of 240 x 120 (gfx.c CV_MAX), each blitted
 * whole: no fill of the whole screen, nothing past the canvas. Deterministic in t (a hash, no state): any frame can be drawn
 * alone (the first one at power-on, before the rest is up). */
#ifndef FELUCCA_VERSION
#define FELUCCA_VERSION "v1.5.1"
#endif
#define SPL_COLS 20u                                      /* the rain: 12 px columns */
#define SPL_ROWS 10u                                      /* .. 14 px rows over the top 140 (the terminal below) */
#define SPL_RAIN_H 150
#define SPL_TERM_Y 154

static uint32_t spl_hash(uint32_t a, uint32_t b)
{
    uint32_t h = a * 2654435761u ^ (b + 0x9E3779B9u + (a << 6) + (a >> 2));
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    return h;
}
static const char SPL_CH[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ@#$%&*+=<>/?";

/* the rain of column c at time t: its head row (may be past the bottom: a gap before it comes round) and trail */
static void spl_rain(uint32_t t, int32_t y0)
{
    uint32_t c, i;
    char b[2] = {0, 0};
    for (c = 0; c < SPL_COLS; c++) {
        uint32_t hc = spl_hash(c, 77u), sp = 2u + hc % 5u, len = 4u + (hc >> 8) % 7u;
        int32_t head = (int32_t)(((t * sp) / 3u + (hc >> 16) % 19u) % (SPL_ROWS + len + 4u));
        for (i = 0; i <= len; i++) {
            int32_t r = head - (int32_t)i, y;
            uint16_t ink;
            if (r < 0 || r >= (int32_t)SPL_ROWS)
                continue;
            y = r * 14 + 1;
            ink = (uint16_t)(i == 0u ? T_TEXT : i <= 2u ? SM_WHITE_DIM : SM_DIM_INK);
            b[0] = SPL_CH[spl_hash(c * 31u + (uint32_t)r, i == 0u ? t : t / 6u) % (sizeof SPL_CH - 1u)];
            sm_px_text((int32_t)(c * 12u), y - y0, 2u, b, ink, 0);
        }
    }
    for (i = 0; i < (uint32_t)SPL_RAIN_H; i += 3u)       /* the scanlines */
        cv_rect(0, (int32_t)i - y0, 240, 1, T_BG);
}

/* the title, typed in by ~40 % of the time, on a black shadow; a glitch frame now and then */
static void spl_title(uint32_t t, int32_t y0)
{
    static const char *const W = "pulhra";
    char b[8];
    uint32_t i, on = t * 6u / 40u, g = spl_hash(t, 5u);
    int32_t x, y = 56 - y0, dx = 0;
    static const int8_t SH[8][2] = {{-2, 0}, {2, 0}, {0, -2}, {0, 2}, {-2, -2}, {2, 2}, {-2, 2}, {2, -2}};
    on = on > 6u ? 6u : on;
    for (i = 0; i < on; i++)
        b[i] = W[i];
    b[i] = 0;
    if (on == 6u && g % 7u == 0u) {                       /* a glitch: the word knocked sideways, a ghost, a letter of noise */
        dx = (int32_t)(g >> 8) % 2 ? 6 : -6;
        b[(g >> 12) % 6u] = SPL_CH[(g >> 16) % (sizeof SPL_CH - 1u)];
    }
    x = (240 - 24 * (int32_t)on) / 2;
    if (dx) {                                             /* (the glitch's tears: two thin bars of noise across the rain) */
        cv_rect(0, (int32_t)((g >> 4) % 140u) - y0, 240, 1, SM_WHITE_DIM);
        cv_rect((int32_t)((g >> 20) % 120u), (int32_t)((g >> 10) % 140u) - y0, 120, 2, T_TEXT);
    }
    for (i = 0; i < 8u; i++)                              /* (a black shadow round the letters: the rain shows behind the word) */
        sm_px_text(x + dx + SH[i][0], y + SH[i][1], 4u, b, T_BG, 0);
    if (dx)
        sm_px_text(x - dx, y, 4u, b, SM_DIM_INK, 0);
    sm_px_text(x + dx, y, 4u, b, T_TEXT, 0);
    if (on < 6u && ((t / 4u) & 1u))                      /* the typing cursor */
        cv_rect(x + 24 * (int32_t)on + 2, y + 20, 12, 8, T_TEXT);
}

/* the terminal: the boot lines typed one after another, the block cursor on the line being typed, the spinner */
static void spl_term(uint32_t t, int32_t y0)
{
    static const char *const L[4] = {"> PULHRA " FELUCCA_VERSION, "> VOICES ..... OK", "> BANKS A-E .. OK", "> MIDI ....... OK"};
    static const uint8_t T0[4] = {30, 48, 64, 80};       /* (when each line starts: 2.5 characters a step) */
    static const char spin[4] = {'|', '/', '-', '\\'};
    char b[24];
    uint32_t l, i, n;
    for (l = 0; l < 4u; l++) {
        int32_t y = SPL_TERM_Y + (int32_t)l * 17 - y0;
        if (t < T0[l])
            break;
        n = (t - T0[l]) * 5u / 2u;
        for (i = 0; L[l][i] && i < n && i < 19u; i++)
            b[i] = L[l][i];
        b[i] = 0;
        sm_px_text(4, y, 2u, b, l == 0u ? T_TEXT : SM_WHITE_DIM, 0);
        if (L[l][i] && ((t / 3u) & 1u))                   /* (still typing: the block cursor) */
            cv_rect(4 + 12 * (int32_t)i, y, 10, 14, T_TEXT);
        else if (!L[l][i] && l == 3u && ((t / 5u) & 1u))
            cv_rect(4, y + 17, 10, 14, T_TEXT);           /* (all typed: the prompt's cursor below) */
    }
    b[0] = spin[(t / 3u) & 3u];
    b[1] = 0;
    sm_px_text(240 - 16, SPL_TERM_Y + 3 * 17 + 17 - y0, 2u, b, T_TEXT, 0);
}

static void sm_splash_band(uint32_t t, int32_t y0)
{
    smd_begin(0, (uint32_t)y0, 240u, 120u, T_BG);
    spl_rain(t, y0);
    spl_title(t, y0);
    spl_term(t, y0);
    smd_end();
}
void sm_splash_paint(uint32_t t)
{
    t = t > 100u ? 100u : t;
    sm_splash_band(t, 0);
    sm_splash_band(t, 120);
    lcd_sync();
}
