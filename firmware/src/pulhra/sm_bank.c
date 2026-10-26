/* SPDX-License-Identifier: GPL-3.0-only */
/* PULHRA banks and storage: 5 banks A..E of 8 patterns (40, A01..E08), the kit bound to its pattern. The main loop's
 * alone (it is the model's only writer); the audio stream only reads the RAM halves and smp.hbank (sm_seq.c).
 *
 * RAM: two banks (smp.pat, two halves of 8 slots, smp.hbank[h] the bank of half h). The half of the pattern playing is never
 * written over; a bank wanted (a pattern asked for, a chain's next link, a GLO save into it) is loaded into the OTHER half
 * from the flash (a read, no erase: fine while playing), then the switch is asked for (smp.next, the RAM slot) and comes on
 * its boundary as any switch, so the pattern playing plays on, every step, while the bank comes in. Why two halves and not
 * one bank and a copy of the pattern playing: the flash is written only while stopped (an erase silences the audio), so the
 * edits of a bank left while playing must stay in RAM until STOP; two halves keep them (the other half is written to the
 * flash first, stopped, before another bank takes it). Playing, a half with unsaved edits is not given up: a third bank waits
 * for STOP (the request, or the chain's switch, waits; nothing is lost). A bank loaded is "clean" (sm_hhash: its hash then);
 * one that differs from it is "dirty" and is written before it leaves RAM (stopped), at SAVE and by the autosave (8 s with no
 * edit and no note sounding, stopped). With no flash (the part not recognised at boot) a bank left is forgotten.
 *
 * Flash (0xB4000..0xE0000: MONOFM's project area (mf_pat.c MFP_A 0xB4000) and Felucca / STRANGE's user sample slots USR2 / USR3.
 * PULHRA plays USR1 only, eng_sample.c SMP_USER_LIVE, but its first SAVE or autosave erases a MONOFM project and user samples
 * saved there, and installing MONOFM later wipes PULHRA's banks: the two do not share a part. The bank format is
 * version 3 (64 steps), unmigrated: older banks are not loaded; the project object is
 * versioned (stage 4: version 2 adds the global effects, a version 1 object loads with their defaults)): the project
 * object (BPM, swing, mutes, change mode, chain, the current pattern) in two copies, one sector each (A 0xB4000, B 0xB5000);
 * the banks in a pool of 5 places of 8 sectors (0xB6000 + 0x8000 * i, to 0xDE000: over Felucca's user presets, which PULHRA
 * has not, and below the update loader's staging at 0xE0000 (ota.c)): a bank is 8 x sm_pattern_t of 64 steps = 30592 B (version 3: banks A..D
 * of 64 steps; a bank of an older version, another size, is not loaded: Tim, the banks start empty). Each place: a 32-byte header at +0 {magic "SWB1", version, bank, used, seq,
 * size, crc32 of the payload, hcrc} and the payload at +64. A save of bank b goes to a place that holds no bank's newest
 * copy (its own older copy first: "the older one", else an empty place, else the oldest other copy); with 5 places and 4
 * banks there is always one such place, so a bank's newest copy is never erased. The payload is written first, the header
 * last and read back: a write cut at any point leaves a place with no valid header (or a bad CRC), and the bank's newest
 * copy before it is the one loaded at power-on (per bank, the valid copy with the highest seq). The project likewise (A/B,
 * the older written). Every write only while stopped. */
#define SM_FL_LO 0xB4000u                                 /* PULHRA's flash */
#define SM_FL_HI 0xE0000u                                 /* (the OTA staging from here, ota.c OTA_AREA) */
#define SM_FL_SEC 0x1000u
#define SM_FL_PROJ 0xB4000u                               /* the project: copy A, copy B a sector above */
#define SM_FL_SLOT0 0xB6000u                              /* the bank places */
#define SM_FL_SPAN 0x8000u                                /* 8 sectors a place */
#define SM_FL_NSLOT 5u
#define SM_FL_DATA 64u                                    /* the payload's offset in a place (the header at +0) */
#define SM_BANK_BYTES (SM_BPAT * (uint32_t)sizeof(sm_pattern_t))
#define SM_BMAGIC 0x31425753u                             /* "SWB1" */
#define SM_PMAGIC 0x31505753u                             /* "SWP1" */
#define SM_STORE_VER 1u                                   /* version 1 of the project object (and of the banks) */
#define SM_BANK_VER 6u                                    /* 6: the lanes' envelopes (sm_pattern_t.lenv) after the filters; 5, 4 and 3
                                                             load widened (sm_bank_fill) */
#define SM_PAT5_BYTES 3852u
#define SM_BANK_VER5 5u                                    /* 5: the lanes' filters (sm_pattern_t.lflt) after the mix */
#define SM_PAT4_BYTES 3840u
#define SM_BANK_VER4 4u                                    /* 4: the KIT's lanes' mix (sm_pattern_t.lmix) at each pattern's end: a
                                                             version 3 bank (3824-byte patterns) loads widened, the defaults there
                                                             (sm_bank_v3) */
#define SM_PAT3_BYTES 3824u
#define SM_BANK_VER3 3u                                    /* the banks': 3 = 64 steps (a lock's place trk << 6 | step), the speed per
                                                             track, 183 locks and the KIT's lanes. Versions 1 and 2 (32 steps, other
                                                             places) are not loaded */
_Static_assert(sizeof(sm_pattern_t) == SM_PAT5_BYTES + 16u && __builtin_offsetof(sm_pattern_t, lmix) == SM_PAT3_BYTES &&
               __builtin_offsetof(sm_pattern_t, lflt) == SM_PAT4_BYTES && __builtin_offsetof(sm_pattern_t, lenv) == SM_PAT5_BYTES &&
               SM_PAT4_BYTES + 12u == SM_PAT5_BYTES,
               "a pattern's size is the banks' layout (6: 5's and the envelopes; 5: 4's and the lanes' filters; 4: 3's and the mix)");
static uint32_t sm_pat_bytes(uint32_t ver)
{
    return ver == SM_BANK_VER3 ? SM_PAT3_BYTES : ver == SM_BANK_VER4 ? SM_PAT4_BYTES : ver == SM_BANK_VER5 ? SM_PAT5_BYTES :
           (uint32_t)sizeof(sm_pattern_t);
}
#define SM_PROJ_VER 6u                                    /* 6: the compressor's makeup (a version 5 object: the first 7 of g4) */
#define SM_PREC5_BYTES (SM_PREC3_BYTES + 2u * 7u)
#define SM_PROJ_VER5 5u
                                   /* 5: the reverb's PRE and TONE, the compressor (sm_prec_t.g4) */
