/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Tim (646timur-cell), on Felucca by Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* MONOFM's machines, after the synthesis machines of Elektron's Monomachine (their ideas and parameter names as its
 * manual describes them; none of its code): eight parameters each (EDIT 1 / EDIT 2), the last one TUNE (+-100 cents).
 * A track plays them MONO by its factory sounds (as the Monomachine's tracks); EDIT > VOICE POLY gives up to MF_POLY.
 *   SAW    SWAVE SAW: a saw, a unison pair (UNIL UNIW) and an extended pair twice as wide (UNXL), AGE (the oscillators
 *          drifting), the subs: a square an octave down (SUBX), sines one and two down (SUB1 SUB2)
 *   PULSE  SWAVE PULSE: a pulse and its unison pair (UNIL UNIW), PW and its sweep to a spike and back (PWAD, PWRS: from
 *          PW again at each note), two sine subs
 *   ENS    SWAVE ENSEMBLE: up to four pitches (PCH2..4: semitones or just intervals 6/5 5/4 4/3 3/2: a chord on a
 *          mono track, p-lock it), each two saws that WAVE and PW turn from a saw to a square to spikes, a chorus of
 *          its own (CHRL CHRW)
 *   SID    a 6581-style oscillator: TRI SAW PULSE NOISE and their combined waves (ANDed, as the chip's), PW and its
 *          sweep, RING / SYNC / R+S against a second oscillator MRAT semitones off, GRIT (12 bits down to 4, the rate
 *          held), NOIS (the noise generator added)
 *   WAVE   DIGIPRO WAVE: 32 raw 12-bit single cycles (tools/gen_mono_tables.py), MORF toward the next one, MSPD a
 *          sweep through them (MRST: from WAVE again at each note), SYNC (a hard-synced slave up to 4 octaves), BITS,
 *          DEC (the rate held)
 *   DDRAW  DIGIPRO DOUBLEDRAW: two cycles (WAV1 WAV2, the second OCT2 octaves and DTUN cents off) blended by MIX, its
 *          LFO MMOD
 *   FM-ST  FM+ STATIC: a sine carrier, two modulators (ratios RAT2 RAT3, depths DEP2 DEP3) side by side or stacked
 *          (ALGO), FB on the first, MDEC: the depths decaying (0: held)
 *   FM-PAR FM+ PARALLEL: three modulators into the carrier, each a ratio and a level, MDEC
 *   FM-DYN FM+ DYNAMIC: a linear FM (LIN, LRAT) and an exponential one (EXP, ERAT), FB on the carrier, a pitch
 *          envelope (PENV PDEC)
 *   VO-6   a voice: a consonant (CONS, 19 and none: plosives, nasals, liquids, fricatives, affricates; CLEN its length,
 *          CVOL its loudness) into a vowel (VOW1) gliding to another (VOW2, MORF the glide), BRTH breath; a glottal
 *          pulse and noise through three formants
 *   SINE   GND SINE: a sine, FOLD (a wavefolder), SUB (a sine an octave down)
 *   NOISE  GND NOISE: noise held at the note's rate (pitched digital noise, RATE how much of it), COLR its colour
 * (BBOX, DIGIPRO BEAT BOX: eng_glitch.c, a 12-bit drum kit on GLITCH's engine.) */

#include "felucca_mono.h"

#define MF_POLY 4
static const char *const N_MF_ONOFF[] = {"OFF", "ON"};
#define MF_NONE {"-", F_INT, 0, 0, 0, 0, 0}
#define MF_TUNE {"TUNE", F_INT, -64, 63, 0, 0, 0}

/* the note with TUNE (E8: -64..63, +-100 cents) */
static uint32_t mf_inc(const track_t *t, const vmod_t *m)
{
    int32_t ct = t->p[P_E7] * 100 / 64;
    return ct ? cents_inc(m->pitch16, ct, m->fine) : m->inc;
}
/* inc x 2^(ct / 1200), capped below Nyquist */
static uint32_t mf_mul(uint32_t inc, int32_t ct)
{
    uint64_t r = ((uint64_t)inc * px_exp2(ct * 4369 / 80)) >> 16;   /* (65536 / 1200 = 54.61) */
    return r > 0x7F000000u ? 0x7F000000u : (uint32_t)r;
}
/* the gain (Q15) that brings a sum of oscillators of total weight w (Q15, one oscillator = 32767) back to one */
static int32_t mf_norm(int32_t w)
{
    return (int32_t)(((int64_t)32767 << 15) / (w < 32767 ? 32767 : w));
}
/* a fresh voice (from silence): its phases to the given seeds, so unison pairs do not start in phase */
static int mf_fresh(const voice_t *v) { return !v->env && !v->env_out; }

/* ================================================================ SWAVE SAW === */
/* ph[0] the saw, ph[1] ph[2] the unison pair, s[0] s[1] the extended pair, s[2] SUB1's (and SUBX's) phase, s[3] SUB2's,
 * s[4] AGE's walk (cents Q8), s[5] its random numbers */
static void msaw_note_on(track_t *t, voice_t *v)
{
    (void)t;
    if (mf_fresh(v)) {
        uint32_t r = rng() | 1u;
        v->ph[0] = 0;
        v->ph[1] = r;
        v->ph[2] = r * 2654435761u;
        v->s[0] = (int32_t)(r * 40503u);
        v->s[1] = (int32_t)(r ^ 0x5BD1E995u);
        v->s[2] = v->s[3] = 0;
        v->s[5] = (int32_t)(r * 69069u + 1u);
    }
}
static void msaw_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t i, b, iu1, iu2, ix1, ix2, isub, ph0 = v->ph[0], ph1 = v->ph[1], ph2 = v->ph[2];
    uint32_t px1 = (uint32_t)v->s[0], px2 = (uint32_t)v->s[1], ps1 = (uint32_t)v->s[2], ps2 = (uint32_t)v->s[3];
    int32_t ul = p[P_E0] * 258, xl = p[P_E2] * 258, sx = p[P_E4] * 258, s1 = p[P_E5] * 258, s2 = p[P_E6] * 258;
    int32_t wc = p[P_E1] * 50 / 127, age = p[P_E3], g, drift = 0;
    if (age) {                                           /* AGE: a slow walk of up to +-12 cents on the pairs, a third of
                                                          * it on the saw */
        uint32_t rs = (uint32_t)v->s[5];
        v->s[4] = sx_walk(v->s[4], &rs, age * 40, age * 24);
        v->s[5] = (int32_t)rs;
        drift = v->s[4] >> 8;
    }
    b = mf_mul(mf_inc(t, m), drift / 3);
    iu1 = mf_mul(b, wc + drift);
    iu2 = mf_mul(b, -wc - drift);
    ix1 = mf_mul(b, 2 * wc - drift);
    ix2 = mf_mul(b, -2 * wc + drift);
    isub = b >> 1;
    g = mf_norm(32767 + 2 * ul + 2 * xl + (sx + s1 + s2) / 2);
    for (i = 0; i < n; i++) {
        int32_t o = osc_saw(ph0, b);
        if (ul)
            o += mulq15(osc_saw(ph1, iu1) + osc_saw(ph2, iu2), ul);
        if (xl)
            o += mulq15(osc_saw(px1, ix1) + osc_saw(px2, ix2), xl);
        if (sx)
            o += mulq15(ps1 < 0x80000000u ? 26000 : -26000, sx);
        if (s1)
            o += mulq15(sine_i(ps1), s1);
        if (s2)
            o += mulq15(sine_i(ps2), s2);
        ph0 += b;
        ph1 += iu1;
        ph2 += iu2;
        px1 += ix1;
        px2 += ix2;
        ps1 += isub;
        ps2 += isub >> 1;
        out[i] += voice_amp(clamp(mulq15(o, g), -32767, 32767), m, i);
    }
    v->ph[0] = ph0;
    v->ph[1] = ph1;
    v->ph[2] = ph2;
    v->s[0] = (int32_t)px1;
    v->s[1] = (int32_t)px2;
    v->s[2] = (int32_t)ps1;
    v->s[3] = (int32_t)ps2;
}

/* ============================================================== SWAVE PULSE === */
/* ph[0] the pulse, ph[1] ph[2] the unison pair, s[0] SUB1, s[1] SUB2, s[3] the PW sweep's phase */
static void mpulse_note_on(track_t *t, voice_t *v)
{
    if (mf_fresh(v)) {
        uint32_t r = rng() | 1u;
        v->ph[0] = 0;
        v->ph[1] = r;
        v->ph[2] = r * 2654435761u;
        v->s[0] = v->s[1] = 0;
    }
    if (t->p[P_E6])                                      /* PWRS: the sweep from PW again */
        v->s[3] = 0;
}
static void mpulse_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t i, b = mf_inc(t, m), wc = (uint32_t)(p[P_E1] * 50 / 127), iu1 = mf_mul(b, (int32_t)wc), iu2 = mf_mul(b, -(int32_t)wc);
    uint32_t ph0 = v->ph[0], ph1 = v->ph[1], ph2 = v->ph[2], ps1 = (uint32_t)v->s[0], ps2 = (uint32_t)v->s[1], pw;
    int32_t ul = p[P_E0] * 258, s1 = p[P_E2] * 258, s2 = p[P_E3] * 258, g;
    int32_t pwf = 655 + p[P_E4] * 31457 / 127;          /* PW: 2 % .. 98 % (Q15), 64 the square */
    if (p[P_E5]) {                                       /* PWAD: a triangle 0.1 .. ~13 Hz toward the spike and back */
        uint32_t sw = (uint32_t)v->s[3], hz = sx_expmap(p[P_E5], 6554u, 7);
        int32_t tri, spike = p[P_E4] >= 64 ? 32112 : 655;
        sw += sx_hz_inc(hz) * n;
        v->s[3] = (int32_t)sw;
        tri = sw < 0x80000000u ? (int32_t)(sw >> 16) : (int32_t)(0xFFFFu - ((sw >> 16) & 0x7FFFu) - 0x8000u);
        tri = clamp(tri, 0, 32767);                      /* (0 .. 1, up and down) */
        pwf += mulq15(spike - pwf, tri);
    }
    pw = (uint32_t)pwf << 17;
    g = mf_norm(32767 + 2 * ul + (s1 + s2) / 2);
    for (i = 0; i < n; i++) {
        int32_t o = osc_pulse(ph0, b, pw) << 1;
        if (ul)
            o += mulq15((osc_pulse(ph1, iu1, pw) + osc_pulse(ph2, iu2, pw)) << 1, ul);
        if (s1)
            o += mulq15(sine_i(ps1), s1);
        if (s2)
            o += mulq15(sine_i(ps2), s2);
        ph0 += b;
        ph1 += iu1;
        ph2 += iu2;
        ps1 += b >> 1;
        ps2 += b >> 2;
        out[i] += voice_amp(clamp(mulq15(o, g), -32767, 32767), m, i);
    }
    v->ph[0] = ph0;
    v->ph[1] = ph1;
    v->ph[2] = ph2;
    v->s[0] = (int32_t)ps1;
    v->s[1] = (int32_t)ps2;
}

/* =========================================================== SWAVE ENSEMBLE === */
/* PCH2..4: OFF, -12 .. -1, +1 .. +12 semitones, then 6/5 5/4 4/3 3/2 (cents) */
static const char *const N_MENS_PCH[] = {"OFF", "-12", "-11", "-10", "-9", "-8", "-7", "-6", "-5", "-4", "-3", "-2", "-1",
                                         "+1", "+2", "+3", "+4", "+5", "+6", "+7", "+8", "+9", "+10", "+11", "+12",
                                         "6/5", "5/4", "4/3", "3/2"};
static int32_t mens_cents(int32_t k)
{
    static const int16_t JI[4] = {316, 386, 498, 702};
    return k <= 0 ? 0 : k <= 12 ? (k - 13) * 100 : k <= 24 ? (k - 12) * 100 : JI[(k - 25) & 3];
}
typedef struct {
    uint32_t ph[4], pc[4];                               /* each pitch's oscillator and its chorus copy */
    uint32_t lfo;
} mens_slot_t;
static mens_slot_t *mens_slot(track_t *t, voice_t *v)
{
    return (mens_slot_t *)ENG_MEM(t - trk) + (uint32_t)(v - t->v) % NVOICE;
}
ENG_MEM_FITS("ENS", sizeof(mens_slot_t) * NVOICE);
static void mens_note_on(track_t *t, voice_t *v)
{
    if (mf_fresh(v)) {
        mens_slot_t *S = mens_slot(t, v);
        uint32_t k, r = rng() | 1u;
        for (k = 0; k < 4u; k++) {
            S->ph[k] = r * (k * 2u + 1u);
            S->pc[k] = r * (k * 2u + 2u) * 2654435761u;
        }
    }
}
static void mens_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    mens_slot_t *S = mens_slot(t, v);
    uint32_t i, k, np = 0, b = mf_inc(t, m), inc[4], incc[4], ph[4], pc[4];
    uint32_t pwo = (uint32_t)(655 + p[P_E4] * 31457 / 127) << 17;   /* the second saw's offset: 2 % .. 98 % */
    int32_t wv = p[P_E3] * 258, cl = p[P_E5] * 258, cw = p[P_E6], g, lfo;
    S->lfo += sx_hz_inc(42598u) * n;                     /* the chorus: 0.65 Hz */
    lfo = sine_i(S->lfo);
    for (k = 0; k < 4u; k++) {
        int32_t ct = k ? mens_cents(p[P_E0 + k - 1u]) : 0;
        if (k && !ct)
            continue;
        inc[np] = mf_mul(b, ct);
        incc[np] = mf_mul(b, ct + ((cw * (lfo * (k & 1u ? -1 : 1))) >> 15) / 5 + cw / 12);   /* up to ~25 cents */
        ph[np] = S->ph[k];
        pc[np] = S->pc[k];
        np++;
    }
    g = mf_norm((int32_t)np * (32767 + cl) * 2 / 2);
    for (i = 0; i < n; i++) {
        int32_t o = 0;
        for (k = 0; k < np; k++) {
            int32_t a = osc_saw(ph[k], inc[k]) - mulq15(osc_saw(ph[k] + pwo, inc[k]), wv);
            if (cl)
                a += mulq15(osc_saw(pc[k], incc[k]) - mulq15(osc_saw(pc[k] + pwo, incc[k]), wv), cl);
            o += a;
            ph[k] += inc[k];
            pc[k] += incc[k];
        }
        out[i] += voice_amp(clamp(mulq15(o, g), -32767, 32767), m, i);
    }
    for (k = 0, np = 0; k < 4u; k++) {
        if (k && !mens_cents(p[P_E0 + k - 1u]))
            continue;
        S->ph[k] = ph[np];
        S->pc[k] = pc[np];
        np++;
    }
}

