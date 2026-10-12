/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test of the sound-module build (FELUCCA_MODULE=1): the UI and sound sources of ui_test.c, its helpers, and
 * checks of what the module changes:
 *   TRACK CH   MENU > MIDI > MIDI IN TRACK CH (the default): each track on its own channel, T1 CH .. T4 CH in the
 *              MIDI tab (1..4 by default, as CH1-4), any of 16, kept with the settings; a channel no track has is
 *              ignored; a changed channel lets go of the notes the old one held.
 *   TRANSPOSE  a track's TRN moves its MIDI notes (a kit keeps its key map); a note-off ends what its note-on started
 *              even when TRN changed meanwhile; a sound load keeps TRN.
 *   HOME       KNOB 1..4 the LEVEL of T1..T4; HOME tapped again: TRANSPOSE, KNOB 1..4 the TRN of T1..T4; again:
 *              the MIXER.
 *   PANEL      no SEQ / ARP / SONG / PHRASES pages, no SEQ TOOLS or REC layer; the FX layer and FX pages kept; the
 *              transport plays no steps; ARP forced off.
 *   GURDY      engine 14 (from GHOULBOX): on the PRESETS list, its presets load and sound, its favourites apart from
 *              MIDI LEARN's row.
 * Built and run by tests/run_tests.sh (and on its own: cc -Ibuild/gen -Ifirmware/src tests/module_test.c -lm). */
#ifndef FELUCCA_MODULE
#define FELUCCA_MODULE 1
#endif
#define UI_TEST_NO_MAIN 1
#include "ui_test.c"

static uint32_t sounding(const track_t *t, uint32_t note)   /* voices of t on note, gate on (note 128: any) */
{
    uint32_t i, n = 0;
    for (i = 0; i < NVOICE; i++)
        n += t->v[i].active && t->v[i].gate && (note > 127u || t->v[i].note == note);
    return n;
}
static void all_off(void)
{
    uint32_t ch, n;
    for (ch = 0; ch < 16u; ch++)
        for (n = 0; n < 128u; n++)
            if (midi_sel_on[ch][n])
                midi_event(0x80, ch, n, 0);
    blocks(4);
}
static uint32_t menu_row_of(const char *name)
{
    uint32_t r;
    for (r = 0; r < MI_COUNT && !str_eq(MI_NAME[r], name); r++)
        ;
    return r;
}

static int test_track_ch(void)
{
    int bad = 0, ok;
    uint32_t r1 = menu_row_of("T1 CH"), r4 = menu_row_of("T4 CH"), i;
    ui_power_on();
    ok = song.g[G_ROUTE] == 0 && str_eq(N_ROUTE[0], "TRACK CH");
    for (i = 0; i < NTRK; i++)
        ok &= trk_ch(i) == i && trk_midi_ch(i) == i;
    bad += check("TRACK CH is the default, channels 1..4 -> tracks 1..4 (as CH1-4)", ok);
    ok = r1 < MI_COUNT && r4 == r1 + 3u && MI_TAB[r1] == MTAB_MIDI && MI_TAB[r4] == MTAB_MIDI && menu_n(r1) == 16u &&
         mtab_rows(MTAB_MIDI) <= MTAB_ROWS && str_eq(menu_vname(r1, menu_get(r1)), "1") && str_eq(menu_vname(r4, 15), "16");
    bad += check("MENU > MIDI: T1 CH .. T4 CH, 1..16", ok);

    midi_event(0x90, 0, 60, 100); midi_event(0x90, 3, 62, 100); midi_event(0x90, 9, 64, 100);
    blocks(2);
    bad += check("ch 1 plays T1, ch 4 plays T4, ch 10 (no track) is ignored",
                 sounding(&trk[0], 60) && sounding(&trk[3], 62) && !sounding(&trk[0], 64) && !sounding(&trk[1], 64) &&
                 !midi_sel_on[9][64]);
    menu_put(r1, 9);                                  /* T1 on channel 10 */
    blocks(2);
    bad += check("T1 CH set to 10: what channel 1 held is let go, the setting byte is the nibble", !sounding(&trk[0], 60) &&
                 trk_ch(0) == 9u && menu_get(r1) == 9u && (mch_tab[0] & 15u) == 9u);
    midi_event(0x90, 9, 65, 100); midi_event(0x90, 0, 66, 100);
    blocks(2);
    bad += check("now ch 10 plays T1 and ch 1 nothing", sounding(&trk[0], 65) && !sounding(&trk[0], 66) && !midi_sel_on[0][66]);
    bad += check("the keys of T1 send on channel 10", trk_midi_ch(0) == 9u);
    midi_event(0x80, 9, 65, 0);
    blocks(2);
    bad += check("its note-off ends it", !sounding(&trk[0], 65));
    menu_put(menu_row_of("T3 CH"), 9);                /* T3 on channel 10 too: the first track wins */
    midi_event(0x90, 9, 67, 100);
    blocks(2);
    bad += check("two tracks on one channel: the first one plays it", sounding(&trk[0], 67) && !sounding(&trk[2], 67));
    all_off();
    {
        persist_t p;
        settings_export(&p);
        bad += check("the channels are kept with the settings (favorites row 15, bytes 8..9)",
                     p.favorites.factory[15][8] == mch_tab[0] && p.favorites.factory[15][9] == mch_tab[1] &&
                     mch_tab[0] == 9u && ((mch_tab[1] & 15u) == (uint32_t)((9 - 2) & 15)));
    }
    mch_tab[0] = mch_tab[1] = 0;
    song.g[G_ROUTE] = 1;                              /* SEL as before */
    song.sel = 2;
    midi_event(0x90, 11, 60, 100);
    blocks(2);
    bad += check("SEL still plays every channel on the selected track", sounding(&trk[2], 60));
    all_off();
    song.g[G_ROUTE] = 0;
    blocks(1);
    return bad;
}