#define SM_PROJ_VER_OLD 4u                                    /* the project object's: version 2 adds the global effects, 3 the MIDI routing,
                                                             4 the master dynamics and the reverb's pre-delay */
#define SM_AUTOSAVE_US 8000000u                           /* 8 s of quiet, stopped: the autosave */
#define SM_STORE_CHECK_US 200000u                         /* how often the autosave looks (a hash of both halves) */
#define SM_SAVE_HOLD_US 800000u                           /* SEQ + F#5 held this long: SAVE */
#define SM_GLO_HOLD_US 800000u                            /* GLO + a pattern key held this long: save the current into it */

static void sm_up_boot(void);                             /* the user sounds (sm_upre.c): power-on, and the SAVE at STOP */
static void sm_up_poll(void);
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t bank, used;                                   /* (the project: 0xFF, 0) */
    uint32_t seq, size, crc, rsv[2];
    uint32_t hcrc;
} sm_fhdr_t;
_Static_assert(sizeof(sm_fhdr_t) == 32u, "PULHRA flash header");
_Static_assert(SM_FL_DATA >= sizeof(sm_fhdr_t), "the header before the payload");
_Static_assert(SM_FL_DATA + SM_BANK_BYTES <= SM_FL_SPAN, "a bank fits its place (8 sectors)");
_Static_assert(SM_FL_PROJ == SM_FL_LO && SM_FL_PROJ + 2u * SM_FL_SEC <= SM_FL_SLOT0, "the project's two sectors, then the banks");
_Static_assert(SM_FL_SLOT0 + SM_FL_NSLOT * SM_FL_SPAN <= SM_FL_HI, "the bank places stay below SM_FL_HI");
_Static_assert(SM_FL_NSLOT >= SM_NBANK + 1u, "a save always has a place holding no bank's newest copy");
_Static_assert(SMP_USER_BASE + SMP_USER_LIVE * SMP_USER_SIZE <= SM_FL_LO, "the live user sample slots stay below PULHRA's flash");
_Static_assert(SM_FL_LO >= 0xB4000u && SM_FL_HI <= 0xE0000u && (SM_FL_SLOT0 & 0xFFFu) == 0u && (SM_FL_SPAN & 0xFFFu) == 0u,
               "PULHRA's flash: 0xB4000..0xE0000 (the OTA staging), whole sectors");

/* the project object: what is not a pattern's. Version 1 (stage 3) ended at chain (SM_PREC1_BYTES); version 2 adds the global
 * effects, song.g[SM_GFX[i]] (the delay, the reverb, the chorus: the deep FX pages). The bank format is untouched (the banks
 * keep SM_STORE_VER). A version 1 object loads with the new fields at their defaults (GP[id].def); the first save after that
 * writes version 2 (the other copy keeps the old one until then: the newest valid copy of either version wins) */
#define SM_NGFX 9u
static const uint8_t SM_GFX[SM_NGFX] = {G_DTIME, G_DFDBK, G_DCOLOR, G_DMIX, G_RTYPE, G_RSIZE, G_RDAMP, G_CRATE, G_CDEPTH};
#define SM_NGFX4 8u                                       /* version 5: the reverb's PRE TONE, the compressor (version 4 had five of
                                                             another meaning: not loaded) */
static const uint8_t SM_GFX4[SM_NGFX4] = {G_RPRE, G_RTONE, G_CTHR, G_CRAT, G_CATK, G_CREL, G_CSC, G_CMKUP};   /* (6: + MKUP) */
typedef struct {
    uint16_t bpm10;
    uint8_t swing, mute, mode, nchain, cur, rsv;          /* cur: the pattern id (0..39) */
    uint8_t chain[SM_NCHAIN];                             /* pattern ids */
    int16_t g[SM_NGFX];                                   /* version 2: the global effects (SM_GFX order) */
    uint8_t mch[SM_NTRK];                                 /* version 3: the MIDI page's channel of each track (0 OFF, 1..16) */
    uint8_t msrc, mtrn;                                   /* version 3: the SETUP page's MIDI source (0 USB, 1 INT, 2 JACK); its
                                                             CLK (bits 0..1: AUTO INT USB JACK) and TRNS OFF (bit 2): 0 AUTO, ON */
    int16_t g4[SM_NGFX4];                                 /* version 5: SM_GFX4 */
} sm_prec_t;
#define SM_PREC1_BYTES 24u                                /* version 1's size: everything up to g */
#define SM_PREC2_BYTES (SM_PREC1_BYTES + 2u * SM_NGFX)    /* version 2's: up to mch */
#define SM_PREC3_BYTES (SM_PREC2_BYTES + SM_NTRK + 2u)   /* version 3's: up to g4 */
#define SM_PREC4_BYTES (SM_PREC3_BYTES + 2u * 5u)        /* version 4's: five g4 of another meaning */
_Static_assert(sizeof(sm_prec_t) == SM_PREC3_BYTES + 2u * SM_NGFX4, "the project object: version 1, g, the MIDI routing, g4, no padding");
_Static_assert(sizeof(sm_prec_t) + SM_FL_DATA <= SM_FL_SEC, "the project fits its sector");

/* the flash places, as found at power-on and kept since (the main loop's) */
static uint8_t sm_fs_bank[SM_FL_NSLOT];                   /* the bank in place i (SM_BANK_NONE: none valid) */
static uint8_t sm_fs_used[SM_FL_NSLOT];
static uint32_t sm_fs_seq[SM_FL_NSLOT], sm_fs_crc[SM_FL_NSLOT];
static uint8_t sm_fs_ver[SM_FL_NSLOT];                    /* each place's bank version (1: converted at the load) */
static uint8_t sm_fs_top[SM_NBANK];                       /* the place of each bank's newest valid copy (SM_BANK_NONE: none) */
static uint8_t sm_fp_ok[2];                               /* the project's copies valid, their seq */
static uint32_t sm_fp_seq[2];
static uint32_t sm_fs_next;                               /* the next seq (above every one in the flash) */