/* ======================================================================= SID === */
static const char *const N_MSID_WAVE[] = {"TRI", "SAW", "PULS", "NOIS", "T+S", "T+P", "S+P", "TSP"};
static const char *const N_MSID_MOD[] = {"OFF", "RING", "SYNC", "R+S"};
/* ph[0] the oscillator, ph[1] the modulating one, ph[2] PW's sweep (Q8 of 12 bits), s[0] the noise register (23 bits),
 * s[1] the clocking bit, s[2] GRIT's hold counter, s[3] the held sample, s[4] the DC blocker */
static void msid_note_on(track_t *t, voice_t *v)
{
    (void)t;
    if (mf_fresh(v)) {
        v->ph[0] = v->ph[1] = 0;
        v->s[0] = 0x7FFFF8;
        v->s[1] = v->s[2] = v->s[3] = v->s[4] = 0;
    }
}
static inline uint32_t msid_noise12(uint32_t r)          /* the chip's eight noise output bits */
{
    return ((r >> 11) & 0x800u) | ((r >> 10) & 0x400u) | ((r >> 7) & 0x200u) | ((r >> 5) & 0x100u) |
           ((r >> 4) & 0x080u) | ((r >> 1) & 0x040u) | ((r << 1) & 0x020u) | ((r << 2) & 0x010u);
}
static void msid_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t i, b = mf_inc(t, m), mi = mf_mul(b, p[P_E4] * 100), wave = (uint32_t)clamp(p[P_E0], 0, 7);
    uint32_t mod = (uint32_t)clamp(p[P_E3], 0, 3), ring = mod & 1u, sync = mod >> 1, ph = v->ph[0], mph = v->ph[1];
    if (sync) {                                          /* SYNC: the modulator is the master at the note, the oscillator
                                                          * MRAT semitones off (the sync sweep: MRAT, a p-lock, an LFO) */
        uint32_t x = b;
        b = mi;
        mi = x;
    }
    uint32_t r = (uint32_t)v->s[0], clk = (uint32_t)v->s[1], hold = (uint32_t)v->s[2], pw12, grit = (uint32_t)p[P_E5];
    uint32_t keep = 0xFFFu << (grit >> 4), dec = 1u + (grit >> 3);   /* GRIT: 12 bits .. 5, the rate held 1 .. 16 */
    int32_t held = v->s[3], dc = v->s[4], nzl = p[P_E6] * 258;
    static const uint8_t WMASK[8] = {1, 2, 4, 8, 3, 5, 6, 7};   /* bits: TRI SAW PULSE NOISE */
    uint32_t wm = WMASK[wave];
    v->ph[2] += (uint32_t)(p[P_E2] * 3 * (int32_t)n / 4);   /* PWAD: the width register counting up, wrapping */
    pw12 = ((uint32_t)p[P_E1] * 4095u / 127u + (v->ph[2] >> 8)) & 0xFFFu;
    if (!grit)
        dec = 1u;
    for (i = 0; i < n; i++) {
        uint32_t w = 0xFFFu, nb, y;
        int32_t x;
        uint32_t was = mph;
        mph += mi;
        if (sync && mph < was)                           /* SYNC: the modulator's wrap restarts the oscillator */
            ph = 0;
        else
            ph += b;
        nb = (ph >> 27) & 1u;                            /* the noise register steps on bit 19 (of 24) rising */
        if (nb && !clk) {
            uint32_t fb = ((r >> 22) ^ (r >> 17)) & 1u;
            r = ((r << 1) | fb) & 0x7FFFFFu;
        }
        clk = nb;
        if (wm & 1u) {                                   /* TRI (RING: its fold by the modulator's top bit) */
            uint32_t msb = (ph >> 31) ^ (ring ? mph >> 31 : 0u);
            w &= ((ph >> 19) ^ (msb ? 0xFFFu : 0u)) & 0xFFFu;
        }
        if (wm & 2u)
            w &= ph >> 20;
        if (wm & 4u)
            w &= (ph >> 20) >= pw12 ? 0xFFFu : 0u;
        if (wm & 8u)
            w &= msid_noise12(r);
        if (--hold > dec) {                              /* GRIT: a new sample every dec */
            hold = dec;
            held = (int32_t)(w & keep);
        }
        y = (uint32_t)held;
        x = ((int32_t)y - 2048) << 4;
        if (nzl)
            x += mulq15(((int32_t)msid_noise12(r) - 2048) << 4, nzl);
        dc += (x - dc) >> 9;                             /* (the ANDed waves sit off centre: a DC blocker, ~14 Hz) */
        out[i] += voice_amp(clamp(x - dc, -32767, 32767), m, i);
    }
    v->ph[0] = ph;
    v->ph[1] = mph;
    v->s[0] = (int32_t)r;
    v->s[1] = (int32_t)clk;
    v->s[2] = (int32_t)hold;
    v->s[3] = held;
    v->s[4] = dc;
}

