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


/* EUCLID: LEN 8, 3 hits: steps 0 3 6 play (x..x..x.); an empty hit plays C3 + ROOT, a hit with a note its note, a step
 * off the pattern nothing even with a note; a lock on a hit applies; ROT 1 moves it; SEQ tapped twice toggles it and
 * shows its page */
static uint32_t eu_played[16], eu_note[16];
static int test_euclid(void)
{
    int32_t buf[2 * CTL];
    int bad = 0;
    uint32_t k, last = 0xFFFFu, ok;
    track_t *t = &trk[0];
    ui_power_on();
    song.g[G_BPM] = 120;
    song.sel = 0;
    set_engine_of(t, ENGI_MSINE);
    apply_preset_to(t, 0);
    for (k = 0; k < 8u; k++)
        mix_block(buf, CTL);
    memset(t->step, 0, sizeof t->step);
    t->p[P_SLEN] = 8;
    t->p[P_ROOT] = 2;                                    /* (D: C3 + 2 = 50) */
    t->step[3].n = 1;                                    /* a note on a hit */
    t->step[3].note[0] = 67;
    t->step[3].time = ST_NOTE;
    t->step[4].n = 1;                                    /* a note off the pattern */
    t->step[4].note[0] = 72;
    t->step[4].time = ST_NOTE;
    t->p[P_EUC] = 1;
    t->p[P_EHITS] = 3;
    t->p[P_EROT] = 0;
    motion_set_lock(t, 6, P_E0, 90);                     /* (FOLD locked on step 6) */
    memset(eu_played, 0, sizeof eu_played);
    seq_start();
    for (k = 0; k < (uint32_t)(SR * 4u / CTL); k++) {
        uint32_t j;
        mix_block(buf, CTL);
        if (t->seq_idx != last) {                        /* a step started: what sounds now */
            last = t->seq_idx;
            for (j = 0; j < NVOICE; j++)
                if (t->v[j].active && t->v[j].gate) {
                    eu_played[last % 16u]++;
                    eu_note[last % 16u] = t->v[j].note;
                }
            if (last == 6u && t->p[P_E0] != 90)
                eu_played[15] = 99;                      /* (the lock missing) */
        }
    }
    seq_stop();
    ok = eu_played[0] && eu_played[3] && eu_played[6] && !eu_played[1] && !eu_played[2] && !eu_played[4] &&
         !eu_played[5] && !eu_played[7] && eu_note[0] == 50u && eu_note[3] == 67u && eu_note[6] == 50u && !eu_played[15];
    bad += check("MONOFM EUCLID: 3 of 8 plays steps 0 3 6, the root on empty hits, a hit's own note, its lock", ok);
    t->p[P_EROT] = 1;
    bad += check("MONOFM EUCLID: ROT 1 moves the hits (steps 2 5 7)", !mf_euc_hit(t->p, 0) && mf_euc_hit(t->p, 2) &&
                 mf_euc_hit(t->p, 5) && mf_euc_hit(t->p, 7) && !mf_euc_hit(t->p, 3));
    t->p[P_EHITS] = 0;
    bad += check("MONOFM EUCLID: HITS 0 plays nothing", !mf_euc_hit(t->p, 0) && !mf_euc_hit(t->p, 2));
    t->p[P_EHITS] = 3;
    t->p[P_EUC] = 0;
    frames(600);
    btn_down(B_SEQ);
    frame();
    turn(EN_SELECT, 1);
    btn_up(B_SEQ);
    frames(200);
    ok = t->p[P_EUC] == 1 && !ui.home && cur_page()->graph == GR_EUCLID && !ui.lock;
    btn_down(B_SEQ);
    frame();
    turn(EN_SELECT, -1);
    btn_up(B_SEQ);
    frames(200);
    ok &= t->p[P_EUC] == 0 && cur_page()->graph == GR_EUCLID;
    press(B_SEQ);
    press(B_SEQ);
    frames(200);
    bad += check("MONOFM EUCLID: SEQ + SELECT right on (its page), left off; SEQ tapped twice only pages (no EUCLID, "
                 "no lock)", ok && t->p[P_EUC] == 0 && !ui.lock);
    bad += check("MONOFM EUCLID: its parameters stay with a sound load", (apply_preset_to(t, 1), t->p[P_EHITS] == 3 &&
                 t->p[P_SLEN] == 8));
    return bad;
}

/* HOME tapped twice: XY; KNOB 1 / 2 set X / Y on every track; HOME again: HOME. X drives a route (ARP > X: X -> FOLD) */
static int test_xy(void)
{
    int bad = 0, ok;
    uint32_t i;
    ui_power_on();
    frames(600);
    if (!ui.home)
        press(B_HOME);
    frames(600);
    press(B_HOME);
    press(B_HOME);
    frames(100);
    ok = ui.home == HOME_XY;
    turn(EN_K1, 40);
    turn(EN_K2, 20);
    for (i = 0; i < NPART; i++)
        ok &= trk[i].p[P_XV] > 20 && trk[i].p[P_YV] > 10 && trk[i].p[P_XV] == trk[0].p[P_XV];
    bad += check("MONOFM XY: HOME twice opens it, KNOB 1 / 2 set X / Y on all four tracks", ok);
    press(B_HOME);
    frames(100);
    bad += check("MONOFM XY: HOME again: HOME", ui.home == 1u);
    midi_event(0xB0, 2, 16, 100);
    ok = 1;
    for (i = 0; i < NPART; i++)
        ok &= trk[i].p[P_XV] == 100;
    bad += check("MONOFM XY: CC16 on any channel sets X everywhere; the matrix source reads it", ok &&
                 mod_tsrc(&trk[1], MS_X, 0) == 100 * 258);
    return bad;
}

/* the track FILTER: a saw through LP at a low CUT is much quieter than open; HP at a high CUT too; OFF as before */
static double flt_rms(int32_t ty, int32_t cut)
{
    int32_t buf[2 * CTL];
    uint32_t k, i;
    double sq = 0;
    ui_power_on();
    song.master_q12 = 4096;
    set_engine_of(&trk[0], ENGI_MSAW);
    apply_preset_to(&trk[0], 1);
    trk[0].p[P_DLY] = trk[0].p[P_REV] = trk[0].p[P_CHOR] = 0;
    trk[0].p[P_FLT] = (int16_t)ty;
    trk[0].p[P_FCUT] = (int16_t)cut;
    trk[0].p[P_FRES] = 0;
    for (k = 0; k < 8u; k++)
        mix_block(buf, CTL);
    midi_event(0x90, 0, 45, 100);
    for (k = 0; k < 1000u; k++) {
        mix_block(buf, CTL);
        if (k > 200u)
            for (i = 0; i < CTL; i++)
                sq += (double)buf[2u * i] * buf[2u * i];
    }
    midi_event(0x80, 0, 45, 0);
    return sqrt(sq / (800.0 * CTL));
}
static int test_filter(void)
{
    double off = flt_rms(0, 127), lp = flt_rms(1, 8), lpo = flt_rms(1, 127), hp = flt_rms(3, 110), bp = flt_rms(2, 60);
    char what[160];
    snprintf(what, sizeof what, "MONOFM FILTER: rms OFF %.0f, LP open %.0f, LP 8 %.0f, HP 110 %.0f, BP 60 %.0f", off, lpo, lp,
             hp, bp);
    return check(what, lp < off * 0.4 && fabs(lpo / off - 1.0) < 0.25 && hp < off * 0.6 && bp > 50.0 && bp < off);
}

/* the punch-in's target (FX + D#4: perf_trk): OCT DN held, T1 a saw. Target T2 (silent): T1 untouched, as with no effect;
 * target T1, or the master: T1 through it (the output changes) */
static double punch_rms(int target, int fx, double *diff, int32_t *ref)
{
    int32_t buf[2 * CTL];
    uint32_t k, i;
    double sq = 0, d = 0;
    ui_power_on();
    memset(eng_mem, 0, sizeof eng_mem);
    memset(eng_mem_owner, 0, sizeof eng_mem_owner);
    rng_state = 0x1234567u;
    song.master_q12 = 4096;
    for (i = 0; i < NPART; i++)
        trk[i].p[P_DLY] = trk[i].p[P_REV] = trk[i].p[P_CHOR] = 0;
    set_engine_of(&trk[0], ENGI_MSAW);
    apply_preset_to(&trk[0], 1);
    trk[0].p[P_DLY] = trk[0].p[P_REV] = trk[0].p[P_CHOR] = 0;
    for (k = 0; k < 8u; k++)
        mix_block(buf, CTL);
    perf_trk = (uint8_t)target;
    if (fx)
        perf_press(PF_ODN, 1);
    midi_event(0x90, 0, 45, 100);
    for (k = 0; k < 400u; k++) {
        mix_block(buf, CTL);
        for (i = 0; i < CTL; i++) {
            int32_t x = buf[2u * i];
            if (k > 100u) {
                sq += (double)x * x;
                if (ref)
                    d += fabs((double)x - ref[(k - 101u) * CTL + i]);
            }
        }
    }
    if (fx)
        perf_press(PF_ODN, 0);
    perf_trk = 0;
    if (diff)
        *diff = d;
    return sqrt(sq / (299.0 * CTL));
}
static int32_t punch_ref[300 * CTL];
static int test_punch(void)
{
    int32_t buf[2 * CTL];
    double d2, d1, d0;
    uint32_t k, i, f = 0;
    char what[160];
    /* the reference: no effect */
    ui_power_on();
    memset(eng_mem, 0, sizeof eng_mem);
    memset(eng_mem_owner, 0, sizeof eng_mem_owner);
    rng_state = 0x1234567u;
    song.master_q12 = 4096;
    for (i = 0; i < NPART; i++)
        trk[i].p[P_DLY] = trk[i].p[P_REV] = trk[i].p[P_CHOR] = 0;
    set_engine_of(&trk[0], ENGI_MSAW);
    apply_preset_to(&trk[0], 1);
    trk[0].p[P_DLY] = trk[0].p[P_REV] = trk[0].p[P_CHOR] = 0;
    for (k = 0; k < 8u; k++)
        mix_block(buf, CTL);
    midi_event(0x90, 0, 45, 100);
    for (k = 0; k < 400u; k++) {
        mix_block(buf, CTL);
        if (k > 100u)
            for (i = 0; i < CTL; i++)
                punch_ref[f++] = buf[2u * i];
    }
    punch_rms(2, 1, &d2, punch_ref);
    punch_rms(1, 1, &d1, punch_ref);
    punch_rms(0, 1, &d0, punch_ref);
    snprintf(what, sizeof what, "MONOFM punch-in: OCT DN on T2 leaves T1 (diff %.0f), on T1 (%.0f) and the master (%.0f) not",
             d2 / (299.0 * CTL), d1 / (299.0 * CTL), d0 / (299.0 * CTL));
    return check(what, d2 / (299.0 * CTL) < 10.0 && d1 / (299.0 * CTL) > 200.0 && d0 / (299.0 * CTL) > 200.0);
}


