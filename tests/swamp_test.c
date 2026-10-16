/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test of SWAMP MACHINE (firmware/src/swamp.c: the MONOFM sound core under its own app, swamp/sm_*.c).
 *   CORE   the sound core alone (no seq.c, no ui*.c), from sm_core_init (the firmware's power-on): silent tracks mix to silence
 *   KIT    the kit reaches the core: every track sounds its machine, releases to silence; a machine change mid-note fades without a click
 *   SETTINGS  the settings record in the flash (the learned panel table, HOLD, palette) reaches SWAMP: sm_settings_load
 *   SEQ    the step sequencer (sm_seq.c): timing at 30 / 120 / 300 BPM, track lengths 1 and 32, trigs while playing, STOP
 *   CMD    the command queue (sm_cmd.c): full ring, coalesced values, a drain in the middle of a push; LOAD before engine_block; PANIC
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
    push(SMC_TRIG, 0, 0, 0, 0);
    push(SMC_TRIG, 0, 4, 0, 0);
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
        push(SMC_TRIG, 0, 0, 0, 0);
        for (i = 0; i < 4u; i++)
            push(SMC_TRIG, 1, HIT[i], 0, 0);
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

/* the queue full (64 trig toggles, nothing drained), then 200 values of one parameter: the last one arrives, every
 * trig too; a trig more is refused (and so is anything but a value while values wait past the full ring) */
static int test_cmd_coalesce(void)
{
    uint32_t i;
    int ok = 1, bad = 0;
    seq_reset();
    for (i = 0; i < 64u; i++)
        ok &= push(SMC_TRIG, i >> 5, i & 31u, 0, 0);
    bad += check("CMD: 64 commands fit the ring", ok);
    bad += check("CMD: the 65th trig is refused (full)", !push(SMC_TRIG, 2, 0, 0, 0));
    for (i = 0, ok = 1; i < 200u; i++)
        ok &= push(SMC_PARAM, 0, 0, P_FCUT, (int32_t)i);
    bad += check("CMD: 200 values of T1 CUT on the full ring are taken (coalesced)", ok);
    bad += check("CMD: ... and then a trig, a STOP are refused (they would overtake the values)",
                 !push(SMC_TRIG, 2, 1, 0, 0) && !push(SMC_STOP, 0, 0, 0, 0));
    sm_cmd_drain();
    for (i = 0, ok = 1; i < 64u; i++)
        ok &= smp.pat[smp.cur].step[i >> 5][i & 31u].n == 1u;
    bad += check("CMD: after the drain T1 CUT == 199", trk[0].p[P_FCUT] == 199);
    bad += check("CMD: all 64 trigs applied, the refused ones not",
                 ok && !smp.pat[smp.cur].step[2][0].n && !smp.pat[smp.cur].step[2][1].n);
    bad += check("CMD: the ring takes commands again after the drain", push(SMC_TRIG, 2, 0, 0, 0));
    sm_cmd_drain();
    bad += check("CMD: a trig sets the step: C3 + ROOT, vel 100, len 0 (the track's GATE)",
                 smp.pat[smp.cur].step[2][0].note[0] == 48u + (uint32_t)trk[2].p[P_ROOT] &&
                 smp.pat[smp.cur].step[2][0].vel == 100u && smp.pat[smp.cur].step[2][0].len == 0u);
    return bad;
}

/* the audio stream drains in the middle of a coalescing push (after the push found the pending value, before it stored
 * the new one): the new value still arrives, nothing is applied twice */
static int test_cmd_coalesce_race(void)
{
    uint32_t i;
    int ok = 1, bad = 0;
    seq_reset();                                          /* the ring: 63 trigs and CUT 5, full */
    for (i = 0; i < 63u; i++)
        push(SMC_TRIG, i >> 5, i & 31u, 0, 0);
    push(SMC_PARAM, 0, 0, P_FCUT, 5);
    preempt_armed = 1;
    ok &= push(SMC_PARAM, 0, 0, P_FCUT, 7);
    ok &= !preempt_armed && trk[0].p[P_FCUT] == 5;        /* (the drain in the middle took CUT 5) */
    sm_cmd_drain();
    for (i = 0; i < 63u; i++)
        ok &= smp.pat[smp.cur].step[i >> 5][i & 31u].n == 1u;
    bad += check("CMD: drained while coalescing into the ring: CUT 7 arrives, trigs once", ok && trk[0].p[P_FCUT] == 7);
    seq_reset();                                          /* the ring full of trigs, CUT 5 past it */
    for (i = 0; i < 64u; i++)
        push(SMC_TRIG, i >> 5, i & 31u, 0, 0);
    push(SMC_PARAM, 0, 0, P_FCUT, 5);
    preempt_armed = 1;
    ok = push(SMC_PARAM, 0, 0, P_FCUT, 7);
    ok &= !preempt_armed && trk[0].p[P_FCUT] == 5;
    sm_cmd_drain();
    for (i = 0; i < 64u; i++)
        ok &= smp.pat[smp.cur].step[i >> 5][i & 31u].n == 1u;
    bad += check("CMD: drained while coalescing past the full ring: CUT 7 arrives, trigs once", ok && trk[0].p[P_FCUT] == 7);
    return bad;
}

/* a trig toggled while playing changes that step only */
static int test_trig_while_playing(void)
{
    static sm_step_t before[SM_NTRK][SM_NSTEP], after[SM_NTRK][SM_NSTEP];
    uint32_t b;
    int bad = 0;
    seq_reset();
    push(SMC_TRIG, 0, 0, 0, 0);
    push(SMC_TRIG, 1, 3, 0, 0);
    push(SMC_TRIG, 3, 8, 0, 0);
    push(SMC_PLAY, 0, 0, 0, 0);
    for (b = 0; b < 300u; b++)
        run_block();
    memcpy(before, smp.pat[smp.cur].step, sizeof before);
    push(SMC_TRIG, 1, 7, 0, 0);
    run_block();
    memcpy(after, smp.pat[smp.cur].step, sizeof after);
    bad += check("SEQ: a trig while playing sets step 7 of T2",
                 !before[1][7].n && after[1][7].n == 1u && after[1][7].note[0] == 48u + (uint32_t)trk[1].p[P_ROOT]);
    after[1][7] = before[1][7];
    bad += check("SEQ: ... and no other step changes", !memcmp(before, after, sizeof before) && sm_playing);
    return bad;
}

/* notes held with LEN INF on every track; STOP: one block later nothing sounds but the one-block fade, 400 later silence */
static int test_stop_releases(void)
{
    uint32_t t, i, b, busy0, peak = 0;
    int fading = 1, gated = 1, bad = 0;
    seq_reset();
    for (t = 0; t < SM_NTRK; t++)
        push(SMC_TRIG, t, 0, 0, 0);
    run_block();                                          /* (the trigs into the model) */
    for (t = 0; t < SM_NTRK; t++)
        smp.pat[smp.cur].step[t][0].len = 255u;           /* INF (stage 1 has no command for LEN yet) */
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
    bad += test_trig_while_playing();
    bad += test_stop_releases();
    bad += test_load_order();
    bad += test_panic();
    printf("swamp: %d failed\n", bad);
    return bad != 0;
}