static int test_transpose(void)
{
    int bad = 0;
    uint32_t pi;
    ui_power_on();
    trk[1].p[P_TRANS] = 7;
    midi_event(0x90, 1, 60, 100);
    blocks(2);
    bad += check("TRN +7 on T2: MIDI 60 sounds 67", sounding(&trk[1], 67) && !sounding(&trk[1], 60));
    trk[1].p[P_TRANS] = -12;
    midi_event(0x80, 1, 60, 0);
    blocks(2);
    bad += check("TRN changed while held: the note-off still ends 67", !sounding(&trk[1], 128));
    midi_event(0x90, 1, 60, 100);
    blocks(2);
    bad += check("TRN -12: MIDI 60 sounds 48", sounding(&trk[1], 48));
    midi_event(0x80, 1, 60, 0);
    blocks(2);
    trk[1].p[P_TRANS] = 24;
    midi_event(0x90, 1, 120, 100);
    blocks(2);
    bad += check("clamped at 127", sounding(&trk[1], 127));
    all_off();
    song.sel = 3;
    set_engine_of(&trk[3], ENGI_DRUM);
    trk[3].p[P_TRANS] = 5;
    blocks(4);
    midi_event(0x90, 3, 36, 100);
    blocks(2);
    bad += check("a DRUM track keeps its key map (no TRN on its MIDI notes)", midi_sel_on[3][36] == 4u && !mchord_held(4, 41));
    all_off();
    song.sel = 1;
    trk[1].p[P_TRANS] = 3;
    preset_step(1);
    pi = trk[1].preset;
    preset_step(1);
    bad += check("a sound load keeps the track's TRN", trk[1].p[P_TRANS] == 3 && trk[1].preset != pi);
    return bad;
}

static int test_home(void)
{
    int bad = 0, ok;
    int16_t l2, t3;
    ui_power_on();
    press(B_ENV);
    press(B_HOME);
    bad += check("HOME from a page: HOME (the levels)", ui.home == 1u);
    l2 = trk[1].p[P_LEVEL];
    turn(EN_K1 + 1, -5);
    frame();
    bad += check("HOME KNOB 2: T2's LEVEL, whatever track is selected", trk[1].p[P_LEVEL] < l2 && song.sel == 0u);
    frames(200);
    press(B_HOME);
    frame();
    bad += check("HOME again: TRANSPOSE", ui.home == HOME_TRANSPOSE);
    t3 = trk[2].p[P_TRANS];
    turn(EN_K1 + 2, 4);
    frame();
    ok = trk[2].p[P_TRANS] == t3 + 4 && trk[0].p[P_TRANS] == 0 && trk[1].p[P_TRANS] == 0 && trk[3].p[P_TRANS] == 0;
    bad += check("TRANSPOSE KNOB 3: T3's TRN in semitones, the others untouched", ok);
    turn(EN_K1, -40);
    frame();
    bad += check("TRN stops at -24", trk[0].p[P_TRANS] == -24);
    {
        char ti[20];
        str_cpy(ti, ui.home == HOME_TRANSPOSE ? "TRANSPOSE" : "HOME", sizeof ti);
        bad += check("its title and hint", str_eq(ti, "TRANSPOSE") && str_eq(help_page(), H_TRANS));
    }
    frames(200);
    press(B_HOME);
    frame();
    bad += check("HOME a third time: the MIXER", !ui.home && cur_page()->graph == GR_TRK);
    press(B_HOME);
    frame();
    bad += check("then CLOCK (the tempo the synced effects follow)", !ui.home && cur_page()->id[0] == G_BPM);
    press(B_HOME);
    frame();
    bad += check("and back HOME from there", ui.home == 1u);
    bad += check("HOME shows the four tracks' rows by default", home_tracks() && home_levels());
    return bad;
}