/* STABILITY of the Monomachine machines (29..41): each one fuzzed MF_FUZZ rounds: a random preset, its eight sound
 * parameters (and VOICE GLIDE DETUNE, the envelope, the filters) at their ends or anywhere, chords of 1..6 notes over the
 * whole MIDI range at any velocity, a parameter changed every block or two while they sound, engine switches between
 * the machines with notes held. Fails on: a run of full-scale samples (a blow-up), DC that stays, a voice left active
 * or sound left after every note was let go with a short release, output that never comes back to a preset's own level
 * (state ruined); built with -fsanitize=undefined,address (tests/monofm_stab.sh) it catches overflows and table reads
 * out of range too. Then each preset held MF_LONG seconds: its level at the end against its level after the attack */
static uint32_t st_rng = 0x9E3779B9u;
static uint32_t st_rand(void) { st_rng ^= st_rng << 13; st_rng ^= st_rng >> 17; st_rng ^= st_rng << 5; return st_rng; }
static int16_t st_val(uint32_t e, uint32_t id)
{
    const param_desc_t *d = param_desc_of(e, id);
    uint32_t r = st_rand() % 4u, span = (uint32_t)(d->max - d->min + 1);
    return (int16_t)(r == 0u ? d->min : r == 1u ? d->max : d->min + (int32_t)(st_rand() % span));
}
static const uint8_t ST_IDS[] = {P_E0, P_E0 + 1, P_E0 + 2, P_E0 + 3, P_E0 + 4, P_E0 + 5, P_E0 + 6, P_E0 + 7, P_E8,
                                 P_FATK, P_FDEC, P_DIST, P_DTYPE, P_DTONE, P_DLCUT, P_DOUT, P_VOICE, P_GLIDE, P_DETUNE, P_ATK, P_DEC, P_SUS, P_REL, P_FLT, P_FCUT, P_FRES,
                                 P_HBASE, P_HWIDTH, P_HRES, P_ED_FLT, P_LEVEL};
static double st_level(uint32_t e, uint32_t preset, double secs, double *late, int32_t *peak)
{
    int32_t buf[2 * CTL];
    uint32_t f = 0, n = (uint32_t)(SR * secs), i;
    double a = 0, b = 0;
    *peak = 0;
    for (i = 0; i < 16u; i++)                          /* (the engine switched in: engine_block) */
        mix_block(buf, CTL);
    midi_event(0x90, 0, 48, 100);
    while (f < n) {
        mix_block(buf, CTL);
        for (i = 0; i < CTL; i++, f++) {
            double x = buf[2u * i];
            int32_t ax = buf[2u * i] < 0 ? -buf[2u * i] : buf[2u * i];
            *peak = ax > *peak ? ax : *peak;
            if (f >= SR / 5u && f < SR / 5u + SR / 2u)
                a += x * x;
            if (f >= n - SR / 2u)
                b += x * x;
        }
    }
    midi_event(0x80, 0, 48, 0);
    (void)e; (void)preset;
    *late = sqrt(b / (SR / 2u));
    return sqrt(a / (SR / 2u));
}
static void st_reset(void)
{
    ui_power_on();
    memset(eng_mem, 0, sizeof eng_mem);
    memset(eng_mem_owner, 0, sizeof eng_mem_owner);
    memset(dly_buf, 0, sizeof dly_buf);
    rev_clear();
    song.master_q12 = 4096;
    song.sel = 0;
}
static int test_stability(void)
{
    int bad = 0;
    uint32_t e, rounds = getenv("MF_FUZZ") ? (uint32_t)atoi(getenv("MF_FUZZ")) : 40u;
    double long_s = getenv("MF_LONG") ? atof(getenv("MF_LONG")) : 20.0;
    for (e = ENGI_MSAW; e < NENGINES; e++) {
        int32_t buf[2 * CTL];
        uint32_t r, run = 0, worst_run = 0, blocks = 0, clips = 0, stuck = 0, ruined = 0;
        double worst_dc = 0, worst_tail = 0;
        char what[200];
        int16_t fresh[P_COUNT];
        st_reset();
        set_engine_of(&trk[0], e);
        apply_preset_to(&trk[0], 0);
        memcpy(fresh, trk[0].p, sizeof fresh);
        for (r = 0; r < rounds; r++) {
            uint32_t nn = 1u + st_rand() % 6u, notes[6], i, b, len = 4u + st_rand() % 120u, k;
            double sum = 0, tail = 0;
            uint32_t cnt = 0;
            if (st_rand() % 8u == 0u) {                  /* another machine with notes held, then back */
                set_engine_of(&trk[0], ENGI_MSAW + st_rand() % (NENGINES - ENGI_MSAW));
                midi_event(0x90, 0, 60, 90);
                for (b = 0; b < 20u; b++) mix_block(buf, CTL);
                set_engine_of(&trk[0], e);
                midi_event(0x80, 0, 60, 0);
            }
            apply_preset_to(&trk[0], st_rand() % ENGINES[e]->npresets);
            for (i = 0; i < NELEM(ST_IDS); i++)
                if (st_rand() % 2u)
                    trk[0].p[ST_IDS[i]] = st_val(e, ST_IDS[i]);
            trk[0].p[P_DLY] = trk[0].p[P_REV] = trk[0].p[P_CHOR] = 0;   /* (the machine dry: the master's tails aside) */
            for (i = 0; i < nn; i++) {
                notes[i] = st_rand() % 128u;
                midi_event(0x90, 0, notes[i], 1u + st_rand() % 127u);
            }
            for (b = 0; b < len; b++) {
                if (st_rand() % 2u) {
                    uint32_t id = ST_IDS[st_rand() % NELEM(ST_IDS)];
                    trk[0].p[id] = st_val(e, id);
                }
                if (st_rand() % 16u == 0u)
                    midi_event(0x90, 0, st_rand() % 128u, 1u + st_rand() % 127u);
                mix_block(buf, CTL);
                blocks++;
                for (k = 0; k < CTL; k++) {
                    int32_t l = buf[2u * k], a = l < 0 ? -l : l;
                    if (a >= 32700) { clips++; run++; worst_run = run > worst_run ? run : worst_run; } else run = 0;
                    sum += l;
                    cnt++;
                }
            }
            for (i = 0; i < 128u; i++)                   /* every note let go, a short release, FX dry */
                midi_event(0x80, 0, i, 0);
            trk[0].p[P_REL] = 0;
            trk[0].p[P_DLY] = trk[0].p[P_REV] = trk[0].p[P_CHOR] = 0;
            trk[0].p[P_ED_FLT] = 0;
            sum = 0;
            for (b = 0; b < (uint32_t)(SR * 2u / CTL); b++) {
                mix_block(buf, CTL);
                if (b >= (uint32_t)(SR * 3u / 2u / CTL))
                    for (k = 0; k < CTL; k++) {
                        tail += (double)buf[2u * k] * buf[2u * k];
                        sum += buf[2u * k];
                    }
            }
            tail = sqrt(tail / (SR / 2u));
            sum = fabs(sum / (SR / 2u));                 /* (the DC left once everything is quiet) */
            worst_dc = sum > worst_dc ? sum : worst_dc;
            (void)cnt;
            worst_tail = tail > worst_tail ? tail : worst_tail;
            for (i = 0; i < NVOICE; i++)
                stuck += trk[0].v[i].active;
            dly_buf[0] = dly_buf[0];
            memset(dly_buf, 0, sizeof dly_buf);          /* (the reverb / delay tails are the master's, not the machine's) */
            rev_clear();
        }
        {   /* after all that: preset 0 sounds as from a fresh start */
            double l0, l1, late;
            int32_t pk;
            memcpy(trk[0].p, fresh, sizeof fresh);
            trk[0].p[P_DLY] = trk[0].p[P_REV] = trk[0].p[P_CHOR] = 0;
            l1 = st_level(e, 0, 1.0, &late, &pk);
            st_reset();
            set_engine_of(&trk[0], e);
            apply_preset_to(&trk[0], 0);
            trk[0].p[P_DLY] = trk[0].p[P_REV] = trk[0].p[P_CHOR] = 0;
            l0 = st_level(e, 0, 1.0, &late, &pk);
            ruined = l0 > 50.0 && (l1 < 0.5 * l0 || l1 > 2.0 * l0);
            snprintf(what, sizeof what, "MONOFM STABLE %-6s %u rounds: clip run %4u (%5.2f %%) dc %5.0f tail %4.0f stuck %u, "
                     "fresh %5.0f after %5.0f", ENGINES[e]->name, rounds, worst_run, 100.0 * clips / (blocks * CTL + 1u),
                     worst_dc, worst_tail, stuck, l0, l1);
            bad += check(what, worst_run < 2000u && worst_dc < 20.0 && worst_tail < 30.0 && !stuck && !ruined);
        }
    }
    for (e = ENGI_MSAW; long_s > 0 && e < NENGINES; e++) {   /* each preset held long_s seconds */
        uint32_t k;
        for (k = 0; k < ENGINES[e]->npresets; k++) {
            double a, late;
            int32_t pk;
            char what[160];
            const preset_t *pr = &ENGINES[e]->presets[k];
            int sustains = pr->env[2] > 40u && e != ENGI_MBBOX;   /* (BBOX: drum hits, they end) */
            st_reset();
            set_engine_of(&trk[0], e);
            apply_preset_to(&trk[0], k);
            trk[0].p[P_DLY] = trk[0].p[P_REV] = trk[0].p[P_CHOR] = 0;
            a = st_level(e, k, long_s, &late, &pk);
            snprintf(what, sizeof what, "MONOFM LONG %-6s %-12s %4.0f s: rms after the attack %5.0f, at the end %5.0f, peak %5d",
                     ENGINES[e]->name, pr->name, long_s, a, late, pk);
            bad += check(what, pk < 32700 && (!sustains || a < 30.0 || (late > 0.3 * a && late < 2.5 * a)) && late < 3.0 * a + 50.0);
        }
    }
    return bad;
}


