/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test of SWAMP MACHINE (firmware/src/swamp.c: the MONOFM sound core under its own app, swamp/sm_*.c).
 *   CORE   the sound core alone (no seq.c, no ui*.c), from sm_core_init (the firmware's power-on): silent tracks mix to silence
 *   SETTINGS  the settings record in the flash (the learned panel table, HOLD, palette) reaches SWAMP: sm_settings_load
 *   cc -O2 -w -DFELUCCA_SWAMP=1 -DFELUCCA_MONOFM=1 -DFELUCCA_MODULE=0 -Ibuild/gen -Ifirmware/src -o build/host/swamp_test tests/swamp_test.c -lm */
#include "swamp_host.h"
/* the settings record as the firmware reads it (swamp.c: gfx, panel, settings_persist, storage on a simulated NOR) */
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c) { (void)x; (void)y; (void)w; (void)h; (void)c; }
static void lcd_sync(void) {}
static void lcd_power(uint32_t s) { (void)s; }
static void lcd_wake_now(void) {}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *p)
{ (void)x; (void)y; (void)w; (void)h; (void)p; }
static void fm1_led_key(unsigned k, int on) { (void)k; (void)on; }
#include "../firmware/src/gfx.c"
#include "../firmware/src/panel.c"
static void settings_save(void) {}
#include "../firmware/src/settings_persist.c"
static uint8_t nor[0x100000], flash_ok = 1;
static int st_read(uint32_t off, void *dst, uint32_t n) { memcpy(dst, nor + off, n); return 0; }
static int st_erase(uint32_t off) { memset(nor + off, 0xFF, 4096); return 0; }
static int st_prog(uint32_t off, const void *src, uint32_t n)
{
    const uint8_t *s = src;
    uint32_t i;
    for (i = 0; i < n; i++)
        nor[off + i] &= s[i];
    return 0;
}
#include "../firmware/src/storage.c"
#include "../firmware/src/swamp/sm_app.c"

static int check(const char *what, int ok)
{
    printf("swamp: %-74s %s\n", what, ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}

static int test_core_silent(void)
{
    int32_t out[2 * CTL];
    uint32_t b, i, peak = 0;
    sm_core_init();
    for (b = 0; b < 100u; b++) {
        mix_block(out, CTL);
        for (i = 0; i < 2u * CTL; i++) {
            uint32_t a = (uint32_t)(out[i] < 0 ? -out[i] : out[i]);
            peak = a > peak ? a : peak;
        }
    }
    printf("swamp: peak %u\n", peak);
    return check("CORE: 100 blocks of silent tracks, |x| <= 1", peak <= 1u);
}

/* a unit calibrated in Felucca: the learned panel table and the settings in the flash reach SWAMP's boot */
static int test_settings(void)
{
    persist_t w = {0};
    int bad = 0;
    memset(nor, 0xFF, sizeof nor);
    panel = PANEL_DEFAULT;
    sm_settings_load();
    panel_init();
    bad += check("SETTINGS: blank flash: the default panel table", !memcmp(&panel, &PANEL_DEFAULT, sizeof panel));
    w.magic = PERSIST_MAGIC;
    w.palette = palette_to_stored(UI_CRYPT_INDEX);
    w.bold = HOLD_TAG | 3u;                           /* HOLD 600 ms */
    w.panel = PANEL_DEFAULT;
    w.panel.enc[0] = 3; w.panel.enc[3] = 0; w.panel.enc[1] = 1;   /* a swap of two encoders: still a permutation */
    w.panel.enc[2] = 6; w.panel.enc[4] = 2; w.panel.enc[5] = 4; w.panel.enc[6] = 5;
    w.panel.dir[2] = -1;
    w.panel.btn[0] = 3; w.panel.btn[1] = 2;             /* FX and SCL swapped */
    bad += check("SETTINGS: the record saves", st_save(OBJ_SETTINGS, &w, sizeof w) == 0);
    panel = PANEL_DEFAULT; settings_hold = HOLD_DEF;
    sm_settings_load();
    settings_init();
    panel_init();
    bad += check("SETTINGS: learned panel table loaded (encoders, directions, buttons)",
                 panel.enc[0] == 3 && panel.enc[3] == 0 && panel.dir[2] == -1 && panel.btn[0] == 3 && panel.btn[1] == 2);
    bad += check("SETTINGS: HOLD and palette loaded", settings_hold == 3u && settings.palette == UI_CRYPT_INDEX);
    memset(nor + 0xFC000, 0, 64);                       /* both copies torn: the defaults */
    memset(nor + 0xFD000, 0, 64);
    panel = PANEL_DEFAULT; panel.enc[0] = 3;
    panel.magic = 0;                                    /* (a .noinit that power-off garbled) */
    sm_settings_load();
    panel_init();
    bad += check("SETTINGS: a torn record: the default table", !memcmp(&panel, &PANEL_DEFAULT, sizeof panel));
    return bad;
}

int main(void)
{
    int bad = 0;
    bad += test_core_silent();
    bad += test_settings();
    printf("swamp: %d failed\n", bad);
    return bad != 0;
}