/* ============================================================= DIGIPRO WAVE === */
static const char *const N_MBITS[] = {"12", "10", "8", "6", "5", "4", "3"};
static const uint8_t MBITS_SH[7] = {0, 2, 4, 6, 7, 8, 9};   /* of the 12 */
/* wave w's sample at phase ph (linear between the 128 points) */
static inline int32_t dpro_at(uint32_t w, uint32_t ph)
{
    const int16_t *a = DPRO_WAVE[w % DPRO_N];
    uint32_t k = ph >> 25;
    int32_t f = (int32_t)((ph >> 10) & 0x7FFFu);
    return a[k] + (((a[k + 1u] - a[k]) * f) >> 15);
}
/* ph[0] the oscillator, ph[1] SYNC's master, s[0] MSPD's position (Q16 of a wave), s[1] DEC's counter, s[2] the held */
static void mwave_note_on(track_t *t, voice_t *v)
{
    if (mf_fresh(v))
        v->ph[0] = v->ph[1] = 0;
    if (t->p[P_E3])                                      /* MRST: the sweep from WAVE again */
        v->s[0] = 0;
}
static void mwave_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t i, b = mf_inc(t, m), si = p[P_E4] ? mf_mul(b, p[P_E4] * 100) : b, ph = v->ph[0], mph = v->ph[1];
    uint32_t pos, w0, w1, sh = MBITS_SH[clamp(p[P_E5], 0, 6)], dec = 1u + (uint32_t)p[P_E6] / 8u, cnt = (uint32_t)v->s[1];
    int32_t f, held = v->s[2];
    if (p[P_E2])                                         /* MSPD: through the waves, 0.05 .. ~6 waves a second */
        v->s[0] = (int32_t)(((uint32_t)v->s[0] + (sx_expmap(p[P_E2], 3277u, 7) * n) / 44100u * 65536u / 65536u) %
                            (DPRO_N * 65536u));
    pos = (uint32_t)clamp(p[P_E0], 0, DPRO_N - 1) * 65536u + (uint32_t)p[P_E1] * 65535u / 127u + (uint32_t)v->s[0];
    w0 = (pos >> 16) % DPRO_N;
    w1 = (w0 + 1u) % DPRO_N;
    f = (int32_t)((pos >> 1) & 0x7FFFu);
    for (i = 0; i < n; i++) {
        int32_t a, c;
        if (p[P_E4]) {                                   /* SYNC: the master's wrap restarts the slave */
            uint32_t was = mph;
            mph += b;
            ph = mph < was ? 0u : ph + si;
        } else {
            ph += b;
        }
        if (--cnt > dec) {
            cnt = dec;
            a = dpro_at(w0, ph);
            c = f ? dpro_at(w1, ph) : a;
            held = (a + (((c - a) * f) >> 15)) >> sh << sh;
        }
        out[i] += voice_amp(clamp(held, -32767, 32767), m, i);
    }
    v->ph[0] = ph;
    v->ph[1] = mph;
    v->s[1] = (int32_t)cnt;
    v->s[2] = held;
}

/* ======================================================== DIGIPRO DOUBLEDRAW === */
static const char *const N_MOCT2[] = {"-2", "-1", "0", "+1", "+2"};
/* ph[0] WAV1, ph[1] WAV2, s[0] MMOD's LFO */
static void mddraw_note_on(track_t *t, voice_t *v)
{
    (void)t;
    if (mf_fresh(v)) {
        v->ph[0] = 0;
        v->ph[1] = rng();
    }
}
static void mddraw_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t i, b = mf_inc(t, m), b2 = mf_mul(b, (p[P_E5] - 2) * 1200 + p[P_E4] * 50 / 64), ph1 = v->ph[0], ph2 = v->ph[1];
    uint32_t w1 = (uint32_t)clamp(p[P_E0], 0, DPRO_N - 1), w2 = (uint32_t)clamp(p[P_E1], 0, DPRO_N - 1);
    uint32_t sh = MBITS_SH[clamp(p[P_E6], 0, 6)];
    int32_t mix = p[P_E2] * 258, mix1;
    if (p[P_E3]) {                                       /* MMOD: a triangle 0.05 .. ~6 Hz, half the range either way */
        uint32_t lf = (uint32_t)v->s[0] + sx_hz_inc(sx_expmap(p[P_E3], 3277u, 7)) * n;
        int32_t tri = osc_tri(lf);
        v->s[0] = (int32_t)lf;
        mix = clamp(mix + (tri >> 1), 0, 32767);
    }
    mix1 = 32767 - mix;
    for (i = 0; i < n; i++) {
        int32_t y = mulq15(dpro_at(w1, ph1), mix1) + mulq15(dpro_at(w2, ph2), mix);
        ph1 += b;
        ph2 += b2;
        out[i] += voice_amp(clamp(y >> sh << sh, -32767, 32767), m, i);
    }
    v->ph[0] = ph1;
    v->ph[1] = ph2;
}

