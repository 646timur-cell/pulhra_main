/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test of the MONOFM build (FELUCCA_MONOFM=1, a groovebox build: Felucca's sequencer and panel, darkmachine's
 * engines only; MONOFM.md):
 *   ENGINES  the 14 darkmachine engines are the only ones shown, Felucca's 0..14 hidden; the EDIT layer's white keys
 *            are them in ENGINE_ORDER, INIT after; power-on: STRING MODAL LYRA ERROR on T1..T4
 *   PARTS    four render and hear MIDI (channels 1..4)
 *   STEPS    a pattern on each of the four tracks plays its engine through the whole mix: every track sounds, the
 *            mix stays under full scale with no DC; with the heaviest engines on all four, too
 *   P-LOCK   a step's lock sets the parameter while its step plays, the base comes back after it
 *   MEMORY   ENG_MEM_PART holds each engine's state (the static asserts) and four of them fit the pool budget
 * WAVS=dir writes the four-track mixes as WAVs.
 *   cc -O2 -w -DFELUCCA_MONOFM=1 -DFELUCCA_MODULE=0 -Ibuild/gen -Ifirmware/src -o build/host/monofm_test tests/monofm_test.c -lm */
#ifndef FELUCCA_MONOFM
#define FELUCCA_MONOFM 1
#endif
#define FELUCCA_MODULE 0
#define UI_TEST_NO_MAIN 1
#include "ui_test.c"
#include <math.h>

#define SR 44100u
static int32_t wav_l[SR * 12], wav_r[SR * 12];

static void wav_write(const char *dir, const char *name, uint32_t frames)
{
    char path[512];
    uint32_t i, bytes = frames * 4u, r = bytes + 36u;
    FILE *f;
    uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
                     0x44, 0xAC, 0, 0, 0x10, 0xB1, 0x02, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
    snprintf(path, sizeof path, "%s/monofm_%s.wav", dir, name);
    f = fopen(path, "wb");
    if (!f)
        return;
    for (i = 0; i < 4u; i++) {
        h[4 + i] = (uint8_t)(r >> (8 * i));
        h[40 + i] = (uint8_t)(bytes >> (8 * i));
    }
    fwrite(h, 1, 44, f);
    for (i = 0; i < frames; i++) {
        int16_t s[2] = {(int16_t)clamp(wav_l[i], -32768, 32767), (int16_t)clamp(wav_r[i], -32768, 32767)};
        fwrite(s, 2, 2, f);
    }
    fclose(f);
}

