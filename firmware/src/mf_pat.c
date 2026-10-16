/* SPDX-License-Identifier: GPL-3.0-only */
/* MONOFM: THE PATTERNS (MONOFM.md), Elektron's model. One project of 16 patterns A01..A16, all in RAM (mfp_bank): a
 * pattern is the four tracks' sounds (engine, preset, every parameter) and their trigs (32 steps, chords of up to 4
 * notes, each step's VEL LEN condition, accent, slide, ratchet, nudge), the p-locks and automation (motion), and its
 * master length. The pattern playing lives in the tracks themselves (trk[]: what the sequencer, the pages and the
 * editor already use); mfp_pack copies it back into its bank slot, mfp_unpack brings another one in. So every pattern
 * keeps its edits when another one plays: nothing has to be saved to switch.
 * Changing (SEQ held: ALGORITHM picks, let go of SEQ: it is asked for; ui_layer.c), as Elektron's three modes (the
 * project's, SEQ + F#3): SEQUENTIAL at the end of the pattern playing (its master length; AUTO: its longest track's
 * loop), every track from step 1; DIRECT START at once, from step 1; DIRECT JUMP at once, the tracks where they were.
 * Stopped: at once. A chain (SEQ held, OCT+ adds the picked pattern; up to 16) plays in turn, at each pattern's end.
 * The audio ISR switches (mf_pat_tick: pack the old one, unpack the new one; no flash, no wait), the main loop does the
 * rest (mf_pat_poll). A master length set (not AUTO) also starts every track again from step 1 at its end.
 * Storage: the project in flash, two copies (A / B, the older one written: a power cut keeps the other) and a working
 * copy (W: the autosave, stopped and idle, so nothing is lost at power-off); each 8 sectors, a header (sequence number,
 * CRC) written last. SAVE (SAVE > PROJECT, SEQ + F#5 held) writes A / B, stopped only (an erase silences the audio);
 * while playing it is done at STOP. Power-on: the newest valid copy (W, else A / B). RELOAD: the pattern, or all, as
 * last SAVEd. Flash: USR2 / USR3's (eng_sample.c SMP_USER_LIVE: those sample slots are off in MONOFM). */
#define MFP_N 16u
#define MFP_STEPS 32u
#define MFP_NONE 0xFFu
#define MFP_MAGIC 0x3150464Du                             /* "MFP1" */
#define MFP_VERSION 2u                                    /* (2: a trig's LEN, the tracks' TRN / SHIFT; 1 read) */
#define MFP_A 0xB4000u
#define MFP_B 0xBC000u
#define MFP_W 0xC4000u
#define MFP_SPAN 0x8000u                                   /* 8 sectors each */
enum { MFP_SEQ, MFP_START, MFP_JUMP };                    /* the change modes */
typedef struct {
    uint8_t note[4];
    uint8_t nt;                                           /* n (bits 0..2) | time << 3 */
    uint8_t flags, vel, prob;
    uint8_t len;                                          /* the trig's LEN (core.h MF_LEN8; 0 the track's GATE) */
} mfp_step_t;
typedef struct {
    uint8_t eng, preset;
    int8_t trn, shift;                                    /* SEQ's TRN / SHIFT on the track so far (semitones, steps) */
    int8_t p[P_COUNT];                                    /* every parameter (-64..127, as user presets keep them) */
    uint8_t pad[(4u - (P_COUNT & 3u)) & 3u];
    mfp_step_t step[MFP_STEPS];
} mfp_trk_t;
typedef struct {
    uint8_t mlen;                                         /* master length in 1/16 steps (1X), 0 = AUTO */
    uint8_t rsv[3];
    mfp_trk_t t[NTRK];
    motion_store_t motion;                                /* (its places: track << 6 | step, steps < 32) */
} mfp_pat_t;
typedef struct {
    uint32_t magic, version, seq, size, npat, mode, crc, rsv[8], hcrc;
} mfp_hdr_t;
_Static_assert(sizeof(mfp_hdr_t) == 64u, "MONOFM project header");
#define MFP_LO 7u                                         /* patterns in .noinit, the rest in .bss */
_Static_assert(MFP_LO * sizeof(mfp_pat_t) <= 0x3D50u, "7 patterns fit .noinit (app.ld NOINIT, proj_slot gone in MONOFM)");
/* version 1 (8-byte steps, no LEN): read and brought over */
typedef struct { uint8_t note[4], nt, flags, vel, prob; } mfp_step1_t;
typedef struct {
    uint8_t eng, preset, rsv[2];
    int8_t p[P_COUNT];
    uint8_t pad[(4u - (P_COUNT & 3u)) & 3u];
    mfp_step1_t step[MFP_STEPS];
} mfp_trk1_t;
typedef struct { uint8_t mlen, rsv[3]; mfp_trk1_t t[NTRK]; motion_store_t motion; } mfp_pat1_t;
_Static_assert(64u + MFP_N * sizeof(mfp_pat_t) <= MFP_SPAN, "the project fits its 8 sectors");
_Static_assert(MFP_W + MFP_SPAN <= 0xDC000u, "the project's copies stay below the user presets");

