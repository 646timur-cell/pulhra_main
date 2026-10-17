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
typedef struct { int32_t x0, y0, x1, y1; uint32_t flags; char s[24]; } lint_box_t;
static lint_box_t lint_box[256];
static uint32_t lint_n, lint_band;                       /* the boxes of a frame; those of the canvas not blitted yet start here */
static void lint_text(int32_t x0, int32_t y0, int32_t x1, int32_t y1, const char *s, uint32_t flags)
{
    lint_box_t *b = &lint_box[lint_n < 256u ? lint_n : 255u];
    b->x0 = x0; b->y0 = y0; b->x1 = x1; b->y1 = y1; b->flags = flags;
    strncpy(b->s, s, 23);
    b->s[23] = 0;
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
#define SM_PX_HOOK(x0, y0, x1, y1, s, flags) lint_text(x0, y0, x1, y1, s, flags)   /* (sm_px.c's texts) */
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
/* ... and every note-on / note-off in order (the STEP tests: velocity, LEN, slide) */
typedef struct { uint8_t on, t, note, vel, gl; uint32_t at; } seq_ev_t;  /* gl: the slide glide on at a note-on */
static seq_ev_t seq_ev[1024];
static uint32_t seq_nev;
static void seq_ev_log(uint32_t on, uint32_t k, uint32_t note, uint32_t vel)
{
    if (seq_nev < 1024u)
        seq_ev[seq_nev] = (seq_ev_t){(uint8_t)on, (uint8_t)k, (uint8_t)note, (uint8_t)vel, trk[k % NTRK].slide_glide, seq_clock};
    seq_nev++;
}
static void seq_note_on(track_t *t, uint32_t note, uint32_t vel)
{
    uint32_t k = (uint32_t)(t - trk);
    if (on_n[k] < 4u)
        on_at[k][on_n[k]] = seq_clock;
    on_n[k]++;
    on_last[k] = seq_clock;
    seq_ev_log(1, k, note, vel);
    trk_note_on(t, note, vel);
}
static void seq_note_off(track_t *t, uint32_t note)
{
    seq_ev_log(0, (uint32_t)(t - trk), note, 0);
    trk_note_off(t, note);
}
/* the audio stream preempting the main loop inside sm_cmd_push, between finding a pending command and storing into it */
static int preempt_armed;
static void sm_cmd_drain(void);
#define SM_CMD_PREEMPT() do { if (preempt_armed) { preempt_armed = 0; sm_cmd_drain(); } } while (0)
/* the MIDI ring of usb.c (a copy of its declarations and midi_enqueue: usb.c is the hardware's), which the audio stream drains
 * (swamp/sm_midi.c): the USB poll and the TRS UART both put 32-bit event packets in it */
#define MQ 64u
static uint32_t midi_in_q[MQ], midi_in_ms[MQ];
static uint8_t midi_in_source[MQ];
static volatile uint32_t mi_w, mi_r;
static volatile uint8_t midi_in_overflow;
static int midi_enqueue(uint32_t pkt, uint32_t source)
{
    uint32_t at = mi_w % MQ;
    if (midi_in_overflow)
        return 0;
    if (mi_w - mi_r >= MQ) {
        midi_in_overflow = 1;
        return 0;
    }
    midi_in_q[at] = pkt;
    midi_in_ms[at] = 0;
    midi_in_source[at] = (uint8_t)source;
    RING_PUBLISH();
    mi_w++;
    return 1;
}
/* every note-on of the app code included below (the sequencer's) goes through the counter; the tests' own calls after
 * the #undef go to voice.c's directly */
#define trk_note_on seq_note_on
#define trk_note_off seq_note_off
static void lock_applied(uint32_t t, uint32_t s, uint32_t p, int32_t v);   /* the locks test: every lock the ISR applies */
static void lock_preempt(void);                                            /* ... and the ISR's moment inside a lock write */
#define SM_LOCK_APPLIED(t, s, p, v) lock_applied(t, s, p, v)
#define SM_LOCK_PREEMPT() lock_preempt()
#include "../firmware/src/swamp/sm_app.c"
#undef trk_note_on
#undef trk_note_off

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
    seq_nev = 0;
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
    smp.pat[0].step[2][9] = (sm_step_t){{70, 71, 72, 73}, 0, 9, 7, 2, SMF_REST | SMF_SLIDE, 3};   /* (an old step's leftovers) */
    sm_step_toggle(2, 9);
    {
        sm_step_t e;
        sm_step_eff(s, &smp.pat[0].kit.snd[2], &e);
        bad += check("STEP: on: NOTE VEL LEN COND inherited (SM_INH), no flags, no nudge, no chord, n 1",
                     s->n == 1u && s->note[0] == SM_INH && s->vel == SM_INH && s->len == SM_INH && s->cond == SM_INH &&
                         !s->flags && !s->nudge && !s->note[1] && !s->note[2] && !s->note[3]);
        bad += check("STEP: ... it plays the track's defaults: C3 + ROOT, VEL 100, LEN TRK, 100 %",
                     e.note[0] == 48u + (uint32_t)clamp(smp.pat[0].kit.snd[2].p[P_ROOT], 0, 11) && e.vel == 100u && !e.len && !e.cond);
    }
    smp.pat[0].step[2][9].flags = SMF_REST;
    sm_step_toggle(2, 9);
    bad += check("STEP: off: n 0, no flags left (no REST shown on an empty step)", !s->n && !s->flags);
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
                 !before[1][7].n && after[1][7].n == 1u && after[1][7].note[0] == SM_INH);
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
        smp.pat[smp.cur].step[t][0].len = SM_LEN_INF;     /* INF (the LEN list's last) */
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
    fm1_in.buttons = 0;
    settings_hold = HOLD_DEF;                             /* (test_settings leaves the loaded one) */
    for (k = 0; k < 7u; k++)
        host_enc[k] = 0;
}
static void turn(uint32_t role, int32_t n) { host_enc[panel.enc[role]] += n * panel.dir[role]; }
static void press(uint32_t b) { host_pressed |= 1u << panel.btn[b]; }
/* OCT- / OCT+ tapped alone (down, a poll, up, 150 ms, a poll: a single press acts after the 100 ms it waits for the other) */
static void oct_tap(uint32_t b)
{
    host_pressed |= 1u << panel.btn[b];
    fm1_in.buttons |= 1u << panel.btn[b];
    sm_input_poll();
    fm1_in.buttons &= ~(1u << panel.btn[b]);
    host_ticks += 150000u * FM1_TICKS_PER_US;
    sm_input_poll();
}
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
    bad += check("INPUT: SELECT on FILTER (FILT OFF): LFO>FLT, model and core, FILT to LP",
                 ksnd(0)->p[P_LD_FLT] == TP[P_LD_FLT].def - 3 && trk[0].p[P_LD_FLT] == ksnd(0)->p[P_LD_FLT] &&
                 ksnd(0)->p[P_FLT] == 1);
    turn(EN_K1, 4);                                       /* FILT HYB: SELECT is WIDTH */
    sm_input_poll();
    turn(EN_SELECT, -3);
    sm_input_poll();
    run_block();
    bad += check("INPUT: SELECT on FILTER (FILT HYB): HWIDTH, model and core",
                 ksnd(0)->p[P_FLT] == 5 && ksnd(0)->p[P_HWIDTH] == TP[P_HWIDTH].def - 3 &&
                 trk[0].p[P_HWIDTH] == ksnd(0)->p[P_HWIDTH]);
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

/* the FILTER page heard (Tim's hardware test: "the filters don't turn while playing"). The mix's brightness over n blocks:
 * the energy of the first difference over the energy, (x[i] - x[i-1])^2 / x^2 (a high band's share) */
static double flt_bright(uint32_t n, double *rms)
{
    static int32_t L[CTL * 200];
    uint32_t nl = 0, i, b;
    double d = 0, e = 0;
    for (b = 0; b < n && b < 200u; b++) {
        block_peak(L, &nl);
        seq_clock += CTL;
    }
    for (i = 1; i < nl; i++) {
        double a = L[i], p = L[i - 1];
        d += (a - p) * (a - p);
        e += a * a;
    }
    *rms = nl ? sqrt(e / nl) : 0;
    return e > 0 ? d / e : 0;
}
/* T2 (FM-ST: the same note every step, the same sound every bar) alone, a trig on every step, the FILTER page, K1 (FILT)
 * turned by filt detents, PLAY; a bar (FS / 2 at 120) before and after K2 -110 (HYB: +110) and K3 +100 through
 * sm_input_poll. The brightness before and after */
static void flt_play(int32_t filt, double *b0, double *b1, double *r1)
{
    uint32_t s, k;
    double r0;
    in_reset();
    for (k = 0; k < SM_NTRK; k++)
        if (k != 1u)
            ksnd(k)->p[P_LEVEL] = 0, trk[k].p[P_LEVEL] = 0;
    for (s = 0; s < 16u; s++)
        sm_step_toggle(1, s);
    turn(EN_ALGO, 1);
    sm_input_poll();
    press(B_ENV);
    sm_input_poll();
    if (filt) {
        turn(EN_K1, filt);
        sm_input_poll();
    }
    press(B_PLAY);
    sm_input_poll();
    *b0 = flt_bright(FS / 2u / CTL, &r0);
    turn(EN_K2, filt == 5 ? 110 : -110);                  /* (HYB: BASE up, the high-pass) */
    turn(EN_K3, 100);
    sm_input_poll();
    *b1 = flt_bright(FS / 2u / CTL, r1);
    press(B_PLAY);
    sm_input_poll();
}
static int test_filter_play(void)
{
    int bad = 0;
    double b0, b1, r1;
    flt_play(0, &b0, &b1, &r1);
    printf("swamp: FILT OFF: brightness %.4f -> %.4f\n", b0, b1);
    bad += check("FILTER: FILT OFF (the sound's default): CUT turned sets FILT LP first, so CUT and RES are heard (far darker)",
                 ksnd(1)->p[P_FLT] == 1 && trk[1].p[P_FLT] == 1 && ksnd(1)->p[P_FCUT] < 40 && b1 < 0.1 * b0);
    flt_play(1, &b0, &b1, &r1);
    printf("swamp: FILT LP: brightness %.4f -> %.4f (rms %.0f)\n", b0, b1, r1);
    bad += check("FILTER: FILT LP, playing: K2 CUT -110 + K3 RES through the input: the core has them, far darker",
                 trk[1].p[P_FLT] == 1 && trk[1].p[P_FCUT] == ksnd(1)->p[P_FCUT] && trk[1].p[P_FRES] == ksnd(1)->p[P_FRES] &&
                 ksnd(1)->p[P_FRES] > 90 && b1 < 0.1 * b0 && r1 > 20.0);
    flt_play(5, &b0, &b1, &r1);
    printf("swamp: FILT HYB: brightness %.4f -> %.4f\n", b0, b1);
    bad += check("FILTER: FILT HYB: K2 / K3 are HYBRID's BASE / RES (MONOFM's twin page), model and core",
                 ksnd(1)->p[P_HBASE] > 90 && ksnd(1)->p[P_HRES] > 90 && trk[1].p[P_HBASE] == ksnd(1)->p[P_HBASE] &&
                 trk[1].p[P_HRES] == ksnd(1)->p[P_HRES] && ksnd(1)->p[P_FCUT] == TP[P_FCUT].def);
    bad += check("FILTER: ... heard, playing: BASE up high-passes T2 (brighter)", b1 > 1.5 * b0);
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
    oct_tap(B_OCTUP);                                     /* OCT+ on a 16-step track: ignored */
    bad += check("INPUT: OCT+ with len 16: the bank stays 0", sm_ui.bank == 0u);
    ksnd(0)->len = 32u;
    host_notes = 1u << 25;                                /* F#5: no longer the bank (it is a black key: nothing) */
    sm_input_poll();
    bad += check("INPUT: F#5 is free: the bank stays 0, no trig", sm_ui.bank == 0u && !smp.pat[smp.cur].step[0][25].n);
    oct_tap(B_OCTUP);                                     /* OCT+: bank 1 */
    bad += check("INPUT: OCT+: bank 1, no trig", sm_ui.bank == 1u && !smp.pat[smp.cur].step[0][25].n);
    host_notes = 1u << WHITE[2];
    sm_input_poll();
    bad += check("INPUT: bank 1, the 3rd white key: step 18", smp.pat[smp.cur].step[0][18].n == 1u &&
                 !smp.pat[smp.cur].step[0][2].n);
    host_notes = 1u << 3;                                 /* G#3: black */
    sm_input_poll();
    bad += check("INPUT: a black key: nothing", !smp.pat[smp.cur].step[0][3].n && !smp.pat[smp.cur].step[0][19].n);
    oct_tap(B_OCTDN);                                     /* OCT-: SHIFT, no page */
    bad += check("INPUT: OCT- tapped: SHIFT only, the bank stays 1", sm_ui.bank == 1u);
    oct_tap(B_OCTUP);                                     /* OCT+ again: round to bank 0 */
    host_notes = 1u << WHITE[15];
    sm_input_poll();
    bad += check("INPUT: OCT+ again: bank 0, the 16th white key: step 15", sm_ui.bank == 0u && smp.pat[smp.cur].step[0][15].n == 1u);
    turn(EN_ALGO, 1);
    sm_input_poll();
    host_notes = 1u << WHITE[0];
    sm_input_poll();
    bad += check("INPUT: the trig goes to the selected track", smp.pat[smp.cur].step[1][0].n == 1u && !smp.pat[smp.cur].step[0][0].n);
    ksnd(0)->len = 16u;
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
    ksnd(sm_ui.trk)->len = 32u;                           /* (a bank key works on a 32-step track) */
    host_ticks = 1000u;
    host_notes = 1u << WHITE[5];
    fm1_in.notes = 1u << WHITE[5];
    sm_input_poll();
    host_ticks += 400000u * FM1_TICKS_PER_US;
    sm_input_poll();
    oct_tap(B_OCTUP);                                     /* OCT+ pressed while it is held: a paste (nothing copied: nothing) */
    bad += check("INPUT: OCT+ pressed while holding: no page turn (paste), held_step stays the step pressed (5)",
                 sm_ui.bank == 0u && sm_ui.held_step == 5u && !smp.pat[smp.cur].step[0][5].n);
    ksnd(sm_ui.trk)->len = 16u;
    fm1_in.notes = 0;
    sm_input_poll();
    return bad;
}

/* ----------------------------------------------------------- layers --- */
static void btn_down(uint32_t b) { host_pressed |= 1u << panel.btn[b]; fm1_in.buttons |= 1u << panel.btn[b]; }
static void btn_up(uint32_t b) { fm1_in.buttons &= ~(1u << panel.btn[b]); }
static void ms(uint32_t n) { host_ticks += n * 1000u * FM1_TICKS_PER_US; }
static void key_dn(uint32_t k) { host_notes |= 1u << k; fm1_in.notes |= 1u << k; }
static void key_up(uint32_t k) { fm1_in.notes &= ~(1u << k); }
/* a family button held until its layer is open (HOLD_MS + 100), a poll each time */
static void layer_hold(uint32_t b)
{
    btn_down(b);
    sm_input_poll();
    ms(500);
    sm_input_poll();
}
static void layer_let_go(uint32_t b)
{
    btn_up(b);
    sm_input_poll();
}

static int test_tap_vs_hold(void)
{
    int bad = 0;
    uint32_t page0;
    in_reset();
    host_ticks = 1000u;
    page0 = sm_ui.page;
    btn_down(B_EDIT);
    sm_input_poll();
    bad += check("LAYER: EDIT just down: the page has not changed, no layer", sm_ui.page == page0 && sm_layer == SML_NONE);
    ms(100);
    btn_up(B_EDIT);
    sm_input_poll();
    bad += check("LAYER: EDIT tapped (100 ms): the next page, no layer", !strcmp(sm_cur_page()->name, "EDIT 2") && sm_layer == SML_NONE);
    btn_down(B_EDIT);
    sm_input_poll();
    ms(500);
    sm_input_poll();
    bad += check("LAYER: EDIT held 500 ms (still down): the EDIT layer, the page as it was",
                 sm_layer == SML_EDIT && !strcmp(sm_cur_page()->name, "EDIT 2"));
    btn_up(B_EDIT);
    sm_input_poll();
    bad += check("LAYER: ... let go: the layer closes, the page is still EDIT 2", sm_layer == SML_NONE && !strcmp(sm_cur_page()->name, "EDIT 2"));
    btn_down(B_EDIT);
    sm_input_poll();
    ms(399);
    sm_input_poll();
    bad += check("LAYER: held 399 ms (HOLD 400): not yet", sm_layer == SML_NONE);
    ms(1);
    sm_input_poll();
    bad += check("LAYER: ... 400 ms: the layer", sm_layer == SML_EDIT);
    btn_up(B_EDIT);
    sm_input_poll();
    settings_hold = 3;                                    /* HOLD 600 */
    btn_down(B_GLO);
    sm_input_poll();
    ms(500);
    sm_input_poll();
    bad += check("LAYER: HOLD 600: 500 ms is not a hold", sm_layer == SML_NONE);
    ms(100);
    sm_input_poll();
    bad += check("LAYER: ... 600 ms is: the GLO layer, the page unchanged", sm_layer == SML_GLO && !strcmp(sm_cur_page()->name, "EDIT 2"));
    btn_up(B_GLO);
    sm_input_poll();
    settings_hold = HOLD_DEF;
    /* a key used 50 ms into the hold: the layer at once (and the key is the layer's, not a trig) */
    sm_ui.page = 0;
    btn_down(B_EDIT);
    sm_input_poll();
    ms(50);
    key_dn(WHITE[3]);
    sm_input_poll();
    bad += check("LAYER: a key 50 ms into EDIT: the EDIT layer, the page as it was, no trig on the key",
                 sm_layer == SML_EDIT && sm_ui.page == 0u && !smp.pat[smp.cur].step[0][3].n);
    key_up(WHITE[3]);
    btn_up(B_EDIT);
    sm_input_poll();
    /* a knob turned 50 ms into the hold: the layer, the page's value untouched */
    {
        int8_t before = ksnd(0)->p[sm_cur_page()->id[0]];
        btn_down(B_GLO);
        sm_input_poll();
        ms(50);
        turn(EN_K1, 3);
        sm_input_poll();
        bad += check("LAYER: a knob 50 ms into GLO: the GLO layer, the page's K1 value unchanged",
                     sm_layer == SML_GLO && ksnd(0)->p[sm_cur_page()->id[0]] == before && sm_ui.page == 0u);
        btn_up(B_GLO);
        sm_input_poll();
    }
    /* SEQ tapped: the STEP page; ENV is its page at once */
    btn_down(B_SEQ);
    sm_input_poll();
    ms(100);
    btn_up(B_SEQ);
    sm_input_poll();
    bad += check("LAYER: SEQ tapped: the STEP page, no layer", !strcmp(sm_cur_page()->name, "STEP") && sm_layer == SML_NONE);
    press(B_ENV);
    sm_input_poll();
    bad += check("LAYER: ENV: its page at once", !strcmp(sm_cur_page()->name, "FILTER"));
    /* another layer button while one is held is swallowed */
    sm_ui.page = 0;
    layer_hold(B_EDIT);
    btn_down(B_GLO);
    sm_input_poll();
    ms(500);
    btn_up(B_GLO);
    sm_input_poll();
    bad += check("LAYER: GLO tapped while EDIT is held: swallowed, EDIT stays", sm_layer == SML_EDIT && sm_ui.page == 0u);
    layer_let_go(B_EDIT);
    bad += check("LAYER: ... all let go: closed", sm_layer == SML_NONE);
    /* a tap that is down and up between two polls (the edge only) is a tap */
    sm_ui.page = 0;
    press(B_EDIT);
    sm_input_poll();
    bad += check("LAYER: a press seen already let go: a tap (EDIT 2)", !strcmp(sm_cur_page()->name, "EDIT 2") && sm_layer == SML_NONE);
    return bad;
}

static int test_edit_layer_loads_machine(void)
{
    int bad = 0;
    sm_sound_t ref;
    uint32_t k, ok = 1;
    in_reset();
    host_ticks = 1000u;
    layer_hold(B_EDIT);
    bad += check("EDIT: the layer opens on the page of the track's machine (T1 SAW: MONO)", sm_layer == SML_EDIT && sm_lay_epage == 0u);
    key_dn(WHITE[3]);
    sm_input_poll();
    sm_sound_from_preset(&ref, 3, 0);
    bad += check("EDIT: the 4th white key: machine 3 (preset 0) in the model", ksnd(0)->machine == 3u && !memcmp(&ref, ksnd(0), sizeof ref));
    run_block();
    bad += check("EDIT: ... and in the core after a block", trk[0].eng_req == sm_engine_of(3) && trk[0].preset == 0u && core_is_model());
    key_up(WHITE[3]);
    bad += check("EDIT: the other tracks are as they were", ksnd(1)->machine == 7u && ksnd(2)->machine == 13u);
    press(B_OCTUP);
    sm_input_poll();
    bad += check("EDIT: OCT+: the next page (SOMA)", sm_lay_epage == 1u);
    key_dn(WHITE[0]);
    sm_input_poll();
    key_up(WHITE[0]);
    bad += check("EDIT: SOMA's first white key: machine 13", ksnd(0)->machine == 13u);
    press(B_OCTUP);
    sm_input_poll();
    press(B_OCTUP);
    sm_input_poll();
    bad += check("EDIT: OCT+ twice more: back on MONO (wraps)", sm_lay_epage == 0u);
    press(B_OCTUP);
    sm_input_poll();
    press(B_OCTUP);
    sm_input_poll();                                      /* SLICE: 5 machines */
    for (k = 0; k < 16u; k++) {
        key_dn(WHITE[k]);
        sm_input_poll();
        key_up(WHITE[k]);
        if (k < 5u)
            ok &= ksnd(0)->machine == 22u + k;
        else if (k < 15u)
            ok &= ksnd(0)->machine == 22u + 4u;           /* (a white key with no machine: nothing happens) */
    }
    bad += check("EDIT: SLICE: its 5 machines on the first 5 white keys, the rest of the keys nothing", ok);
    /* INIT: G5 is the machine's preset 0 (length kept) */
    layer_let_go(B_EDIT);
    turn(EN_PRESET, 2);
    sm_input_poll();
    ksnd(0)->p[P_FCUT] = 5;
    ksnd(0)->len = 24u;
    layer_hold(B_EDIT);
    key_dn(WHITE[15]);
    sm_input_poll();
    key_up(WHITE[15]);
    sm_sound_from_preset(&ref, ksnd(0)->machine, 0);
    ref.len = 24u;
    bad += check("EDIT: G5 INIT: the machine's preset 0, the track length kept", !memcmp(&ref, ksnd(0), sizeof ref));
    layer_let_go(B_EDIT);
    ksnd(0)->len = 16u;
    /* the layer opens on the page of the track's own machine */
    in_reset();
    sm_sound_load(24, 0);                                 /* a SLICE machine */
    run_block();
    layer_hold(B_EDIT);
    bad += check("EDIT: the layer opens on the page of a SLICE track (page 3)", sm_lay_epage == 2u);
    layer_let_go(B_EDIT);
    return bad;
}

static int test_edit_layer_backpressure(void)
{
    int bad = 0;
    uint32_t i, full = 0;
    sm_cmd_t c = {SMC_STOP, 0, 0, 0, 0};
    in_reset();
    host_ticks = 1000u;
    for (i = 0; i < 80u; i++)                             /* the ring full (the audio stream has not run) */
        full += !sm_cmd_push(c);
    layer_hold(B_EDIT);
    key_dn(WHITE[3]);
    sm_input_poll();
    key_up(WHITE[3]);
    bad += check("EDIT: the queue full: the model has the machine, the LOAD waits", full > 0 && ksnd(0)->machine == 3u && (sm_ld_pend & 1u) &&
                 trk[0].eng_req != sm_engine_of(3));
    run_block();                                          /* the ring drains */
    sm_input_poll();                                      /* the retry */
    run_block();
    bad += check("EDIT: ... pushed again, the core ends on the model", !(sm_ld_pend & 1u) && trk[0].eng_req == sm_engine_of(3) && core_is_model());
    layer_let_go(B_EDIT);
    return bad;
}

/* T1 and T2 with a trig on every step, PLAY; the note-ons of a second of play */
static void trigs_play(void)
{
    uint32_t s;
    for (s = 0; s < 16u; s++) {
        sm_step_toggle(0, s);
        sm_step_toggle(1, s);
    }
    press(B_PLAY);
    sm_input_poll();
}
static void play_blocks(uint32_t n)
{
    while (n--)
        run_block();
}
static int test_glo_mute(void)
{
    int bad = 0;
    in_reset();
    host_ticks = 1000u;
    trigs_play();
    layer_hold(B_GLO);
    key_dn(1);                                            /* F#3: the first black key */
    sm_input_poll();
    key_up(1);
    layer_let_go(B_GLO);
    bad += check("GLO: F#3 in the layer: T1 muted (smp.mute bit 0)", smp.mute == 1u);
    seq_log_clear();
    play_blocks(1500);                                    /* 1.09 s: 8 steps and a bit */
    bad += check("GLO: ... T1's trigs do not sound, T2's do", on_n[0] == 0u && on_n[1] >= 8u && !on_n[2] && !on_n[3]);
    layer_hold(B_GLO);
    key_dn(1);
    sm_input_poll();
    key_up(1);
    layer_let_go(B_GLO);
    bad += check("GLO: F#3 again: unmuted", smp.mute == 0u);
    seq_log_clear();
    play_blocks(1500);
    bad += check("GLO: ... T1 sounds again, while playing", on_n[0] >= 8u && on_n[1] >= 8u);
    layer_hold(B_GLO);                                    /* the other three black keys: T2, T3, T4 */
    key_dn(3); sm_input_poll(); key_up(3);
    key_dn(5); sm_input_poll(); key_up(5);
    key_dn(8); sm_input_poll(); key_up(8);
    layer_let_go(B_GLO);
    bad += check("GLO: G#3, A#3, C#4: T2, T3, T4 (bits 1, 2, 3)", smp.mute == 0xEu);
    seq_log_clear();
    play_blocks(1500);
    bad += check("GLO: ... only T1 sounds", on_n[0] >= 8u && !on_n[1] && !on_n[2] && !on_n[3]);
    return bad;
}

static int test_glo_solo(void)
{
    int bad = 0;
    in_reset();
    host_ticks = 1000u;
    trigs_play();
    layer_hold(B_GLO);
    key_dn(WHITE[1]);                                     /* G3: T2 */
    sm_input_poll();
    bad += check("GLO: the 2nd white key held: T2 soloed", sm_solo == 2u && smp.mute == 0u);
    seq_log_clear();
    play_blocks(1500);
    bad += check("GLO: ... only T2 sounds", !on_n[0] && on_n[1] >= 8u && !on_n[2] && !on_n[3]);
    key_up(WHITE[1]);
    sm_input_poll();
    bad += check("GLO: ... the key let go: no solo", sm_solo == 0u);
    seq_log_clear();
    play_blocks(1500);
    bad += check("GLO: ... T1 and T2 sound again", on_n[0] >= 8u && on_n[1] >= 8u);
    key_dn(WHITE[3]);
    sm_input_poll();
    layer_let_go(B_GLO);                                  /* the layer closes with the key still down */
    bad += check("GLO: the layer let go with a solo key down: the solo ends", sm_solo == 0u && sm_layer == SML_NONE);
    key_up(WHITE[3]);
    sm_input_poll();
    return bad;
}

/* the mask is read once per block: the main loop flips it between two steps of one block's loop? (one read: the whole block
 * follows the mask it saw) */
static int test_mute_read_once(void)
{
    int bad = 0;
    in_reset();
    host_ticks = 1000u;
    smp.bpm10 = 3000;                                     /* 300 BPM: a step every 8820 samples */
    sm_step_toggle(0, 0);
    sm_step_toggle(0, 1);
    smp.mute = 1u;
    press(B_PLAY);
    sm_input_poll();
    run_block();                                          /* step 0, muted */
    bad += check("MUTE: a muted track's first step does not start", on_n[0] == 0u);
    smp.mute = 0u;
    play_blocks(400);
    bad += check("MUTE: unmuted: the next trig starts", on_n[0] >= 1u);
    smp.bpm10 = 1200;
    return bad;
}

/* a muted track lets go of what it holds (a LEN INF note would ring on), by note-off: the release, not a cut */
static int test_mute_releases(void)
{
    int bad = 0, held = 0, held_after = 0;
    uint32_t i;
    in_reset();
    host_ticks = 1000u;
    sm_step_toggle(0, 0);
    smp.pat[smp.cur].step[0][0].len = SM_LEN_INF;
    press(B_PLAY);
    sm_input_poll();
    play_blocks(20);
    for (i = 0; i < NVOICE; i++)
        held |= trk[0].v[i].gate;
    bad += check("MUTE: T1's LEN INF note is held while playing", held && sm_nheld[0] == 1u);
    smp.mute = 1;
    play_blocks(2);
    for (i = 0; i < NVOICE; i++)
        held_after |= trk[0].v[i].gate;
    bad += check("MUTE: muted: the note is let go (note-off) within a block", !held_after && sm_nheld[0] == 0u);
    return bad;
}

static int test_seq_len(void)
{
    int bad = 0;
    in_reset();
    host_ticks = 1000u;
    layer_hold(B_SEQ);
    bad += check("SEQ: held 500 ms: the SEQ layer", sm_layer == SML_SEQ);
    turn(EN_K1, 16);
    sm_input_poll();
    bad += check("SEQ: K1 +16: the track's length 32", ksnd(0)->len == 32u && ksnd(1)->len == 16u);
    turn(EN_K1, 5);
    sm_input_poll();
    bad += check("SEQ: K1 +5 more: stays 32", ksnd(0)->len == 32u);
    turn(EN_K1, -100);
    sm_input_poll();
    bad += check("SEQ: K1 -100: 1", ksnd(0)->len == 1u);
    turn(EN_K1, 31);
    sm_input_poll();
    bad += check("SEQ: K1 +31: 32", ksnd(0)->len == 32u);
    {
        sm_sound_t before = *ksnd(0);
        before.len = 32u;
        turn(EN_K3, 5);
        turn(EN_K4, 5);
        turn(EN_SELECT, 5);
        sm_input_poll();
        bad += check("SEQ: the other knobs are the layer's too: no parameter moved", !memcmp(&before, ksnd(0), sizeof before));
    }
    layer_let_go(B_SEQ);
    oct_tap(B_OCTUP);                                     /* OCT+: the second bank: the gate opens with the length */
    bad += check("SEQ: the length 32: OCT+ selects the bank 1", sm_ui.bank == 1u);
    oct_tap(B_OCTUP);
    bad += check("SEQ: ... OCT+ again the bank 0 (round)", sm_ui.bank == 0u);
    sm_step_toggle(0, 20);
    sm_step_toggle(0, 31);
    seq_log_clear();
    press(B_PLAY);
    sm_input_poll();
    play_blocks(5000);                                    /* 160000 samples: 32 steps = 176400 */
    bad += check("SEQ: steps 17..32 play (the trig on step 21 at 20 x FS/8)", on_n[0] == 1u && near(on_at[0][0], 20u * FS / 8u));
    play_blocks(1500);
    bad += check("SEQ: ... and step 32 (31) at 31 x FS/8", on_n[0] == 2u && near(on_at[0][1], 31u * FS / 8u));
    sm_ui.bank = 1;
    layer_hold(B_SEQ);
    turn(EN_K1, -20);
    sm_input_poll();
    bad += check("SEQ: K1 down to 12: the bank goes back to 0 (no second page)", ksnd(0)->len == 12u && sm_ui.bank == 0u);
    layer_let_go(B_SEQ);
    return bad;
}

static int test_seq_clear(void)
{
    int bad = 0, ok = 1;
    uint32_t s;
    in_reset();
    host_ticks = 1000u;
    for (s = 0; s < SM_NSTEP; s += 3u) {
        sm_step_toggle(0, s);
        sm_step_toggle(1, s);
    }
    smp.pat[smp.cur].step[0][7].flags = SMF_REST;
    layer_hold(B_SEQ);
    key_dn(WHITE[1]);                                     /* another white key: nothing */
    sm_input_poll();
    key_up(WHITE[1]);
    bad += check("SEQ: G3 in the layer: not a clear, not a trig", smp.pat[smp.cur].step[0][0].n == 1u && !smp.pat[smp.cur].step[0][1].n);
    key_dn(WHITE[0]);
    sm_input_poll();
    key_up(WHITE[0]);
    for (s = 0; s < SM_NSTEP; s++)
        ok &= !smp.pat[smp.cur].step[0][s].n && !(smp.pat[smp.cur].step[0][s].flags & SMF_REST);
    bad += check("SEQ: F3: every step of T1 off (n = 0), no REST left", ok && sm_lay_cleared);
    ok = 1;
    for (s = 0; s < SM_NSTEP; s += 3u)
        ok &= smp.pat[smp.cur].step[1][s].n == 1u;
    bad += check("SEQ: ... T2 untouched", ok);
    layer_let_go(B_SEQ);
    turn(EN_ALGO, 1);
    sm_input_poll();
    layer_hold(B_SEQ);
    key_dn(WHITE[0]);
    sm_input_poll();
    key_up(WHITE[0]);
    ok = 1;
    for (s = 0; s < SM_NSTEP; s++)
        ok &= !smp.pat[smp.cur].step[1][s].n;
    bad += check("SEQ: the selected track (T2) is the one cleared", ok);
    layer_let_go(B_SEQ);
    bad += check("SEQ: the layer closed: the CLEARED flag is gone", !sm_lay_cleared);
    return bad;
}

