/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test of SWAMP MACHINE (firmware/src/swamp.c: the MONOFM sound core under its own app, swamp/sm_*.c).
 *   CORE   the sound core alone (no seq.c, no ui*.c), from sm_core_init (the firmware's power-on): silent tracks mix to silence
 *   KIT    the kit reaches the core: every track sounds its machine, releases to silence; a machine change mid-note fades without a click
 *   SETTINGS  the settings record in the flash (the learned panel table, HOLD, palette) reaches SWAMP: sm_settings_load
 *   SEQ    the step sequencer (sm_seq.c): timing at 30 / 120 / 300 BPM, track lengths 1 and 32, trigs while playing, STOP
 *   CMD    the command queue (sm_cmd.c): full ring, coalesced and clamped values, a drain in the middle of a push
 *   STEP   sm_step_toggle (the main loop's); LOAD before engine_block; PANIC
 *   DRAW   the screen (sm_draw.c): every text inside its cell, the budget of pixels per change, the clean ink on a grained ground
 *   cc -O2 -w -DFELUCCA_SWAMP=1 -DFELUCCA_MONOFM=1 -DFELUCCA_MODULE=0 -Ibuild/gen -Ifirmware/src -o build/host/swamp_test tests/swamp_test.c -lm */
#include "swamp_host.h"
/* the settings record as the firmware reads it (swamp.c: gfx, panel, settings_persist, storage on a simulated NOR) */
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c) { (void)x; (void)y; (void)w; (void)h; (void)c; }
static void lcd_sync(void) {}
static void lcd_power(uint32_t s) { (void)s; }
static void lcd_wake_now(void) {}
/* the screen as the panel gets it (byte-swapped RGB565), and what the draw hooks of gfx.c see: the ink boxes of the texts of
 * the canvas being drawn, moved to screen rows when it is blitted (the lint of test_draw_lint) */
static uint16_t host_screen[240 * 240];
static uint32_t lcd_px;                                  /* the pixels the display got (the budget is measured here) */
static void (*begin_hook)(void);                         /* run at the next cv_begin (once): the audio stream moving on mid-frame */
static void lint_begin(void)
{
    void (*h)(void) = begin_hook;
    begin_hook = 0;
    if (h)
        h();
}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *p)
{
    uint32_t j;
    lcd_px += w * h;
    for (j = 0; j < h; j++)
        memcpy(host_screen + (y + j) * 240u + x, p + j * w, w * 2u);
}
typedef struct { int32_t x0, y0, x1, y1; uint32_t flags; char s[16]; } lint_box_t;
static lint_box_t lint_box[256];
static uint32_t lint_n, lint_band;                       /* the boxes of a frame; those of the canvas not blitted yet start here */
static void lint_text(int32_t x0, int32_t y0, int32_t x1, int32_t y1, const char *s, uint32_t flags)
{
    lint_box_t *b = &lint_box[lint_n < 256u ? lint_n : 255u];
    b->x0 = x0; b->y0 = y0; b->x1 = x1; b->y1 = y1; b->flags = flags;
    strncpy(b->s, s, 15);
    b->s[15] = 0;
    lint_n++;
}
static void lint_blit(uint32_t x, uint32_t y)
{
    uint32_t i;
    for (i = lint_band; i < lint_n && i < 256u; i++) {
        lint_box[i].x0 += (int32_t)x; lint_box[i].x1 += (int32_t)x;
        lint_box[i].y0 += (int32_t)y; lint_box[i].y1 += (int32_t)y;
    }
    lint_band = lint_n;
}
#define GFX_HOOK_TEXT(x0, y0, x1, y1, s, flags) lint_text(x0, y0, x1, y1, s, flags)
#define GFX_HOOK_BLIT(x, y, r0) lint_blit(x, y)
#define GFX_HOOK_BEGIN() lint_begin()
#define GFX_HOOK_PIXELS(n) ((void)0)
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
/* the sequencer's note-ons (swamp/sm_seq.c) through a counter: per track how many, at which sample (seq_clock: the samples
 * the test has mixed or run through events_block before this block) */
static uint32_t seq_clock, on_n[NTRK], on_at[NTRK][4], on_last[NTRK];
static void seq_note_on(track_t *t, uint32_t note, uint32_t vel)
{
    uint32_t k = (uint32_t)(t - trk);
    if (on_n[k] < 4u)
        on_at[k][on_n[k]] = seq_clock;
    on_n[k]++;
    on_last[k] = seq_clock;
    trk_note_on(t, note, vel);
}
/* the audio stream preempting the main loop inside sm_cmd_push, between finding a pending command and storing into it */
static int preempt_armed;
static void sm_cmd_drain(void);
#define SM_CMD_PREEMPT() do { if (preempt_armed) { preempt_armed = 0; sm_cmd_drain(); } } while (0)
/* every note-on of the app code included below (the sequencer's) goes through the counter; the tests' own calls after
 * the #undef go to voice.c's directly */
#define trk_note_on seq_note_on
#include "../firmware/src/swamp/sm_app.c"
#undef trk_note_on

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

/* ------------------------------------------------- the queue, the sequencer --- */
static void seq_log_clear(void)
{
    seq_clock = 0;
    memset(on_n, 0, sizeof on_n);
    memset(on_at, 0, sizeof on_at);
    memset(on_last, 0, sizeof on_last);
}

/* power-on, the kit dry in the model and on the tracks (an SMC_LOAD copies the model's sends) */
static void seq_reset(void)
{
    uint32_t t, i, pp;
    sm_core_init();
    for (pp = 0; pp < SM_NPAT; pp++)
        for (t = 0; t < SM_NTRK; t++)
            for (i = 0; i < 4u; i++)
                smp.pat[pp].kit.snd[t].p[P_DIST + i] = 0;
    kit_dry();
    seq_log_clear();
}

static int push(uint32_t kind, uint32_t t, uint32_t step, uint32_t param, int32_t val)
{
    sm_cmd_t c;
    c.kind = (uint8_t)kind;
    c.trk = (uint8_t)t;
    c.step = (uint8_t)step;
    c.param = (uint8_t)param;
    c.val = (int16_t)val;
    return sm_cmd_push(c);
}

/* the main loop's way to change a value: the model first (in its range), then the command */
static int set_param(uint32_t t, uint32_t p, int32_t v)
{
    smp.pat[smp.cur].kit.snd[t].p[p] = (int8_t)v;
    return push(SMC_PARAM, t, 0, p, v);
}