/* =========================================================== FM+ (all three) === */
static const char *const N_MFM_RAT[] = {"0.5", "1", "1.5", "2", "2.5", "3", "3.5", "4", "5", "6", "7", "8", "9", "10",
                                        "11", "12", "13", "14", "15", "16"};
static const uint16_t MFM_RAT[20] = {128, 256, 384, 512, 640, 768, 896, 1024, 1280, 1536, 1792, 2048, 2304, 2560,
                                     2816, 3072, 3328, 3584, 3840, 4096};   /* Q8 */
static const char *const N_MFM_ALG[] = {"PAR", "STACK"};
static inline uint32_t mfm_rinc(uint32_t b, int32_t k)  /* the carrier's increment at ratio k */
{
    uint64_t r = ((uint64_t)b * MFM_RAT[clamp(k, 0, 19)]) >> 8;
    return r > 0x7F000000u ? 0x7F000000u : (uint32_t)r;
}
/* the modulators' depth envelope (MDEC): s[7] Q15, 1 at the note, decaying (0: held) */
static void mfm_note_on(track_t *t, voice_t *v)
{
    (void)t;
    if (mf_fresh(v))
        v->ph[0] = v->ph[1] = v->ph[2] = 0;
    v->s[7] = 32767;
    v->s[5] = 32767;                                     /* (DYN: its pitch envelope) */
}
static int32_t mfm_env(voice_t *v, int32_t dec, uint32_t n)
{
    if (dec) {                                           /* ~20 ms .. ~6 s to a tenth */
        int32_t k = (int32_t)(32768u - 32768u * n / (uint32_t)(sx_expmap(dec, 900u << 16, 8) >> 16));
        v->s[7] = mulq15(v->s[7], clamp(k, 0, 32767));
    }
    return dec ? v->s[7] : 32767;
}
/* STATIC: ph[0] carrier, ph[1] A (RAT2 DEP2, FB), ph[2] B (RAT3 DEP3) into the carrier (PAR) or into A (STACK) */
static void mfmst_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t i, b = mf_inc(t, m), ia = mfm_rinc(b, p[P_E0]), ib = mfm_rinc(b, p[P_E2]), stack = p[P_E5] == 1;
    uint32_t pc = v->ph[0], pa = v->ph[1], pb = v->ph[2];
    int32_t env = mfm_env(v, p[P_E6], n), da = mulq15(p[P_E1] * 258, env), db = mulq15(p[P_E3] * 258, env);
    int32_t fb = p[P_E4] * 128, prev = v->s[0];
    for (i = 0; i < n; i++) {
        int32_t mb = sine_i(pb), ma, c;
        uint32_t oa = (uint32_t)((prev * fb) >> 15) << 16;
        if (stack)
            oa += (uint32_t)((mb * db) >> 15) << 19;   /* (up to 4 turns) */
        ma = sine_i(pa + oa);
        prev = (prev + ma) >> 1;
        c = sine_i(pc + ((uint32_t)((ma * da) >> 15) << 19) + (stack ? 0u : (uint32_t)((mb * db) >> 15) << 19));
        pc += b;
        pa += ia;
        pb += ib;
        out[i] += voice_amp(c, m, i);
    }
    v->ph[0] = pc;
    v->ph[1] = pa;
    v->ph[2] = pb;
    v->s[0] = prev;
}
/* PARALLEL: three modulators (ph[1], ph[2], s[1]) side by side into the carrier */
static void mfmpar_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t i, b = mf_inc(t, m), i1 = mfm_rinc(b, p[P_E0]), i2 = mfm_rinc(b, p[P_E2]), i3 = mfm_rinc(b, p[P_E4]);
    uint32_t pc = v->ph[0], p1 = v->ph[1], p2 = v->ph[2], p3 = (uint32_t)v->s[1];
    int32_t env = mfm_env(v, p[P_E6], n), d1 = mulq15(p[P_E1] * 258, env), d2 = mulq15(p[P_E3] * 258, env);
    int32_t d3 = mulq15(p[P_E5] * 258, env);
    for (i = 0; i < n; i++) {
        int32_t s = mulq15(sine_i(p1), d1) + mulq15(sine_i(p2), d2) + mulq15(sine_i(p3), d3);
        int32_t c = sine_i(pc + ((uint32_t)s << 18));    /* (each up to 2 turns) */
        pc += b;
        p1 += i1;
        p2 += i2;
        p3 += i3;
        out[i] += voice_amp(c, m, i);
    }
    v->ph[0] = pc;
    v->ph[1] = p1;
    v->ph[2] = p2;
    v->s[1] = (int32_t)p3;
}
/* 2^(x / 32768) for x -65536 .. 65536 (two octaves either way), Q15 x Q15 -> Q16; to ~0.3 % */
static inline uint32_t mfm_exp2q(int32_t x)
{
    int32_t i = x >> 15, f = x & 0x7FFF;                 /* integer octaves, the fraction Q15 */
    uint32_t y = 32768u + (uint32_t)((f * (21512 + ((f * 11256) >> 15))) >> 15);   /* 1 + f (0.6565 + 0.3435 f) */
    return i >= 0 ? y << (1 + i) : (y << 1) >> -i;
}
/* DYNAMIC: ph[0] carrier (FB on itself), ph[1] the linear modulator (LIN LRAT), ph[2] the exponential one (EXP ERAT);
 * s[5] the pitch envelope (PENV PDEC), s[0] the carrier's last output */
static void mfmdyn_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t i, b, il, ie, pc = v->ph[0], pl = v->ph[1], pe = v->ph[2];
    int32_t dl = p[P_E0] * 258, de = p[P_E2] * 258, fb = p[P_E4] * 128, prev = v->s[0];
    if (p[P_E6]) {                                       /* PDEC: the pitch envelope's fall, ~5 ms .. 3 s */
        int32_t k = (int32_t)(32768u - 32768u * n / (uint32_t)(sx_expmap(p[P_E6], 220u << 16, 9) >> 16));
        v->s[5] = mulq15(v->s[5], clamp(k, 0, 32767));
    } else {
        v->s[5] = 0;
    }
    b = mf_mul(mf_inc(t, m), mulq15(v->s[5], p[P_E5] * 2400 / 64));   /* PENV: up to two octaves either way */
    il = mfm_rinc(b, p[P_E1]);
    ie = mfm_rinc(b, p[P_E3]);
    for (i = 0; i < n; i++) {
        int32_t ml = sine_i(pl), me = sine_i(pe), c;
        uint32_t ci = (uint32_t)(((uint64_t)b * mfm_exp2q(mulq15(me, de) * 2)) >> 16);   /* EXP: up to +-2 octaves */
        c = sine_i(pc + ((uint32_t)((ml * dl) >> 15) << 19) + ((uint32_t)((prev * fb) >> 15) << 16));
        prev = (prev + c) >> 1;
        pc += ci;
        pl += il;
        pe += ie;
        out[i] += voice_amp(c, m, i);
    }
    v->ph[0] = pc;
    v->ph[1] = pl;
    v->ph[2] = pe;
    v->s[0] = prev;
}

/* ======================================================================= VO-6 === */
static const char *const N_MVO_VOW[] = {"A", "E", "I", "O", "U", "AE", "OE", "UE", "ER", "AH"};
static const uint16_t MVO_F[10][3] = {                   /* formants, Hz */
    {730, 1090, 2440}, {530, 1840, 2480}, {270, 2290, 3010}, {570, 840, 2410}, {300, 870, 2240},
    {660, 1720, 2410}, {500, 1500, 2400}, {280, 1800, 2200}, {490, 1350, 1690}, {640, 1190, 2390},
};
static const char *const N_MVO_CONS[] = {"--", "B", "D", "G", "P", "T", "K", "M", "N", "L", "R", "F", "S", "SH", "H",
                                         "V", "Z", "CH", "J", "TH"};