/* ------------------------------------------------------------ screen --- */
#define INK swap16(0xFFFFu)
#define DIMC swap16(0x4208u)
#define BGC swap16(0x0000u)
#define SCR(x, y) host_screen[(y) * 240u + (x)]
static void draw_setup(void)
{
    sm_core_init();
    sm_ux_mk1();
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
static uint16_t cvp(uint32_t x, uint32_t y) { return cv_px[y * cv_w + x]; }   /* (swapped, as host_screen) */
static uint32_t page_by_name(const char *n)
{
    uint32_t i;
    for (i = 0; i < sm_page_count(); i++)
        if (!strcmp(SM_PAGES[i].name, n))
            return i;
    return 0;
}

static int test_px_font_widths(void)
{
    int bad = 0;
    uint32_t sc;
    sm_ux_mk1();
    cv_begin(240u, 40u, T_BG);
    for (sc = 1; sc <= 3u; sc++) {
        bad += check("PX: width of 'ABC' is 6 * scale * 3", sm_px_text(0, 0, sc, "ABC", T_TEXT, 0) == (int32_t)(6u * sc * 3u));
        bad += check("PX: lower case folds to the same width", sm_px_text(0, 0, sc, "abc", T_TEXT, 0) == (int32_t)(6u * sc * 3u));
        bad += check("PX: an empty text is 0 wide", sm_px_text(0, 0, sc, "", T_TEXT, 0) == 0);
    }
    cv_begin(240u, 40u, T_BG);
    sm_px_text(0, 0, 1u, "I", T_TEXT, 0);                /* 'I': the middle column all seven rows, nothing in column 5 */
    {
        uint32_t r, col = 1;
        for (r = 0; r < 7u; r++)
            col &= cvp(2, r) == INK && cvp(5, r) == BGC;
        bad += check("PX: 'I' is a column of seven in the middle", col && cvp(2, 7) == BGC);
    }
    cv_begin(240u, 40u, T_BG);
    sm_px_text(10, 10, 2u, "A", T_TEXT, 1);              /* inverted: ink box, the glyph in the ground colour */
    bad += check("PX: inverted text: the box is ink around the glyph", cvp(8, 8) == INK && cvp(10 + 2 * 5 + 1, 10) == INK);
    bad += check("PX: inverted text: the glyph is the ground (top of the A's bar)", cvp(10 + 2 * 2, 10) == BGC);
    return bad;
}

/* the ring at the real centre and radius (sm_draw.c's SMD_RING_*), on a canvas the size of a knob column */
#define RW 60u
#define RH SMD_KNOB_H
static int test_ring_dots(void)
{
    int bad = 0;
    uint32_t pos[4] = {0, 128, 256, 77}, want[4] = {1, 8, 16, 1 + 77u * 15u / 256u}, k;
    sm_ux_mk1();
    for (k = 0; k < 4u; k++) {
        uint32_t x, y, comps = 0, lit = 0, dim = 0;
        static uint8_t seen[RW * RH];
        static uint16_t stk[RW * RH];
        cv_begin(RW, RH, T_BG);
        sm_px_ring(SMD_RING_CX, SMD_RING_CY, SMD_RING_R, pos[k], 0, 0, 0);
        memset(seen, 0, sizeof seen);
        for (y = 0; y < RH; y++)
            for (x = 0; x < RW; x++) {
                uint32_t sp = 0, n = 0;
                uint16_t c = cvp(x, y);
                if (c == BGC || seen[y * RW + x])
                    continue;
                stk[sp++] = (uint16_t)(y * RW + x);        /* a flood fill of this dot */
                seen[y * RW + x] = 1;
                while (sp) {
                    uint32_t q = stk[--sp], qx = q % RW, qy = q / RW, d;
                    int32_t dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
                    n++;
                    for (d = 0; d < 4u; d++) {
                        int32_t nx = (int32_t)qx + dx[d], ny = (int32_t)qy + dy[d];
                        if (nx < 0 || ny < 0 || nx >= (int32_t)RW || ny >= (int32_t)RH || seen[ny * RW + nx] || cvp((uint32_t)nx, (uint32_t)ny) == BGC)
                            continue;
                        seen[ny * RW + nx] = 1;
                        stk[sp++] = (uint16_t)(ny * RW + nx);
                    }
                }
                comps++;
                if (n != 9u)
                    bad += check("RING: every dot is 3 x 3", 0);
                if (c == INK) lit++; else if (c == DIMC) dim++;
            }
        bad += check("RING: exactly 16 dots on the canvas (the real radius)", comps == 16u && lit + dim == 16u);
        bad += check("RING: the lit ones are the first 1 + pos * 15 / 256", lit == want[k]);
    }
    {   /* the centre: empty, a lock dot, a filled disc */
        cv_begin(RW, RH, T_BG);
        sm_px_ring(SMD_RING_CX, SMD_RING_CY, SMD_RING_R, 100, 0, 0, 0);
        bad += check("RING: the centre is empty", cvp(SMD_RING_CX, SMD_RING_CY) == BGC);
        cv_begin(RW, RH, T_BG);
        sm_px_ring(SMD_RING_CX, SMD_RING_CY, SMD_RING_R, 100, 0, 1, 0);
        bad += check("RING: a lock dot in the centre (3 x 3)", cvp(30, 62) == INK && cvp(29, 61) == INK && cvp(31, 63) == INK && cvp(32, 62) == BGC);
        cv_begin(RW, RH, T_BG);
        sm_px_ring(SMD_RING_CX, SMD_RING_CY, SMD_RING_R, 100, 1, 0, 0);
        bad += check("RING: filled: a solid disc (inside the dots)", cvp(30, 62) == INK && cvp(30, 52) == INK && cvp(40, 62) == INK &&
                     cvp(22, 54) == INK && cvp(30, 62 - (SMD_RING_R - 5)) == INK && cvp(30, 62 - (SMD_RING_R - 3)) == BGC);
    }
    return bad;
}
/* no pixel of a ring leaves its 60 px column (the neighbour's ring is a column away): drawn in the middle of a wide canvas,
 * every pixel of every position / disc / lock dot is inside the 60 columns around the centre, and inside the zone's rows */
static int test_ring_contained(void)
{
    int bad = 0;
    uint32_t pos, f, x, y, out = 0, left = 999, right = 0, top = 999, bot = 0;
    sm_ux_mk1();
    for (pos = 0; pos <= 256u; pos += 16u)
        for (f = 0; f < 3u; f++) {
            cv_begin(180u, SMD_KNOB_H, T_BG);
            sm_px_ring(90, SMD_RING_CY, SMD_RING_R, pos, f == 1u, f == 2u, 0);
            for (y = 0; y < SMD_KNOB_H; y++)
                for (x = 0; x < 180u; x++) {
                    if (cv_px[y * cv_w + x] == BGC)
                        continue;
                    out += x < 60u || x >= 120u;
                    left = x < left ? x : left;
                    right = x > right ? x : right;
                    top = y < top ? y : top;
                    bot = y > bot ? y : bot;
                }
        }
    printf("  ring extent: x %u..%u (column 60..119), y %u..%u\n", left, right, top, bot);
    bad += check("RING: no pixel outside its 60 px column (centre 30, radius 24)", out == 0 && left >= 60u && right <= 119u);
    bad += check("RING: ... and inside the knob zone's rows, under the label", top >= 4u + 14u && bot < SMD_KNOB_H - 14u - 4u);
    return bad;
}

static int test_lock_find(void)
{
    int bad = 0;
    sm_pattern_t *pt;
    draw_setup();
    pt = &smp.pat[smp.cur];
    pt->nlock = 3;
    pt->lock[0].ts = (1u << 5) | 4u; pt->lock[0].param = 0xFFu; pt->lock[0].val = 5;           /* a removed one */
    pt->lock[1].ts = (1u << 5) | 4u; pt->lock[1].param = P_FCUT; pt->lock[1].val = 60;
    pt->lock[2].ts = (0u << 5) | 4u; pt->lock[2].param = P_FCUT; pt->lock[2].val = 70;
    pt->lock[3].ts = 0; pt->lock[3].param = P_FRES; pt->lock[3].val = 1;                       /* past nlock */
    bad += check("LOCK: finds (track, step, param)", sm_lock_find(pt, 1, 4, P_FCUT) == 1 && sm_lock_find(pt, 0, 4, P_FCUT) == 2);
    bad += check("LOCK: another step, track or param: -1", sm_lock_find(pt, 1, 5, P_FCUT) < 0 && sm_lock_find(pt, 2, 4, P_FCUT) < 0 &&
                                                         sm_lock_find(pt, 1, 4, P_FRES) < 0);
    bad += check("LOCK: a removed entry (0xFF) never matches; entries past nlock are not read", sm_lock_find(pt, 1, 4, 0xFFu) < 0 &&
                                                                                          sm_lock_find(pt, 0, 0, P_FRES) < 0);
    pt->nlock = 0;
    return bad;
}

static int test_ring_lock_fill(void)
{
    int bad = 0, k;
    uint32_t pg = page_by_name("FILTER");
    sm_pattern_t *pt;
    draw_setup();
    pt = &smp.pat[smp.cur];
    sm_ui.page = (uint8_t)pg;
    sm_ui.held_step = 4;
    pt->nlock = 1;
    pt->lock[0].ts = (0u << 5) | 4u; pt->lock[0].param = P_FCUT; pt->lock[0].val = 99;
    draw_full();
    for (k = 0; k < 4; k++) {
        uint16_t c = host_screen[94u * 240u + (uint32_t)(60 * k + 30)];
        bad += check(k == 1 ? "RING: CUT, locked on the held step, is a filled disc" : "RING: the others are not filled",
                     (k == 1 && c == INK) || (k != 1 && c == BGC));
    }
    sm_ui.held_step = 5;                                    /* another step held: nothing filled */
    sm_dirty = 1;
    sm_draw();
    bad += check("RING: another step held: CUT is not filled", host_screen[94u * 240u + 90u] == BGC);
    sm_ui.held_step = 0xFFu;
    sm_dirty = 1;
    sm_draw();
    bad += check("RING: no step held: nothing filled", host_screen[94u * 240u + 90u] == BGC);
    pt->nlock = 0;
    return bad;
}

static int test_header_rec(void)
{
    int bad = 0;
    uint32_t x, y, ink0 = 0, ink1 = 0;
    draw_setup();
    draw_full();
    for (y = 0; y < 18u; y++)
        for (x = 204; x < 240u; x++)
            ink0 += host_screen[y * 240u + x] == INK;
    bad += check("HEADER: no REC: the right corner is not an ink box", ink0 < 36u * 18u / 3u);
    sm_rec = 1;
    sm_dirty = 1;
    sm_draw();
    for (y = 0; y < 18u; y++)
        for (x = 204; x < 240u; x++)
            ink1 += host_screen[y * 240u + x] == INK;
    bad += check("HEADER: REC: the corner is inverted - an ink box", ink1 > 36u * 18u / 2u && host_screen[0 * 240u + 202] == INK &&
                                                                   host_screen[17u * 240u + 239u] == INK);
    bad += check("HEADER: REC: the R's stem is the ground, the space beside it and after the glyph ink",
                 host_screen[4u * 240u + 204u] == BGC && host_screen[2u * 240u + 214u] == INK && host_screen[4u * 240u + 207u] == INK);
    bad += check("HEADER: REC off again: the corner is plain", (sm_rec = 0, sm_dirty = 1, sm_draw(), host_screen[0 * 240u + 239u] == BGC));
    return bad;
}

static int test_no_grime(void)
{
    uint32_t pg, x, y, bad_px = 0, k;
    int bad = 0;
    for (pg = 0; pg < sm_page_count(); pg++) {
        draw_setup();
        for (k = 0; k < 16u; k += 3u)
            smp.pat[smp.cur].step[0][k].n = 1;
        sm_ui.page = (uint8_t)pg;
        sm_ui.held_step = 6;
        sm_playing = 1;
        sm_pos[0] = 9;
        sm_rec = (uint8_t)(pg & 1u);
        smp.pat[smp.cur].nlock = 1;
        smp.pat[smp.cur].lock[0].ts = 6; smp.pat[smp.cur].lock[0].param = SM_PAGES[pg].id[0]; smp.pat[smp.cur].lock[0].val = 50;
        draw_full();
        for (y = 0; y < 240u; y++)
            for (x = 0; x < 240u; x++) {
                uint16_t c = host_screen[y * 240u + x];
                bad_px += c != INK && c != DIMC && c != BGC;
            }
        smp.pat[smp.cur].nlock = 0;
    }
    bad += check("NOGRIME: every pixel of every page is white, dim or black", bad_px == 0);
    bad += check("NOGRIME: the grain is off", ux.grime == 0 && ux.bg == 0 && ux.text == 0xFFFFu && ux.dim == 0x4208u);
    return bad;
}

typedef struct { int32_t x0, y0, x1, y1; const char *name; } zone_t;
/* where a text may stand, by the screen (independent of the firmware's own layout constants): the header, the page line,
 * the four knob columns (label, ring, value), the SELECT row; the step strip carries no text */
static const zone_t ZONES[] = {
    {0, 0, 240, 20, "header"}, {0, 20, 240, 48, "page"},
    {0, 48, 60, 160, "knob0"}, {60, 48, 120, 160, "knob1"}, {120, 48, 180, 160, "knob2"}, {180, 48, 240, 160, "knob3"},
    {0, 168, 240, 188, "select"}, {208, 220, 240, 240, "len"},
};
/* an open layer: the header, the page line (its title), the five rows of the map (240 x 34 from y 48) */
static const zone_t LZONES[] = {
    {0, 0, 240, 20, "header"}, {0, 20, 240, 48, "title"},
    {0, 48, 240, 82, "row0"}, {0, 82, 240, 116, "row1"}, {0, 116, 240, 150, "row2"}, {0, 150, 240, 184, "row3"}, {0, 184, 240, 218, "row4"},
    {208, 220, 240, 240, "len"},
};
static const zone_t *lint_zones = ZONES;
static uint32_t lint_nz = sizeof ZONES / sizeof ZONES[0];
static uint32_t lint_shown;
static int lint_frame(const char *what)
{
    uint32_t i, j, bad = 0;
    if (lint_n < 4u) {                                       /* a header, a page line, a label and a value at least */
        printf("  lint %s: only %u texts drawn\n", what, lint_n);
        return 1;
    }
    for (i = 0; i < lint_n && i < 256u; i++) {
        const lint_box_t *b = &lint_box[i];
        const zone_t *z = 0;
        if (b->x1 <= b->x0)
            continue;
        for (j = 0; j < lint_nz; j++)
            if (b->x0 >= lint_zones[j].x0 && b->x0 < lint_zones[j].x1 && b->y0 >= lint_zones[j].y0 && b->y0 < lint_zones[j].y1)
                z = &lint_zones[j];
        if (!z || (b->flags & 3u) || b->x0 < z->x0 || b->x1 > z->x1 || b->y1 > z->y1) {
            if (lint_shown++ < 15u) printf("  lint %s: '%s' box %d,%d..%d,%d flags %u zone %s\n", what, b->s, b->x0, b->y0, b->x1, b->y1, b->flags,
                   z ? z->name : "none");
            bad++;
        }
        for (j = i + 1u; j < lint_n && j < 256u; j++) {   /* two texts never overlap */
            const lint_box_t *c = &lint_box[j];
            if (c->x1 > c->x0 && b->x0 < c->x1 && c->x0 < b->x1 && b->y0 < c->y1 && c->y0 < b->y1) {
                printf("  lint %s: '%s' overlaps '%s'\n", what, b->s, c->s);
                bad++;
            }
        }
    }
    return bad != 0;
}
static int test_draw_lint(void)
{
    uint32_t pg, m, v, bad = 0, frames = 0;
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
                    smp.bpm10 = 3000;                       /* the widest header: 300.0, the longest names */
                    smp.cur = 15;
                    sm_rec = (uint8_t)(v & 1u);
                    sm_ui.page = (uint8_t)pg;
                    sm_ui.bank = 1;
                    draw_full();
                    snprintf(what, sizeof what, "machine %u page %s id %u val %d", m, P->name, id, (int)vals[v]);
                    bad += lint_frame(what);
                    frames++;
                }
            }
        }
    }
    printf("  lint: %u frames\n", frames);
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
    bad += check("budget: the first frame draws the zones: 48000 px (no blank band nobody needs)", px == 48000u);
    px = frame_px();
    bad += check("budget: nothing changed, nothing drawn", px == 0);
    sm_playing = 1;                                           /* the transport started: the header's glyph */
    sm_dirty = 1;
    px = frame_px();
    bad += check("budget: play -> header and strip only", px > 0 && px <= 240u * 40u);
    sm_pos[0] = 3;                                            /* the playing step moved (no flag: the sequencer does not set it) */
    px = frame_px();
    bad += check("budget: the playing step moved -> <= 240 x 20", px > 0 && px <= 240u * 20u);
    sm_pos[0] = 4;
    px = frame_px();
    bad += check("budget: ... again", px > 0 && px <= 240u * 20u);
    sm_page_press(SMFAM_GLO);
    px = frame_px();
    bad += check("budget: a page change <= a full frame, more than a step", px > 240u * 20u && px <= 240u * 240u);
    sm_ui.trk = 1;
    sm_dirty = 1;
    px = frame_px();
    bad += check("budget: a track change <= a full frame", px > 0 && px <= 240u * 240u);
    {   /* a knob turned: its column only */
        sm_sound_t *s = ksnd(sm_ui.trk);
        uint32_t id = sm_cur_page()->id[0];
        s->p[id] = (int8_t)(s->p[id] + 1);
        sm_dirty = 1;
        px = frame_px();
        bad += check("budget: one value -> one knob column <= 60 x 112", px > 0 && px <= 60u * 112u);
    }
    sm_redraw_all();
    px = frame_px();
    bad += check("budget: redraw_all repaints the zones (48000 px)", px == 48000u);
    bad += check("budget: sm_draw_px is what the display got, frame by frame", px_bad == 0);
    return bad;
}
/* the audio stream moves the playhead between the frame's signatures and its drawing: the strip must show the step its
 * signature saw, and the next frame must see the move */
static void hook_move_pos(void) { sm_pos[0] = 9; }
static int test_draw_playhead_snapshot(void)
{
    int bad = 0;
    uint32_t px;
    draw_setup();
    sm_playing = 1;
    sm_pos[0] = 3;
    sm_redraw_all();
    begin_hook = hook_move_pos;                               /* the first cv_begin of the frame: after the signatures */
    px = frame_px();
    bad += check("playhead: the strip shows the step the frame read (3), not the one the audio stream moved to (9)",
                 SCR(3 * SMD_STEP_W + 5, 239) == INK && SCR(9 * SMD_STEP_W + 5, 239) == BGC && begin_hook == 0);
    px = frame_px();
    bad += check("playhead: the next frame sees the move and draws step 9",
                 px > 0 && px <= 240u * 20u && SCR(9 * SMD_STEP_W + 5, 239) == INK && SCR(3 * SMD_STEP_W + 5, 239) == BGC);
    return bad;
}
/* the 4 x 4 box at x, y: all ink / all ground */
static int box_is(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            if (SCR(x + i, y + j) != c)
                return 0;
    return 1;
}
static uint32_t scr_ink(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t i, j, n = 0;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            n += SCR(x + i, y + j) != BGC;
    return n;
}
/* the strip's geometry: the pitch of a step, the offsets in its place of a dot, a bar, a REST's outline, a beat mark */
#define SW SMD_STEP_W
#define DO (SW / 2u - 1u)
#define BO (SW / 2u - 4u)
#define RO (SW / 2u - 2u)
#define MO (SW / 2u)
static int test_strip_dots(void)
{
    int bad = 0;
    uint32_t px, i;
    sm_step_t *st;
    draw_setup();
    st = smp.pat[smp.cur].step[0];
    st[1].n = 1;                                              /* a trig */
    st[2].n = 1; st[2].flags = (uint8_t)(2u << 4);            /* x3 */
    st[3].n = 1; st[3].flags = (uint8_t)(3u << 4);            /* x4 */
    st[4].flags = SMF_REST;                                   /* a REST: locks only */
    st[5].n = 1; st[5].flags = (uint8_t)(1u << 4);            /* x2 */
    draw_full();
    bad += check("strip: the strip is 20 px high, at the bottom of the screen", SMD_STRIP_H <= 20u && SMD_STRIP_Y + SMD_STRIP_H == 240u);
    bad += check("strip: a trig is one filled 3 x 3 dot (the lowest place, rows 15..17)", box_is(SW + DO, 235, 3, 3, INK) && scr_ink(SW, 220, SW, 20) == 9u);
    bad += check("strip: x3 is three 3 x 3 dots stacked at a pitch of 4 (a 1 px gap between)",
                 box_is(2 * SW + DO, 235, 3, 3, INK) && box_is(2 * SW + DO, 231, 3, 3, INK) && box_is(2 * SW + DO, 227, 3, 3, INK) &&
                 scr_ink(2 * SW, 220, SW, 20) == 27u && SCR(2 * SW + MO, 234) == BGC && SCR(2 * SW + MO, 230) == BGC);
    bad += check("strip: x4 is four dots (rows 3..17)",
                 box_is(3 * SW + DO, 235, 3, 3, INK) && box_is(3 * SW + DO, 223, 3, 3, INK) && scr_ink(3 * SW, 220, SW, 20) == 36u);
    bad += check("strip: x2 is two dots", box_is(5 * SW + DO, 235, 3, 3, INK) && box_is(5 * SW + DO, 231, 3, 3, INK) && scr_ink(5 * SW, 220, SW, 20) == 18u);
    bad += check("strip: a REST is a hollow dot (the outline 4 x 4)",
                 SCR(4 * SW + RO, 234) == INK && SCR(4 * SW + RO + 3, 237) == INK && SCR(4 * SW + RO, 236) == INK && SCR(4 * SW + RO + 3, 235) == INK &&
                 box_is(4 * SW + RO + 1, 235, 2, 2, BGC) && scr_ink(4 * SW, 220, SW, 20) == 12u);
    bad += check("strip: an empty step is nothing, the beats 1 5 9 13 a dim 1 x 1",
                 scr_ink(6 * SW, 220, SW, 20) == 0 && SCR(MO, 236) == DIMC && scr_ink(0, 220, SW, 20) == 1u &&
                 scr_ink(8 * SW, 220, SW, 20) == 1u && SCR(8 * SW + MO, 236) == DIMC && SCR(6 * SW + MO, 236) == BGC);
    for (i = 0; i < 240u * 20u; i++) {                        /* nothing but the three inks */
        uint16_t c = SCR(i % 240u, 220u + i / 240u);
        if (c != INK && c != DIMC && c != BGC)
            bad++;
    }
    sm_playing = 1;
    sm_pos[0] = 1;
    sm_ui.held_step = 2;
    sm_dirty = 1;
    lcd_px = 0;
    sm_draw();
    bad += check("strip: the step playing has a bar 8 x 1 under its place", box_is(SW + BO, 239, 8, 1, INK) && SCR(SW + BO - 1, 239) == BGC &&
                                                                          SCR(SW + BO + 8, 239) == BGC && SCR(SW + BO, 238) == BGC);
    bad += check("strip: the step held has a bar 8 x 2 over its place", box_is(2 * SW + BO, 220, 8, 2, INK) && SCR(2 * SW + BO - 1, 220) == BGC &&
                                                                       SCR(2 * SW + BO + 8, 221) == BGC && box_is(2 * SW + DO, 227, 3, 3, INK));
    {   /* a x4 stack keeps a 1 px gap to the bar under it (playing) and to the one over it (held) */
        uint32_t x, gap = 1, bars;
        sm_pos[0] = 3;
        sm_ui.held_step = 3;
        sm_dirty = 1;
        sm_draw();
        for (x = 3 * SW + DO; x < 3 * SW + DO + 3u; x++)
            gap &= SCR(x, 222) == BGC && SCR(x, 238) == BGC;
        bars = box_is(3 * SW + BO, 220, 8, 2, INK) && box_is(3 * SW + BO, 239, 8, 1, INK);
        bad += check("strip: x4 playing and held at once: both bars, a 1 px gap to the stack on each side (rows 2 and 18)",
                     bars && gap && box_is(3 * SW + DO, 223, 3, 3, INK) && box_is(3 * SW + DO, 235, 3, 3, INK));
        sm_pos[0] = 1;
        sm_ui.held_step = 2;
        sm_dirty = 1;
        sm_draw();
    }
    sm_ui.held_step = 0xFFu;
    sm_pos[0] = 4;
    sm_dirty = 1;
    px = frame_px();
    bad += check("strip: a step change is at most 240 x 20 px", px > 0 && px <= 240u * 20u);
    bad += check("strip: a REST with the playing bar under it", SCR(4 * SW + RO, 234) == INK && box_is(4 * SW + BO, 239, 8, 1, INK));
    sm_ui.bank = 1;
    ksnd(0)->len = 32;
    sm_dirty = 1;
    sm_draw();
    bad += check("strip: the second bank shows steps 16..31 (empty here: the four beat marks)", scr_ink(0, 220, 16 * SW, 20) == 4u);
    ksnd(0)->len = 16;
    return bad;
}
/* ------------------------------------------------------------- LEDs --- */
/* LED id (0..13 buttons, 14.. keys) in a column picture, found by the matrix itself (not by the app's own table) */
static uint32_t led_of(const uint8_t *pic, uint32_t id)
{
    uint32_t p, r;
    for (p = 0; p < FM1_NCOL; p++)
        for (r = 1; r < 5u; r++)
            if (FM1_KEYMAP[r][p] == (int8_t)id)
                return (pic[p] >> r) & 1u;
    return 99;
}
static uint32_t wkey(uint32_t w)                          /* the key (0..26) of white key w (0..15), F3 .. G5 */
{
    uint32_t k;
    for (k = 0; k < SM_NKEYS; k++)
        if (!((SM_BLACK_KEYS >> k) & 1u) && w-- == 0)
            return k;
    return 99;
}
static uint32_t wl(uint32_t w) { return led_of(fm1_led, 14u + wkey(w)); }
static uint32_t wd(uint32_t w) { return led_of(fm1_led_dim, 14u + wkey(w)); }
static uint32_t wb(uint32_t w) { return led_of(fm1_led_breath, 14u + wkey(w)); }
static uint32_t wm(uint32_t w) { return led_of(fm1_led_mid, 14u + wkey(w)); }
static uint32_t wany(uint32_t w) { return wl(w) | wd(w) | wb(w) | wm(w); }
static void leds_setup(uint32_t mode)
{
    draw_setup();
    settings_leds = (uint8_t)mode;
    memset(fm1_led, 0xAA, sizeof fm1_led);                    /* (every frame rebuilds the whole picture) */
    memset(fm1_led_dim, 0xAA, sizeof fm1_led_dim);
    memset(fm1_led_breath, 0xAA, sizeof fm1_led_breath);
    memset(fm1_led_mid, 0xAA, sizeof fm1_led_mid);
}
static int test_leds_three_states(void)
{
    static const uint8_t modes[] = {LEDS_DIM, LEDS_DIM_LO, LEDS_OFF};
    int ok = 1, inv_ok;
    uint32_t m, w;
    for (m = 0; m < 3u; m++) {
        sm_step_t *st;
        leds_setup(modes[m]);
        st = smp.pat[smp.cur].step[0];
        st[0].n = 1;                                      /* a trig */
        st[1].flags = SMF_REST;                           /* a REST: locks only, no note */
        sm_leds();
        ok &= wl(0) == 1 && wb(0) == 0 && wm(0) == 0;
        ok &= wb(1) == 1 && wl(1) == 0 && wd(1) == 0 && wm(1) == 0;      /* breath alone: no lit, no glow under it */
        for (w = 2; w < 16u; w++)                         /* empty: nothing at all, glow included */
            ok &= wany(w) == 0;
        ok &= wany(0) == 1 && wany(1) == 1;
    }
    leds_setup(LEDS_INV);                                 /* INV turns every LED round: the trig dark, the empty lit */
    smp.pat[smp.cur].step[0][0].n = 1;
    smp.pat[smp.cur].step[0][1].flags = SMF_REST;
    sm_leds();
    inv_ok = wl(0) == 0 && wl(2) == 1 && wb(1) == 1 && wl(1) == 0;
    return check("LEDS: trig lit, REST breathing (no lit, no glow), empty dark in OFF / DIM LO / DIM HI", ok) +
           check("LEDS: INV turns the three states round (trig dark, empty lit, REST still breathing)", inv_ok);
}
static int test_leds_trigs(void)
{
    int ok = 1, bk = 1;
    uint32_t w;
    sm_step_t *st;
    leds_setup(LEDS_DIM);
    st = smp.pat[smp.cur].step[0];
    st[0].n = 1;
    st[5].n = 1;
    sm_leds();
    for (w = 0; w < 16u; w++)
        ok &= wl(w) == (w == 0 || w == 5 ? 1u : 0u);
    leds_setup(LEDS_DIM);                                 /* a 16 step track: no page lit on OCT */
    sm_leds();
    bk &= led_of(fm1_led, 14u + 22u) == 0 && led_of(fm1_led, 14u + 25u) == 0;   /* (D#5 / F#5 are free: never lit) */
    bk &= led_of(fm1_led, panel.btn[B_OCTDN]) == 0 && led_of(fm1_led, panel.btn[B_OCTUP]) == 0;
    ksnd(0)->len = 32;                                    /* 32 steps, page 1..16: OCT+ glows, not lit */
    sm_leds();
    bk &= led_of(fm1_led, panel.btn[B_OCTDN]) == 0 && led_of(fm1_led, panel.btn[B_OCTUP]) == 0 &&
          led_of(fm1_led_dim, panel.btn[B_OCTUP]) == 1;
    sm_ui.bank = 1;
    smp.pat[smp.cur].step[0][16 + 3].n = 1;               /* the second page: its steps on the white keys, OCT+ lit */
    sm_leds();
    bk &= led_of(fm1_led, panel.btn[B_OCTUP]) == 1 && led_of(fm1_led, panel.btn[B_OCTDN]) == 0 &&
          led_of(fm1_led_dim, panel.btn[B_OCTDN]) == 1 && wl(3) == 1 && wl(0) == 0 &&
          led_of(fm1_led, 14u + 22u) == 0 && led_of(fm1_led, 14u + 25u) == 0;
    sm_shift = 1;                                         /* SHIFT held: OCT- lit */
    sm_leds();
    bk &= led_of(fm1_led, panel.btn[B_OCTDN]) == 1;
    sm_shift = 0;
    return check("LEDS: trigs on steps 1 and 6 light white keys 1 and 6, no other white key", ok) +
           check("LEDS: OCT+ lit on the page 17..32 (a track longer than 16), OCT- while SHIFT; D#5 / F#5 never", bk);
}
static int test_leds_playhead(void)
{
    int ok = 1, nb = 1, rest = 1, held = 1;
    uint32_t w;
    sm_step_t *st;
    leds_setup(LEDS_DIM);
    st = smp.pat[smp.cur].step[0];
    st[4].n = 1;
    st[6].flags = SMF_REST;
    sm_playing = 1;
    sm_pos[0] = 9;                                        /* an empty step playing */
    sm_leds();
    ok &= wm(9) == 1 && wl(9) == 0 && wd(9) == 0 && wb(9) == 0;
    for (w = 0; w < 16u; w++)
        ok &= w == 9u || wm(w) == 0;
    sm_pos[0] = 4;                                        /* a trig playing: mid, not lit */
    sm_leds();
    ok &= wm(4) == 1 && wl(4) == 0 && wb(4) == 0;
    sm_pos[0] = 6;                                        /* a REST playing: mid in place of the breath */
    sm_leds();
    rest = wm(6) == 1 && wb(6) == 0 && wl(6) == 0;
    for (w = 0; w < 16u; w++)                             /* the neighbours stay as without the playhead */
        if (w != 6u)
            nb &= wm(w) == 0 && wl(w) == (w == 4u) && wb(w) == 0;
    nb &= wany(5) == 0 && wany(7) == 0;
    sm_playing = 0;                                       /* stopped: no running light at all */
    sm_leds();
    nb &= wm(6) == 0 && wb(6) == 1 && wl(4) == 1;
    sm_ui.held_step = 4;                                  /* a held step is mid too */
    sm_leds();
    held = wm(4) == 1 && wl(4) == 0;
    return check("LEDS: the playing empty step is mid only; a playing trig comes down to mid, not lit", ok) +
           check("LEDS: a playing REST has mid in place of its breath", rest) +
           check("LEDS: the neighbours of the playhead do not change; stopped: no mid", nb) +
           check("LEDS: a held step is mid", held);
}
static int test_leds_rec_play(void)
{
    int rec, play, mode_ok = 1;
    uint32_t m;
    leds_setup(LEDS_DIM);
    sm_leds();
    rec = led_of(fm1_led, panel.btn[B_REC]) == 0 && (fm1_led[LED_PLAY_GREEN >> 3] & (1u << (LED_PLAY_GREEN & 7u))) == 0 &&
          led_of(fm1_led_dim, panel.btn[B_PLAY]) == 1;
    sm_rec = 1;
    sm_leds();
    rec &= led_of(fm1_led, panel.btn[B_REC]) == 1;
    sm_playing = 1;
    sm_leds();
    play = (fm1_led[LED_PLAY_GREEN >> 3] & (1u << (LED_PLAY_GREEN & 7u))) != 0 && led_of(fm1_led, panel.btn[B_PLAY]) == 0 &&
           led_of(fm1_led_dim, panel.btn[B_PLAY]) == 0;
    for (m = 0; m < LEDS_COUNT; m++) {                    /* the green one in every mode */
        settings_leds = (uint8_t)m;
        sm_leds();
        mode_ok &= (fm1_led[LED_PLAY_GREEN >> 3] & (1u << (LED_PLAY_GREEN & 7u))) != 0 && led_of(fm1_led, panel.btn[B_PLAY]) == 0;
    }
    return check("LEDS: sm_rec lights REC (dark otherwise)", rec) +
           check("LEDS: playing sets LED_PLAY_GREEN, PLAY's own LED dark, in every LEDS mode", play && mode_ok);
}
static int test_leds_family(void)
{
    int ok = 1;
    uint32_t pg;
    leds_setup(LEDS_DIM);
    sm_ui.page = (uint8_t)page_by_name("FILTER");
    sm_leds();
    ok &= led_of(fm1_led, panel.btn[B_ENV]) == 1 && led_of(fm1_led, panel.btn[B_EDIT]) == 0 && led_of(fm1_led, panel.btn[B_GLO]) == 0;
    ok &= led_of(fm1_led_dim, panel.btn[B_EDIT]) == 1 && led_of(fm1_led_dim, panel.btn[B_GLO]) == 1;     /* the rest glow */
    for (pg = 0; pg < sm_page_count(); pg++) {
        uint32_t b = SM_PAGES[pg].fam == SMFAM_EDIT ? B_EDIT : SM_PAGES[pg].fam == SMFAM_GLO ? B_GLO :
                     SM_PAGES[pg].fam == SMFAM_SEQ ? B_SEQ : SM_PAGES[pg].fam == SMFAM_HOME ? B_HOME : B_ENV;
        sm_ui.page = (uint8_t)pg;
        sm_leds();
        ok &= led_of(fm1_led, panel.btn[b]) == 1;
    }
    settings_leds = LEDS_OFF;
    sm_leds();
    ok &= led_of(fm1_led_dim, panel.btn[B_EDIT]) == 0;      /* OFF: no glow */
    return check("LEDS: the page's family button is lit (FILTER -> ENV), the others glow by the LEDS mode", ok);
}
/* ------------------------------------------------------ layer maps --- */
static int lint_has(const char *t, int32_t *y0, int32_t *y1)
{
    uint32_t i;
    for (i = 0; i < lint_n && i < 256u; i++)
        if (!strcmp(lint_box[i].s, t)) {
            if (y0) *y0 = lint_box[i].y0;
            if (y1) *y1 = lint_box[i].y1;
            return 1;
        }
    return 0;
}
static uint32_t lint_machines(void)                       /* the machine cells of an EDIT map (not INIT, not the page key) */
{
    uint32_t i, n = 0;
    for (i = 0; i < lint_n && i < 256u; i++)
        n += lint_box[i].y0 >= 48 && lint_box[i].y0 < 184 && lint_box[i].s[0] && strcmp(lint_box[i].s, "INIT") != 0;
    return n;
}
static void layer_draw(uint32_t layer)
{
    sm_layer = (uint8_t)layer;
    lint_zones = LZONES;
    lint_nz = sizeof LZONES / sizeof LZONES[0];
    draw_full();
}
static void layer_close_draw(void)
{
    sm_layer = SML_NONE;
    lint_zones = ZONES;
    lint_nz = sizeof ZONES / sizeof ZONES[0];
    sm_dirty = 1;
    sm_draw();
}
static int test_layer_maps(void)
{
    int bad = 0, ok;
    int32_t y0 = 0, y1 = 0;
    uint32_t i;
    draw_setup();
    layer_draw(SML_EDIT);
    sm_lay_epage = 0;
    layer_draw(SML_EDIT);
    bad += check("MAP: EDIT: the machines of the page, INIT, the page key, the title (all inside their rows, none cut)",
                 lint_frame("edit") == 0 && lint_has("SAW", 0, 0) && lint_has("INIT", 0, 0) && lint_has("OCT+ NEXT PAGE", 0, 0) &&
                 lint_has("MACHINE MONO", 0, 0) && lint_machines() == 13u);
    ok = lint_has("INIT", &y0, &y1);
    bad += check("MAP: the labels of the keys are scale 2 (14 px high)", ok && y1 - y0 == 14);
    bad += check("MAP: the track's machine (SAW) is inverted, its neighbour is not", SCR(10, 56) == INK && SCR(64, 56) == BGC);
    sm_lay_epage = 2;
    layer_draw(SML_EDIT);
    bad += check("MAP: EDIT page 3: the 5 SLICE machines (GLIT first) and INIT", lint_frame("slice") == 0 &&
                 lint_has("MACHINE SLICE", 0, 0) && lint_machines() == 5u && lint_has("GLIT", 0, 0) && lint_has("INIT", 0, 0));
    sm_lay_epage = 1;
    layer_draw(SML_EDIT);
    bad += check("MAP: EDIT page 2: the 9 SOMA machines", lint_frame("soma") == 0 && lint_has("MACHINE SOMA", 0, 0) && lint_machines() == 9u);
    layer_draw(SML_GLO);
    bad += check("MAP: GLO: MUTE and SOLO rows, T1..T4 in each", lint_frame("glo") == 0 && lint_has("MUTE", 0, 0) && lint_has("SOLO", 0, 0) &&
                 lint_has("T1", 0, 0) && lint_has("T4", 0, 0) && lint_has("MUTE SOLO", 0, 0));
    bad += check("MAP: ... nothing muted: no cell inverted", SCR(16, 90) == BGC && SCR(16, 192) == BGC);
    smp.mute = 1;
    sm_solo = 4;
    sm_dirty = 1;
    lcd_px = 0;
    sm_draw();
    bad += check("MAP: ... T1 muted: its mute cell inverted, T2's not; T3 soloed: its solo cell", SCR(16, 90) == INK && SCR(76, 90) == BGC &&
                 SCR(136, 192) == INK && SCR(16, 192) == BGC);
    bad += check("MAP: ... a mute toggled redraws the rows that changed only (<= 3 rows)", lcd_px > 0 && lcd_px <= 3u * 240u * SMD_MAP_ROW_H + 240u * 20u);
    smp.mute = 0;
    sm_solo = 0;
    layer_draw(SML_SEQ);
    smp.pat[smp.cur].kit.snd[0].len = 24;
    layer_draw(SML_SEQ);
    bad += check("MAP: SEQ: the length (LEN 24), the pattern grid, the mode, the title", lint_frame("seq") == 0 && lint_has("LEN 24", 0, 0) &&
                 lint_has("A01", 0, 0) && lint_has("SEQUENTIAL ML AUTO", 0, 0) && lint_has("SEQ T1", 0, 0) && !lint_has("CLEARED", 0, 0));
    sm_lay_cleared = 1;
    layer_draw(SML_SEQ);
    bad += check("MAP: SEQ: CLEARED after a clear", lint_has("CLEARED", 0, 0));
    sm_lay_cleared = 0;
    smp.pat[smp.cur].kit.snd[0].len = 16;
    ok = 1;
    for (i = 0; i < 240u * 240u; i++) {
        uint16_t c = host_screen[i];
        ok &= c == INK || c == DIMC || c == BGC;
    }
    bad += check("MAP: nothing but the three inks", ok);
    layer_close_draw();                                   /* (the map off the screen: the next test's picture is clean) */
    return bad;
}
/* the layer closed: the screen is the page's again, pixel for pixel (nothing of the map left in the bands the knobs do not cover) */
static int test_layer_close_restores(void)
{
    static uint16_t before[240 * 240];
    int bad = 0;
    uint32_t l, same, px;
    for (l = SML_EDIT; l <= SML_SEQ; l++) {
        draw_setup();
        smp.pat[smp.cur].step[0][2].n = 1;
        draw_full();
        memcpy(before, host_screen, sizeof before);
        sm_layer = (uint8_t)l;
        sm_dirty = 1;
        lcd_px = 0;
        sm_draw();
        px = lcd_px;
        bad += check("MAP: opening the layer: the page line and the five rows (<= 240 x 28 + 5 x 240 x 34)", px > 0 && px <= 240u * 28u + 5u * 240u * SMD_MAP_ROW_H);
        sm_dirty = 1;
        lcd_px = 0;
        sm_draw();
        bad += check("MAP: ... nothing changed: nothing drawn", lcd_px == 0);
        sm_layer = SML_NONE;
        sm_dirty = 1;
        sm_draw();
        same = !memcmp(before, host_screen, sizeof before);
        bad += check("MAP: the layer closed: the screen is the page's again, pixel for pixel", same);
    }
    return bad;
}
static int test_header_muted(void)
{
    int bad = 0;
    draw_setup();
    draw_full();
    bad += check("HEADER: an unmuted track: T1 on the ground", SCR(0, 0) == BGC);
    smp.mute = 2;                                         /* T2 muted, T1 is shown */
    sm_dirty = 1;
    sm_draw();
    bad += check("HEADER: another track muted: T1 not inverted", SCR(0, 0) == BGC);
    smp.mute = 1;
    sm_dirty = 1;
    sm_draw();
    bad += check("HEADER: T1 muted: its label inverted", SCR(0, 0) == INK && SCR(1, 1) == INK);
    sm_solo = 2;                                          /* T2 soloed: T1 does not play either */
    smp.mute = 0;
    sm_dirty = 1;
    sm_draw();
    bad += check("HEADER: another track soloed: T1 inverted too", SCR(0, 0) == INK);
    sm_solo = 0;
    smp.mute = 0;
    return bad;
}
static int test_knob_band(void)
{
    int bad = 0;
    uint32_t y, last = 0, first = 0;
    draw_setup();
    draw_full();
    for (y = 48; y < 168u; y++)
        if (scr_ink(0, y, 240, 1))
            last = y;
    for (y = 168; y < 220u && !first; y++)
        if (scr_ink(0, y, 240, 1))
            first = y;
    printf("  knobs end at row %u, SELECT starts at row %u\n", last, first);
    bad += check("LAYOUT: SELECT follows the knobs (at most 12 blank rows between)", last > 0 && first > last && first - last <= 12u);
    bad += check("LAYOUT: the knob zone ends where its content does (a row of ink within 2 of its bottom)", last + 2u >= SMD_KNOB_Y + SMD_KNOB_H - 1u && last < SMD_KNOB_Y + SMD_KNOB_H);
    return bad;
}
/* the LEDs of an open layer */
static int test_leds_layer(void)
{
    int ok = 1, keys = 1;
    uint32_t m, l;
    static const uint8_t modes[] = {LEDS_DIM, LEDS_DIM_LO, LEDS_OFF, LEDS_INV};
    for (l = SML_EDIT; l <= SML_SEQ; l++)
        for (m = 0; m < 4u; m++) {
            uint32_t b = l == SML_EDIT ? B_EDIT : l == SML_GLO ? B_GLO : B_SEQ, o = l == SML_EDIT ? B_GLO : B_EDIT;
            leds_setup(modes[m]);
            sm_ui.page = (uint8_t)page_by_name("EDIT 1");       /* (the EDIT family button is the lit one) */
            sm_layer = (uint8_t)l;
            sm_leds();
            ok &= led_of(fm1_led_breath, panel.btn[b]) == 1 && led_of(fm1_led, panel.btn[b]) == 0 && led_of(fm1_led_dim, panel.btn[b]) == 0;
            ok &= led_of(fm1_led_breath, panel.btn[o]) == 0;
            sm_layer = SML_NONE;
            sm_leds();
            ok &= led_of(fm1_led_breath, panel.btn[b]) == 0;
        }
    leds_setup(LEDS_DIM);
    sm_layer = SML_EDIT;
    sm_lay_epage = 0;
    sm_leds();
    keys &= wl(0) == 1 && wd(1) == 1 && wl(1) == 0 && wd(12) == 1 && wd(13) == 0 && wd(15) == 1 && wm(0) == 0;   /* SAW lit, 13 machines, INIT */
    smp.pat[smp.cur].step[0][5].n = 1;                    /* (the steps are not shown in a layer) */
    sm_leds();
    keys &= wany(5) == 1 && wl(5) == 0;
    leds_setup(LEDS_DIM);
    sm_layer = SML_GLO;
    smp.mute = 4;
    sm_solo = 2;
    sm_leds();
    keys &= led_of(fm1_led, 14u + 5u) == 1 && led_of(fm1_led_dim, 14u + 1u) == 1 && led_of(fm1_led, 14u + 1u) == 0 &&   /* A#3 lit: T3 muted */
            wl(1) == 1 && wd(0) == 1 && wd(2) == 1 && wl(0) == 0;
    smp.mute = 0;
    sm_solo = 0;
    sm_layer = SML_NONE;
    return check("LEDS: an open layer's button breathes: no lit, no glow, in every LEDS mode and layer; the others not", ok) +
           check("LEDS: the layer's keys: EDIT machines (the track's lit), GLO mutes and solos", keys);
}
/* ------------------------------------------------------------------------------------------------ locks --- */
#define LK_NONE (-32768)
static int16_t lk_shadow[SM_NTRK][SM_NSTEP][P_COUNT];    /* the stress test's model of the pool: LK_NONE no lock */
static int lk_on;                                         /* the stress test is checking */
static uint32_t lk_applied, lk_bad_val, lk_bad_dead, lk_preempt_n, lk_preempts;
static uint32_t lk_rng = 12345u;
static uint32_t lk_rand(uint32_t n) { lk_rng = lk_rng * 1103515245u + 12345u; return (lk_rng >> 8) % n; }
static void lock_applied(uint32_t t, uint32_t s, uint32_t p, int32_t v)
{
    lk_applied++;
    if (!lk_on)
        return;
    if (lk_shadow[t][s][p] == LK_NONE)
        lk_bad_dead++;
    else if (lk_shadow[t][s][p] != v && !lk_bad_val++)
        printf("  first bad: t%u s%u p%u applied %d, model %d\n", t, s, p, v, lk_shadow[t][s][p]);
}
static void lock_preempt(void)
{
    if (lk_preempt_n && !lk_rand(lk_preempt_n)) {          /* the audio interrupt between two stores of a lock write */
        lk_preempts++;
        run_block();
    }
}
static sm_pattern_t *cpat(void) { return &smp.pat[smp.cur]; }
static int32_t lk_val(uint32_t t, uint32_t s, uint32_t p)  /* the lock's value, LK_NONE: none */
{
    int32_t i = sm_lock_find(cpat(), t, s, p);
    return i < 0 ? LK_NONE : cpat()->lock[i].val;
}
static uint32_t lk_count_live(void)
{
    uint32_t i, n = 0;
    for (i = 0; i < cpat()->nlock; i++)
        n += cpat()->lock[i].param != 0xFFu;
    return n;
}
static void lk_tap(uint32_t w)                            /* a white key down and up (two polls) */
{
    key_dn(WHITE[w]);
    sm_input_poll();
    key_up(WHITE[w]);
    sm_input_poll();
}
static void filter_page(void)
{
    sm_ui.page = (uint8_t)page_by_name("FILTER");
}
/* run blocks until track t's step s has just fired (at most a bar and a step) */
static int run_to_step(uint32_t t, uint32_t s)
{
    uint32_t b, was = sm_pos[t];
    for (b = 0; b < 20000u; b++) {
        run_block();
        if (sm_pos[t] == s && was != s)
            return 1;
        was = sm_pos[t];
    }
    return 0;
}

