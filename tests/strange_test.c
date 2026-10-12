/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test of the STRANGE build (FELUCCA_STRANGE=1, a sound-module build): the UI and sound sources of ui_test.c and
 * checks of what STRANGE changes:
 *   ENGINES   LYRA CHAOS STRING MODAL BLOW are engines 15..19, shown first, then PHYS and GURDY; the others hidden;
 *             every preset of the seven has a category; the favourites of the new ones in row 1
 *   PARTS     two: T3 and T4 never render, hear no MIDI, cannot be selected
 *   SOUND     every preset of the five plays a phrase through the whole mix (mix_block): it sounds, stays under full
 *             scale, has no DC, is silent again after the release (LYRA: after its delay rings out); the pitched
 *             ones are in tune (an autocorrelation of the held note); a sustained bow or breath does not die out
 *   CPU       instructions per sample of each preset, two parts at once at their voice caps (STRANGE_CPU=1 only,
 *             under valgrind: tests/strange_cpu.sh)
 * WAVS=dir writes each preset's phrase as a WAV (44.1 kHz stereo) there.
 *   cc -O1 -w -DFELUCCA_STRANGE=1 -Ibuild/gen -Ifirmware/src -o build/host/strange_test tests/strange_test.c -lm */
#ifndef FELUCCA_STRANGE
#define FELUCCA_STRANGE 1
#endif
#define FELUCCA_MODULE 1
#define UI_TEST_NO_MAIN 1
#include "ui_test.c"
#include <math.h>

#define SR 44100u
static int32_t wav_l[SR * 30], wav_r[SR * 30];

static void wav_write(const char *dir, const char *eng, const char *name, uint32_t frames)
{
    char path[512], nm[64];
    uint32_t i, k = 0, bytes = frames * 4u;
    FILE *f;
    for (i = 0; name[i] && k + 1u < sizeof nm; i++)
        nm[k++] = name[i] == ' ' ? '_' : name[i];
    nm[k] = 0;
    snprintf(path, sizeof path, "%s/%s_%s.wav", dir, eng, nm);
    f = fopen(path, "wb");
    if (!f)
        return;
    {
        uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
                         0x44, 0xAC, 0, 0, 0x10, 0xB1, 0x02, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
        uint32_t r = bytes + 36u;
        memcpy(h + 4, &r, 4);
        memcpy(h + 40, &bytes, 4);
        fwrite(h, 1, 44, f);
    }
    for (i = 0; i < frames; i++) {
        int16_t s[2] = {(int16_t)clamp(wav_l[i], -32768, 32767), (int16_t)clamp(wav_r[i], -32768, 32767)};
        fwrite(s, 2, 2, f);
    }
    fclose(f);
}

static uint32_t voices_on(const track_t *t)
{
    uint32_t i, n = 0;
    for (i = 0; i < NVOICE; i++)
        n += t->v[i].active;
    return n;
}

/* the fundamental (Hz) of wav_l[a .. a + len): the first strong peak of the normalised autocorrelation between 30
 * Hz and 2 kHz, refined by a parabola; 0 = none */
static double f0_of(uint32_t a, uint32_t len)
{
    static double r[SR / 30u + 2u];
    uint32_t lag, lo = SR / 2000u, hi = SR / 30u, best = 0, i;
    double e0 = 0, top = 0;
    for (i = 0; i < len; i++)
        e0 += (double)wav_l[a + i] * wav_l[a + i];
    if (e0 <= 0)
        return 0;
    for (lag = lo; lag <= hi + 1u; lag++) {
        double s = 0;
        for (i = 0; i + lag < len; i++)
            s += (double)wav_l[a + i] * wav_l[a + i + lag];
        r[lag] = s / e0;
        if (r[lag] > top)
            top = r[lag];
    }
    for (lag = lo + 1u; lag <= hi; lag++)              /* the first peak within 85 % of the strongest */
        if (r[lag] > r[lag - 1u] && r[lag] >= r[lag + 1u] && r[lag] > 0.85 * top) {
            best = lag;
            break;
        }
    if (!best)
        return 0;
    {
        double y0 = r[best - 1u], y1 = r[best], y2 = r[best + 1u], d = y0 - 2 * y1 + y2;
        double off = d != 0 ? 0.5 * (y0 - y2) / d : 0;
        return SR / (best + off);
    }
}

typedef struct {
    double peak, dc, rms_hold, rms_late, rms_early, f0, tail_rms;
    uint32_t clip, frames, free_at;
} phr_t;

/* T1 plays notes (vel 100) for hold_s, released, rendered on until the voices are free and the tail has rung out (at
 * most 12 s in all); T2 silent */
