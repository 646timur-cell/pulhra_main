/* SPDX-License-Identifier: GPL-3.0-only */
/* PULHRA's PRESET page and its user sounds (Tim, 26.10). SAVE tapped opens the page (tapped again: back to the page before; held:
 * UNDO as ever). K1 the machine, K2 the category (ALL, BASS .. OTHER: category.c), K3 a sound of that machine and category (its
 * factory sounds F01.., then the user's U01..), K4 a user place to save into (U01 .. U24). Turning K3 asks LOAD, turning K4 asks
 * SAVE: OCT+ does it (the SELECT row says which, inverted). LOAD puts the sound onto the selected track (the sequencer's length,
 * speed and trig defaults stay, as the PRESETS knob's); SAVE writes the selected track's sound into the place, named after the
 * machine and the category, four letters each, and the place's number ("SAW LEAD07"); the category is K2's, or with ALL the
 * sound's own (its factory sound's).
 *
 * The flash: two sectors at 0xDE000 / 0xDF000 (above the bank places, below the update loader's staging, sm_bank.c), a log of
 * 144-byte records (magic, place, category, a sequence number, the name, the sound, a CRC). A save appends a record to the
 * sector in use; the newest record of each place wins (power-on: both sectors scanned). A full sector: the other one erased, the
 * newest record of every other place copied over, then the new one (24 places at most, 28 records a sector: it always fits).
 * A record is written whole, then its CRC is what makes it count: a write cut leaves the place's record before it. Every write
 * only while stopped (an erase stops the audio): a SAVE while playing is kept and written at STOP (SAVE AT STOP). RAM: the
 * index of the places (the pool) and one record waiting, nothing else; the names are read from the flash when shown. */

#include "category.c"

#define SM_UP_N 24u                                       /* the user places U01 .. U24 */
#define SM_UP_SEC0 0xDE000u
#define SM_UP_MAGIC 0x5055u                               /* "UP" */
#define SM_UP_NAME 10u
typedef struct {
    uint16_t magic;
    uint8_t place, cat;
    uint32_t seq;
    char name[SM_UP_NAME];
    sm_sound_t snd;
    uint16_t crc;                                         /* the low 16 bits of the CRC-32 of everything before it */
} sm_urec_t;
_Static_assert(sizeof(sm_urec_t) == 144u, "a user sound's record: 144 bytes, 28 a sector");
#define SM_UP_PER (SM_FL_SEC / (uint32_t)sizeof(sm_urec_t))
_Static_assert(SM_UP_PER >= SM_UP_N + 2u, "a sector holds every place's record, the new one and a torn one");
_Static_assert(SM_FL_SLOT0 + SM_FL_NSLOT * SM_FL_SPAN <= SM_UP_SEC0 && SM_UP_SEC0 + 2u * SM_FL_SEC <= SM_FL_HI,
               "the user sounds' two sectors between the bank places and the loader's staging");

static struct {
    uint16_t at[SM_UP_N];                                 /* each place's newest record: (sector << 15 | offset) + 1, 0 none (the
                                                             pool's zeros: none before the boot) */
    uint8_t mach[SM_UP_N], cat[SM_UP_N];                  /* (what K1 and K2 filter by) */
    uint32_t seq;                                         /* the newest record's */
    uint8_t act;                                          /* the sector written to */
    uint16_t next;                                        /* its first free record's offset (SM_FL_SEC: full) */
    uint8_t pend;                                         /* a SAVE waiting for STOP (its record in rec) */
    sm_urec_t rec;
} sm_up __attribute__((section(".pool")));

static uint32_t sm_up_addr(uint32_t at) { return SM_UP_SEC0 + (at >> 15) * SM_FL_SEC + (at & 0x7FFFu); }   /* (at: no + 1) */
static uint32_t sm_up_rat(uint32_t k) { return (uint32_t)sm_up.at[k] - 1u; }   /* place k's record (used) */
static uint16_t sm_up_crc(const sm_urec_t *r) { return (uint16_t)st_crc32(r, __builtin_offsetof(sm_urec_t, crc)); }
static int sm_up_rec_ok(const sm_urec_t *r) { return r->magic == SM_UP_MAGIC && r->place < SM_UP_N && r->crc == sm_up_crc(r); }
static int sm_up_used(uint32_t k) { return k < SM_UP_N && sm_up.at[k] != 0u; }