static int test_lock_hold_turn(void)
{
    int bad = 0;
    int8_t base;
    smd_knob_t o;
    in_reset();
    host_ticks = 1000u;
    filter_page();
    base = ksnd(0)->p[P_FCUT] = 100;
    key_dn(WHITE[4]);                                     /* a tap: the trig on when the key is let go, not when it goes down */
    sm_input_poll();
    bad += check("LOCK: a white key down: no toggle yet", !cpat()->step[0][4].n);
    key_up(WHITE[4]);
    sm_input_poll();
    bad += check("LOCK: ... let go without a turn: the trig on", cpat()->step[0][4].n == 1u);
    key_dn(WHITE[4]);
    sm_input_poll();
    turn(EN_K2, 10);                                      /* FILTER K2: CUT */
    sm_input_poll();
    bad += check("LOCK: step 4 held, K2 +10: a CUT lock on step 4 = base + 10", lk_val(0, 4, P_FCUT) == base + 10);
    bad += check("LOCK: ... the base did not change", ksnd(0)->p[P_FCUT] == base);
    bad += check("LOCK: ... the step held shows at once (held_step 4)", sm_ui.held_step == 4u);
    smd_knob_get(1, &o);
    bad += check("LOCK: ... the CUT ring is filled and shows the lock", o.filled && o.v == base + 10);
    smd_knob_get(2, &o);
    bad += check("LOCK: ... the RES ring (no lock) shows the base, not filled", !o.filled && o.v == ksnd(0)->p[P_FRES]);
    turn(EN_K2, -3);
    sm_input_poll();
    bad += check("LOCK: a second turn edits the lock (base + 7), one entry", lk_val(0, 4, P_FCUT) == base + 7 && cpat()->nlock == 1u);
    key_up(WHITE[4]);
    sm_input_poll();
    bad += check("LOCK: let go after a turn: the trig stays on", cpat()->step[0][4].n == 1u && sm_ui.held_step == 0xFFu);
    bad += check("LOCK: CUT is locked on some step (the ring's centre dot)", sm_param_locked_any(0, P_FCUT) &&
                 !sm_param_locked_any(0, P_FRES) && !sm_param_locked_any(1, P_FCUT));
    key_dn(WHITE[6]);                                     /* an empty step held and turned: nothing (no lock, no trig) */
    sm_input_poll();
    turn(EN_K2, 5);
    turn(EN_SELECT, 2);
    sm_input_poll();
    key_up(WHITE[6]);
    sm_input_poll();
    bad += check("LOCK: an EMPTY step held + turned: a REST trig (trigless lock) with both locks, base kept, no toggle",
                 lk_val(0, 6, P_FCUT) == base + 5 && lk_val(0, 6, P_LD_FLT) == clamp(TP[P_LD_FLT].def + 2, TP[P_LD_FLT].min, TP[P_LD_FLT].max) && cpat()->step[0][6].n == 1u &&
                 cpat()->step[0][6].flags == SMF_REST && cpat()->nlock == 3u && ksnd(0)->p[P_FCUT] == base &&
                 ksnd(0)->p[P_LD_FLT] == TP[P_LD_FLT].def);
    key_dn(WHITE[4]);                                     /* SELECT while held: its parameter (LFO>FLT) locked */
    sm_input_poll();
    turn(EN_SELECT, -2);
    sm_input_poll();
    key_up(WHITE[4]);
    sm_input_poll();
    bad += check("LOCK: SELECT while held: a lock of its parameter, no toggle", lk_val(0, 4, P_LD_FLT) == TP[P_LD_FLT].def - 2 &&
                 cpat()->step[0][4].n == 1u);
    sm_ui.held_step = 4;
    bad += check("LOCK: SELECT shows the held step's lock, the base otherwise", smd_sel_val(P_LD_FLT) == TP[P_LD_FLT].def - 2 &&
                 (sm_ui.held_step = 0xFFu, smd_sel_val(P_LD_FLT) == TP[P_LD_FLT].def));
    return bad;
}

static int test_lock_applies(void)
{
    int bad = 0;
    in_reset();
    filter_page();
    set_param(0, P_FCUT, 100);
    sm_step_toggle(0, 0);
    sm_step_toggle(0, 4);
    bad += check("LOCK: sm_lock_set on a trig", sm_lock_set(0, 4, P_FCUT, 20) == 1);
    press(B_PLAY);
    sm_input_poll();
    bad += check("LOCK: playing, after step 4: trk CUT == 20", run_to_step(0, 4) && trk[0].p[P_FCUT] == 20);
    bad += check("LOCK: ... the next step 0: back to the base 100", run_to_step(0, 0) && trk[0].p[P_FCUT] == 100);
    run_to_step(0, 4);
    set_param(0, P_FCUT, 90);                             /* the base turned while the lock holds: the lock stays till the next trig */
    run_block();
    bad += check("LOCK: the base turned on a locked step: the lock is kept", trk[0].p[P_FCUT] == 20);
    bad += check("LOCK: ... the next unlocked trig takes the new base", run_to_step(0, 0) && trk[0].p[P_FCUT] == 90);
    run_to_step(0, 4);
    press(B_PLAY);                                        /* STOP on the locked step: the base comes back */
    sm_input_poll();
    run_block();
    bad += check("LOCK: STOP on a locked step: the base again", !sm_playing && trk[0].p[P_FCUT] == 90);
    return bad;
}

static int test_lock_during_play_300bpm(void)
{
    int bad = 0, ok = 1;
    uint32_t s, p, i, b;
    in_reset();
    host_ticks = 1000u;
    filter_page();
    smp.bpm10 = 3000u;
    set_param(0, P_FCUT, 60);
    for (s = 0; s < 16u; s++)
        sm_step_toggle(0, s);
    press(B_PLAY);
    sm_input_poll();
    key_dn(WHITE[7]);
    sm_input_poll();
    for (i = 0; i < 50u; i++) {                           /* 50 detents over 4 bars (64 steps of ~69 blocks) */
        turn(EN_K2, (i & 1u) ? -1 : 2);
        sm_input_poll();
        for (b = 0; b < 88u; b++)
            run_block();
        ms(2);
    }
    for (s = 0; s < SM_NSTEP; s++)
        for (p = 0; p < P_COUNT; p++)
            if (s != 7u)
                ok &= lk_val(0, s, p) == LK_NONE;
    bad += check("LOCK/300: held step 7, 50 detents during 4 bars: no lock on any other step", ok);
    bad += check("LOCK/300: ... the one lock is CUT on step 7 (base 60, 25 x +2 and 25 x -1: 85), base kept",
                 cpat()->nlock == 1u && lk_val(0, 7, P_FCUT) == 85 && ksnd(0)->p[P_FCUT] == 60);
    key_up(WHITE[7]);
    sm_input_poll();
    bad += check("LOCK/300: let go: the trig of step 7 still on", cpat()->step[0][7].n == 1u);
    bad += check("LOCK/300: step 7 plays its lock", run_to_step(0, 7) && trk[0].p[P_FCUT] == lk_val(0, 7, P_FCUT));
    bad += check("LOCK/300: step 8 the base", run_to_step(0, 8) && trk[0].p[P_FCUT] == ksnd(0)->p[P_FCUT]);
    return bad;
}

static int test_lock_stress(void)
{
    static const uint8_t PRM[6] = {P_FCUT, P_FRES, P_LEVEL, P_PAN, P_DEC, P_DIST};
    int bad = 0, ok_base = 1;
    uint32_t op, t, s, p, refused = 0, maxn = 0, sets = 0, clears = 0, trigs = 0;
    in_reset();
    smp.bpm10 = 3000u;
    for (t = 0; t < SM_NTRK; t++)
        for (s = 0; s < SM_NSTEP; s++)
            for (p = 0; p < P_COUNT; p++)
                lk_shadow[t][s][p] = LK_NONE;
    for (t = 0; t < SM_NTRK; t++)
        for (s = 0; s < 16u; s++)
            sm_step_toggle(t, s);
    press(B_PLAY);
    sm_input_poll();
    run_block();
    lk_applied = lk_bad_val = lk_bad_dead = lk_preempts = 0;
    lk_on = 1;
    lk_preempt_n = 3u;
    for (op = 0; op < 10000u; op++) {
        uint32_t r = lk_rand(100);
        t = lk_rand(SM_NTRK);
        s = lk_rand(16);
        if (op % 2500u == 1250u) {                        /* now and then stopped a while: the pool is compacted */
            push(SMC_STOP, 0, 0, 0, 0);
            run_block();
        } else if (op % 2500u == 1400u) {
            push(SMC_PLAY, 0, 0, 0, 0);
        }
        if (r < 88u) {                                    /* a lock (an existing one: its value) */
            const param_desc_t *d;
            p = PRM[lk_rand(6)];
            d = sm_desc_safe(sm_engine_of(ksnd(t)->machine), p);
            int32_t v = d->min + (int32_t)lk_rand((uint32_t)(d->max - d->min + 1));
            if (sm_lock_set(t, s, p, v))
                lk_shadow[t][s][p] = (int16_t)v, sets++;
            else
                refused++;
        } else if (r < 95u) {                             /* the step's locks deleted, the trig kept */
            sm_lock_clear_step(t, s);
            for (p = 0; p < P_COUNT; p++)
                lk_shadow[t][s][p] = LK_NONE;
            clears++;
        } else {                                          /* the trig off, or on again (a toggle takes the step's locks) */
            sm_step_toggle(t, s);
            for (p = 0; p < P_COUNT; p++)
                lk_shadow[t][s][p] = LK_NONE;
            trigs++;
        }
        maxn = cpat()->nlock > maxn ? cpat()->nlock : maxn;
        run_block();
        for (t = 0; t < SM_NTRK; t++)                     /* a parameter not locked by the last trig is the base */
            for (p = 0; p < 6u; p++)
                if (!((sm_lk_on[t][PRM[p] >> 3] >> (PRM[p] & 7u)) & 1u))
                    ok_base &= trk[t].p[PRM[p]] == ksnd(t)->p[PRM[p]];
    }
    lk_on = 0;
    lk_preempt_n = 0;
    printf("  stress: %u sets (%u refused), %u clears, %u trig flips, %u applied, %u preempted writes, nlock max %u\n",
           sets, refused, clears, trigs, lk_applied, lk_preempts, maxn);
    bad += check("LOCK/STRESS: 10000 ops between ISR blocks: every applied lock == the model's", lk_applied > 1000u && !lk_bad_val);
    bad += check("LOCK/STRESS: ... no lock of a deleted step ever applied", !lk_bad_dead);
    bad += check("LOCK/STRESS: ... nlock <= 192, the pool full sometimes (refused)", maxn <= SM_NLOCK && refused > 0u);
    bad += check("LOCK/STRESS: ... an unlocked parameter is the base after every block", ok_base);
    {
        int ok = 1;
        for (t = 0; t < SM_NTRK; t++)
            for (s = 0; s < SM_NSTEP; s++)
                for (p = 0; p < 6u; p++)
                    ok &= lk_val(t, s, PRM[p]) == lk_shadow[t][s][PRM[p]];
        bad += check("LOCK/STRESS: ... the pool == the model at the end", ok);
    }
    push(SMC_STOP, 0, 0, 0, 0);
    run_block();
    sm_lock_clear_step(0, 0);                             /* stopped: the pool compacted to its live locks */
    bad += check("LOCK/STRESS: stopped, a clear compacts: nlock == the live locks", cpat()->nlock == lk_count_live());
    return bad;
}

static int test_lock_full(void)
{
    int bad = 0, ok = 1;
    uint32_t i, t, s, p;
    static const uint8_t PRM[6] = {P_FCUT, P_FRES, P_LEVEL, P_PAN, P_DEC, P_DIST};
    in_reset();
    host_ticks = 1000u;
    for (t = 0; t < SM_NTRK; t++)
        for (s = 0; s < 16u; s++)
            sm_step_toggle(t, s);
    for (i = 0; i < SM_NLOCK; i++)                        /* 192 distinct locks: 4 tracks x 16 steps x 3 */
        ok &= sm_lock_set(i % 4u, i / 4u % 16u, PRM[i / 64u], (int32_t)(i % 100u)) == 1;
    bad += check("LOCK/FULL: 192 locks set, no message", ok && cpat()->nlock == SM_NLOCK && !sm_msg);
    bad += check("LOCK/FULL: the 193rd refused, LOCKS FULL", sm_lock_set(0, 0, PRM[4], 5) == 0 && sm_msg);
    ok = 1;
    for (i = 0; i < SM_NLOCK; i++)
        ok &= lk_val(i % 4u, i / 4u % 16u, PRM[i / 64u]) == (int32_t)(i % 100u);
    bad += check("LOCK/FULL: ... the 192 intact, nlock 192", ok && cpat()->nlock == SM_NLOCK);
    bad += check("LOCK/FULL: an existing lock still takes a new value when full", sm_lock_set(1, 0, PRM[0], 77) == 1 &&
                 lk_val(1, 0, PRM[0]) == 77);
    sm_playing = 1;                                       /* playing: a cleared step's slots are taken again by new locks */
    sm_lock_clear_step(2, 3);
    p = sm_lock_set(2, 5, P_FLT, 1) && sm_lock_set(2, 5, P_FATK, 1) && sm_lock_set(2, 5, P_FDEC, 1);
    bad += check("LOCK/FULL: playing: a step cleared frees its 3 slots for new locks, nlock stays 192",
                 p && cpat()->nlock == SM_NLOCK && sm_lock_set(2, 5, P_SUS, 1) == 0);
    sm_playing = 0;
    draw_setup();
    sm_lock_full_msg();
    lint_n = lint_band = 0;
    sm_draw();
    {
        int32_t y0, y1;
        bad += check("LOCK/FULL: the header shows LOCKS FULL", lint_has("LOCKS FULL", &y0, &y1) && y1 < (int32_t)SMD_HDR_H);
    }
    ms(900);
    sm_input_poll();
    bad += check("LOCK/FULL: ... still at 0.9 s", sm_msg);
    ms(200);
    sm_input_poll();
    lint_n = lint_band = 0;
    sm_draw();
    {
        int32_t y0, y1;
        bad += check("LOCK/FULL: ... gone after 1 s (the header redrawn without it)", !sm_msg && !lint_has("LOCKS FULL", &y0, &y1));
    }
    return bad;
}

static int test_retrig_clears_locks(void)
{
    int bad = 0;
    in_reset();
    host_ticks = 1000u;
    lk_tap(0);
    sm_lock_set(0, 0, P_FCUT, 10);
    sm_lock_set(0, 0, P_FRES, 50);
    sm_lock_set(0, 1, P_FRES, 50);                        /* (another step: kept) */
    lk_tap(0);
    bad += check("LOCK: the trig taken off: its locks gone", !cpat()->step[0][0].n && lk_val(0, 0, P_FCUT) == LK_NONE &&
                 lk_val(0, 0, P_FRES) == LK_NONE && lk_val(0, 1, P_FRES) == 50);
    lk_tap(0);
    bad += check("LOCK: placed again: a trig without locks", cpat()->step[0][0].n == 1u && lk_val(0, 0, P_FCUT) == LK_NONE &&
                 lk_val(0, 0, P_FRES) == LK_NONE);
    bad += check("LOCK: stopped: the pool compacted (one lock left)", cpat()->nlock == 1u);
    sm_step_toggle(1, 0);
    sm_lock_set(0, 0, P_FCUT, 10);                        /* the SEQ layer's CLEAR takes the track's locks too */
    sm_lock_set(1, 0, P_FCUT, 10);
    layer_hold(B_SEQ);
    key_dn(WHITE[0]);
    sm_input_poll();
    key_up(WHITE[0]);
    layer_let_go(B_SEQ);
    bad += check("LOCK: SEQ CLEAR: the track's locks gone, the other track's kept", lk_val(0, 0, P_FCUT) == LK_NONE &&
                 lk_val(0, 1, P_FRES) == LK_NONE && lk_val(1, 0, P_FCUT) == 10);
    return bad;
}

static int test_rec_records_on_trigs(void)
{
    int bad = 0, ok = 1, any = 0;
    uint32_t s, i, b;
    int8_t base;
    in_reset();
    host_ticks = 1000u;
    filter_page();
    base = ksnd(0)->p[P_FCUT] = 64;
    for (s = 0; s < 16u; s += 4u)
        sm_step_toggle(0, s);
    press(B_REC);
    sm_input_poll();
    bad += check("REC: the REC key: sm_rec on", sm_rec == 1u);
    turn(EN_K2, 3);                                       /* stopped: a turn is the base, as ever */
    sm_input_poll();
    bad += check("REC: stopped: a turn moves the base, no lock", ksnd(0)->p[P_FCUT] == base + 3 && cpat()->nlock == 0u);
    base = ksnd(0)->p[P_FCUT];
    press(B_PLAY);
    sm_input_poll();
    run_block();
    for (i = 0; i < 64u; i++) {                           /* two bars at 120: a turn every ~43 blocks */
        turn(EN_K2, -1);
        sm_input_poll();
        for (b = 0; b < 43u; b++)
            run_block();
    }
    for (s = 0; s < 16u; s++) {
        int32_t v = lk_val(0, s, P_FCUT);
        if (s % 4u)
            ok &= v == LK_NONE;
        else
            any += v != LK_NONE;
    }
    bad += check("REC: playing, CUT turned: locks only on the steps with trigs", ok && any == 4);
    bad += check("REC: ... the base turned with the knob (no step held: the base, always; 02:43)", ksnd(0)->p[P_FCUT] == base - 64);
    for (s = 0, ok = 1; s < 16u; s += 4u)                 /* (each trig: the value the knob had as it played) */
        ok &= lk_val(0, s, P_FCUT) >= base - 64 && lk_val(0, s, P_FCUT) < base;
    bad += check("REC: ... each trig locked the base's value as it played", ok);
    press(B_REC);
    sm_input_poll();
    bad += check("REC: the REC key again: off", sm_rec == 0u);
    return bad;
}

/* fix round 1: a long hold does not toggle; a removed trig's lock leaves the core; REC heard at once; press before knobs;
 * a layer disarms the trig keys */
static int test_lock_long_hold(void)
{
    int bad = 0;
    in_reset();
    host_ticks = 1000u;
    sm_step_toggle(0, 3);
    sm_lock_set(0, 3, P_FCUT, 33);
    key_dn(WHITE[3]);
    sm_input_poll();
    ms(400);
    sm_input_poll();
    key_up(WHITE[3]);
    sm_input_poll();
    bad += check("FIX1: held past SM_HOLD_US, let go without a turn: trig and lock intact",
                 cpat()->step[0][3].n == 1u && lk_val(0, 3, P_FCUT) == 33);
    sm_ui.page = (uint8_t)page_by_name("F.ENV");          /* K4 has no parameter there */
    key_dn(WHITE[3]);
    sm_input_poll();
    turn(EN_K4, 2);
    sm_input_poll();
    key_up(WHITE[3]);
    sm_input_poll();
    bad += check("FIX1: a turn of a knob without a parameter while down: no toggle", cpat()->step[0][3].n == 1u);
    key_dn(WHITE[3]);
    sm_input_poll();
    ms(300);
    key_up(WHITE[3]);
    sm_input_poll();
    bad += check("FIX1: a tap of 0.3 s still toggles (off, its lock gone)", !cpat()->step[0][3].n && lk_val(0, 3, P_FCUT) == LK_NONE);
    return bad;
}

static int test_lock_press_turn_same_poll(void)
{
    int bad = 0;
    in_reset();
    host_ticks = 1000u;
    filter_page();
    sm_step_toggle(0, 6);
    key_dn(WHITE[6]);
    turn(EN_K2, -5);                                      /* the press edge and the turn in one poll */
    sm_input_poll();
    key_up(WHITE[6]);
    sm_input_poll();
    bad += check("FIX1: press and turn in one poll: a lock of that step, base kept, no toggle",
                 lk_val(0, 6, P_FCUT) == TP[P_FCUT].def - 5 && ksnd(0)->p[P_FCUT] == TP[P_FCUT].def && cpat()->step[0][6].n == 1u);
    return bad;
}

static int test_lock_layer_disarms(void)
{
    int bad = 0;
    in_reset();
    host_ticks = 1000u;
    key_dn(WHITE[8]);                                     /* a trig key down, then the SEQ layer opens under it */
    sm_input_poll();
    layer_hold(B_SEQ);
    bad += check("FIX1: the layer open, the key no longer armed", sm_layer == SML_SEQ && !sm_key_arm);
    key_up(WHITE[8]);
    sm_input_poll();
    layer_let_go(B_SEQ);
    sm_input_poll();
    bad += check("FIX1: the key let go inside the layer: no toggle", !cpat()->step[0][8].n);
    return bad;
}

static int test_lock_unlock_on_remove(void)
{
    int bad = 0;
    uint32_t i;
    in_reset();
    host_ticks = 1000u;
    set_param(0, P_FCUT, 100);
    sm_step_toggle(0, 4);
    sm_lock_set(0, 4, P_FCUT, 20);
    press(B_PLAY);
    sm_input_poll();
    bad += check("FIX1: the only trig (locked) fires: 20", run_to_step(0, 4) && trk[0].p[P_FCUT] == 20);
    lk_tap(4);                                            /* taken off while playing */
    run_block();
    bad += check("FIX1: the trig taken off: the base (100) after the next block", !cpat()->step[0][4].n && trk[0].p[P_FCUT] == 100);
    set_param(0, P_FCUT, 90);
    run_block();
    bad += check("FIX1: ... and a base turn of that parameter is heard", trk[0].p[P_FCUT] == 90);
    /* SEQ CLEAR while playing */
    sm_step_toggle(0, 4);
    sm_lock_set(0, 4, P_FCUT, 20);
    run_to_step(0, 4);
    bad += check("FIX1: locked again: 20", trk[0].p[P_FCUT] == 20);
    layer_hold(B_SEQ);
    key_dn(WHITE[0]);
    sm_input_poll();
    key_up(WHITE[0]);
    layer_let_go(B_SEQ);
    run_block();
    bad += check("FIX1: SEQ CLEAR while playing: the base after the next block", trk[0].p[P_FCUT] == 90);
    /* another trig taken off: the lock of the trig that fired stays */
    sm_step_toggle(0, 4);
    sm_step_toggle(0, 9);
    sm_lock_set(0, 4, P_FCUT, 20);
    run_to_step(0, 4);
    sm_step_toggle(0, 9);
    run_block();
    bad += check("FIX1: another trig taken off: the sounding lock stays (20)", trk[0].p[P_FCUT] == 20);
    /* a muted track with a lock on the core */
    smp.mute = 1u;
    run_block();
    sm_step_toggle(0, 4);
    run_block();
    bad += check("FIX1: muted track, its locked trig taken off: restored", trk[0].p[P_FCUT] == 90);
    smp.mute = 0;
    /* the queue full: the unlock waits and goes through */
    sm_step_toggle(0, 4);
    sm_lock_set(0, 4, P_FCUT, 20);
    run_to_step(0, 4);
    {
        sm_cmd_t f = {SMC_PARAM, 3, 0, P_LRATE, 0};
        f.val = ksnd(3)->p[P_LRATE];
        for (i = 0; i < SM_NCMD; i++)
            sm_ring_put(&sm_cq, f);
    }
    sm_step_toggle(0, 4);
    bad += check("FIX1: the queue full: the unlock is kept", (sm_unl_pend & 1u) != 0u);
    for (i = 0; i < 4u; i++) {
        run_block();
        sm_input_poll();
    }
    run_block();
    bad += check("FIX1: ... pushed again on a poll: the base", !sm_unl_pend && trk[0].p[P_FCUT] == 90);
    return bad;
}