static phr_t phrase(uint32_t eng, uint32_t preset, const uint8_t *notes, uint32_t nn, double hold_s)
{
    phr_t R;
    int32_t buf[2 * CTL];
    uint32_t i, k, f = 0, hold = (uint32_t)(hold_s * SR), end = SR * 30u, quiet = 0;
    double sum = 0, sq = 0, sl = 0, se = 0;
    uint32_t nl = 0;
    memset(&R, 0, sizeof R);
    ui_power_on();
    memset(xy_val, 0, sizeof xy_val);                    /* (performance state: not reset by a power-on of the tests) */
    rng_state = 0x1234567u;                              /* (the noise seeds: the same whatever ran before) */
    song.master_q12 = 4096;
    song.sel = 0;
    set_engine_of(&trk[0], eng);
    apply_preset_to(&trk[0], preset);
    for (k = 0; k < 8u; k++)                            /* settle the engine switch */
        mix_block(buf, CTL);
    for (k = 0; k < nn; k++)
        midi_event(0x90, 0, notes[k], 100);
    R.free_at = 0;
    while (f < end) {
        if (f >= hold && f - hold < CTL)
            for (k = 0; k < nn; k++)
                midi_event(0x80, 0, notes[k], 0);
        mix_block(buf, CTL);
        for (i = 0; i < CTL && f < end; i++, f++) {
            int32_t l = buf[2u * i], r = buf[2u * i + 1u], a = l < 0 ? -l : l;
            wav_l[f] = l;
            wav_r[f] = r;
            if (a > R.peak)
                R.peak = a;
            if (a >= 32000)
                R.clip++;
            sum += l;
            if (f > SR / 2u && f < hold) {
                sq += (double)l * l;
                nl++;
            }
            if (f > hold - SR / 4u && f < hold)
                sl += (double)l * l;
            if (f > SR / 50u && f < SR * 3u / 10u)
                se += (double)l * l;
        }
        if (f > hold && !voices_on(&trk[0]) && !R.free_at)
            R.free_at = f;
        if (R.free_at) {
            int32_t m = 0;
            for (i = 0; i < CTL; i++)
                m = abs(buf[2u * i]) > m ? abs(buf[2u * i]) : m;
            quiet = m < 8 ? quiet + CTL : 0;
            if (quiet > SR / 2u)                          /* half a second of silence: done */
                break;
        }
    }
    R.frames = f;
    R.dc = sum / f;
    R.rms_hold = nl ? sqrt(sq / nl) : 0;
    R.rms_late = sqrt(sl / (SR / 4u));
    R.rms_early = sqrt(se / (SR * 3u / 10u - SR / 50u));
    R.tail_rms = quiet > SR / 2u ? 0 : 1;
    if (hold > SR)                                       /* the held note's pitch: 0.3 s from 0.5 s in (a long hold: */
        R.f0 = f0_of(hold > 3u * SR ? hold - SR / 2u : SR / 2u, (uint32_t)(SR * 0.3));   /* its end, past the attack) */
    return R;
}