/* every parameter of every track: the core's == the model's */
static int core_is_model(void)
{
    uint32_t t, p;
    int ok = 1;
    for (t = 0; t < SM_NTRK; t++)
        for (p = 0; p < P_COUNT; p++)
            ok &= trk[t].p[p] == (int16_t)smp.pat[smp.cur].kit.snd[t].p[p];
    return ok;
}

static void run_block(void)
{
    block_peak(0, 0);
    seq_clock += CTL;
}

static int near(uint32_t a, uint32_t b) { return (a > b ? a - b : b - a) <= CTL; }

/* 120 BPM, T1 trigs on steps 0 and 4, PLAY: note-ons at sample 0 and 4 x FS / 8 (a 1/16 is FS / 8 at 120) */
static int test_seq_timing(void)
{
    uint32_t b;
    int bad = 0;
    seq_reset();
    sm_step_toggle(0, 0);
    sm_step_toggle(0, 4);
    push(SMC_PLAY, 0, 0, 0, 0);
    for (b = 0; b < 1000u; b++)                          /* 32000 samples: steps 0 .. 5 */
        run_block();
    printf("swamp: seq 120 BPM: %u note-ons at %u, %u (want 0, %u)\n", on_n[0], on_at[0][0], on_at[0][1], 4u * FS / 8u);
    bad += check("SEQ: 120 BPM, trigs on 0 and 4: note-ons at 0 and 4 x FS/8 (+-CTL), nothing else",
                 on_n[0] == 2u && near(on_at[0][0], 0) && near(on_at[0][1], 4u * FS / 8u) &&
                 !on_n[1] && !on_n[2] && !on_n[3]);
    bad += check("SEQ: sm_playing, sm_seq_pos on step 5 of T1", sm_playing && sm_seq_pos(0) == 5u);
    return bad;
}

/* 30 and 300 BPM (and 133.7: a step of no whole number of samples), T1 len 1 with a trig on step 0, T2 len 32 with 4
 * trigs, 64 bars: exactly 64 x 16 / len x hits note-ons, the last one on step 1023's sample (+-CTL): no drift, no skip */
static int test_seq_tempo_edges(void)
{
    static const uint16_t BPM[3] = {300, 3000, 1337};
    static const uint8_t HIT[4] = {0, 5, 17, 31};
    uint32_t k, i;
    int bad = 0;
    for (k = 0; k < 3u; k++) {
        double S = (double)FS * 150.0 / BPM[k];              /* samples per 1/16 */
        uint32_t last = (uint32_t)(1023.0 * S + 0.5), end = (uint32_t)(1023.5 * S);
        char what[120];
        seq_reset();
        smp.bpm10 = BPM[k];
        smp.pat[smp.cur].kit.snd[0].len = 1;
        smp.pat[smp.cur].kit.snd[1].len = 32;
        sm_step_toggle(0, 0);
        for (i = 0; i < 4u; i++)
            sm_step_toggle(1, HIT[i]);
        push(SMC_PLAY, 0, 0, 0, 0);
        while (seq_clock <= end) {                           /* (the sequencer alone: nothing to hear) */
            events_block(CTL);
            seq_clock += CTL;
        }
        printf("swamp: seq %u.%u BPM: T1 %u note-ons, last at %u; T2 %u, last at %u (want 1024 / 128 at %u)\n",
               BPM[k] / 10u, BPM[k] % 10u, on_n[0], on_last[0], on_n[1], on_last[1], last);
        snprintf(what, sizeof what, "SEQ: %u.%u BPM, 64 bars: T1 len 1 1024, T2 len 32 4 hits 128, last on step 1023",
                 BPM[k] / 10u, BPM[k] % 10u);
        bad += check(what, on_n[0] == 1024u && on_n[1] == 128u && !on_n[2] && !on_n[3] &&
                           near(on_last[0], last) && near(on_last[1], last));
    }
    return bad;
}

/* T2's parameters 0..63 to another value in their range (and in the model's int8): 64 distinct values, the ring full */
static int fill_ring_t2(void)
{
    uint32_t p;
    int ok = 1;
    for (p = 0; p < 64u; p++) {
        const param_desc_t *d = param_desc_of(eng_idx(trk[1].eng_req), p);
        int32_t v = smp.pat[smp.cur].kit.snd[1].p[p] == d->min ? d->max : d->min;
        ok &= set_param(1, p, clamp(v, -64, 127));
    }
    return ok;
}

/* the queue full (64 values of distinct parameters, nothing drained), then 200 values of one parameter: the last one
 * arrives, all 64 too; anything but a value is refused while values wait past the full ring; values out of range land
 * clamped; after the drain the core is the model */
static int test_cmd_coalesce(void)
{
    uint32_t i;
    int ok = 1, bad = 0;
    seq_reset();
    bad += check("CMD: 64 commands fit the ring", fill_ring_t2());
    bad += check("CMD: the 65th command (a LOAD) is refused (full)", !push(SMC_LOAD, 2, 0, 0, 0));
    for (i = 0, ok = 1; i < 200u; i++)
        ok &= set_param(0, P_FCUT, (int32_t)(i % 128u));
    bad += check("CMD: 200 values of T1 CUT on the full ring are taken (coalesced)", ok);
    bad += check("CMD: ... and then a LOAD, a STOP are refused (they would overtake the values)",
                 !push(SMC_LOAD, 2, 0, 0, 0) && !push(SMC_STOP, 0, 0, 0, 0));
    bad += check("CMD: (nothing reached the core before the drain)", trk[0].p[P_FCUT] != 199 % 128 && !core_is_model());
    sm_cmd_drain();
    printf("swamp: cmd: T1 CUT %d (want %d)\n", trk[0].p[P_FCUT], 199 % 128);
    bad += check("CMD: after the drain T1 CUT == the last value, all 64 values of T2 applied: core == model",
                 trk[0].p[P_FCUT] == 199 % 128 && core_is_model());
    bad += check("CMD: the ring takes commands again after the drain", push(SMC_STOP, 0, 0, 0, 0));
    smp.pat[smp.cur].kit.snd[0].p[P_FCUT] = 127;          /* (the main loop's model: its own clamp) */
    push(SMC_PARAM, 0, 0, P_FCUT, 300);
    smp.pat[smp.cur].kit.snd[0].p[P_LEVEL] = 0;
    push(SMC_PARAM, 0, 0, P_LEVEL, -50);
    sm_cmd_drain();
    bad += check("CMD: values out of range land clamped (CUT 300 -> 127, LEVEL -50 -> 0): core == model",
                 trk[0].p[P_FCUT] == 127 && trk[0].p[P_LEVEL] == 0 && core_is_model());
    return bad;
}