/* power-on (after the banks, sm_store_boot): both sectors scanned, each place's newest record, the sector in use */
static void sm_up_boot(void)
{
    sm_urec_t r;
    uint32_t s, o, top = 0, have = 0;
    memset(sm_up.at, 0, sizeof sm_up.at);
    sm_up.act = 0;
    sm_up.next = 0;
    sm_up.pend = 0;
    sm_up.seq = 0;
    for (s = 0; flash_ok && s < 2u; s++)
        for (o = 0; o + sizeof r <= SM_FL_SEC; o += sizeof r) {
            uint32_t at = s << 15 | o;
            if (st_read(sm_up_addr(at), &r, sizeof r) || !sm_up_rec_ok(&r))
                continue;
            if (sm_up_used(r.place)) {
                sm_urec_t q;
                if (!st_read(sm_up_addr(sm_up_rat(r.place)), &q, sizeof q) && q.seq > r.seq)
                    continue;
            }
            sm_up.at[r.place] = (uint16_t)(at + 1u);
            sm_up.mach[r.place] = r.snd.machine;
            sm_up.cat[r.place] = r.cat;
            if (!have || r.seq > top) {
                top = r.seq;
                sm_up.act = (uint8_t)s;
            }
            have = 1;
        }
    sm_up.seq = top;
    for (o = 0; o + sizeof r <= SM_FL_SEC; o += sizeof r) {   /* the first record of the sector in use never written (all 1s) */
        uint32_t k, blank = 1;
        if (st_read(sm_up_addr((uint32_t)sm_up.act << 15 | o), &r, sizeof r))
            break;
        for (k = 0; k < sizeof r && blank; k++)
            blank = ((const uint8_t *)&r)[k] == 0xFFu;
        if (blank)
            break;
    }
    sm_up.next = (uint16_t)(o + sizeof r <= SM_FL_SEC ? o : SM_FL_SEC);
}

/* place k's name into b (11 bytes), "" for an empty place */
static void sm_up_name(uint32_t k, char *b)
{
    sm_urec_t r;
    uint32_t i;
    b[0] = 0;
    if (!sm_up_used(k) || st_read(sm_up_addr(sm_up_rat(k)) + __builtin_offsetof(sm_urec_t, name), r.name, SM_UP_NAME))
        return;
    for (i = 0; i < SM_UP_NAME && r.name[i]; i++)
        b[i] = r.name[i];
    b[i] = 0;
}

/* the record rec into the sector in use (the other one first when it is full). 0: written */
static int sm_up_write(sm_urec_t *rec)
{
    sm_urec_t r;
    uint32_t at, k;
    if (!flash_ok)
        return 1;
    rec->magic = SM_UP_MAGIC;
    rec->seq = sm_up.seq + 1u;
    rec->crc = sm_up_crc(rec);
    if ((uint32_t)sm_up.next + sizeof r > SM_FL_SEC) {   /* full: the other sector, every other place's newest record there */
        uint32_t ns = sm_up.act ^ 1u, o = 0;
        uint16_t nat[SM_UP_N];
        if (st_erase(SM_UP_SEC0 + ns * SM_FL_SEC))
            return 2;
        fm1_wdt_feed();
        memcpy(nat, sm_up.at, sizeof nat);
        for (k = 0; k < SM_UP_N; k++) {
            if (k == rec->place || !sm_up_used(k))
                continue;
            if (st_read(sm_up_addr(sm_up_rat(k)), &r, sizeof r) || !sm_up_rec_ok(&r))
                return 3;
            if (sm_flash_put(sm_up_addr(ns << 15 | o), &r, sizeof r))
                return 4;
            nat[k] = (uint16_t)((ns << 15 | o) + 1u);
            o += sizeof r;
        }
        memcpy(sm_up.at, nat, sizeof nat);
        sm_up.act = (uint8_t)ns;
        sm_up.next = (uint16_t)o;
    }
    at = (uint32_t)sm_up.act << 15 | sm_up.next;
    sm_up.next = (uint16_t)(sm_up.next + sizeof r);       /* (a failed write leaves its place used: the next one after it) */
    if (sm_flash_put(sm_up_addr(at), rec, sizeof *rec) || st_read(sm_up_addr(at), &r, sizeof r) || memcmp(&r, rec, sizeof r))
        return 5;
    sm_up.at[rec->place] = (uint16_t)(at + 1u);
    sm_up.mach[rec->place] = rec->snd.machine;
    sm_up.cat[rec->place] = rec->cat;
    sm_up.seq = rec->seq;
    return 0;
}

/* every poll while stopped (sm_bank_poll): the SAVE that waited */
static void sm_up_poll(void)
{
    if (!sm_up.pend)
        return;
    sm_up.pend = 0;
    sm_msg_show(sm_up_write(&sm_up.rec) ? SM_MSG_FLASH : SM_MSG_SAVED);
    sm_dirty = 1;
}