static uint32_t sm_hhash[SM_NHALF];                       /* each half's hash when it was loaded or saved (clean) */
static uint32_t sm_phash;                                 /* the project's when saved or loaded */
static uint8_t sm_want = SM_PAT_NONE;                     /* a pattern id asked for whose bank is not in RAM yet */
static uint8_t sm_chain_pend[SM_NCHAIN], sm_chain_npend;  /* a chain whose link 1's bank is not in RAM yet */
static uint8_t sm_gop_kind, sm_gop_g;                     /* a GLO save into a bank not in RAM yet (1: waiting) */
static uint8_t sm_flash_hold;                             /* a write failed: no new try until an edit, a SAVE or a request */
static uint8_t sm_save_pend;                              /* SAVE asked while playing: done at STOP */
static uint32_t sm_as_sig, sm_as_t0, sm_as_look;          /* the autosave: the model's hash, since when, the last look */
static uint8_t sm_glo_bank;                               /* GLO: the bank in view */
static uint8_t sm_glo_key = 0xFFu, sm_glo_done;           /* GLO: the pattern key down (its place 0..7), its hold acted */
static uint32_t sm_glo_t0;
static uint32_t sm_fsave_t0;                              /* SEQ: F#5 down since (0: up), its hold acted (sm_fsave_done) */
static uint8_t sm_fsave_dn, sm_fsave_done;
#ifndef SM_STORE_PREEMPT
#define SM_STORE_PREEMPT() ((void)0)                      /* (the host test runs audio blocks in the middle of a load) */
#endif

static uint32_t sm_fnv(uint32_t h, const void *p, uint32_t n)
{
    const uint8_t *b = p;
    while (n--)
        h = (h ^ *b++) * 16777619u;
    return h;
}
static uint32_t sm_half_hash(uint32_t h)
{
    uint32_t b = smp.hbank[h % SM_NHALF], x = sm_fnv(2166136261u, &smp.pat[(h % SM_NHALF) * SM_BPAT], SM_BANK_BYTES);
    return sm_fnv(x, b < SM_NBANK ? &sm_used[b] : (const uint8_t *)"", 1u);
}
static int sm_half_dirty(uint32_t h) { return smp.hbank[h] < SM_NBANK && sm_half_hash(h) != sm_hhash[h]; }
static void sm_prec_get(sm_prec_t *r)
{
    uint32_t g = sm_gid_of(smp.cur), i;
    memset(r, 0, sizeof *r);
    r->bpm10 = smp.bpm10;
    r->swing = smp.swing;
    r->mute = smp.mute;
    r->mode = smp.mode;
    r->nchain = smp.nchain;
    r->cur = (uint8_t)(g < SM_NGPAT ? g : 0u);
    memcpy(r->chain, smp.chain, sizeof r->chain);
    for (i = 0; i < SM_NGFX; i++)
        r->g[i] = song.g[SM_GFX[i]];
    for (i = 0; i < SM_NGFX4; i++)
        r->g4[i] = song.g[SM_GFX4[i]];
    for (i = 0; i < SM_NTRK; i++)
        r->mch[i] = sm_midi_ch[i];
    r->msrc = sm_midi_src;
    r->mtrn = (uint8_t)((sm_midi_clk & 3u) | (sm_midi_trnoff ? 4u : 0u) | (sm_midi_div ? 8u : 0u));   /* (bit 3: DIV 1:2) */
}
static uint32_t sm_proj_hash(void)
{
    sm_prec_t r;
    sm_prec_get(&r);
    return sm_fnv(2166136261u, &r, sizeof r);
}
/* the pattern in RAM slot s holds a trig */
static int sm_pat_has_trig(uint32_t s)
{
    const sm_pattern_t *pt = &smp.pat[s % SM_NPAT];
    uint32_t t, i;
    for (t = 0; t < SM_NTRK; t++)
        for (i = 0; i < SM_NSTEP; i++)
            if (pt->step[t][i].n)
                return 1;
    return 0;
}
/* pattern id g holds something (the GLO grid, the SEQ grid): its used bit, or, in RAM, a trig */
static int sm_pat_used(uint32_t g)
{
    uint32_t s;
    if (g >= SM_NGPAT)
        return 0;
    if ((sm_used[g / SM_BPAT] >> (g % SM_BPAT)) & 1u)
        return 1;
    s = sm_slot_of(g);
    return s < SM_NPAT && sm_pat_has_trig(s);
}

/* ---------------------------------------------------------------- flash --- */
static uint32_t sm_slot_addr(uint32_t i) { return SM_FL_SLOT0 + (i % SM_FL_NSLOT) * SM_FL_SPAN; }
static int sm_seq_newer(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }
static int sm_hdr_ok(const sm_fhdr_t *h, uint32_t magic)
{
    return h->magic == magic && h->hcrc == st_crc32(h, sizeof *h - 4u);
}
/* the project object's version and size agree: 1 with SM_PREC1_BYTES, 2 with the whole record */
static int sm_phdr_ok(const sm_fhdr_t *h)
{
    return sm_hdr_ok(h, SM_PMAGIC) && ((h->version == SM_STORE_VER && h->size == SM_PREC1_BYTES) ||
                                       (h->version == 2u && h->size == SM_PREC2_BYTES) ||
                                       (h->version == 3u && h->size == SM_PREC3_BYTES) ||
                                       (h->version == SM_PROJ_VER_OLD && h->size == SM_PREC4_BYTES) ||
                                       (h->version == SM_PROJ_VER5 && h->size == SM_PREC5_BYTES) ||
                                       (h->version == SM_PROJ_VER && h->size == sizeof(sm_prec_t)));
}
/* the CRC of n bytes of the flash at off (read in 256-byte pieces) */
static uint32_t sm_flash_crc(uint32_t off, uint32_t n)
{
    uint8_t buf[256];
    uint32_t c = 0xFFFFFFFFu;
    while (n) {
        uint32_t k = n < sizeof buf ? n : (uint32_t)sizeof buf;
        if (st_read(off, buf, k))
            return 0;
        c = st_crc32_add(c, buf, k);
        off += k;
        n -= k;
        fm1_wdt_feed();
    }
    return ~c;
}
/* n bytes of RAM at src into the erased flash at off, through a 256-byte buffer (one page at most per program) */
static int sm_flash_put(uint32_t off, const void *src, uint32_t n)
{
    uint8_t buf[256];
    const uint8_t *s = src;
    while (n) {
        uint32_t k = 256u - (off & 0xFFu);
        k = k < n ? k : n;
        memcpy(buf, s, k);
        if (st_prog(off, buf, k))
            return 1;
        off += k;
        s += k;
        n -= k;
        fm1_wdt_feed();
    }
    return 0;
}
/* a whole object at base: erase `secs` sectors, the payload at +SM_FL_DATA, the header last, read back. 0: written */
static int sm_flash_write(uint32_t base, uint32_t secs, sm_fhdr_t *h, const void *data, uint32_t n)
{
    sm_fhdr_t r;
    uint32_t i;
    for (i = 0; i < secs; i++) {
        if (st_erase(base + i * SM_FL_SEC))
            return 1;
        fm1_wdt_feed();
    }
    if (sm_flash_put(base + SM_FL_DATA, data, n))
        return 2;
    h->size = n;
    h->crc = st_crc32(data, n);
    h->hcrc = st_crc32(h, sizeof *h - 4u);
    if (sm_flash_put(base, h, sizeof *h))
        return 3;
    if (st_read(base, &r, sizeof r) || memcmp(&r, h, sizeof r) || sm_flash_crc(base + SM_FL_DATA, n) != h->crc)
        return 4;                                         /* (a failing part must not say SAVED) */
    return 0;
}