/* the audio stream drains in the middle of a coalescing push (after the push found the pending value, before it stored
 * the new one): the new value still arrives, the others once */
static int test_cmd_coalesce_race(void)
{
    uint32_t p;
    int ok = 1, bad = 0;
    seq_reset();                                          /* the ring: T2's values 0..62 and T1 CUT 5, full */
    fill_ring_t2();
    sm_cmd_drain();
    for (p = 0; p < 63u; p++)                             /* (back to the defaults: 63 distinct values again) */
        set_param(1, p, TP[p].def);
    set_param(0, P_FCUT, 5);
    preempt_armed = 1;
    ok &= set_param(0, P_FCUT, 7);
    ok &= !preempt_armed && trk[0].p[P_FCUT] == 5;        /* (the drain in the middle took CUT 5) */
    sm_cmd_drain();
    bad += check("CMD: drained while coalescing into the ring: CUT 7 arrives, core == model",
                 ok && trk[0].p[P_FCUT] == 7 && core_is_model());
    seq_reset();                                          /* the ring full of T2's values, T1 CUT 5 past it */
    fill_ring_t2();
    set_param(0, P_FCUT, 5);
    preempt_armed = 1;
    ok = set_param(0, P_FCUT, 7);
    ok &= !preempt_armed && trk[0].p[P_FCUT] == 5;
    sm_cmd_drain();
    bad += check("CMD: drained while coalescing past the full ring: CUT 7 arrives, core == model",
                 ok && trk[0].p[P_FCUT] == 7 && core_is_model());
    return bad;
}

/* the main loop's sm_step_toggle: on, the whole step then n; off, n = 0 */
static int test_step_toggle(void)
{
    const sm_step_t *s = &smp.pat[0].step[2][9];
    int bad = 0;
    seq_reset();
    smp.pat[0].kit.snd[2].p[P_ROOT] = 3;
    smp.pat[0].step[2][9] = (sm_step_t){{70, 71, 72, 73}, 0, 9, 7, 2, SMF_REST | SMF_SLIDE, 3};   /* (an old step's leftovers) */
    sm_step_toggle(2, 9);
    bad += check("STEP: on: C3 + ROOT, vel 100, LEN 0 (the track's GATE), no flags, n 1",
                 s->n == 1u && s->note[0] == 51u && s->vel == 100u && !s->len && !s->flags && !s->cond && !s->nudge &&
                     !s->note[1] && !s->note[2] && !s->note[3]);
    sm_step_toggle(2, 9);
    bad += check("STEP: off: n 0", !s->n);
    sm_step_toggle(4, 0);
    sm_step_toggle(0, 32);
    bad += check("STEP: out of range: nothing", !smp.pat[0].step[0][0].n);
    return bad;
}

/* a trig toggled while playing changes that step only */
static int test_trig_while_playing(void)
{
    static sm_step_t before[SM_NTRK][SM_NSTEP], after[SM_NTRK][SM_NSTEP];
    uint32_t b;
    int bad = 0;
    seq_reset();
    sm_step_toggle(0, 0);
    sm_step_toggle(1, 3);
    sm_step_toggle(3, 8);
    push(SMC_PLAY, 0, 0, 0, 0);
    for (b = 0; b < 300u; b++)                            /* (to step 1) */
        run_block();
    memcpy(before, smp.pat[smp.cur].step, sizeof before);
    sm_step_toggle(1, 7);
    run_block();
    memcpy(after, smp.pat[smp.cur].step, sizeof after);
    for (b = 0; b < 1000u; b++)                           /* (to step 7.5) */
        run_block();
    bad += check("SEQ: a trig while playing sets step 7 of T2",
                 !before[1][7].n && after[1][7].n == 1u && after[1][7].note[0] == 48u + (uint32_t)trk[1].p[P_ROOT]);
    after[1][7] = before[1][7];
    bad += check("SEQ: ... and no other step changes", !memcmp(before, after, sizeof before) && sm_playing);
    printf("swamp: T2 note-ons %u, at %u, %u (want %u, %u)\n", on_n[1], on_at[1][0], on_at[1][1], 3u * FS / 8u,
           7u * FS / 8u);
    bad += check("SEQ: ... and plays in this pass (T2 steps 3 and 7)",
                 on_n[1] == 2u && near(on_at[1][0], 3u * FS / 8u) && near(on_at[1][1], 7u * FS / 8u));
    return bad;
}

/* notes held with LEN INF on every track; STOP: one block later nothing sounds but the one-block fade, 400 later silence */
static int test_stop_releases(void)
{
    uint32_t t, i, b, busy0, peak = 0;
    int fading = 1, gated = 1, bad = 0;
    seq_reset();
    for (t = 0; t < SM_NTRK; t++)
        sm_step_toggle(t, 0);
    for (t = 0; t < SM_NTRK; t++)
        smp.pat[smp.cur].step[t][0].len = 255u;           /* INF (the main loop's model; no LEN control in stage 1) */
    push(SMC_PLAY, 0, 0, 0, 0);
    for (b = 0; b < 2000u; b++)                           /* past step 0 (172 blocks): the notes ring on into step 11 */
        run_block();
    busy0 = voices_busy();
    for (t = 0; t < SM_NTRK; t++) {                       /* (a key still down on every track, not a release tail) */
        int g = 0;
        for (i = 0; i < NVOICE; i++)
            g |= trk[t].v[i].active && trk[t].v[i].gate;
        gated &= g;
    }
    push(SMC_STOP, 0, 0, 0, 0);
    run_block();
    for (t = 0; t < SM_NTRK; t++)
        for (i = 0; i < NVOICE; i++)
            fading &= !trk[t].v[i].active || trk[t].v[i].stage == 4u;
    printf("swamp: stop: %u voices before, %u after one block\n", busy0, voices_busy());
    bad += check("SEQ: LEN INF holds the notes past their step (gate on, every track)", gated && busy0 >= SM_NTRK);
    bad += check("SEQ: STOP: one block later only fading voices (stage 4) or none, voices_busy == 0",
                 fading && voices_busy() == 0u && !sm_playing && sm_seq_pos(0) == 0u);
    for (b = 0; b < 400u; b++) {
        uint32_t a = block_peak(0, 0);
        if (b >= 390u)
            peak = a > peak ? a : peak;
    }
    bad += check("SEQ: ... 400 blocks later silence (|x| <= 2)", peak <= 2u);
    return bad;
}