static int test_engines(void)
{
    int bad = 0, ok = 1;
    uint32_t i, k, total, nshown = 0;
    static const uint8_t want[] = {ENGI_LYRA, ENGI_CHAOS, ENGI_STRING, ENGI_MODAL, ENGI_BLOW, 8, ENGI_PHYS, ENGI_GURDY, ENGI_FM6, 0, 7, 2, 5, 11, 3};
    ui_power_on();
    bad += check("20 engines; LYRA CHAOS STRING MODAL BLOW are 15..19", NENGINES == 20 && ENGINES[ENGI_LYRA] == &ENG_LYRA &&
                 ENGINES[ENGI_CHAOS] == &ENG_CHAOS && ENGINES[ENGI_STRING] == &ENG_STRING &&
                 ENGINES[ENGI_MODAL] == &ENG_MODAL && ENGINES[ENGI_BLOW] == &ENG_BLOW &&
                 str_eq(N_ENGNAME[ENGI_BLOW], "BLOW") && str_eq(N_ENGNAME[ENGI_LYRA], "LYRA"));
    for (i = 0; i < NELEM(want); i++)
        ok &= eng_vis(i) == want[i] && eng_ok(want[i]);
    for (i = 0; i < NENGINES; i++)
        nshown += eng_ok(i) != 0;
    bad += check("shown: every engine but DIGITAL (retired), SAMPLE, TRIO, DRUM and SLICE, STRANGE's five first",
                 ok && nshown == NELEM(want) && NENG_SHOWN == NELEM(want) && !eng_ok(1) && !eng_ok(ENGI_DRUM) && !eng_ok(13) &&
                 !eng_ok(ENGI_SAMPLE) && !eng_ok(6) && ENGINES[6] == &ENG_FM4_GONE && ENGINES[13] == &ENG_FM4_GONE &&
                 ENGINES[ENGI_GURDY] == &ENG_GURDY);
    preset_all_pos(&total);
    ok = 1;
    for (i = 0; i < total; i++) {
        uint32_t e = preset_all_at(i, &k);
        ok &= e == NENGINES || eng_ok(e);
    }
    bad += check("the PRESETS list holds every shown engine's sounds", ok && total > 100u);
    ok = 1;
    for (i = ENGI_LYRA; i < NENGINES; i++) {
        const char *s = PRESET_CAT[i];
        ok &= s && strlen(s) == ENGINES[i]->npresets;
    }
    bad += check("every new preset has a category (PRESET_CAT's strings as long as the lists)", ok);
    ok = 1;
    for (i = ENGI_LYRA; i < NENGINES; i++) {
        for (k = 0; k < ENGINES[i]->npresets; k++)
            ok &= strlen(ENGINES[i]->presets[k].name) <= 12u;
        ok &= ENGINES[i]->mem <= ENG_MEM_PART;
    }
    bad += check("names fit, engine memory fits the part's region", ok);
    {
        uint8_t row14[32], row15[32];
        memcpy(row14, favorites.factory[14], 32);
        memcpy(row15, favorites.factory[15], 32);
        ok = favorite_set(ENGI_BLOW, 3, 1) && favorite_has(ENGI_BLOW, 3) && favorite_set(ENGI_LYRA, 0, 1) &&
             (favorites.factory[1][24] & 8u) && (favorites.factory[1][0] & 1u) &&
             !memcmp(row14, favorites.factory[14], 32) && !memcmp(row15, favorites.factory[15], 32);
        bad += check("favourites of the new engines: row 1, MIDI LEARN's and the settings' rows untouched", ok);
        {
            persist_t ps;
            settings_export(&ps);
            memset(&favorites, 0, sizeof favorites);
            settings_import(&ps, sizeof ps);
            bad += check("they survive the settings' save and load (no DIGITAL migration of row 1)",
                         favorite_has(ENGI_BLOW, 3) && favorite_has(ENGI_LYRA, 0));
        }
        favorite_set(ENGI_BLOW, 3, 0);
        favorite_set(ENGI_LYRA, 0, 0);
    }
    {   /* darkmachine: DARK by default, and once over earlier settings (then MENU > COLOR as saved) */
        persist_t ps;
        uint32_t keep = settings.palette;
        settings.magic = 0;
        settings_init();
        ok = settings.palette == UI_DARK_INDEX && !strcmp(UI_PALETTES[UI_DARK_INDEX].name, "DARK");
        settings.palette = UI_GREY_INDEX;
        settings_export(&ps);
        ps.favorites.factory[15][25] = 0;                    /* (settings of before) */
        settings_import(&ps, sizeof ps);
        ok &= settings.palette == UI_DARK_INDEX;
        settings.palette = UI_GREY_INDEX;                    /* (MENU > COLOR GREY, saved) */
        settings_export(&ps);
        settings_import(&ps, sizeof ps);
        ok &= settings.palette == UI_GREY_INDEX;
        bad += check("DARK: the default, set once over earlier settings, then the saved COLOR", ok);
        settings.palette = keep;
        palette_set(keep);
    }
    ui_power_on();
    bad += check("power on: T1 LYRA, T2 STRING", trk[0].eng_req == ENGI_LYRA && trk[1].eng_req == ENGI_STRING);
    return bad;
}

static int test_parts(void)
{
    int bad = 0;
    int32_t buf[2 * CTL];
    uint32_t k;
    ui_power_on();
    song.master_q12 = 4096;
    trk[2].p[P_LEVEL] = trk[3].p[P_LEVEL] = 127;
    midi_event(0x90, 2, 60, 100);
    midi_event(0x90, 3, 62, 100);
    for (k = 0; k < 20u; k++)
        mix_block(buf, CTL);
    bad += check("channels 3, 4 (T3 T4 CH) are heard by nobody", !voices_on(&trk[2]) && !voices_on(&trk[3]) &&
                 !voices_on(&trk[0]) && !voices_on(&trk[1]));
    midi_event(0x90, 1, 57, 100);
    for (k = 0; k < 20u; k++)
        mix_block(buf, CTL);
    bad += check("channel 2 plays T2", voices_on(&trk[1]) == 1u);
    midi_event(0x80, 1, 57, 0);
    trk[2].v[0].active = 1;                               /* (a voice on T3 by other means: it is not rendered) */
    trk[2].v[0].stage = 2;
    trk[2].v[0].env = 1 << 24;
    for (k = 0; k < 4u; k++)
        mix_block(buf, CTL);
    bad += check("T3 never renders (its voice is left as it was)", trk[2].v[0].env == 1 << 24 && trk[2].v[0].active);
    trk[2].v[0].active = 0;
    track_select(2);
    track_select(3);
    bad += check("T3, T4 cannot be selected", song.sel < 2u);
    track_select(1);
    bad += check("T2 can", song.sel == 1u);
    return bad;
}