/* half of the bank in .noinit (proj_slot's room: MONOFM has no project slots), half in .bss */
static mfp_pat_t mfp_lo[MFP_LO] __attribute__((section(".noinit")));
static mfp_pat_t mfp_hi[MFP_N - MFP_LO];
static mfp_pat_t *mfp_at(uint32_t i) { i %= MFP_N; return i < MFP_LO ? &mfp_lo[i] : &mfp_hi[i - MFP_LO]; }
static int8_t mfp_trn[NTRK], mfp_shift[NTRK];             /* the pattern playing's tracks' TRN / SHIFT so far */
static mfp_pat_t mfp_clip;                                /* COPY / PASTE (a track: its t[0]) */
static uint8_t mfp_clip_kind;                             /* 0 empty, 1 a track, 2 a pattern */

static struct {
    volatile uint8_t cur;                                 /* the pattern in the tracks */
    volatile uint8_t next;                                /* asked for, MFP_NONE none */
    uint8_t chain[MFP_N], nchain, cpos;                   /* the chain playing (nchain >= 2), cpos: cur's place */
    uint8_t mode;                                         /* MFP_SEQ START JUMP (the project's) */
    volatile uint8_t fired;                               /* the ISR switched: the main loop's part */
    volatile uint8_t start;                               /* PLAY: the pattern's clock from 0 */
    uint8_t pick, picked;                                 /* SEQ held: the pattern ALGORITHM points at; turned */
    uint8_t pchain[MFP_N], npchain;                       /* .. the chain being built (OCT+) */
    uint8_t save_wait;                                    /* SAVE asked while playing: at STOP */
    uint8_t err;                                          /* the last write failed */
    uint32_t pos;                                         /* samples into the master length */
    uint32_t gen;                                         /* + 1 on any change of the bank (its signature) */
    uint32_t saved, wsaved, seen, t_seen, t_w;            /* signatures: SAVEd, autosaved, seen last; times */
} mfp = {0, MFP_NONE, {0}, 0, 0, MFP_SEQ, 0, 0, MFP_NONE, 0, {0}, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

static char mfp_name(uint32_t i, char *b)                 /* "A03" */
{
    b[0] = 'A';
    b[1] = (char)('0' + (i % MFP_N + 1u) / 10u);
    b[2] = (char)('0' + (i % MFP_N + 1u) % 10u);
    b[3] = 0;
    return b[0];
}

/* ---------------------------------------------------------- pack / unpack --- */
static void mfp_pack_step(mfp_step_t *d, const step_t *s)
{
    memcpy(d->note, s->note, 4);
    d->nt = (uint8_t)((s->n > 4u ? 4u : s->n) | (uint32_t)(s->time > ST_REST ? ST_REST : s->time) << 3);
    d->flags = s->flags;
    d->vel = s->vel;
    d->prob = s->probability;
    d->len = s->hit ? 0u : s->acc;
}
static void mfp_unpack_step(step_t *s, const mfp_step_t *d)
{
    memcpy(s->note, d->note, 4);
    s->n = (uint8_t)(d->nt & 7u);
    s->time = (uint8_t)(d->nt >> 3 & 3u);
    if (s->n > 4u) s->n = 4;
    if (s->time > ST_REST) s->time = ST_REST;
    s->flags = d->flags;
    s->vel = (uint8_t)(d->vel & 127u);
    s->hit = 0;
    s->acc = (uint8_t)(d->len < MF_NLEN ? d->len : 0u);
    s->probability = d->prob;
}
static void mfp_pack_track(mfp_trk_t *d, const track_t *t)
{
    uint32_t j;
    d->eng = (uint8_t)(t->eng_req % NENGINES);
    d->preset = t->preset;
    for (j = 0; j < P_COUNT; j++)
        d->p[j] = (int8_t)clamp(motion_base_value(t, j), -128, 127);   /* (the sound's own, not a lock playing) */
    for (j = 0; j < MFP_STEPS; j++)
        mfp_pack_step(&d->step[j], &t->step[j]);
}
/* the tracks -> pattern i (ISR or IRQs off: one consistent picture) */
static void mfp_pack(uint32_t i)
{
    mfp_pat_t *p = mfp_at(i);
    uint32_t k, e, n = 0;
    for (k = 0; k < NTRK; k++) {
        mfp_pack_track(&p->t[k], &trk[k]);
        p->t[k].trn = mfp_trn[k];
        p->t[k].shift = mfp_shift[k];
    }
    p->motion.on = motion.on;
    for (e = 0; e < motion.count && e < MOTION_MAX; e++)
        if ((motion.event[e].place & 63u) < MFP_STEPS)       /* (steps past 32: none in MONOFM) */
            p->motion.event[n++] = motion.event[e];
    p->motion.count = (uint8_t)n;
}
/* pattern i -> the tracks (the audio ISR, or IRQs off). restart: every track from step 1 (carry samples in); else
 * where it was (DIRECT JUMP). A track's MUTE stays (the mutes are the project's) */
static void mfp_unpack_track(track_t *t, const mfp_trk_t *d, uint32_t k)
{
    uint32_t j;
    int16_t mute = t->p[P_MUTE];
    if (t->eng_req != d->eng % NENGINES)
        panic_req |= (uint8_t)(1u << k);                 /* (another engine: what sounds goes, engine_block switches) */
    t->eng_req = (uint8_t)(d->eng % NENGINES);
    for (j = 0; j < P_COUNT; j++)
        t->p[j] = d->p[j];                                /* (in range: mfp_fit when it came from flash) */
    t->p[P_MUTE] = mute;
    t->preset = d->preset;
    t->user = 0;
    for (j = 0; j < MFP_STEPS; j++)
        mfp_unpack_step(&t->step[j], &d->step[j]);
    for (; j < NSTEP; j++) {
        memset(&t->step[j], 0, sizeof t->step[j]);
        t->step[j].time = ST_REST;
    }
}
static void mfp_unpack(uint32_t i, uint32_t restart, uint32_t carry)
{
    const mfp_pat_t *p = mfp_at(i);
    uint32_t k;
    for (k = 0; k < NTRK; k++) {
        track_t *t = &trk[k];
        seq_release(t);
        mfp_unpack_track(t, &p->t[k], k);
        mfp_trn[k] = p->t[k].trn;
        mfp_shift[k] = p->t[k].shift;
        if (restart || t->seq_idx >= (uint32_t)t->p[P_SLEN]) {
            t->seq_idx = (uint16_t)(t->p[P_SLEN] - 1);
            t->seq_pos = 0x7FFFFFFFu;
        }
        t->rh_n = t->rskip_n = 0;
        t->rat_left = 0;
        t->nd_late = t->nd_early = ND_NONE;
        t->euc_pos = 0;
        if (restart) {
            mfp_loop[k] = 0;
            mfp_pre[k] = 0;
        }
    }
    motion = p->motion;
    memset(motion_active, 0, sizeof motion_active);
    memset(motion_locked, 0, sizeof motion_locked);
    motion_base_valid = 0;
    if (restart)
        mf_pat_carry = carry;
}

/* the master length in samples (AUTO: the longest track's loop) */
static uint32_t mfp_len_samples(void)
{
    uint32_t k, l = 0, m = mfp_at(mfp.cur)->mlen;
    if (m)
        return m * div_samples(2u);
    for (k = 0; k < NTRK; k++) {
        const track_t *t = &trk[k];
        uint32_t x = (uint32_t)clamp(t->p[P_SLEN], 1, MFP_STEPS) * div_samples(seq_div(t));
        l = x > l ? x : l;
    }
    return l ? l : div_samples(2u) * 16u;
}

/* ISR: pattern n now (pack the one playing first) */
static __attribute__((noinline)) void mfp_switch(uint32_t n, uint32_t restart, uint32_t carry)
{
    mfp_pack(mfp.cur);
    mfp.cur = (uint8_t)(n % MFP_N);
    mfp_unpack(mfp.cur, restart, carry);
    mfp.fired = 1;
    mfp.gen++;
}
static void mf_pat_start(void)                            /* (seq.c seq_start, the ISR's PLAY) */
{
    uint32_t k;
    mfp.pos = 0;
    for (k = 0; k < NTRK; k++) {
        mfp_loop[k] = 0;
        mfp_pre[k] = 0;
    }
}
/* the audio ISR, every block while playing, before the tracks' ticks */
static void mf_pat_tick(uint32_t n)
{
    uint32_t len, nx;
    if (!song.playing)
        return;
    if (mfp.next != MFP_NONE && mfp.mode != MFP_SEQ) {    /* DIRECT START / JUMP: at once */
        nx = mfp.next;
        mfp.next = MFP_NONE;
        mfp_switch(nx, mfp.mode == MFP_START, 0);
        if (mfp.mode == MFP_START)
            mfp.pos = 0;
    }
    mfp.pos += n;
    len = mfp_len_samples();
    if (mfp.pos < len)
        return;
    mfp.pos -= len;
    if (mfp.pos >= len)
        mfp.pos = 0;
    nx = mfp.next;
    if (nx == MFP_NONE && mfp.nchain >= 2u) {
        mfp.cpos = (uint8_t)((mfp.cpos + 1u) % mfp.nchain);
        nx = mfp.chain[mfp.cpos];
    }
    mfp.next = MFP_NONE;
    if (nx != MFP_NONE)
        mfp_switch(nx, 1, mfp.pos);
    else if (mfp_at(mfp.cur)->mlen) {                     /* a master length: every track from step 1 again */
        uint32_t k;
        for (k = 0; k < NTRK; k++) {
            track_t *t = &trk[k];
            t->seq_idx = (uint16_t)(t->p[P_SLEN] - 1);
            t->seq_pos = 0x7FFFFFFFu;
        }
        mf_pat_carry = mfp.pos;
    }
}

/* ------------------------------------------------------------------ flash --- */
#if FELUCCA_FLASH
#define MFP_FLASH_OK() flash_ok
static int mfp_read(uint32_t off, void *d, uint32_t n) { return st_read(off, d, n); }
static int mfp_erase(uint32_t off) { return st_erase(off); }
static int mfp_prog(uint32_t off, const void *s, uint32_t n) { return st_prog(off, s, n); }
#else                                                      /* host builds: a RAM image of the three copies (tests) */
#define MFP_FLASH_OK() 1
static uint8_t mfp_nor[3u * MFP_SPAN];
static uint32_t mfp_nor_cut = 0xFFFFFFFFu;                /* (tests: a power cut after this many programmed bytes) */
static uint8_t *mfp_nor_at(uint32_t off) { return &mfp_nor[(off - MFP_A) % sizeof mfp_nor]; }
static int mfp_read(uint32_t off, void *d, uint32_t n) { memcpy(d, mfp_nor_at(off), n); return 0; }
static int mfp_erase(uint32_t off) { memset(mfp_nor_at(off & ~0xFFFu), 0xFF, 0x1000u); return 0; }
static int mfp_prog(uint32_t off, const void *s, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (!mfp_nor_cut)
            return -9;
        mfp_nor_cut--;
        mfp_nor_at(off)[i] &= ((const uint8_t *)s)[i];
    }
    return 0;
}
#endif
static uint32_t mfp_crc_add(uint32_t c, const void *p, uint32_t n)   /* CRC-32 (zlib), running from 0xFFFFFFFF */
{
    const uint8_t *b = p;
    while (n--) {
        uint32_t j;
        c ^= *b++;
        for (j = 0; j < 8u; j++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c;
}
static uint32_t mfp_bank_crc(void)
{
    uint32_t i, c = 0xFFFFFFFFu;
    for (i = 0; i < MFP_N; i++)
        c = mfp_crc_add(c, mfp_at(i), sizeof(mfp_pat_t));
    return ~c;
}
/* copy `base`'s header, if it is a valid one with valid data (its CRC read back from flash): 1 */
static int mfp_head(uint32_t base, mfp_hdr_t *h)
{
    uint8_t buf[256];
    uint32_t off, c = 0xFFFFFFFFu, n;
    if (mfp_read(base, h, sizeof *h) || h->magic != MFP_MAGIC ||
        !((h->version == MFP_VERSION && h->size == sizeof(mfp_pat_t)) || (h->version == 1u && h->size == sizeof(mfp_pat1_t))) ||
        h->npat != MFP_N || ~mfp_crc_add(0xFFFFFFFFu, h, sizeof *h - 4u) != h->hcrc)
        return 0;
    n = MFP_N * h->size;
    for (off = 0; off < n; off += sizeof buf) {
        uint32_t k = n - off < sizeof buf ? n - off : (uint32_t)sizeof buf;
        if (mfp_read(base + 64u + off, buf, k))
            return 0;
        c = mfp_crc_add(c, buf, k);
    }
    return ~c == h->crc;
}
static uint32_t mfp_seq_max(void)                         /* the newest sequence number of the three copies */
{
    mfp_hdr_t h;
    uint32_t m = 0, i;
    static const uint32_t B[3] = {MFP_A, MFP_B, MFP_W};
    for (i = 0; i < 3u; i++)
        if (mfp_head(B[i], &h) && h.seq > m)
            m = h.seq;
    return m;
}
/* the bank -> the copy at base (its 8 sectors erased, the patterns, the header last): 0 ok */
static int mfp_write(uint32_t base, uint32_t seq)
{
    uint8_t buf[256];
    uint32_t off, i, n = MFP_N * (uint32_t)sizeof(mfp_pat_t);
    mfp_hdr_t h;
    for (i = 0; i < MFP_SPAN; i += 0x1000u)
        if (mfp_erase(base + i))
            return 1;
    for (off = 0; off < n; off += sizeof buf) {          /* (256-byte pages: never across a pattern's end in one copy) */
        uint32_t k = n - off < sizeof buf ? n - off : (uint32_t)sizeof buf, j;
        for (j = 0; j < k; j++) {
            uint32_t a = off + j;
            buf[j] = ((const uint8_t *)mfp_at(a / sizeof(mfp_pat_t)))[a % sizeof(mfp_pat_t)];
        }
        if (mfp_prog(base + 64u + off, buf, k))
            return 2;
    }
    memset(&h, 0, sizeof h);
    h.magic = MFP_MAGIC;
    h.version = MFP_VERSION;
    h.seq = seq;
    h.size = sizeof(mfp_pat_t);
    h.npat = MFP_N;
    h.mode = mfp.mode;
    h.crc = mfp_bank_crc();
    h.hcrc = ~mfp_crc_add(0xFFFFFFFFu, &h, sizeof h - 4u);
    if (mfp_prog(base, &h, sizeof h))
        return 3;
    {
        mfp_hdr_t r;
        return mfp_head(base, &r) && r.seq == seq ? 0 : 4;   /* (read back: a failing part must not say SAVED) */
    }
}
/* the copy at base -> the bank (each value back in its range); 0 none valid */
static void mfp_fit(void);
/* pattern i of the copy at base (its header h) -> d: version 1 brought over (its trigs' LEN the track's GATE) */
static int mfp_read_pat(uint32_t base, const mfp_hdr_t *h, uint32_t i, mfp_pat_t *d)
{
    uint32_t k, j;
    if (h->version == MFP_VERSION)
        return mfp_read(base + 64u + i * h->size, d, sizeof *d);
    {
        static mfp_pat1_t o;                              /* (main loop only) */
        if (mfp_read(base + 64u + i * h->size, &o, sizeof o))
            return 1;
        memset(d, 0, sizeof *d);
        d->mlen = o.mlen;
        d->motion = o.motion;
        for (k = 0; k < NTRK; k++) {
            d->t[k].eng = o.t[k].eng;
            d->t[k].preset = o.t[k].preset;
            memcpy(d->t[k].p, o.t[k].p, sizeof d->t[k].p);
            for (j = 0; j < MFP_STEPS; j++)
                memcpy(&d->t[k].step[j], &o.t[k].step[j], sizeof o.t[k].step[j]);
        }
    }
    return 0;
}
static int mfp_load(uint32_t base)
{
    mfp_hdr_t h;
    uint32_t i;
    if (!mfp_head(base, &h))
        return 0;
    for (i = 0; i < MFP_N; i++)
        if (mfp_read_pat(base, &h, i, mfp_at(i)))
            return 0;
    mfp.mode = (uint8_t)(h.mode <= MFP_JUMP ? h.mode : MFP_SEQ);
    mfp_fit();
    return 1;
}
static uint32_t mfp_best(int with_w)                      /* the newest valid copy's base, 0 none */
{
    mfp_hdr_t h;
    uint32_t i, best = 0, bs = 0;
    static const uint32_t B[3] = {MFP_A, MFP_B, MFP_W};
    for (i = 0; i < (with_w ? 3u : 2u); i++)
        if (mfp_head(B[i], &h) && (!best || h.seq > bs)) {
            best = B[i];
            bs = h.seq;
        }
    return best;
}
/* every stored value back in its range: an engine that exists, each parameter as that engine takes it, the steps' fields */
static void mfp_fit(void)
{
    uint32_t i, k, j;
    for (i = 0; i < MFP_N; i++) {
        mfp_pat_t *p = mfp_at(i);
        if (p->mlen > 128u)
            p->mlen = 0;
        if (p->motion.count > MOTION_MAX)
            p->motion.count = 0;
        for (k = 0; k < NTRK; k++) {
            mfp_trk_t *t = &p->t[k];
            if (t->eng >= NENGINES || !eng_ok(t->eng))
                t->eng = TRK_DEF[k][0];
            for (j = 0; j < P_COUNT; j++)
                t->p[j] = (int8_t)param_fit(param_desc_of(t->eng, j), t->p[j]);
            if (t->p[P_SLEN] > (int8_t)MFP_STEPS || t->p[P_SLEN] < 1)
                t->p[P_SLEN] = 16;
            for (j = 0; j < MFP_STEPS; j++) {
                mfp_step_t *s = &t->step[j];
                if ((s->nt & 7u) > 4u || (s->nt >> 3) > ST_REST)
                    s->nt = (uint8_t)(ST_REST << 3);
                for (uint32_t q = 0; q < 4u; q++)
                    s->note[q] &= 127u;
                s->vel &= 127u;
                if (s->prob > MF_COND0 + MF_NCOND - 1u)
                    s->prob = 0;
            }
        }
    }
}

/* ------------------------------------------------------------- main loop --- */
/* what the music is now (the tracks and the bank), hashed: what an autosave / SAVE would hold */
static uint32_t mfp_sig(void)
{
    uint32_t h = 2166136261u, k, j;
    for (k = 0; k < NTRK; k++) {
        const track_t *t = &trk[k];
        for (j = 0; j < P_COUNT; j++) {
            int16_t v = j == P_MUTE ? 0 : motion_base_value(t, j);
            h = as_mix(h, &v, sizeof v);
        }
        h = as_mix(h, &t->eng_req, 1);
        h = as_mix(h, t->step, MFP_STEPS * sizeof t->step[0]);
    }
    h = as_mix(h, &motion, sizeof motion);
    h = as_mix(h, &mfp.cur, 1);
    h = as_mix(h, &mfp.mode, 1);
    return h ^ mfp.gen * 2654435761u;
}
/* the tracks into their pattern's slot now (IRQs off: the ISR may switch meanwhile) */
static void mfp_sync(void)
{
    uint32_t f = motion_guard();
    mfp_pack(mfp.cur);
    motion_unguard(f);
}

/* SAVE: the project into the older of A / B (stopped; playing: at STOP). 0 saved */
static int mfp_save_now(void)
{
    uint32_t seq, base;
    mfp_hdr_t ha, hb;
    int va, vb;
    if (!MFP_FLASH_OK()) {
        ui_message("SAVE ERROR");
        return 1;
    }
    mfp_sync();
    seq = mfp_seq_max() + 1u;
    va = mfp_head(MFP_A, &ha);
    vb = mfp_head(MFP_B, &hb);
    base = !va ? MFP_A : !vb ? MFP_B : ha.seq <= hb.seq ? MFP_A : MFP_B;
    mfp.err = (uint8_t)(mfp_write(base, seq) != 0);
    if (mfp.err) {
        ui_message("SAVE ERROR");
        return 1;
    }
    mfp.saved = mfp.wsaved = mfp.seen = mfp_sig();
    ui_message("PROJECT SAVED");
    return 0;
}
static void mf_pat_save(void)
{
    if (transport_busy()) {
        mfp.save_wait = 1;
        ui_message("SAVES AT STOP");
        return;
    }
    mfp_save_now();
}
/* RELOAD: pattern `one` (or every one: MFP_NONE) as last SAVEd; the one playing comes into the tracks */
static void mf_pat_reload(uint32_t one)
{
    uint32_t base = MFP_FLASH_OK() ? mfp_best(0) : 0;
    if (!base) {
        ui_message("NOTHING SAVED");
        return;
    }
    if (one == MFP_NONE) {
        uint32_t f;
        if (transport_busy()) {                          /* (all of them: as a project load, stopped) */
            transport_req = 2;
            seq_stop();
        }
        if (!mfp_load(base)) {
            ui_message("LOAD ERROR");
            return;
        }
        f = motion_guard();
        mfp.next = MFP_NONE;
        mfp.nchain = 0;
        mfp_unpack(mfp.cur, 1, 0);
        motion_unguard(f);
        ui_message("PROJECT RELOADED");
    } else {
        mfp_hdr_t h;
        mfp_pat_t *p = &mfp_clip;                         /* (read into the clipboard's room first, then in) */
        uint8_t kind = mfp_clip_kind;
        if (!mfp_head(base, &h) || mfp_read_pat(base, &h, one % MFP_N, p)) {
            ui_message("LOAD ERROR");
            return;
        }
        {
            uint32_t f = motion_guard();
            *mfp_at(one) = *p;
            if (one % MFP_N == mfp.cur)
                mfp_unpack(mfp.cur, 0, 0);
            motion_unguard(f);
        }
        mfp_fit();
        mfp_clip_kind = 0;                               /* (the clipboard's room was used) */
        (void)kind;
        ui_message("PATTERN RELOADED");
    }
    mfp.gen++;
    undo.trk = 0;
    sync_reload = 1;
    ui.force = 1;
}

/* power-on, after the defaults (main.c, in place of the autosave's): the newest copy (W too), else a new project: every
 * pattern the power-on sounds with no trigs */
static void mf_pat_boot(void)
{
    uint32_t base = MFP_FLASH_OK() ? mfp_best(1) : 0, i, k, f;
    mfp.cur = 0;
    mfp.next = MFP_NONE;
    if (!base || !mfp_load(base)) {
        memset(&mfp_clip, 0, sizeof mfp_clip);
        memset(mfp_trn, 0, sizeof mfp_trn);
        memset(mfp_shift, 0, sizeof mfp_shift);
        mfp_pack(0);
        for (k = 0; k < NTRK; k++)
            for (i = 0; i < MFP_STEPS; i++) {
                memset(&mfp_at(0)->t[k].step[i], 0, sizeof(mfp_step_t));
                mfp_at(0)->t[k].step[i].nt = (uint8_t)(ST_REST << 3);
            }
        mfp_at(0)->motion.count = 0;
        mfp_at(0)->mlen = 0;
        memset(mfp_at(0)->rsv, 0, sizeof mfp_at(0)->rsv);
        for (i = 1; i < MFP_N; i++)
            *mfp_at(i) = *mfp_at(0);
    }
    f = motion_guard();
    mfp_unpack(0, 1, 0);
    motion_unguard(f);
    for (k = 0; k < NTRK; k++)
        pat_sig[k] = ~steps_sig(&trk[k]);
    mfp.saved = mfp.wsaved = mfp.seen = mfp_sig();
    if (base == MFP_W)
        mfp.saved ^= 1u;                                  /* (the working copy: not SAVEd as it is) */
    mfp.t_seen = fm1_ms;
}

/* main loop, every frame: the ISR's switch done (the header, the pages, the editor), a SAVE waiting for STOP, the
 * autosave (stopped, quiet, a while after the last change) */
#define MFP_AS_IDLE_MS 8000u
#define MFP_AS_GAP_MS 30000u
static void mf_pat_poll(void)
{
    uint32_t sig;
    if (mfp.fired) {
        uint32_t k;
        mfp.fired = 0;
        for (k = 0; k < NTRK; k++)
            pat_sig[k] = ~steps_sig(&trk[k]);
        undo.trk = 0;                                     /* (the undo copy was the pattern before's) */
        sync_reload = 1;
        ui.force = 1;
    }
    if (mfp.save_wait && !transport_busy()) {
        mfp.save_wait = 0;
        mfp_save_now();
    }
    if (fm1_ms - mfp.t_w < 500u)
        return;
    sig = mfp_sig();
    if (sig != mfp.seen || !autosave_quiet() || ui.menu || ui.confirm) {
        mfp.seen = sig;
        mfp.t_seen = fm1_ms;
        return;
    }
    if (sig == mfp.wsaved || (ui_prefs & PREF_RESTORE_OFF) || fm1_ms - mfp.t_seen < MFP_AS_IDLE_MS ||
        fm1_ms - mfp.t_w < MFP_AS_GAP_MS || !MFP_FLASH_OK())
        return;
    mfp.t_w = fm1_ms;
    mfp_sync();
    if (!mfp_write(MFP_W, mfp_seq_max() + 1u))
        mfp.wsaved = sig;
}
static int mf_pat_unsaved(void) { return mfp_sig() != mfp.saved; }

/* ------------------------------------------------------------- the moves --- */
/* pattern n asked for (SEQ let go after ALGORITHM; the PROJECT page): stopped at once, playing as the mode says */
static void mf_pat_ask(uint32_t n)
{
    char b[16] = "PATTERN ";
    n %= MFP_N;
    mfp.nchain = 0;
    if (!transport_busy()) {
        uint32_t f = motion_guard();
        if (n != mfp.cur) {
            mfp_pack(mfp.cur);
            mfp.cur = (uint8_t)n;
            mfp_unpack(n, 1, 0);
            mfp.gen++;
        }
        mfp.next = MFP_NONE;
        motion_unguard(f);
        mfp.fired = 1;
    } else {
        mfp.next = (uint8_t)n;
    }
    mfp_name(n, b + 8);
    ui_message(b);
}
/* a chain of the patterns n[0..c-1] (2 ..): the first comes as one asked for, then they take turns */
static void mf_pat_chain(const uint8_t *n, uint32_t c)
{
    uint32_t i;
    if (c < 2u) {
        if (c)
            mf_pat_ask(n[0]);
        return;
    }
    mf_pat_ask(n[0]);
    for (i = 0; i < c && i < MFP_N; i++)
        mfp.chain[i] = (uint8_t)(n[i] % MFP_N);
    mfp.cpos = 0;
    mfp.nchain = (uint8_t)c;
    ui_message("CHAIN");
}
static void mf_pat_mode_step(void)
{
    static const char *const M[3] = {"SEQUENTIAL", "DIRECT START", "DIRECT JUMP"};
    mfp.mode = (uint8_t)((mfp.mode + 1u) % 3u);
    mfp.gen++;
    ui_message(M[mfp.mode]);
}
/* a pattern holds trigs (its picture on SEQ's map: filled) */
static int mfp_has_trigs(uint32_t i)
{
    const mfp_pat_t *p = mfp_at(i);
    uint32_t k, j;
    if (i % MFP_N == mfp.cur) {
        for (k = 0; k < NTRK; k++)
            for (j = 0; j < MFP_STEPS; j++)
                if (trk[k].step[j].time == ST_NOTE && trk[k].step[j].n)
                    return 1;
        return 0;
    }
    for (k = 0; k < NTRK; k++)
        for (j = 0; j < MFP_STEPS; j++)
            if ((p->t[k].step[j].nt >> 3) == ST_NOTE && (p->t[k].step[j].nt & 7u))
                return 1;
    return 0;
}

/* "TRN +3": the track's total so far */
static void mf_tool_say(const char *what, int32_t v)
{
    char b[12];
    b[0] = v > 0 ? '+' : v < 0 ? '-' : ' ';
    fmt_int(b + (v != 0), v < 0 ? -v : v);
    if (!v)
        str_cpy(b, "0", sizeof b);
    ui_say(what, b);
}
static int32_t mf_pat_trn(uint32_t k) { return mfp_trn[k % NTRK]; }
static int32_t mf_pat_shift(uint32_t k) { return mfp_shift[k % NTRK]; }
/* SEQ + white keys: CLEAR the track, SHIFT < >, TRN - +, COPY / PASTE the track, COPY / PASTE the pattern */
static void mf_pat_action(uint32_t a)
{
    track_t *t = TSEL;
    uint32_t i, k = song.sel % NTRK, len = (uint32_t)clamp(t->p[P_SLEN], 1, MFP_STEPS);
    switch (a) {
    case MA_CLEAR:                                        /* (SEQ TOOLS' own: one undo, SAVE held) */
        tl_do(t, TL_CLEAR, 0);
        motion_clear(t);
        mfp_trn[k] = mfp_shift[k] = 0;
        ui_message("TRACK CLEARED");
        break;
    case MA_LEFT:
    case MA_RIGHT:                                        /* (the locks move with their steps) */
        tl_do(t, a == MA_LEFT ? TL_LEFT : TL_RIGHT, 0);
        mfp_shift[k] = (int8_t)(((int32_t)mfp_shift[k] + (a == MA_LEFT ? -1 : 1) + (int32_t)len * 64) % (int32_t)len);
        if (mfp_shift[k] > (int8_t)(len / 2u))
            mfp_shift[k] = (int8_t)(mfp_shift[k] - (int32_t)len);   /* (shown -LEN/2 .. +LEN/2: where step 1 went) */
        mf_tool_say("SHIFT ", mfp_shift[k]);
        break;
    case MA_TDN:
    case MA_TUP:
        step_undo_take(t, 0);
        for (i = 0; i < len; i++)
            for (uint32_t j = 0; j < t->step[i].n && j < 4u; j++)
                t->step[i].note[j] = (uint8_t)clamp(t->step[i].note[j] + (a == MA_TUP ? 1 : -1), 1, 127);
        motion_undo_done(t);
        mfp_trn[k] = (int8_t)clamp(mfp_trn[k] + (a == MA_TUP ? 1 : -1), -99, 99);
        mf_tool_say("TRN ", mfp_trn[k]);
        break;
    case MA_CTRK:
        mfp_pack_track(&mfp_clip.t[0], t);
        mfp_clip.motion.count = 0;
        for (i = 0; i < motion.count; i++)                /* (the track's locks and automation, its place 0) */
            if ((motion.event[i].place >> 6) == k && mfp_clip.motion.count < MOTION_MAX) {
                motion_event_t e = motion.event[i];
                e.place &= 63u;
                mfp_clip.motion.event[mfp_clip.motion.count++] = e;
            }
        mfp_clip_kind = 1;
        ui_message("TRACK COPIED");
        break;
    case MA_PTRK: {
        uint32_t f, n = 0;
        if (mfp_clip_kind != 1u) {
            ui_message("COPY A TRACK FIRST");
            break;
        }
        step_undo_take(t, 0);
        f = motion_guard();
        seq_release(t);
        {
            int16_t keep = t->p[P_MUTE];
            mfp_unpack_track(t, &mfp_clip.t[0], k);
            t->p[P_MUTE] = keep;
        }
        for (i = 0; i < motion.count; i++)                /* (its own records out, the clipboard's in) */
            if ((motion.event[i].place >> 6) != k)
                motion.event[n++] = motion.event[i];
        for (i = 0; i < mfp_clip.motion.count && n < MOTION_MAX; i++) {
            motion_event_t e = mfp_clip.motion.event[i];
            e.place = (uint8_t)(k << 6 | (e.place & 63u));
            motion.event[n++] = e;
        }
        motion.count = (uint8_t)n;
        memset(motion_active[k], 0, sizeof motion_active[k]);
        memset(motion_locked[k], 0, sizeof motion_locked[k]);
        motion_base_valid = 0;
        motion_unguard(f);
        motion_undo_done(t);
        sync_reload = 1;
        ui_message("TRACK PASTED");
        break;
    }
    case MA_CPAT:
        mfp_sync();
        mfp_clip = *mfp_at(mfp.cur);
        mfp_clip_kind = 2;
        ui_message("PATTERN COPIED");
        break;
    case MA_PPAT: {
        uint32_t f;
        if (mfp_clip_kind != 2u) {
            ui_message("COPY A PATTERN FIRST");
            break;
        }
        f = motion_guard();
        *mfp_at(mfp.cur) = mfp_clip;
        mfp_unpack(mfp.cur, 0, 0);
        motion_unguard(f);
        undo.trk = 0;
        sync_reload = 1;
        ui_message("PATTERN PASTED");
        break;
    }
    default:
        return;
    }
    mfp.gen++;
    ui.force = 1;
}

/* --------------------------------------------------------- the pages' view --- */
static uint32_t mf_pat_cur(void) { return mfp.cur; }
static uint32_t mf_pat_next(void) { return mfp.next; }
static uint32_t mf_pat_mode(void) { return mfp.mode; }
static uint32_t mf_pat_nchain(void) { return mfp.nchain >= 2u ? mfp.nchain : 0u; }
static uint32_t mf_pat_chain_at(uint32_t i) { return mfp.chain[i % MFP_N]; }
static void mf_pat_label(uint32_t i, char *b) { mfp_name(i, b); }
static uint32_t mf_pat_mlen(void) { return mfp_at(mfp.cur)->mlen; }
static void mf_pat_mlen_turn(int32_t s)
{
    mfp_pat_t *p = mfp_at(mfp.cur);
    p->mlen = (uint8_t)clamp((int32_t)p->mlen + s, 0, 128);
    mfp.gen++;
}
static void mf_chain_stop(void)
{
    mfp.nchain = 0;
    ui_message("CHAIN OFF");
}