/* the place for a save of bank b: never a bank's newest copy; b's own older copy first, else an empty place, else the oldest */
static uint32_t sm_slot_pick(uint32_t b)
{
    uint32_t i, k, best = SM_FL_NSLOT, score = 9u;
    for (i = 0; i < SM_FL_NSLOT; i++) {
        uint32_t sc;
        for (k = 0; k < SM_NBANK && sm_fs_top[k] != i; k++) {}
        if (k < SM_NBANK)
            continue;
        sc = sm_fs_bank[i] == b ? 0u : sm_fs_bank[i] >= SM_NBANK ? 1u : 2u;
        if (sc < score || (sc == score && sc == 2u && sm_seq_newer(sm_fs_seq[best], sm_fs_seq[i]))) {
            best = i;
            score = sc;
        }
    }
    return best;
}

/* RAM half h (its bank) into the flash: the used bits take every pattern with a trig first. 0: written (clean now) */
static int sm_bank_save(uint32_t h)
{
    uint32_t b = smp.hbank[h % SM_NHALF], i, sl, s0 = (h % SM_NHALF) * SM_BPAT;
    sm_fhdr_t hd;
    if (b >= SM_NBANK)
        return 0;
    if (!flash_ok)
        return 1;
    for (i = 0; i < SM_BPAT; i++)
        if (sm_pat_has_trig(s0 + i))
            sm_used[b] |= (uint8_t)(1u << i);
    sl = sm_slot_pick(b);
    if (sl >= SM_FL_NSLOT)
        return 1;
    memset(&hd, 0, sizeof hd);
    hd.magic = SM_BMAGIC;
    hd.version = SM_BANK_VER;
    hd.bank = (uint8_t)b;
    hd.used = sm_used[b];
    hd.seq = sm_fs_next;
    sm_fs_bank[sl] = SM_BANK_NONE;                        /* (erased now, whatever comes of it) */
    if (sm_flash_write(sm_slot_addr(sl), SM_FL_SPAN / SM_FL_SEC, &hd, &smp.pat[s0], SM_BANK_BYTES))
        return 1;
    sm_fs_bank[sl] = (uint8_t)b;
    sm_fs_used[sl] = hd.used;
    sm_fs_seq[sl] = hd.seq;
    sm_fs_crc[sl] = hd.crc;
    sm_fs_ver[sl] = SM_BANK_VER;
    sm_fs_top[b] = (uint8_t)sl;
    sm_fs_next++;
    sm_hhash[h % SM_NHALF] = sm_half_hash(h);
    return 0;
}
/* the project object into its older copy. 0: written */
static int sm_proj_save(void)
{
    sm_prec_t r;
    sm_fhdr_t hd;
    uint32_t c;
    if (!flash_ok)
        return 1;
    sm_prec_get(&r);
    c = !sm_fp_ok[0] ? 0u : !sm_fp_ok[1] ? 1u : sm_seq_newer(sm_fp_seq[0], sm_fp_seq[1]) ? 1u : 0u;
    memset(&hd, 0, sizeof hd);
    hd.magic = SM_PMAGIC;
    hd.version = SM_PROJ_VER;
    hd.bank = 0xFFu;
    hd.seq = sm_fs_next;
    sm_fp_ok[c] = 0;
    if (sm_flash_write(SM_FL_PROJ + c * SM_FL_SEC, 1u, &hd, &r, sizeof r))
        return 1;
    sm_fp_ok[c] = 1;
    sm_fp_seq[c] = hd.seq;
    sm_fs_next++;
    sm_phash = sm_fnv(2166136261u, &r, sizeof r);
    return 0;
}
/* everything not in the flash yet: the dirty halves, the project. 0: all written */
static int sm_store_all(void)
{
    uint32_t h;
    int bad = 0;
    for (h = 0; h < SM_NHALF; h++)
        if (sm_half_dirty(h))
            bad |= sm_bank_save(h);
    if (sm_proj_hash() != sm_phash)
        bad |= sm_proj_save();
    sm_flash_hold = bad != 0;                             /* (a failing part: no try on every poll, see sm_bank_poll) */
    return bad;
}

/* every value of the 8 kits of RAM slots s0.. inside its descriptor's range on the sound's machine (a bank from the flash is
 * data: a CHAOS value out of range crashed once), the machine itself inside the list */
static void sm_bank_clamp(uint32_t s0)
{
    uint32_t i, t, p;
    for (i = 0; i < SM_BPAT; i++)
        for (t = 0; t < SM_NTRK; t++) {
            sm_sound_t *sd = &smp.pat[s0 + i].kit.snd[t];
            sd->machine %= SM_NMACHINE;
            sm_ens_semi(sd);                              /* (ENS: a just ratio of an older bank its semitone) */
            if (t == SM_KIT_TRK && sm_snd_kit(sd)) {      /* (the KIT's lanes: each inside its machine's ranges) */
                uint32_t L, k;
                for (L = 0; L < 4u; L++)
                    for (k = 0; k < 9u; k++) {
                        const param_desc_t *d = dr_lane_desc(dr_kmach(sd->p[P_E0 + L]), k);
                        smp.pat[s0 + i].lane[L][k] = (int8_t)clamp(smp.pat[s0 + i].lane[L][k], d->min, d->max);
                    }
                for (L = 0; L < 4u; L++)
                    for (k = 0; k < 4u; k++)
                        smp.pat[s0 + i].lmix[L][k] = (int8_t)clamp(smp.pat[s0 + i].lmix[L][k], SM_LMD[k].min, SM_LMD[k].max);
                for (L = 0; L < 4u; L++)
                    for (k = 0; k < 3u; k++)
                        smp.pat[s0 + i].lflt[L][k] = (int8_t)clamp(smp.pat[s0 + i].lflt[L][k], SM_LFD[k].min, SM_LFD[k].max);
                for (L = 0; L < 4u; L++)
                    for (k = 0; k < 4u; k++)
                        smp.pat[s0 + i].lenv[L][k] = (int8_t)clamp(smp.pat[s0 + i].lenv[L][k], SM_LED[k].min, SM_LED[k].max);
            }
            for (p = 0; p < P_COUNT; p++) {
                const param_desc_t *d = sm_desc_safe(sm_engine_of(sd->machine), p);
                if (d)
                    sd->p[p] = (int8_t)clamp(sd->p[p], d->min, d->max);
            }
        }
}