typedef struct {
    uint32_t eng;
    uint8_t note[3], nn;
    double hz;                                           /* the expected fundamental (0: no pitch check) */
} who_t;

static int test_sound(const char *wavs)
{
    int bad = 0;
    uint32_t e, k;
    static const uint8_t A3[] = {57}, CH[] = {57, 64};
    /* the stock engines' own sounds: tests/regress.c's; here their STRANGE textures, after them */
    static const uint8_t STOCK_N[ENGI_LYRA] = {[0] = 12, [8] = 4, [9] = 9, [11] = 4, [14] = 6};
    for (e = 0; e < NENGINES; e++) {
        const engine_t *en = ENGINES[e];
        if (e < ENGI_LYRA && (!STOCK_N[e] || !eng_ok(e)))
            continue;
        for (k = e < ENGI_LYRA ? STOCK_N[e] : 0u; k < en->npresets; k++) {
            char what[96];
            int ok;
            phr_t R;
            int mono = en->presets[k].mono || en->poly < 2u;
            int pitched = e == ENGI_STRING || e == ENGI_BLOW || (e == ENGI_MODAL && en->presets[k].e[0] == 7);
            int hit = (e == ENGI_STRING && en->presets[k].e[0] != SR_BOW) ||
                      (e == ENGI_MODAL && en->presets[k].e[5] <= MX_STICK);   /* (struck: it decays while held) */
            R = phrase(e, k, mono || pitched ? A3 : CH, mono || pitched ? 1u : 2u, en->presets[k].env[0] >= 80 ? 5.0 : 2.0);
            if (wavs)
                wav_write(wavs, en->name, en->presets[k].name, R.frames);
            snprintf(what, sizeof what, "%s %-12s peak %5.0f rms %5.0f dc %4.0f clip %u free %.2fs f0 %.1f", en->name,
                     en->presets[k].name, R.peak, R.rms_hold, R.dc, R.clip, R.free_at / (double)SR, R.f0);
            ok = R.peak > 1500 && R.clip == 0 && fabs(R.dc) < 200 && R.free_at && !R.tail_rms &&
                 (hit ? R.rms_early > 600 : R.rms_hold > 600);
            if (hit)
                R.f0 = f0_of(SR / 20u, (uint32_t)(SR * 0.2));
            if (pitched && e != ENGI_MODAL && R.f0 > 0) {
                double ct = 1200 * log2(R.f0 / 220.0);
                if (e == ENGI_BLOW)                      /* (an overblown preset: the note's octave) */
                    ct = fmod(ct + 1800.0, 1200.0) - 600.0;
                ok &= fabs(ct) < (e == ENGI_BLOW ? 60 : 25);
            } else if (pitched && e != ENGI_MODAL) {
                ok = 0;                                  /* no pitch found */
            }
            if (e == ENGI_STRING && en->presets[k].e[0] == SR_BOW)   /* a bow sustains: the last quarter second of the hold */
                ok &= R.rms_late > 0.3 * R.rms_hold;
            if (e == ENGI_BLOW)
                ok &= R.rms_late > 0.3 * R.rms_hold;
            bad += check(what, ok);
        }
    }
    return bad;
}

/* LYRA's delay rings on after the voices: the part still mixed, then silent */
static int test_lyra_tail(void)
{
    int bad = 0;
    int32_t buf[2 * CTL];
    uint32_t k, after = 0, loud = 0;
    ui_power_on();
    song.master_q12 = 4096;
    set_engine_of(&trk[0], ENGI_LYRA);
    apply_preset_to(&trk[0], 0);
    trk[0].p[P_REL] = 0;
    trk[0].p[P_ATK] = 0;
    trk[0].p[P_REV] = trk[0].p[P_CHOR] = trk[0].p[P_DLY] = 0;
    for (k = 0; k < 8u; k++)
        mix_block(buf, CTL);
    midi_event(0x90, 0, 60, 120);
    for (k = 0; k < 400u; k++)
        mix_block(buf, CTL);
    midi_event(0x80, 0, 60, 0);
    for (k = 0; k < 4000u; k++) {
        uint32_t i;
        int32_t m = 0;
        mix_block(buf, CTL);
        for (i = 0; i < 2u * CTL; i++)
            m = abs(buf[i]) > m ? abs(buf[i]) : m;
        if (!voices_on(&trk[0])) {
            after++;
            if (m > 200)
                loud = after;
        }
    }
    bad += check("LYRA: the echoes go on after the voice is free (>0.2 s) and end", loud > 300u && loud < after - 400u);
    return bad;
}


