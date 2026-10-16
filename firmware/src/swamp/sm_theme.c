/* SPDX-License-Identifier: GPL-3.0-only */
/* SWAMP MACHINE theme PIZDAKS (grindcore / noise): a black ground, bone text, blood-red accent, rust for the secondary,
 * thick square frames. The photocopy grain is gfx.c's cv_grime (STRANGE DARK's wear: fixed noise of the screen's own
 * pixels, so it does not shimmer), here with its own flat colours: it touches only pixels that are exactly BG, SURF or
 * RAISE, so text, its antialiased edges, the step cells and the frames stay clean. Nothing draws SURF / RAISE in SWAMP. */
#define SMT_BG 0x0000u
#define SMT_SURF 0x0841u
#define SMT_RAISE 0x1082u
#define SMT_TEXT 0xEF5Bu                                  /* bone */
#define SMT_ACCENT 0xB800u                                /* blood */
#define SMT_MID 0x8A22u                                   /* rust */
#define SMT_DIM 0x4100u
#define SMT_REC 0xF800u

static void sm_theme_pizdaks(void)
{
    const uint16_t f[3] = {SMT_BG, SMT_SURF, SMT_RAISE};
    uint32_t k;
    ux.bg = SMT_BG; ux.surf = SMT_SURF; ux.text = SMT_TEXT; ux.theme = SMT_TEXT; ux.accent = SMT_ACCENT;
    ux.mid = SMT_MID; ux.dim = SMT_DIM; ux.rec = SMT_REC;
    ux.line = SMT_DIM; ux.sel = SMT_ACCENT; ux.tint = SMT_SURF; ux.ink = SMT_BG; ux.raise = SMT_RAISE;
    ux.key = SMT_MID; ux.lane = SMT_SURF; ux.grid = SMT_SURF;
    ux.light = 0; ux.mono = 0; ux.style = ST_FLAT;
    ux.grime = 1;
    for (k = 0; k < GM_GOUGES; k++) {                     /* the long scratches across the screen */
        uint32_t g = gm_hash(k + 911u, 7u);
        gm_gouge[k][0] = (int16_t)(g % 200u);
        gm_gouge[k][1] = (int16_t)(gm_gouge[k][0] + 30 + (int32_t)((g >> 8) % 120u));
        gm_gouge[k][2] = (int16_t)((g >> 16) % 240u);
        gm_gouge[k][3] = (int16_t)((int32_t)((g >> 24) % 49u) - 24);
    }
    for (k = 0; k < 3u; k++) {                            /* dust (bone), stain (dried blood), soot, harsher than the crypt's */
        ux.gk[k] = swap16(f[k]);
        ux.gd[k][0] = swap16(ux_mix(f[k], SMT_TEXT, k ? 12 : 16));
        ux.gd[k][1] = swap16(k ? ux_mix(f[k], SMT_BG, 55) : ux_mix(f[k], SMT_ACCENT, 28));
        ux.gd[k][2] = swap16(k ? ux_mix(f[k], SMT_BG, 28) : ux_mix(f[k], SMT_TEXT, 7));
    }
    ux.gen++;                                             /* the text ramps follow the theme */
}