static void sc_pattern(track_t *t, uint8_t note, uint32_t len)
{
    uint32_t i;
    memset(t->step, 0, sizeof t->step);
    for (i = 0; i < NSTEP; i++)
        t->step[i].time = ST_REST;
    for (i = 0; i < len; i += 2u) {
        t->step[i].time = ST_NOTE;
        t->step[i].n = 1;
        t->step[i].note[0] = note;
        t->step[i].vel = 100;
    }
    t->p[P_SLEN] = (int16_t)len;
}

/* THE PATTERNS (mf_pat.c, Elektron's model): 16 in RAM, an edit kept when another one plays; the three change modes
 * (SEQUENTIAL at the master length's end, DIRECT START, DIRECT JUMP), chains, SEQ + ALGORITHM (asked when SEQ is let
 * go) and OCT+ chaining; SAVE / RELOAD and a power cut in a SAVE; the autosave's working copy; the trig conditions; the
 * track SCALE; the actions (COPY / PASTE, TRN, SHIFT); the mutes the project's; FILL on GLO + F5 */
static void tp_blocks(uint32_t n) { int32_t buf[2 * CTL]; while (n--) mix_block(buf, CTL); }
static int tp_wait_switch(uint32_t max, uint32_t want, int *at_wrap)   /* play until pattern `want` is in; *at_wrap: T1 at 0 */
{
    int32_t buf[2 * CTL];
    uint32_t k;
    for (k = 0; k < max; k++) {
        mix_block(buf, CTL);
        if (mfp.cur == want) {
            mf_pat_poll();
            if (at_wrap)
                *at_wrap = trk[0].seq_idx == 0u;
            return 1;
        }
    }
    return 0;
}
static void tp_fresh(void)
{
    ui_power_on();
    memset(mfp_nor, 0xFF, sizeof mfp_nor);
    mfp_nor_cut = 0xFFFFFFFFu;
    mf_pat_boot();
    mfp.mode = MFP_SEQ;
    mfp.nchain = 0;
}
static int test_mfpat(void)
{
    int bad = 0, ok, wrap;
    uint32_t k, i;
    tp_fresh();
    ok = mfp.cur == 0u && mfp.next == MFP_NONE && !mfp_has_trigs(0) && !mfp_has_trigs(15);
    bad += check("MONOFM PATTERNS: power-on with nothing saved: A01, 16 empty patterns", ok);
    /* A01: SAW, notes 48 every other step; A02: SINE, 60, LEN 8 */
    set_engine_of(&trk[0], ENGI_MSAW);
    trk[0].engine = trk[0].eng_req;
    sc_pattern(&trk[0], 48, 16);
    trk[0].p[P_E0] = 11;
    mf_pat_ask(1);
    ok = mfp.cur == 1u && !step_on(&trk[0].step[0]);
    set_engine_of(&trk[0], ENGI_MSINE);
    sc_pattern(&trk[0], 60, 8);
    trk[0].p[P_E0] = 22;
    mf_pat_ask(0);
    ok &= mfp.cur == 0u && trk[0].eng_req == ENGI_MSAW && trk[0].p[P_E0] == 11 && trk[0].step[0].note[0] == 48u &&
          trk[0].p[P_SLEN] == 16;
    mf_pat_ask(1);
    ok &= trk[0].eng_req == ENGI_MSINE && trk[0].step[0].note[0] == 60u && trk[0].p[P_SLEN] == 8;
    bad += check("MONOFM PATTERNS: stopped, a pattern comes at once; each keeps its edits (sound, trigs, LEN) unsaved", ok);
    /* SEQUENTIAL: A02 playing (AUTO length: T1's 8 steps), A01 asked: in at the end, every track from step 1 */
    mf_pat_ask(1);
    trk[1].p[P_MUTE] = 1;
    seq_start();
    tp_blocks(30);
    mf_pat_ask(0);
    ok = mfp.next == 0u && mfp.cur == 1u;
    ok &= tp_wait_switch(20000u, 0u, &wrap) && wrap && trk[0].eng_req == ENGI_MSAW && trk[1].p[P_MUTE] == 1 && song.playing;
    bad += check("MONOFM PATTERNS: SEQUENTIAL: asked while playing, it comes at the pattern's end, from step 1; MUTE kept", ok);
    {   /* its notes play */
        uint32_t seen = 0;
        int32_t buf[2 * CTL];
        for (k = 0; k < 600u; k++) {
            mix_block(buf, CTL);
            for (i = 0; i < NVOICE; i++)
                seen |= trk[0].v[i].active && trk[0].v[i].note == 48u;
        }
        bad += check("MONOFM PATTERNS: .. and its trigs sound", seen != 0u);
    }
    /* DIRECT START / JUMP */
    tp_blocks(37);
    mfp.mode = MFP_START;
    mf_pat_ask(1);
    tp_blocks(1);
    ok = mfp.cur == 1u;
    tp_blocks(1);
    ok &= trk[0].seq_idx == 0u;
    bad += check("MONOFM PATTERNS: DIRECT START: at once, from step 1", ok);
    tp_blocks(300);
    {
        uint32_t idx;
        mfp.mode = MFP_JUMP;
        idx = trk[0].seq_idx;
        mf_pat_ask(0);
        tp_blocks(1);
        ok = mfp.cur == 0u && trk[0].seq_idx >= idx && trk[0].seq_idx <= idx + 1u;
        bad += check("MONOFM PATTERNS: DIRECT JUMP: at once, the tracks where they were", ok);
    }
    /* a chain A01 > A02 > A03 (A03 empty): in turn, at each end */
    mfp.mode = MFP_SEQ;
    {
        static const uint8_t C[3] = {0, 1, 2};
        mf_pat_chain(C, 3);
        ok = tp_wait_switch(40000u, 1u, &wrap) && wrap;
        ok &= tp_wait_switch(40000u, 2u, 0);
        ok &= tp_wait_switch(40000u, 0u, 0);
        ok &= tp_wait_switch(40000u, 1u, 0);
        bad += check("MONOFM PATTERNS: a chain of three plays them in turn, each at its pattern's end, round", ok);
        mf_chain_stop();
    }
    /* SEQ held + ALGORITHM: picked, asked when SEQ is let go; OCT+ builds a chain */
    seq_stop();
    stop_transport();
    tp_blocks(4);
    mf_pat_ask(0);
    frames(600);
    btn_down(B_SEQ);
    frames(50);
    turn(EN_ALGO, 1);
    turn(EN_ALGO, 1);
    ok = mfp.cur == 0u;                                 /* (not yet: SEQ still held) */
    btn_up(B_SEQ);
    frames(100);
    ok &= mfp.cur == 2u;
    bad += check("MONOFM PATTERNS: SEQ + ALGORITHM picks (A03), it comes when SEQ is let go", ok);
    btn_down(B_SEQ);
    frames(50);
    turn(EN_ALGO, -1);
    turn(EN_ALGO, -1);                                  /* A01 (one pattern a turn) */
    press(B_OCTUP);
    turn(EN_ALGO, 1);                                   /* A02 */
    press(B_OCTUP);
    btn_up(B_SEQ);
    frames(100);
    ok = mfp.cur == 0u && mfp.nchain == 2u && mfp.chain[0] == 0u && mfp.chain[1] == 1u;
    bad += check("MONOFM PATTERNS: SEQ + ALGORITHM + OCT+ twice: a chain A01 > A02", ok);
    mf_chain_stop();
    /* SAVE, an edit, RELOAD; a SAVE cut by a power loss keeps the copy before; power-on loads the newest */
    mf_pat_ask(0);
    ok = mf_pat_unsaved();
    mf_pat_save();
    ok &= !mf_pat_unsaved() && mfp_best(0) != 0u;
    trk[0].step[0].note[0] = 70;
    ok &= mf_pat_unsaved();
    mf_pat_reload(0);
    ok &= trk[0].step[0].note[0] == 48u;
    trk[0].step[0].note[0] = 71;
    mf_pat_reload(MFP_NONE);
    ok &= trk[0].step[0].note[0] == 48u && mfp.cur == 0u;
    bad += check("MONOFM PATTERNS: SAVE (stopped), RELOAD the pattern, RELOAD all: as saved", ok);
    trk[0].step[0].note[0] = 72;
    mfp_nor_cut = 3000u;                                /* (the power goes 3000 bytes into the next SAVE) */
    mf_pat_save();
    mfp_nor_cut = 0xFFFFFFFFu;
    mf_pat_reload(MFP_NONE);
    ok = trk[0].step[0].note[0] == 48u;
    trk[0].step[0].note[0] = 73;
    mf_pat_save();
    ui_power_on();
    mf_pat_boot();
    ok &= trk[0].step[0].note[0] == 73u && trk[0].eng_req == ENGI_MSAW && mfp_at(1)->t[0].eng == ENGI_MSINE;
    bad += check("MONOFM PATTERNS: a SAVE cut off keeps the one before; power-on loads the newest (every pattern)", ok);
    /* SAVE while playing: at STOP */
    seq_start();
    trk[0].step[0].note[0] = 74;
    mf_pat_save();
    ok = mfp.save_wait == 1u && mf_pat_unsaved();
    seq_stop();
    stop_transport();
    mf_pat_poll();
    ok &= !mfp.save_wait && !mf_pat_unsaved();
    bad += check("MONOFM PATTERNS: SAVE while playing: written at STOP", ok);
    /* the autosave: stopped, quiet, a while after a change: the working copy (W), loaded at power-on */
    trk[0].step[0].note[0] = 75;
    for (k = 0; k < 3000u && mfp_best(1) != MFP_W; k++) {
        fm1_ms += 16u;
        mf_pat_poll();
    }
    ui_power_on();
    mf_pat_boot();
    bad += check("MONOFM PATTERNS: the autosave (stopped, idle) keeps an unsaved edit over a power-off; SAVE still sees it",
                 trk[0].step[0].note[0] == 75u && mf_pat_unsaved());
    /* the conditions */
    {
        uint32_t r[8], j;
        memset(mfp_loop, 0, sizeof mfp_loop);
        for (j = 0; j < 8u; j++) {
            mfp_loop[0] = (uint16_t)(j + 1u);
            r[j] = (uint32_t)mf_trig_plays(0, MF_COND0 + MC_AB + 0u);   /* 1:2 */
        }
        ok = r[0] && !r[1] && r[2] && !r[3];
        for (j = 0; j < 8u; j++) {
            mfp_loop[0] = (uint16_t)(j + 1u);
            r[j] = (uint32_t)mf_trig_plays(0, MF_COND0 + MC_AB + 4u);   /* 3:3 (1:2 2:2 1:3 2:3 3:3) */
        }
        ok &= !r[0] && !r[1] && r[2] && !r[3] && !r[4] && r[5];
        mfp_loop[0] = 1;
        ok &= mf_trig_plays(0, MF_COND0 + MC_1ST) && !mf_trig_plays(0, MF_COND0 + MC_N1ST);
        mfp_loop[0] = 2;
        ok &= !mf_trig_plays(0, MF_COND0 + MC_1ST);
        mfp_fill = 1;
        ok &= mf_trig_plays(0, MF_COND0 + MC_FILL) && !mf_trig_plays(0, MF_COND0 + MC_NFILL);
        mfp_fill = 0;
        ok &= !mf_trig_plays(0, MF_COND0 + MC_FILL) && mf_trig_plays(0, MF_COND0 + MC_NFILL);
        mfp_pre[0] = 1;                                  /* (the last one played) */
        ok &= mf_trig_plays(0, MF_COND0 + MC_PRE) && !mf_trig_plays(0, MF_COND0 + MC_NPRE) && mf_trig_plays(1, MF_COND0 + MC_NEI);
        ok &= mf_trig_plays(0, 0) && !mf_trig_plays(0, 101u);
        {
            char nb[8];
            mf_cond_name(MF_COND0 + MC_AB + 4u, nb);
            ok &= str_eq(nb, "3:3");
            mf_cond_name(50u, nb);
            ok &= str_eq(nb, "50%");
        }
        bad += check("MONOFM CONDITIONS: A:B (1:2, 3:3), 1ST, FILL / !FILL, PRE / !PRE, NEI, 100% / 0%, their names", ok);
    }
    {   /* the knob's walk: 100% then the conditions, back */
        uint32_t v = 0;
        v = mf_cond_step(v, 1);
        ok = v == MF_COND0;
        v = mf_cond_step(v, -1);
        ok &= v == 0u;
        v = mf_cond_step(v, -50);
        ok &= v == 50u;
        bad += check("MONOFM CONDITIONS: KNOB 4's walk: 50% .. 100%, then 1ST !1ST PRE .. A:B", ok);
    }
    /* a 1:2 trig plays on every other loop of its track, while playing */
    {
        uint32_t plays = 0, loops = 0, last = 0;
        int32_t buf[2 * CTL];
        tp_fresh();
        set_engine_of(&trk[0], ENGI_MSINE);
        tp_blocks(16);
        sc_pattern(&trk[0], 60, 4);                     /* (steps 0 and 2) */
        trk[0].step[2].time = ST_REST;
        trk[0].step[2].n = 0;
        trk[0].step[0].probability = (uint8_t)(MF_COND0 + MC_AB + 0u);   /* 1:2 */
        trk[0].p[P_REL] = 0;
        seq_start();
        for (k = 0; k < 6000u && loops < 8u; k++) {
            mix_block(buf, CTL);
            if (trk[0].seq_idx == 0u && last != 0u) {
                loops++;
                tp_blocks(4);
                for (i = 0; i < NVOICE; i++)
                    if (trk[0].v[i].active && trk[0].v[i].gate) {
                        plays++;
                        break;
                    }
            }
            last = trk[0].seq_idx;
        }
        seq_stop();
        stop_transport();
        bad += check("MONOFM CONDITIONS: a 1:2 trig sounds on half of its track's loops", loops >= 8u && plays >= 3u && plays <= 5u);
    }
    /* the SCALE: 2X twice as fast as 1X */
    {
        track_t *t = &trk[0];
        uint32_t a, b;
        t->p[P_SDIV] = 2;
        a = div_samples(seq_div(t));
        t->p[P_SDIV] = 0;
        b = div_samples(seq_div(t));
        t->p[P_SDIV] = 6;
        ok = a == 2u * b && div_samples(seq_div(t)) / 8u >= a - 1u && div_samples(seq_div(t)) / 8u <= a + 1u && str_eq(TP[P_SDIV].names[0], "2X") && TP[P_SLEN].max == 32;
        t->p[P_SDIV] = 2;
        bad += check("MONOFM SCALE: 2X a step of 1/32, 1X 1/16, 1/8X 1/2; LEN up to 32", ok);
    }
    /* the actions: TRN +, SHIFT >, COPY / PASTE the track, COPY / PASTE the pattern */
    {
        track_t *t = &trk[0];
        tp_fresh();
        song.sel = 0;
        sc_pattern(t, 60, 16);
        mf_pat_action(MA_TUP);
        ok = t->step[0].note[0] == 61u;
        mf_pat_action(MA_RIGHT);
        ok &= !step_on(&t->step[0]) && t->step[1].note[0] == 61u;
        mf_pat_action(MA_CTRK);
        song.sel = 2;
        mf_pat_action(MA_PTRK);
        ok &= trk[2].step[1].note[0] == 61u;
        mf_pat_action(MA_CPAT);
        mf_pat_ask(5);
        ok &= !step_on(&trk[0].step[1]);
        mf_pat_action(MA_PPAT);
        ok &= trk[0].step[1].note[0] == 61u && trk[2].step[1].note[0] == 61u && mfp.cur == 5u;
        song.sel = 0;
        bad += check("MONOFM ACTIONS: TRN +, SHIFT >, COPY / PASTE a track, COPY / PASTE a pattern (A01 -> A06)", ok);
    }
    /* a trig's LEN: 4 steps sounds over the empty steps after it; 1/2 ends inside its step; TRK as the track's GATE */
    {
        int32_t buf[2 * CTL];
        uint32_t on_at[16], j;
        track_t *t = &trk[0];
        tp_fresh();
        set_engine_of(t, ENGI_MSINE);
        tp_blocks(16);
        for (j = 0; j < NSTEP; j++) { t->step[j].time = ST_REST; t->step[j].n = 0; t->step[j].acc = 0; }
        t->p[P_SLEN] = 16;
        t->p[P_REL] = 0;
        t->step[0].time = ST_NOTE; t->step[0].n = 1; t->step[0].note[0] = 60; t->step[0].acc = 9;   /* LEN 4 */
        t->step[8].time = ST_NOTE; t->step[8].n = 1; t->step[8].note[0] = 60; t->step[8].acc = 3;   /* LEN 1/2 */
        memset(on_at, 0, sizeof on_at);
        seq_start();
        for (k = 0; k < 4000u; k++) {
            uint32_t idx = t->seq_idx % 16u, half = t->seq_pos < div_samples(seq_div(t)) / 2u;
            mix_block(buf, CTL);
            for (i = 0; i < NVOICE; i++)
                if (t->v[i].active && t->v[i].gate)
                    on_at[idx] |= half ? 1u : 2u;
            if (k > 10u && t->seq_idx == 15u)
                break;
        }
        seq_stop();
        stop_transport();
        ok = on_at[0] && on_at[1] && on_at[2] && on_at[3] && !(on_at[4] & 2u) && !on_at[5] && (on_at[8] & 1u) && !(on_at[8] & 2u) && !on_at[9];
        bad += check("MONOFM LEN: a trig of LEN 4 sounds over the empty steps after it, LEN 1/2 half its step", ok);
    }
    /* REC never starts the transport */
    tp_fresh();
    frames(600);
    song.rec = 0;
    press(B_REC);
    frames(100);
    bad += check("MONOFM REC: a tap arms the track, the transport stays stopped", (song.rec & 1u) && !song.playing && transport_req != 1u);
    press(B_REC);
    frames(100);
    /* TRN / SHIFT so far, kept with the pattern */
    {
        track_t *t = &trk[0];
        song.sel = 0;
        sc_pattern(t, 60, 16);
        mf_pat_action(MA_TUP);
        mf_pat_action(MA_TUP);
        mf_pat_action(MA_TUP);
        mf_pat_action(MA_TDN);
        mf_pat_action(MA_RIGHT);
        mf_pat_action(MA_RIGHT);
        ok = mf_pat_trn(0) == 2 && mf_pat_shift(0) == 2 && msg_is("SHIFT +2");
        mf_pat_ask(3);
        ok &= mf_pat_trn(0) == 0;
        mf_pat_ask(0);
        ok &= mf_pat_trn(0) == 2 && mf_pat_shift(0) == 2;
        mf_pat_action(MA_CLEAR);
        ok &= mf_pat_trn(0) == 0 && mf_pat_shift(0) == 0;
        bad += check("MONOFM ACTIONS: TRN / SHIFT counted (+2, +2), said, kept with the pattern, CLEAR zeroes them", ok);
    }
    /* fewer pages: a kit machine's EDIT is EDIT 1, EDIT 2, KIT (SPREAD there), VOICE; others EDIT 1 2, VOICE, VOICE 2 (SPREAD);
     * no AUTOMATION; SEQ: STEP, EUCLID, DETAIL */
    {
        uint32_t n = 0, kit = 0, sprd = 0, aut = 0, seq[4], ns = 0;
        set_engine_of(&trk[0], ENGI_GLITCH);
        frames(50);
        for (i = 0; i < NPAGES; i++) {
            if (PAGES[i].fam == FAM_EDIT && page_visible(i)) {
                n++;
                kit |= str_eq(PAGES[i].title, "KIT");
                for (k = 0; k < 4u; k++)
                    sprd |= PAGES[i].id[k] == P_SPRD && str_eq(PAGES[i].title, "KIT");
            }
            aut |= PAGES[i].graph == GR_EVENTS && page_visible(i);
            if (PAGES[i].fam == FAM_SEQ && page_visible(i) && ns < 4u)
                seq[ns++] = i;
        }
        ok = n == 3u && kit && sprd && !aut && ns == 2u && PAGES[seq[0]].graph == GR_ROLL && PAGES[seq[1]].graph == GR_EUCLID;
        set_engine_of(&trk[0], ENGI_MSAW);
        frames(50);
        for (n = 0, sprd = 0, i = 0; i < NPAGES; i++)
            if (PAGES[i].fam == FAM_EDIT && page_visible(i)) {
                n++;
                for (k = 0; k < 4u; k++)
                    sprd |= PAGES[i].id[k] == P_SPRD;
            }
        ok &= n == 3u && sprd;
        bad += check("MONOFM PAGES: EDIT 3 pages (GLITCH: SPREAD on KIT), no AUTOMATION, SEQ: STEP EUCLID", ok);
    }
    /* a project saved by the version before (8-byte steps) loads */
    {
        static mfp_pat1_t o;
        mfp_hdr_t h;
        uint32_t c = 0xFFFFFFFFu, j;
        tp_fresh();
        memset(&o, 0, sizeof o);
        o.t[0].eng = ENGI_MSINE;
        for (j = 0; j < MFP_STEPS; j++)
            o.t[0].step[j].nt = (uint8_t)(ST_REST << 3);
        o.t[0].step[0].nt = 1u;                         /* (n 1, NOTE) */
        o.t[0].step[0].note[0] = 66;
        o.t[0].p[P_SLEN] = 16;
        memset(mfp_nor, 0xFF, sizeof mfp_nor);
        for (j = 0; j < MFP_N; j++) {
            memcpy(mfp_nor_at(MFP_A + 64u + j * sizeof o), &o, sizeof o);
            c = mfp_crc_add(c, &o, sizeof o);
        }
        memset(&h, 0, sizeof h);
        h.magic = MFP_MAGIC; h.version = 1; h.seq = 5; h.size = sizeof o; h.npat = MFP_N; h.crc = ~c;
        h.hcrc = ~mfp_crc_add(0xFFFFFFFFu, &h, sizeof h - 4u);
        memcpy(mfp_nor_at(MFP_A), &h, sizeof h);
        ui_power_on();
        mf_pat_boot();
        bad += check("MONOFM PATTERNS: a project of the version before loads (its trigs, LEN as the track's GATE)",
                     trk[0].eng_req == ENGI_MSINE && trk[0].step[0].note[0] == 66u && step_on(&trk[0].step[0]) && !trk[0].step[0].acc);
    }
    /* FILL: GLO held + F5 held */
    frames(600);
    btn_down(B_GLO);
    frames(50);
    key_down(key_at(0, 14));
    frames(50);
    ok = mfp_fill == 1u;
    key_up(key_at(0, 14));
    frames(50);
    btn_up(B_GLO);
    frames(100);
    bad += check("MONOFM FILL: on while GLO + F5 are held, off after", ok && mfp_fill == 0u);
    return bad;
}

