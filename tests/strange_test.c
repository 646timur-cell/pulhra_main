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
static int32_t wav_l[SR * 12], wav_r[SR * 12];

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
    uint32_t i, k, f = 0, hold = (uint32_t)(hold_s * SR), end = SR * 12u, quiet = 0;
    double sum = 0, sq = 0, sl = 0, se = 0;
    uint32_t nl = 0;
    memset(&R, 0, sizeof R);
    ui_power_on();
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
    if (hold > SR)                                       /* the held note's pitch: 0.3 s from 0.5 s in */
        R.f0 = f0_of(SR / 2u, (uint32_t)(SR * 0.3));
    return R;
}

static int test_engines(void)
{
    int bad = 0, ok = 1;
    uint32_t i, k, total, nshown = 0;
    static const uint8_t want[] = {ENGI_LYRA, ENGI_CHAOS, ENGI_STRING, ENGI_MODAL, ENGI_BLOW, ENGI_PHYS, ENGI_GURDY};
    ui_power_on();
    bad += check("20 engines; LYRA CHAOS STRING MODAL BLOW are 15..19", NENGINES == 20 && ENGINES[ENGI_LYRA] == &ENG_LYRA &&
                 ENGINES[ENGI_CHAOS] == &ENG_CHAOS && ENGINES[ENGI_STRING] == &ENG_STRING &&
                 ENGINES[ENGI_MODAL] == &ENG_MODAL && ENGINES[ENGI_BLOW] == &ENG_BLOW &&
                 str_eq(N_ENGNAME[ENGI_BLOW], "BLOW") && str_eq(N_ENGNAME[ENGI_LYRA], "LYRA"));
    for (i = 0; i < NELEM(want); i++)
        ok &= eng_vis(i) == want[i] && eng_ok(want[i]);
    for (i = 0; i < NENGINES; i++)
        nshown += eng_ok(i) != 0;
    bad += check("shown: LYRA CHAOS STRING MODAL BLOW PHYS GURDY, nothing else", ok && nshown == 7u && NENG_SHOWN == 7);
    preset_all_pos(&total);
    ok = 1;
    for (i = 0; i < total; i++) {
        uint32_t e = preset_all_at(i, &k);
        ok &= e == NENGINES || eng_ok(e);
    }
    bad += check("the PRESETS list holds only those", ok && total > 40u);
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
    for (e = ENGI_LYRA; e < NENGINES; e++) {
        const engine_t *en = ENGINES[e];
        for (k = 0; k < en->npresets; k++) {
            char what[96];
            int ok;
            phr_t R;
            int mono = en->presets[k].mono || en->poly < 2u;
            int pitched = e == ENGI_STRING || e == ENGI_BLOW || (e == ENGI_MODAL && en->presets[k].e[0] == 7);
            int hit = (e == ENGI_STRING && en->presets[k].e[0] != SR_BOW) ||
                      (e == ENGI_MODAL && en->presets[k].e[5] <= MX_STICK);   /* (struck: it decays while held) */
            R = phrase(e, k, mono || pitched ? A3 : CH, mono || pitched ? 1u : 2u, 2.0);
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

#ifndef STRANGE_NO_MAIN
int main(void)
{
    int bad = 0;
    const char *wavs = getenv("WAVS");
    bad += test_engines();
    bad += test_parts();
    bad += test_lyra_tail();
    bad += test_sound(wavs && *wavs ? wavs : 0);
    if (bad) {
        printf("strange: %d FAILED\n", bad);
        return 1;
    }
    printf("strange: all passed\n");
    return 0;
}
#endif