/* SMC_LOAD of another machine through the queue, a note sounding: the queue is drained before engine_block, so the very
 * first block already fades the old engine with its own values (xf_on, pe_old), never the old one with the new values */
static int test_load_order(void)
{
    int16_t pe_saw[8];
    uint32_t i, b;
    sm_sound_t *s = &smp.pat[0].kit.snd[0];
    int bad = 0;
    seq_reset();
    trk_note_on(&trk[0], 60, 100);
    for (b = 0; b < 20u; b++)
        run_block();
    for (i = 0; i < 8u; i++)
        pe_saw[i] = trk[0].p[P_E0 + i];
    sm_sound_from_preset(s, eng_rank(ENGI_STRING), 0);    /* the main loop: the model first, then the command */
    for (i = 0; i < 4u; i++)
        s->p[P_DIST + i] = 0;
    for (i = 0; i < 8u && s->p[P_E0 + i] == pe_saw[i]; i++)
        ;
    bad += check("LOAD: (STRING's values differ from SAW's)", i < 8u);
    push(SMC_LOAD, 0, 0, 0, 0);
    run_block();
    bad += check("LOAD: the first block after a queued machine change fades the old engine (xf_on, xf 3)",
                 trk[0].engine == ENGI_MSAW && trk[0].eng_req == ENGI_STRING && trk[0].xf_on &&
                 trk[0].xf == XF_BLOCKS - 1u);
    bad += check("LOAD: ... with its own values (pe_old == SAW's), the new ones in p[]",
                 !memcmp(trk[0].pe_old, pe_saw, sizeof pe_saw) && trk[0].p[P_E0] == s->p[P_E0]);
    for (b = 0; b < 8u; b++)
        run_block();
    bad += check("LOAD: then STRING", trk[0].engine == ENGI_STRING && !trk[0].xf_on);
    trk_all_off(&trk[0]);
    return bad;
}

/* panic_req (M-UPGRADE before it takes the flash): the flagged tracks let go of every note in the next block */
static int test_panic(void)
{
    uint32_t b, i;
    int held0 = 0, held1 = 0, bad = 0;
    seq_reset();
    trk_note_on(&trk[0], 60, 100);
    trk_note_on(&trk[1], 60, 100);
    for (b = 0; b < 10u; b++)
        run_block();
    panic_req = 1u;                                       /* T1 only */
    run_block();
    for (i = 0; i < NVOICE; i++) {
        held0 |= trk[0].v[i].gate;
        held1 |= trk[1].v[i].gate;
    }
    bad += check("PANIC: T1 released in one block, T2 still held, panic_req cleared", !held0 && held1 && !panic_req);
    trk_all_off(&trk[1]);
    return bad;
}


/* ------------------------------------------------------------------------------------------------ INPUT --- */
static void in_reset(void)
{
    uint32_t k;
    for (k = 0; k < 8u; k++)
        run_block();                                      /* (the ring empty) */
    panel = PANEL_DEFAULT;
    seq_reset();
    host_pressed = host_notes = 0;
    fm1_in.notes = 0;
    for (k = 0; k < 7u; k++)
        host_enc[k] = 0;
}
static void turn(uint32_t role, int32_t n) { host_enc[panel.enc[role]] += n * panel.dir[role]; }
static void press(uint32_t b) { host_pressed |= 1u << panel.btn[b]; }
static const uint8_t WHITE[16] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};   /* F3 .. G5 */
static sm_sound_t *ksnd(uint32_t t) { return &smp.pat[smp.cur].kit.snd[t]; }

static int test_alg_selects_track(void)
{
    int bad = 0;
    in_reset();
    turn(EN_ALGO, 5);
    sm_input_poll();
    bad += check("INPUT: 5 detents of ALGORITHM: track 1", sm_ui.trk == 1u);
    turn(EN_ALGO, -2);
    sm_input_poll();
    bad += check("INPUT: ... -2 more: wraps to track 3", sm_ui.trk == 3u);
    return bad;
}

static int test_pages_cycle(void)
{
    int bad = 0, ok = 1;
    static const char *const want[4] = {"EDIT 1", "EDIT 2", "VOICE", "EDIT 1"};
    uint32_t k;
    in_reset();
    press(B_ENV);
    sm_input_poll();
    bad += check("INPUT: ENV: FILTER, again: F.ENV, again: FILTER",
                 !strcmp(sm_cur_page()->name, "FILTER"));
    press(B_ENV);
    sm_input_poll();
    ok = !strcmp(sm_cur_page()->name, "F.ENV");
    press(B_ENV);
    sm_input_poll();
    bad += check("INPUT: ... the cycle of the ENV family", ok && !strcmp(sm_cur_page()->name, "FILTER"));
    for (k = 0; k < 4u; k++) {
        press(B_EDIT);
        sm_input_poll();
        ok &= !strcmp(sm_cur_page()->name, want[k]);
    }
    bad += check("INPUT: EDIT x4 from another family: EDIT 1, EDIT 2, VOICE, EDIT 1", ok);
    press(B_GLO);
    sm_input_poll();
    ok = !strcmp(sm_cur_page()->name, "AMP");
    press(B_GLO);
    sm_input_poll();
    bad += check("INPUT: GLO: AMP, MIX", ok && !strcmp(sm_cur_page()->name, "MIX"));
    return bad;
}