enum { CK_NONE, CK_STOP, CK_NASAL, CK_LIQUID, CK_FRIC, CK_AFFR };
static const struct {
    uint8_t kind, voiced;
    uint16_t f[3];                                       /* the loci (STOP, NASAL, LIQUID) or the noise's colour (FRIC) */
} MVO_C[20] = {
    {CK_NONE, 0, {0, 0, 0}},
    {CK_STOP, 1, {200, 900, 2200}}, {CK_STOP, 1, {200, 1700, 2600}}, {CK_STOP, 1, {200, 2200, 2400}},
    {CK_STOP, 0, {400, 900, 2200}}, {CK_STOP, 0, {400, 1700, 3500}}, {CK_STOP, 0, {400, 2200, 2800}},
    {CK_NASAL, 1, {250, 1100, 2300}}, {CK_NASAL, 1, {250, 1700, 2600}},
    {CK_LIQUID, 1, {360, 1300, 2900}}, {CK_LIQUID, 1, {420, 1300, 1600}},
    {CK_FRIC, 0, {1500, 3500, 5000}}, {CK_FRIC, 0, {2500, 4500, 6500}}, {CK_FRIC, 0, {1800, 2600, 4000}},
    {CK_FRIC, 0, {700, 1500, 2500}},
    {CK_FRIC, 1, {1500, 3500, 5000}}, {CK_FRIC, 1, {2500, 4500, 6500}},
    {CK_AFFR, 0, {1800, 2600, 4000}}, {CK_AFFR, 1, {1800, 2600, 4000}},
    {CK_FRIC, 0, {1400, 2800, 5500}},
};
/* a Chamberlin band-pass at f Hz, bandwidth bw Hz: its coefficients (Q15) */
static void mvo_coef(uint32_t f, uint32_t bw, int32_t *k, int32_t *q)
{
    uint32_t inc = sx_hz_inc(f << 16);                   /* 2 sin(pi f / fs) */
    int32_t s = sine_i(inc >> 1);
    *k = clamp(s * 2, 200, 30000);
    *q = clamp((int32_t)((bw << 15) / (f ? f : 1u)), 1200, 32767);
}
/* ph[0] the glottal saw, ph[1] the source's low-pass, s[0..5] the three band-passes (low, band), s[6] the samples since the
 * note, s[7] the noise's state */
static void mvo_note_on(track_t *t, voice_t *v)
{
    (void)t;
    v->s[6] = 0;
    if (mf_fresh(v)) {
        uint32_t k;
        for (k = 0; k < 6u; k++)
            v->s[k] = 0;
        v->ph[1] = 0;
        v->s[7] = (int32_t)(rng() | 1u);
    }
}
static void mvo_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t i, j, b = mf_inc(t, m), cons = (uint32_t)clamp(p[P_E0], 0, 19), kind = MVO_C[cons].kind;
    uint32_t v1 = (uint32_t)clamp(p[P_E3], 0, 9), v2 = (uint32_t)clamp(p[P_E4], 0, 9), seed = (uint32_t)v->s[7];
    int32_t clen = 441 + p[P_E1] * 87, at = v->s[6], f[3], kk[3], qq[3], av, an, morf, tv, glp = (int32_t)v->ph[1];
    int32_t brth = p[P_E6] * 120, cvol = p[P_E2] * 258;
    /* where the voice is: the consonant (0 .. clen), then the vowel gliding VOW1 -> VOW2 over MORF */
    if (!cons || at >= clen) {
        int32_t mt = p[P_E5] ? 441 + p[P_E5] * 600 : 0;   /* MORF: 10 ms .. ~1.7 s, 0: VOW1 held */
        morf = mt ? clamp((int32_t)(((int64_t)(at - (cons ? clen : 0)) << 15) / mt), 0, 32767) : 0;
        for (j = 0; j < 3u; j++)
            f[j] = MVO_F[v1][j] + mulq15((int32_t)MVO_F[v2][j] - (int32_t)MVO_F[v1][j], morf);
        if (cons && (kind == CK_STOP || kind == CK_NASAL || kind == CK_LIQUID) && at < clen * 2) {
            tv = (int32_t)(((int64_t)(at - clen) << 15) / clen);   /* the transition from the locus, one more clen */
            for (j = 0; j < 3u; j++)
                f[j] = MVO_C[cons].f[j] + mulq15(f[j] - (int32_t)MVO_C[cons].f[j], tv);
        }
        av = 32767;
        an = brth;
    } else {                                             /* the consonant */
        int32_t ph = (int32_t)(((int64_t)at << 15) / clen);
        for (j = 0; j < 3u; j++)
            f[j] = MVO_C[cons].f[j];
        switch (kind) {
        case CK_STOP:                                    /* closure, then the burst at the end */
            av = MVO_C[cons].voiced ? 6000 : 0;
            an = ph > 22000 ? mulq15(cvol, 32767 - (ph - 22000) * 3) : 0;
            break;
        case CK_NASAL:
            av = mulq15(cvol, 20000);
            an = 0;
            break;
        case CK_LIQUID:
            av = mulq15(cvol, 30000);
            an = 0;
            break;
        case CK_AFFR:                                    /* a stop into a fricative */
            av = MVO_C[cons].voiced ? 8000 : 0;
            an = ph > 9000 ? cvol : 0;
            break;
        default:                                         /* FRIC */
            av = MVO_C[cons].voiced ? 14000 : 0;
            an = cvol;
            break;
        }
    }
    for (j = 0; j < 3u; j++)
        mvo_coef((uint32_t)clamp(f[j], 80, 7000), j == 0 ? 80u : j == 1 ? 110u : 160u, &kk[j], &qq[j]);
    if (!(!cons || at >= clen) && kind == CK_FRIC)       /* (fricative noise: broad bands) */
        for (j = 0; j < 3u; j++)
            qq[j] = clamp(qq[j] * 4, 1200, 32767);
    for (i = 0; i < n; i++) {
        int32_t src, y = 0;
        int32_t g = osc_saw(v->ph[0], b);
        v->ph[0] += b;
        glp += (g - glp) >> 2;                           /* the glottal pulse a little rounded */
        src = mulq15(glp, av) + mulq15(sx_noise(&seed) >> 5, an);
        for (j = 0; j < 3u; j++) {                       /* three band-passes in parallel */
            int32_t lo = v->s[2u * j], bd = v->s[2u * j + 1u], hi;
            hi = src - lo - px_m(bd, qq[j], 15);
            bd = clamp(bd + px_m(hi, kk[j], 15), -400000, 400000);
            lo = clamp(lo + px_m(bd, kk[j], 15), -400000, 400000);
            v->s[2u * j] = lo;
            v->s[2u * j + 1u] = bd;
            y += j == 0 ? bd : j == 1 ? (bd * 3) >> 2 : bd >> 1;
        }
        out[i] += voice_amp(clamp(soft_knee(y >> 2, 26000), -32767, 32767), m, i);
    }
    v->ph[1] = (uint32_t)glp;
    v->s[6] = at + (int32_t)n;
    v->s[7] = (int32_t)seed;
}

/* ================================================================ GND SINE, NOISE === */
/* SINE: ph[0] the sine, ph[1] SUB's */
static void msine_note_on(track_t *t, voice_t *v)
{
    (void)t;
    if (mf_fresh(v))
        v->ph[0] = v->ph[1] = 0;
}
static void msine_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t i, b = mf_inc(t, m), ph = v->ph[0], ps = v->ph[1];
    int32_t fold = p[P_E0] * 258, sub = p[P_E1] * 258, g = mf_norm(32767 + sub / 2);
    for (i = 0; i < n; i++) {
        int32_t y = sine_i(ph);
        if (fold) {                                      /* up to 4 x, folded back */
            y += px_m(y, fold * 3, 15);
            while (y > 32767 || y < -32767)
                y = y > 32767 ? 65534 - y : -65534 - y;
        }
        y += mulq15(sine_i(ps), sub);
        ph += b;
        ps += b >> 1;
        out[i] += voice_amp(mulq15(y, g), m, i);
    }
    v->ph[0] = ph;
    v->ph[1] = ps;
}
/* NOISE: ph[0] the hold's phase (the note's rate), s[0] the noise, s[1] the held sample, s[2] COLR's low-pass */
static void mnoise_note_on(track_t *t, voice_t *v)
{
    (void)t;
    if (!v->s[0])
        v->s[0] = (int32_t)(rng() | 1u);
}
static void mnoise_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t i, ph = v->ph[0], seed = (uint32_t)v->s[0], b = mf_inc(t, m) << 3;   /* (held 8 x the note's pitch) */
    int32_t rate = p[P_E0], held = v->s[1], lp = v->s[2], k = 1200 + (127 - p[P_E1]) * 250;
    for (i = 0; i < n; i++) {
        uint32_t was = ph;
        ph += b;
        if (!rate || ph < was || (int32_t)(sx_noise(&seed) & 127) >= rate)
            held = sx_noise(&seed) >> 5;                 /* RATE: how often the note's clock holds it (0: white) */
        lp += px_m(held - lp, k, 15);
        out[i] += voice_amp(clamp(lp, -32767, 32767), m, i);
    }
    v->ph[0] = ph;
    v->s[0] = (int32_t)seed;
    v->s[1] = held;
    v->s[2] = lp;
}