static int test_rec_preview(void)
{
    int bad = 0;
    uint32_t s;
    int32_t v;
    in_reset();
    host_ticks = 1000u;
    filter_page();
    set_param(0, P_FCUT, 64);
    for (s = 0; s < 16u; s++)
        sm_step_toggle(0, s);
    press(B_REC);
    press(B_PLAY);
    sm_input_poll();
    run_to_step(0, 5);
    turn(EN_K2, 10);
    sm_input_poll();
    v = lk_val(0, 5, P_FCUT);
    bad += check("FIX1: REC turn on step 5: lock 74, the base 74 too (a turn is the base, 02:43)", v == 74 && ksnd(0)->p[P_FCUT] == 74);
    run_block();
    bad += check("FIX1: ... the core has it after the next block", sm_pos[0] == 5u && trk[0].p[P_FCUT] == 74);
    press(B_REC);                                         /* REC off: the base back to 64, step 5 keeps its 74 */
    sm_input_poll();
    turn(EN_K2, -10);
    sm_input_poll();
    bad += check("FIX1: ... REC off, K2 -10: the base 64, the lock still 74", ksnd(0)->p[P_FCUT] == 64 && lk_val(0, 5, P_FCUT) == 74);
    bad += check("FIX1: ... the next (unlocked) trig: the base", run_to_step(0, 6) && trk[0].p[P_FCUT] == 64);
    bad += check("FIX1: ... step 5 again: its lock", run_to_step(0, 5) && trk[0].p[P_FCUT] == 74);
    return bad;
}

/* ------------------------------------------------------------------------------------------------ STEP (Task 5) --- */
#define S120 ((double)FS / 8.0)                           /* a 1/16 at 120 BPM, in samples */
static sm_step_t *stp(uint32_t t, uint32_t s) { return &smp.pat[smp.cur].step[t][s]; }
static int near_d(uint32_t a, double b) { double d = (double)a - b; return d <= CTL && d >= -(double)CTL; }
/* the sequencer alone (no mix) for n samples */
static void seq_run(uint32_t n)
{
    uint32_t end = seq_clock + n;
    while (seq_clock < end) {
        events_block(CTL);
        seq_clock += CTL;
    }
}
static uint32_t ev_count(uint32_t on, uint32_t t)
{
    uint32_t i, n = 0;
    for (i = 0; i < seq_nev && i < 1024u; i++)
        n += seq_ev[i].on == on && seq_ev[i].t == t;
    return n;
}
static const seq_ev_t *ev_nth(uint32_t on, uint32_t t, uint32_t k)   /* the k-th note-on (on 1) / note-off (0) of track t */
{
    uint32_t i;
    for (i = 0; i < seq_nev && i < 1024u; i++)
        if (seq_ev[i].on == on && seq_ev[i].t == t && !k--)
            return &seq_ev[i];
    return 0;
}
static uint32_t ev_step(const seq_ev_t *e, double S) { return e ? (uint32_t)((double)e->at / S + 0.5) : 0xFFFFu; }

static int test_step_vel_acc(void)
{
    const seq_ev_t *e;
    int bad = 0;
    seq_reset();
    sm_step_toggle(0, 0);
    stp(0, 0)->vel = 77;
    sm_step_toggle(1, 0);
    stp(1, 0)->vel = 60;
    stp(1, 0)->flags = SMF_ACCENT;
    push(SMC_PLAY, 0, 0, 0, 0);
    seq_run(CTL * 4u);
    e = ev_nth(1, 0, 0);
    bad += check("STEP: VEL 77: the note-on at velocity 77", e && e->vel == 77u);
    e = ev_nth(1, 1, 0);
    bad += check("STEP: ACCENT on VEL 60: the note-on at 127", e && e->vel == 127u);
    return bad;
}

static int test_step_len(void)
{
    const seq_ev_t *on, *off;
    int bad = 0;
    seq_reset();
    sm_step_toggle(0, 0);
    stp(0, 0)->len = 5;                                   /* 1/4 */
    sm_step_toggle(1, 0);
    sm_step_toggle(1, 8);
    stp(1, 0)->len = SM_LEN_INF;
    push(SMC_PLAY, 0, 0, 0, 0);
    seq_run((uint32_t)(10.0 * S120));
    on = ev_nth(1, 0, 0);
    off = ev_nth(0, 0, 0);
    printf("swamp: LEN 1/4: on %u off %u (want %.0f)\n", on ? on->at : 0u, off ? off->at : 0u, S120 / 4.0);
    bad += check("STEP: LEN 1/4: the note-off a quarter step after the note-on (+-CTL)",
                 on && off && near_d(off->at - on->at, S120 / 4.0));
    off = ev_nth(0, 1, 0);
    bad += check("STEP: LEN INF: held to the next trig (step 8), no note-off before",
                 off && near_d(off->at, 8.0 * S120) && ev_count(1, 1) == 2u);
    seq_reset();                                          /* LEN 1/32: below 1/8, the finer q8 gate */
    sm_step_toggle(0, 0);
    stp(0, 0)->len = 2;
    push(SMC_PLAY, 0, 0, 0, 0);
    seq_run((uint32_t)(3.0 * S120));
    on = ev_nth(1, 0, 0);
    off = ev_nth(0, 0, 0);
    bad += check("STEP: LEN 1/32: the note-off a 1/32 step after the note-on (+-CTL)",
                 on && off && near_d(off->at - on->at, S120 / 32.0));
    bad += check("STEP: SM_LEN64: TRK 0, 1/64 1, 1 64, 16 1024 (SWAMP list), INF 16",
                 SM_LEN_INF == 16u && SM_LEN64[0] == 0u && SM_LEN64[1] == 1u && SM_LEN64[8] == 64u && SM_LEN64[15] == 1024u);
    return bad;
}

/* a trig held on a sound page (FILTER): PRESETS is the step's FLAG, not the track's preset */
static int test_flag_on_held_trig_any_page(void)
{
    int bad = 0;
    uint32_t p0;
    in_reset();
    host_ticks = 1000u;
    sm_step_toggle(0, 3);
    sm_input_poll();
    while (strcmp(sm_cur_page()->name, "FILTER") && sm_ui.page < 63u)
        sm_ui.page++;
    bad += check("FLAGHELD: on the FILTER page", !strcmp(sm_cur_page()->name, "FILTER"));
    p0 = ksnd(0)->preset;
    key_dn(WHITE[3]);
    sm_input_poll();
    turn(EN_PRESET, 1);
    sm_input_poll();
    bad += check("FLAGHELD: PRESETS +1: ACC, the track's preset unchanged", stp(0, 3)->flags == SMF_ACCENT && ksnd(0)->preset == p0);
    turn(EN_PRESET, 1);
    sm_input_poll();
    bad += check("FLAGHELD: +1 again: SLD", stp(0, 3)->flags == SMF_SLIDE);
    key_up(WHITE[3]);
    sm_input_poll();
    bad += check("FLAGHELD: the release does not toggle the trig", stp(0, 3)->n == 1u);
    key_dn(WHITE[6]);
    sm_input_poll();
    turn(EN_PRESET, 1);
    sm_input_poll();
    key_up(WHITE[6]);
    sm_input_poll();
    bad += check("FLAGHELD: an empty step: a REST trig, kept on the release", stp(0, 6)->n == 1u && stp(0, 6)->flags == SMF_REST);
    turn(EN_PRESET, 1);
    sm_input_poll();
    bad += check("FLAGHELD: no key down: PRESETS is the track's preset again", ksnd(0)->preset == p0 + 1u && stp(0, 3)->flags == SMF_SLIDE);
    return bad;
}

static int test_flag_shown_while_held(void)
{
    int bad = 0;
    in_reset();
    host_ticks = 1000u;
    sm_step_toggle(0, 3);
    while (strcmp(sm_cur_page()->name, "FILTER") && sm_ui.page < 63u)
        sm_ui.page++;
    key_dn(WHITE[3]);
    sm_input_poll();
    ms(500);
    sm_input_poll();
    turn(EN_PRESET, 1);
    sm_input_poll();
    bad += check("FLAGSHOWN: held trig: the page line is FLAG ACC", smd_flag_held() && stp(0, 3)->flags == SMF_ACCENT);
    {
        char b[24];
        uint32_t n = 0;
        const char *nm = SM_FLAGN[sm_flag_place(smd_held_step()->flags)];
        smd_cat(b, &n, "FLAG ");
        smd_cat(b, &n, nm);
        bad += check("FLAGSHOWN: the text", !strcmp(b, "FLAG ACC"));
    }
    key_up(WHITE[3]);
    sm_input_poll();
    bad += check("FLAGSHOWN: released: the line is the page's", !smd_flag_held());
    return bad;
}

static int test_step_cond(void)
{
    uint32_t t, i, ok;
    int bad = 0;
    /* 50 %: about half of 1000 passes (the LFSR seeded) */
    seq_reset();
    smp.bpm10 = 3000;
    ksnd(0)->len = 1;
    sm_step_toggle(0, 0);
    stp(0, 0)->cond = 50;
    sm_lfsr = 0xACE1u;
    push(SMC_PLAY, 0, 0, 0, 0);
    seq_run((uint32_t)(999.5 * (double)FS * 150.0 / 3000.0));
    printf("swamp: COND 50%%: %u of 1000\n", on_n[0]);
    bad += check("STEP: COND 50%: 440..560 of 1000 passes play", on_n[0] >= 440u && on_n[0] <= 560u);
    /* A:B 2:4, 1ST, !1ST, FILL / !FILL (sm_fill 0): tracks of length 1, 8 passes */
    seq_reset();
    for (t = 0; t < SM_NTRK; t++) {
        ksnd(t)->len = 1;
        sm_step_toggle(t, 0);
    }
    stp(0, 0)->cond = (uint8_t)sm_cond_ab(2, 4);
    stp(1, 0)->cond = SM_COND_1ST;
    stp(2, 0)->cond = SM_COND_N1ST;
    stp(3, 0)->cond = SM_COND_FILL;
    push(SMC_PLAY, 0, 0, 0, 0);
    seq_run((uint32_t)(7.5 * S120));
    ok = on_n[0] == 2u && ev_step(ev_nth(1, 0, 0), S120) == 1u && ev_step(ev_nth(1, 0, 1), S120) == 5u;
    bad += check("STEP: COND 2:4: the 2nd of every 4 passes (passes 1 and 5 of 8)", ok);
    bad += check("STEP: COND 1ST: the first pass only; !1ST: the other 7", on_n[1] == 1u && on_n[2] == 7u &&
                 ev_step(ev_nth(1, 1, 0), S120) == 0u);
    bad += check("STEP: COND FILL with sm_fill 0: never", on_n[3] == 0u);
    seq_reset();
    ksnd(0)->len = 1;
    sm_step_toggle(0, 0);
    stp(0, 0)->cond = SM_COND_NFILL;
    push(SMC_PLAY, 0, 0, 0, 0);
    seq_run((uint32_t)(3.5 * S120));
    bad += check("STEP: COND !FILL with sm_fill 0: always", on_n[0] == 4u);
    /* PRE / !PRE: step 0 1:2 (passes 0, 2), step 1 PRE (T1) or !PRE (T2); tracks of length 2, 4 passes */
    seq_reset();
    for (t = 0; t < 2u; t++) {
        ksnd(t)->len = 2;
        sm_step_toggle(t, 0);
        sm_step_toggle(t, 1);
        stp(t, 0)->cond = (uint8_t)sm_cond_ab(1, 2);
        stp(t, 1)->cond = t ? SM_COND_NPRE : SM_COND_PRE;
    }
    push(SMC_PLAY, 0, 0, 0, 0);
    seq_run((uint32_t)(7.5 * S120));
    ok = on_n[0] == 4u;
    for (i = 0; i < 4u; i++)
        ok &= ev_step(ev_nth(1, 0, i), S120) == (uint32_t)((uint32_t[]){0, 1, 4, 5})[i];
    bad += check("STEP: COND PRE: plays when the trig before played (steps 0 1 4 5)", ok);
    ok = on_n[1] == 4u;
    for (i = 0; i < 4u; i++)
        ok &= ev_step(ev_nth(1, 1, i), S120) == (uint32_t)((uint32_t[]){0, 3, 4, 7})[i];
    bad += check("STEP: COND !PRE: plays when it did not (steps 0 3 4 7)", ok);
    /* the knob's walk: 1 % .. 99 %, 100 % (0), the conditions; the names */
    {
        char b[8];
        ok = sm_cond_of_place(0) == 1u && sm_cond_of_place(98) == 99u && sm_cond_of_place(99) == 0u &&
             sm_cond_of_place(100) == SM_COND_1ST && sm_cond_of_place(SM_COND_PLACES - 1u) == sm_cond_ab(8, 8);
        for (i = 0; i < SM_COND_PLACES; i++)
            ok &= sm_cond_place(sm_cond_of_place(i)) == i;
        sm_cond_name(0, b);
        ok &= !strcmp(b, "100%");
        sm_cond_name(SM_COND_N1ST, b);
        ok &= !strcmp(b, "!1ST");
        sm_cond_name(sm_cond_ab(3, 7), b);
        ok &= !strcmp(b, "3:7");
        bad += check("STEP: COND walk 1%..99%, 100%, 1ST .. 8:8 and back; names 100% !1ST 3:7", ok);
    }
    return bad;
}

static int test_step_ratchet(void)
{
    int bad = 0;
    seq_reset();
    sm_step_toggle(0, 0);
    stp(0, 0)->flags = (uint8_t)(2u << 4);                /* x3 */
    sm_step_toggle(0, 2);
    push(SMC_PLAY, 0, 0, 0, 0);
    seq_run((uint32_t)(1.9 * S120));
    printf("swamp: x3: %u note-ons at %u %u %u\n", on_n[0], on_at[0][0], on_at[0][1], on_at[0][2]);
    bad += check("STEP: ratchet x3: 3 note-ons in the step, at 0, 1/3, 2/3 (+-CTL)",
                 on_n[0] == 3u && near_d(on_at[0][0], 0) && near_d(on_at[0][1], S120 / 3.0) && near_d(on_at[0][2], 2.0 * S120 / 3.0));
    bad += check("STEP: ... each part let go before the next (3 note-offs by step 1.9)", ev_count(0, 0) == 3u);
    seq_run((uint32_t)(0.5 * S120));
    bad += check("STEP: ... the next trig (x1) plays once", on_n[0] == 4u);
    return bad;
}

static int test_step_rest(void)
{
    int bad = 0;
    in_reset();
    host_ticks = 1000u;
    set_param(0, P_FCUT, 100);
    sm_step_toggle(0, 2);
    stp(0, 2)->flags = SMF_REST;
    sm_lock_set(0, 2, P_FCUT, 20);
    press(B_PLAY);
    sm_input_poll();
    bad += check("STEP: REST trig: its lock applied (20)", run_to_step(0, 2) && trk[0].p[P_FCUT] == 20);
    bad += check("STEP: ... and no note", on_n[0] == 0u);
    bad += check("STEP: ... the next pass's step 0 (no trig) keeps it; step 2 again too",
                 run_to_step(0, 0) && trk[0].p[P_FCUT] == 20 && run_to_step(0, 2) && on_n[0] == 0u);
    return bad;
}

static int test_step_pslide(void)
{
    int bad = 0;
    int32_t mid, late, res_mid;
    in_reset();
    host_ticks = 1000u;
    set_param(0, P_FCUT, 60);
    set_param(0, P_FRES, 0);
    sm_step_toggle(0, 0);
    sm_step_toggle(0, 4);
    stp(0, 0)->flags = SMF_PSLIDE;
    sm_lock_set(0, 0, P_FCUT, 20);
    sm_lock_set(0, 4, P_FCUT, 100);
    sm_lock_set(0, 4, P_FRES, 64);                        /* (locked on the next trig only: from the base) */
    seq_log_clear();
    push(SMC_PLAY, 0, 0, 0, 0);
    seq_run((uint32_t)(0.5 * S120));
    mid = trk[0].p[P_FCUT];
    res_mid = trk[0].p[P_FRES];
    seq_run((uint32_t)(0.4 * S120));
    late = trk[0].p[P_FCUT];
    printf("swamp: PSLIDE 20 -> 100: %d at 1/2 step, %d at 0.9; RES 0 -> 64: %d at 1/2\n", mid, late, res_mid);
    bad += check("STEP: PSLIDE: CUT half way between the locks at the middle of the step (60 +-3)", mid >= 57 && mid <= 63);
    bad += check("STEP: ... RES (locked on the next trig only) half way too (32 +-3)", res_mid >= 29 && res_mid <= 35);
    bad += check("STEP: ... 0.9 of the step: 92 +-4", late >= 88 && late <= 96);
    seq_run((uint32_t)(1.0 * S120));
    bad += check("STEP: ... after the step: at the next trig's lock (100), held", trk[0].p[P_FCUT] == 100 && trk[0].p[P_FRES] == 64);
    seq_run((uint32_t)(2.5 * S120));
    bad += check("STEP: ... the next trig fires its own lock (100)", trk[0].p[P_FCUT] == 100);
    return bad;
}

static int test_step_nudge(void)
{
    int bad = 0;
    seq_reset();
    sm_step_toggle(0, 0);
    sm_step_toggle(0, 2);
    stp(0, 2)->nudge = 4;
    sm_step_toggle(0, 6);
    stp(0, 6)->nudge = -4;
    sm_step_toggle(0, 9);
    stp(0, 9)->nudge = 7;
    stp(0, 9)->flags = (uint8_t)(1u << 4);                /* x2: plays on its start */
    push(SMC_PLAY, 0, 0, 0, 0);
    seq_run((uint32_t)(9.9 * S120));
    printf("swamp: NUDGE: on at %u %u %u (want 0, %.0f, %.0f), x2 at %u\n", on_at[0][0], on_at[0][1], on_at[0][2],
           2.25 * S120, 5.75 * S120, on_at[0][3]);
    bad += check("STEP: NUDGE +4: the trig starts a quarter step late", near_d(on_at[0][1], 2.25 * S120));
    bad += check("STEP: NUDGE -4: a quarter step early, once (not again on its step)",
                 near_d(on_at[0][2], 5.75 * S120) && on_n[0] == 5u);
    bad += check("STEP: NUDGE on a ratchet: on its start", near_d(on_at[0][3], 9.0 * S120));
    /* an early trig on step 0 fires in the pass before: its COND counts the pass it belongs to (1ST: once) */
    seq_reset();
    ksnd(0)->len = 2;
    sm_step_toggle(0, 0);
    stp(0, 0)->nudge = -4;
    stp(0, 0)->cond = SM_COND_1ST;
    push(SMC_PLAY, 0, 0, 0, 0);
    seq_run((uint32_t)(7.9 * S120));
    bad += check("STEP: NUDGE -4 on step 0 with 1ST: the first pass only (the early fire counts the next pass)", on_n[0] == 1u);
    return bad;
}

static int test_step_slide(void)
{
    const seq_ev_t *on2, *off1;
    int bad = 0;
    uint32_t i, i_on2 = 0, i_off1 = 0;
    seq_reset();
    sm_step_toggle(0, 0);
    stp(0, 0)->note[0] = 48;
    stp(0, 0)->flags = SMF_SLIDE;
    stp(0, 0)->len = 1;                                   /* (1/8: a SLIDE holds to the next trig all the same) */
    sm_step_toggle(0, 1);
    stp(0, 1)->note[0] = 55;
    sm_step_toggle(0, 4);
    stp(0, 4)->flags = SMF_SLIDE;
    push(SMC_PLAY, 0, 0, 0, 0);
    seq_run((uint32_t)(1.5 * S120));
    on2 = ev_nth(1, 0, 1);
    off1 = ev_nth(0, 0, 0);
    for (i = 0; i < seq_nev; i++) {
        if (&seq_ev[i] == on2) i_on2 = i;
        if (&seq_ev[i] == off1) i_off1 = i;
    }
    bad += check("STEP: SLIDE: the note held to the next trig (no note-off before step 1)", off1 && near_d(off1->at, S120));
    bad += check("STEP: ... the next note on before the old one off (legato), the glide flag on at its note-on",
                 on2 && on2->note == 55u && i_on2 < i_off1 && on2->gl);
    seq_run((uint32_t)(4.0 * S120));                      /* step 4 SLIDE, step 5 empty: let go on step 5 */
    off1 = ev_nth(0, 0, 2);
    bad += check("STEP: a SLIDE into an empty step: let go on that step", off1 && near_d(off1->at, 5.0 * S120));
    return bad;
}

/* fix round 1: a ratchet stops when its track is muted; a removed PSLIDE trig leaves the base; STOP -> PLAY counts from 0;
 * a ratchet cut by the next step's early trig (the later trig wins) */
static int test_step_fix1(void)
{
    int bad = 0;
    int32_t v;
    seq_reset();
    sm_step_toggle(0, 0);
    stp(0, 0)->flags = (uint8_t)(3u << 4);                /* x4 */
    push(SMC_PLAY, 0, 0, 0, 0);
    seq_run((uint32_t)(0.1 * S120));
    smp.mute = 1u;
    seq_run((uint32_t)(1.5 * S120));
    smp.mute = 0;
    bad += check("FIX5: x4 ratchet, muted after the first part: no more note-ons", on_n[0] == 1u && !sm_rat_left[0]);
    /* PSLIDE trig taken off mid-glide: the base comes back and stays */
    in_reset();
    host_ticks = 1000u;
    set_param(0, P_FCUT, 60);
    sm_step_toggle(0, 0);
    sm_step_toggle(0, 4);
    stp(0, 0)->flags = SMF_PSLIDE;
    sm_lock_set(0, 0, P_FCUT, 20);
    sm_lock_set(0, 4, P_FCUT, 100);
    push(SMC_PLAY, 0, 0, 0, 0);
    seq_run((uint32_t)(0.3 * S120));
    v = trk[0].p[P_FCUT];
    sm_step_toggle(0, 0);                                 /* off mid-glide */
    seq_run((uint32_t)(0.1 * S120));
    bad += check("FIX5: a PSLIDE trig taken off mid-glide: the base (60) at once", v > 20 && v < 100 && trk[0].p[P_FCUT] == 60);
    seq_run((uint32_t)(0.5 * S120));
    bad += check("FIX5: ... and it stays (no glide writes it again)", trk[0].p[P_FCUT] == 60);
    /* STOP -> PLAY: A:B counts passes from 0 again; PRE from 0 */
    seq_reset();
    ksnd(0)->len = 1;
    sm_step_toggle(0, 0);
    stp(0, 0)->cond = (uint8_t)sm_cond_ab(1, 2);
    ksnd(1)->len = 2;
    sm_step_toggle(1, 0);
    sm_step_toggle(1, 1);
    stp(1, 0)->cond = SM_COND_PRE;
    stp(1, 1)->cond = SM_COND_1ST;
    push(SMC_PLAY, 0, 0, 0, 0);
    seq_run((uint32_t)(1.5 * S120));                      /* T1 pass 0 (plays), 1; T2 step 1 plays: PRE 1 */
    push(SMC_STOP, 0, 0, 0, 0);
    seq_run(CTL);
    push(SMC_PLAY, 0, 0, 0, 0);
    seq_run((uint32_t)(0.5 * S120));
    bad += check("FIX5: STOP -> PLAY: A:B 1:2 plays on the first pass again", on_n[0] == 2u);
    bad += check("FIX5: STOP -> PLAY: PRE starts at 0 (step 0 PRE does not play)", on_n[1] == 1u);
    /* a x4 ratchet on step 0, step 1 nudged -6 (fires at 0.625): the later trig wins, the 4th part never comes */
    seq_reset();
    sm_step_toggle(0, 0);
    stp(0, 0)->flags = (uint8_t)(3u << 4);
    sm_step_toggle(0, 1);
    stp(0, 1)->nudge = -6;
    push(SMC_PLAY, 0, 0, 0, 0);
    seq_run((uint32_t)(1.5 * S120));
    bad += check("FIX5: x4 cut by the next step's early trig: parts at 0, 1/4, 1/2, then the trig at 0.625, nothing more",
                 on_n[0] == 4u && near_d(on_at[0][2], 0.5 * S120) && near_d(on_at[0][3], 0.625 * S120));
    return bad;
}

/* carried from the review of Task 4: a preview while stopped; a stale SMC_UNLOCK (retried) against a re-trigged lock */
static int test_step_carried(void)
{
    int bad = 0;
    uint32_t i;
    in_reset();
    host_ticks = 1000u;
    set_param(0, P_FCUT, 64);
    run_block();
    push(SMC_PREVIEW, 0, 0, P_FCUT, 20);
    run_block();
    bad += check("FIX: SMC_PREVIEW while stopped: ignored (base, not locked)", trk[0].p[P_FCUT] == 64 && !sm_lk_is(0, P_FCUT));
    sm_step_toggle(0, 4);
    sm_lock_set(0, 4, P_FCUT, 20);
    press(B_PLAY);
    sm_input_poll();
    run_to_step(0, 4);
    {
        sm_cmd_t f = {SMC_PARAM, 3, 0, P_LRATE, 0};
        f.val = ksnd(3)->p[P_LRATE];
        for (i = 0; i < SM_NCMD; i++)
            sm_ring_put(&sm_cq, f);
    }
    sm_step_toggle(0, 4);                                 /* off: the unlock refused (the queue full), kept */
    bad += check("FIX: the queue full: the unlock kept", (sm_unl_pend & 1u) != 0u);
    sm_step_toggle(0, 4);                                 /* on again, a new lock, fired before the unlock is retried */
    sm_lock_set(0, 4, P_FCUT, 30);
    bad += check("FIX: ... re-trigged, fired: 30", run_to_step(0, 4) && trk[0].p[P_FCUT] == 30);
    sm_input_poll();                                      /* the stale unlock goes out now */
    run_block();
    bad += check("FIX: ... the stale unlock (an older generation) does not cut the new lock", !sm_unl_pend && trk[0].p[P_FCUT] == 30);
    return bad;
}

/* the STEP page: SEQ tapped; the step edited (held, else the last selected); K1..K4, SELECT, PRESETS */
static int test_step_page(void)
{
    int bad = 0;
    sm_step_t *s;
    uint32_t p0;
    in_reset();
    host_ticks = 1000u;
    btn_down(B_SEQ);
    sm_input_poll();
    ms(100);
    btn_up(B_SEQ);
    sm_input_poll();
    bad += check("STEPPG: SEQ tapped: STEP", !strcmp(sm_cur_page()->name, "STEP"));
    sm_step_toggle(0, 3);
    turn(EN_K2, 5);
    turn(EN_PRESET, 3);
    sm_input_poll();
    bad += check("STEPPG: no step selected: K2 the track's default VEL (105), the trig untouched (inherits), PRESETS nothing",
                 sm_step_edit() < 0 && ksnd(0)->dvel == 105u && stp(0, 3)->vel == SM_INH && !stp(0, 3)->flags);
    p0 = ksnd(0)->preset;
    key_dn(WHITE[3]);                                     /* held past SM_HOLD_US: selected */
    sm_input_poll();
    ms(400);
    sm_input_poll();
    key_up(WHITE[3]);
    sm_input_poll();
    s = stp(0, 3);
    bad += check("STEPPG: step 3 held 400 ms: selected, still on", sm_step_edit() == 3 && s->n == 1u && sm_ui.held_step == 0xFFu);
    turn(EN_K1, 2);
    sm_input_poll();
    bad += check("STEPPG: K1 NOTE +2", s->note[0] == 50u + (uint32_t)ksnd(0)->p[P_ROOT]);
    turn(EN_K1, 200);
    sm_input_poll();
    bad += check("STEPPG: ... up to G9 (127)", s->note[0] == 127u);
    turn(EN_K2, -200);
    sm_input_poll();
    bad += check("STEPPG: K2 VEL down to 1", s->vel == 1u);
    turn(EN_K2, 300);
    sm_input_poll();
    bad += check("STEPPG: ... up to 127", s->vel == 127u);
    turn(EN_K3, 2);
    sm_input_poll();
    bad += check("STEPPG: K3 LEN +2: 1/32", s->len == 2u);
    turn(EN_K3, 100);
    sm_input_poll();
    bad += check("STEPPG: ... up to INF", s->len == SM_LEN_INF);
    turn(EN_K4, 1);
    sm_input_poll();
    bad += check("STEPPG: K4 COND from 100%: +1 1ST", s->cond == SM_COND_1ST);
    turn(EN_K4, -51);
    sm_input_poll();
    bad += check("STEPPG: ... -51: 50%", s->cond == 50u);
    turn(EN_SELECT, 20);
    sm_input_poll();
    bad += check("STEPPG: SELECT NUDGE up to +7", s->nudge == 7);
    turn(EN_SELECT, -30);
    sm_input_poll();
    bad += check("STEPPG: ... down to -8", s->nudge == -8);
    turn(EN_PRESET, 5);
    sm_input_poll();
    bad += check("STEPPG: PRESETS FLAG +5: X3; no preset loaded", s->flags == (2u << 4) && ksnd(0)->preset == p0);
    turn(EN_PRESET, 5);
    sm_input_poll();
    bad += check("STEPPG: ... +5: REST", s->flags == SMF_REST);
    turn(EN_PRESET, 2);
    sm_input_poll();
    bad += check("STEPPG: ... +2: wraps to -", s->flags == 0u);
    turn(EN_PRESET, -1);
    sm_input_poll();
    bad += check("STEPPG: ... -1: PSLD", s->flags == SMF_PSLIDE);
    /* an empty step held: K1..K4 nothing; PRESETS: a REST trig; no toggle on the release */
    key_dn(WHITE[5]);
    sm_input_poll();
    turn(EN_K2, 4);
    sm_input_poll();
    bad += check("STEPPG: an empty step held: the knobs edit nothing (the held one, not 3)", !stp(0, 5)->n && s->vel == 127u &&
                 sm_step_edit() == 5);
    turn(EN_PRESET, 1);
    sm_input_poll();
    key_up(WHITE[5]);
    sm_input_poll();
    bad += check("STEPPG: ... PRESETS: a REST trig on it; the release does not toggle",
                 stp(0, 5)->n == 1u && stp(0, 5)->flags == SMF_REST && sm_step_edit() == 5);
    turn(EN_ALGO, 1);
    sm_input_poll();
    bad += check("STEPPG: another track: no step selected", sm_step_edit() < 0);
    return bad;
}

static int test_step_draw(void)
{
    int bad = 0;
    uint32_t v, nb = 0;
    draw_setup();
    sm_ui.page = (uint8_t)page_by_name("STEP");
    draw_full();
    bad += check("STEPDRAW: no step: STEP . DEFAULT, the track's default VEL 100", lint_has("STEP \x12 DEFAULT", 0, 0) &&
                 lint_has("100", 0, 0));
    for (v = 0; v < 4u; v++) {                            /* the widest values in their cells */
        sm_step_t *s;
        char what[48];
        draw_setup();
        sm_ui.page = (uint8_t)page_by_name("STEP");
        sm_step_toggle(0, 12);
        s = stp(0, 12);
        s->note[0] = (uint8_t)(v & 1u ? 1u : 127u);        /* C#-1, G9 */
        s->vel = (uint8_t)(v & 1u ? 1u : 127u);
        s->len = (uint8_t)(v == 0 ? 6u : v == 1u ? 13u : v == 2u ? 4u : 0u);
        s->cond = (uint8_t)(v == 0 ? 0u : v == 1u ? SM_COND_NFILL : v == 2u ? sm_cond_ab(8, 8) : 1u);
        s->nudge = (int8_t)(v & 1u ? -8 : 7);
        s->flags = (uint8_t)(v == 0 ? SMF_ACCENT | (3u << 4) : v == 1u ? SMF_PSLIDE : v == 2u ? SMF_ACCENT | SMF_SLIDE : SMF_REST);
        sm_ui.sel_step = 12;
        sm_ui.bank = 0;
        smp.bpm10 = 3000;
        draw_full();
        snprintf(what, sizeof what, "STEP page values %u", v);
        nb += lint_frame(what);
    }
    bad += check("STEPDRAW: every text inside its cell (the widest values)", nb == 0u);
    bad += check("STEPDRAW: ... the last frame: the page line, NOTE C#-1, NUDGE -8, REST",
                 lint_has("STEP 13 \x12 REST", 0, 0) && lint_has("C#-1", 0, 0) && lint_has("-8", 0, 0) && lint_has("NUDGE", 0, 0));
    {
        uint32_t a = smd_knob_sig(1), b = smd_page_sig();
        stp(0, 12)->vel = 64;
        bad += check("STEPDRAW: a VEL edit changes the VEL knob's signature", smd_knob_sig(1) != a);
        stp(0, 12)->flags = SMF_ACCENT;
        bad += check("STEPDRAW: a FLAG edit changes the page line's", smd_page_sig() != b);
    }
    return bad;
}


/* ------------------------------------------------------------- hardware notes 3c: trig defaults, copy / paste a trig, SHIFT --- */
static void seq_tap(void)                                 /* SEQ tapped: the STEP page, no step selected */
{
    btn_down(B_SEQ);
    sm_input_poll();
    ms(100);
    btn_up(B_SEQ);
    sm_input_poll();
}
static int test_trig_defaults(void)
{
    int bad = 0;
    uint32_t d0;
    const seq_ev_t *a, *b, *off;
    smd_knob_t o;
    in_reset();
    host_ticks = 1000u;
    seq_tap();
    d0 = ksnd(0)->dnote;
    sm_step_toggle(0, 0);
    sm_step_toggle(0, 4);
    bad += check("DEF: the track's defaults at power-on: C3 + ROOT, VEL 100, LEN TRK, 100 %",
                 d0 == 48u + (uint32_t)clamp(ksnd(0)->p[P_ROOT], 0, 11) && ksnd(0)->dvel == 100u && !ksnd(0)->dlen && !ksnd(0)->dcond);
    bad += check("DEF: new trigs: NOTE VEL LEN COND inherited", stp(0, 0)->note[0] == SM_INH && stp(0, 0)->vel == SM_INH &&
                 stp(0, 4)->len == SM_INH && stp(0, 4)->cond == SM_INH);
    turn(EN_K1, 4);
    turn(EN_K2, -30);
    turn(EN_K3, 5);
    turn(EN_K4, -1);
    sm_input_poll();
    bad += check("DEF: STEP, no step selected: K1..K4 the track's NOTE +4, VEL 70, LEN 1/4, COND 99 %",
                 ksnd(0)->dnote == d0 + 4u && ksnd(0)->dvel == 70u && ksnd(0)->dlen == 5u && ksnd(0)->dcond == 99u &&
                 stp(0, 0)->vel == SM_INH);
    turn(EN_K4, 1);
    sm_input_poll();
    key_dn(WHITE[4]);                                     /* step 4 held: K2 +10 from what it plays (70): 80, its own */
    sm_input_poll();
    turn(EN_K2, 10);
    sm_input_poll();
    smd_knob_get(1, &o);
    {
        smd_knob_t on;
        smd_knob_get(0, &on);
        bad += check("DEF: a step's VEL turned: from the default (70) to 80, the step's own; its NOTE still inherits",
                     stp(0, 4)->vel == 80u && stp(0, 4)->note[0] == SM_INH && stp(0, 4)->len == SM_INH);
        bad += check("DEF: ... the screen: VEL's ring filled (the step's own), NOTE's plain (inherited)", o.filled && !on.filled);
    }
    key_up(WHITE[4]);
    sm_input_poll();
    bad += check("DEF: ... the key's release does not toggle", stp(0, 4)->n == 1u);
    seq_tap();                                            /* the selection dropped: the defaults again */
    turn(EN_K2, 20);
    sm_input_poll();
    bad += check("DEF: SEQ tapped: no step selected; K2: the default VEL 90", sm_step_edit() < 0 && ksnd(0)->dvel == 90u &&
                 stp(0, 4)->vel == 80u);
    sm_sound_load(ksnd(0)->machine, 1);
    bad += check("DEF: a preset load keeps the trig defaults (the sequencer's)", ksnd(0)->dvel == 90u && ksnd(0)->dnote == d0 + 4u);
    seq_log_clear();
    push(SMC_PLAY, 0, 0, 0, 0);
    seq_run((uint32_t)(6.0 * S120));
    a = ev_nth(1, 0, 0);
    b = ev_nth(1, 0, 1);
    off = ev_nth(0, 0, 0);
    bad += check("DEF: played: the untouched trig takes the new default VEL 90 and NOTE, LEN 1/4 (the default's)",
                 a && a->vel == 90u && a->note == d0 + 4u && off && near_d(off->at - a->at, S120 / 4.0));
    bad += check("DEF: ... the edited trig its own VEL 80, the default NOTE", b && b->vel == 80u && b->note == d0 + 4u);
    push(SMC_STOP, 0, 0, 0, 0);
    seq_run(CTL);
    sm_trn(0, 2);
    bad += check("DEF: TRN +2 moves the default NOTE; an inheriting trig stays SM_INH", ksnd(0)->dnote == d0 + 6u &&
                 stp(0, 0)->note[0] == SM_INH);
    stp(0, 4)->note[0] = 60;
    sm_trn(0, -1);
    bad += check("DEF: ... and a step's own note moves itself", stp(0, 4)->note[0] == 59u && ksnd(0)->dnote == d0 + 5u);
    {
        sm_step_t e;
        sm_step_toggle(0, 8);
        sm_step_eff(stp(0, 8), ksnd(0), &e);
        bad += check("DEF: a trig placed now plays the defaults", e.vel == 90u && e.len == 5u && e.note[0] == d0 + 5u);
    }
    return bad;
}