static int test_mod_panel(void)
{
    int bad = 0, ok = 1;
    uint32_t i;
    ui_power_on();
    for (i = 0; i < NPAGES; i++)
        if (PAGES[i].fam == FAM_SEQ || PAGES[i].fam == FAM_ARP || PAGES[i].graph == GR_SONG || PAGES[i].graph == GR_PATS)
            ok &= !page_visible(i);
    bad += check("no SEQ, ARP, SONG or PHRASES page", ok);
    press(B_SEQ); frame();
    bad += check("SEQ tapped: no page", ui.home == 1u);
    press(B_ARP); frame();
    bad += check("ARP tapped: no page", ui.home == 1u);
    press(B_GLO); frame();
    bad += check("GLO tapped: no SONG", ui.home == 1u);
    hold(B_SEQ);
    bad += check("SEQ held: no SEQ TOOLS", ui.layer != LAYER_SEQ);
    hold(B_REC);
    bad += check("REC held: no REC layer, nothing armed", ui.layer != LAYER_REC && !song.rec);
    press(B_REC); frame();
    bad += check("REC tapped: nothing armed, no transport", !song.rec && !transport_req && !song.playing);
    btn_down(B_FX); frames(800);
    bad += check("FX held: the FX layer is still there", ui.layer == LAYER_FX);
    btn_up(B_FX); frames(200);
    press(B_FX); frame();
    bad += check("FX tapped: the FX pages are still there", !ui.home && cur_page()->fam == FAM_FX);
    press(B_SAVE); frame();
    for (i = 0; i < 6u && !ui.home; i++) {
        ok &= cur_page()->graph != GR_PATS;
        press(B_SAVE); frame();
    }
    bad += check("SAVE's pages go round without PHRASES", ok);
    go_home();
    {   /* steps in T1, the transport on: nothing plays */
        trk[0].step[0].n = 1; trk[0].step[0].time = ST_NOTE;
        trk[0].step[0].note[0] = 60;
        trk[0].step[4].n = 1; trk[0].step[4].time = ST_NOTE;
        trk[0].step[4].note[0] = 64;
        trk[0].p[P_AMODE] = 1;
        transport_req = 1;
        blocks(400);
        bad += check("the transport runs and plays no steps", song.playing && !sounding(&trk[0], 128));
        bad += check("ARP forced off", trk[0].p[P_AMODE] == 0);
        transport_req = 2;
        blocks(4);
    }
    return bad;
}

static int test_gurdy(void)
{
    int bad = 0, ok = 0;
    uint32_t i, k, total, e;
    uint8_t row14[32];
    ui_power_on();
    bad += check("GURDY is engine 14, shown before DRUM", ENGI_GURDY == 14u && ENGINES[ENGI_GURDY] == &ENG_GURDY &&
                 eng_ok(ENGI_GURDY) && str_eq(N_ENGNAME[ENGI_GURDY], "GURDY"));
    preset_all_pos(&total);
    for (i = 0; i < total; i++)
        if (preset_all_at(i, &k) == ENGI_GURDY)
            ok++;
    bad += check("its six presets on the PRESETS list", ok == 6);
    song.sel = 0;
    set_engine_of(TSEL, ENGI_GURDY);
    apply_preset(0);
    blocks(4);
    bad += check("HURDY GURDY loads", TSEL->eng_req == ENGI_GURDY && str_eq(ENG_GURDY.presets[TSEL->preset].name, "HURDY GURDY"));
    bad += check("its category is LEAD, DRONE WHEEL's PAD", preset_cat(ENGI_GURDY, 0) == CAT_LEAD && preset_cat(ENGI_GURDY, 1) == CAT_PAD);
    {
        int32_t buf[2 * 256];
        int64_t sum = 0;
        midi_event(0x90, 0, 62, 110);
        for (i = 0; i < 40u; i++) {
            memset(buf, 0, sizeof buf);
            events_block(128);
            track_render(&trk[0], buf, 128);
            for (k = 0; k < 128u; k++)
                sum += buf[k] < 0 ? -buf[k] : buf[k];
        }
        midi_event(0x80, 0, 62, 0);
        bad += check("it sounds (one note, ~0.1 s, not silent)", sum > 1000000);
    }
    memcpy(row14, favorites.factory[14], sizeof row14);
    e = favorite_set(ENGI_GURDY, 2, 1);
    bad += check("a GURDY favourite: row 15, MIDI LEARN's row 14 untouched", e && favorite_has(ENGI_GURDY, 2) &&
                 (favorites.factory[15][0] & 4u) && !memcmp(row14, favorites.factory[14], sizeof row14));
    favorite_set(ENGI_GURDY, 2, 0);
    bad += check("and off again", !favorite_has(ENGI_GURDY, 2) && !(favorites.factory[15][0] & 4u));
    return bad;
}

int main(void)
{
    int bad = 0;
    bad += test_track_ch();
    bad += test_transpose();
    bad += test_home();
    bad += test_mod_panel();
    bad += test_gurdy();
    if (bad) {
        printf("module: %d FAILED\n", bad);
        return 1;
    }
    printf("module: all passed\n");
    return 0;
}