/* RAM half h <- bank b (the newest valid copy in the flash, else 8 empty patterns with the default kit). The half is free:
 * no pattern of it plays or is asked for (the caller's check); hbank SM_BANK_NONE while it is written, so the audio stream
 * finds no link in it meanwhile */
static void sm_bank_fill(uint32_t h, uint32_t b)
{
    uint32_t sl = b < SM_NBANK ? sm_fs_top[b] : SM_BANK_NONE, s0 = (h % SM_NHALF) * SM_BPAT, i, ok = 0;
    smp.hbank[h % SM_NHALF] = SM_BANK_NONE;
    SM_BARRIER();
    SM_STORE_PREEMPT();
    if (flash_ok && sl < SM_FL_NSLOT) {
        uint32_t pb = sm_pat_bytes(sm_fs_ver[sl]), nb = SM_BPAT * pb;
        ok = !st_read(sm_slot_addr(sl) + SM_FL_DATA, &smp.pat[s0], nb) && st_crc32(&smp.pat[s0], nb) == sm_fs_crc[sl];
        if (ok && pb < sizeof(sm_pattern_t))
            for (i = SM_BPAT; i-- > 0;) {                 /* (an older version: widened in place, the last pattern first) */
                volatile uint8_t *d = (uint8_t *)&smp.pat[s0 + i];   /* (volatile: the loop stays a loop, not a memmove call) */
                const uint8_t *src = (uint8_t *)&smp.pat[s0] + i * pb;
                uint32_t j = pb;
                while (j--)                               /* (backwards: d >= src, overlapping; no memmove in libc.c) */
                    d[j] = src[j];
                if (pb < SM_PAT4_BYTES)
                    sm_lmix_default(&smp.pat[s0 + i]);
                if (pb < SM_PAT5_BYTES)
                    sm_lflt_default(&smp.pat[s0 + i]);
                sm_lenv_default(&smp.pat[s0 + i]);
            }
        if (!ok)
            sm_msg_show(SM_MSG_FLASH);
        else {
            sm_bank_clamp(s0);
        }
    }
    if (!ok) {
        sm_kit_t kit;
        sm_kit_default(&kit);
        for (i = 0; i < SM_BPAT; i++)
            sm_pat_blank(&smp.pat[s0 + i], &kit);
    }
    SM_STORE_PREEMPT();
    SM_BARRIER();
    smp.hbank[h % SM_NHALF] = (uint8_t)b;
    sm_hhash[h % SM_NHALF] = sm_half_hash(h);
}

/* power-on, after sm_core_init, before the audio starts: the flash scanned (every place's header and CRC, the project's two
 * copies), the newest valid copy of each bank and of the project; the current pattern's bank into half 0, its kit onto the
 * tracks. No valid project: the power-on defaults (A01) */
