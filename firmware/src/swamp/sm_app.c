/* SPDX-License-Identifier: GPL-3.0-only */
/* SWAMP MACHINE app: the part of the build that the host test (tests/swamp_test.c) shares with the firmware
 * (swamp.c includes it after the sound core and audio.c, before master.c and sm_main.c). No build-flag conditionals, nothing
 * from ui*.c, seq.c, project.c, mf_pat.c, song_chain.c. The next tasks add the input and the screen. */

#include "sm_model.h"
#include "sm_model.c"
#include "sm_kit.c"

/* bit per track: release every sounding note (M-UPGRADE sets it before it takes the flash; events_block serves it) */
static volatile uint8_t panic_req;

#include "sm_cmd.c"                                       /* the command queue: the main loop -> the audio stream */
#include "sm_seq.c"                                       /* events_block: the commands, the engines, the steps */
#include "sm_midi.c"                                      /* MIDI IN: the notes of channels 1..4 (midi_in_q of usb.c) onto T1..T4 */

#include "sm_pages.c"                                     /* the pages, sm_ui */
#include "sm_lock.c"                                      /* the parameter locks: the pool, its writers (the main loop's) */
#include "sm_bank.c"                                      /* the banks A..E: two in RAM, the flash (2 copies, autosave) */
#include "sm_input.c"                                     /* sm_input_poll: the knobs and keys into the model */
#include "sm_edit.c"                                      /* CLEAR, COPY / PASTE, SHIFT, TRN, UNDO: tracks and patterns rewritten */
#include "sm_layer.c"                                     /* the button holds: tap or hold, the EDIT, GLO and SEQ layers */

#include "sm_font.c"                                      /* the 5 x 7 pixel font */
#include "sm_px.c"                                        /* pixel text and the knob ring on the canvas */

/* the main loop's second half (sm_main.c): the model onto the screen */
#include "sm_draw.c"
#include "sm_leds.c"                                      /* the panel LEDs: the steps, the playhead, the page family, REC, PLAY */

/* power-on, before audio_init: every track with its parameter defaults and empty steps, the master level, then the kit
 * of the current pattern onto the tracks (T1 SAW, T2 FM-ST, T3 STRING, T4 BBOX), the sequencer stopped, the queue empty */
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
    sm_project_init();
    sm_kit_boot(&smp.pat[smp.cur].kit);
    sm_seq_init();
    sm_input_init();
    sm_layer_init();
    sm_edit_init();
    sm_bank_init();
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