/* ------------------------------------------------------------ the page --- */
#define SM_PR_ALL 0u                                      /* K2's first: every category */
static struct {
    uint8_t mach, cat, item, place, arm, prev;            /* K1 .. K4; arm: 0 none, 1 LOAD, 2 SAVE asked; the page before */
} sm_pr __attribute__((section(".pool")));               /* (the pool: RAM keeps its 4 KiB headroom) */

/* K1's machines: every place but the drum machines of the KIT's lanes (the DRUM page has the KIT alone) */
static int sm_pr_mach_ok(uint32_t m) { return m < MF_PAGE_4 || m == SM_KIT_MACH; }
/* a factory sound's category, and a sound's (its factory sound's) */
static uint32_t sm_pr_fcat(uint32_t m, uint32_t k) { return preset_cat(sm_engine_of(m), k); }
static uint32_t sm_pr_cat_ok(uint32_t c) { return sm_pr.cat == SM_PR_ALL || c == sm_pr.cat; }
/* K3's list: machine K1's factory sounds of category K2, then the user places of that machine and category. Item i: 1 user
 * (*k the place), 0 factory (*k the preset); -1 past the end. The count */
static int32_t sm_pr_item(uint32_t i, uint32_t *k)
{
    const engine_t *e = ENGINES[sm_engine_of(sm_pr.mach)];
    uint32_t j;
    for (j = 0; j < e->npresets; j++)
        if (sm_pr_cat_ok(sm_pr_fcat(sm_pr.mach, j)) && !i--) {
            *k = j;
            return 0;
        }
    for (j = 0; j < SM_UP_N; j++)
        if (sm_up_used(j) && sm_up.mach[j] == sm_pr.mach && sm_pr_cat_ok(sm_up.cat[j]) && !i--) {
            *k = j;
            return 1;
        }
    return -1;
}
static uint32_t sm_pr_count(void)
{
    uint32_t n = 0, k;
    while (sm_pr_item(n, &k) >= 0)
        n++;
    return n;
}

/* SAVE tapped: the page opens on the selected track's sound (its machine, ALL, its factory sound, the first empty place); tapped on
 * it: back to the page before */
static void sm_pr_open(void)
{
    const sm_sound_t *s = &smp.pat[smp.cur % SM_NPAT].kit.snd[sm_ui.trk % SM_NTRK];
    uint32_t n = sm_page_count(), i, k;
    if (sm_page_home() == SMH_PRESET) {
        sm_ui.page = (uint8_t)(sm_pr.prev % n);
        sm_dirty = 1;
        return;
    }
    sm_pr.prev = sm_ui.page;
    for (i = 0; i < n && SM_PAGES[i].home != SMH_PRESET; i++) {}
    sm_ui.page = (uint8_t)(i % n);
    sm_pr.mach = (uint8_t)(sm_pr_mach_ok(s->machine) ? s->machine : 0u);
    sm_pr.cat = SM_PR_ALL;
    sm_pr.item = 0;
    for (i = 0; sm_pr_item(i, &k) == 0; i++)
        if (k == s->preset) {
            sm_pr.item = (uint8_t)i;
            break;
        }
    for (i = 0; i < SM_UP_N && sm_up_used(i); i++) {}
    sm_pr.place = (uint8_t)(i < SM_UP_N ? i : 0u);
    sm_pr.arm = 0;
    sm_dirty = 1;
}

static uint32_t sm_pr_wrap(int32_t v, uint32_t n) { return n ? (uint32_t)(((v % (int32_t)n) + (int32_t)n) % (int32_t)n) : 0u; }
/* K1 .. K4 on the page */
static void sm_pr_page(const int32_t *k)
{
    if (k[0]) {                                           /* K1: the machine (round, the KIT's lane machines left out) */
        int32_t m = sm_pr.mach, d = k[0] < 0 ? -1 : 1, steps = k[0] < 0 ? -k[0] : k[0];
        while (steps--)
            do
                m = (int32_t)sm_pr_wrap(m + d, SM_NMACHINE);
            while (!sm_pr_mach_ok((uint32_t)m));
        sm_pr.mach = (uint8_t)m;
        sm_pr.item = 0;
        sm_pr.arm = 0;
        sm_dirty = 1;
    }
    if (k[1]) {                                           /* K2: the category (ALL, BASS .. OTHER) */
        sm_pr.cat = (uint8_t)sm_pr_wrap((int32_t)sm_pr.cat + k[1], CAT_N);
        sm_pr.item = 0;
        sm_pr.arm = 0;
        sm_dirty = 1;
    }
    if (k[2]) {                                           /* K3: the sound to load, LOAD asked */
        uint32_t n = sm_pr_count();
        sm_pr.item = (uint8_t)sm_pr_wrap((int32_t)sm_pr.item + k[2], n);
        sm_pr.arm = n ? 1u : 0u;
        sm_dirty = 1;
    }
    if (k[3]) {                                           /* K4: the place to save into, SAVE asked */
        sm_pr.place = (uint8_t)sm_pr_wrap((int32_t)sm_pr.place + k[3], SM_UP_N);
        sm_pr.arm = 2;
        sm_dirty = 1;
    }
}