static int test_knob_writes_model_and_core(void)
{
    int bad = 0;
    int8_t v0, v1;
    uint32_t sel;
    in_reset();
    press(B_ENV);
    sm_input_poll();
    sel = sm_ui.trk;
    turn(EN_K2, -200);
    sm_input_poll();
    v0 = ksnd(sel)->p[P_FCUT];
    bad += check("INPUT: FILTER, K2 -200: CUT at its minimum in the model", v0 == TP[P_FCUT].min);
    turn(EN_K2, 10);
    sm_input_poll();
    v1 = ksnd(sel)->p[P_FCUT];
    bad += check("INPUT: K2 +10: CUT grew", v1 > v0);
    run_block();
    bad += check("INPUT: ... after one block the core has it", trk[sel].p[P_FCUT] == v1);
    turn(EN_K2, 500);
    sm_input_poll();
    run_block();
    bad += check("INPUT: K2 +500: clipped at TP max, model and core",
                 ksnd(sel)->p[P_FCUT] == TP[P_FCUT].max && trk[sel].p[P_FCUT] == TP[P_FCUT].max);
    sm_dirty = 0;
    turn(EN_K1, 1);
    sm_input_poll();
    bad += check("INPUT: a knob turn sets sm_dirty", sm_dirty);
    in_reset();
    press(B_ENV);                                         /* FILTER: SELECT is HWIDTH */
    sm_input_poll();
    turn(EN_SELECT, -3);
    sm_input_poll();
    run_block();
    bad += check("INPUT: SELECT on FILTER: HWIDTH, model and core",
                 ksnd(0)->p[P_HWIDTH] == TP[P_HWIDTH].def - 3 && trk[0].p[P_HWIDTH] == ksnd(0)->p[P_HWIDTH]);
    in_reset();
    press(B_ENV);
    sm_input_poll();
    press(B_ENV);                                         /* F.ENV: the fourth knob has no parameter */
    sm_input_poll();
    {
        sm_sound_t before = *ksnd(0);
        turn(EN_K4, 5);
        turn(EN_SELECT, 5);
        sm_input_poll();
        bad += check("INPUT: a knob without a parameter (0xFF) changes nothing", !memcmp(&before, ksnd(0), sizeof before));
    }
    return bad;
}

static int test_white_key_toggles_trig(void)
{
    int bad = 0;
    in_reset();
    host_notes = 1u << WHITE[2];
    sm_input_poll();
    bad += check("INPUT: the 3rd white key: step 2 of the selected track on", smp.pat[smp.cur].step[0][2].n == 1u);
    host_notes = 1u << WHITE[2];
    sm_input_poll();
    bad += check("INPUT: ... again: off", smp.pat[smp.cur].step[0][2].n == 0u);
    host_notes = 1u << 25;                                /* F#5: bank 1 */
    sm_input_poll();
    bad += check("INPUT: F#5: bank 1, no trig", sm_ui.bank == 1u && !smp.pat[smp.cur].step[0][25].n);
    host_notes = 1u << WHITE[2];
    sm_input_poll();
    bad += check("INPUT: bank 1, the 3rd white key: step 18", smp.pat[smp.cur].step[0][18].n == 1u &&
                 !smp.pat[smp.cur].step[0][2].n);
    host_notes = 1u << 3;                                 /* G#3: black */
    sm_input_poll();
    bad += check("INPUT: a black key: nothing", !smp.pat[smp.cur].step[0][3].n && !smp.pat[smp.cur].step[0][19].n);
    host_notes = 1u << 22;                                /* D#5: bank 0 */
    sm_input_poll();
    host_notes = 1u << WHITE[15];
    sm_input_poll();
    bad += check("INPUT: D#5: bank 0, the 16th white key: step 15", sm_ui.bank == 0u && smp.pat[smp.cur].step[0][15].n == 1u);
    turn(EN_ALGO, 1);
    sm_input_poll();
    host_notes = 1u << WHITE[0];
    sm_input_poll();
    bad += check("INPUT: the trig goes to the selected track", smp.pat[smp.cur].step[1][0].n == 1u && !smp.pat[smp.cur].step[0][0].n);
    return bad;
}

static int test_presets_knob_loads(void)
{
    int bad = 0;
    uint32_t sel;
    sm_sound_t ref;
    in_reset();
    sel = sm_ui.trk;
    turn(EN_PRESET, 1);
    sm_input_poll();
    bad += check("INPUT: PRESETS +1: the model's sound is preset 1", ksnd(sel)->preset == 1u);
    sm_sound_from_preset(&ref, ksnd(sel)->machine, 1);
    bad += check("INPUT: ... all of it (sm_sound_from_preset)", !memcmp(&ref, ksnd(sel), sizeof ref));
    run_block();
    bad += check("INPUT: ... after a block the core's preset is 1 and the values are the model's",
                 trk[sel].preset == 1u && core_is_model());
    turn(EN_PRESET, -1);
    sm_input_poll();
    turn(EN_ALGO, 2);
    sm_input_poll();
    run_block();
    bad += check("INPUT: PRESETS -1 back to 0 (track 0 only); ALGORITHM +2 moves the next ones",
                 ksnd(0)->preset == 0u && ksnd(2)->preset == 0u && sm_ui.trk == 2u && core_is_model());
    return bad;
}

static int test_held_step(void)
{
    int bad = 0;
    in_reset();
    host_ticks = 1000u;
    host_notes = 1u << WHITE[5];
    fm1_in.notes = 1u << WHITE[5];
    sm_input_poll();
    bad += check("INPUT: a key just pressed: no held step", sm_ui.held_step == 0xFFu);
    host_ticks += 300000u * FM1_TICKS_PER_US;
    sm_input_poll();
    bad += check("INPUT: held 0.30 s: still none", sm_ui.held_step == 0xFFu);
    host_ticks += 100000u * FM1_TICKS_PER_US;
    sm_input_poll();
    bad += check("INPUT: held 0.40 s: held_step 5", sm_ui.held_step == 5u);
    fm1_in.notes = 0;
    sm_input_poll();
    bad += check("INPUT: let go: none again", sm_ui.held_step == 0xFFu);
    return bad;
}

/* the PLAY button: PLAY / STOP in turn, through the queue; STOP leaves silence */
static int test_play_key(void)
{
    int bad = 0;
    uint32_t b, peak = 0, loud = 0;
    in_reset();
    sm_step_toggle(0, 0);
    bad += check("PLAY: stopped at the start", !sm_playing);
    press(B_PLAY);
    sm_input_poll();
    run_block();
    bad += check("PLAY key: sm_playing == 1 after a block", sm_playing == 1u);
    for (b = 0; b < 400u; b++) {
        uint32_t a = block_peak(0, 0);
        loud = a > loud ? a : loud;
    }
    bad += check("PLAY key: the step sounds", loud > 2u);
    press(B_PLAY);
    sm_input_poll();
    run_block();
    bad += check("PLAY key again: sm_playing == 0", sm_playing == 0u);
    for (b = 0; b < 400u; b++) {
        uint32_t a = block_peak(0, 0);
        if (b >= 390u)
            peak = a > peak ? a : peak;
    }
    bad += check("PLAY key: ... silence 400 blocks later (|x| <= 2)", peak <= 2u);
    return bad;
}