/* the RMS of T1 playing note 57 (vel 110) for n blocks after 60 blocks of attack, through the whole mix */
static double mix_rms(uint32_t eng, uint32_t pre, uint32_t n, void (*tweak)(void))
{
    int32_t buf[2 * CTL];
    uint32_t k, i;
    double sq = 0;
    ui_power_on();
    memset(xy_val, 0, sizeof xy_val);
    song.master_q12 = 4096;
    set_engine_of(&trk[0], eng);
    apply_preset_to(&trk[0], pre);
    trk[0].p[P_REV] = trk[0].p[P_DLY] = trk[0].p[P_CHOR] = 0;
    for (k = 0; k < 8u; k++)
        mix_block(buf, CTL);
    if (tweak)
        tweak();
    midi_event(0x90, 0, 57, 110);
    for (k = 0; k < 60u + n; k++) {
        mix_block(buf, CTL);
        if (k >= 60u)
            for (i = 0; i < 2u * CTL; i++)
                sq += (double)buf[i] * buf[i];
    }
    return sqrt(sq / (2.0 * CTL * n));
}
static void tw_lpf(void) { song.g[G_MLPF] = 110; }
static void tw_hpf(void) { song.g[G_MHPF] = 120; }
static void tw_x(void) { midi_event(0xB0, 0, 16, 127); }

static int test_xy_filter(void)
{
    int bad = 0, ok;
    double r0, rl, rh, rx;
    int16_t e2;
    /* the XY morph: LYRA's X default routes (MOD SHRP HLFO), a slot of the matrix taking X instead */
    ui_power_on();
    set_engine_of(&trk[0], ENGI_LYRA);
    apply_preset_to(&trk[0], 0);
    trk[0].engine = trk[0].eng_req;
    e2 = trk[0].p[P_E2];
    midi_event(0xB0, 0, 16, 127);
    blocks(2);
    bad += check("CC16 on T1's channel sets T1's X; CC17 its Y", xy_val[0][0] == 127 && (midi_event(0xB0, 0, 17, 64), blocks(2),
                 xy_val[0][1] == 64));
    ok = mod_begin(&trk[0]) && trk[0].p[P_E2] > e2 && trk[0].p[P_E2] <= 127;
    mod_end(&trk[0]);
    bad += check("X at the top: LYRA's MOD up for the block (its default route), the stored value back after", ok &&
                 trk[0].p[P_E2] == e2);
    trk[0].p[P_M1SRC] = MS_X;
    trk[0].p[P_M1DST] = MD_E1 + 6;                         /* X -> FDBK only */
    trk[0].p[P_M1AMT] = 40;
    {
        int16_t e6 = trk[0].p[P_E6];
        ok = mod_begin(&trk[0]) && trk[0].p[P_E2] == e2 && trk[0].p[P_E6] > e6;
        mod_end(&trk[0]);
    }
    bad += check("a matrix slot taking X: its route plays, X's defaults do not (Y's still do)", ok);
    xy_val[0][0] = xy_val[0][1] = 0;
    trk[0].p[P_M1SRC] = trk[0].p[P_M1DST] = trk[0].p[P_M1AMT] = 0;
    bad += check("X, Y at 0 and an empty matrix: no modulation at all", !mod_begin(&trk[0]));
    /* HOME: KNOB 3 / 4 the selected track's X / Y; TRANSPOSE: the master HPF / LPF */
    ui_power_on();
    go_home();
    song.sel = 1;
    turn(EN_K1 + 2, 10);
    frame();
    turn(EN_K1 + 3, 5);
    frame();
    bad += check("HOME KNOB 3 / 4: T2's X and Y (the selected track)", xy_val[1][0] > 0 && xy_val[1][1] > 0 && !xy_val[0][0]);
    frames(200);
    press(B_HOME);
    frame();
    turn(EN_K1 + 2, 6);
    frame();
    turn(EN_K1 + 3, 6);
    frame();
    bad += check("TRANSPOSE KNOB 3 / 4: the master HPF and LPF", ui.home == HOME_TRANSPOSE && song.g[G_MHPF] > 0 &&
                 song.g[G_MLPF] > 0);
    {
        char v[12];
        const char *u;
        param_format(&GP[G_MLPF], 0, v, &u);
        ok = str_eq(v, "OFF");
        param_format(&GP[G_MLPF], 127, v, &u);
        bad += check("LPF: 0 OFF, up closes it (127: 30 Hz)", ok && str_eq(v, "30") && str_eq(u, "Hz"));
    }
    /* the filters on the mix */
    r0 = mix_rms(ENGI_STRING, 0, 300, 0);
    rl = mix_rms(ENGI_STRING, 0, 300, tw_lpf);
    rh = mix_rms(ENGI_STRING, 0, 300, tw_hpf);
    bad += check("the master LPF near the bottom and the HPF near the top take the cello down by > 12 dB",
                 r0 > 1000 && rl < r0 / 4 && rh < r0 / 4);
    rx = mix_rms(ENGI_LYRA, 0, 300, tw_x);
    bad += check("LYRA with X at the top sounds otherwise than at 0", fabs(rx - mix_rms(ENGI_LYRA, 0, 300, 0)) > 1.0);
    return bad;
}


