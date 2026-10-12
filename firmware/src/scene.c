/* SPDX-License-Identifier: GPL-3.0-only */
/* STRANGE: SCENES. Eight snapshots of the whole instrument, on GLO held: its white keys G4 .. G5 (ui_layer.c).
 * A tap calls one back (an empty one: stores there), a key held SC_HOLD_MS stores over it.
 * A scene: both parts (engine, preset or user preset, every parameter), the master's globals (BPM SWING, the delay,
 * reverb, chorus, REVERB TYPE, the master HPF / LPF), X and Y of both parts, and the notes each part's LATCH holds
 * (or EUCLID's held notes): they sound again when it is called back (seq.c sc_latch_*: set in the audio ISR).
 * FM6's patch edits are not kept (the preset's or the user preset's own patch comes back).
 * Storage: one object (storage.c OBJ_SCENES) in USR3's last two sectors (eng_sample.c SMP_SLOT_SIZE), RAM mirror. */
#define SC_N 8u
#define SC_HOLD_MS 800u
#define SC_MAGIC 0x31435353u                          /* "SSC1" */
typedef struct {
    uint8_t eng, preset, user, nlatch;                /* user: slot + 1 (track_t.user), 0 none */
    int8_t p[P_COUNT];                                /* every parameter, as stored by user presets: -64 .. 127 */
    uint8_t latch[16];
} sc_part_t;
typedef struct {
    uint8_t used, rsv[3];
    int16_t g[G_COUNT];
    uint8_t xy[2][2];
    sc_part_t part[2];
} scene_t;
typedef struct {
    uint32_t magic, pcount, gcount, size;
    scene_t s[SC_N];
} sc_bank_t;
_Static_assert(sizeof(sc_bank_t) <= 4096u - 256u, "the scenes fit one storage object (storage.c ST_PAYLOAD_MAX)");
static sc_bank_t sc_bank;
static uint8_t sc_last = 0xFFu;                       /* the scene called back or stored last (its key lit) */

static int sc_used(uint32_t n) { return n < SC_N && sc_bank.s[n].used; }
static int sc_save_bank(void);
/* "LYRA + STRG": the two parts' engines (SAVE > SCENES' list) */
static void sc_desc(uint32_t n, char *b, uint32_t cap)
{
    const scene_t *s = &sc_bank.s[n % SC_N];
    str_cpy(b, eng_abbr(ENGINES[s->part[0].eng % NENGINES]->name), cap);
    str_cpy(b + str_len(b), " + ", cap - str_len(b));
    str_cpy(b + str_len(b), eng_abbr(ENGINES[s->part[1].eng % NENGINES]->name), cap - str_len(b));
}
static int sc_erase(uint32_t n)
{
    if (!sc_used(n))
        return 1;
    sc_bank.s[n % SC_N].used = 0;
    if (sc_last == n)
        sc_last = 0xFFu;
    return sc_save_bank();
}
static int sc_global_kept(uint32_t g)                 /* the globals a scene holds */
{
    return g == G_BPM || g == G_SWING || (g >= G_DTIME && g <= G_CDEPTH) || g == G_RTYPE || g == G_MHPF || g == G_MLPF;
}

static void sc_boot(void)
{
#if FELUCCA_FLASH
    if (flash_ok && st_load(OBJ_SCENES, &sc_bank, sizeof sc_bank) == (int)sizeof sc_bank && sc_bank.magic == SC_MAGIC &&
        sc_bank.pcount == P_COUNT && sc_bank.gcount == G_COUNT && sc_bank.size == sizeof(scene_t))
        return;
#endif
    memset(&sc_bank, 0, sizeof sc_bank);
}