/* ================================================================== presets === */
/* name, {E1 .. E8}, {A D S R}, fenv, mono, FX(dist, chorus, delay, reverb); categories a letter a preset (category.c) */
#define MONO_CAT_MSAW "BLPLBL"
static const preset_t MSAW_PRESETS[] = {
    {"SUPER SAW", {110, 60, 90, 20, 0, 0, 0, 0}, {0, 64, 127, 40}, 0, 1, FX(0, 20, 20, 40)},
    {"SAW LEAD", {50, 30, 0, 10, 0, 40, 0, 0}, {0, 64, 127, 30}, 0, 1, FX(10, 0, 30, 30)},
    {"HOOVER", {127, 110, 127, 60, 90, 60, 0, 0}, {10, 64, 127, 60}, 0, 1, FX(20, 30, 20, 50)},
    {"WIDE PAD", {90, 40, 70, 70, 0, 50, 20, 0}, {90, 64, 127, 110}, 0, 0, FX(0, 40, 30, 110)},
    {"SUB SAW", {0, 0, 0, 0, 80, 110, 60, 0}, {0, 64, 127, 20}, 0, 1, FX(10, 0, 0, 10)},
    {"OLD SYNTH", {70, 20, 30, 110, 40, 0, 0, 0}, {5, 64, 127, 50}, 0, 1, FX(0, 20, 20, 50)},
};
#define MONO_CAT_MPULSE "BLLPBK"
static const preset_t MPULSE_PRESETS[] = {
    {"PULSE BASS", {0, 0, 90, 30, 40, 0, 0, 0}, {0, 64, 127, 20}, 0, 1, FX(10, 0, 0, 10)},
    {"PWM LEAD", {70, 25, 0, 0, 64, 50, 0, 0}, {0, 64, 127, 40}, 0, 1, FX(0, 10, 30, 30)},
    {"SPIKE", {40, 10, 0, 0, 120, 0, 0, 0}, {0, 64, 127, 30}, 0, 1, FX(20, 0, 20, 30)},
    {"PWM STRINGS", {100, 50, 0, 40, 64, 30, 0, 0}, {80, 64, 127, 110}, 0, 0, FX(0, 40, 30, 110)},
    {"SQUARE SUB", {0, 0, 120, 60, 64, 0, 0, 0}, {0, 64, 127, 20}, 0, 1, FX(0, 0, 0, 10)},
    {"CLAV SWEEP", {30, 10, 0, 0, 100, 90, 1, 0}, {0, 40, 40, 30}, 0, 1, FX(0, 0, 20, 30)},
};
#define MONO_CAT_MENS "PPKLPP"
static const preset_t MENS_PRESETS[] = {
    {"MINOR CHORD", {15, 19, 0, 0, 64, 90, 60, 0}, {40, 64, 127, 90}, 0, 1, FX(0, 20, 30, 90)},
    {"MAJOR JUST", {26, 28, 0, 0, 64, 90, 60, 0}, {40, 64, 127, 90}, 0, 1, FX(0, 20, 30, 90)},
    {"STAB 7TH", {15, 19, 22, 80, 64, 60, 40, 0}, {0, 50, 60, 40}, 0, 1, FX(10, 0, 30, 50)},
    {"FIFTH LEAD", {28, 0, 0, 40, 40, 50, 30, 0}, {0, 64, 127, 40}, 0, 1, FX(10, 0, 30, 40)},
    {"OCTAVE PAD", {1, 25, 0, 0, 64, 127, 90, 0}, {90, 64, 127, 120}, 0, 1, FX(0, 30, 30, 120)},
    {"SUS4 SPIKES", {27, 28, 0, 127, 110, 80, 50, 0}, {20, 64, 127, 90}, 0, 1, FX(0, 20, 40, 100)},
};
#define MONO_CAT_MSID "BLLFLB"
static const preset_t MSID_PRESETS[] = {
    {"SID BASS", {5, 40, 0, 0, 0, 0, 0, 0}, {0, 64, 127, 20}, 0, 1, FX(10, 0, 0, 10)},
    {"C64 LEAD", {2, 64, 40, 0, 0, 0, 0, 0}, {0, 64, 127, 40}, 0, 1, FX(0, 0, 30, 30)},
    {"RING TONE", {0, 64, 0, 1, 7, 0, 0, 0}, {0, 64, 127, 40}, 0, 1, FX(0, 0, 20, 40)},
    {"NOISE HIT", {3, 64, 0, 0, 0, 40, 0, 0}, {0, 30, 0, 20}, 0, 1, FX(0, 0, 10, 30)},
    {"SYNC SCREAM", {1, 64, 0, 2, 19, 0, 0, 0}, {0, 64, 127, 40}, 0, 1, FX(20, 0, 30, 30)},
    {"GRIT BASS", {6, 30, 20, 0, 0, 70, 20, 0}, {0, 64, 127, 20}, 0, 1, FX(20, 0, 0, 10)},
};
#define MONO_CAT_MWAVE "LPKFBL"
static const preset_t MWAVE_PRESETS[] = {
    {"RAW DIGI", {9, 0, 0, 0, 0, 0, 0, 0}, {0, 64, 127, 40}, 0, 1, FX(0, 0, 20, 30)},
    {"VOX MORPH", {11, 0, 40, 1, 0, 0, 0, 0}, {40, 64, 127, 90}, 0, 1, FX(0, 20, 30, 90)},
    {"ORGAN 12BIT", {8, 0, 0, 0, 0, 1, 0, 0}, {0, 64, 127, 30}, 0, 1, FX(0, 20, 0, 40)},
    {"WAVE SCAN", {16, 0, 70, 1, 0, 2, 0, 0}, {20, 64, 127, 80}, 0, 1, FX(0, 0, 40, 70)},
    {"SYNC BASS", {2, 0, 0, 0, 14, 1, 0, 0}, {0, 64, 127, 20}, 0, 1, FX(10, 0, 0, 10)},
    {"LOFI FM", {24, 60, 0, 0, 0, 3, 20, 0}, {0, 64, 127, 40}, 0, 1, FX(10, 0, 30, 40)},
};
#define MONO_CAT_MDDRAW "PLKF"
static const preset_t MDDRAW_PRESETS[] = {
    {"DOUBLE PAD", {9, 12, 64, 30, 10, 2, 0, 0}, {80, 64, 127, 110}, 0, 1, FX(0, 30, 30, 110)},
    {"OCT LEAD", {2, 3, 50, 0, 6, 3, 0, 0}, {0, 64, 127, 40}, 0, 1, FX(0, 0, 30, 40)},
    {"BELL ORGAN", {8, 16, 60, 0, 0, 3, 1, 0}, {0, 64, 100, 60}, 0, 1, FX(0, 20, 20, 60)},
    {"CHAOS DRONE", {31, 26, 64, 60, 20, 1, 2, 0}, {90, 64, 127, 120}, 0, 1, FX(10, 20, 40, 120)},
};
#define MONO_CAT_MFMST "KBUL"
static const preset_t MFMST_PRESETS[] = {
    {"FM PIANO", {1, 50, 13, 20, 10, 0, 60, 0}, {0, 80, 40, 50}, 0, 1, FX(0, 20, 10, 50)},
    {"FM BASS", {1, 70, 3, 30, 30, 1, 40, 0}, {0, 64, 127, 20}, 0, 1, FX(10, 0, 0, 10)},
    {"FM BELL", {9, 60, 13, 40, 0, 0, 90, 0}, {0, 90, 0, 90}, 0, 1, FX(0, 20, 30, 90)},
    {"FB LEAD", {1, 40, 3, 0, 90, 0, 0, 0}, {0, 64, 127, 40}, 0, 1, FX(10, 0, 30, 40)},
};
#define MONO_CAT_MFMPAR "KUPF"
static const preset_t MFMPAR_PRESETS[] = {
    {"FM ORGAN", {1, 40, 3, 30, 5, 20, 0, 0}, {0, 64, 127, 30}, 0, 1, FX(0, 30, 0, 40)},
    {"GLASS", {7, 50, 13, 30, 19, 20, 80, 0}, {0, 80, 20, 80}, 0, 1, FX(0, 20, 30, 90)},
    {"FM PAD", {1, 30, 5, 20, 3, 10, 0, 0}, {80, 64, 127, 110}, 0, 1, FX(0, 40, 30, 110)},
    {"METALLIC", {8, 90, 12, 80, 17, 70, 50, 0}, {0, 64, 60, 60}, 0, 1, FX(10, 0, 30, 60)},
};
#define MONO_CAT_MFMDYN "DBLF"
static const preset_t MFMDYN_PRESETS[] = {
    {"FM KICK", {40, 1, 0, 1, 0, 50, 30, 0}, {0, 40, 0, 20}, 0, 1, FX(20, 0, 0, 10)},
    {"WOBBLE BASS", {20, 1, 50, 0, 20, 0, 0, 0}, {0, 64, 127, 20}, 0, 1, FX(20, 0, 0, 10)},
    {"SCREAM", {60, 3, 30, 5, 80, 10, 60, 0}, {0, 64, 127, 40}, 0, 1, FX(20, 0, 30, 40)},
    {"LASER", {30, 7, 40, 9, 20, 60, 50, 0}, {0, 50, 30, 40}, 0, 1, FX(0, 0, 40, 50)},
};
#define MONO_CAT_MVO "LLPF"
static const preset_t MVO_PRESETS[] = {
    {"TALKING", {7, 30, 90, 0, 3, 40, 10, 0}, {0, 64, 127, 40}, 0, 1, FX(0, 0, 20, 40)},
    {"ROBOT YES", {18, 40, 100, 1, 5, 50, 20, 0}, {0, 64, 127, 40}, 0, 1, FX(10, 0, 30, 40)},
    {"CHOIR OOH", {0, 0, 0, 4, 0, 0, 30, 0}, {80, 64, 127, 110}, 0, 0, FX(0, 40, 30, 110)},
    {"WHISPER", {12, 60, 110, 0, 4, 60, 127, 0}, {30, 64, 127, 80}, 0, 1, FX(0, 20, 30, 90)},
};
#define MONO_CAT_MSINE "BBP"
static const preset_t MSINE_PRESETS[] = {
    {"SINE", {0, 0, 0, 0, 0, 0, 0, 0}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 20)},
    {"SUB SINE", {0, 90, 0, 0, 0, 0, 0, 0}, {0, 64, 127, 20}, 0, 1, FX(0, 0, 0, 10)},
    {"FOLD PAD", {50, 30, 0, 0, 0, 0, 0, 0}, {80, 64, 127, 110}, 0, 0, FX(0, 30, 30, 110)},
};
#define MONO_CAT_MNOISE "FDF"
static const preset_t MNOISE_PRESETS[] = {
    {"WHITE", {0, 0, 0, 0, 0, 0, 0, 0}, {0, 64, 127, 30}, 0, 1, FX(0, 0, 0, 30)},
    {"DIGI NOISE", {100, 30, 0, 0, 0, 0, 0, 0}, {0, 30, 0, 20}, 0, 1, FX(0, 0, 20, 30)},
    {"DARK WIND", {0, 110, 0, 0, 0, 0, 0, 0}, {90, 64, 127, 110}, 0, 0, FX(0, 20, 30, 110)},
};

