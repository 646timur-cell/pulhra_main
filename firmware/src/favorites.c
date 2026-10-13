/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
#define FELUCCA_FAVORITES 1
/* Stable engine/preset references; fixed capacity independent of optional engines. */
typedef struct {
    uint8_t factory[16][32]; /* engine 0..15, preset 0..255 */
    uint32_t user, filter;
} favorites_t;
static favorites_t favorites __attribute__((section(".pool")));
/* the byte holding factory preset `preset`'s star, 0 = none. Row 14 is MIDI LEARN's (midi_control.c ml_tab), so GURDY
 * (engine 14, sound-module build) keeps its stars in row 15's bytes 0..7 (64 presets; no setting uses them: bytes 8, 9
 * hold the tracks' MIDI channels, ui.c ui_mch; 10.. the others, settings_persist.c) */
static uint8_t *fav_byte(uint32_t engine, uint32_t preset)
{
    if (engine == ENGI_GURDY)
        return preset < 64u ? &favorites.factory[15][preset / 8u] : 0;
#if FELUCCA_STRANGE
    /* STRANGE's engines 15..19: row 1 (DIGITAL's, retired: never shown, never starred), 6 bytes (48 presets) each */
    if (engine >= ENGI_LYRA && engine <= ENGI_BLOW)
        return preset < 48u ? &favorites.factory[1][(engine - ENGI_LYRA) * 6u + preset / 8u] : 0;
    /* 20..23 (RESO GLITCH JUNK DRIFT): the rows of the engines STRANGE dropped, TRIO DRUM SLICE SAMPLE (never shown),
     * their first 16 bytes (128 presets); 24, 25, 26 (RITUAL MECH BIO): the second halves of TRIO's, DRUM's, SLICE's rows */
    if (engine >= ENGI_RESO && engine <= ENGI_DRIFT) {
        static const uint8_t ROW[4] = {6, 10, 13, 4};
        return preset < 128u ? &favorites.factory[ROW[engine - ENGI_RESO]][preset / 8u] : 0;
    }
    if (engine >= ENGI_RITUAL && engine <= ENGI_BIO) {
        static const uint8_t ROW2[3] = {6, 10, 13};
        return preset < 128u ? &favorites.factory[ROW2[engine - ENGI_RITUAL]][16u + preset / 8u] : 0;
    }
#endif
    return engine < NENGINES && engine < 14u && preset < 256u ? &favorites.factory[engine][preset / 8u] : 0;
}
static int favorite_has(uint32_t engine, uint32_t preset)
{
    const uint8_t *b;
    if (engine == NENGINES)
        return preset < UP_SLOTS && ((favorites.user >> preset) & 1u);
    b = fav_byte(engine, preset);
    return b && ((*b >> (preset % 8u)) & 1u);
}
static int favorite_set(uint32_t engine, uint32_t preset, int on)
{
    if (engine > NENGINES || (engine == NENGINES ? preset >= UP_SLOTS : !fav_byte(engine, preset)))
        return 0;
    if (favorite_has(engine, preset) == !!on) return 0;
    if (engine == NENGINES) {
        if (on) favorites.user |= 1u << preset;
        else favorites.user &= ~(1u << preset);
    } else {
        uint8_t *b = fav_byte(engine, preset);
        if (on) *b |= (uint8_t)(1u << (preset % 8u));
        else *b &= (uint8_t)~(1u << (preset % 8u));
    }
    return 1;
}