/* SEQ > STEP's KEYS editor (the default): the white keys the page's steps (a tap a trig on / off in the last note, held
 * past MF_PICK_MS only picked, held + a knob a lock), silent; KNOB 1..4 the cursor step's NOTE VEL LEN CHANCE;
 * SEQ + ARP: the keys play and write the step's notes (a chord), again: the steps; MENU STEP EDIT ROLL: the old roll.
 * The LEDs: playing, on any page, the trigs and the running light; stopped with REC held, the trigs */
static void ke_step_page(void)
{
    uint32_t i;
    for (i = 0; i < NPAGES; i++)
        if (PAGES[i].graph == GR_ROLL && str_eq(PAGES[i].title, "STEP"))
            break;
    ui.home = 0;
    ui.page = (uint8_t)i;
    page_entered();
    frame();
}
static uint32_t ke_voices(const track_t *t)
{
    uint32_t j, n = 0;
    for (j = 0; j < NVOICE; j++)
        n += t->v[j].active && t->v[j].gate;
    return n;
}
static int test_keys_editor(void)
{
    int bad = 0, ok;
    track_t *t;
    int32_t buf[2 * CTL];
    uint32_t k, i, k4 = 0, kk;
    ui_power_on();
    t = &trk[0];
    song.sel = 0;
    set_engine_of(t, ENGI_MSAW);
    frames(100);
    sc_pattern(t, 48, 16);
    for (i = 0; i < NSTEP; i++) { t->step[i].time = ST_REST; t->step[i].n = 0; }
    ke_step_page();
    last_note = 55;
    tap_k(key_at(0, 2));
    for (k = 0; k < 4u; k++) mix_block(buf, CTL);
    ok = grid_on() && step_on(&t->step[2]) && t->step[2].note[0] == (uint32_t)(48 + t->p[P_ROOT]) && ui.cursor == 2u && !ke_voices(t);
    bad += check("MONOFM KEYS: STEP's white key 3 tapped: a trig on step 3 in C3 + ROOT (not the last note), the cursor there, silent", ok);
    tap_k(key_at(0, 2));
    bad += check("MONOFM KEYS: tapped again: the trig goes", !step_on(&t->step[2]));
    tap_k(key_at(0, 4));
    key_down(key_at(0, 4));
    frames(500);
    key_up(key_at(0, 4));
    frame();
    bad += check("MONOFM KEYS: a trig's key held past 350 ms only picks it (it stays, the cursor there)",
                 step_on(&t->step[4]) && ui.cursor == 4u);
    {
        uint32_t n0 = t->step[4].note[0];
        key_down(key_at(0, 4));
        frame();
        turn(EN_PRESET, 1);
        turn(EN_PRESET, 1);
        turn(EN_PRESET, 1);
        turn(EN_K1, 1);
        turn(EN_K3, 1);
        turn(EN_K3, 1);
        key_up(key_at(0, 4));
        frame();
        bad += check("MONOFM KEYS: a trig held + PRESETS its NOTE (+3), KNOB 1 ACC, KNOB 3 its LEN (1/4); the trig stays, no lock",
                     t->step[4].note[0] == n0 + 3u && t->step[4].acc == 2u && (t->step[4].flags & SF_ACCENT) &&
                     step_on(&t->step[4]) && motion_lock_count(t) == 0u);
        t->step[4].note[0] = (uint8_t)n0;
        t->step[4].acc = 0;
        t->step[4].flags = 0;
    }
    key_down(key_at(0, 4));
    frame();
    press(B_ENV);
    ok = mf_plk && PAGES[ui.plk_src - 1u].fam == FAM_ENV && cur_page()->graph == GR_ROLL;
    key_up(key_at(0, 4));
    frame();
    frames(200);
    bad += check("MONOFM KEYS: a trig held + ENV: ENV's page locks, STEP stays, the trig too", ok && cur_page()->graph == GR_ROLL &&
                 step_on(&t->step[4]));
    ui.cursor = 4;
    frames(600);
    k4 = t->step[4].note[0];
    turn(EN_PRESET, 1);
    turn(EN_PRESET, 1);
    ok = t->step[4].note[0] == k4 + 2u;
    turn(EN_K1, 1);
    turn(EN_K1, 1);
    ok &= (t->step[4].flags & SF_SLIDE) && !(t->step[4].flags & SF_ACCENT);
    turn(EN_K2, -10);
    ok &= t->step[4].vel && t->step[4].vel < 96u;
    for (k = 0; k < 5u; k++)
        turn(EN_K3, 1);
    ok &= t->step[4].acc == 5u;                         /* (TRK 1/8 1/4 1/2 3/4 1) */
    turn(EN_K4, -5);
    ok &= step_chance(&t->step[4]) < 100u;
    bad += check("MONOFM KEYS: the trig picked: PRESETS NOTE (+2), KNOB 1 FLAG (SLD), 2 VEL, 3 LEN (1), 4 COND", ok);
    /* SEQ + ARP: the keyboard */
    btn_down(B_SEQ);
    frame();
    press(B_ARP);
    btn_up(B_SEQ);
    frames(100);
    ok = mf_kbd && !grid_on() && cur_page()->graph == GR_ROLL;
    t->p[P_VOICE] = V_POLY;                             /* (MONO / LEGATO: the last note alone) */
    kk = key_at(0, 7);                                  /* (C4 +) */
    key_down(kk);
    frame();
    for (k = 0; k < 4u; k++) mix_block(buf, CTL);
    ok &= ke_voices(t) > 0u;
    k4 = kb_note[kk];
    key_down(key_at(0, 9));
    frame();
    key_up(kk);
    key_up(key_at(0, 9));
    frame();
    ok &= t->step[4].n == 2u && t->step[4].note[0] == k4 && ui.cursor == 4u;
    bad += check("MONOFM KEYS: SEQ + ARP: the keys play and write the cursor step (two held: a chord of 2)", ok);
    key_down(kk);
    frame();
    key_up(kk);
    frame();
    bad += check("MONOFM KEYS: a new press replaces the step's notes", t->step[4].n == 1u && t->step[4].note[0] == k4);
    btn_down(B_SEQ);
    frame();
    press(B_ARP);
    btn_up(B_SEQ);
    frames(100);
    bad += check("MONOFM KEYS: SEQ + ARP again: the keys are the steps", !mf_kbd && grid_on());
    btn_down(B_ARP);                                    /* ARP first, then SEQ: the same; no ARP page, no SEQ layer */
    frame();
    btn_down(B_SEQ);
    frame();
    btn_up(B_SEQ);
    btn_up(B_ARP);
    frames(100);
    ok = mf_kbd && cur_page()->graph == GR_ROLL && !ui.layer;
    btn_down(B_ARP);
    frame();
    btn_down(B_SEQ);
    frame();
    btn_up(B_SEQ);
    btn_up(B_ARP);
    frames(100);
    bad += check("MONOFM KEYS: ARP held, then SEQ: the keyboard on, and off again (STEP stays)", ok && !mf_kbd && cur_page()->graph == GR_ROLL);
    /* the LEDs: playing on HOME, the trigs and the running light */
    sc_pattern(t, 48, 16);                              /* (trigs on 0 2 4 .. 14) */
    go_home();
    frame();
    seq_start();
    {
        uint32_t seen_run = 0, trig_ok = 1, n = 0;
        for (k = 0; k < 6000u; k++) {
            uint32_t m, idx, ph;
            mix_block(buf, CTL);
            if (k % 16u)
                continue;
            m = key_leds(0);
            idx = t->seq_idx;
            ph = key_at(0, idx % 16u);
            for (i = 0; i < 16u; i++) {
                uint32_t want = (i % 2u == 0u) ^ (i == idx % 16u);
                trig_ok &= ((m >> key_at(0, i)) & 1u) == want;
            }
            seen_run |= 1u << (idx % 16u);
            n++;
            (void)ph;
        }
        bad += check("MONOFM LEDS: playing, on HOME: the trigs lit, the step playing inverted (running over all 16)",
                     trig_ok && seen_run == 0xFFFFu && n > 50u);
    }
    seq_stop();
    stop_transport();
    frame();
    {
        uint32_t m0 = key_leds(0), m1;
        btn_down(B_REC);
        frames(800);
        m1 = key_leds(0);
        btn_up(B_REC);
        frames(100);
        ok = ((m1 >> key_at(0, 0)) & 1u) && ((m1 >> key_at(0, 2)) & 1u) && !((m1 >> key_at(0, 1)) & 1u) && !((m0 >> key_at(0, 2)) & 1u);
        bad += check("MONOFM LEDS: stopped, REC held: the trigs lit (not without)", ok);
    }
    {   /* Elektron's trig keys on the sound pages: EDIT 1 shown, a white key a trig (silent), held + KNOB 1 a lock of
         * EDIT 1's first parameter; the keys show the trigs, an empty step dark; HOME: the keys play */
        uint32_t i, m, g, id;
        int32_t buf[2 * CTL];
        frames(600);
        for (i = 0; i < NPAGES; i++)
            if (PAGES[i].fam == FAM_EDIT && PAGES[i].scope == SC_ENGINE)
                break;
        ui.home = 0; ui.page = (uint8_t)i; page_entered(); frame();
        for (k = 0; k < NSTEP; k++) { t->step[k].time = ST_REST; t->step[k].n = 0; }
        motion.count = 0;
        ui.bank = 0;
        tap_k(key_at(0, 5));
        for (k = 0; k < 4u; k++) mix_block(buf, CTL);
        ok = grid_on() && step_on(&t->step[5]) && !ke_voices(t);
        key_down(key_at(0, 5));
        frame();
        id = PAGES[i].id[0];
        turn(EN_K1, 4);
        key_up(key_at(0, 5));
        frame();
        {
            int16_t v;
            ok &= motion_lock_get(t, 5, id, &v) && step_on(&t->step[5]) && cur_page() == &PAGES[i];
        }
        m = key_leds(0);
        g = grid_glow();
        ok &= ((m >> key_at(0, 5)) & 1u) && !((m >> key_at(0, 6)) & 1u) && !((g >> key_at(0, 6)) & 1u) && !grid_beats();
        bad += check("MONOFM TRIG KEYS: on EDIT a white key is a trig (silent), held + KNOB 1 locks EDIT's first; lit / dark", ok);
        key_down(key_at(0, 5));
        frame();
        press(B_OCTDN);
        key_up(key_at(0, 5));
        frame();
        {
            int16_t v;
            bad += check("MONOFM TRIG KEYS: a trig held + OCT-: its locks go, the trig stays",
                         !motion_lock_get(t, 5, id, &v) && step_on(&t->step[5]));
        }
        ke_step_page();
        frames(600);
        key_down(key_at(0, 5));
        frame();
        btn_down(B_EDIT);
        frame();
        btn_up(B_EDIT);
        frame();
        ok = mf_plk && PAGES[ui.plk_src - 1u].fam == FAM_EDIT && cur_page()->graph == GR_ROLL && !ui.layer;
        turn(EN_K2, 3);
        key_up(key_at(0, 5));
        frames(100);
        ok &= motion_lock_count(t) >= 1u && step_on(&t->step[5]);
        bad += check("MONOFM TRIG KEYS: on STEP a trig held + EDIT: EDIT's page locks (no layer), KNOB 2 a lock", ok);
        go_home();
        frame();
        key_down(key_at(0, 7));
        frame();
        for (k = 0; k < 4u; k++) mix_block(buf, CTL);
        ok = !grid_on() && ke_voices(t) > 0u;
        key_up(key_at(0, 7));
        frame();
        bad += check("MONOFM TRIG KEYS: on HOME the keys play", ok);
    }
    return bad;
}

