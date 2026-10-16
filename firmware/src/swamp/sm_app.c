/* SPDX-License-Identifier: GPL-3.0-only */
/* SWAMP MACHINE app: the part of the build that the host test (tests/swamp_test.c) shares with the firmware
 * (swamp.c includes it after the sound core and audio.c, before master.c and sm_main.c). No build-flag conditionals, nothing
 * from ui*.c, seq.c, project.c, mf_pat.c, song_chain.c. The next tasks add the model, the sequencer and the screen. */

/* the sequencer's side of the audio block (fx.c mix_block calls it first); the stub plays nothing (Task 4 replaces it:
 * the audio stream is the only writer of trk[]) */
static void events_block(uint32_t n) { (void)n; }

/* bit per track: release every sounding note (M-UPGRADE sets it before it takes the flash; events_block, Task 4, serves it) */
static volatile uint8_t panic_req;

/* the main loop's two halves, 15 ms apart (sm_main.c): the knobs and keys into the model, the model onto the screen
 * (Tasks 5 and 6 replace them) */
static void sm_input_poll(void) {}
static void sm_draw(void) {}

/* power-on, before audio_init: every track with its parameter defaults and empty steps, the master level; the
 * engines at rest (Task 3 gives each track its machine) */
static void sm_core_init(void)
{
    uint32_t i, k;
    for (i = 0; i < G_COUNT; i++)
        song.g[i] = GP[i].def;
    for (k = 0; k < NTRK; k++) {
        for (i = 0; i < P_E0; i++)
            trk[k].p[i] = TP[i].def;
        for (i = 0; i < NSTEP; i++)
            trk[k].step[i].time = ST_REST;
    }
    song.master_q12 = 2048;                   /* (master_boot replaces it with the pot's level before the audio starts) */
}

/* the settings record (storage.c OBJ_SETTINGS, written by Felucca): its panel table and settings into panel / settings,
 * as project.c persist_boot does; no record, a torn or a foreign one: the defaults (panel_init, settings_init) */
static void sm_settings_load(void)
{
    persist_t p;
    int n = st_load(OBJ_SETTINGS, &p, sizeof p);
    (void)settings_import(&p, n);
}