/* the model is the main loop's: a full ring and the audio stream behind -> the core ends equal to the model */
static int test_input_backpressure(void)
{
    int bad = 0;
    uint32_t i, t, n;
    sm_cmd_t f = {SMC_PARAM, 3, 0, P_LRATE, 0};
    in_reset();
    f.val = ksnd(3)->p[P_LRATE];
    for (i = 0; i < SM_NCMD; i++)
        sm_ring_put(&sm_cq, f);                           /* (the ring full of what changes nothing) */
    press(B_GLO);
    sm_input_poll();                                      /* AMP: K1..K4, SELECT ESYNC */
    for (t = 0; t < 2u; t++) {                            /* 10 distinct {trk, param} past the full ring: sm_cov (8) overflows */
        turn(EN_K1, 3); turn(EN_K2, -2); turn(EN_K3, 5); turn(EN_K4, 4); turn(EN_SELECT, 1);
        sm_input_poll();
        turn(EN_ALGO, 1);
        sm_input_poll();
    }
    turn(EN_PRESET, 1);                                   /* a LOAD on the way (refused: the values wait in sm_cov) */
    sm_input_poll();
    press(B_PLAY);
    sm_input_poll();                                      /* a PLAY, refused */
    {
        uint32_t w, any = 0;
        for (t = 0; t < SM_NTRK; t++)
            for (w = 0; w < 4u; w++)
                any |= sm_pdirty[t][w];
        bad += check("INPUT/FULL: the ring and sm_cov full: values refused (marked), LOAD and PLAY kept, the core behind",
                     any && sm_ld_pend && sm_tr_pend == 1u && !core_is_model());
    }
    for (n = 0; n < 40u; n++) {                           /* the main loop keeps polling, the audio stream drains a block now and then */
        sm_input_poll();
        if (n % 4u == 3u)
            run_block();
    }
    for (n = 0; n < 8u; n++) {
        sm_input_poll();
        run_block();
    }
    bad += check("INPUT/FULL: then the core's values == the model's, the preset too", core_is_model() &&
                 trk[sm_ui.trk].preset == ksnd(sm_ui.trk)->preset);
    bad += check("INPUT/FULL: and the refused PLAY went through", sm_playing);
    press(B_PLAY);
    sm_input_poll();
    run_block();
    bad += check("INPUT: PLAY again: STOP", !sm_playing);
    return bad;
}

/* P_E8 on a machine without a ninth value: the descriptor does not exist (param_desc_of would read past edit[]) */
static int test_cmd_e8_guard(void)
{
    int bad = 0;
    uint32_t m, e = NENGINES, p;
    int16_t before[P_COUNT];
    in_reset();
    for (m = 0; m < SM_NMACHINE && e == NENGINES; m++)
        if (!ENGINES[sm_engine_of(m)]->e8.label)
            e = sm_engine_of(m);
    bad += check("CMD/E8: a machine without a ninth value exists", e != NENGINES);
    if (e == NENGINES)
        return bad;
    trk[0].eng_req = (uint8_t)e;
    trk[0].p[P_E8] = 33;
    memcpy(before, trk[0].p, sizeof before);
    push(SMC_PARAM, 0, 0, P_E8, 99);
    push(SMC_PARAM, 0, 0, P_COUNT, 5);
    run_block();
    for (p = 0; p < P_COUNT; p++)
        if (trk[0].p[p] != before[p])
            break;
    bad += check("CMD/E8: SMC_PARAM P_E8 (and an id past P_COUNT) there: nothing changes", p == P_COUNT && trk[0].p[P_E8] == 33);
    push(SMC_PARAM, 0, 0, P_FCUT, 40);
    run_block();
    bad += check("CMD/E8: a normal value on that track still lands", trk[0].p[P_FCUT] == 40);
    return bad;
}

static int test_held_step_bank_key(void)
{
    int bad = 0;
    in_reset();
    host_ticks = 1000u;
    host_notes = 1u << WHITE[5];
    fm1_in.notes = 1u << WHITE[5];
    sm_input_poll();
    host_notes = 1u << 25;                                /* F#5 pressed while it is held */
    fm1_in.notes |= 1u << 25;
    host_ticks += 400000u * FM1_TICKS_PER_US;
    sm_input_poll();
    bad += check("INPUT: a bank key pressed while holding: held_step stays the step pressed (5)",
                 sm_ui.bank == 1u && sm_ui.held_step == 5u);
    fm1_in.notes = 0;
    sm_input_poll();
    return bad;
}

/* ------------------------------------------------------------ screen --- */
static void draw_setup(void)
{
    sm_core_init();
    sm_theme_pizdaks();
    sm_playing = 0;
    memset(sm_pos, 0, sizeof sm_pos);
    sm_rec = 0;
    sm_ui.page = 0; sm_ui.trk = 0; sm_ui.held_step = 0xFFu; sm_ui.bank = 0;
    sm_redraw_all();
    sm_dirty = 1;
}
static void draw_full(void)
{
    lint_n = lint_band = 0;
    sm_redraw_all();
    sm_dirty = 1;
    sm_draw();
}
typedef struct { int32_t x0, y0, x1, y1, inset; const char *name; } zone_t;
/* where a text may stand, by the screen (independent of the firmware's own layout constants): the header above its
 * frame, the page name, the four cells (a margin inside each), the SELECT row; the step strip carries no text */