static int test_engines(void)
{
    int bad = 0, ok = 1;
    uint32_t i, e, pg;
    bad += check("MONOFM: 27 machines shown (13 MONO, 9 SOMA, 5 SLICE)", NENG_SHOWN == 27u && NENGINES == 42u &&
                 NLIVE == NPART && !FELUCCA_MODULE);
    for (e = 0; e < ENGI_LYRA; e++)
        ok &= !eng_ok(e);
    bad += check("MONOFM: Felucca's engines 0..14 hidden", ok);
    ok = 1;
    for (i = 0; i < NENG_SHOWN; i++)
        ok &= eng_vis(i) >= ENGI_LYRA && eng_vis(i) < NENGINES && eng_ok(eng_vis(i)) && eng_rank(eng_vis(i)) == i;
    for (e = ENGI_LYRA; e < NENGINES; e++)
        ok &= eng_ok(e) && ENGINES[e]->npresets > 0u && ENGINES[e]->render && ENGINES[e]->note_on;
    bad += check("MONOFM: darkmachine's 14 and the 13 machines shown, each once, each with presets", ok);
    ok = 1;
    for (pg = 0; pg < 3u; pg++) {
        ly_epage = (uint8_t)pg;
        for (i = 0; i < 16u; i++)
            ok &= ly_eng(i) == (i < (uint32_t)(MF_PAGE[pg + 1u] - MF_PAGE[pg]) ? ENGINE_ORDER[MF_PAGE[pg] + i] : NENGINES);
        for (i = MF_PAGE[pg]; i < MF_PAGE[pg + 1u]; i++)
            ok &= ly_pg_of(i) == pg;
    }
    ly_epage = 0;
    bad += check("MONOFM: EDIT's three pages (MONO SOMA SLICE), white keys F3.., INIT on G5", ok && LY_PAGES == 3u &&
                 LY_INIT == 15u && ly_eng(0) == ENGI_MSAW);
    ui_power_on();
    bad += check("MONOFM: power-on T1..T4 SAW FM-ST STRING BBOX", trk[0].eng_req == ENGI_MSAW &&
                 trk[1].eng_req == ENGI_MFMST && trk[2].eng_req == ENGI_STRING && trk[3].eng_req == ENGI_MBBOX);
    ok = 1;
    for (e = ENGI_LYRA; e < NENGINES; e++)
        ok &= ENGINES[e]->mem <= ENG_MEM_PART;
    bad += check("MONOFM: every engine's state fits a part's 28 KiB", ok && ENG_MEM_PART == 28672u);
    ok = 1;
    for (e = ENGI_MSAW; e < NENGINES; e++) {             /* favourites: two bytes of row 0 each, none shared */
        uint32_t k;
        for (k = 0; k < ENGINES[e]->npresets; k++)
            ok &= fav_byte(e, k) == &favorites.factory[0][(e - ENGI_MSAW) * 2u + k / 8u];
        ok &= ENGINES[e]->npresets <= 16u && str_eq(N_ENGNAME[e], ENGINES[e]->name);
    }
    bad += check("MONOFM: the machines' names, favourites (row 0), categories", ok);
    /* the pool (app.ld POOL, 0x5A000): what the stock build keeps besides the engine memory (276,248 B, CI's log), four
     * parts of ENG_MEM_PART, the global delay 32 KiB shorter; at least 8 KiB spare (tools/build.py) */
    bad += check("MONOFM: four parts fit the pool with 8 KiB spare",
                 276248u - 2u * 65536u + sizeof dly_buf + NPART * ENG_MEM_PART + 8192u <= 0x5A000u);
    return bad;
}

/* the fundamental (Hz) of wav_l[a .. a + len): the first strong autocorrelation peak, 30 Hz .. 2 kHz; 0 = none */
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
    for (lag = lo + 1u; lag <= hi; lag++)
        if (r[lag] > r[lag - 1u] && r[lag] >= r[lag + 1u] && r[lag] > 0.85 * top) {
            best = lag;
            break;
        }
    if (!best)
        return 0;
    {
        double y0 = r[best - 1u], y1 = r[best], y2 = r[best + 1u], d = y0 - 2 * y1 + y2;
        return SR / (best + (d != 0 ? 0.5 * (y0 - y2) / d : 0));
    }
}

/* every preset of every machine 29..41: note 45 (A2, 110 Hz) held 1 s on T1, released; the whole mix 2 s. It sounds, stays
 * under full scale, has no DC, goes quiet after the release; the plain pitched ones are in tune (f0 of the held note) */
