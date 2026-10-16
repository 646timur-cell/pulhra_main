/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test of SWAMP MACHINE (firmware/src/swamp.c: the MONOFM sound core under its own app, swamp/sm_*.c).
 *   CORE   the sound core alone (no seq.c, no ui*.c), from sm_core_init (the firmware's power-on): silent tracks mix to silence
 *   KIT    the kit reaches the core: every track sounds its machine, releases to silence; a machine change mid-note fades without a click
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

/* one block of the mix: the peak |x| of both channels, the left channel appended to *L (if given) */
static uint32_t block_peak(int32_t *L, uint32_t *nl)
{
    int32_t out[2 * CTL];
    uint32_t i, peak = 0;
    mix_block(out, CTL);
    for (i = 0; i < 2u * CTL; i++) {
        uint32_t a = (uint32_t)(out[i] < 0 ? -out[i] : out[i]);
        peak = a > peak ? a : peak;
    }
    if (L)
        for (i = 0; i < CTL; i++)
            L[(*nl)++] = out[2u * i];
    return peak;
}

/* the kit's own sends (distortion / chorus / delay / reverb) ring on for seconds, by design, and the core has no reset for
 * the lines: the tests that wait for silence play dry, so nothing ever enters the lines */
static void kit_dry(void)
{
    uint32_t t, i;
    for (t = 0; t < SM_NTRK; t++)
        for (i = 0; i < 4u; i++)
            trk[t].p[P_DIST + i] = 0;
}

static int test_core_silent(void)
{
    uint32_t b, peak = 0, loud = 0;
    int bad = 0;
    sm_core_init();                                   /* (power-on boots the kit: real engines on the tracks) */
    for (b = 0; b < 100u; b++) {
        uint32_t a = block_peak(0, 0);
        peak = a > peak ? a : peak;
    }
    printf("swamp: peak %u\n", peak);
    bad += check("CORE: 100 blocks of silent tracks, |x| <= 1", peak <= 1u);
    /* the same mixer with a note on: silence above is silence of the tracks, not of a dead mixer */
    kit_dry();
    trk_note_on(&trk[0], 60, 100);
    for (b = 0; b < 40u; b++) {
        uint32_t a = block_peak(0, 0);
        loud = a > loud ? a : loud;
    }
    bad += check("CORE: the same mixer with a note on T1 sounds (peak > 200)", loud > 200u);
    trk_all_off(&trk[0]);
    return bad;
}

/* power-on boots the kit; sm_kit_boot loads all 4 tracks: machine, preset and every parameter (int8 -> int16, signed) */
static int test_kit_boot(void)
{
    sm_kit_t k;
    uint32_t t, i;
    int ok = 1, bad = 0;
    sm_core_init();
    bad += check("KIT: power-on gave T1 SAW, T2 FM-ST, T3 STRING, T4 BBOX",
                 trk[0].engine == ENGI_MSAW && trk[1].engine == ENGI_MFMST && trk[2].engine == ENGI_STRING &&
                 trk[3].engine == ENGI_MBBOX);
    memset(&k, 0, sizeof k);
    for (t = 0; t < SM_NTRK; t++) {
        sm_sound_from_preset(&k.snd[t], t, 1u);
        for (i = 0; i < P_COUNT; i++)
            k.snd[t].p[i] = (int8_t)((int)i * 3 - 64 - (int)t);       /* -64..: negative values stay negative */
    }
    sm_kit_boot(&k);
    for (t = 0; t < SM_NTRK; t++) {
        ok &= trk[t].eng_req == sm_engine_of(k.snd[t].machine) && trk[t].engine == trk[t].eng_req &&
              trk[t].preset == k.snd[t].preset;
        for (i = 0; i < P_COUNT; i++)
            ok &= trk[t].p[i] == (int16_t)k.snd[t].p[i];
    }
    bad += check("KIT: sm_kit_boot: engine, preset and all P_COUNT parameters (negatives sign-extended)", ok);
    bad += check("KIT: ... -64 stays -64 in trk[].p", trk[0].p[0] == -64);
    return bad;
}

static int test_kit_sounds(void)
{
    uint32_t t, b;
    int bad = 0;
    sm_project_init();
    sm_kit_boot(&smp.pat[0].kit);
    kit_dry();
    for (t = 0; t < SM_NTRK; t++) {
        uint32_t peak = 0, tail = 0, a;
        int clip = 0;
        char what[96];
        trk_note_on(&trk[t], 60, 100);
        for (b = 0; b < 40u; b++) {
            int32_t out[2 * CTL];
            uint32_t i;
            mix_block(out, CTL);
            for (i = 0; i < 2u * CTL; i++) {
                a = (uint32_t)(out[i] < 0 ? -out[i] : out[i]);
                peak = a > peak ? a : peak;
                clip |= a >= 32767u;
            }
        }
        trk_all_off(&trk[t]);
        for (b = 0; b < 400u; b++) {
            a = block_peak(0, 0);
            if (b >= 390u)
                tail = a > tail ? a : tail;
        }
        printf("swamp: T%u peak %u, tail %u\n", t + 1u, peak, tail);
        snprintf(what, sizeof what, "KIT: T%u sounds (peak > 200, no clip), dry release to |x| <= 2", t + 1u);
        bad += check(what, peak > 200u && !clip && tail <= 2u);
    }
    return bad;
}

/* T1 plays a SAW note; mid-note T1 becomes STRING: the core's fade (engine_block) takes the old voices out */
static int test_machine_switch_no_click(void)
{
    static int32_t L[CTL * 200];
    uint32_t n = 0, b, nd = 0, i, switch_at, j, premax = 0, med, mx = 0, tail = 0, endfade = 0;
    static uint32_t d[CTL * 80];
    sm_sound_t snd_string;
    int bad = 0;
    sm_core_init();
    kit_dry();
    sm_sound_from_preset(&snd_string, eng_rank(ENGI_STRING), 0);
    for (i = 0; i < 4u; i++)                          /* (dry as well: STRING's reverb send would ring on the fading SAW) */
        snd_string.p[P_DIST + i] = 0;
    trk_note_on(&trk[0], 60, 100);
    for (b = 0; b < 60u; b++)
        block_peak(L, &n);
    switch_at = n;
    for (i = 8; i < switch_at; i++) {                 /* the held note's steps before the change (skip the attack) */
        d[nd++] = (uint32_t)abs(L[i] - L[i - 1]);
        premax = d[nd - 1] > premax ? d[nd - 1] : premax;
    }
    {                                                 /* median (insertion sort) */
        uint32_t a, c;
        for (a = 1; a < nd; a++) {
            uint32_t v = d[a];
            for (c = a; c > 0 && d[c - 1] > v; c--)
                d[c] = d[c - 1];
            d[c] = v;
        }
        med = d[nd / 2];
    }
    sm_track_load(0, &snd_string);
    for (b = 0; b < 40u; b++)
        block_peak(L, &n);
    for (j = switch_at; j < n; j++) {                 /* the seam: from the last sample before, through the fade and after */
        uint32_t df = (uint32_t)abs(L[j] - L[j - 1]);
        mx = df > mx ? df : mx;
    }
    for (j = switch_at + 4u * CTL; j < n; j++)        /* after the 4 blocks of the fade: nothing of the old engine */
        endfade = (uint32_t)abs(L[j]) > endfade ? (uint32_t)abs(L[j]) : endfade;
    for (j = n - 4u * CTL; j < n; j++)
        tail = (uint32_t)abs(L[j]) > tail ? (uint32_t)abs(L[j]) : tail;
    printf("swamp: switch: median step %u, max step before %u, max step at the seam %u, after the fade |x| %u, engine %u\n",
           med, premax, mx, endfade, trk[0].engine);
    bad += check("SWITCH: the engine is STRING after the fade", trk[0].engine == ENGI_STRING && !trk[0].xf_on);
    /* the brief's "<= 4 x the median step" cannot hold for a SAW note alone (its own edges are 40 x the median, see
     * premax): the seam is held to the note's own steepest step instead, with the end of the fade at exactly 0 */
    bad += check("SWITCH: no step at the seam beyond the SAW note's own steepest step", med > 0 && mx <= premax);
    bad += check("SWITCH: the old engine is silent after the fade (4 blocks), nothing hangs", endfade <= 2u && tail <= 2u);
    /* a note that comes during the fade waits and then plays on the new engine */
    sm_core_init();
    kit_dry();
    for (i = 0; i < 4u; i++)
        snd_string.p[P_DIST + i] = 0;
    trk_note_on(&trk[0], 60, 100);
    for (b = 0; b < 20u; b++)
        block_peak(0, 0);
    sm_track_load(0, &snd_string);
    trk_note_on(&trk[0], 64, 100);                    /* (queued by the core: xp_note) */
    {
        uint32_t peak = 0, a;
        for (b = 0; b < 40u; b++) {
            a = block_peak(0, 0);
            peak = a > peak ? a : peak;
        }
        bad += check("SWITCH: a note struck during the fade sounds on the new engine",
                     trk[0].engine == ENGI_STRING && peak > 200u);
    }
    trk_all_off(&trk[0]);
    for (b = 0; b < 400u; b++)
        block_peak(0, 0);
    bad += check("SWITCH: no hanging voices 400 blocks after the release (voices_busy == 0)", voices_busy() == 0);
    return bad;
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

_Static_assert(sizeof(sm_step_t) == 10u, "a step is 10 bytes");

/* the project at power-on: 120.0 BPM, swing 50, empty steps, no locks, the default kit in every pattern */
static int test_model_init(void)
{
    uint32_t pp, t, s;
    int empty = 1;
    int bad = 0;
    memset(&smp, 0x5A, sizeof smp);
    sm_project_init();
    for (pp = 0; pp < SM_NPAT; pp++) {
        empty &= smp.pat[pp].nlock == 0;
        for (t = 0; t < SM_NTRK; t++)
            for (s = 0; s < SM_NSTEP; s++)
                empty &= smp.pat[pp].step[t][s].n == 0;
    }
    bad += check("MODEL: 16 patterns x 4 tracks x 32 steps empty, no locks", empty);
    bad += check("MODEL: BPM 120.0, swing 50, 16 steps",
                 smp.bpm10 == 1200u && smp.swing == 50u && smp.pat[0].kit.snd[0].len == 16u);
    bad += check("MODEL: kit T1 SAW, T2 FM-ST, T3 STRING, T4 BBOX",
                 sm_engine_of(smp.pat[0].kit.snd[0].machine) == ENGI_MSAW &&
                 sm_engine_of(smp.pat[0].kit.snd[1].machine) == ENGI_MFMST &&
                 sm_engine_of(smp.pat[0].kit.snd[2].machine) == ENGI_STRING &&
                 sm_engine_of(smp.pat[0].kit.snd[3].machine) == ENGI_MBBOX);
    bad += check("MODEL: the kit is the same in pattern 0 and 15",
                 !memcmp(&smp.pat[0].kit, &smp.pat[15].kit, sizeof(sm_kit_t)));
    return bad;
}

/* every machine, every preset: the engine's values, the envelope, the filter envelope, the sends */
static int test_sound_from_preset(void)
{
    uint32_t m, pi, i;
    int ok = 1, n = 0;
    sm_sound_t s;
    static const uint8_t FXD[4] = {0, 24, 28, 36};
    for (m = 0; m < SM_NMACHINE; m++) {
        const engine_t *e = ENGINES[sm_engine_of(m)];
        for (pi = 0; pi < e->npresets; pi++, n++) {
            const preset_t *pr = &e->presets[pi];
            sm_sound_from_preset(&s, m, pi);
            ok &= s.machine == m && s.preset == pi;
            for (i = 0; i < 8u; i++)
                ok &= s.p[P_E0 + i] == (int8_t)pr->e[i];
            ok &= s.p[P_ATK] == (int8_t)pr->env[0] && s.p[P_DEC] == (int8_t)pr->env[1] &&
                  s.p[P_SUS] == (int8_t)pr->env[2] && s.p[P_REL] == (int8_t)pr->env[3];
            ok &= s.p[P_ED_FLT] == pr->fenv;
            ok &= s.p[P_VOICE] == (pr->mono ? V_LEGATO : V_POLY);
            for (i = 0; i < 4u; i++)
                ok &= s.p[P_DIST + i] == (pr->fx[i] ? pr->fx[i] - 1 : FXD[i]);
        }
    }
    printf("swamp: sizeof smp %u, pattern %u\n", (unsigned)sizeof smp, (unsigned)sizeof(sm_pattern_t));
    printf("swamp: %d presets over %u machines\n", n, (unsigned)SM_NMACHINE);
    sm_sound_from_preset(&s, 0, 6);                      /* SAW SYNC LEAD: the ninth value */
    ok &= s.p[P_E8] == 80;
    return check("MODEL: every machine's every preset reaches the sound", ok);
}

int main(void)
{
    int bad = 0;
    bad += test_core_silent();
    bad += test_kit_boot();
    bad += test_kit_sounds();
    bad += test_machine_switch_no_click();
    bad += test_settings();
    bad += test_model_init();
    bad += test_sound_from_preset();
    printf("swamp: %d failed\n", bad);
    return bad != 0;
}