/* MONOFM v4 (MONOFM.md "Страницы", "SELECT"): every family two pages, three at most (FX's deep pages hidden); SELECT the
 * fifth knob; F.ENV, DIST's own, the FLAG list with the ratchet, NUDGE by SELECT (it plays), EUCLID by SEQ + SELECT */
static void v4_page(const char *title)
{
    uint32_t i;
    for (i = 0; i < NPAGES; i++)
        if (str_eq(PAGES[i].title, title) && page_visible(i))
            break;
    ui.home = 0;
    ui.page = (uint8_t)(i % NPAGES);
    page_entered();
    frame();
}
static double v4_rms(uint32_t blocks, int32_t note)
{
    int32_t buf[2 * CTL];
    uint32_t k, i;
    double sq = 0;
    midi_event(0x90, 0, (uint32_t)note, 100);
    for (k = 0; k < blocks; k++) {
        mix_block(buf, CTL);
        for (i = 0; i < CTL; i++)
            sq += (double)buf[2u * i] * buf[2u * i];
    }
    midi_event(0x80, 0, (uint32_t)note, 0);
    for (k = 0; k < 400u; k++)
        mix_block(buf, CTL);
    return sqrt(sq / ((double)blocks * CTL));
}
static int test_v4(void)
{
    static const uint8_t ENG[3] = {ENGI_MSAW, ENGI_GLITCH, ENGI_STRING};
    int bad = 0, ok;
    uint32_t i, f, e, n, worst = 0;
    track_t *t = &trk[0];
    char what[200];
    tp_fresh();
    ok = 1;
    for (e = 0; e < 3u; e++) {                          /* the pages a family goes round, by machine */
        set_engine_of(t, ENG[e]);
        frames(50);
        for (f = 0; f < FAM_COUNT; f++) {
            if (f == FAM_HOME)
                continue;
            for (n = 0, i = 0; i < NPAGES; i++)
                n += PAGES[i].fam == f && page_visible(i) && !PAGES[i].hide;
            ok &= n <= (f == FAM_FX ? 4u : 3u);         /* (FX: FX DIST INSERT SLICER, as asked) */
            worst = n > worst && f != FAM_FX ? n : worst;
        }
    }
    for (i = 0; i < NPAGES; i++)                        /* gone: DETAIL, the roll, PHRASES, TOOLS, SONG, AUTOMATION */
        ok &= !str_eq(PAGES[i].title, "DETAIL") && PAGES[i].graph != GR_PATS && PAGES[i].graph != GR_TOOLS &&
              PAGES[i].graph != GR_SONG && PAGES[i].graph != GR_EVENTS;
    snprintf(what, sizeof what, "MONOFM v4 PAGES: every family 3 pages at most (the most %u; FX 4), no DETAIL PHRASES TOOLS SONG "
             "AUTOMATION", worst);
    bad += check(what, ok);
    for (ok = 1, i = 0; i < MI_COUNT; i++)
        ok &= !str_eq(MI_NAME[i], "BPM LOCK") && !str_eq(MI_NAME[i], "STEP EDIT");
    bad += check("MONOFM v4 MENU: no BPM LOCK, no STEP EDIT (the roll)", ok);

    /* SELECT: HOME the tempo, nowhere else */
    set_engine_of(t, ENGI_MSAW);
    go_home();
    frame();
    song.g[G_BPM] = 120;
    turn(EN_SELECT, 3);
    ok = song.g[G_BPM] > 120;
    song.g[G_BPM] = 120;
    v4_page("LFO DEST");
    turn(EN_SELECT, 3);
    ok &= song.g[G_BPM] == 120 && t->p[P_LFADE] > 0;
    bad += check("MONOFM v4 SELECT: HOME the tempo; LFO DEST its FADE, the tempo stays", ok);
    btn_down(B_GLO);
    frames(100);
    turn(EN_SELECT, 3);
    btn_up(B_GLO);
    frames(400);
    bad += check("MONOFM v4 SELECT: GLO + SELECT is no tempo", song.g[G_BPM] == 120);

    /* FILTER: FILT HYB shows the HYBRID twin (BASE RES, SELECT its WIDTH) */
    t->p[P_FLT] = 0;
    v4_page("FILTER");
    ok = cur_page()->id[1] == P_FCUT;
    turn(EN_K1, 5);
    frames(50);
    ok &= t->p[P_FLT] == 5 && str_eq(cur_page()->title, "FILTER") && cur_page()->id[1] == P_HBASE;
    i = (uint32_t)t->p[P_HWIDTH];
    turn(EN_SELECT, -4);
    ok &= t->p[P_HWIDTH] < (int16_t)i;
    turn(EN_K1, -2);
    frames(50);
    ok &= t->p[P_FLT] == 3 && cur_page()->id[1] == P_FCUT;
    bad += check("MONOFM v4 FILTER: HYB is a FILT type (its twin page: BASE RES, SELECT WIDTH), back to HP: CUT", ok);

    /* EDIT: the machine's most used value; INSERT: its dry / wet; AMP: ENV SYNC */
    v4_page("EDIT 1");
    i = (uint32_t)t->p[P_E8];
    turn(EN_SELECT, 4);
    ok = t->p[P_E8] > (int16_t)i && mf_sel_id(cur_page()) == P_E8;   /* (SAW: its ninth, SYNC) */
    v4_page("INSERT");
    t->p[P_IMIX] = 127;
    turn(EN_SELECT, -5);
    ok &= t->p[P_IMIX] < 127;
    v4_page("AMP");
    t->p[P_ESYNC] = 0;
    turn(EN_SELECT, 1);
    ok &= t->p[P_ESYNC] == 1;
    t->p[P_ESYNC] = 0;
    for (e = 0, i = 0; i < NENGINES; i++)
        e += ENGINES[i]->name && mf_sel_e(i) != 0xFFu && eng_rank(i) < NENG_SHOWN;
    ok &= e == NENG_SHOWN;
    bad += check("MONOFM v4 SELECT: EDIT the machine's ninth (SAW SYNC; every machine has one or doubles its most used), "
                 "INSERT dry / wet, AMP ENV SYNC", ok);

    /* SAW's ninth, SYNC: the note's pitch stays, its spectrum changes; SYNC LEAD sets it; a sound load resets it */
    {
        double r0, r1;
        ui_power_on();
        song.master_q12 = 4096;
        set_engine_of(t, ENGI_MSAW);
        apply_preset_to(t, 1);
        t->p[P_DLY] = t->p[P_REV] = t->p[P_CHOR] = 0;
        r0 = v4_rms(300, 45);
        t->p[P_E8] = 100;
        r1 = v4_rms(300, 45);
        apply_preset_to(t, 6);
        ok = t->p[P_E8] == 80 && str_eq(MSAW_PRESETS[6].name, "SYNC LEAD");
        apply_preset_to(t, 0);
        ok &= t->p[P_E8] == 0;
        snprintf(what, sizeof what, "MONOFM v4 SAW SYNC: rms %.0f -> %.0f (sounds), SYNC LEAD 80, another sound 0", r0, r1);
        bad += check(what, ok && r1 > 300.0 && fabs(r1 - r0) > 20.0);
        set_engine_of(t, ENGI_MSAW);
    }

    /* GLO > MIX: KNOB 4 a send, SELECT which (DST CHO DLY REV) */
    v4_page("MIX");
    mf_mix_send = 3u;
    t->p[P_REV] = t->p[P_DLY] = 10;
    turn(EN_K4, 5);
    ok = t->p[P_REV] > 10 && t->p[P_DLY] == 10;
    turn(EN_SELECT, -1);
    turn(EN_K4, 5);
    ok &= mf_mix_send == 2u && t->p[P_DLY] > 10;
    bad += check("MONOFM v4 GLO > MIX: KNOB 4 a send (REV), SELECT another (DLY)", ok);

    /* XY: SELECT the axis */
    v4_page("XY");
    turn(EN_SELECT, 1);
    {
        int16_t *vp;
        ok = mf_xy_axis == 1 && page_desc(cur_page(), 0, &vp) && vp == &t->p[P_FM2_ATK];
        turn(EN_SELECT, -1);
        ok &= mf_xy_axis == 0 && page_desc(cur_page(), 0, &vp) && vp == &t->p[P_FM1_ATK];
    }
    bad += check("MONOFM v4 ARP > XY: one page, SELECT X / Y routes", ok);

    /* FX + SELECT: the deep pages DIST CHORUS DLY REVERB, the page stays; FX tapped: its first */
    v4_page("FX");
    btn_down(B_FX);
    frames(100);
    turn(EN_SELECT, 1);
    ok = str_eq(cur_page()->title, "DIST");
    turn(EN_SELECT, 1);
    turn(EN_SELECT, 1);
    ok &= str_eq(cur_page()->title, "DLY");
    btn_up(B_FX);
    frames(300);
    ok &= str_eq(cur_page()->title, "DLY") && !ui.layer && cur_page()->scope == SC_GLOBAL;
    i = (uint32_t)song.g[G_DFDBK];
    turn(EN_K2, 3);
    ok &= song.g[G_DFDBK] != (int16_t)i;
    press(B_FX);
    frames(50);
    ok &= str_eq(cur_page()->title, "FX");
    bad += check("MONOFM v4 FX + SELECT: DIST, CHORUS, DLY (its knobs; stays when FX is let go), FX tapped: FX", ok);

    /* STEP: KNOB 1 FLAG (- ACC SLD A+S X2 X3 X4 AX2 AX3 AX4), SELECT NUDGE (plays with QNTZ as it is) */
    track_defaults_steps(t);
    t->step[2].note[0] = 60;
    t->step[2].n = 1;
    t->step[2].time = ST_NOTE;
    t->step[2].vel = 100;
    ke_step_page();
    ui.cursor = 2;
    frame();
    for (i = 0; i < 4u; i++)
        turn(EN_K1, 1);
    ok = step_ratchet(&t->step[2]) == 2u && !(t->step[2].flags & (SF_ACCENT | SF_SLIDE)) && mf_flag_of(&t->step[2]) == 4u;
    for (i = 0; i < 4u; i++)
        turn(EN_K1, 1);
    ok &= step_ratchet(&t->step[2]) == 3u && (t->step[2].flags & SF_ACCENT) && mf_flag_of(&t->step[2]) == 8u;
    for (i = 0; i < 12u; i++)
        turn(EN_K1, -1);
    ok &= mf_flag_of(&t->step[2]) == 0u && step_ratchet(&t->step[2]) == 1u;
    bad += check("MONOFM v4 STEP: KNOB 1 FLAG goes - ACC SLD A+S X2 X3 X4 AX2 AX3 AX4 (the ratchet)", ok);
    turn(EN_SELECT, 1);
    turn(EN_SELECT, 1);
    ok = step_nudge(&t->step[2]) == 2 && seq_nudge(t, &t->step[2]) == 2;
    for (i = 0; i < 3u; i++)
        turn(EN_SELECT, -1);
    ok &= step_nudge(&t->step[2]) == -1;
    v4_page("EDIT 1");
    key_down(key_at(0, 2));
    frame();
    turn(EN_SELECT, 1);
    key_up(key_at(0, 2));
    frames(100);
    ok &= step_nudge(&t->step[2]) == 0 && step_on(&t->step[2]);
    bad += check("MONOFM v4 NUDGE: SELECT on STEP (the trig picked), a trig held on EDIT; it plays (QNTZ ignored)", ok);

    /* OCT- + OCT+ together: the keys play the selected track (no trigs), the octave as it was; again: trigs */
    v4_page("EDIT 1");
    mf_kbd = 0;
    song.octave = 1;
    btn_down(B_OCTDN);
    frame();
    btn_down(B_OCTUP);
    frame();
    btn_up(B_OCTDN);
    btn_up(B_OCTUP);
    frames(100);
    ok = mf_kbd == 1 && song.octave == 1 && !grid_on();
    btn_down(B_OCTUP);
    btn_down(B_OCTDN);
    frame();
    btn_up(B_OCTDN);
    btn_up(B_OCTUP);
    frames(100);
    ok &= mf_kbd == 0 && song.octave == 1 && grid_on();
    press(B_OCTDN);
    ok &= song.octave == 0;
    bad += check("MONOFM v4 OCT- + OCT+: the keys play the track (the octave kept), again: the trigs; one OCT- still "
                 "the octave", ok);
    song.octave = 0;

    /* RETRIG: ARP held, a key: its note again every RATE (1/16), not toggling a trig; KNOB 1 its RATE; ARP let go: the
     * hits stop, ARP's page not opened */
    {
        int32_t buf[2 * CTL];
        uint32_t k, ons = 0, was = 0, pg;
        v4_page("EDIT 1");
        pg = ui.page;
        track_defaults_steps(t);
        mf_kbd = 0;
        mf_rtg_p[0] = 3;
        btn_down(B_ARP);
        frame();
        ok = mf_rtg_ui && !grid_on();
        turn(EN_K1, 1);
        ok &= mf_rtg_p[0] == 4;
        turn(EN_K1, -1);
        key_down(key_at(0, 0));
        frame();
        for (k = 0; k < 2000u; k++) {                   /* ~1.45 s at 120: 1/16 every 5512 samples, ~11 hits */
            uint32_t n = ke_voices(t);
            if (n && !was)
                ons++;
            was = n;
            mix_block(buf, CTL);
            if (mf_rtg_snd[0] == 0 && mf_rtg_n && n)
                ;                                       /* (gated off between hits) */
        }
        ok &= mf_rtg_n == 1u && !step_on(&t->step[0]);
        key_up(key_at(0, 0));
        frame();
        btn_up(B_ARP);
        frames(100);
        for (k = 0; k < 200u; k++)
            mix_block(buf, CTL);
        ok &= !mf_rtg_ui && !mf_rtg_n && ui.page == pg && !ui.home;
        snprintf(what, sizeof what, "MONOFM v4 RETRIG: ARP held + a key repeats it (%u hits in 1.45 s), no trig; KNOB 1 RATE; "
                 "ARP let go: stops, no ARP page", ons);
        bad += check(what, ok && ons >= 6u && ons <= 14u);
        press(B_ARP);
        frames(50);
        bad += check("MONOFM v4 RETRIG: ARP tapped alone still opens ARP", cur_page()->fam == FAM_ARP);
    }

    /* F.ENV: LP at a low CUT opened by FILTER's ENV with a slow decay: louder than without */
    {
        double flat, env, dist[4];
        ui_power_on();
        song.master_q12 = 4096;
        set_engine_of(t, ENGI_MSAW);
        apply_preset_to(t, 1);
        t->p[P_DLY] = t->p[P_REV] = t->p[P_CHOR] = 0;
        t->p[P_LD_FLT] = t->p[P_ED_PIT] = t->p[P_ED_SHP] = 0;
        t->p[P_FLT] = 1;
        t->p[P_FCUT] = 10;
        t->p[P_FRES] = 0;
        t->p[P_ED_FLT] = 0;
        flat = v4_rms(300, 45);
        t->p[P_ED_FLT] = 63;
        t->p[P_FATK] = 0;
        t->p[P_FDEC] = 110;
        env = v4_rms(300, 45);
        snprintf(what, sizeof what, "MONOFM v4 F.ENV: LP 10 rms %.0f, with ENV +63 (DEC 110) %.0f", flat, env);
        bad += check(what, env > flat * 1.5);
        t->p[P_ED_FLT] = 0;
        t->p[P_FLT] = 0;
        t->p[P_DIST] = 110;
        ok = 1;
        for (i = 0; i < 4u; i++) {
            t->p[P_DTYPE] = (int16_t)i;
            dist[i] = v4_rms(300, 45);
            ok &= dist[i] > 100.0 && dist[i] < 30000.0;
        }
        ok &= fabs(dist[1] - dist[0]) > 50.0 && fabs(dist[2] - dist[0]) > 50.0 && fabs(dist[3] - dist[0]) > 50.0;
        snprintf(what, sizeof what, "MONOFM v4 DIST: SOFT %.0f HARD %.0f FOLD %.0f FUZZ %.0f (each its own, none silent "
                 "or wild)", dist[0], dist[1], dist[2], dist[3]);
        bad += check(what, ok);
        t->p[P_DTYPE] = 0;
        t->p[P_DOUT] = -64;
        {
            double q = v4_rms(200, 45);
            snprintf(what, sizeof what, "MONOFM v4 DIST: OUT -64 is silence (rms %.1f, the reverb's tail)", q);
            bad += check(what, q < dist[0] * 0.01);
        }
        t->p[P_DOUT] = 0;
        t->p[P_DIST] = 0;
    }
    return bad;
}