static void sm_store_boot(void)
{
    sm_fhdr_t hd;
    sm_prec_t r;
    uint32_t i, b, top = 0, have = 0, g = 0, pc = 2u;
    memset(sm_fs_bank, SM_BANK_NONE, sizeof sm_fs_bank);
    memset(sm_fs_top, SM_BANK_NONE, sizeof sm_fs_top);
    memset(sm_fp_ok, 0, sizeof sm_fp_ok);
    sm_up_boot();                                         /* (the user sounds' index: sm_upre.c) */
    for (i = 0; flash_ok && i < SM_FL_NSLOT; i++) {
        if (st_read(sm_slot_addr(i), &hd, sizeof hd) || !sm_hdr_ok(&hd, SM_BMAGIC) || (hd.version != SM_BANK_VER && hd.version != SM_BANK_VER5 && hd.version != SM_BANK_VER4 &&
             hd.version != SM_BANK_VER3) || hd.bank >= SM_NBANK ||
            hd.size != SM_BPAT * sm_pat_bytes(hd.version) || sm_flash_crc(sm_slot_addr(i) + SM_FL_DATA, hd.size) != hd.crc)
            continue;
        sm_fs_bank[i] = hd.bank;
        sm_fs_used[i] = hd.used;
        sm_fs_seq[i] = hd.seq;
        sm_fs_crc[i] = hd.crc;
        sm_fs_ver[i] = (uint8_t)hd.version;
        if (sm_fs_top[hd.bank] >= SM_FL_NSLOT || sm_seq_newer(hd.seq, sm_fs_seq[sm_fs_top[hd.bank]]))
            sm_fs_top[hd.bank] = (uint8_t)i;
        if (!have || sm_seq_newer(hd.seq, top))
            top = hd.seq;
        have = 1;
    }
    for (i = 0; flash_ok && i < 2u; i++) {
        if (st_read(SM_FL_PROJ + i * SM_FL_SEC, &hd, sizeof hd) || !sm_phdr_ok(&hd) ||
            sm_flash_crc(SM_FL_PROJ + i * SM_FL_SEC + SM_FL_DATA, hd.size) != hd.crc)
            continue;
        sm_fp_ok[i] = 1;
        sm_fp_seq[i] = hd.seq;
        if (pc > 1u || sm_seq_newer(hd.seq, sm_fp_seq[pc]))
            pc = i;
        if (!have || sm_seq_newer(hd.seq, top))
            top = hd.seq;
        have = 1;
    }
    sm_fs_next = have ? top + 1u : 1u;
    for (b = 0; b < SM_NBANK; b++)
        sm_used[b] = sm_fs_top[b] < SM_FL_NSLOT ? sm_fs_used[sm_fs_top[b]] : 0u;
    memset(&r, 0, sizeof r);
    for (i = 0; i < SM_NGFX; i++)                         /* (a version 1 object has no g: the defaults) */
        r.g[i] = GP[SM_GFX[i]].def;
    for (i = 0; i < SM_NGFX4; i++)                        /* (one before version 4 has no g4: the defaults, off) */
        r.g4[i] = GP[SM_GFX4[i]].def;
    for (i = 0; i < SM_NTRK; i++)                         /* (a version 1 or 2 object has no MIDI routing: channel 1..4, INT) */
        r.mch[i] = (uint8_t)(i + 1u);
    r.msrc = 1u;
    if (pc < 2u && !st_read(SM_FL_PROJ + pc * SM_FL_SEC, &hd, sizeof hd) &&
        !st_read(SM_FL_PROJ + pc * SM_FL_SEC + SM_FL_DATA, &r, hd.size)) {
        if (hd.version < SM_PROJ_VER5)                    /* (version 4's g4 meant another thing: the defaults; 5's: its 7, read) */
            for (i = 0; i < SM_NGFX4; i++)
                r.g4[i] = GP[SM_GFX4[i]].def;
        for (i = 0; i < SM_NGFX; i++) {                   /* (the global effects into the core's song.g, in their ranges) */
            const param_desc_t *d = &GP[SM_GFX[i]];
            song.g[SM_GFX[i]] = (int16_t)clamp(r.g[i], d->min, d->max);
        }
        for (i = 0; i < SM_NGFX4; i++) {
            const param_desc_t *d = &GP[SM_GFX4[i]];
            song.g[SM_GFX4[i]] = (int16_t)clamp(r.g4[i], d->min, d->max);
        }
        for (i = 0; i < SM_NTRK; i++)
            sm_midi_ch[i] = (uint8_t)(r.mch[i] <= 16u ? r.mch[i] : i + 1u);
        sm_midi_src = (uint8_t)(r.msrc < 3u ? r.msrc : 1u);
        sm_midi_clk = (uint8_t)(r.mtrn & 3u);
        sm_midi_trnoff = (uint8_t)((r.mtrn >> 2) & 1u);
        sm_midi_div = (uint8_t)((r.mtrn >> 3) & 1u);
        smp.bpm10 = (uint16_t)clamp(r.bpm10, 300, 3000);
        smp.swing = (uint8_t)clamp(r.swing, 50, 75);
        smp.mute = (uint8_t)(r.mute & ((1u << SM_NTRK) - 1u));
        smp.mode = (uint8_t)(r.mode % SM_PM_COUNT);
        for (i = 0; i < SM_NCHAIN; i++)
            smp.chain[i] = (uint8_t)(r.chain[i] % SM_NGPAT);
        smp.nchain = (uint8_t)(r.nchain >= 2u && r.nchain <= SM_NCHAIN ? r.nchain : 0u);
        g = r.cur % SM_NGPAT;
    } else if (sm_fs_top[0] >= SM_FL_NSLOT) {
        sm_used[0] |= 1u;                                 /* (nothing saved: A01, the current, is used) */
    }
    sm_bank_fill(0, g / SM_BPAT);
    smp.hbank[1] = SM_BANK_NONE;
    sm_hhash[1] = 0;
    smp.cur = (uint8_t)(g % SM_BPAT);
    smp.next = SM_PAT_NONE;
    sm_phash = sm_proj_hash();
    sm_kit_boot(&smp.pat[smp.cur].kit);
    sm_lanes_core(&smp.pat[smp.cur]);
    sm_seq_init();
    sm_as_sig = 0;
    sm_as_t0 = sm_as_look = fm1_ticks();
}

/* ---------------------------------------------------------- the halves --- */
/* power-on (sm_core_init): banks A, B in RAM as they are (clean), nothing asked, nothing pending */
static void sm_bank_init(void)
{
    uint32_t h;
    for (h = 0; h < SM_NHALF; h++)
        sm_hhash[h] = sm_half_hash(h);
    sm_phash = sm_proj_hash();
    sm_want = SM_PAT_NONE;
    sm_chain_npend = 0;
    sm_gop_kind = 0;
    sm_save_pend = 0;
    sm_flash_hold = 0;
    sm_glo_bank = 0;
    sm_glo_key = 0xFFu;
    sm_fsave_dn = sm_fsave_done = 0;
    sm_as_sig = 0;
    sm_as_t0 = sm_as_look = fm1_ticks();
}

/* the half free to take another bank: not the half playing, none of its slots asked for; 2: none (a pattern of each plays
 * or is asked for) */
static uint32_t sm_half_free(void)
{
    uint32_t h = (*(volatile uint8_t *)&sm_play_pat % SM_NPAT) / SM_BPAT ^ 1u, nx = smp.next;
    if (nx < SM_NPAT && nx / SM_BPAT == h)
        return 2u;
    return h;
}
enum { SM_BR_OK, SM_BR_WAIT, SM_BR_FAIL };
/* bank b into RAM if it is not there (the free half: its bank written to the flash first when dirty, stopped; playing, a
 * dirty half waits for STOP) */
static uint32_t sm_wait_t0;                               /* the last bring that had to wait (its next try ~20 ms on: the hash */
static uint8_t sm_waiting;                                /* of a half is 20 KB, the poll runs without a pause) */
static uint32_t sm_bank_bring(uint32_t b)
{
    uint32_t h, k, old, now = fm1_ticks();
    for (k = 0; k < SM_NHALF; k++)
        if (smp.hbank[k] == b)
            return SM_BR_OK;
    if (sm_waiting && (uint32_t)(now - sm_wait_t0) < 20000u * FM1_TICKS_PER_US && *(volatile uint8_t *)&sm_playing)
        return SM_BR_WAIT;
    sm_waiting = 1;
    sm_wait_t0 = now;
    h = sm_half_free();
    if (h >= SM_NHALF)
        return SM_BR_WAIT;
    if (sm_half_dirty(h) && flash_ok) {                   /* (no flash: RAM only, a bank left is forgotten) */
        if (*(volatile uint8_t *)&sm_playing)
            return SM_BR_WAIT;                            /* (its edits stay in RAM till STOP: never lost) */
        if (sm_flash_hold)
            return SM_BR_FAIL;                            /* (it failed a moment ago: no erase again till an edit or a request) */
        if (sm_bank_save(h)) {
            sm_flash_hold = 1;
            sm_msg_show(SM_MSG_FLASH);
            return SM_BR_FAIL;
        }
    }
    old = smp.hbank[h];
    smp.hbank[h] = SM_BANK_NONE;                          /* (no link resolves into it from here) */
    SM_BARRIER();
    if ((*(volatile uint8_t *)&sm_play_pat % SM_NPAT) / SM_BPAT == h) {   /* (a chain's link switched into it just before: */
        smp.hbank[h] = (uint8_t)old;                      /* it plays, it stays) */
        return SM_BR_WAIT;
    }
    sm_bank_fill(h, b);
    sm_waiting = 0;
    return SM_BR_OK;
}