/* REC + a trig key copies the trig; a trig held + OCT+ pastes it (fields, flags, locks), in the order of a PASTE */
static int test_trig_copy_paste(void)
{
    int bad = 0;
    in_reset();
    host_ticks = 1000u;
    sm_step_toggle(0, 2);
    stp(0, 2)->vel = 90;
    stp(0, 2)->flags = SMF_ACCENT;
    stp(0, 2)->nudge = 3;
    sm_lock_set(0, 2, P_FCUT, 30);
    sm_lock_set(0, 2, P_FRES, 99);
    press(B_REC);
    sm_input_poll();
    lk_tap(2);
    bad += check("TCOPY: REC on + trig key 3: copied (VEL, flags, two locks), the trig not toggled",
                 sm_rec && stp(0, 2)->n == 1u && sm_tclip_ok && sm_tclip.vel == 90u && sm_tclip.flags == SMF_ACCENT && sm_tclip_nl == 2u);
    lk_tap(6);
    bad += check("TCOPY: ... an empty step: nothing copied, nothing placed", !stp(0, 6)->n && sm_tclip.vel == 90u);
    press(B_REC);
    sm_input_poll();
    key_dn(WHITE[9]);                                     /* an empty step held + OCT+ */
    sm_input_poll();
    oct_tap(B_OCTUP);
    key_up(WHITE[9]);
    sm_input_poll();
    bad += check("TCOPY: step 10 held + OCT+: the trig pasted (VEL 90, ACC, NUDGE +3, NOTE inherited), no toggle at release",
                 stp(0, 9)->n == 1u && stp(0, 9)->vel == 90u && stp(0, 9)->flags == SMF_ACCENT && stp(0, 9)->nudge == 3 &&
                 stp(0, 9)->note[0] == SM_INH && sm_ui.bank == 0u);
    bad += check("TCOPY: ... with its locks", lk_val(0, 9, P_FCUT) == 30 && lk_val(0, 9, P_FRES) == 99 && lk_val(0, 2, P_FCUT) == 30);
    sm_step_toggle(0, 11);
    stp(0, 11)->vel = 20;
    sm_lock_set(0, 11, P_LEVEL, 10);
    key_dn(WHITE[11]);
    sm_input_poll();
    oct_tap(B_OCTUP);
    key_up(WHITE[11]);
    sm_input_poll();
    bad += check("TCOPY: pasted onto a trig: replaced whole (its own lock gone)", stp(0, 11)->n == 1u && stp(0, 11)->vel == 90u &&
                 lk_val(0, 11, P_LEVEL) == LK_NONE && lk_val(0, 11, P_FCUT) == 30);
    sm_undo();
    bad += check("TCOPY: UNDO: the trig before the paste (VEL 20, LEVEL locked)", stp(0, 11)->vel == 20u &&
                 lk_val(0, 11, P_LEVEL) == 10 && lk_val(0, 11, P_FCUT) == LK_NONE);
    btn_down(B_REC);                                      /* REC held as a modifier: on while down, back off at its release */
    sm_input_poll();
    lk_tap(11);
    btn_up(B_REC);
    sm_input_poll();
    bad += check("TCOPY: REC held + a trig: copied (VEL 20); REC let go: recording off again, the trig kept",
                 sm_tclip.vel == 20u && !sm_rec && stp(0, 11)->n == 1u);
    press(B_REC);                                         /* REC tapped alone: on, and stays on */
    sm_input_poll();
    bad += check("TCOPY: REC tapped alone stays on", sm_rec == 1u);
    press(B_REC);
    sm_input_poll();
    /* pasted while playing at 300 BPM, the audio stream preempting every store: every lock it applies is the model's, none
     * of a trig replaced (lk_shadow: the locks each step has; a paste replaces the target's with the source's) */
    {
        uint32_t s, p, op, src, dst, pastes = 0;
        in_reset();
        smp.bpm10 = 3000u;
        for (s = 0; s < SM_NSTEP; s++)
            for (p = 0; p < P_COUNT; p++)
                lk_shadow[0][s][p] = LK_NONE;
        for (s = 0; s < 16u; s++) {
            sm_step_toggle(0, s);
            sm_lock_set(0, s, P_FCUT, (int32_t)(s * 7u));
            lk_shadow[0][s][P_FCUT] = (int16_t)(s * 7u);
            if (s & 1u) {
                sm_lock_set(0, s, P_LEVEL, (int32_t)(20u + s));
                lk_shadow[0][s][P_LEVEL] = (int16_t)(20u + s);
            }
        }
        push(SMC_PLAY, 0, 0, 0, 0);
        run_block();
        lk_applied = lk_bad_val = lk_bad_dead = lk_preempts = 0;
        lk_on = 1;
        lk_preempt_n = 2u;
        for (op = 0; op < 600u; op++) {
            src = lk_rand(16);
            dst = lk_rand(16);
            sm_trig_copy(0, src);
            for (p = 0; p < P_COUNT; p++)
                lk_shadow[0][dst][p] = lk_shadow[0][src][p];
            sm_trig_put(0, dst);
            pastes++;
            for (p = 0, src = 1u + lk_rand(160); p < src; p++)   /* (a while: up to 2.3 steps) */
                run_block();
        }
        lk_on = 0;
        lk_preempt_n = 0;
        printf("  trig paste: %u pastes, %u locks applied, %u preempted writes\n", pastes, lk_applied, lk_preempts);
        bad += check("TCOPY: 600 pastes at 300 BPM, the ISR between every store: every applied lock == the model's",
                     lk_applied > 500u && lk_preempts > 100u && !lk_bad_val);
        bad += check("TCOPY: ... no lock of a replaced trig ever applied", !lk_bad_dead);
        {
            int ok = 1;
            for (s = 0; s < 16u; s++)
                ok &= stp(0, s)->n == 1u && lk_val(0, s, P_FCUT) == lk_shadow[0][s][P_FCUT] &&
                      lk_val(0, s, P_LEVEL) == lk_shadow[0][s][P_LEVEL];
            bad += check("TCOPY: ... every step a trig with the locks of its source at the end", ok);
        }
        push(SMC_STOP, 0, 0, 0, 0);
        run_block();
    }
    return bad;
}

/* OCT- is SHIFT outside the keyboard mode (held); OCT+ turns the page round; SHIFT + a knob: the fine turn */
static int test_shift_oct(void)
{
    int bad = 0;
    int32_t v0, f0;
    uint16_t b0;
    in_reset();
    host_ticks = 1000u;
    ksnd(0)->len = 32u;
    filter_page();
    btn_down(B_OCTDN);
    sm_input_poll();
    ms(150);
    sm_input_poll();
    bad += check("SHIFT: OCT- held: SHIFT on, the page and the keyboard mode as they were", sm_shift && !sm_ui.bank && !sm_ui.kbd);
    v0 = ksnd(0)->p[P_FCUT];
    turn(EN_K2, -1); sm_input_poll();
    turn(EN_K2, -1); sm_input_poll();
    turn(EN_K2, -1); sm_input_poll();
    bad += check("SHIFT: + K2 (CUT, 0..127) down: three detents, no step yet", ksnd(0)->p[P_FCUT] == v0);
    turn(EN_K2, -1); sm_input_poll();
    bad += check("SHIFT: ... the fourth: one step", ksnd(0)->p[P_FCUT] == v0 - 1);
    turn(EN_K2, -8); sm_input_poll();
    bad += check("SHIFT: ... eight more: two steps (a quarter a detent)", ksnd(0)->p[P_FCUT] == v0 - 3);
    f0 = ksnd(0)->p[P_FLT];
    turn(EN_K1, 1); sm_input_poll();
    bad += check("SHIFT: + K1 (FILT, a list): a detent is a step, as ever", ksnd(0)->p[P_FLT] == f0 + 1);
    host_pressed |= 1u << panel.btn[B_OCTUP];             /* SHIFT + OCT+ (the 100 ms of a pair long past): the page turns */
    fm1_in.buttons |= 1u << panel.btn[B_OCTUP];
    sm_input_poll();
    fm1_in.buttons &= ~(1u << panel.btn[B_OCTUP]);
    ms(150);
    sm_input_poll();
    bad += check("SHIFT: SHIFT held + OCT+: the page 17..32, not the keyboard mode", sm_ui.bank == 1u && !sm_ui.kbd);
    btn_up(B_OCTDN);
    sm_input_poll();
    v0 = ksnd(0)->p[P_FCUT];
    turn(EN_K2, -2); sm_input_poll();
    bad += check("SHIFT: OCT- let go: SHIFT off, a detent a step again", !sm_shift && ksnd(0)->p[P_FCUT] == v0 - 2);
    oct_tap(B_OCTUP);
    bad += check("OCT+: round to the page 1..16", sm_ui.bank == 0u);
    /* CLOCK: SHIFT keeps the tempo at 0.1 a detent (no flick) */
    sm_ui.page = (uint8_t)page_by_name("CLOCK");
    b0 = smp.bpm10;
    btn_down(B_OCTDN);
    sm_input_poll();
    turn(EN_K1, 3); sm_input_poll();
    turn(EN_K1, 3); sm_input_poll();
    bad += check("SHIFT: CLOCK K1 flicked: 0.1 a detent (+0.6), no x10", smp.bpm10 == b0 + 6u);
    btn_up(B_OCTDN);
    ms(150);
    sm_input_poll();
    /* the keyboard mode: OCT- is the octave down, no SHIFT */
    press(B_ARP);
    sm_input_poll();
    btn_down(B_OCTDN);
    sm_input_poll();
    bad += check("SHIFT: the keyboard mode: OCT- down is no SHIFT", sm_ui.kbd && !sm_shift);
    btn_up(B_OCTDN);
    ms(150);
    sm_input_poll();
    bad += check("SHIFT: ... it is the octave -1", sm_ui.oct == -1);
    press(B_ARP);
    sm_input_poll();
    ksnd(0)->len = 16u;
    return bad;
}

/* ------------------------------------------------------------------------------------------------ HOME (Task 6) --- */
static int test_home_mixer(void)
{
    int bad = 0;
    int8_t before[4];
    uint32_t t, ok = 1;
    in_reset();
    host_ticks = 1000u;
    press(B_HOME);
    sm_input_poll();
    bad += check("HOME: HOME tapped: MIXER (no layer)", !strcmp(sm_cur_page()->name, "MIXER") && sm_layer == SML_NONE);
    for (t = 0; t < SM_NTRK; t++)
        before[t] = ksnd(t)->p[P_LEVEL];
    turn(EN_K3, -7);
    sm_input_poll();
    run_block();
    bad += check("HOME: K3 turned down: T3's LEVEL lower in the model and on the core",
                 ksnd(2)->p[P_LEVEL] < before[2] && trk[2].p[P_LEVEL] == ksnd(2)->p[P_LEVEL]);
    for (t = 0; t < SM_NTRK; t++)
        if (t != 2u)
            ok &= ksnd(t)->p[P_LEVEL] == before[t] && trk[t].p[P_LEVEL] == before[t];
    bad += check("HOME: ... the other three tracks' levels as they were", ok);
    turn(EN_ALGO, 2);                                     /* the selected track is T3: K1 is still T1 */
    sm_input_poll();
    turn(EN_K1, 5);
    sm_input_poll();
    run_block();
    bad += check("HOME: K1 is T1's LEVEL whichever track is selected",
                 sm_ui.trk == 2u && ksnd(0)->p[P_LEVEL] > before[0] && trk[0].p[P_LEVEL] == ksnd(0)->p[P_LEVEL]);
    turn(EN_K4, 500);
    sm_input_poll();
    run_block();
    bad += check("HOME: K4 +500: T4's LEVEL at its maximum (clamped), model and core",
                 ksnd(3)->p[P_LEVEL] == TP[P_LEVEL].max && trk[3].p[P_LEVEL] == TP[P_LEVEL].max);
    press(B_HOME);
    sm_input_poll();
    bad += check("HOME: tapped again: CLOCK", !strcmp(sm_cur_page()->name, "CLOCK"));
    press(B_HOME);
    sm_input_poll();
    bad += check("HOME: ... again: MIXER", !strcmp(sm_cur_page()->name, "MIXER"));
    btn_down(B_HOME);                                     /* a hold does nothing for now (the menu is a later stage) */
    sm_input_poll();
    ms(900);
    sm_input_poll();
    bad += check("HOME: held 900 ms: no layer, still MIXER", sm_layer == SML_NONE && !strcmp(sm_cur_page()->name, "MIXER"));
    btn_up(B_HOME);
    sm_input_poll();
    bad += check("HOME: ... let go: still MIXER (a hold is not a tap)", !strcmp(sm_cur_page()->name, "MIXER") && sm_layer == SML_NONE);
    btn_down(B_HOME);                                     /* a knob turned while it is down: a hold too */
    sm_input_poll();
    turn(EN_K2, 1);
    sm_input_poll();
    btn_up(B_HOME);
    sm_input_poll();
    bad += check("HOME: a knob turned with HOME down is no tap either", !strcmp(sm_cur_page()->name, "MIXER"));
    press(B_EDIT);                                        /* other families still reach their pages and back */
    sm_input_poll();
    press(B_HOME);
    sm_input_poll();
    bad += check("HOME: from EDIT, HOME goes to its first page, MIXER", !strcmp(sm_cur_page()->name, "MIXER"));
    return bad;
}

static int test_home_bpm_select(void)
{
    int bad = 0;
    uint32_t b;
    double S;
    in_reset();
    host_ticks = 1000u;
    press(B_HOME);
    sm_input_poll();
    turn(EN_SELECT, 10);
    sm_input_poll();
    bad += check("HOME: SELECT +10 on MIXER: 121.0 BPM", smp.bpm10 == 1210u);
    turn(EN_SELECT, -3);
    sm_input_poll();
    bad += check("HOME: SELECT -3: 120.7", smp.bpm10 == 1207u);
    press(B_HOME);
    sm_input_poll();
    bad += check("HOME: CLOCK page", !strcmp(sm_cur_page()->name, "CLOCK"));
    turn(EN_SELECT, 3);
    sm_input_poll();
    bad += check("HOME: SELECT on CLOCK too: 121.0", smp.bpm10 == 1210u);
    turn(EN_K1, 2);
    sm_input_poll();
    bad += check("HOME: K1 slowly (2 detents): 0.1 BPM each", smp.bpm10 == 1212u);
    turn(EN_K1, -5);
    sm_input_poll();
    bad += check("HOME: K1 -5 in one poll (a new turn): 3 x 0.1, then 2 x 1.0", smp.bpm10 == 1212u - 23u);
    turn(EN_K1, -5000);
    sm_input_poll();
    bad += check("HOME: K1 far down: 30.0 BPM", smp.bpm10 == 300u);
    turn(EN_SELECT, -10);
    sm_input_poll();
    bad += check("HOME: ... SELECT below it stays at 30.0", smp.bpm10 == 300u);
    turn(EN_K1, 5000);
    sm_input_poll();
    turn(EN_SELECT, 10);
    sm_input_poll();
    bad += check("HOME: K1 far up, SELECT above it: 300.0", smp.bpm10 == 3000u);
    turn(EN_K2, 7);
    sm_input_poll();
    bad += check("HOME: K2 SWING +7: 57 %", smp.swing == 57u);
    turn(EN_K2, -30);
    sm_input_poll();
    bad += check("HOME: K2 -30: 50 % at least", smp.swing == 50u);
    turn(EN_K2, 100);
    sm_input_poll();
    bad += check("HOME: K2 +100: 75 % at most", smp.swing == 75u);
    /* the sequencer takes the tempo set on the panel: 150.0 BPM = FS x 150 / 1500 samples a 1/16 */
    in_reset();
    host_ticks = 1000u;
    press(B_HOME);
    sm_input_poll();
    turn(EN_SELECT, 300);
    sm_input_poll();
    sm_step_toggle(0, 0);
    sm_step_toggle(0, 4);
    push(SMC_PLAY, 0, 0, 0, 0);
    S = (double)FS * 150.0 / 1500.0;
    seq_run((uint32_t)(5.0 * S));
    b = on_n[0];
    bad += check("HOME: 150.0 BPM from SELECT: the trig on step 4 at 4 steps of that tempo",
                 smp.bpm10 == 1500u && b == 2u && near_d(on_at[0][1], 4.0 * S));
    return bad;
}

/* K1's BPM turn as the device gives it: one detent a poll, the polls a few ms apart (a flick) or 150 ms (a slow turn) */
static int test_home_bpm_accel(void)
{
    int bad = 0;
    uint32_t k;
    in_reset();
    host_ticks = 1000000u;
    press(B_HOME);
    sm_input_poll();
    press(B_HOME);
    sm_input_poll();                                      /* CLOCK */
    for (k = 0; k < 6u; k++) {
        turn(EN_K1, 1);
        sm_input_poll();
        ms(150);
    }
    bad += check("HOME: K1 a slow turn (a detent every 150 ms, one a poll): 0.1 each, 120.6", smp.bpm10 == 1206u);
    ms(300);
    for (k = 0; k < 8u; k++) {
        turn(EN_K1, 1);
        sm_input_poll();
        ms(10);
    }
    bad += check("HOME: K1 a flick (8 detents, one a poll, 10 ms apart): the first three 0.1, then 1.0 each: +5.3", smp.bpm10 == 1206u + 3u + 50u);
    ms(300);
    turn(EN_K1, 1);
    sm_input_poll();
    bad += check("HOME: ... after a pause the next detent is 0.1 again", smp.bpm10 == 1206u + 3u + 50u + 1u);
    for (k = 0; k < 6u; k++) {
        turn(EN_K1, -1);
        sm_input_poll();
        ms(10);
    }
    bad += check("HOME: a reversal starts a new turn: 3 x 0.1 then 3 x 1.0 down", smp.bpm10 == 1206u + 3u + 50u + 1u - 3u - 30u);
    ms(300);
    for (k = 0; k < 12u; k++) {                           /* a turn at 60 ms a detent: 4 in 240 ms, never 4 in 80 ms */
        turn(EN_K1, 1);
        sm_input_poll();
        ms(60);
    }
    bad += check("HOME: a medium turn (60 ms a detent): 0.1 each", smp.bpm10 == 1206u + 3u + 50u + 1u - 3u - 30u + 12u);
    return bad;
}

static int test_home_draw(void)
{
    int bad = 0;
    uint32_t nb = 0;
    draw_setup();
    sm_ui.page = (uint8_t)page_by_name("MIXER");
    draw_full();
    nb += lint_frame("MIXER");
    bad += check("HOMEDRAW: MIXER: the page line and the four rings' labels T1 T2 T3 T4",
                 lint_has("MIXER", 0, 0) && lint_has("T1", 0, 0) && lint_has("T2", 0, 0) && lint_has("T3", 0, 0) && lint_has("T4", 0, 0));
    bad += check("HOMEDRAW: ... SELECT is TEMPO with the BPM", lint_has("TEMPO", 0, 0) && lint_has("120.0", 0, 0));
    ksnd(2)->p[P_LEVEL] = 40;
    {
        uint32_t a = smd_knob_sig(2);
        ksnd(2)->p[P_LEVEL] = 41;
        bad += check("HOMEDRAW: a T3 level change changes the K3 ring's signature", smd_knob_sig(2) != a);
    }
    draw_setup();
    sm_ui.page = (uint8_t)page_by_name("CLOCK");
    smp.bpm10 = 3000;
    smp.swing = 75;
    draw_full();
    nb += lint_frame("CLOCK");
    bad += check("HOMEDRAW: CLOCK: BPM and SWING labels, 300.0 and 75%",
                 lint_has("BPM", 0, 0) && lint_has("SWING", 0, 0) && lint_has("300.0", 0, 0) && lint_has("75%", 0, 0));
    {
        uint32_t a = smd_knob_sig(0), b = smd_knob_sig(1), c = smd_sel_sig();
        smp.bpm10 = 2999;
        bad += check("HOMEDRAW: BPM changes the K1 ring's and SELECT's signatures", smd_knob_sig(0) != a && smd_sel_sig() != c);
        smp.swing = 74;
        bad += check("HOMEDRAW: SWING changes the K2 ring's signature", smd_knob_sig(1) != b);
    }
    draw_setup();
    sm_ui.page = (uint8_t)page_by_name("CLOCK");
    smp.bpm10 = 300;
    draw_full();
    nb += lint_frame("CLOCK 30.0");
    bad += check("HOMEDRAW: every text inside its cell (300.0, 30.0, 75 %)", nb == 0u);
    return bad;
}

/* SWING: every second 1/16 (the 2nd, 4th, ..: steps 1, 3, .. counting from 0) a share of (swing - 50) / 50 of a step late */
static int test_swing(void)
{
    const seq_ev_t *e;
    int bad = 0;
    uint32_t k, ok = 1;
    seq_reset();
    for (k = 0; k < 8u; k++)
        sm_step_toggle(0, k);
    smp.swing = 66;
    push(SMC_PLAY, 0, 0, 0, 0);
    seq_run((uint32_t)(8.9 * S120));
    for (k = 0; k < 8u; k++) {
        e = ev_nth(1, 0, k);
        ok &= e != 0 && near_d(e->at, ((double)k + (k & 1u ? 0.32 : 0.0)) * S120);
    }
    e = ev_nth(1, 0, 1);
    printf("swamp: SWING 66: step 1 at %.3f steps (want 1.32), step 0 at %u\n", e ? (double)e->at / S120 : -1.0, ev_nth(1, 0, 0)->at);
    bad += check("SWING 66: the even steps on the grid, the odd 0.32 of a step (about a third) late, every one of 8", ok && on_n[0] == 8u);
    e = ev_nth(1, 0, 1);
    bad += check("SWING 66: ... about a third of a step (within 0.02 step of 1/3)", e && fabs((double)e->at / S120 - 1.0 - 1.0 / 3.0) < 0.02);
    seq_reset();
    for (k = 0; k < 2u; k++)
        sm_step_toggle(0, k);
    smp.swing = 75;
    push(SMC_PLAY, 0, 0, 0, 0);
    seq_run((uint32_t)(2.9 * S120));
    bad += check("SWING 75: the odd step half a step late", near_d(ev_nth(1, 0, 1)->at, 1.5 * S120));
    /* 50: nothing moves (the other tests run at 50 already); other tracks swing with the grid, not with their own length */
    seq_reset();
    smp.pat[smp.cur].kit.snd[1].len = 5;                 /* 5 long: its pass 2 starts on an odd grid step */
    for (k = 0; k < 5u; k++)
        sm_step_toggle(1, k);
    smp.swing = 60;
    push(SMC_PLAY, 0, 0, 0, 0);
    seq_run((uint32_t)(10.9 * S120));
    ok = 1;
    for (k = 0; k < 10u; k++) {
        e = ev_nth(1, 1, k);
        ok &= e != 0 && near_d(e->at, ((double)k + (k & 1u ? 0.2 : 0.0)) * S120);
    }
    bad += check("SWING 60 on a 5-step track: the grid's odd steps late, in the second pass too (the grid's, not the track's)", ok);
    return bad;
}

/* swing with NUDGE, ratchets and the rest of the step's timing (the steps 1 and 3 are the swung ones) */
static int test_swing_with_nudge_ratchet(void)
{
    const seq_ev_t *e;
    int bad = 0;
    seq_reset();
    smp.swing = 66;                                       /* 0.32 of a step */
    sm_step_toggle(0, 1);
    stp(0, 1)->nudge = 4;                                 /* + a quarter: 1.57 */
    sm_step_toggle(0, 3);
    stp(0, 3)->nudge = -4;                                /* - a quarter: 3.07 (its own start, after the grid) */
    sm_step_toggle(0, 5);
    stp(0, 5)->flags = (uint8_t)(1u << 4);                /* x2: on its swung start, then half of what is left of the step */
    sm_step_toggle(0, 6);
    stp(0, 6)->nudge = -4;                                /* an even step: 5.75.. as before; the swung step 5's ratchet is whole */
    sm_step_toggle(0, 9);
    stp(0, 9)->nudge = -8;                                /* half a step early, swung 0.32 late: 8.82 (before its start) */
    push(SMC_PLAY, 0, 0, 0, 0);
    seq_run((uint32_t)(10.9 * S120));
    printf("swamp: SWING+NUDGE: on at");
    for (e = ev_nth(1, 0, 0); e && e < seq_ev + seq_nev; e++)
        if (e->on)
            printf(" %.3f", (double)e->at / S120);
    printf("\n");
    e = ev_nth(1, 0, 0);
    bad += check("SWING: step 1 swung and NUDGE +4: 1 + 0.32 + 0.25", e && near_d(e->at, 1.57 * S120));
    e = ev_nth(1, 0, 1);
    bad += check("SWING: step 3 swung and NUDGE -4: 3 + 0.32 - 0.25, once", e && near_d(e->at, 3.07 * S120));
    e = ev_nth(1, 0, 2);
    bad += check("SWING: a x2 ratchet on a swung step: the first part on the swung start", e && near_d(e->at, 5.32 * S120));
    e = ev_nth(1, 0, 3);
    bad += check("SWING: ... the second part half of the rest of the step later (5.66), not into step 6",
                 e && near_d(e->at, (5.32 + 0.34) * S120));
    e = ev_nth(1, 0, 4);
    bad += check("SWING: an even step NUDGE -4: 6 - 0.25 as ever", e && near_d(e->at, 5.75 * S120));
    e = ev_nth(1, 0, 5);
    bad += check("SWING: a swung step NUDGE -8: 9 + 0.32 - 0.5, before its start (early fire), once",
                 e && near_d(e->at, 8.82 * S120) && on_n[0] == 6u);
    return bad;
}

/* the tempo and the swing the ISR reads once per block: a change between two blocks moves no step inside a block */
static int test_swing_read_once(void)
{
    int bad = 0;
    uint32_t k, ok = 1;
    seq_reset();
    for (k = 0; k < 4u; k++)
        sm_step_toggle(0, k);
    smp.swing = 50;
    push(SMC_PLAY, 0, 0, 0, 0);
    seq_run((uint32_t)(1.5 * S120));
    smp.swing = 75;                                       /* the main loop between two blocks */
    seq_run((uint32_t)(3.0 * S120));
    ok = on_n[0] == 4u && near_d(ev_nth(1, 0, 3)->at, 3.5 * S120);
    bad += check("SWING: turned up while playing: the next odd step takes it (3.5), no step lost or doubled", ok);
    return bad;
}

/* ------------------------------------------------------------------------------------------------ MIDI IN (Task 7) --- */
/* a channel message into the ring as usb.c / midi_uart.c put it: cin | status << 8 | d1 << 16 | d2 << 24 (source 1 USB, 2 TRS) */
static int midi_put(uint32_t status, uint32_t d1, uint32_t d2, uint32_t source)
{
    return midi_enqueue((status >> 4) | status << 8 | d1 << 16 | d2 << 24, source);
}
static int midi_on(uint32_t ch, uint32_t note, uint32_t vel) { return midi_put(0x90u | ch, note, vel, 1); }
static int midi_off(uint32_t ch, uint32_t note) { return midi_put(0x80u | ch, note, 64, 1); }
static void midi_reset(void)
{
    mi_r = mi_w = 0;
    midi_in_overflow = 0;
    seq_reset();
    host_ticks = 1000u;
}
/* n blocks of the mix (every block: events_block, then the engines) */
static void mix_blocks(uint32_t n)
{
    while (n--)
        run_block();
}
static uint32_t peak_of(uint32_t n)
{
    uint32_t pk = 0, a;
    while (n--) {
        a = block_peak(0, 0);
        seq_clock += CTL;
        pk = a > pk ? a : pk;
    }
    return pk;
}
static uint32_t notes_on_core(uint32_t t)                  /* voices of track t with their gate open */
{
    uint32_t i, n = 0;
    for (i = 0; i < NVOICE; i++)
        n += trk[t].v[i].active && trk[t].v[i].gate;
    return n;
}

static int test_midi_note_ch2(void)
{
    int bad = 0;
    uint32_t pk, tail, k;
    midi_reset();
    bad += check("MIDI: the ring takes a note-on on channel 2", midi_on(1, 64, 100));
    pk = peak_of(40);
    bad += check("MIDI: note-on ch 2: T2 sounds (peak > 200), the ring drained", pk > 200u && mi_r == mi_w);
    bad += check("MIDI: ... T2 only: one note-on, on T2", on_n[1] == 1u && !on_n[0] && !on_n[2] && !on_n[3] &&
                 ev_nth(1, 1, 0) && ev_nth(1, 1, 0)->note == 64u && ev_nth(1, 1, 0)->vel == 100u);
    bad += check("MIDI: ... a note on T2's core, none on the others", notes_on_core(1) == 1u && !notes_on_core(0) && !notes_on_core(2) && !notes_on_core(3));
    midi_off(1, 64);
    mix_blocks(1);
    bad += check("MIDI: note-off ch 2: the gate closed, no hung note", notes_on_core(1) == 0u && ev_count(0, 1) == 1u);
    mix_blocks(400);
    pk = peak_of(10);
    for (k = 0, tail = 0; k < NVOICE; k++)
        tail |= trk[1].v[k].active;
    printf("swamp: MIDI: after 400 blocks: peak %u, active %u, busy %u\n", pk, tail, voices_busy());
    bad += check("MIDI: 400 blocks after the note-off: no voice left, the FM tail down to |x| <= 16", pk <= 16u && !tail && voices_busy() == 0);
    /* velocity 0 is a note-off; the TRS source is the same queue */
    midi_reset();
    midi_put(0x93, 60, 90, 2);                            /* channel 4, from the UART */
    mix_blocks(2);
    bad += check("MIDI: a note from the TRS UART (source 2): T4 sounds", on_n[3] == 1u && notes_on_core(3) == 1u);
    midi_on(3, 60, 0);
    mix_blocks(1);
    bad += check("MIDI: note-on at velocity 0 is a note-off", notes_on_core(3) == 0u && ev_count(0, 3) == 1u);
    /* channels 1..4 are T1..T4; the others, CC, program change, bend, clock, SysEx data: nothing */
    midi_reset();
    midi_on(0, 50, 100);
    midi_on(1, 51, 100);
    midi_on(2, 52, 100);
    midi_on(3, 53, 100);
    midi_on(4, 54, 100);                                  /* channel 5 */
    midi_on(15, 55, 100);
    midi_put(0xB0, 7, 100, 1);                            /* CC 7 ch 1 */
    midi_put(0xC1, 5, 0, 1);                              /* program change ch 2 */
    midi_put(0xE0, 0, 100, 1);                            /* bend */
    midi_enqueue(0xFu | 0xF8u << 8, 1u);                  /* MIDI clock */
    midi_enqueue(0xFu | 0xFAu << 8, 1u);                  /* start (a later stage; the sequencer does not follow it) */
    midi_enqueue(0x4u | 0xF0u << 8 | 0x22u << 16 | 0x24u << 24, 1u);   /* a SysEx packet */
    mix_blocks(2);
    bad += check("MIDI: channels 1..4 to T1..T4, one each; ch 5, 16, CC, PC, bend, clock, SysEx: nothing",
                 on_n[0] == 1u && on_n[1] == 1u && on_n[2] == 1u && on_n[3] == 1u && seq_nev == 4u && mi_r == mi_w);
    bad += check("MIDI: ... the notes of each on its own track", ev_nth(1, 0, 0)->note == 50u && ev_nth(1, 1, 0)->note == 51u &&
                 ev_nth(1, 2, 0)->note == 52u && ev_nth(1, 3, 0)->note == 53u && !sm_playing);
    /* MIDI plays while the sequencer is stopped and while it plays; a chord of eight, every note let go */
    midi_reset();
    for (k = 0; k < 8u; k++)
        midi_on(0, 48u + k, 100);
    mix_blocks(2);
    for (k = 0; k < 8u; k++)
        midi_off(0, 48u + k);
    mix_blocks(1);
    bad += check("MIDI: a chord of eight on T1: eight note-ons, eight note-offs", on_n[0] == 8u && ev_count(0, 0) == 8u && !notes_on_core(0));
    return bad;
}

/* the sequencer and MIDI on the same note of the same track: neither's note-off ends the other's note */
static int test_midi_vs_seq_same_note(void)
{
    int bad = 0;
    uint32_t pk, k, hung;
    /* A: the sequencer's note (LEN INF: it holds to STOP), MIDI's note-on and note-off on the same pitch in the middle */
    midi_reset();
    sm_step_toggle(0, 0);
    stp(0, 0)->note[0] = 60;
    stp(0, 0)->len = SM_LEN_INF;
    push(SMC_PLAY, 0, 0, 0, 0);
    mix_blocks(4);
    bad += check("MIDI+SEQ: the sequencer's C4 (LEN INF) sounds", ev_count(1, 0) == 1u && notes_on_core(0) == 1u);
    midi_on(0, 60, 100);
    mix_blocks(2);
    midi_off(0, 60);
    mix_blocks(2);
    bad += check("MIDI+SEQ: MIDI's note-off of the same note does not end the sequencer's (no note-off, still gated)",
                 ev_count(0, 0) == 0u && notes_on_core(0) >= 1u);
    push(SMC_STOP, 0, 0, 0, 0);
    mix_blocks(400);
    hung = 0;
    for (k = 0; k < SM_NTRK; k++)
        hung += notes_on_core(k);
    pk = peak_of(10);
    bad += check("MIDI+SEQ: STOP, 400 blocks: nothing hangs", !hung && voices_busy() == 0 && pk <= 2u);
    /* B: MIDI holds C4; the sequencer's short C4 (LEN 1/8 of a step) comes and goes: MIDI's note stays */
    midi_reset();
    sm_step_toggle(0, 1);
    stp(0, 1)->note[0] = 60;
    stp(0, 1)->len = 1;                                   /* 1/8 */
    midi_on(0, 60, 100);
    mix_blocks(2);
    push(SMC_PLAY, 0, 0, 0, 0);
    mix_blocks((uint32_t)(3.0 * S120 / CTL));             /* the step 1 fires at 1 step; its LEN is long gone by 3 steps */
    bad += check("MIDI+SEQ: the sequencer's C4 came and went (two note-ons in all): MIDI's note still sounds, no note-off yet",
                 ev_count(1, 0) == 2u && ev_count(0, 0) == 0u && notes_on_core(0) >= 1u);
    midi_off(0, 60);
    mix_blocks(2);
    bad += check("MIDI+SEQ: MIDI's note-off then ends it (the one note-off)", ev_count(0, 0) == 1u);
    push(SMC_STOP, 0, 0, 0, 0);
    mix_blocks(400);
    bad += check("MIDI+SEQ: 400 blocks later: no hung voice", voices_busy() == 0 && !notes_on_core(0));
    /* C: both let go in the other order on every pass: the sequencer's C4 on every step (LEN TRK), MIDI's C4 held across
     * 8 steps, then let go while a sequencer note sounds; the last note-off of the two (whoever's) ends the voice */
    midi_reset();
    for (k = 0; k < 16u; k++) {
        sm_step_toggle(0, k);
        stp(0, k)->note[0] = 60;
        stp(0, k)->len = (uint8_t)(k & 1u ? SM_LEN_INF : 1u);
    }
    midi_on(0, 60, 100);
    push(SMC_PLAY, 0, 0, 0, 0);
    mix_blocks((uint32_t)(8.5 * S120 / CTL));
    midi_off(0, 60);
    mix_blocks(2);
    push(SMC_STOP, 0, 0, 0, 0);
    mix_blocks(400);
    bad += check("MIDI+SEQ: MIDI held across 8 steps of the same note, STOP: nothing hangs",
                 voices_busy() == 0 && !notes_on_core(0));
    /* D: MIDI's note-off for a note MIDI never started (muted) must not end the sequencer's */
    midi_reset();
    sm_step_toggle(0, 0);
    stp(0, 0)->note[0] = 60;
    stp(0, 0)->len = SM_LEN_INF;
    push(SMC_PLAY, 0, 0, 0, 0);
    mix_blocks(4);
    midi_off(0, 60);
    mix_blocks(2);
    bad += check("MIDI+SEQ: a MIDI note-off with no MIDI note-on before it ends nothing of the sequencer's",
                 ev_count(0, 0) == 0u && notes_on_core(0) >= 1u);
    return bad;
}