/* EDIT layer: one page, the first 15 engines on the white keys from F3, INIT on G5, LOFI SAMPLE on the black keys 1, 2 */
static uint32_t black_key(uint32_t p) { return key_at(1, p); }
static int test_edit_pages(void)
{
    int bad = 0, ok;
    ui_power_on();
    set_engine_of(TSEL, ENGI_LYRA);
    go_home();
    frame();
    lay_combo(B_EDIT, white(1));                        /* G3: CHAOS */
    key_up(white(1));
    frame();
    ok = TSEL->eng_req == ENGI_CHAOS && ui.layer == LAYER_EDIT;
    key_down(white(14)); key_up(white(14)); frame();    /* F5: the 15th, LOFI */
    ok &= TSEL->eng_req == 3u;
    TSEL->p[P_AHOLD] = 0;
    key_down(black_key(10)); key_up(black_key(10)); frame();   /* F#5: LATCH */
    ok &= TSEL->eng_req == 3u && eqa_count(TSEL) == 11u && TSEL->p[P_AHOLD] == 1;
    key_down(black_key(10)); key_up(black_key(10)); frame();
    ok &= TSEL->p[P_AHOLD] == 0;
    key_down(black_key(0)); key_up(black_key(0)); frame();   /* F#3: nothing (no engines past the white keys) */
    ok &= TSEL->eng_req == 3u && TSEL->p[P_AHOLD] == 0;
    btn_up(B_EDIT); frame();
    bad += check("EDIT, one page: 15 engines on the white keys, LATCH on the last black key, F#5 (on, off)", ok);
    return bad;
}

/* LATCH (P_AHOLD in a module, VOICE 3): notes stay after their keys; a new gesture replaces them, the same single note
 * again stops it, LATCH OFF lets go */
static void run(uint32_t n)
{
    int32_t buf[2 * CTL];
    while (n--)
        mix_block(buf, CTL);
}
static int test_latch(void)
{
    int bad = 0;
    track_t *t = &trk[0];
    int16_t keep = t->p[P_VOICE];
    ui_power_on();
    set_engine_of(t, 0);
    apply_preset_to(t, 0);
    t->engine = t->eng_req;
    t->p[P_VOICE] = V_POLY;
    t->p[P_AHOLD] = 1;
    run(4);
    midi_event(0x90, 0, 60, 100); midi_event(0x90, 0, 64, 100); run(4);
    midi_event(0x80, 0, 60, 0); midi_event(0x80, 0, 64, 0); run(20);
    bad += check("LATCH: a chord sounds on after its keys", gated(t, 60) && gated(t, 64));
    midi_event(0x90, 0, 67, 100); run(4); midi_event(0x80, 0, 67, 0); run(20);
    bad += check("LATCH: a new gesture replaces it", gated(t, 67) && !gated(t, 60) && !gated(t, 64));
    midi_event(0x90, 0, 67, 100); run(4);
    bad += check("LATCH: the same note again stops it", !gated(t, 67));
    midi_event(0x80, 0, 67, 0); run(4);
    midi_event(0x90, 0, 62, 100); run(4); midi_event(0x80, 0, 62, 0); run(4);
    bad += check("LATCH: and the next one latches again", gated(t, 62));
    t->p[P_AHOLD] = 0; run(4);
    bad += check("LATCH OFF lets go", !gated(t, 62));
    midi_event(0x90, 0, 60, 100); run(4); t->p[P_AHOLD] = 1; run(2); midi_event(0x80, 0, 60, 0); run(4);
    bad += check("a key down before LATCH ON is let go as usual", !gated(t, 60));
    midi_event(0x90, 0, 65, 100); run(4); midi_event(0x80, 0, 65, 0); run(4);
    bad += check("(then a new one latches)", gated(t, 65));
    t->p[P_AHOLD] = 0; run(4);
    t->p[P_VOICE] = keep;                                 /* (a track's own: no power-on of the tests resets it) */
    return bad;
}