static int test_machines(const char *wavs)
{
    int bad = 0;
    uint32_t e, k;
    for (e = ENGI_MSAW; e < NENGINES; e++)
        for (k = 0; k < ENGINES[e]->npresets; k++) {
            int32_t buf[2 * CTL], peak = 0;
            uint32_t f = 0, i, hold = SR, end = 2u * SR, clip = 0;
            double sum = 0, sq = 0, late = 0, f0 = 0;
            char what[160];
            int pitched;
            ui_power_on();
            memset(eng_mem, 0, sizeof eng_mem);
            memset(eng_mem_owner, 0, sizeof eng_mem_owner);
            memset(dly_buf, 0, sizeof dly_buf);
            rev_clear();
            rng_state = 0x1234567u;
            song.master_q12 = 4096;
            song.sel = 0;
            set_engine_of(&trk[0], e);
            apply_preset_to(&trk[0], k);
            trk[0].p[P_DLY] = trk[0].p[P_REV] = trk[0].p[P_CHOR] = 0;   /* (the dry sound: its pitch, its silence) */
            for (i = 0; i < 8u; i++)
                mix_block(buf, CTL);
            midi_event(0x90, 0, 45, 100);
            while (f < end) {
                if (f >= hold && f - hold < CTL)
                    midi_event(0x80, 0, 45, 0);
                mix_block(buf, CTL);
                for (i = 0; i < CTL && f < end; i++, f++) {
                    int32_t l = buf[2u * i], a = l < 0 ? -l : l;
                    wav_l[f] = l;
                    wav_r[f] = buf[2u * i + 1u];
                    peak = a > peak ? a : peak;
                    clip += a >= 32000;
                    sum += l;
                    if (f > SR / 10u && f < hold)
                        sq += (double)l * l;
                    if (f > end - SR / 4u)
                        late += (double)l * l;
                }
            }
            pitched = (e == ENGI_MSAW || e == ENGI_MPULSE || e == ENGI_MSINE || e == ENGI_MWAVE || e == ENGI_MFMPAR ||
                       (e == ENGI_MSID && k != 3u && k != 2u && k != 4u) || (e == ENGI_MFMST && k != 2u) || e == ENGI_MDDRAW) &&
                      !(e == ENGI_MWAVE && k >= 3u) && !(e == ENGI_MDDRAW && k == 3u) && !(e == ENGI_MFMPAR && k == 3u);
            if (pitched)
                f0 = f0_of(SR / 2u, (uint32_t)(SR * 0.3));
            snprintf(what, sizeof what, "MONOFM %-6s %-12s peak %5d rms %5.0f dc %4.0f clip %u late %4.0f f0 %6.1f",
                     ENGINES[e]->name, ENGINES[e]->presets[k].name, peak, sqrt(sq / (SR * 0.9)), sum / f, clip,
                     sqrt(late / (SR / 4u)), f0);
            bad += check(what, peak > 2000 && clip < 16u && fabs(sum / f) < 400.0 &&
                               (ENGINES[e]->presets[k].env[3] >= 100u || sqrt(late / (SR / 4u)) < 600.0) &&
                               (!pitched || (f0 > 0 && (fabs(f0 / 110.0 - 1.0) < 0.02 || fabs(f0 / 55.0 - 1.0) < 0.02 ||
                                                       fabs(f0 / 220.0 - 1.0) < 0.02))));
            if (wavs) {
                char nm[80];
                snprintf(nm, sizeof nm, "%s_%s", ENGINES[e]->name, ENGINES[e]->presets[k].name);
                for (i = 0; nm[i]; i++)
                    nm[i] = nm[i] == ' ' || nm[i] == '/' ? '_' : nm[i];
                wav_write(wavs, nm, f);
            }
        }
    return bad;
}

/* T tracks i < nt: engine eng[i] preset 0, a note on every step in step_every, the rest muted (solo); n seconds of the
 * whole mix (mix_block). Returns the peaks: per track (its meter) and of the mix; clips and the DC */