static const zone_t ZONES[] = {
    {0, 0, 240, 20, 2, "header"}, {0, 24, 240, 48, 2, "page"},
    {0, 48, 120, 112, 4, "cell0"}, {120, 48, 240, 112, 4, "cell1"},
    {0, 112, 120, 176, 4, "cell2"}, {120, 112, 240, 176, 4, "cell3"},
    {0, 178, 240, 198, 2, "select"},
};
static uint32_t lint_shown;
static int lint_frame(const char *what)
{
    uint32_t i, j, bad = 0;
    if (lint_n < 4u) {                                       /* a header, a page name, a label and a value at least */
        printf("  lint %s: only %u texts drawn\n", what, lint_n);
        return 1;
    }
    for (i = 0; i < lint_n && i < 256u; i++) {
        const lint_box_t *b = &lint_box[i];
        const zone_t *z = 0;
        if (b->x1 <= b->x0)
            continue;
        for (j = 0; j < sizeof ZONES / sizeof ZONES[0]; j++)
            if (b->x0 >= ZONES[j].x0 && b->x0 < ZONES[j].x1 && b->y0 >= ZONES[j].y0 && b->y0 < ZONES[j].y1)
                z = &ZONES[j];
        if (!z || (b->flags & 3u) || b->x0 < z->x0 + z->inset || b->x1 > z->x1 - z->inset || b->y1 > z->y1) {
            if (lint_shown++ < 15u) printf("  lint %s: '%s' box %d,%d..%d,%d flags %u zone %s\n", what, b->s, b->x0, b->y0, b->x1, b->y1, b->flags,
                   z ? z->name : "none");
            bad++;
        }
        for (j = i + 1u; j < lint_n && j < 256u; j++) {   /* two texts of a zone never overlap (the page name's shadow aside) */
            const lint_box_t *c = &lint_box[j];
            if (z && z->name[0] != 'p' && c->x1 > c->x0 && b->x0 < c->x1 && c->x0 < b->x1 && b->y0 < c->y1 && c->y0 < b->y1) {
                printf("  lint %s: '%s' overlaps '%s'\n", what, b->s, c->s);
                bad++;
            }
        }
    }
    return bad != 0;
}
static int test_draw_lint(void)
{
    uint32_t pg, m, v, bad = 0, frames = 0, grained = 0;
    static uint16_t shot[240 * 240];
    static lint_box_t keep[256];
    for (m = 0; m < SM_NMACHINE; m++) {
        for (pg = 0; pg < sm_page_count(); pg++) {
            const sm_page_t *P = &SM_PAGES[pg];
            uint32_t slot;
            for (slot = 0; slot < 5u; slot++) {              /* the four knobs and SELECT */
                uint32_t id = slot < 4u ? P->id[slot] : P->sel;
                const param_desc_t *d;
                int32_t vals[3];
                char what[64];
                if (id == SM_NOPARAM)
                    continue;
                d = sm_desc_safe(sm_engine_of(m), id);
                if (!d)
                    continue;
                vals[0] = d->min; vals[1] = d->max; vals[2] = d->def;
                for (v = 0; v < 3u; v++) {
                    sm_sound_t *s;
                    draw_setup();
                    s = ksnd(0);
                    sm_sound_from_preset(s, m, 0);
                    s->p[id] = (int8_t)clamp(vals[v], -64, 127);
                    smp.bpm10 = 3000;                       /* the widest header: 300.0, REC, T1, the longest name */
                    smp.cur = 15;
                    sm_rec = 1;
                    sm_ui.page = (uint8_t)pg;
                    sm_ui.bank = 1;
                    draw_full();
                    snprintf(what, sizeof what, "machine %u page %s id %u val %d", m, P->name, id, (int)vals[v]);
                    bad += lint_frame(what);
                    {   /* no grain, dust or blood inside any ink box: the same frame without the grime, box by box */
                        uint32_t n = lint_n < 256u ? lint_n : 256u, b, x, y;
                        memcpy(keep, lint_box, n * sizeof keep[0]);
                        memcpy(shot, host_screen, sizeof shot);
                        ux.grime = 0;
                        draw_full();
                        ux.grime = 1;
                        for (b = 0; b < n; b++)
                            for (y = (uint32_t)keep[b].y0; y < (uint32_t)keep[b].y1 && y < 240u; y++)
                                for (x = (uint32_t)keep[b].x0; x < (uint32_t)keep[b].x1 && x < 240u; x++)
                                    if (shot[y * 240u + x] != host_screen[y * 240u + x]) {
                                        if (grained++ < 8u)
                                            printf("  grain in '%s' at %u,%u (%s)\n", keep[b].s, x, y, what);
                                    }
                    }
                    frames++;
                }
            }
        }
    }
    printf("  lint: %u frames\n", frames);
    bad += check("draw lint: no grain or blood inside any text's ink box", grained == 0);
    return check("draw lint: every text inside its cell, none cut, none overlapping", bad == 0 && frames > 500u);
}
static uint32_t px_bad;
static uint32_t frame_px(void)                           /* a frame; its pixels as the display counted them (= px) */
{
    lcd_px = 0;
    sm_draw();
    if (lcd_px != sm_draw_px)
        px_bad++;
    return lcd_px;
}
static int test_draw_budget(void)
{
    int bad = 0;
    uint32_t px;
    px_bad = 0;
    draw_setup();
    px = frame_px();
    bad += check("budget: the first frame draws the screen", px >= 240u * 200u && px <= 240u * 240u);
    px = frame_px();
    bad += check("budget: nothing changed, nothing drawn", px == 0);
    sm_playing = 1;                                           /* the transport started: the header's glyph */
    sm_dirty = 1;
    px = frame_px();
    bad += check("budget: play -> header and strip only", px > 0 && px <= 240u * 70u);
    sm_pos[0] = 3;                                            /* the playing step moved (no flag: the sequencer does not set it) */
    px = frame_px();
    bad += check("budget: the playing step moved -> <= 240 x 40", px > 0 && px <= 240u * 40u);
    sm_pos[0] = 4;
    px = frame_px();
    bad += check("budget: ... again", px > 0 && px <= 240u * 40u);
    sm_page_press(SMFAM_GLO);
    px = frame_px();
    bad += check("budget: a page change <= a full frame, more than a step", px > 240u * 40u && px <= 240u * 240u);
    sm_ui.trk = 1;
    sm_dirty = 1;
    px = frame_px();
    bad += check("budget: a track change <= a full frame", px > 0 && px <= 240u * 240u);
    {   /* a knob turned: its row of cells only (and the SELECT row never) */
        sm_sound_t *s = ksnd(sm_ui.trk);
        uint32_t id = sm_cur_page()->id[0];
        s->p[id] = (int8_t)(s->p[id] + 1);
        sm_dirty = 1;
        px = frame_px();
        bad += check("budget: one value -> one row of cells <= 240 x 64", px > 0 && px <= 240u * 64u);
    }
    sm_redraw_all();
    px = frame_px();
    bad += check("budget: redraw_all repaints the screen", px >= 240u * 200u && px <= 240u * 240u);
    bad += check("budget: sm_draw_px is what the display got, frame by frame", px_bad == 0);
    return bad;
}
static int test_draw_grain(void)
{
    const uint16_t TEXT = swap16(0xEF5Bu), DIM = swap16(0x4100u), BG = swap16(0x0000u), CLEAN = swap16(SMT_CLEAN);
    const uint16_t BLOOD = swap16(0xB800u);
    uint32_t x, y, s, grain = 0, tot = 0, dirty_in = 0, blood_in_strip = 0;
    int bad = 0;
    draw_setup();
    smp.pat[smp.cur].step[0][2].n = 1;                         /* a trig on step 2, none on step 3 */
    smp.pat[smp.cur].step[0][3].n = 0;
    draw_full();
    bad += check("grain: a trig is bone all through", host_screen[220 * 240 + 2 * 15 + 7] == TEXT && host_screen[208 * 240 + 2 * 15 + 3] == TEXT);
    bad += check("grain: an empty step has its dim frame", host_screen[206 * 240 + 3 * 15 + 6] == DIM);
    for (s = 0; s < 16u; s++)                                  /* the inside of every empty step: clean, all of it */
        if (s != 2u)
            for (y = 207; y < 235; y++)
                for (x = s * 15u + 1u; x < s * 15u + 13u; x++)
                    dirty_in += host_screen[y * 240u + x] != CLEAN;
    bad += check("grain: the inside of an empty step is clean (no grain, no blood)", dirty_in == 0);
    for (y = 202; y < 240; y++)
        for (x = 0; x < 240; x++) {
            uint16_t c = host_screen[y * 240u + x];
            if (y < 206 || y >= 236) { tot++; grain += c != BG; blood_in_strip += c == BLOOD; }
        }
    bad += check("grain: the ground around the strip wears the photocopy, sparsely", grain > tot / 50 && grain < tot / 3);
    bad += check("grain: no blood run reaches the step strip", blood_in_strip == 0);
    {   /* blood runs are there (the look), somewhere above the strip */
        uint32_t n = 0;
        for (y = 0; y < 202; y++)
            for (x = 0; x < 240; x++)
                n += host_screen[y * 240u + x] == BLOOD;
        bad += check("grain: the blood runs are drawn", n > 40u);
    }
    return bad;
}
/* the audio stream moves the playhead between the frame's signatures and its drawing: the strip must show the step its
 * signature saw, and the next frame must see the move */