/* EUCLID (SEQ): the arp through STEPS / HITS / ROT over the held notes */
static int test_euclid(void)
{
    int bad = 0, ok = 1;
    track_t *t = &trk[0];
    uint32_t k, last_pos, last_idx, hits = 0, pat = 0, guard = 0;
    ui_power_on();
    set_engine_of(t, 0);
    apply_preset_to(t, 0);
    t->engine = t->eng_req;
    t->p[P_AMODE] = 1;                                    /* UP */
    t->p[P_SLEN] = 8; t->p[P_SSWING] = 3; t->p[P_SGATE] = 0;
    run(4);
    midi_event(0x90, 0, 60, 100); midi_event(0x90, 0, 64, 100);
    run(1);
    last_pos = t->euc_pos; last_idx = t->arp_idx;
    pat = t->arp_idx != 0xFFFFFFFFu ? 1u : 0u;            /* (step 0 fired on the first block) */
    hits = pat;
    for (k = 1; k < 8u && guard < 200000u; guard++) {
        run(1);
        if (t->euc_pos != last_pos) {
            if (t->arp_idx != last_idx) { pat |= 1u << k; hits++; }
            last_pos = t->euc_pos; last_idx = t->arp_idx;
            k++;
        }
    }
    ok = pat == 0x49u && hits == 3u;                      /* x..x..x. */
    t->p[P_SGATE] = 1;                                    /* ROT 1: .x..x..x */
    ok &= euc_hit(t, 1) && euc_hit(t, 4) && euc_hit(t, 7) && !euc_hit(t, 0);
    t->p[P_SSWING] = 8;
    ok &= euc_hit(t, 0) && euc_hit(t, 5);                 /* HITS = STEPS: every step */
    midi_event(0x80, 0, 60, 0); midi_event(0x80, 0, 64, 0); run(8);
    ok &= !t->arp_note;
    bad += check("EUCLID: 3 of 8 (x..x..x.), ROT turns it, HITS = STEPS plays every step, let go: silent", ok);
    t->p[P_AMODE] = 0; t->p[P_SLEN] = 16; t->p[P_SSWING] = 16; t->p[P_SGATE] = 0;
    go_home(); frame();
    press(B_SEQ);
    ok = !ui.home && !strcmp(cur_page()->title, "EUCLID") && !song.seq_mode;
    press(B_SEQ);
    ok &= !strcmp(cur_page()->title, "EUCLID 2");
    press(B_SEQ); press(B_SEQ);
    ok &= !strcmp(cur_page()->title, "EUCLID");
    go_home(); frame();
    bad += check("SEQ: EUCLID, EUCLID 2, EUCLID 3 round (the keys still play)", ok);
    return bad;
}

/* ARP > X / Y: the sound's own routes replace the engine's; ARP's pages FAV X Y, FAV the favourites' list */
static void tw_x_amp(void) { trk[0].p[XY_ROUTE(0)] = 4; trk[0].p[XY_ROUTE(0) + 1u] = -64; midi_event(0xB0, 0, 16, 127); }
static void tw_x_none(void) { trk[0].p[XY_ROUTE(0)] = 4; trk[0].p[XY_ROUTE(0) + 1u] = 0; midi_event(0xB0, 0, 16, 127); }
static int test_xy_routes(void)
{
    int bad = 0, ok;
    double dry = mix_rms(0, 0, 40, 0), amp = mix_rms(0, 0, 40, tw_x_amp), none = mix_rms(0, 0, 40, tw_x_none);
    ok = amp < dry * 0.5 && fabs(none - mix_rms(0, 0, 40, tw_x)) < dry * 0.02;
    bad += check("X's own route (AMP -64) turns it down; a route without an amount: the engine's play", ok);
    ui_power_on();
    trk[0].p[XY_ROUTE(1)] = 2; trk[0].p[XY_ROUTE(1) + 1u] = 30;
    apply_preset_to(&trk[0], 0);
    ok = !xy_custom(trk[0].p, 1) && !strcmp(TP[XY_ROUTE(0)].label, "DST1");
    bad += check("a factory preset clears the own routes (the engine's play again)", ok);
    go_home(); frame();
    press(B_ARP);
    ok = !strcmp(cur_page()->title, "FAV") && list_mode() == 1u;
    press(B_ARP);
    ok &= !strcmp(cur_page()->title, "X") && list_mode() != 1u;
    press(B_ARP);
    ok &= !strcmp(cur_page()->title, "Y");
    go_home(); frame();
    bad += check("ARP: FAV (the FAV list), X, Y", ok);
    return bad;
}