typedef struct {
    int32_t tpeak[NPART], peak;
    uint32_t clip;
    double dc, rms;
    uint32_t frames;
} run_t;
static run_t run_pattern(const uint8_t *eng, uint32_t mask, double secs)
{
    run_t R;
    int32_t buf[2 * CTL];
    uint32_t i, k, f = 0, end = (uint32_t)(secs * SR);
    double sum = 0, sq = 0;
    memset(&R, 0, sizeof R);
    ui_power_on();
    memset(eng_mem, 0, sizeof eng_mem);                 /* (a run before: its tails, LYRA's delay, gone, as at power-on) */
    memset(eng_mem_owner, 0, sizeof eng_mem_owner);
    memset(dly_buf, 0, sizeof dly_buf);
    rng_state = 0x1234567u;
    song.master_q12 = 4096;
    song.g[G_BPM] = 120;
    for (i = 0; i < NPART; i++) {
        track_t *t = &trk[i];
        set_engine_of(t, eng[i]);
        apply_preset_to(t, 0);
        memset(t->step, 0, sizeof t->step);
        t->p[P_SLEN] = 16;
        for (k = 0; k < 16u; k += 4u - (i & 1u)) {       /* (T2, T4 every third step: the parts off each other) */
            step_t *s = &t->step[k];
            s->n = 1;
            s->note[0] = (uint8_t)(48 + 7 * i + (k % 8u));
            s->time = ST_NOTE;
            s->vel = 100;
        }
        t->p[P_MUTE] = !((mask >> i) & 1u);
    }
    for (k = 0; k < 8u; k++)                            /* settle the engine switches */
        mix_block(buf, CTL);
    for (i = 0; i < NPART; i++)
        trk[i].peak = 0;
    seq_start();
    while (f < end) {
        mix_block(buf, CTL);
        for (i = 0; i < CTL && f < end; i++, f++) {
            int32_t l = buf[2u * i], r = buf[2u * i + 1u], a = l < 0 ? -l : l;
            wav_l[f] = l;
            wav_r[f] = r;
            if (a > R.peak)
                R.peak = a;
            if (a >= 32000 || (r < 0 ? -r : r) >= 32000)
                R.clip++;
            sum += l;
            sq += (double)l * l;
        }
        for (i = 0; i < NPART; i++)
            if (trk[i].peak > R.tpeak[i])
                R.tpeak[i] = trk[i].peak;
    }
    seq_stop();
    R.frames = f;
    R.dc = sum / f;
    R.rms = sqrt(sq / f);
    return R;
}

static int test_steps(const char *wavs)
{
    static const uint8_t SETS[][NPART] = {
        {ENGI_MSAW, ENGI_MFMST, ENGI_STRING, ENGI_MBBOX},          /* the power-on four */
        {ENGI_MSID, ENGI_MWAVE, ENGI_MVO, ENGI_MENS},
        {ENGI_MPULSE, ENGI_MDDRAW, ENGI_MFMDYN, ENGI_MFMPAR},
        {ENGI_STRING, ENGI_MODAL, ENGI_LYRA, ENGI_ERROR},
        {ENGI_BLOW, ENGI_RESO, ENGI_JUNK, ENGI_GLITCH},
        {ENGI_CHAOS, ENGI_DRIFT, ENGI_PLANT, ENGI_RITUAL},
        {ENGI_LYRA, ENGI_DRIFT, ENGI_RESO, ENGI_STRING},           /* the heaviest state, all four at once */
        {ENGI_MECH, ENGI_BIO, ENGI_MODAL, ENGI_BLOW},
    };
    static const char *const NAMES[] = {"default", "sid_wave_vo_ens", "pulse_ddraw_dyn_par", "string_modal_lyra_error",
                                        "blow_reso_junk_glitch", "chaos_drift_plant_ritual", "heavy",
                                        "mech_bio_modal_blow"};
    int bad = 0;
    uint32_t s, i;
    for (s = 0; s < NELEM(SETS); s++) {
        run_t R = run_pattern(SETS[s], 0xFu, 6.0);
        char what[160];
        int all = 1;
        for (i = 0; i < NPART; i++)
            all &= R.tpeak[i] > 300;
        snprintf(what, sizeof what, "MONOFM steps %-26s peaks %5d %5d %5d %5d mix %5d clip %u dc %.0f", NAMES[s],
                 R.tpeak[0], R.tpeak[1], R.tpeak[2], R.tpeak[3], R.peak, R.clip, R.dc);
        bad += check(what, all && R.clip < 64u && fabs(R.dc) < 300.0 && R.rms > 200.0);
        if (wavs)
            wav_write(wavs, NAMES[s], R.frames);
    }
    {   /* each alone: only it sounds (the others' meters stay at 0) */
        int ok = 1;
        for (i = 0; i < NPART; i++) {
            run_t R = run_pattern(SETS[0], 1u << i, 2.0);
            uint32_t j;
            ok &= R.tpeak[i] > 300;
            for (j = 0; j < NPART; j++)
                ok &= j == i || R.tpeak[j] == 0;
            if (getenv("V"))
                printf("solo %u: %d %d %d %d\n", i, R.tpeak[0], R.tpeak[1], R.tpeak[2], R.tpeak[3]);
        }
        bad += check("MONOFM steps: each track alone sounds, the muted ones do not", ok);
    }
    return bad;
}