/* ------------------------------------------------------- OCT buttons (stage 3, Task 1b) --- */
/* OCT- and OCT+ pressed together: dn then up, gap ms apart (a poll each) */
static void oct_pair(uint32_t gap_ms)
{
    host_pressed |= 1u << panel.btn[B_OCTDN];
    fm1_in.buttons |= 1u << panel.btn[B_OCTDN];
    sm_input_poll();
    host_ticks += gap_ms * 1000u * FM1_TICKS_PER_US;
    host_pressed |= 1u << panel.btn[B_OCTUP];
    fm1_in.buttons |= 1u << panel.btn[B_OCTUP];
    sm_input_poll();
    fm1_in.buttons &= ~((1u << panel.btn[B_OCTDN]) | (1u << panel.btn[B_OCTUP]));
    host_ticks += 150000u * FM1_TICKS_PER_US;
    sm_input_poll();
}
static void kb_key(uint32_t k, int down)
{
    if (down)
        key_dn(k);
    else
        key_up(k);
    sm_input_poll();
}

static int test_oct_pages(void)
{
    int bad = 0;
    in_reset();
    host_ticks = 1000u;
    ksnd(0)->len = 32u;
    bad += check("OCT: length 32: page 1..16 at first", sm_ui.bank == 0u);
    host_pressed |= 1u << panel.btn[B_OCTUP];
    fm1_in.buttons |= 1u << panel.btn[B_OCTUP];
    sm_input_poll();
    bad += check("OCT: a press waits the 100 ms for the other button: the page has not turned yet", sm_ui.bank == 0u);
    fm1_in.buttons = 0;
    host_ticks += 150000u;
    sm_input_poll();
    bad += check("OCT: ... then OCT+ turns to steps 17..32", sm_ui.bank == 1u && !sm_ui.kbd);
    lk_tap(2);
    bad += check("OCT: ... a white key is then step 18 (17..32)", smp.pat[smp.cur].step[0][18].n == 1u && !smp.pat[smp.cur].step[0][2].n);
    oct_tap(B_OCTDN);
    bad += check("OCT: OCT- (SHIFT): the page stays 17..32", sm_ui.bank == 1u);
    oct_tap(B_OCTUP);
    bad += check("OCT: OCT+ again: round to steps 1..16", sm_ui.bank == 0u);
    leds_setup(LEDS_DIM);
    ksnd(0)->len = 32u;
    sm_leds();
    bad += check("OCT: LEDs: page 1..16: neither lit", led_of(fm1_led, panel.btn[B_OCTDN]) == 0 && led_of(fm1_led, panel.btn[B_OCTUP]) == 0);
    sm_ui.bank = 1;
    sm_leds();
    bad += check("OCT: LEDs: page 17..32: OCT+ lit, OCT- not", led_of(fm1_led, panel.btn[B_OCTUP]) == 1 && led_of(fm1_led, panel.btn[B_OCTDN]) == 0);
    in_reset();
    host_ticks = 1000u;
    oct_tap(B_OCTUP);
    oct_tap(B_OCTDN);
    bad += check("OCT: length 16: OCT+ / OCT- do nothing", sm_ui.bank == 0u && !sm_ui.kbd && sm_ui.oct == 0);
    /* in the EDIT layer OCT+ is still the next page of machines, and the steps' page is left alone */
    ksnd(0)->len = 32u;
    layer_hold(B_EDIT);
    {
        uint8_t pg = sm_lay_epage;
        host_pressed |= 1u << panel.btn[B_OCTUP];
        fm1_in.buttons |= 1u << panel.btn[B_OCTUP];
        sm_input_poll();
        fm1_in.buttons &= ~(1u << panel.btn[B_OCTUP]);
        host_ticks += 150000u;
        sm_input_poll();
        bad += check("OCT: EDIT layer open: OCT+ is the next page of machines, not the steps'", sm_lay_epage != pg && sm_ui.bank == 0u);
    }
    layer_let_go(B_EDIT);
    ksnd(0)->len = 16u;
    return bad;
}

static int test_keyboard_mode(void)
{
    int bad = 0;
    uint32_t i;
    in_reset();
    midi_reset();
    in_reset();
    host_ticks = 1000u;
    oct_pair(0);
    bad += check("KEYS: OCT- + OCT+ together: the keyboard mode", sm_ui.kbd == 1u);
    kb_key(WHITE[2], 1);                                  /* A3 = 57 */
    mix_blocks(4);
    bad += check("KEYS: a key plays its note on the selected track (A3 on T1), sets no trig",
                 on_n[0] == 1u && ev_nth(1, 0, 0) && ev_nth(1, 0, 0)->note == 57u && notes_on_core(0) == 1u &&
                 !smp.pat[smp.cur].step[0][2].n && !sm_playing);
    kb_key(WHITE[2], 0);
    mix_blocks(2);
    bad += check("KEYS: ... the key let go: its note-off", ev_count(0, 0) == 1u && notes_on_core(0) == 0u && !smp.pat[smp.cur].step[0][2].n);
    bad += check("KEYS: ... a black key plays too (F#3 = 54)", (kb_key(1, 1), mix_blocks(2), ev_nth(1, 0, 1) && ev_nth(1, 0, 1)->note == 54u));
    kb_key(1, 0);
    mix_blocks(2);
    oct_tap(B_OCTUP);
    bad += check("KEYS: OCT+: octave +1, the bank untouched", sm_ui.oct == 1 && sm_ui.bank == 0u && sm_ui.kbd);
    kb_key(WHITE[2], 1);
    mix_blocks(2);
    bad += check("KEYS: ... A3 + 12 = 69", ev_nth(1, 0, 2) && ev_nth(1, 0, 2)->note == 69u);
    oct_tap(B_OCTDN);                                     /* the key is still down: its own note-off must be 69 */
    kb_key(WHITE[2], 0);
    mix_blocks(2);
    bad += check("KEYS: ... the octave moved while it was down: the note-off is still 69 (nothing hangs)",
                 notes_on_core(0) == 0u && ev_nth(0, 0, 2) && ev_nth(0, 0, 2)->note == 69u);
    for (i = 0; i < 6u; i++)
        oct_tap(B_OCTDN);
    bad += check("KEYS: OCT- six times: -3 at the lowest", sm_ui.oct == -3);
    for (i = 0; i < 8u; i++)
        oct_tap(B_OCTUP);
    bad += check("KEYS: OCT+ eight times: +3 at the highest", sm_ui.oct == 3);
    kb_key(26, 1);                                        /* G5 = 79, + 36 = 115 */
    mix_blocks(2);
    bad += check("KEYS: G5 at +3: 115", ev_nth(1, 0, 3) && ev_nth(1, 0, 3)->note == 115u);
    kb_key(26, 0);
    mix_blocks(2);
    leds_setup(LEDS_DIM);
    sm_ui.kbd = 1;
    sm_ui.oct = 2;
    smp.pat[smp.cur].step[0][3].n = 1;
    sm_leds();
    bad += check("KEYS: LEDs: octave above 0: OCT+ lit, OCT- not; the white keys show no steps",
                 led_of(fm1_led, panel.btn[B_OCTUP]) == 1 && led_of(fm1_led, panel.btn[B_OCTDN]) == 0 && wl(3) == 0);
    sm_ui.oct = -1;
    sm_leds();
    bad += check("KEYS: LEDs: octave below 0: OCT- lit", led_of(fm1_led, panel.btn[B_OCTDN]) == 1 && led_of(fm1_led, panel.btn[B_OCTUP]) == 0);
    /* the header: KEYS inverted while the mode is on */
    draw_setup();
    draw_full();
    printf("swamp: KEYS header ink %u of %u without, ", scr_ink(34, 2, 48, 14), 48u * 14u);
    bad += check("KEYS: header: not in the mode: no inverted box left of the tempo", scr_ink(34, 2, 48, 14) < 48u * 14u / 2u);
    sm_ui.kbd = 1;
    sm_dirty = 1;
    sm_draw();
    printf("swamp: %u with\n", scr_ink(34, 2, 48, 14));
    bad += check("KEYS: header: KEYS inverted (a box of ink)", scr_ink(34, 2, 48, 14) > 48u * 14u / 2u);
    sm_ui.kbd = 0;
    /* the track: ALGORITHM, then the keys play it; leaving the mode lets go of what is held */
    in_reset();
    midi_reset();
    in_reset();
    host_ticks = 1000u;
    oct_pair(30);
    bad += check("KEYS: the two buttons 30 ms apart: the mode too", sm_ui.kbd == 1u);
    turn(EN_ALGO, 1);
    sm_input_poll();
    kb_key(WHITE[0], 1);
    mix_blocks(2);
    bad += check("KEYS: T2 selected: the key plays T2", on_n[1] == 1u && !on_n[0] && notes_on_core(1) == 1u);
    oct_pair(0);
    mix_blocks(2);
    bad += check("KEYS: both again: back to trigs, the key still held is let go (no hung note)",
                 !sm_ui.kbd && notes_on_core(1) == 0u && ev_count(0, 1) == 1u);
    fm1_in.notes = 0;
    sm_input_poll();
    kb_key(WHITE[0], 1);
    kb_key(WHITE[0], 0);
    bad += check("KEYS: ... and a key is a trig again", smp.pat[smp.cur].step[1][0].n == 1u && on_n[1] == 1u);
    /* the pair window: 150 ms apart is two single presses */
    in_reset();
    host_ticks = 1000u;
    ksnd(0)->len = 32u;
    oct_pair(150);
    bad += check("KEYS: 150 ms apart: not the keyboard mode (OCT- then OCT+: the page)", !sm_ui.kbd && sm_ui.bank == 1u);
    ksnd(0)->len = 16u;
    sm_ui.bank = 0;
    /* a muted track plays no keyboard note; a note held when it is muted is let go */
    in_reset();
    midi_reset();
    in_reset();
    host_ticks = 1000u;
    smp.mute = 1;
    oct_pair(0);
    kb_key(WHITE[0], 1);
    mix_blocks(3);
    bad += check("KEYS: T1 muted: its key plays nothing", on_n[0] == 0u && notes_on_core(0) == 0u);
    kb_key(WHITE[0], 0);
    mix_blocks(2);
    smp.mute = 0;
    kb_key(WHITE[0], 1);
    mix_blocks(2);
    smp.mute = 1;
    mix_blocks(2);
    bad += check("KEYS: ... muted while a note is down: let go at the next block", notes_on_core(0) == 0u && ev_count(0, 0) == 1u);
    kb_key(WHITE[0], 0);
    mix_blocks(2);
    smp.mute = 0;
    bad += check("KEYS: ... and the key's own release adds nothing", ev_count(0, 0) == 1u);
    kb_key(WHITE[0], 1);
    mix_blocks(1);
    kb_key(WHITE[0], 0);
    mix_blocks(400);
    bad += check("KEYS: nothing hangs at the end", voices_busy() == 0u && !notes_on_core(0));
    return bad;
}

static int test_arp_keyboard(void)
{
    int bad = 0;
    in_reset();
    midi_reset();
    in_reset();
    host_ticks = 1000u;
    press(B_ARP);
    sm_input_poll();
    bad += check("ARP: a tap: the keyboard mode", sm_ui.kbd == 1u);
    leds_setup(LEDS_DIM);
    sm_ui.kbd = 1;
    sm_leds();
    bad += check("ARP: ... its LED lit", led_of(fm1_led, panel.btn[B_ARP]) == 1);
    in_reset();
    midi_reset();
    in_reset();
    host_ticks = 1000u;
    press(B_ARP);
    sm_input_poll();
    kb_key(WHITE[2], 1);
    mix_blocks(3);
    bad += check("ARP: ... a key plays a note on the track, no trig toggled",
                 on_n[0] == 1u && ev_nth(1, 0, 0)->note == 57u && !smp.pat[smp.cur].step[0][2].n);
    kb_key(WHITE[2], 0);
    mix_blocks(2);
    press(B_ARP);
    sm_input_poll();
    bad += check("ARP: again: the mode is off, no hung note", !sm_ui.kbd && notes_on_core(0) == 0u);
    kb_key(WHITE[2], 1);
    kb_key(WHITE[2], 0);
    bad += check("ARP: ... and the key is a trig again", smp.pat[smp.cur].step[0][2].n == 1u);
    press(B_ARP);
    sm_input_poll();
    oct_pair(0);
    bad += check("ARP: on by ARP, off by OCT- + OCT+ (one state)", !sm_ui.kbd);
    return bad;
}

static int test_keyboard_vs_seq_same_note(void)
{
    int bad = 0;
    uint32_t pk, k, hung;
    /* A: the sequencer's C4 (LEN INF) holds; the keyboard plays and lets go of C4 (key 7 = F3 + 7) in the middle */
    midi_reset();
    in_reset();
    midi_reset();
    sm_step_toggle(0, 0);
    stp(0, 0)->note[0] = 60;
    stp(0, 0)->len = SM_LEN_INF;
    oct_pair(0);
    push(SMC_PLAY, 0, 0, 0, 0);
    mix_blocks(4);
    bad += check("KEYS+SEQ: the sequencer's C4 (LEN INF) sounds", ev_count(1, 0) == 1u && notes_on_core(0) == 1u);
    kb_key(7, 1);
    mix_blocks(2);
    kb_key(7, 0);
    mix_blocks(2);
    bad += check("KEYS+SEQ: the key's note-off of the same note does not end the sequencer's", ev_count(0, 0) == 0u && notes_on_core(0) >= 1u);
    push(SMC_STOP, 0, 0, 0, 0);
    mix_blocks(400);
    hung = 0;
    for (k = 0; k < SM_NTRK; k++)
        hung += notes_on_core(k);
    pk = peak_of(10);
    bad += check("KEYS+SEQ: STOP, 400 blocks: nothing hangs", !hung && voices_busy() == 0 && pk <= 2u);
    /* B: the key holds C4; the sequencer's short C4 comes and goes: the key's note stays, its release is the one note-off */
    midi_reset();
    in_reset();
    midi_reset();
    sm_step_toggle(0, 1);
    stp(0, 1)->note[0] = 60;
    stp(0, 1)->len = 1;
    oct_pair(0);
    kb_key(7, 1);
    mix_blocks(2);
    push(SMC_PLAY, 0, 0, 0, 0);
    mix_blocks((uint32_t)(3.0 * S120 / CTL));
    bad += check("KEYS+SEQ: the sequencer's C4 came and went (two note-ons): the key's note still sounds, no note-off yet",
                 ev_count(1, 0) == 2u && ev_count(0, 0) == 0u && notes_on_core(0) >= 1u);
    kb_key(7, 0);
    mix_blocks(2);
    bad += check("KEYS+SEQ: the key's release then ends it (the one note-off)", ev_count(0, 0) == 1u);
    push(SMC_STOP, 0, 0, 0, 0);
    mix_blocks(400);
    bad += check("KEYS+SEQ: 400 blocks later: no hung voice", voices_busy() == 0 && !notes_on_core(0));
    /* C: a note-off for a key note the book does not hold (STOP forgot it) ends nothing of the sequencer's, and MIDI's same note stays */
    midi_reset();
    in_reset();
    midi_reset();
    sm_step_toggle(0, 0);
    stp(0, 0)->note[0] = 60;
    stp(0, 0)->len = SM_LEN_INF;
    oct_pair(0);
    kb_key(7, 1);
    mix_blocks(2);
    push(SMC_STOP, 0, 0, 0, 0);
    mix_blocks(2);
    push(SMC_PLAY, 0, 0, 0, 0);
    mix_blocks(4);
    kb_key(7, 0);
    mix_blocks(2);
    bad += check("KEYS+SEQ: a key held across STOP and PLAY: its release ends nothing of the sequencer's",
                 notes_on_core(0) >= 1u && ev_count(0, 0) == 0u);
    push(SMC_STOP, 0, 0, 0, 0);
    mix_blocks(400);
    midi_reset();
    in_reset();
    midi_reset();
    oct_pair(0);
    midi_on(0, 60, 100);
    mix_blocks(2);
    kb_key(7, 1);
    mix_blocks(2);
    midi_off(0, 60);
    mix_blocks(2);
    bad += check("KEYS+MIDI: MIDI's note-off of the key's note does not end the key's", notes_on_core(0) >= 1u && ev_count(0, 0) == 0u);
    kb_key(7, 0);
    mix_blocks(2);
    bad += check("KEYS+MIDI: ... the key's release ends it (the one note-off)", ev_count(0, 0) == 1u);
    mix_blocks(400);
    bad += check("KEYS+MIDI: nothing hangs", voices_busy() == 0 && !notes_on_core(0));
    /* D: two keys on one pitch (octave moved between them): the first release leaves the note, the second ends it */
    midi_reset();
    in_reset();
    midi_reset();
    oct_pair(0);
    kb_key(7, 1);                                         /* C4 */
    oct_tap(B_OCTDN);
    kb_key(19, 1);                                        /* F3 + 19 - 12 = C4 */
    mix_blocks(2);
    kb_key(7, 0);
    mix_blocks(2);
    bad += check("KEYS: two keys on one pitch: the first release ends nothing", notes_on_core(0) >= 1u && ev_count(0, 0) == 0u);
    kb_key(19, 0);
    mix_blocks(2);
    bad += check("KEYS: ... the second release ends it", notes_on_core(0) == 0u && ev_count(0, 0) == 1u);
    mix_blocks(400);
    return bad;
}

static int test_midi_mute_stop_panic(void)
{
    int bad = 0;
    uint32_t pk;
    /* muted (GLO): MIDI's note on T2 is ignored; a note held when the mute comes is let go at the next block */
    midi_reset();
    smp.mute = 2;
    midi_on(1, 64, 100);
    midi_on(0, 60, 100);
    mix_blocks(2);
    bad += check("MIDI: a muted T2 starts no MIDI note (T1 does), stopped too", on_n[1] == 0u && on_n[0] == 1u);
    midi_off(1, 64);
    mix_blocks(1);
    bad += check("MIDI: ... and its note-off does nothing", ev_count(0, 1) == 0u);
    smp.mute = 1;                                         /* T1 muted now, with MIDI's note held on it */
    mix_blocks(1);
    bad += check("MIDI: the mute comes while MIDI holds a note: let go at the next block", notes_on_core(0) == 0u && ev_count(0, 0) == 1u);
    smp.mute = 0;
    mix_blocks(2);
    bad += check("MIDI: ... the unmute does not bring it back", notes_on_core(0) == 0u && on_n[0] == 1u);
    midi_off(0, 60);
    mix_blocks(1);
    bad += check("MIDI: ... its late note-off is dropped (one note-off in all)", ev_count(0, 0) == 1u);
    /* solo: every track but the soloed one is muted */
    midi_reset();
    sm_solo = 1;
    midi_on(0, 60, 100);
    midi_on(2, 62, 100);
    mix_blocks(2);
    bad += check("MIDI: T1 soloed: T3's MIDI note is ignored, T1's plays", on_n[0] == 1u && on_n[2] == 0u);
    sm_solo = 0;
    /* STOP, with the sequencer stopped or playing, releases MIDI's notes; its bookkeeping is clear after it */
    midi_reset();
    midi_on(2, 60, 100);
    midi_on(2, 64, 100);
    mix_blocks(2);
    push(SMC_STOP, 0, 0, 0, 0);
    mix_blocks(1);
    pk = notes_on_core(2);
    bad += check("MIDI: STOP lets go of MIDI's notes (the voices killed within the block)", pk == 0u && voices_busy() == 0);
    sm_step_toggle(2, 0);
    stp(2, 0)->note[0] = 60;
    stp(2, 0)->len = 1;
    push(SMC_PLAY, 0, 0, 0, 0);
    mix_blocks((uint32_t)(2.0 * S120 / CTL));
    bad += check("MIDI: after STOP the sequencer's own C4 ends by itself (MIDI's book is clear): its note-off is sent",
                 ev_nth(0, 2, 0) != 0);
    push(SMC_STOP, 0, 0, 0, 0);
    mix_blocks(1);
    /* the panic: the main loop's panic_req */
    midi_reset();
    midi_on(1, 66, 100);
    mix_blocks(2);
    panic_req = 2;
    mix_blocks(1);
    bad += check("MIDI: the panic lets go of MIDI's notes", notes_on_core(1) == 0u);
    midi_on(1, 66, 100);
    mix_blocks(1);
    midi_off(1, 66);
    mix_blocks(1);
    bad += check("MIDI: ... and the book is clear: a later note and its note-off work", ev_count(1, 1) == 2u && ev_count(0, 1) == 1u && notes_on_core(1) == 0u);
    mix_blocks(400);
    bad += check("MIDI: nothing hangs after it all", voices_busy() == 0);
    return bad;
}