/* SHOTS=dir: the v4 screens as PPMs (240 x 240), for a look (tests/ui_render.py makes PNGs of a ppm/ folder) */
static void shot(const char *dir, const char *name)
{
    char path[256];
    FILE *f;
    uint32_t k;
    ui.force = 1;
    frame();
    frame();
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    if (!(f = fopen(path, "wb")))
        return;
    fprintf(f, "P6\n240 240\n255\n");
    for (k = 0; k < 240u * 240u; k++) {
        uint16_t v = (uint16_t)((host_screen[k] >> 8) | (host_screen[k] << 8));
        uint8_t rgb[3] = {(uint8_t)((v >> 11) * 255 / 31), (uint8_t)(((v >> 5) & 63) * 255 / 63), (uint8_t)((v & 31) * 255 / 31)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}
static void v4_shots(const char *dir)
{
    track_t *t = &trk[0];
    static const char *const P[] = {"FILTER", "F.ENV", "LFO", "LFO DEST", "MOD", "FX", "DIST", "INSERT", "SLICER", "SCL",
                                    "CHORD", "MICRO", "EDIT 1", "EDIT 2", "VOICE", "AMP", "MIX", "USER", "PRESETS", "PROJECT",
                                    "ARP", "ARP 2", "XY", "EUCLID", "MIXER", "CLOCK"};
    uint32_t i;
    char n[32];
    tp_fresh();
    palette_set(UI_CRYPT_INDEX);
    track_defaults_steps(t);
    for (i = 0; i < 16u; i += 3u) {
        t->step[i].note[0] = (uint8_t)(48 + i);
        t->step[i].n = 1;
        t->step[i].time = ST_NOTE;
        t->step[i].vel = 100;
    }
    mf_flag_set(&t->step[3], 5u);
    step_set_nudge(&t->step[3], 3);
    t->p[P_ED_FLT] = 40;
    go_home();
    shot(dir, "home");
    ke_step_page();
    ui.cursor = 3;
    shot(dir, "step");
    for (i = 0; i < NELEM(P); i++) {
        v4_page(P[i]);
        str_cpy(n, P[i], sizeof n);
        if (n[1] == '.') n[1] = '_';
        if (n[4] == ' ') n[4] = '_';
        if (n[3] == ' ') n[3] = '_';
        shot(dir, n);
    }
    t->p[P_FLT] = 5;
    v4_page("FILTER");
    shot(dir, "FILTER_HYB");
    v4_page("FX");
    btn_down(B_FX);
    frames(100);
    turn(EN_SELECT, 1);
    turn(EN_SELECT, 1);
    turn(EN_SELECT, 1);
    btn_up(B_FX);
    frames(300);
    shot(dir, "fx_deep_dly");
    v4_page("EDIT 1");
    turn(EN_SELECT, 2);
    ui.msg_t = 38;
    ui.force = 1;
    ui_draw();
    shot(dir, "msg_glitch");
}

/* machines one by one (v4): each machine's ninth value (SELECT on EDIT) and SINE's / NOISE's new knobs change the sound:
 * the same note rendered with the value at its default and at another, the difference's rms against the sound's */
static double e8_buf[2][300 * CTL];
static double e8_render(uint32_t e, uint32_t id, int32_t v, uint32_t k, uint32_t vel)
{
    int32_t buf[2 * CTL];
    uint32_t b, i;
    double sq = 0;
    st_reset();
    song.master_q12 = 4096;
    set_engine_of(&trk[0], e);
    trk[0].p[P_DLY] = trk[0].p[P_REV] = trk[0].p[P_CHOR] = trk[0].p[P_DIST] = 0;
    trk[0].p[P_VOICE] = V_MONO;
    if (id != 0xFFu)
        trk[0].p[id] = (int16_t)v;
    for (b = 0; b < 16u; b++)
        mix_block(buf, CTL);
    midi_event(0x90, 0, 48, vel);
    for (b = 0; b < 300u; b++) {
        mix_block(buf, CTL);
        for (i = 0; i < CTL; i++) {
            e8_buf[k][b * CTL + i] = buf[2u * i];
            sq += (double)buf[2u * i] * buf[2u * i];
        }
    }
    midi_event(0x80, 0, 48, 0);
    return sqrt(sq / (300.0 * CTL));
}
static int test_machines_v4(void)
{
    static const struct { uint8_t e, id; int16_t v; } T[] = {
        {ENGI_MSAW, P_E8, 100}, {ENGI_MPULSE, P_E8, 100}, {ENGI_MENS, P_E8, 127}, {ENGI_MSID, P_E8, 3},
        {ENGI_MWAVE, P_E8, 127}, {ENGI_MDDRAW, P_E8, 127}, {ENGI_MFMST, P_E8, 127}, {ENGI_MFMPAR, P_E8, 127},
        {ENGI_MFMDYN, P_E8, 127}, {ENGI_MVO, P_E8, 127}, {ENGI_MBBOX, P_E8, 10}, {ENGI_GLITCH, P_E8, 10},
        {ENGI_MSINE, P_E0 + 2, 100}, {ENGI_MSINE, P_E0 + 3, 50}, {ENGI_MSINE, P_E0 + 5, 127}, {ENGI_MSINE, P_E0 + 6, 127},
        {ENGI_MNOISE, P_E0 + 2, 10}, {ENGI_MNOISE, P_E0 + 3, 120}, {ENGI_MNOISE, P_E0 + 4, 6},
    };
    int bad = 0;
    uint32_t k, i, n = 300u * CTL;
    char what[160];
    for (k = 0; k < NELEM(T); k++) {
        const param_desc_t *d = param_desc_of(T[k].e, T[k].id);
        double a = e8_render(T[k].e, 0xFFu, 0, 0, 60), dsq = 0, r;
        if (T[k].id == P_E8 && T[k].e == ENGI_MSINE)
            continue;
        e8_render(T[k].e, T[k].id, T[k].v, 1, 60);
        for (i = 0; i < n; i++)
            dsq += (e8_buf[1][i] - e8_buf[0][i]) * (e8_buf[1][i] - e8_buf[0][i]);
        r = sqrt(dsq / n);
        snprintf(what, sizeof what, "MONOFM machine %-6s %-4s %4d: the sound rms %5.0f, changed by %5.0f", ENGINES[T[k].e]->name,
                 d->label, T[k].v, a, r);
        bad += check(what, a > 100.0 && r > a * 0.05 && d->label && d->label[0] != '-');
    }
    {   /* every MONO and SLICE machine has a ninth (SINE and NOISE: their pages filled instead); its default is silent */
        static const uint8_t NINE[] = {ENGI_MSAW, ENGI_MPULSE, ENGI_MENS, ENGI_MSID, ENGI_MWAVE, ENGI_MDDRAW, ENGI_MBBOX,
                                       ENGI_MFMST, ENGI_MFMPAR, ENGI_MFMDYN, ENGI_MVO, ENGI_GLITCH, ENGI_ERROR,
                                       ENGI_RITUAL, ENGI_MECH, ENGI_BIO};
        int ok = 1;
        for (k = 0; k < NELEM(NINE); k++)
            ok &= ENGINES[NINE[k]]->e8.label && ENGINES[NINE[k]]->e8.def == 0;
        for (k = 0; k < 7u; k++)
            ok &= ENGINES[ENGI_MSINE]->edit[k].label[0] != '-';
        for (k = 0; k < 5u; k++)
            ok &= ENGINES[ENGI_MNOISE]->edit[k].label[0] != '-';
        bad += check("MONOFM machines: MONO and SLICE each a ninth (default off), SINE 7 knobs, NOISE 5", ok);
    }
    return bad;
}

int main(void)
{
    const char *wavs = getenv("WAVS");
    int bad = 0;
    if (getenv("BENCH_ENG")) {                          /* one machine, its preset BENCH_PRE: an 8-note chord for BENCH seconds,
                                                         * T1 only (tests/monofm_cpu.sh ENG) */
        int32_t buf[2 * CTL];
        uint32_t e = (uint32_t)atoi(getenv("BENCH_ENG")), k, f, n = (uint32_t)(SR * atof(getenv("BENCH") ? getenv("BENCH") : "1"));
        st_reset();
        set_engine_of(&trk[0], e % NENGINES);
        apply_preset_to(&trk[0], getenv("BENCH_PRE") ? (uint32_t)atoi(getenv("BENCH_PRE")) : 0u);
        trk[0].p[P_VOICE] = V_POLY;
        for (k = 0; k < 16u; k++) mix_block(buf, CTL);
        for (k = 0; k < 8u; k++) midi_event(0x90, 0, 36 + k * 5u, 100);
        for (f = 0; f < n; f += CTL) mix_block(buf, CTL);
        return 0;
    }
    if (getenv("MF_KEYS_ONLY"))
        return test_keys_editor() != 0;
    if (getenv("MF_PAT_ONLY"))
        return test_mfpat() != 0;
    if (getenv("SHOTS")) {
        v4_shots(getenv("SHOTS"));
        return 0;
    }
    if (getenv("MF_MACH_ONLY"))
        return test_machines_v4() != 0;
    if (getenv("MF_V4_ONLY"))
        return test_v4() != 0;
    if (getenv("MF_STAB_ONLY"))
        return test_stability() != 0;
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
    bad += test_euclid();
    bad += test_xy();
    bad += test_filter();
    bad += test_punch();
    bad += test_mfpat();
    bad += test_keys_editor();
    bad += test_v4();
    bad += test_machines_v4();
    bad += test_stability();
    printf(bad ? "monofm: %d FAILED\n" : "monofm: all passed\n", bad);
    return bad != 0;
}