/* ================================================================== engines === */
#define MF_ENGINE(nm, t1, t2, pre, on, ren, k0, k1, k2, k3) \
    .name = nm, .page_title = {t1, t2}, .presets = pre, .npresets = NELEM(pre), .note_on = on, .render = ren, \
    .knob = {k0, k1, k2, k3}, .poly = MF_POLY
static const engine_t ENG_MSAW = {
    MF_ENGINE("SAW", "SAW", "SUBS", MSAW_PRESETS, msaw_note_on, msaw_render, P_E0, P_E1, P_E2, P_E4),
    .edit = {{"UNIL", F_PCT, 0, 127, 0, 0, 0}, {"UNIW", F_PCT, 0, 127, 40, 0, 0}, {"UNXL", F_PCT, 0, 127, 0, 0, 0},
             {"AGE", F_PCT, 0, 127, 0, 0, 0}, {"SUBX", F_PCT, 0, 127, 0, 0, 0}, {"SUB1", F_PCT, 0, 127, 0, 0, 0},
             {"SUB2", F_PCT, 0, 127, 0, 0, 0}, MF_TUNE},
    .xyd = {{XY_E(0), XY_E(1), XY_E(2)}, {XY_E(4), XY_E(5), XY_E(3)}},   /* X: the unison (the width), Y: the subs, AGE */
    .xya = {{50, 40, 40}, {50, 40, 40}},
};
static const engine_t ENG_MPULSE = {
    MF_ENGINE("PULSE", "PULSE", "WIDTH", MPULSE_PRESETS, mpulse_note_on, mpulse_render, P_E0, P_E4, P_E5, P_E2),
    .edit = {{"UNIL", F_PCT, 0, 127, 0, 0, 0}, {"UNIW", F_PCT, 0, 127, 40, 0, 0}, {"SUB1", F_PCT, 0, 127, 0, 0, 0},
             {"SUB2", F_PCT, 0, 127, 0, 0, 0}, {"PW", F_BIPCT, 0, 127, 64, 0, 0}, {"PWAD", F_PCT, 0, 127, 0, 0, 0},
             {"PWRS", F_ENUM, 0, 1, 0, N_MF_ONOFF, 0}, MF_TUNE},
    .xyd = {{XY_E(4), XY_E(0), 0}, {XY_E(5), XY_E(1), XY_E(2)}},   /* X: PW, the unison; Y: the sweep, the width, SUB1 */
    .xya = {{50, 40, 0}, {50, 30, 30}},
};
static const engine_t ENG_MENS = {
    .name = "ENS", .page_title = {"CHORD", "WAVE"}, .presets = MENS_PRESETS, .npresets = NELEM(MENS_PRESETS),
    .note_on = mens_note_on, .render = mens_render, .knob = {P_E0, P_E3, P_E4, P_E5},
    .edit = {{"PCH2", F_ENUM, 0, 28, 0, N_MENS_PCH, 0}, {"PCH3", F_ENUM, 0, 28, 0, N_MENS_PCH, 0},
             {"PCH4", F_ENUM, 0, 28, 0, N_MENS_PCH, 0}, {"WAVE", F_PCT, 0, 127, 0, 0, 0},
             {"PW", F_PCT, 0, 127, 64, 0, 0}, {"CHRL", F_PCT, 0, 127, 0, 0, 0}, {"CHRW", F_PCT, 0, 127, 40, 0, 0},
             MF_TUNE},
    .poly = 2,
    .mem = sizeof(mens_slot_t) * NVOICE,
    .xyd = {{XY_E(3), XY_E(4), 0}, {XY_E(5), XY_E(6), 0}},   /* X: the wave (saw .. spikes), Y: the chorus */
    .xya = {{50, 40, 0}, {50, 40, 0}},
};
static const engine_t ENG_MSID = {
    .name = "SID", .page_title = {"OSC", "MOD"}, .presets = MSID_PRESETS, .npresets = NELEM(MSID_PRESETS),
    .note_on = msid_note_on, .render = msid_render, .knob = {P_E0, P_E1, P_E3, P_E5},
    .edit = {{"WAVE", F_ENUM, 0, 7, 2, N_MSID_WAVE, 0}, {"PW", F_PCT, 0, 127, 64, 0, 0}, {"PWAD", F_PCT, 0, 127, 0, 0, 0},
             {"MOD", F_ENUM, 0, 3, 0, N_MSID_MOD, 0}, {"MRAT", F_INT, -24, 24, 7, 0, "st"},
             {"GRIT", F_PCT, 0, 127, 0, 0, 0}, {"NOIS", F_PCT, 0, 127, 0, 0, 0}, MF_TUNE},
    .poly = 3,
    .xyd = {{XY_E(1), XY_E(2), 0}, {XY_E(4), XY_E(5), XY_E(6)}},   /* X: the width and its sweep, Y: MRAT GRIT NOIS */
    .xya = {{50, 40, 0}, {40, 40, 30}},
};
static const engine_t ENG_MWAVE = {
    MF_ENGINE("WAVE", "WAVE", "CRUSH", MWAVE_PRESETS, mwave_note_on, mwave_render, P_E0, P_E1, P_E2, P_E4),
    .edit = {{"WAVE", F_ENUM, 0, DPRO_N - 1, 0, DPRO_NAMES, 0}, {"MORF", F_PCT, 0, 127, 0, 0, 0},
             {"MSPD", F_PCT, 0, 127, 0, 0, 0}, {"MRST", F_ENUM, 0, 1, 0, N_MF_ONOFF, 0},
             {"SYNC", F_INT, 0, 48, 0, 0, "st"}, {"BITS", F_ENUM, 0, 6, 0, N_MBITS, 0}, {"DEC", F_PCT, 0, 127, 0, 0, 0},
             MF_TUNE},
    .xyd = {{XY_E(1), XY_E(2), 0}, {XY_E(4), XY_E(6), 0}},   /* X: the morph and its sweep, Y: SYNC and the rate */
    .xya = {{60, 30, 0}, {50, 30, 0}},
};
static const engine_t ENG_MDDRAW = {
    MF_ENGINE("DDRAW", "DRAW", "MIX", MDDRAW_PRESETS, mddraw_note_on, mddraw_render, P_E0, P_E1, P_E2, P_E3),
    .edit = {{"WAV1", F_ENUM, 0, DPRO_N - 1, 0, DPRO_NAMES, 0}, {"WAV2", F_ENUM, 0, DPRO_N - 1, 2, DPRO_NAMES, 0},
             {"MIX", F_PCT, 0, 127, 64, 0, 0}, {"MMOD", F_PCT, 0, 127, 0, 0, 0}, {"DTUN", F_INT, -64, 63, 0, 0, 0},
             {"OCT2", F_ENUM, 0, 4, 2, N_MOCT2, 0}, {"BITS", F_ENUM, 0, 6, 0, N_MBITS, 0}, MF_TUNE},
    .xyd = {{XY_E(2), XY_E(4), 0}, {XY_E(3), XY_E(6), 0}},   /* X: the mix, the detune; Y: the LFO, the bits */
    .xya = {{60, 30, 0}, {50, 30, 0}},
};
static const engine_t ENG_MFMST = {
    MF_ENGINE("FM-ST", "MODS", "FB", MFMST_PRESETS, mfm_note_on, mfmst_render, P_E1, P_E3, P_E4, P_E6),
    .edit = {{"RAT2", F_ENUM, 0, 19, 1, N_MFM_RAT, 0}, {"DEP2", F_PCT, 0, 127, 40, 0, 0},
             {"RAT3", F_ENUM, 0, 19, 3, N_MFM_RAT, 0}, {"DEP3", F_PCT, 0, 127, 0, 0, 0}, {"FB", F_PCT, 0, 127, 0, 0, 0},
             {"ALGO", F_ENUM, 0, 1, 0, N_MFM_ALG, 0}, {"MDEC", F_PCT, 0, 127, 0, 0, 0}, MF_TUNE},
    .xyd = {{XY_E(1), XY_E(3), 0}, {XY_E(4), XY_E(6), 0}},   /* X: the depths, Y: FB, the decay */
    .xya = {{50, 40, 0}, {50, 30, 0}},
};
static const engine_t ENG_MFMPAR = {
    MF_ENGINE("FM-PAR", "MODS", "MOD 3", MFMPAR_PRESETS, mfm_note_on, mfmpar_render, P_E1, P_E3, P_E5, P_E6),
    .edit = {{"RAT2", F_ENUM, 0, 19, 1, N_MFM_RAT, 0}, {"LEV2", F_PCT, 0, 127, 40, 0, 0},
             {"RAT3", F_ENUM, 0, 19, 3, N_MFM_RAT, 0}, {"LEV3", F_PCT, 0, 127, 0, 0, 0},
             {"RAT4", F_ENUM, 0, 19, 5, N_MFM_RAT, 0}, {"LEV4", F_PCT, 0, 127, 0, 0, 0}, {"MDEC", F_PCT, 0, 127, 0, 0, 0},
             MF_TUNE},
    .xyd = {{XY_E(1), XY_E(3), XY_E(5)}, {XY_E(6), 0, 0}},   /* X: the three levels, Y: the decay */
    .xya = {{40, 40, 40}, {50, 0, 0}},
};
static const engine_t ENG_MFMDYN = {
    MF_ENGINE("FM-DYN", "FM", "PITCH", MFMDYN_PRESETS, mfm_note_on, mfmdyn_render, P_E0, P_E2, P_E4, P_E5),
    .edit = {{"LIN", F_PCT, 0, 127, 30, 0, 0}, {"LRAT", F_ENUM, 0, 19, 1, N_MFM_RAT, 0}, {"EXP", F_PCT, 0, 127, 0, 0, 0},
             {"ERAT", F_ENUM, 0, 19, 1, N_MFM_RAT, 0}, {"FB", F_PCT, 0, 127, 0, 0, 0}, {"PENV", F_INT, -64, 63, 0, 0, 0},
             {"PDEC", F_PCT, 0, 127, 40, 0, 0}, MF_TUNE},
    .xyd = {{XY_E(0), XY_E(2), 0}, {XY_E(4), XY_E(5), 0}},   /* X: the two FMs, Y: FB and the pitch envelope */
    .xya = {{50, 40, 0}, {50, 30, 0}},
};
static const engine_t ENG_MVO = {
    MF_ENGINE("VO-6", "VOICE", "VOWEL", MVO_PRESETS, mvo_note_on, mvo_render, P_E0, P_E3, P_E4, P_E5),
    .edit = {{"CONS", F_ENUM, 0, 19, 7, N_MVO_CONS, 0}, {"CLEN", F_PCT, 0, 127, 40, 0, 0},
             {"CVOL", F_PCT, 0, 127, 90, 0, 0}, {"VOW1", F_ENUM, 0, 9, 0, N_MVO_VOW, 0},
             {"VOW2", F_ENUM, 0, 9, 3, N_MVO_VOW, 0}, {"MORF", F_PCT, 0, 127, 40, 0, 0},
             {"BRTH", F_PCT, 0, 127, 10, 0, 0}, MF_TUNE},
    .xyd = {{XY_E(3), XY_E(4), 0}, {XY_E(5), XY_E(6), XY_E(1)}},   /* X: the vowels, Y: the glide, breath, CLEN */
    .xya = {{40, 40, 0}, {50, 40, 30}},
};
static const engine_t ENG_MSINE = {
    MF_ENGINE("SINE", "SINE", "-", MSINE_PRESETS, msine_note_on, msine_render, P_E0, P_E1, P_E7, P_REL),
    .edit = {{"FOLD", F_PCT, 0, 127, 0, 0, 0}, {"SUB", F_PCT, 0, 127, 0, 0, 0}, MF_NONE, MF_NONE, MF_NONE, MF_NONE,
             MF_NONE, MF_TUNE},
    .xyd = {{XY_E(0), 0, 0}, {XY_E(1), 0, 0}},
    .xya = {{60, 0, 0}, {60, 0, 0}},
};
static const engine_t ENG_MNOISE = {
    MF_ENGINE("NOISE", "NOISE", "-", MNOISE_PRESETS, mnoise_note_on, mnoise_render, P_E0, P_E1, P_REL, P_LEVEL),
    .edit = {{"RATE", F_PCT, 0, 127, 0, 0, 0}, {"COLR", F_PCT, 0, 127, 0, 0, 0}, MF_NONE, MF_NONE, MF_NONE, MF_NONE,
             MF_NONE, MF_TUNE},
    .xyd = {{XY_E(0), 0, 0}, {XY_E(1), 0, 0}},
    .xya = {{60, 0, 0}, {60, 0, 0}},
};
#undef MF_ENGINE