/* pattern id g asked for (SEQ let go, a GLO tap on a used place): the chain off, the request replaces any before; it is
 * served by sm_bank_poll (the bank brought in, then smp.next) */
static void sm_ask(uint32_t g)
{
    if (g >= SM_NGPAT)
        return;
    sm_flash_hold = 0;                                    /* (a request: one more try) */
    smp.nchain = 0;
    sm_chain_npend = 0;
    SM_BARRIER();
    smp.next = SM_PAT_NONE;
    sm_want = (uint8_t)g;
    if (sm_slot_of(g) < SM_NPAT) {                        /* (in RAM: asked for at once) */
        SM_BARRIER();
        smp.next = (uint8_t)sm_slot_of(g);
        sm_want = SM_PAT_NONE;
    }
    sm_dirty = 1;
}
static void sm_chain_set(const uint8_t *c, uint32_t n);   /* sm_layer.c */
/* a chain c[0 .. n-1] of pattern ids: published once its link 1's bank is in RAM (sm_bank_poll) */
static void sm_chain_ask(const uint8_t *c, uint32_t n)
{
    uint32_t i;
    n = n < SM_NCHAIN ? n : SM_NCHAIN;
    sm_flash_hold = 0;
    sm_want = SM_PAT_NONE;
    for (i = 0; i < n; i++)
        sm_chain_pend[i] = (uint8_t)(c[i] % SM_NGPAT);
    sm_chain_npend = (uint8_t)n;
    if (n && sm_slot_of(sm_chain_pend[0]) < SM_NPAT) {    /* (link 1 in RAM: published at once) */
        sm_chain_set(sm_chain_pend, n);
        sm_chain_npend = 0;
    }
    sm_dirty = 1;
}

/* the current pattern copied into pattern id g (kit and all; GLO): its bank in RAM (else it waits), the slot quiet while the
 * copy is written (no link resolves into its half, not asked for). 0 done, 1 waiting, 2 the flash failed (SM_MSG_FLASH shown) */
static int sm_pat_store(uint32_t g)
{
    uint32_t s, h, b = g / SM_BPAT, r;
    if (g >= SM_NGPAT)
        return 0;
    r = sm_bank_bring(b);
    if (r == SM_BR_FAIL)
        return 2;
    s = sm_slot_of(g);
    if (r == SM_BR_WAIT || s >= SM_NPAT)
        return 1;
    if (s == smp.cur % SM_NPAT)
        return 0;                                         /* (into itself: nothing) */
    h = s / SM_BPAT;
    smp.hbank[h] = SM_BANK_NONE;
    SM_BARRIER();
    if (smp.next == s)
        smp.next = SM_PAT_NONE;
    SM_BARRIER();
    if (*(volatile uint8_t *)&sm_play_pat == s) {         /* (switched into it a moment ago: it is the current one now) */
        smp.hbank[h] = (uint8_t)b;
        return 0;
    }
    smp.pat[s] = smp.pat[smp.cur % SM_NPAT];
    sm_used[b] |= (uint8_t)(1u << (g % SM_BPAT));
    SM_BARRIER();
    smp.hbank[h] = (uint8_t)b;
    sm_dirty = 1;
    return 0;
}

/* SAVE (SEQ + F#5 held): everything into the flash now, or at STOP while playing */
static void sm_save_ask(void)
{
    sm_save_pend = 1;
    sm_flash_hold = 0;
    if (*(volatile uint8_t *)&sm_playing)
        sm_msg_show(SM_MSG_SAVEPEND);                     /* (playing: written at STOP, SAVED then; stopped: SAVED at the next poll) */
}

/* every poll, after sm_pat_poll: a request or a chain whose bank came in is asked for; the next link's bank brought in on the
 * link before (playing; stopped: link 1's); a GLO save that waited; SAVE at STOP; the autosave (8 s of quiet, stopped) */
static void sm_bank_poll(uint32_t now)
{
    uint32_t playing = *(volatile uint8_t *)&sm_playing, s, r;
    if (sm_want < SM_NGPAT) {
        r = sm_bank_bring(sm_want / SM_BPAT);
        s = sm_slot_of(sm_want);
        if (r == SM_BR_FAIL) {
            sm_want = SM_PAT_NONE;
        } else if (r == SM_BR_OK && s < SM_NPAT) {
            SM_BARRIER();
            smp.next = (uint8_t)s;                        /* (a RAM slot: the audio stream switches on the boundary) */
            sm_want = SM_PAT_NONE;
            sm_dirty = 1;
        }
    }
    if (sm_chain_npend) {
        r = sm_bank_bring(sm_chain_pend[0] / SM_BPAT);
        if (r == SM_BR_FAIL) {
            sm_chain_npend = 0;
        } else if (r == SM_BR_OK && sm_slot_of(sm_chain_pend[0]) < SM_NPAT) {
            sm_chain_set(sm_chain_pend, sm_chain_npend);
            sm_chain_npend = 0;
            sm_dirty = 1;
        }
    }
    if (smp.nchain >= 2u && smp.nchain <= SM_NCHAIN && sm_want >= SM_NGPAT && !sm_chain_npend) {
        uint32_t nc = smp.nchain, c = playing ? (sm_cpos + 1u) % nc : 0u;
        if (sm_slot_of(smp.chain[c]) >= SM_NPAT)
            (void)sm_bank_bring(smp.chain[c] / SM_BPAT);   /* (playing with the other half dirty: the switch waits) */
    }
    if (sm_gop_kind) {                                   /* the GLO save that waited for a free half: done, SAVED */
        uint32_t g = sm_pat_store(sm_gop_g);
        if (g != 1u) {
            sm_gop_kind = 0;
            if (!g)
                sm_msg_show(SM_MSG_SAVED);
        }
    }
    if (playing) {
        sm_as_t0 = now;                                   /* (the quiet counts while stopped) */
        return;
    }
    sm_up_poll();                                         /* (a user sound's SAVE that waited for STOP) */
    if (sm_save_pend) {
        sm_save_pend = 0;
        sm_msg_show(sm_store_all() ? SM_MSG_FLASH : SM_MSG_SAVED);
        sm_as_t0 = now;
        return;
    }
    if ((uint32_t)(now - sm_as_look) >= SM_STORE_CHECK_US * FM1_TICKS_PER_US) {
        uint32_t sig = sm_fnv(sm_half_hash(0) ^ sm_half_hash(1) * 31u ^ sm_proj_hash() * 7u, &smp.hbank, sizeof smp.hbank);
        int dirty = sm_half_dirty(0) || sm_half_dirty(1) || sm_proj_hash() != sm_phash;
        sm_as_look = now;
        if (sig != sm_as_sig || voices_busy()) {          /* (quiet: no edit, and no note sounding: an erase silences the */
            if (sig != sm_as_sig)                         /* audio, so not over a note played on the keys or from MIDI) */
                sm_flash_hold = 0;                        /* (an edit: a failed write is tried again) */
            sm_as_sig = sig;
            sm_as_t0 = now;
        } else if (dirty && flash_ok && !sm_flash_hold && (uint32_t)(now - sm_as_t0) >= SM_AUTOSAVE_US * FM1_TICKS_PER_US) {
            if (sm_store_all())
                sm_msg_show(SM_MSG_FLASH);
            sm_as_t0 = now;
        }
    }
}