/* a lock on step 8 of T1: P_E1 (the engine's second knob) to 5 while step 8 plays, the base value (100) at steps 0, 4
 * and after the lock; the lock is the only motion record */
static int test_plock(void)
{
    int32_t buf[2 * CTL];
    uint32_t k, at8 = 0, at8_ok = 1, else_ok = 1, seen = 0;
    track_t *t = &trk[0];
    ui_power_on();
    song.g[G_BPM] = 120;
    set_engine_of(t, ENGI_STRING);
    apply_preset_to(t, 0);
    for (k = 0; k < 8u; k++)
        mix_block(buf, CTL);
    memset(t->step, 0, sizeof t->step);
    t->p[P_SLEN] = 16;
    for (k = 0; k < 16u; k += 4u) {
        t->step[k].n = 1;
        t->step[k].note[0] = 60;
        t->step[k].time = ST_NOTE;
        t->step[k].vel = 100;
    }
    t->p[P_E1] = 100;
    motion_set_lock(t, 8, P_E1, 5);
    seq_start();
    for (k = 0; k < (uint32_t)(SR * 4u / CTL); k++) {
        mix_block(buf, CTL);
        if (t->seq_idx == 8u) {
            at8++;
            at8_ok &= t->p[P_E1] == 5;
        } else if (t->seq_idx < 8u || t->seq_idx > 8u) {
            seen++;
            else_ok &= t->p[P_E1] == 100;
        }
    }
    seq_stop();
    return check("MONOFM p-lock: step 8 plays its lock, the other steps the base", at8 > 4u && at8_ok && seen > 40u &&
                 else_ok && motion_lock_count(t) == 1u);
}

/* MIDI channels 1..4 play T1..T4 (CH1-4, the default routing) */
static int test_midi_parts(void)
{
    int32_t buf[2 * CTL];
    uint32_t i, k;
    int ok = 1;
    ui_power_on();
    for (k = 0; k < 8u; k++)
        mix_block(buf, CTL);
    for (i = 0; i < NPART; i++) {
        uint32_t on = 0, j;
        midi_event(0x90, i, 60, 100);
        for (k = 0; k < 4u; k++)
            mix_block(buf, CTL);
        for (j = 0; j < NVOICE; j++)
            on += trk[i].v[j].active;
        ok &= on > 0u;
        midi_event(0x80, i, 60, 0);
        for (k = 0; k < 200u; k++)
            mix_block(buf, CTL);
    }
    return check("MONOFM: MIDI channels 1..4 play T1..T4", ok);
}

int main(void)
{
    const char *wavs = getenv("WAVS");
    int bad = 0;
    if (getenv("BENCH")) {                              /* tests/monofm_cpu.sh: one set for BENCH seconds */
        static const uint8_t B[][NPART] = {{ENGI_LYRA, ENGI_DRIFT, ENGI_RESO, ENGI_STRING},
                                           {ENGI_MODAL, ENGI_MODAL, ENGI_MODAL, ENGI_MODAL},
                                           {ENGI_STRING, ENGI_BLOW, ENGI_LYRA, ENGI_CHAOS}};
        run_pattern(B[atoi(getenv("SET") ? getenv("SET") : "0") % 3], 0xFu, atof(getenv("BENCH")));
        return 0;
    }
    bad += test_engines();
    bad += test_midi_parts();
    bad += test_plock();
    bad += test_steps(wavs);
    bad += test_machines(wavs);
    printf(bad ? "monofm: %d FAILED\n" : "monofm: all passed\n", bad);
    return bad != 0;
}