/* store the instrument as it is into scene n: 0 ok, 1 the flash failed (the RAM copy holds it until power-off) */
static int sc_store(uint32_t n)
{
    scene_t *s = &sc_bank.s[n % SC_N];
    uint32_t k, i;
    memset(s, 0, sizeof *s);
    s->used = 1;
    for (i = 0; i < G_COUNT; i++)
        s->g[i] = song.g[i];
    for (k = 0; k < 2u; k++) {
        const track_t *t = &trk[k];
        sc_part_t *q = &s->part[k];
        q->eng = (uint8_t)(t->eng_req % NENGINES);
        q->preset = t->preset;
        q->user = t->user;
        for (i = 0; i < P_COUNT; i++)
            q->p[i] = (int8_t)clamp(t->p[i], -64, 127);
        fm1_irq_off();                                /* (the ISR edits held[]) */
        q->nlatch = (uint8_t)((t->p[P_AHOLD] || t->p[P_AMODE]) ? t->nheld : 0u);
        for (i = 0; i < q->nlatch && i < 16u; i++)
            q->latch[i] = t->held[i];
        fm1_irq_on();
        s->xy[k][0] = xy_val[k][0];
        s->xy[k][1] = xy_val[k][1];
    }
    sc_last = (uint8_t)(n % SC_N);
    return sc_save_bank();
}
static int sc_save_bank(void)                         /* 0 ok, 1 the flash failed (RAM keeps it until power-off) */
{
    sc_bank.magic = SC_MAGIC;
    sc_bank.pcount = P_COUNT;
    sc_bank.gcount = G_COUNT;
    sc_bank.size = sizeof(scene_t);
#if FELUCCA_FLASH
    if (flash_ok && st_save(OBJ_SCENES, &sc_bank, sizeof sc_bank))
        return 1;
#endif
    return 0;
}

/* call scene n back: 0 ok, 1 empty */
static int sc_recall(uint32_t n)
{
    const scene_t *s = &sc_bank.s[n % SC_N];
    uint32_t k, i;
    if (!s->used)
        return 1;
    for (k = 0; k < 2u; k++) {
        track_t *t = &trk[k];
        const sc_part_t *q = &s->part[k];
        load_begin(t, UNDO_SOUND);
        panic_req |= (uint8_t)(1u << k);
        fm1_irq_off();                                /* the audio ISR must not see half a sound */
        t->eng_req = q->eng % NENGINES;
        for (i = 0; i < P_COUNT; i++)
            t->p[i] = q->p[i];                        /* (stored from a valid state: in range for that engine) */
        t->preset = q->preset;
        fm1_irq_on();
        if (q->user && up_used(q->user - 1u))
            upf_track_load(t, q->user - 1u);          /* FM6: the user preset's patch */
        else
            fm6_track_loaded(t);                      /* FM6: the preset's (its SLOT) */
        t->user = q->user && up_used(q->user - 1u) ? q->user : 0u;
        load_end(t);
        xy_val[k][0] = s->xy[k][0];
        xy_val[k][1] = s->xy[k][1];
        sc_latch_n[k] = (uint8_t)(q->nlatch > 16u ? 16u : q->nlatch);
        memcpy(sc_latch_note[k], q->latch, sizeof sc_latch_note[k]);
    }
    for (i = 0; i < G_COUNT; i++)
        if (sc_global_kept(i))
            song.g[i] = s->g[i];
    sc_latch_wait = 24u;                              /* (seq.c: the latched notes again, in the ISR, once the */
    sc_latch_req = 3u;                                /* engines have switched) */
    sc_last = (uint8_t)(n % SC_N);
    sync_reload = 1;
    ui.force = 1;
    return 0;
}

/* GLO's scene key n (0..7) pressed / let go / still held at ms (ui_layer.c): a tap calls it back (empty: stores),
 * held SC_HOLD_MS stores over it, once */
static uint8_t sc_key = 0xFFu, sc_done;
static uint32_t sc_t0;
static void sc_say(const char *what, uint32_t n)
{
    char b[16] = "SCENE ";
    b[6] = (char)('1' + n % SC_N);
    b[7] = 0;
    ui_say(b, what);
}
static void sc_press(uint32_t n)
{
    sc_key = (uint8_t)n;
    sc_t0 = fm1_ms;
    sc_done = 0;
}
static void sc_poll(int held)
{
    uint32_t n = sc_key;
    if (n >= SC_N)
        return;
    if (held && !sc_done && fm1_ms - sc_t0 >= SC_HOLD_MS) {
        sc_done = 1;
        sc_say(sc_store(n) ? " NOT SAVED" : " SAVED", n);
    } else if (!held) {
        sc_key = 0xFFu;
        if (sc_done)
            return;
        if (sc_used(n))
            sc_say(sc_recall(n) ? " EMPTY" : "", n);
        else
            sc_say(sc_store(n) ? " NOT SAVED" : " SAVED", n);
    }
}