/* ------------------------------------------------------- the GLO grid --- */
/* GLO opened: the grid shows the current pattern's bank */
static void sm_glo_open(void)
{
    uint32_t g = sm_gid_of(smp.cur);
    sm_glo_bank = (uint8_t)(g < SM_NGPAT ? g / SM_BPAT : 0u);
    sm_glo_key = 0xFFu;
    sm_glo_done = 0;
}
/* a pattern key (place 0..7 of the bank in view) let go before the hold: a used place is asked for (the current one: nothing),
 * an empty one takes a copy of the current pattern */
static void sm_glo_tap(uint32_t place)
{
    uint32_t g = sm_glo_bank * SM_BPAT + place % SM_BPAT, r;
    if (g == sm_gid_of(smp.cur))
        return;
    sm_flash_hold = 0;                                    /* (a request: one more try) */
    if (sm_pat_used(g)) {
        sm_ask(g);
        return;
    }
    r = sm_pat_store(g);
    if (r == 1u) {                                        /* (its bank waits for a free half) */
        sm_gop_kind = 1;
        sm_gop_g = (uint8_t)g;
    } else if (!r) {
        sm_msg_show(SM_MSG_SAVED);                        /* (2: the flash failed, its FLASH shown) */
    }
}
/* GLO, every poll: a pattern key down (white C4..C5, place 0..7), its hold (>= 0.8 s: the current pattern saved into it again,
 * no switch) or its release (a tap); a bank key (black G#4 A#4 C#5 D#5 F#5) puts that bank in view */
static void sm_glo_poll(uint32_t notes, uint32_t now)
{
    static const uint8_t BANK_KEY[SM_NBANK] = {15, 17, 20, 22, 25};
    static const uint8_t PAT_KEY[SM_BPAT] = {7, 9, 11, 12, 14, 16, 18, 19};
    uint32_t i;
    for (i = 0; i < SM_NBANK; i++)
        if ((notes >> BANK_KEY[i]) & 1u) {
            sm_glo_bank = (uint8_t)i;
            sm_dirty = 1;
        }
    for (i = 0; i < SM_BPAT; i++)
        if ((notes >> PAT_KEY[i]) & 1u) {
            sm_glo_key = (uint8_t)i;
            sm_glo_t0 = now;
            sm_glo_done = 0;
        }
    if (sm_glo_key >= SM_BPAT)
        return;
    if (!((fm1_in.notes >> PAT_KEY[sm_glo_key]) & 1u)) {
        if (!sm_glo_done)
            sm_glo_tap(sm_glo_key);
        sm_glo_key = 0xFFu;
        sm_dirty = 1;
    } else if (!sm_glo_done && (uint32_t)(now - sm_glo_t0) >= SM_GLO_HOLD_US * FM1_TICKS_PER_US) {
        uint32_t g = sm_glo_bank * SM_BPAT + sm_glo_key, r = 0;
        sm_glo_done = 1;
        sm_flash_hold = 0;
        if (g != sm_gid_of(smp.cur))
            r = sm_pat_store(g);
        if (r == 1u) {                                      /* (its bank waits for a free half) */
            sm_gop_kind = 1;
            sm_gop_g = (uint8_t)g;
        } else if (!r) {
            sm_msg_show(SM_MSG_SAVED);                      /* (2: the flash failed, its FLASH shown) */
        }
        sm_dirty = 1;
    }
}

/* SEQ, every poll: F#5 held 0.8 s: SAVE (once a hold) */
/* SEQ held + D#5 / F#5 held 0.8 s (Tim): the selected track copied / pasted (a tap: "HOLD: .." in the header, nothing done).
 * sm_fsave_dn: the key down (22 or 25), 0 none. (SEQ + F#5 is no SAVE any more: SHIFT + SAVE is) */
#define SM_TCOPY_KEY 22u                                  /* D#5 */
#define SM_TPASTE_KEY 25u                                 /* F#5 */
static void sm_copy_trk(uint32_t t);
static void sm_paste_trk(uint32_t t);
static void sm_fsave_poll(uint32_t notes, uint32_t now)
{
    uint32_t k;
    for (k = SM_TCOPY_KEY; k <= SM_TPASTE_KEY; k += SM_TPASTE_KEY - SM_TCOPY_KEY)
        if ((notes >> k) & 1u) {
            sm_fsave_dn = (uint8_t)k;
            sm_fsave_done = 0;
            sm_fsave_t0 = now;
            sm_msg_show(k == SM_TCOPY_KEY ? SM_MSG_TCOPYP : SM_MSG_TPASTEP);
        }
    if (!sm_fsave_dn)
        return;
    if (!((fm1_in.notes >> sm_fsave_dn) & 1u)) {
        if (!sm_fsave_done && (sm_msg == SM_MSG_TCOPYP || sm_msg == SM_MSG_TPASTEP))
            sm_msg = SM_MSG_NONE;                         /* (let go early: nothing done) */
        sm_fsave_dn = 0;
        sm_dirty = 1;
    } else if (!sm_fsave_done && (int32_t)(now - sm_fsave_t0) >= (int32_t)(SM_SAVE_HOLD_US * FM1_TICKS_PER_US)) {
        sm_fsave_done = 1;
        if (sm_fsave_dn == SM_TCOPY_KEY) {
            sm_copy_trk(sm_ui.trk % SM_NTRK);
            sm_msg_show(SM_MSG_TCOPY);
        } else {
            sm_paste_trk(sm_ui.trk % SM_NTRK);
            sm_msg_show(SM_MSG_TPASTE);
        }
    }
}