/* SCENES: GLO held, G4 .. G5: a tap stores into an empty one, calls a stored one back; held 0.8 s stores over it */
static int test_scenes(void)
{
    int bad = 0, ok;
    int32_t buf[2 * CTL];
    uint32_t k;
    ui_power_on();
    memset(&sc_bank, 0, sizeof sc_bank);
    sc_last = 0xFFu;
    set_engine_of(&trk[0], ENGI_LYRA); apply_preset_to(&trk[0], 2); trk[0].engine = trk[0].eng_req;
    set_engine_of(&trk[1], ENGI_MODAL); apply_preset_to(&trk[1], 1); trk[1].engine = trk[1].eng_req;
    trk[0].p[P_E0 + 2] = 33; trk[0].p[P_AHOLD] = 1;
    song.g[G_MLPF] = 40; xy_val[1][0] = 90;
    for (k = 0; k < 4u; k++) mix_block(buf, CTL);
    midi_event(0x90, 0, 50, 100); midi_event(0x90, 0, 57, 100);
    for (k = 0; k < 4u; k++) mix_block(buf, CTL);
    midi_event(0x80, 0, 50, 0); midi_event(0x80, 0, 57, 0);
    for (k = 0; k < 4u; k++) mix_block(buf, CTL);
    go_home(); frame();
    lay_combo(B_GLO, white(8)); key_up(white(8)); frame(); btn_up(B_GLO); frame();   /* G4: empty, a tap stores */
    ok = sc_used(0) && sc_last == 0 && sc_bank.s[0].part[0].nlatch == 2u && sc_bank.s[0].part[0].eng == ENGI_LYRA;
    /* change everything */
    set_engine_of(&trk[0], ENGI_FM6); apply_preset_to(&trk[0], 0);
    set_engine_of(&trk[1], 0); apply_preset_to(&trk[1], 0);
    song.g[G_MLPF] = 0; xy_val[1][0] = 0; trk[0].p[P_AHOLD] = 0;
    for (k = 0; k < 40u; k++) mix_block(buf, CTL);
    lay_combo(B_GLO, white(8)); key_up(white(8)); frame(); btn_up(B_GLO); frame();   /* a tap: called back */
    for (k = 0; k < 60u; k++) mix_block(buf, CTL);
    ok &= trk[0].eng_req == ENGI_LYRA && trk[0].p[P_E0 + 2] == 33 && trk[0].p[P_AHOLD] == 1 && trk[1].eng_req == ENGI_MODAL &&
          song.g[G_MLPF] == 40 && xy_val[1][0] == 90 && gated(&trk[0], 50) && gated(&trk[0], 57);
    bad += check("scenes: a tap on an empty key stores, a tap calls back both parts, globals, XY and LATCH's notes", ok);
    xy_val[1][0] = 10;
    lay_combo(B_GLO, white(8)); frames(900); key_up(white(8)); frame(); btn_up(B_GLO); frame();   /* held: stores over */
    ok = sc_bank.s[0].xy[1][0] == 10 && !sc_used(1);
    xy_val[1][0] = 77;
    lay_combo(B_GLO, white(8)); key_up(white(8)); frame(); btn_up(B_GLO); frame();
    ok &= xy_val[1][0] == 10;
    bad += check("scenes: held 0.8 s stores over a stored one (no call back on letting go)", ok);
    memset(&sc_bank, 0, sizeof sc_bank);
    sc_last = 0xFFu;
    trk[0].p[P_AHOLD] = 0;
    go_home(); frame();
    return bad;
}

#ifndef STRANGE_NO_MAIN
int main(void)
{
    int bad = 0;
    const char *wavs = getenv("WAVS");
    bad += test_engines();
    bad += test_parts();
    bad += test_lyra_tail();
    bad += test_xy_filter();
    bad += test_edit_pages();
    bad += test_latch();
    bad += test_euclid();
    bad += test_xy_routes();
    bad += test_scenes();
    bad += test_sound(wavs && *wavs ? wavs : 0);
    if (bad) {
        printf("strange: %d FAILED\n", bad);
        return 1;
    }
    printf("strange: all passed\n");
    return 0;
}
#endif