static void hook_move_pos(void) { sm_pos[0] = 9; }
static int test_draw_playhead_snapshot(void)
{
    const uint16_t BLOOD = swap16(0xB800u);
    int bad = 0;
    uint32_t px;
    draw_setup();
    sm_playing = 1;
    sm_pos[0] = 3;
    sm_redraw_all();
    begin_hook = hook_move_pos;                               /* the first cv_begin of the frame: after the signatures */
    px = frame_px();
    bad += check("playhead: the strip shows the step the frame read (3), not the one the audio stream moved to (9)",
                 host_screen[206 * 240 + 3 * 15 + 5] == BLOOD && host_screen[206 * 240 + 9 * 15 + 5] != BLOOD && begin_hook == 0);
    px = frame_px();
    bad += check("playhead: the next frame sees the move and draws step 9",
                 px > 0 && px <= 240u * 40u && host_screen[206 * 240 + 9 * 15 + 5] == BLOOD && host_screen[206 * 240 + 3 * 15 + 5] != BLOOD);
    return bad;
}
static void write_ppm(const char *dir, const char *name)
{
    char path[256];
    FILE *f;
    uint32_t k;
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    if (!(f = fopen(path, "wb")))
        return;
    fprintf(f, "P6\n240 240\n255\n");
    for (k = 0; k < 240u * 240u; k++) {
        uint16_t v = swap16(host_screen[k]);
        uint8_t rgb[3] = {(uint8_t)((v >> 11) * 255 / 31), (uint8_t)(((v >> 5) & 63) * 255 / 63), (uint8_t)((v & 31) * 255 / 31)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}
/* SHOTS=dir: swamp_<page>.ppm for every page, a playing pattern with some trigs (to look at, no assert) */
static int test_draw_shots(void)
{
    const char *dir = getenv("SHOTS");
    uint32_t pg, k;
    if (!dir)
        return 0;
    for (pg = 0; pg < sm_page_count(); pg++) {
        char n[48], *c;
        draw_setup();
        for (k = 0; k < 16u; k += (k % 4u == 0 ? 2u : 3u))
            smp.pat[smp.cur].step[0][k].n = 1;
        sm_ui.page = (uint8_t)pg;
        sm_ui.trk = (uint8_t)(pg % 2u);
        sm_playing = 1;
        sm_pos[sm_ui.trk] = 5;
        sm_ui.held_step = 9;
        draw_full();
        snprintf(n, sizeof n, "swamp_%s", SM_PAGES[pg].name);
        for (c = n; *c; c++)
            if (*c == ' ' || *c == '.')
                *c = '_';
        write_ppm(dir, n);
    }
    return 0;
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
    bad += test_seq_timing();
    bad += test_seq_tempo_edges();
    bad += test_cmd_coalesce();
    bad += test_cmd_coalesce_race();
    bad += test_step_toggle();
    bad += test_trig_while_playing();
    bad += test_stop_releases();
    bad += test_load_order();
    bad += test_panic();
    bad += test_alg_selects_track();
    bad += test_pages_cycle();
    bad += test_knob_writes_model_and_core();
    bad += test_white_key_toggles_trig();
    bad += test_presets_knob_loads();
    bad += test_held_step();
    bad += test_play_key();
    bad += test_input_backpressure();
    bad += test_cmd_e8_guard();
    bad += test_held_step_bank_key();
    bad += test_draw_lint();
    bad += test_draw_budget();
    bad += test_draw_grain();
    bad += test_draw_playhead_snapshot();
    bad += test_draw_shots();
    printf("swamp: %d failed\n", bad);
    return bad != 0;
}