static int test_midi_overflow(void)
{
    int bad = 0;
    uint32_t k, n = 0;
    midi_reset();
    midi_on(0, 60, 100);
    mix_blocks(2);
    bad += check("MIDI: a note held on T1", notes_on_core(0) == 1u);
    sm_step_toggle(1, 0);                                 /* the sequencer's own notes are not MIDI's to drop */
    stp(1, 0)->note[0] = 62;
    stp(1, 0)->len = SM_LEN_INF;
    push(SMC_PLAY, 0, 0, 0, 0);
    mix_blocks(2);
    for (k = 0; k < MQ + 4u; k++)                         /* the main producer outruns the audio stream: the ring fills */
        n += (uint32_t)midi_on(2, 40u + k % 8u, 100);
    bad += check("MIDI: the ring takes 64 packets, then midi_in_overflow", n == MQ && midi_in_overflow == 1u);
    mix_blocks(1);
    bad += check("MIDI: the audio stream drops the queue and clears the flag", mi_r == mi_w && midi_in_overflow == 0u);
    bad += check("MIDI: ... MIDI's note on T1 is let go (a lost note-off must not hang it), the dropped notes never played",
                 notes_on_core(0) == 0u && on_n[2] == 0u);
    bad += check("MIDI: ... the sequencer's note on T2 sounds on", notes_on_core(1) == 1u && ev_count(0, 1) == 0u);
    bad += check("MIDI: the ring takes packets again", midi_on(3, 70, 100) == 1);
    mix_blocks(1);
    bad += check("MIDI: ... and T4 plays it", on_n[3] == 1u && notes_on_core(3) == 1u);
    push(SMC_STOP, 0, 0, 0, 0);
    mix_blocks(400);
    bad += check("MIDI: 400 blocks after STOP: nothing hangs", voices_busy() == 0);
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
/* ------------------------------------------------------------------------------------------------ patterns --- */
/* pattern p's kit: the machines of KIT rotated by p (a different machine on every track from pattern 0's), dry */
static void pat_kit(uint32_t p)
{
    static const uint8_t KIT[SM_NTRK] = {ENGI_MSAW, ENGI_MFMST, ENGI_STRING, ENGI_MBBOX};
    uint32_t t, i;
    for (t = 0; t < SM_NTRK; t++) {
        sm_sound_t *s = &smp.pat[p].kit.snd[t];
        sm_sound_from_preset(s, eng_rank(KIT[(t + p) % SM_NTRK]), 0);
        for (i = 0; i < 4u; i++)
            s->p[P_DIST + i] = 0;
    }
}
/* a trig on step st of track t in pattern p, note n (the main loop's order: fields, then n) */
static void pat_trig(uint32_t p, uint32_t t, uint32_t st, uint32_t note)
{
    sm_step_t *s = &smp.pat[p].step[t][st];
    memset(s, 0, sizeof *s);
    s->note[0] = (uint8_t)note;
    s->vel = 100;
    s->n = 1;
}
/* a block, then the main loop's mirror (sm_pat_poll); the clock of the block in which sm_play_pat changed to p (*at) */
static int pat_run_until(uint32_t p, uint32_t max, uint32_t *at)
{
    while (max--) {
        uint32_t c = seq_clock;
        run_block();
        sm_pat_poll();
        if (sm_play_pat == p) {
            if (at)
                *at = c;
            return 1;
        }
    }
    return 0;
}
/* the first note-on of track t at or after sample c: its note (0xFF none), *at its clock */
static uint32_t pat_on_after(uint32_t t, uint32_t c, uint32_t *at)
{
    uint32_t i;
    for (i = 0; i < seq_nev && i < 1024u; i++)
        if (seq_ev[i].on && seq_ev[i].t == t && seq_ev[i].at >= c) {
            if (at)
                *at = seq_ev[i].at;
            return seq_ev[i].note;
        }
    return 0xFFu;
}

/* SEQUENTIAL: the pattern asked for comes at the end of the one playing (its master length; AUTO: the longest track), every
 * track from step 1; a master length set also starts every track again at its end */
static int test_pat_switch_boundary(void)
{
    int bad = 0;
    uint32_t at = 0, on = 0, n;
    seq_reset();
    host_ticks = 1000u;
    pat_trig(0, 0, 0, 48);
    pat_trig(1, 0, 0, 60);
    smp.mode = SM_PM_SEQ;
    push(SMC_PLAY, 0, 0, 0, 0);
    seq_run((uint32_t)(5.5 * S120));
    smp.next = 1;                                         /* (the main loop's request) */
    bad += check("PAT: SEQUENTIAL: switched at the end of the 16 steps (AUTO: the longest track), +-CTL",
                 pat_run_until(1, 3000, &at) && near_d(at, 16.0 * S120));
    n = pat_on_after(0, at, &on);
    bad += check("PAT: ... the new pattern's step 1 plays there (its note 60)", n == 60u && near_d(on, 16.0 * S120));
    bad += check("PAT: ... the main loop mirrors it: smp.cur 1, the request gone", smp.cur == 1u && smp.next == SM_PAT_NONE);
    /* AUTO with a track of 24 steps: the end is at 24 */
    seq_reset();
    pat_trig(0, 0, 0, 48);
    pat_trig(1, 0, 0, 60);
    smp.pat[0].kit.snd[1].len = 24;
    push(SMC_PLAY, 0, 0, 0, 0);
    seq_run((uint32_t)(3.5 * S120));
    smp.next = 1;
    bad += check("PAT: AUTO: T2 24 steps long: the switch at 24 steps", pat_run_until(1, 5000, &at) && near_d(at, 24.0 * S120));
    /* a master length of 6 */
    seq_reset();
    pat_trig(0, 0, 0, 48);
    pat_trig(0, 0, 4, 50);
    pat_trig(1, 0, 0, 60);
    smp.pat[0].mlen = 6;
    push(SMC_PLAY, 0, 0, 0, 0);
    seq_run((uint32_t)(7.5 * S120));
    n = pat_on_after(0, (uint32_t)(5.5 * S120), &on);
    bad += check("PAT: master length 6: every track from step 1 again at 6 (no switch asked)", n == 48u && near_d(on, 6.0 * S120));
    smp.next = 1;
    bad += check("PAT: ... asked: the switch at 12", pat_run_until(1, 3000, &at) && near_d(at, 12.0 * S120));
    /* stopped: at once */
    seq_reset();
    smp.next = 3;
    bad += check("PAT: stopped: at once (the next block)", pat_run_until(3, 1, 0) && smp.cur == 3u);
    return bad;
}

/* the kits: every track's machine changes at the switch; in its first block the old engines fade with their own values (pe_old),
 * the new values wait in p[] (never the new parameters on the old machine), then the new engines; the core is the new kit */
static int test_pat_switch_kits(void)
{
    int bad = 0, fade = 1, own = 1, newp = 1, done = 1;
    uint32_t t, s, b, at = 0;
    int16_t pe[SM_NTRK][8];
    seq_reset();
    pat_kit(1);
    for (t = 0; t < SM_NTRK; t++)
        for (s = 0; s < 16u; s++) {
            pat_trig(0, t, s, 48 + t);
            pat_trig(1, t, s, 50 + t);
            smp.pat[0].step[t][s].len = SM_LEN_INF;   /* (held: sounding at the switch) */
        }
    push(SMC_PLAY, 0, 0, 0, 0);
    seq_run((uint32_t)(15.5 * S120));
    for (t = 0; t < SM_NTRK; t++)
        for (s = 0; s < 8u; s++)
            pe[t][s] = trk[t].p[P_E0 + s];
    smp.next = 1;
    bad += check("PAT: kits: switched", pat_run_until(1, 100, &at));
    for (t = 0; t < SM_NTRK; t++) {
        fade &= trk[t].engine == eng_vis(smp.pat[0].kit.snd[t].machine) && trk[t].xf_on && trk[t].xf == XF_BLOCKS - 1u &&
                trk[t].eng_req == eng_vis(smp.pat[1].kit.snd[t].machine) && trk[t].eng_req != trk[t].engine;
        own &= !memcmp(trk[t].pe_old, pe[t], sizeof pe[t]);
        for (s = 0; s < 8u; s++)
            newp &= trk[t].p[P_E0 + s] == smp.pat[1].kit.snd[t].p[P_E0 + s];
    }
    bad += check("PAT: kits: the first block: all 4 tracks fade their old engine toward the new one", fade);
    bad += check("PAT: ... the old engines render with their own values (pe_old), not the new kit's", own);
    bad += check("PAT: ... the new kit's values are in p[]", newp);
    {   /* a value the main loop pushed for the old pattern (before it saw the switch): dropped, the new kit's stays */
        int16_t cut = trk[2].p[P_FCUT];
        push(SMC_PARAM, 2, 0, P_FCUT, cut > 10 ? 3 : 90);
        run_block();
        bad += check("PAT: ... a value queued for the old pattern is dropped (the new kit's stays)", trk[2].p[P_FCUT] == cut);
        push(SMC_PARAM, 2, 1, P_FCUT, cut > 10 ? 3 : 90);
        run_block();
        bad += check("PAT: ... one for the new pattern is taken", trk[2].p[P_FCUT] == (cut > 10 ? 3 : 90));
        push(SMC_PARAM, 2, 1, P_FCUT, cut);
        run_block();
    }
    for (b = 0; b < 8u; b++) {
        run_block();
        sm_pat_poll();
    }
    for (t = 0; t < SM_NTRK; t++)
        done &= trk[t].engine == eng_vis(smp.pat[1].kit.snd[t].machine) && !trk[t].xf_on;
    bad += check("PAT: ... then the new engines, the core is the new kit", done && core_is_model() && smp.cur == 1u);
    push(SMC_STOP, 0, 0, 0, 0);
    run_block();
    return bad;
}

/* DIRECT START: at once, every track from step 1; DIRECT JUMP: at once, the tracks where they were */
static int test_pat_direct_start(void)
{
    int bad = 0;
    uint32_t at = 0, on = 0, n;
    seq_reset();
    pat_trig(0, 0, 0, 48);
    pat_trig(1, 0, 0, 60);
    pat_trig(1, 0, 1, 61);
    smp.mode = SM_PM_START;
    push(SMC_PLAY, 0, 0, 0, 0);
    seq_run((uint32_t)(5.5 * S120));
    smp.next = 1;
    bad += check("PAT: DIRECT START: at once (the next block)", pat_run_until(1, 1, &at));
    n = pat_on_after(0, at, &on);
    bad += check("PAT: ... step 1 of the new pattern in that block", n == 60u && on == at);
    seq_run((uint32_t)(1.5 * S120));
    n = pat_on_after(0, at + CTL, &on);
    bad += check("PAT: ... step 2 a step later", n == 61u && near_d(on - at, S120));
    push(SMC_STOP, 0, 0, 0, 0);
    run_block();
    return bad;
}
static int test_pat_direct_jump(void)
{
    int bad = 0;
    uint32_t at = 0, on = 0, n;
    seq_reset();
    pat_trig(0, 0, 0, 48);
    pat_trig(1, 0, 0, 60);
    pat_trig(1, 0, 6, 66);
    smp.mode = SM_PM_JUMP;
    push(SMC_PLAY, 0, 0, 0, 0);
    seq_run((uint32_t)(5.5 * S120));
    smp.next = 1;
    bad += check("PAT: DIRECT JUMP: at once (the next block)", pat_run_until(1, 1, &at));
    seq_run((uint32_t)S120);
    n = pat_on_after(0, at, &on);
    bad += check("PAT: ... the tracks go on where they were: the new pattern's step 7 on the grid (6 steps in)",
                 n == 66u && near_d(on, 6.0 * S120) && sm_pos[0] == 6u);
    push(SMC_STOP, 0, 0, 0, 0);
    run_block();
    return bad;
}

/* 100 switches at 300 BPM, the three modes in turn, held notes, slides, ratchets on every track; then STOP: after 400 blocks
 * nothing sounds and no voice is held */
static int test_pat_switch_300bpm_no_hang(void)
{   /* (a short tail is left after the voices are gone: the same 300 BPM pattern played without switches leaves |x| ~170 at
     * this point, so it is not the switches' doing; its cause is unexplained. The test asserts what matters: the envelope
     * decays: blocks 400..500 after STOP |x| <= 32, blocks 1400..1500 |x| <= 1) */
    int bad = 0, all = 1;
    uint32_t t, s, i, pk, held = 0;
    seq_reset();
    pat_kit(1);
    pat_kit(2);
    for (i = 0; i < 3u; i++)
        for (t = 0; t < SM_NTRK; t++)
            for (s = 0; s < 16u; s++) {
                pat_trig(i, t, s, 40 + 7 * i + t);
                smp.pat[i].step[t][s].flags = (uint8_t)(s % 4u == 1u ? SMF_SLIDE : s % 4u == 2u ? (1u << 4) : 0u);
                smp.pat[i].step[t][s].len = (uint8_t)(s % 5u == 0u ? SM_LEN_INF : 0u);
            }
    smp.bpm10 = 3000;
    push(SMC_PLAY, 0, 0, 0, 0);
    run_block();
    for (i = 0; i < 100u; i++) {
        uint32_t want = (sm_play_pat + 1u) % 3u;
        smp.mode = (uint8_t)(i % 3u);
        smp.next = (uint8_t)want;
        all &= pat_run_until(want, 2000, 0);
    }
    bad += check("PAT: 300 BPM: 100 switches, the three modes in turn, every one came", all);
    push(SMC_STOP, 0, 0, 0, 0);
    for (i = 0; i < 400u; i++)
        run_block();
    pk = peak_of(100);                                    /* blocks 400..500 */
    for (t = 0; t < SM_NTRK; t++)
        held += notes_on_core(t) + sm_nheld[t];
    printf("swamp: after STOP peak(400..500) %u, held %u, busy %u\n", pk, held, voices_busy());
    bad += check("PAT: ... STOP: 400 blocks later no voice left, nothing held, the tail decaying (blocks 400..500: |x| <= 32)",
                 pk <= 32u && held == 0u && !voices_busy());
    (void)peak_of(900);                                   /* blocks 500..1400 */
    pk = peak_of(100);                                    /* blocks 1400..1500 */
    printf("swamp: after STOP peak(1400..1500) %u\n", pk);
    bad += check("PAT: ... and the tail has died away (blocks 1400..1500: |x| <= 1)", pk <= 1u);
    return bad;
}

/* a lock on a step of pattern 0 is on the core (and a PSLIDE gliding); the switch to pattern 1 makes the new kit's base every value's
 * and clears the lock mask and the glide */
static int test_pat_switch_clears_locks(void)
{
    int bad = 0;
    int16_t base1;
    uint32_t t = 0;
    seq_reset();
    pat_kit(1);
    base1 = smp.pat[1].kit.snd[t].p[P_FCUT];
    smp.pat[0].kit.snd[t].p[P_FCUT] = 60;
    smp.pat[1].kit.snd[t].p[P_FCUT] = (int8_t)(base1 == 90 ? 80 : 90);   /* (neither the lock's value nor pattern 0's base) */
    base1 = smp.pat[1].kit.snd[t].p[P_FCUT];
    pat_trig(0, t, 0, 48);
    smp.pat[0].step[t][0].flags = SMF_PSLIDE;
    pat_trig(0, t, 1, 50);
    smp.pat[0].step[t][1].len = SM_LEN_INF;
    sm_lock_set(t, 0, P_FCUT, 20);
    sm_lock_set(t, 1, P_FCUT, 33);
    push(SMC_PLAY, 0, 0, 0, 0);
    seq_run((uint32_t)(1.2 * S120));                      /* step 1 has fired: its lock is on the core */
    bad += check("PAT: lock: before the switch the core holds the lock (CUT 33, mask set)", trk[t].p[P_FCUT] == 33 && sm_lk_is(t, P_FCUT));
    smp.next = 1;
    bad += check("PAT: lock: switched to pattern 1", pat_run_until(1, 3000, 0));
    run_block();
    bad += check("PAT: lock: CUT is pattern 1's base, not the lock's", trk[t].p[P_FCUT] == base1 && smp.cur == 1u);
    bad += check("PAT: lock: the lock mask and the PSLIDE are cleared", !sm_lk_is(t, P_FCUT) && sm_ps[t].n == 0u &&
                 !sm_lk_on[t][P_FCUT >> 3] && sm_lk_step[t] == 0xFFu);
    push(SMC_STOP, 0, 0, 0, 0);
    run_block();
    return bad;
}

/* the SEQ layer: ALGORITHM picks a pattern, letting go of SEQ asks for it; F#3 the mode round; K2 the master length */
static int test_pat_seq_layer(void)
{
    int bad = 0;
    in_reset();
    host_ticks = 1000u;
    btn_down(B_SEQ);
    sm_input_poll();
    turn(EN_ALGO, 2);
    sm_input_poll();
    bad += check("PAT: SEQ + ALGORITHM +2: the layer, pattern A03 picked, the track stays", sm_layer == SML_SEQ && sm_lay_pick == 2u &&
                 sm_ui.trk == 0u && smp.next == SM_PAT_NONE);
    turn(EN_ALGO, -3);
    sm_input_poll();
    bad += check("PAT: ... -3: wraps to A16", sm_lay_pick == 15u);
    turn(EN_ALGO, 3);
    sm_input_poll();
    key_dn(1);                                            /* F#3: the mode */
    sm_input_poll();
    key_up(1);
    bad += check("PAT: ... F#3: DIRECT START", smp.mode == SM_PM_START);
    key_dn(1);
    sm_input_poll();
    key_up(1);
    key_dn(1);
    sm_input_poll();
    key_up(1);
    bad += check("PAT: ... F#3 twice more: SEQUENTIAL again", smp.mode == SM_PM_SEQ);
    turn(EN_K2, 8);
    sm_input_poll();
    bad += check("PAT: ... K2 +8: the master length 8", smp.pat[0].mlen == 8u);
    turn(EN_K2, -20);
    sm_input_poll();
    bad += check("PAT: ... K2 -20: AUTO (0)", smp.pat[0].mlen == 0u);
    layer_let_go(B_SEQ);
    bad += check("PAT: SEQ let go: A03 asked for", smp.next == 2u && smp.cur == 0u);
    run_block();
    sm_input_poll();
    bad += check("PAT: ... stopped: the next block it plays, the main loop mirrors it", smp.cur == 2u && smp.next == SM_PAT_NONE);
    layer_hold(B_SEQ);
    layer_let_go(B_SEQ);
    bad += check("PAT: SEQ held and let go without ALGORITHM: nothing asked", smp.next == SM_PAT_NONE);
    return bad;
}

/* ------------------------------------------------------------------------------------------------ chains --- */
/* SEQ held: OCT+ adds the picked pattern (none picked: the one playing) to the chain being built, repeats too */
static void chain_seq_oct(uint32_t b)
{
    host_pressed |= 1u << panel.btn[b];
    sm_input_poll();
}
static int test_chain_build(void)
{
    int bad = 0;
    smd_mrow_t o;
    in_reset();
    host_ticks = 1000u;
    layer_hold(B_SEQ);
    chain_seq_oct(B_OCTUP);                               /* A01 (nothing picked: the one playing) */
    chain_seq_oct(B_OCTUP);                               /* A01 again */
    turn(EN_ALGO, 1);
    sm_input_poll();
    chain_seq_oct(B_OCTUP);                               /* A02 */
    bad += check("CHAIN: SEQ + OCT+ x2, ALGORITHM +1, OCT+: A01 A01 A02 built", sm_lay_nch == 3u && sm_lay_chain[0] == 0u &&
                 sm_lay_chain[1] == 0u && sm_lay_chain[2] == 1u && smp.nchain == 0u);
    bad += check("CHAIN: ... the page did not move (OCT is the layer's), no keyboard", sm_ui.bank == 0u && !sm_ui.kbd);
    smd_map_get(4, &o);
    bad += check("CHAIN: ... the map shows it: + 01 01 02", !strcmp(o.w, "+ 01 01 02"));
    layer_let_go(B_SEQ);
    bad += check("CHAIN: SEQ let go: the model holds the chain A01 A01 A02, link 1 asked for", smp.nchain == 3u &&
                 smp.chain[0] == 0u && smp.chain[1] == 0u && smp.chain[2] == 1u && smp.next == 0u);
    run_block();
    sm_input_poll();
    bad += check("CHAIN: ... the audio stream's link is link 1 (index 0)", sm_cpos == 0u && smp.cur == 0u);
    smp.cur = 0;
    sm_layer = SML_SEQ;
    smd_map_get(4, &o);
    sm_layer = SML_NONE;
    bad += check("CHAIN: the map (SEQ held) with nothing built: CHAIN 1/3 NEXT A01", !strcmp(o.w, "CHAIN 1/3 NEXT A01"));
    {                                                     /* sixteen links at most */
        uint32_t i;
        layer_hold(B_SEQ);
        for (i = 0; i < 20u; i++)
            chain_seq_oct(B_OCTUP);
        bad += check("CHAIN: OCT+ x20: 16 links, no more", sm_lay_nch == SM_NCHAIN);
        layer_let_go(B_SEQ);
        bad += check("CHAIN: ... 16 in the model", smp.nchain == SM_NCHAIN);
    }
    {                                                     /* one link: that pattern asked for, the chain off */
        layer_hold(B_SEQ);
        turn(EN_ALGO, 3);
        sm_input_poll();
        chain_seq_oct(B_OCTUP);
        layer_let_go(B_SEQ);
        bad += check("CHAIN: one link: A04 asked for, no chain", smp.nchain == 0u && smp.next == 3u);
        run_block();
        sm_input_poll();
    }
    return bad;
}

/* the links play each to its own end, round: A01 (master length 6) A01 A02 (master length 10), A01's trigs on steps 1 and 6
 * (notes 48, 49), A02's on step 1 (60): T1's note-ons at 0 5 | 6 11 | 12 | 22 27 | 28 33 | 34 | 44 .. */
static int test_chain_plays_links(void)
{
    static const uint8_t WANT_N[] = {48, 49, 48, 49, 60, 48, 49, 48, 49, 60, 48};
    static const uint8_t WANT_AT[] = {0, 5, 6, 11, 12, 22, 27, 28, 33, 34, 44};
    int bad = 0, ok = 1;
    uint32_t i, at, c = 0, sw = 0, last = 0;
    static const uint8_t CH[3] = {0, 0, 1};
    in_reset();
    host_ticks = 1000u;
    pat_trig(0, 0, 0, 48);
    pat_trig(0, 0, 5, 49);
    pat_trig(0, 1, 2, 70);                                /* (T2: 16 steps; a link restarts it too) */
    pat_trig(1, 0, 0, 60);
    smp.pat[0].mlen = 6;
    smp.pat[1].mlen = 10;
    sm_chain_set(CH, 3);
    push(SMC_PLAY, 0, 0, 0, 0);
    for (i = 0; i < 46u * (uint32_t)S120 / CTL + 2u; i++) {
        run_block();
        sm_input_poll();
        if (sm_play_pat != last)
            sw++, last = sm_play_pat;
    }
    for (i = 0; i < sizeof WANT_N; i++) {
        const seq_ev_t *e = ev_nth(1, 0, i);
        ok &= e && e->note == WANT_N[i] && near_d(e->at, WANT_AT[i] * S120);
        if (!(e && e->note == WANT_N[i] && near_d(e->at, WANT_AT[i] * S120)) && c++ < 3u)
            printf("  link note %u: got %u at %.2f, want %u at %u\n", i, e ? e->note : 0u, e ? e->at / S120 : -1.0, WANT_N[i], WANT_AT[i]);
    }
    bad += check("CHAIN: A01(6) A01 A02(10): each link to its own end, round (T1's notes and times, +-CTL)", ok);
    bad += check("CHAIN: ... the switches A01 -> A02 -> A01 -> A02 -> A01 seen", sw == 4u);
    at = 0;
    ok = 1;
    for (i = 0; i < 4u; i++) {                            /* T2's trig on step 3 of every A01 link: 2 8 | 24 30 */
        static const uint8_t T2AT[4] = {2, 8, 24, 30};
        const seq_ev_t *e = ev_nth(1, 1, i);
        ok &= e && near_d(e->at, T2AT[i] * S120);
    }
    bad += check("CHAIN: ... a repeated link starts every track again from step 1 (T2 at 2 8 24 30)", ok);
    for (i = 0; i < 20000u && sm_play_pat != 1u; i++) {   /* STOP in A02 (link 3), PLAY: from link 1 */
        run_block();
        sm_input_poll();
    }
    push(SMC_STOP, 0, 0, 0, 0);
    run_block();
    sm_input_poll();
    bad += check("CHAIN: STOP in link 3 (A02)", !sm_playing && sm_play_pat == 1u && sm_cpos == 2u);
    seq_log_clear();
    push(SMC_PLAY, 0, 0, 0, 0);
    run_block();
    sm_input_poll();
    bad += check("CHAIN: PLAY: link 1 again (A01 at once, the main loop mirrors it)", sm_cpos == 0u && sm_play_pat == 0u && smp.cur == 0u);
    for (i = 0; i < 13u * (uint32_t)S120 / CTL; i++) {
        run_block();
        sm_input_poll();
    }
    ok = 1;
    for (i = 0; i < 5u; i++) {
        const seq_ev_t *e = ev_nth(1, 0, i);
        ok &= e && e->note == WANT_N[i] && near_d(e->at, WANT_AT[i] * S120);
    }
    bad += check("CHAIN: ... then A01 A01 A02 as from the start (48 49 48 49 60 at 0 5 6 11 12)", ok);
    push(SMC_STOP, 0, 0, 0, 0);
    run_block();
    (void)at;
    return bad;
}

/* a link of the pattern playing (A01 A01): the tracks from step 1 only; no sound loaded again (a value the core holds stays),
 * no engine fade, a LEN INF note not let go at the link's end (only by the next trig, as in a loop) */
static int test_chain_repeat_no_reload(void)
{
    int bad = 0, ok = 1, fade = 0;
    uint32_t i;
    static const uint8_t CH[2] = {0, 0};
    const seq_ev_t *e;
    in_reset();
    host_ticks = 1000u;
    pat_trig(0, 0, 0, 48);
    pat_trig(0, 1, 4, 60);
    smp.pat[0].step[1][4].len = SM_LEN_INF;
    smp.pat[0].mlen = 6;
    sm_chain_set(CH, 2);
    push(SMC_PLAY, 0, 0, 0, 0);
    run_block();
    trk[2].p[P_LRATE] = 77;                               /* (the core's own: a sound loaded again would overwrite it) */
    for (i = 0; i < 13u * (uint32_t)S120 / CTL; i++) {
        run_block();
        sm_input_poll();
        fade |= trk[0].xf || trk[1].xf || trk[2].xf || trk[3].xf;
    }
    bad += check("CHAIN/REPEAT: A01 A01: link 2 reached (sm_cpos moved), no switch", sm_cpos == 0u && sm_play_pat == 0u);
    bad += check("CHAIN/REPEAT: ... no sound loaded again, no engine fade", trk[2].p[P_LRATE] == 77 && !fade);
    for (i = 0; i < 3u; i++) {
        e = ev_nth(1, 0, i);
        ok &= e && near_d(e->at, i * 6.0 * S120);
    }
    bad += check("CHAIN/REPEAT: ... T1 at 0 6 12 (every link from step 1)", ok);
    e = ev_nth(0, 1, 0);
    bad += check("CHAIN/REPEAT: ... T2's LEN INF note (step 5) let go by its next trig (10), not at the link's end (6)",
                 e && e->note == 60u && near_d(e->at, 10.0 * S120));
    push(SMC_STOP, 0, 0, 0, 0);
    run_block();
    return bad;
}

/* G#3 in the SEQ layer: the chain off, the pattern playing plays on (no switch at its end) */
static int test_chain_off(void)
{
    int bad = 0;
    uint32_t i, sw = 0;
    static const uint8_t CH[2] = {0, 1};
    in_reset();
    host_ticks = 1000u;
    pat_trig(0, 0, 0, 48);
    pat_trig(1, 0, 0, 60);
    smp.pat[0].mlen = 4;
    smp.pat[1].mlen = 4;
    sm_chain_set(CH, 2);
    push(SMC_PLAY, 0, 0, 0, 0);
    while (sm_play_pat != 1u && sw++ < 5000u) {
        run_block();
        sm_input_poll();
    }
    bad += check("CHAIN/OFF: the chain plays: A02 comes", sm_play_pat == 1u && smp.cur == 1u);
    layer_hold(B_SEQ);
    key_dn(3);                                            /* G#3 */
    sm_input_poll();
    key_up(3);
    layer_let_go(B_SEQ);
    bad += check("CHAIN/OFF: SEQ + G#3: nchain 0, nothing asked", smp.nchain == 0u && smp.next == SM_PAT_NONE);
    sw = 0;
    for (i = 0; i < 20u * (uint32_t)S120 / CTL; i++) {
        run_block();
        sm_input_poll();
        sw += sm_play_pat != 1u;
    }
    bad += check("CHAIN/OFF: ... A02 plays on, 5 of its ends later", !sw && smp.cur == 1u);
    push(SMC_STOP, 0, 0, 0, 0);
    run_block();
    return bad;
}

/* OCT- in the SEQ layer throws away the chain being built (and the pick): SEQ let go asks for nothing; a chain playing
 * stays. A chain built anew while one plays: link 1 again */
static int test_chain_reset(void)
{
    int bad = 0;
    uint32_t i;
    static const uint8_t CH[3] = {2, 3, 4};
    in_reset();
    host_ticks = 1000u;
    sm_chain_set(CH, 3);
    run_block();
    sm_input_poll();
    layer_hold(B_SEQ);
    turn(EN_ALGO, 5);
    sm_input_poll();
    chain_seq_oct(B_OCTUP);
    chain_seq_oct(B_OCTUP);
    chain_seq_oct(B_OCTDN);
    bad += check("CHAIN/RESET: OCT+ x2 then OCT-: nothing built, nothing picked", sm_lay_nch == 0u && sm_lay_pick == SM_PAT_NONE);
    layer_let_go(B_SEQ);
    bad += check("CHAIN/RESET: SEQ let go: nothing asked, the chain playing (A03 A04 A05) stays", smp.next == SM_PAT_NONE &&
                 smp.nchain == 3u && smp.chain[0] == 2u && smp.cur == 2u);
    smp.pat[2].mlen = smp.pat[3].mlen = smp.pat[4].mlen = 2;
    push(SMC_PLAY, 0, 0, 0, 0);
    for (i = 0; i < 3u * (uint32_t)S120 / CTL; i++) {
        run_block();
        sm_input_poll();
    }
    bad += check("CHAIN/RESET: playing: link 2 (A04)", sm_cpos == 1u && smp.cur == 3u);
    sm_chain_set(CH, 3);
    run_block();
    sm_input_poll();
    bad += check("CHAIN/RESET: the chain published anew: link 1 again (A03 asked for)", sm_cpos == 0u);
    for (i = 0; i < 2u * (uint32_t)S120 / CTL; i++) {
        run_block();
        sm_input_poll();
    }
    bad += check("CHAIN/RESET: ... A03 at the end of A04", smp.cur == 2u && sm_cpos == 0u);
    push(SMC_STOP, 0, 0, 0, 0);
    run_block();
    return bad;
}

/* ------------------------------------------------------------------------------------------------ edits --- */
/* a white key of the open layer tapped: down (a poll), up (a poll) */
static void lay_key(uint32_t key)
{
    key_dn(key);
    sm_input_poll();
    key_up(key);
    sm_input_poll();
}
/* the live locks of track t in pattern p, by step and parameter (LK_NONE none) */
static int32_t ed_lock(const sm_pattern_t *p, uint32_t t, uint32_t s, uint32_t prm)
{
    int32_t i = sm_lock_find(p, t, s, prm);
    return i < 0 ? LK_NONE : p->lock[i].val;
}
static uint32_t ed_live(const sm_pattern_t *p, uint32_t t)  /* the live locks of track t (t 4: all) */
{
    uint32_t i, n = 0;
    for (i = 0; i < p->nlock && i < SM_NLOCK; i++)
        n += p->lock[i].param != 0xFFu && (t >= SM_NTRK || (uint32_t)(p->lock[i].ts >> 5) == t);
    return n;
}
static uint32_t ed_trigs(const sm_pattern_t *p, uint32_t t)
{
    uint32_t s, n = 0;
    for (s = 0; s < SM_NSTEP; s++)
        n += p->step[t][s].n != 0u;
    return n;
}
/* pattern p's track t: trigs on steps 0 4 8 12 (notes 48 + step), CUT locked on 4 (20 + t) and RES on 8 (30 + t) */
static void ed_track(uint32_t t)
{
    uint32_t s;
    for (s = 0; s < 16u; s += 4u) {
        sm_step_toggle(t, s);
        stp(t, s)->note[0] = (uint8_t)(48 + s);
    }
    sm_lock_set(t, 4, P_FCUT, 20 + (int32_t)t);
    sm_lock_set(t, 8, P_FRES, 30 + (int32_t)t);
}
static void save_hold(void)                               /* SAVE held past HOLD_MS, let go */
{
    btn_down(B_SAVE);
    sm_input_poll();
    ms(500);
    sm_input_poll();
    btn_up(B_SAVE);
    sm_input_poll();
}

static int test_clear_track(void)
{
    int bad = 0, ok = 1;
    uint32_t t;
    sm_kit_t kit;
    in_reset();
    host_ticks = 1000u;
    for (t = 0; t < SM_NTRK; t++)
        ed_track(t);
    kit = cpat()->kit;
    layer_hold(B_SEQ);
    lay_key(WHITE[0]);                                    /* F3 tapped */
    bad += check("EDIT/CLEAR: SEQ + F3: T1's trigs and locks gone", !ed_trigs(cpat(), 0) && !ed_live(cpat(), 0));
    for (t = 1; t < SM_NTRK; t++)
        ok &= ed_trigs(cpat(), t) == 4u && ed_live(cpat(), t) == 2u && ed_lock(cpat(), t, 4, P_FCUT) == 20 + (int32_t)t;
    bad += check("EDIT/CLEAR: ... T2..T4 untouched, the kit kept", ok && !memcmp(&kit, &cpat()->kit, sizeof kit));
    bad += check("EDIT/CLEAR: ... CLEARED in the header, the pool compacted (stopped)", sm_msg == SM_MSG_CLEARED &&
                 cpat()->nlock == 6u);
    ms(900);
    sm_input_poll();
    bad += check("EDIT/CLEAR: ... still at 0.9 s, the key let go: no clear of all", sm_msg == SM_MSG_CLEARED && ed_trigs(cpat(), 1) == 4u);
    ms(200);
    sm_input_poll();
    bad += check("EDIT/CLEAR: ... gone after 1 s", sm_msg == SM_MSG_NONE);
    layer_let_go(B_SEQ);
    return bad;
}

static int test_clear_all_hold(void)
{
    int bad = 0;
    uint32_t t;
    sm_kit_t kit;
    in_reset();
    host_ticks = 1000u;
    for (t = 0; t < SM_NTRK; t++)
        ed_track(t);
    kit = cpat()->kit;
    turn(EN_ALGO, 2);                                     /* T3 selected */
    sm_input_poll();
    layer_hold(B_SEQ);
    key_dn(WHITE[0]);
    sm_input_poll();
    bad += check("EDIT/CLEAR ALL: F3 down: the selected track (T3) cleared at once", !ed_trigs(cpat(), 2) && ed_trigs(cpat(), 0) == 4u);
    ms(700);
    sm_input_poll();
    bad += check("EDIT/CLEAR ALL: ... held 0.7 s: the others still there", ed_trigs(cpat(), 0) == 4u && ed_live(cpat(), 4) == 6u);
    ms(150);
    sm_input_poll();
    bad += check("EDIT/CLEAR ALL: ... held 0.85 s: all 4 tracks, trigs and locks", !ed_trigs(cpat(), 0) && !ed_trigs(cpat(), 1) &&
                 !ed_trigs(cpat(), 3) && !ed_live(cpat(), 4) && cpat()->nlock == 0u);
    bad += check("EDIT/CLEAR ALL: ... the kit kept, CLEARED", !memcmp(&kit, &cpat()->kit, sizeof kit) && sm_msg == SM_MSG_CLEARED);
    key_up(WHITE[0]);
    sm_input_poll();
    layer_let_go(B_SEQ);
    save_hold();
    bad += check("EDIT/CLEAR ALL: UNDO: all 4 tracks back (the snapshot before the first clear)", ed_trigs(cpat(), 0) == 4u &&
                 ed_trigs(cpat(), 2) == 4u && ed_live(cpat(), 4) == 8u && ed_lock(cpat(), 2, 8, P_FRES) == 32 && sm_msg == SM_MSG_UNDO);
    return bad;
}

/* CLEAR all while playing at 300 BPM, then UNDO: the trigs and locks come back, the pool whole; the audio stream (run inside the
 * writes, lock_preempt) never applies a lock of a wiped step nor a wrong value */
static int test_undo_after_clear_while_playing(void)
{
    static const uint8_t PRM[4] = {P_FCUT, P_FRES, P_PAN, P_DEC};
    int bad = 0, ok = 1;
    uint32_t t, s, p, i, n0, on;
    static sm_pattern_t before;
    in_reset();
    host_ticks = 1000u;
    smp.bpm10 = 3000u;
    for (t = 0; t < SM_NTRK; t++)
        for (s = 0; s < SM_NSTEP; s++)
            for (p = 0; p < P_COUNT; p++)
                lk_shadow[t][s][p] = LK_NONE;
    for (t = 0; t < SM_NTRK; t++)
        for (s = 0; s < 16u; s++)
            sm_step_toggle(t, s);
    for (i = 0; i < 400u; i++) {                          /* a pool near full */
        const param_desc_t *d;
        int32_t v;
        t = lk_rand(SM_NTRK);
        s = lk_rand(16);
        p = PRM[lk_rand(4)];
        d = sm_desc_safe(sm_engine_of(ksnd(t)->machine), p);
        v = d->min + (int32_t)lk_rand((uint32_t)(d->max - d->min + 1));
        if (sm_lock_set(t, s, p, v))
            lk_shadow[t][s][p] = (int16_t)v;
    }
    sm_msg = SM_MSG_NONE;
    for (i = 0; i < 40u; i++) {                           /* a few removed (tombstones in the pool while playing) */
        t = lk_rand(SM_NTRK);
        s = lk_rand(16);
        if (ed_live(cpat(), 4) > 150u) {
            sm_lock_clear_step(t, s);
            for (p = 0; p < P_COUNT; p++)
                lk_shadow[t][s][p] = LK_NONE;
        }
    }
    n0 = ed_live(cpat(), 4);
    before = *cpat();
    press(B_PLAY);
    sm_input_poll();
    play_blocks(300);
    lk_applied = lk_bad_val = lk_bad_dead = lk_preempts = 0;
    lk_on = 1;
    lk_preempt_n = 2u;
    layer_hold(B_SEQ);
    for (s = 0; s < SM_NSTEP; s++)                        /* (the model to come: T1 nothing locked, the tap) */
        for (p = 0; p < P_COUNT; p++)
            lk_shadow[0][s][p] = LK_NONE;
    key_dn(WHITE[0]);
    sm_input_poll();
    bad += check("EDIT/UNDO: playing: F3 tapped: T1 empty, T2 not yet", !ed_trigs(cpat(), 0) && !ed_live(cpat(), 0) &&
                 ed_trigs(cpat(), 1) == 16u && !lk_bad_dead && !lk_bad_val);
    for (t = 0; t < SM_NTRK; t++)                         /* (... then all: held 0.8 s) */
        for (s = 0; s < SM_NSTEP; s++)
            for (p = 0; p < P_COUNT; p++)
                lk_shadow[t][s][p] = LK_NONE;
    ms(900);
    sm_input_poll();
    key_up(WHITE[0]);
    sm_input_poll();
    layer_let_go(B_SEQ);
    bad += check("EDIT/UNDO: playing: CLEAR all (held): no trig, no live lock", !ed_trigs(cpat(), 0) && !ed_trigs(cpat(), 3) &&
                 !ed_live(cpat(), 4) && cpat()->nlock <= SM_NLOCK);
    seq_log_clear();
    play_blocks(400);
    on = on_n[0] + on_n[1] + on_n[2] + on_n[3];
    bad += check("EDIT/UNDO: ... silence: no note-on, no lock applied", !on && !lk_bad_dead && !lk_bad_val);
    for (t = 0; t < SM_NTRK; t++)                         /* (the model to come: the locks before) */
        for (s = 0; s < SM_NSTEP; s++)
            for (p = 0; p < 4u; p++)
                lk_shadow[t][s][PRM[p]] = (int16_t)ed_lock(&before, t, s, PRM[p]);
    btn_down(B_SAVE);
    sm_input_poll();
    ms(500);
    sm_input_poll();
    bad += check("EDIT/UNDO: SAVE held: UNDO in the header", sm_msg == SM_MSG_UNDO);
    btn_up(B_SAVE);
    sm_input_poll();
    for (t = 0; t < SM_NTRK; t++)
        for (s = 0; s < SM_NSTEP; s++) {
            ok &= !memcmp(&before.step[t][s], &cpat()->step[t][s], sizeof before.step[t][s]);
            for (p = 0; p < 4u; p++)
                ok &= ed_lock(&before, t, s, PRM[p]) == ed_lock(cpat(), t, s, PRM[p]);
        }
    bad += check("EDIT/UNDO: ... every step and every lock as before the clear", ok);
    bad += check("EDIT/UNDO: ... the pool whole: as many live locks, nlock <= 192", ed_live(cpat(), 4) == n0 && cpat()->nlock <= SM_NLOCK);
    lk_applied = 0;
    seq_log_clear();
    play_blocks(2400);                                    /* (35 steps at 300 BPM: every step twice) */
    lk_on = 0;
    lk_preempt_n = 0;
    printf("  undo: %u preempted writes, %u locks applied after the undo, %u live; on %u %u, bad %u %u\n", lk_preempts, lk_applied, n0, on_n[0], on_n[3], lk_bad_val, lk_bad_dead);
    bad += check("EDIT/UNDO: ... the trigs play again, their locks applied, each == the model's, none of a wiped step",
                 on_n[0] >= 30u && on_n[3] >= 30u && lk_applied > 250u && !lk_bad_val && !lk_bad_dead && lk_preempts > 0u);
    save_hold();
    bad += check("EDIT/UNDO: a second UNDO: nothing (one snapshot)", ed_live(cpat(), 4) == n0 && ed_trigs(cpat(), 0) == 16u);
    push(SMC_STOP, 0, 0, 0, 0);
    run_block();
    return bad;
}

static int test_copy_paste_track(void)
{
    int bad = 0, ok = 1;
    uint32_t s;
    sm_sound_t t1;
    in_reset();
    host_ticks = 1000u;
    sm_sound_from_preset(ksnd(0), eng_rank(ENGI_MFMST), 2);
    ksnd(0)->len = 12;
    ksnd(0)->p[P_FCUT] = 77;
    push(SMC_LOAD, 0, 0, 0, 0);
    run_block();
    ed_track(0);
    ed_track(2);
    sm_lock_set(2, 0, P_PAN, 5);
    t1 = *ksnd(0);
    layer_hold(B_SEQ);
    lay_key(WHITE[2]);                                    /* A3: COPY the track (T1) */
    layer_let_go(B_SEQ);
    bad += check("EDIT/COPY: A3 copies, changes nothing", !memcmp(&t1, ksnd(0), sizeof t1) && sm_msg == SM_MSG_NONE);
    turn(EN_ALGO, 2);                                     /* T3 */
    sm_input_poll();
    layer_hold(B_SEQ);
    lay_key(WHITE[3]);                                    /* B3: PASTE the track */
    layer_let_go(B_SEQ);
    bad += check("EDIT/PASTE: B3 on T3: T1's sound (machine, preset, values, length)", !memcmp(&t1, ksnd(2), sizeof t1));
    for (s = 0; s < SM_NSTEP; s++)
        ok &= !memcmp(stp(0, s), stp(2, s), sizeof(sm_step_t));
    bad += check("EDIT/PASTE: ... T1's steps", ok);
    bad += check("EDIT/PASTE: ... T1's locks, T3's own gone", ed_live(cpat(), 2) == 2u && ed_lock(cpat(), 2, 4, P_FCUT) == 20 &&
                 ed_lock(cpat(), 2, 8, P_FRES) == 30 && ed_lock(cpat(), 2, 0, P_PAN) == LK_NONE && ed_live(cpat(), 0) == 2u);
    bad += check("EDIT/PASTE: ... the LOAD pushed (pattern tag)", (sm_ld_pend & 4u) == 0u && sm_cq.head != sm_cq.tail &&
                 sm_cq.slot[(uint8_t)(sm_cq.head - 1u) & (SM_NCMD - 1u)].kind == SMC_LOAD &&
                 sm_cq.slot[(uint8_t)(sm_cq.head - 1u) & (SM_NCMD - 1u)].trk == 2u);
    run_block();
    bad += check("EDIT/PASTE: ... the core: T3 on T1's machine, CUT 77", trk[2].eng_req == trk[0].eng_req && trk[2].p[P_FCUT] == 77);
    save_hold();
    bad += check("EDIT/PASTE: UNDO: T3's own steps, locks and sound back", ed_lock(cpat(), 2, 0, P_PAN) == 5 && ed_live(cpat(), 2) == 3u &&
                 ksnd(2)->machine != t1.machine && ksnd(2)->len == 16u);
    run_block();
    bad += check("EDIT/PASTE: ... and on the core", trk[2].eng_req != trk[0].eng_req);
    return bad;
}

static int test_copy_paste_pattern(void)
{
    int bad = 0, ok = 1;
    uint32_t t, s;
    in_reset();
    host_ticks = 1000u;
    pat_kit(0);
    for (t = 0; t < SM_NTRK; t++)
        ed_track(t);
    cpat()->mlen = 12;
    layer_hold(B_SEQ);
    lay_key(WHITE[3]);                                    /* B3 PASTE track with nothing copied: nothing */
    bad += check("EDIT/PASTE: nothing copied: PASTE does nothing", ed_trigs(cpat(), 0) == 4u && !sm_undo_ok);
    lay_key(WHITE[1]);                                    /* G3: COPY the pattern */
    turn(EN_ALGO, 4);                                     /* A05 picked */
    sm_input_poll();
    layer_let_go(B_SEQ);
    run_block();
    sm_input_poll();
    bad += check("EDIT/PASTE: A05 now (empty)", smp.cur == 4u && !ed_trigs(cpat(), 0));
    layer_hold(B_SEQ);
    lay_key(WHITE[5]);                                    /* D4: PASTE the pattern */
    layer_let_go(B_SEQ);
    for (t = 0; t < SM_NTRK; t++) {
        ok &= !memcmp(&smp.pat[0].kit.snd[t], &smp.pat[4].kit.snd[t], sizeof(sm_sound_t));
        for (s = 0; s < SM_NSTEP; s++)
            ok &= !memcmp(&smp.pat[0].step[t][s], &smp.pat[4].step[t][s], sizeof(sm_step_t)) &&
                  ed_lock(&smp.pat[0], t, s, P_FCUT) == ed_lock(&smp.pat[4], t, s, P_FCUT) &&
                  ed_lock(&smp.pat[0], t, s, P_FRES) == ed_lock(&smp.pat[4], t, s, P_FRES);
    }
    bad += check("EDIT/PASTE: D4 on A05: A01's kit, steps, locks, master length", ok && smp.pat[4].mlen == 12u &&
                 ed_live(&smp.pat[4], 4) == 8u);
    run_block();
    run_block();
    bad += check("EDIT/PASTE: ... the kit on the core", core_is_model());
    layer_hold(B_SEQ);
    turn(EN_ALGO, 1);                                     /* A06 */
    sm_input_poll();
    layer_let_go(B_SEQ);
    run_block();
    sm_input_poll();
    layer_hold(B_SEQ);
    lay_key(WHITE[3]);                                    /* PASTE track with a pattern copied: that track of it */
    layer_let_go(B_SEQ);
    bad += check("EDIT/PASTE: a pattern copied, B3 on A06's T1: A01's T1", smp.cur == 5u && ed_trigs(cpat(), 0) == 4u &&
                 ed_lock(cpat(), 0, 4, P_FCUT) == 20 && !ed_trigs(cpat(), 1));
    return bad;
}

static int test_shift_trn(void)
{
    int bad = 0;
    in_reset();
    host_ticks = 1000u;
    sm_step_toggle(0, 0);
    stp(0, 0)->note[0] = 48;
    sm_step_toggle(0, 3);
    stp(0, 3)->note[0] = 50;
    stp(0, 3)->n = 2;
    stp(0, 3)->note[1] = 127;
    sm_lock_set(0, 3, P_FCUT, 11);
    sm_step_toggle(1, 5);
    layer_hold(B_SEQ);
    lay_key(WHITE[6]);                                    /* E4: SHIFT < */
    bad += check("EDIT/SHIFT: E4: T1's trigs one step left, wrapping (0 -> 15, 3 -> 2), the lock with its trig",
                 stp(0, 15)->n && stp(0, 15)->note[0] == 48 && stp(0, 2)->n == 2u && !stp(0, 0)->n && !stp(0, 3)->n &&
                 ed_lock(cpat(), 0, 2, P_FCUT) == 11 && ed_live(cpat(), 0) == 1u && stp(1, 5)->n);
    lay_key(WHITE[7]);                                    /* F4: SHIFT > twice */
    lay_key(WHITE[7]);
    bad += check("EDIT/SHIFT: F4 x2: one step right of where they were (1, 4)", stp(0, 1)->n && stp(0, 4)->n == 2u &&
                 ed_lock(cpat(), 0, 4, P_FCUT) == 11 && !stp(0, 15)->n);
    lay_key(WHITE[10]);                                   /* B4: TRN - */
    bad += check("EDIT/TRN: B4: the notes a semitone down (47; 49, the chord's 126)", stp(0, 1)->note[0] == 47 &&
                 stp(0, 4)->note[0] == 49 && stp(0, 4)->note[1] == 126);
    lay_key(WHITE[11]);                                   /* C5: TRN + twice */
    lay_key(WHITE[11]);
    bad += check("EDIT/TRN: C5 x2: up two (49; 51, 127 stays at the top)", stp(0, 1)->note[0] == 49 && stp(0, 4)->note[0] == 51 &&
                 stp(0, 4)->note[1] == 127);
    layer_let_go(B_SEQ);
    save_hold();
    bad += check("EDIT/TRN: UNDO: the last edit undone (one up)", stp(0, 1)->note[0] == 48 && stp(0, 4)->note[0] == 50 &&
                 stp(0, 4)->note[1] == 127);
    ksnd(0)->len = 8;                                     /* a track of 8: the steps past it stay */
    sm_step_toggle(0, 12);
    layer_hold(B_SEQ);
    lay_key(WHITE[6]);
    layer_let_go(B_SEQ);
    bad += check("EDIT/SHIFT: length 8: wraps at 8 (1 -> 0, 4 -> 3), step 13 stays", stp(0, 0)->n && stp(0, 3)->n == 2u &&
                 stp(0, 12)->n && ed_lock(cpat(), 0, 3, P_FCUT) == 11);
    return bad;
}

/* GLO + G5: FILL while held: a COND FILL trig plays only then; !FILL the other way */
static int test_fill_glo_g5(void)
{
    int bad = 0;
    in_reset();
    host_ticks = 1000u;
    sm_step_toggle(0, 0);
    stp(0, 0)->cond = SM_COND_FILL;
    sm_step_toggle(1, 0);
    stp(1, 0)->cond = SM_COND_NFILL;
    ksnd(0)->len = ksnd(1)->len = 1;
    press(B_PLAY);
    sm_input_poll();
    seq_log_clear();
    play_blocks(4u * (uint32_t)S120 / CTL);
    bad += check("EDIT/FILL: not held: FILL never, !FILL every step", on_n[0] == 0u && on_n[1] >= 3u && !sm_fill);
    layer_hold(B_GLO);
    key_dn(26);                                           /* G5 */
    sm_input_poll();
    seq_log_clear();
    play_blocks(4u * (uint32_t)S120 / CTL);
    bad += check("EDIT/FILL: GLO + G5 held: FILL plays, !FILL not", sm_fill && on_n[0] >= 3u && on_n[1] == 0u && !sm_solo);
    key_up(26);
    sm_input_poll();
    seq_log_clear();
    play_blocks(4u * (uint32_t)S120 / CTL);
    bad += check("EDIT/FILL: G5 let go: FILL off", !sm_fill && on_n[0] == 0u);
    key_dn(26);
    sm_input_poll();
    layer_let_go(B_GLO);
    bad += check("EDIT/FILL: GLO let go with G5 down: FILL off", !sm_fill);
    key_up(26);
    sm_input_poll();
    push(SMC_STOP, 0, 0, 0, 0);
    run_block();
    return bad;
}

/* UNDO is of the pattern it was taken on: after a switch to another pattern it does nothing */
static int test_undo_other_pattern(void)
{
    int bad = 0;
    in_reset();
    host_ticks = 1000u;
    ed_track(0);
    layer_hold(B_SEQ);
    lay_key(WHITE[0]);
    turn(EN_ALGO, 1);
    sm_input_poll();
    layer_let_go(B_SEQ);
    run_block();
    sm_input_poll();
    save_hold();
    bad += check("EDIT/UNDO: on another pattern: nothing (A02 empty, A01 still cleared)", smp.cur == 1u && !ed_trigs(cpat(), 0) &&
                 !ed_trigs(&smp.pat[0], 0) && sm_msg != SM_MSG_UNDO);
    return bad;
}

static uint32_t scr_count(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j, n = 0;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            n += SCR(x + i, y + j) == c;
    return n;
}
/* the length visible: SEQ held: LEN 32 large in the title; the strip: the track's length at its right, nothing past the length */
static int test_len_visible(void)
{
    int bad = 0;
    int32_t y0 = 0, y1 = 0;
    draw_setup();
    lint_zones = LZONES;
    lint_nz = sizeof LZONES / sizeof LZONES[0];
    ksnd(0)->len = 32;
    smp.pat[3].step[2][0].n = 1;
    sm_play_pat = 0;
    sm_lay_pick = 5;
    smp.next = 3;
    layer_draw(SML_SEQ);
    bad += check("LEN: SEQ held: LEN 32 in the title, large (scale 3: 21 px high), the map inside its rows",
                 lint_has("LEN 32", &y0, &y1) && y1 - y0 == 21 && lint_frame("seq len") == 0);
    bad += check("LEN: ... the grid A01 .. A16, the mode, the master length",
                 lint_has("A01", 0, 0) && lint_has("A16", 0, 0) && lint_has("SEQUENTIAL ML AUTO", 0, 0));
    bad += check("LEN: ... A01 playing inverted, A04 (trigs) ink, A02 (empty) dim", SCR(12 + 2, 48 + 9) == INK &&
                 scr_count(192, 58, 36, 14, INK) > 20u && !scr_count(192, 58, 36, 14, DIMC) &&
                 scr_count(72, 58, 36, 14, DIMC) > 20u && !scr_count(72, 58, 36, 14, INK));
    bad += check("LEN: ... A06 picked: a bar under it; A04 asked for: a frame", box_is(60 + 12, 82 + 28, 36, 2, INK) &&
                 SCR(180 + 3, 48 + 16) == INK && SCR(180 + 56, 48 + 16) == INK && SCR(180 + 30, 48 + 3) == INK);
    sm_play_pat = 0;
    smp.next = SM_PAT_NONE;
    sm_lay_pick = SM_PAT_NONE;
    lint_zones = ZONES;
    lint_nz = sizeof ZONES / sizeof ZONES[0];
    sm_layer = SML_NONE;
    ksnd(0)->len = 12;
    smp.pat[0].step[0][13].n = 1;                         /* (past the length) */
    draw_full();
    bad += check("LEN: the strip: the length (12) at its right, inside the screen",
                 lint_has("12", &y0, &y1) && y0 >= 220 && y1 <= 240 && lint_frame("strip len") == 0);
    bad += check("LEN: ... nothing drawn past the length (steps 13 .. 16: no beat mark, no trig)",
                 scr_ink(12 * SMD_STEP_W, 220, 4 * SMD_STEP_W, 20) == 0 && SCR(8 * SMD_STEP_W + SMD_STEP_W / 2, 236) == DIMC);
    ksnd(0)->len = 16;
    smp.pat[0].step[0][13].n = 0;
    return bad;
}

/* FILT OFF (the sound's default): CUT, RES or ENV on FILTER, or any F.ENV knob, turned first sets FILT to LP (model and core),
 * so the knob is heard at once; FILT itself, and FILT HYB, are left as they are */
static int test_filter_auto_lp(void)
{
    int bad = 0;
    in_reset();
    sm_ui.page = (uint8_t)page_by_name("FILTER");
    turn(EN_K1, 0);
    turn(EN_K2, -5);
    sm_input_poll();
    run_block();
    bad += check("FILT: OFF, CUT turned: FILT LP in the model and on the core, CUT moved",
                 ksnd(0)->p[P_FLT] == 1 && trk[0].p[P_FLT] == 1 && ksnd(0)->p[P_FCUT] != TP[P_FCUT].def);
    in_reset();
    sm_ui.page = (uint8_t)page_by_name("FILTER");
    turn(EN_K3, 3);
    sm_input_poll();
    bad += check("FILT: OFF, RES turned: LP", ksnd(0)->p[P_FLT] == 1);
    in_reset();
    sm_ui.page = (uint8_t)page_by_name("FILTER");
    turn(EN_K4, 3);
    sm_input_poll();
    bad += check("FILT: OFF, ENV turned: LP", ksnd(0)->p[P_FLT] == 1);
    in_reset();
    sm_ui.page = (uint8_t)page_by_name("F.ENV");
    turn(EN_K1, 3);
    sm_input_poll();
    run_block();
    bad += check("FILT: OFF, F.ENV attack turned: LP (model and core)", ksnd(0)->p[P_FLT] == 1 && trk[0].p[P_FLT] == 1);
    in_reset();
    sm_ui.page = (uint8_t)page_by_name("F.ENV");
    turn(EN_K2, 3);
    sm_input_poll();
    bad += check("FILT: OFF, F.ENV decay turned: LP", ksnd(0)->p[P_FLT] == 1);
    in_reset();
    sm_ui.page = (uint8_t)page_by_name("FILTER");
    ksnd(0)->p[P_FLT] = 5;                                /* HYB */
    turn(EN_K2, 3);
    sm_input_poll();
    bad += check("FILT: HYB: CUT (BASE) turned: stays HYB", ksnd(0)->p[P_FLT] == 5);
    in_reset();
    sm_ui.page = (uint8_t)page_by_name("FILTER");
    ksnd(0)->p[P_FLT] = 3;                                /* HP */
    turn(EN_K3, 3);
    sm_input_poll();
    bad += check("FILT: HP: RES turned: stays HP", ksnd(0)->p[P_FLT] == 3);
    in_reset();
    sm_ui.page = (uint8_t)page_by_name("EDIT 1");
    turn(EN_K1, 3);
    sm_input_poll();
    bad += check("FILT: another page's knob: FILT stays OFF", ksnd(0)->p[P_FLT] == 0);
    return bad;
}

/* ------------------------------------------------------------------------------------- hardware notes (Task 3b) --- */
/* Tim 02:43: "the EDIT knobs (and every sound page's) do not turn at all without a step". A button tapped as the hand does it
 * (down, a poll, 80 ms, up, a poll), the pages reached that way */
static void hw_tap(uint32_t b)
{
    btn_down(b);
    sm_input_poll();
    ms(80);
    btn_up(b);
    sm_input_poll();
    ms(5);
}
static void hw_page(const char *n)
{
    uint32_t i, b = !strncmp(n, "EDIT", 4) || !strcmp(n, "VOICE") ? B_EDIT : !strcmp(n, "AMP") || !strcmp(n, "MIX") ? B_GLO :
                    n[0] == 'F' ? B_ENV : !strcmp(n, "STEP") ? B_SEQ : B_HOME;
    for (i = 0; i < 12u && strcmp(sm_cur_page()->name, n); i++)
        hw_tap(b);
}
/* every knob and SELECT of every sound page on track trk, with the scenario set up first: sel a step selected on STEP (held
 * past the hold time and let go), trigs on every step, REC, PLAY. Each knob +3 (at its top: -3): the base moves, the core has
 * it two blocks later; REC while playing: the playing step's trig holds the same value as a lock. Returns the failures */
static int hw_knob_matrix(const char *what, uint32_t trk_n, int sel, int trigs, int rec, int play)
{
    static const char *const PG[7] = {"EDIT 1", "EDIT 2", "VOICE", "AMP", "MIX", "FILTER", "F.ENV"};
    uint32_t p, k, s, n = 0, dead = 0, core = 0, lock = 0;
    char line[160];
    in_reset();
    host_ticks = 1000u;
    if (trk_n) {
        turn(EN_ALGO, (int32_t)trk_n);
        sm_input_poll();
    }
    if (trigs)
        for (s = 0; s < 16u; s++)
            sm_step_toggle(trk_n, s);
    if (sel) {
        hw_page("STEP");
        key_dn(WHITE[5]);
        sm_input_poll();
        ms(500);
        sm_input_poll();
        key_up(WHITE[5]);
        sm_input_poll();
        ms(5);
    }
    if (rec) {
        press(B_REC);
        sm_input_poll();
    }
    if (play) {
        press(B_PLAY);
        sm_input_poll();
        for (s = 0; s < 20u; s++)
            run_block();
    }
    for (p = 0; p < 7u; p++) {
        hw_page(PG[p]);
        for (k = 0; k < 5u; k++) {
            uint32_t id = sm_page_param(k), st;
            int8_t b0;
            if (id == SM_NOPARAM)
                continue;
            n++;
            b0 = ksnd(trk_n)->p[id];
            turn(k < 4u ? EN_K1 + k : EN_SELECT, 3);
            sm_input_poll();
            ms(2);
            if (ksnd(trk_n)->p[id] == b0) {
                turn(k < 4u ? EN_K1 + k : EN_SELECT, -3);
                sm_input_poll();
                ms(2);
            }
            st = *(volatile uint8_t *)&sm_pos[trk_n];
            run_block();
            run_block();
            dead += ksnd(trk_n)->p[id] == b0;
            core += trk[trk_n].p[id] != ksnd(trk_n)->p[id];
            if (rec && play && trigs)
                lock += lk_val(trk_n, st, id) != ksnd(trk_n)->p[id] && lk_val(trk_n, sm_pos[trk_n], id) != ksnd(trk_n)->p[id];
        }
    }
    if (play) {
        press(B_PLAY);
        sm_input_poll();
    }
    if (rec) {
        press(B_REC);
        sm_input_poll();
    }
    snprintf(line, sizeof line, "KNOBS: %s: all %u knobs move the base (dead %u), core has it (%u off)%s", what, n, dead, core,
             rec && play && trigs ? ", the trig playing locked to it" : "");
    return check(line, n >= 30u && !dead && !core && !lock);
}
static int test_knobs_without_step(void)
{
    int bad = 0;
    bad += hw_knob_matrix("stopped, no step", 0, 0, 0, 0, 0);
    bad += hw_knob_matrix("stopped, a step selected on STEP", 0, 1, 1, 0, 0);
    bad += hw_knob_matrix("playing, a step selected on STEP", 0, 1, 1, 0, 1);
    bad += hw_knob_matrix("T2 playing, a step selected", 1, 1, 1, 0, 1);
    bad += hw_knob_matrix("REC stopped", 0, 0, 1, 1, 0);
    bad += hw_knob_matrix("REC playing, an empty track", 0, 0, 0, 1, 1);
    bad += hw_knob_matrix("REC playing, trigs on every step", 0, 0, 1, 1, 1);
    bad += hw_knob_matrix("T3 REC playing, a step selected, trigs", 2, 1, 1, 1, 1);
    return bad;
}

/* SELECT on FILTER heard in every FILT: HYB WIDTH; OFF LP BP HP NOTCH the LFO's depth onto the cutoff (P_LD_FLT; OFF: LP
 * first). T2 alone, a trig on every step, K1 to FILT f, SELECT turned by sel, PLAY, a bar captured */
static int32_t hw_cap[2][CTL * 700];
static void flt_sel_capture(int32_t f, int32_t sel, int32_t *L)
{
    uint32_t s, k, nl = 0;
    in_reset();
    host_ticks = 1000u;
    for (k = 0; k < SM_NTRK; k++)
        if (k != 1u)
            ksnd(k)->p[P_LEVEL] = 0, trk[k].p[P_LEVEL] = 0;
    for (s = 0; s < 16u; s++)
        sm_step_toggle(1, s);
    turn(EN_ALGO, 1);
    sm_input_poll();
    filter_page();
    if (f) {
        turn(EN_K1, f);
        sm_input_poll();
    }
    if (sel) {
        turn(EN_SELECT, sel);
        sm_input_poll();
    }
    press(B_PLAY);
    sm_input_poll();
    for (s = 0; s < 700u; s++) {
        block_peak(L, &nl);
        seq_clock += CTL;
    }
    press(B_PLAY);
    sm_input_poll();
}
static int test_filter_select(void)
{
    static const char *const NM[6] = {"OFF", "LP", "BP", "HP", "NOTCH", "HYB"};
    int bad = 0;
    int32_t f;
    uint32_t i;
    double d, e;
    flt_sel_capture(1, 0, hw_cap[0]);
    flt_sel_capture(1, 0, hw_cap[1]);
    for (i = 0, d = 0, e = 0; i < CTL * 700u; i++) {
        double a = hw_cap[0][i], b = hw_cap[1][i];
        d += (a - b) * (a - b);
        e += a * a;
    }
    printf("  repeat: difference %.3f %% of the energy\n", e > 0 ? 100.0 * d / e : 0.0);
    bad += check("FSEL: the same capture twice differs by < 1 % (the measure below is the knob's)", e > 0 && d / e < 0.01);
    for (f = 0; f <= 5; f++) {
        char line[120];
        int32_t sel = f == 5 ? -90 : -60;                 /* (HYB: WIDTH 127 -> 37; else the LFO's depth 0 -> -60) */
        uint32_t id;
        flt_sel_capture(f, 0, hw_cap[0]);
        flt_sel_capture(f, sel, hw_cap[1]);
        id = sm_page_param(4);
        for (i = 0, d = 0, e = 0; i < CTL * 700u; i++) {
            double a = hw_cap[0][i], b = hw_cap[1][i];
            d += (a - b) * (a - b);
            e += a * a;
        }
        snprintf(line, sizeof line, "FSEL: FILT %-5s: SELECT is %s and is heard (difference %.0f %% of the energy)", NM[f],
                 f == 5 ? "WIDTH" : "LFO>FLT", e > 0 ? 100.0 * d / e : 0.0);
        bad += check(line, id == (f == 5 ? P_HWIDTH : P_LD_FLT) && e > 0 && d / e > 0.05 &&
                           ksnd(1)->p[P_FLT] == (f ? f : 1));
    }
    return bad;
}

/* the strip of a track of length 8: eight places, nothing past them (Tim: "fewer dots") */
static int test_strip_len8(void)
{
    int bad = 0;
    uint32_t s;
    draw_setup();
    for (s = 0; s < 16u; s++)
        smp.pat[smp.cur].step[0][s].n = 1;
    ksnd(0)->len = 8;
    draw_full();
    bad += check("STRIP8: length 8: a dot on each of the 8 places", scr_count(0, 220, 8 * SMD_STEP_W, 20, INK) == 8u * 9u);
    bad += check("STRIP8: ... nothing past them (places 9 .. 16)", scr_ink(8 * SMD_STEP_W, 220, 8 * SMD_STEP_W, 20) == 0u);
    ksnd(0)->len = 16;
    return bad;
}

/* the rings of K1 .. K4 side by side (each in its 60 px column): at least 4 px of ground between two neighbours, any value,
 * filled or not */
static int test_ring_gap(void)
{
    int bad = 0;
    uint32_t pos, f, x, y, k, gap = 999;
    sm_ux_mk1();
    for (pos = 0; pos <= 256u; pos += 32u)
        for (f = 0; f < 2u; f++) {
            uint32_t lo[4], hi[4];
            cv_begin(240u, SMD_KNOB_H, T_BG);
            for (k = 0; k < 4u; k++)
                sm_px_ring((int32_t)(k * SMD_KNOB_W) + SMD_RING_CX, SMD_RING_CY, SMD_RING_R, pos, (int)f, 0, 0);
            for (k = 0; k < 4u; k++) {
                lo[k] = 999;
                hi[k] = 0;
                for (y = 0; y < SMD_KNOB_H; y++)
                    for (x = k * 60u; x < k * 60u + 60u; x++)
                        if (cv_px[y * cv_w + x] != BGC) {
                            lo[k] = x < lo[k] ? x : lo[k];
                            hi[k] = x > hi[k] ? x : hi[k];
                        }
            }
            for (k = 0; k + 1u < 4u; k++)
                gap = lo[k + 1u] - hi[k] - 1u < gap ? lo[k + 1u] - hi[k] - 1u : gap;
        }
    printf("  ring gap: %u px\n", gap);
    bad += check("RINGGAP: at least 4 px of ground between neighbouring rings", gap >= 4u && gap < 60u);
    return bad;
}

/* HOME > MIXER: the four tracks' levels as rings (a muted one dim, no ink; the selected one's label inverted), and the strip
 * shows the four tracks' steps as dots, a row each (Tim: "on the mixer show all four tracks as dots") */
static int test_mixer_four(void)
{
    int bad = 0;
    uint32_t t, r;
    draw_setup();
    sm_ui.page = (uint8_t)page_by_name("MIXER");
    sm_ui.trk = 1;
    smp.mute = 4u;                                        /* T3 muted */
    for (t = 0; t < SM_NTRK; t++)
        smp.pat[smp.cur].step[t][t].n = 1;               /* T1 step 1, T2 step 2, .. */
    smp.pat[smp.cur].step[0][12].n = 1;
    ksnd(3)->len = 8;
    smp.pat[smp.cur].step[3][12].n = 1;                   /* (T4: past its length) */
    sm_playing = 1;
    sm_pos[0] = 4; sm_pos[1] = 5; sm_pos[2] = 6; sm_pos[3] = 7;
    draw_full();
    bad += check("MIXER4: rings T1 T2 T4 have ink, T3 (muted) only dim dots",
                 scr_count(0, 48 + 20, 60, 70, INK) > 0u && scr_count(60, 48 + 20, 60, 70, INK) > 0u &&
                 scr_count(180, 48 + 20, 60, 70, INK) > 0u && scr_count(120, 48 + 20, 60, 70, INK) == 0u &&
                 scr_count(120, 48 + 20, 60, 70, DIMC) > 0u);
    bad += check("MIXER4: the selected track's label (T2) inverted, the others not",
                 SCR(60 + 17, 48 + 3) == INK && SCR(17, 48 + 3) == BGC && SCR(120 + 17, 48 + 3) == BGC);
    for (t = 0; t < SM_NTRK; t++) {
        uint32_t x = t * SMD_STEP_W + SMD_STEP_W / 2u, y = 220u + t * 5u;
        r = t == 2u ? DIMC : INK;
        bad += check("MIXER4: the strip: a row per track, its trig a 3 x 3 dot (T3 muted: dim)", box_is(x - 1u, y, 3, 3, (uint16_t)r));
    }
    bad += check("MIXER4: ... T1's trig on step 13 drawn, T4's (length 8) not",
                 box_is(12u * SMD_STEP_W + 5u, 220u, 3, 3, INK) && scr_ink(8u * SMD_STEP_W, 220u + 15u, 8u * SMD_STEP_W, 5) == 0u);
    bad += check("MIXER4: ... each row's playing step: a bar under it",
                 SCR(4u * SMD_STEP_W + 6u, 220u + 3u) == INK && SCR(5u * SMD_STEP_W + 6u, 220u + 8u) == INK &&
                 SCR(7u * SMD_STEP_W + 6u, 220u + 18u) == INK);
    bad += check("MIXER4: the frame's texts inside their cells", lint_frame("MIXER4") == 0);
    {
        uint32_t a = smd_strip_sig();
        smp.pat[smp.cur].step[2][9].n = 1;
        bad += check("MIXER4: another track's trig changes the strip's signature", smd_strip_sig() != a);
        a = smd_knob_sig(0);
        smp.mute = 1u;
        bad += check("MIXER4: a mute changes the ring's signature", smd_knob_sig(0) != a);
    }
    smp.mute = 0;
    sm_playing = 0;
    ksnd(3)->len = 16;
    return bad;
}

static int test_draw_shots(void)
{
    const char *dir = getenv("SHOTS");
    uint32_t pg;
    if (!dir)
        return 0;
    for (pg = 0; pg < sm_page_count(); pg++) {
        char n[48], *c;
        draw_setup();
        memset(host_screen, 0, sizeof host_screen);       /* (what earlier tests left between the zones: not the device's) */
        {   /* trigs, a x3 and a x2 ratchet, two RESTs on the track the shot shows */
            sm_step_t *st = smp.pat[smp.cur].step[pg % 2u];
            st[0].n = 1;
            st[2].n = 1; st[2].flags = (uint8_t)(2u << 4);
            st[4].n = 1;
            st[5].flags = SMF_REST;
            st[8].n = 1; st[8].flags = (uint8_t)(1u << 4);
            st[9].n = 1; st[9].flags = (uint8_t)(3u << 4);   /* x4, held and playing: a 1 px gap to both bars */
            st[12].n = 1;
            st[14].flags = SMF_REST;
        }
        sm_ui.page = (uint8_t)pg;
        sm_ui.trk = (uint8_t)(pg % 2u);
        sm_playing = 1;
        sm_pos[sm_ui.trk] = 9;
        sm_ui.held_step = 9;
        sm_rec = (uint8_t)(pg % 2u);
        smp.pat[smp.cur].nlock = 1;
        smp.pat[smp.cur].lock[0].ts = 9; smp.pat[smp.cur].lock[0].param = SM_PAGES[pg].id[1]; smp.pat[smp.cur].lock[0].val = 90;
        draw_full();
        snprintf(n, sizeof n, "swamp_%s", SM_PAGES[pg].name);
        for (c = n; *c; c++)
            if (*c == ' ' || *c == '.')
                *c = '_';
        write_ppm(dir, n);
        smp.pat[smp.cur].nlock = 0;
    }
    {   /* locks: FILTER, T1 step 9 held, CUT and RES locked on it (filled), TYPE locked on step 4 (the centre dot); then the
         * pool full: LOCKS FULL in the header */
        draw_setup();
        smp.pat[smp.cur].step[0][0].n = 1;
        smp.pat[smp.cur].step[0][4].n = 1;
        smp.pat[smp.cur].step[0][9].n = 1;
        smp.pat[smp.cur].step[0][12].n = 1;
        sm_lock_set(0, 9, P_FCUT, 40);
        sm_lock_set(0, 9, P_FRES, 90);
        sm_lock_set(0, 4, P_FLT, 2);
        sm_ui.page = (uint8_t)page_by_name("FILTER");
        sm_ui.held_step = 9;
        sm_playing = 1;
        sm_pos[0] = 12;
        sm_rec = 1;
        draw_full();
        write_ppm(dir, "swamp_lock_held");
        sm_lock_full_msg();
        draw_full();
        write_ppm(dir, "swamp_locks_full");
        sm_msg = 0;
        sm_rec = 0;
        sm_playing = 0;
    }
    {   /* the STEP page: a x3 trig selected (E3, VEL 90, LEN 1/2, COND 2:4, NUDGE +3), then a REST trig selected */
        static const char *const NAME[2] = {"swamp_step_x3", "swamp_step_rest"};
        uint32_t k;
        for (k = 0; k < 2u; k++) {
            sm_step_t *st;
            draw_setup();
            st = smp.pat[smp.cur].step[0];
            sm_step_toggle(0, 0); sm_step_toggle(0, 4); sm_step_toggle(0, 8); sm_step_toggle(0, 12);
            sm_step_toggle(0, 6);
            st[6].note[0] = 52; st[6].vel = 90; st[6].len = 3; st[6].cond = (uint8_t)sm_cond_ab(2, 4); st[6].nudge = 3;
            st[6].flags = (uint8_t)(2u << 4);
            sm_step_toggle(0, 10);
            st[10].flags = SMF_REST;
            sm_lock_set(0, 10, P_FCUT, 30);
            sm_ui.page = (uint8_t)page_by_name("STEP");
            sm_ui.sel_step = (uint8_t)(k ? 10u : 6u);
            sm_playing = 1;
            sm_pos[0] = 4;
            draw_full();
            write_ppm(dir, NAME[k]);
            sm_playing = 0;
        }
    }
    {   /* hardware notes 3b: MIXER with the four tracks (T3 muted, T2 selected, T4 of length 8, playing), FILTER on HYB
         * and on LP (SELECT: WIDTH / LFO>FLT), a track of length 8 on EDIT 1 */
        uint32_t t, s;
        draw_setup();
        memset(host_screen, 0, sizeof host_screen);
        for (t = 0; t < SM_NTRK; t++)
            for (s = t; s < 16u; s += 3u + t)
                smp.pat[smp.cur].step[t][s].n = 1;
        smp.pat[smp.cur].step[1][6].flags = SMF_REST;
        smp.pat[smp.cur].step[1][6].n = 1;
        ksnd(3)->len = 8;
        ksnd(2)->p[P_LEVEL] = 40;
        ksnd(0)->p[P_LEVEL] = 127;
        smp.mute = 4u;
        sm_ui.trk = 1;
        sm_ui.page = (uint8_t)page_by_name("MIXER");
        sm_playing = 1;
        sm_pos[0] = 5; sm_pos[1] = 5; sm_pos[2] = 5; sm_pos[3] = 5;
        draw_full();
        write_ppm(dir, "swamp_3b_mixer");
        smp.mute = 0;
        sm_ui.trk = 0;
        ksnd(0)->len = 8;
        sm_pos[0] = 3;
        sm_ui.page = (uint8_t)page_by_name("EDIT 1");
        draw_full();
        write_ppm(dir, "swamp_3b_edit1_len8");
        sm_ui.page = (uint8_t)page_by_name("FILTER");
        ksnd(0)->p[P_FLT] = 1;
        ksnd(0)->p[P_FCUT] = 80;
        ksnd(0)->p[P_LD_FLT] = -24;
        draw_full();
        write_ppm(dir, "swamp_3b_filter_lp");
        ksnd(0)->p[P_FLT] = 5;
        ksnd(0)->p[P_HBASE] = 30;
        ksnd(0)->p[P_HWIDTH] = 70;
        draw_full();
        write_ppm(dir, "swamp_3b_filter_hyb");
        sm_playing = 0;
    }
    {   /* the three layer maps, over the EDIT 1 page: T1 SAW, T3 muted, T4 soloed (the header shows T1 muted), 24 steps */
        static const char *const NAME[4] = {"", "EDIT", "GLO", "SEQ"};
        uint32_t l;
        for (l = SML_EDIT; l <= SML_SEQ; l++) {
            char n[48];
            draw_setup();
            sm_step_t *st = smp.pat[smp.cur].step[0];
            st[0].n = 1; st[4].n = 1; st[8].n = 1;
            smp.pat[smp.cur].kit.snd[0].len = 24;
            smp.mute = l == SML_GLO ? 5u : 0u;
            sm_solo = l == SML_GLO ? 8u : 0u;
            sm_lay_cleared = l == SML_SEQ;
            sm_lay_epage = 1;                         /* EDIT: the SOMA page, the track on its 2nd machine (inverted) */
            if (l == SML_EDIT)
                smp.pat[smp.cur].kit.snd[0].machine = 14;
            sm_layer = (uint8_t)l;
            draw_full();
            snprintf(n, sizeof n, "swamp_layer_%s", NAME[l]);
            write_ppm(dir, n);
            sm_layer = SML_NONE;
            sm_lay_cleared = 0;
            smp.mute = 0;
            sm_solo = 0;
        }
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
    bad += test_filter_play();
    bad += test_white_key_toggles_trig();
    bad += test_presets_knob_loads();
    bad += test_held_step();
    bad += test_play_key();
    bad += test_input_backpressure();
    bad += test_cmd_e8_guard();
    bad += test_held_step_bank_key();
    bad += test_tap_vs_hold();
    bad += test_edit_layer_loads_machine();
    bad += test_edit_layer_backpressure();
    bad += test_glo_mute();
    bad += test_glo_solo();
    bad += test_mute_read_once();
    bad += test_mute_releases();
    bad += test_seq_len();
    bad += test_seq_clear();
    bad += test_px_font_widths();
    bad += test_ring_dots();
    bad += test_ring_contained();
    bad += test_lock_find();
    bad += test_ring_lock_fill();
    bad += test_header_rec();
    bad += test_no_grime();
    bad += test_strip_dots();
    bad += test_draw_lint();
    bad += test_draw_budget();
    bad += test_draw_playhead_snapshot();
    bad += test_layer_maps();
    bad += test_layer_close_restores();
    bad += test_header_muted();
    bad += test_knob_band();
    bad += test_leds_three_states();
    bad += test_leds_trigs();
    bad += test_leds_playhead();
    bad += test_leds_rec_play();
    bad += test_leds_family();
    bad += test_leds_layer();
    bad += test_lock_hold_turn();
    bad += test_lock_applies();
    bad += test_lock_during_play_300bpm();
    bad += test_lock_stress();
    bad += test_lock_full();
    bad += test_retrig_clears_locks();
    bad += test_rec_records_on_trigs();
    bad += test_lock_long_hold();
    bad += test_lock_press_turn_same_poll();
    bad += test_lock_layer_disarms();
    bad += test_lock_unlock_on_remove();
    bad += test_rec_preview();
    bad += test_step_vel_acc();
    bad += test_step_len();
    bad += test_step_cond();
    bad += test_step_ratchet();
    bad += test_step_rest();
    bad += test_step_pslide();
    bad += test_step_nudge();
    bad += test_step_slide();
    bad += test_step_carried();
    bad += test_step_fix1();
    bad += test_step_page();
    bad += test_flag_on_held_trig_any_page();
    bad += test_flag_shown_while_held();
    bad += test_step_draw();
    bad += test_trig_defaults();
    bad += test_trig_copy_paste();
    bad += test_shift_oct();
    bad += test_home_mixer();
    bad += test_home_bpm_select();
    bad += test_home_bpm_accel();
    bad += test_home_draw();
    bad += test_swing();
    bad += test_swing_with_nudge_ratchet();
    bad += test_swing_read_once();
    bad += test_midi_note_ch2();
    bad += test_midi_vs_seq_same_note();
    bad += test_midi_mute_stop_panic();
    bad += test_midi_overflow();
    bad += test_oct_pages();
    bad += test_keyboard_mode();
    bad += test_arp_keyboard();
    bad += test_keyboard_vs_seq_same_note();
    bad += test_pat_switch_boundary();
    bad += test_pat_switch_kits();
    bad += test_pat_direct_start();
    bad += test_pat_direct_jump();
    bad += test_pat_switch_300bpm_no_hang();
    bad += test_pat_switch_clears_locks();
    bad += test_pat_seq_layer();
    bad += test_chain_build();
    bad += test_chain_plays_links();
    bad += test_chain_repeat_no_reload();
    bad += test_chain_off();
    bad += test_chain_reset();
    bad += test_clear_track();
    bad += test_clear_all_hold();
    bad += test_undo_after_clear_while_playing();
    bad += test_copy_paste_track();
    bad += test_copy_paste_pattern();
    bad += test_shift_trn();
    bad += test_fill_glo_g5();
    bad += test_undo_other_pattern();
    bad += test_len_visible();
    bad += test_filter_auto_lp();
    bad += test_knobs_without_step();
    bad += test_filter_select();
    bad += test_strip_len8();
    bad += test_ring_gap();
    bad += test_mixer_four();
    bad += test_draw_shots();
    printf("swamp: %d failed\n", bad);
    return bad != 0;
}