/* the name a SAVE gives: the machine's first four letters, the category's, the place's number ("SAW LEAD07"); 11 bytes */
static void sm_pr_autoname(char *b, uint32_t mach, uint32_t cat, uint32_t place)
{
    const char *mn = ENGINES[sm_engine_of(mach)]->name, *cn = CAT_NAME[cat % CAT_N];
    uint32_t i, j;
    for (i = 0; i < 4u; i++)                              /* (past a short name's end: spaces, never read on) */
        b[i] = ' ';
    for (i = 0; i < 4u && mn[i]; i++)
        b[i] = mn[i];
    for (j = 0; j < 4u; j++)
        b[4u + j] = ' ';
    for (j = 0; j < 4u && cn[j] && cn[j] != '-'; j++)
        b[4u + j] = cn[j];
    b[8] = (char)('0' + (place + 1u) / 10u % 10u);
    b[9] = (char)('0' + (place + 1u) % 10u);
    b[10] = 0;
    for (i = 0; b[i]; i++)
        b[i] = b[i] >= 'a' && b[i] <= 'z' ? (char)(b[i] - 32) : b[i];
}
/* the category a SAVE gives the selected track's sound: K2's, or (ALL) its factory sound's */
static uint32_t sm_pr_save_cat(const sm_sound_t *s)
{
    if (sm_pr.cat != SM_PR_ALL)
        return sm_pr.cat;
    return sm_pr_fcat(s->machine, s->preset);
}

/* OCT+ on the page: what K3 / K4 asked */
static void sm_pr_confirm(void)
{
    sm_sound_t *sd = sm_ui_sound();
    uint32_t k;
    if (sm_pr.arm == 1u) {
        int32_t u = sm_pr_item(sm_pr.item, &k);
        if (u < 0)
            return;
        if (!sm_mach_ok(sm_pr.mach, sm_ui.trk)) {         /* (the KIT: T4's alone) */
            sm_msg_show(SM_MSG_T4ONLY);
            return;
        }
        if (!u) {
            sm_sound_load(sm_pr.mach, k);
        } else {
            sm_urec_t r;
            sm_sound_t n;
            if (st_read(sm_up_addr(sm_up_rat(k)), &r, sizeof r) || !sm_up_rec_ok(&r) || r.snd.machine >= SM_NMACHINE) {
                sm_msg_show(SM_MSG_FLASH);
                return;
            }
            n = r.snd;
            n.len = sd->len;                              /* (the sequencer's, as a factory sound's load: sm_sound_load) */
            n.dnote = sd->dnote;
            n.dvel = sd->dvel;
            n.dlen = sd->dlen;
            n.dcond = sd->dcond;
            n.speed = sd->speed;
            fm1_irq_off();
            *sd = n;
            fm1_irq_on();
            if (sm_trk_kit(sm_ui.trk % SM_NTRK, &smp.pat[smp.cur % SM_NPAT]))
                sm_lanes_default(&smp.pat[smp.cur % SM_NPAT]);
            sm_ld_pend |= (uint8_t)(1u << sm_ui.trk);
        }
        sm_pr.arm = 0;
        sm_msg_show(SM_MSG_LOADED);
        sm_dirty = 1;
    } else if (sm_pr.arm == 2u) {
        sm_urec_t *r = &sm_up.rec;
        memset(r, 0, sizeof *r);                          /* (one waiting already: this one replaces it) */
        r->place = sm_pr.place % SM_UP_N;
        r->cat = (uint8_t)sm_pr_save_cat(sd);
        r->snd = *sd;
        sm_pr_autoname(r->name, sd->machine, r->cat, r->place);
        sm_pr.arm = 0;
        sm_pr.mach = sd->machine;                         /* (the list shows it: the machine saved, its category) */
        sm_pr.cat = r->cat;
        sm_up.pend = 1;
        if (*(volatile uint8_t *)&sm_playing)
            sm_msg_show(SM_MSG_SAVEPEND);
        else
            sm_up_poll();
        if (!sm_up.pend) {                                /* (written: K3 on it) */
            uint32_t i, kk;
            for (i = 0; sm_pr_item(i, &kk) >= 0; i++)
                if (sm_pr_item(i, &kk) == 1 && kk == r->place) {
                    sm_pr.item = (uint8_t)i;
                    break;
                }
        }
        sm_dirty = 1;
    }
}
