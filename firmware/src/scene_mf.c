/* SPDX-License-Identifier: GPL-3.0-only */
/* MONOFM: SCENES (MONOFM.md). Sixteen scenes A..P, each a whole project: the four tracks' sounds (engine, preset,
 * every parameter) and their patterns (steps, locks, automation), the master's globals. They are stored as the
 * projects are: A..D the project slots (OBJ_PROJECT0..3), E..P storage.c OBJ_MSC0.. (USR2 / USR3's flash).
 * SEQ held: a white key (F3 .. G5 = A .. P) tapped picks its scene; a second one tapped while SEQ is still held chains
 * the two (A B A B ..); a key held MSC_HOLD_MS stores the music there (stopped only: a flash erase stalls the audio).
 * ALGORITHM turned with SEQ held: the next / previous stored scene.
 * Stopped: a scene loads at once (project_load: the tempo and the master's globals too). Playing: it comes in where
 * track 1's pattern ends (its LEN): the main loop reads it beforehand into mf_stage (mf_scene_poll, flash reads are
 * fine while playing), the audio ISR copies it into the tracks on that step (mf_scene_tick); the tempo, the master's
 * globals and the tracks' MUTE stay as they are. Every track starts its pattern again from step 1 there. */
#define MSC_HOLD_MS 800u
#define MSC_NONE 0xFFu
static project_t mf_stage;                       /* the next scene, decoded (main loop); the ISR reads it once ready */
static struct {
    uint8_t cur;                                 /* the scene playing (MSC_NONE: none yet) */
    uint8_t next;                                /* the scene coming at the pattern's end (MSC_NONE none) */
    uint8_t chain[2], nchain;                    /* a chain of two (nchain 2: they take turns), else 0 */
    uint8_t staged;                              /* the scene mf_stage holds */
    volatile uint8_t ready;                      /* mf_stage is staged's: the ISR may take it */
    volatile uint8_t fired;                      /* the ISR took it: the main loop's part (mf_scene_poll) */
    uint8_t tap1;                                /* the scene tapped first in this SEQ hold (MSC_NONE none) */
    uint8_t key;                                 /* the white key down (+ 1, 0 none), since key_ms */
    uint8_t saved;                               /* .. it was held long enough: stored */
    uint32_t key_ms;
} msc = {MSC_NONE, MSC_NONE, {0, 0}, 0, MSC_NONE, 0, 0, MSC_NONE, 0, 0, 0};

static char msc_letter(uint32_t n) { return (char)('A' + n % 16u); }
static void msc_say(uint32_t n, const char *what)
{
    char b[10] = "SCENE ";
    b[6] = msc_letter(n);
    b[7] = 0;
    ui_say(b, what);
}

/* the audio ISR, at the end of track 1's pattern with a scene staged: the tracks take it (seq.c, before the tracks'
 * ticks: they start the new patterns in this block, mf_sc_carry samples into step 1) */
static __attribute__((noinline)) void mf_scene_apply(uint32_t carry)
{
    const project_t *p = &mf_stage;
    uint32_t k;
    for (k = 0; k < NTRK; k++) {
        track_t *t = &trk[k];
        const proj_trk_t *s = &p->t[k];
        int16_t mute = t->p[P_MUTE];
        seq_release(t);
        if (t->eng_req != s->engine)
            panic_req |= (uint8_t)(1u << k);         /* (another engine: what sounds is let go, engine_block switches) */
        t->eng_req = s->engine;
        memcpy(t->p, s->p, sizeof t->p);             /* (fitted to their ranges in the main loop: proj_bound) */
        t->p[P_MUTE] = mute;
        t->preset = (uint8_t)(ENGINES[s->engine]->npresets && s->preset < PROJ_DEF_KEEP ?
                              s->preset % ENGINES[s->engine]->npresets : 0u);
        t->user = 0;
        memcpy(t->step, s->step, sizeof t->step);
        t->seq_idx = (uint16_t)(t->p[P_SLEN] - 1);
        t->seq_pos = 0x7FFFFFFFu;
        t->rh_n = t->rskip_n = 0;
        t->euc_pos = 0;
    }
    motion = p->motion;
    memset(motion_active, 0, sizeof motion_active);
    motion_base_valid = 0;
    mf_sc_carry = carry;
    msc.ready = 0;
    msc.fired = 1;
}
static void mf_scene_tick(uint32_t n)
{
    const track_t *t = &trk[0];
    uint32_t length;
    if (!msc.ready || !song.playing || t->seq_pos >= 0x7FFFFFFFu || t->seq_idx + 1u < (uint32_t)t->p[P_SLEN])
        return;
    length = step_samples(t, div_samples((uint32_t)t->p[P_SDIV]), t->seq_idx);
    if (t->seq_pos + n < length)
        return;
    mf_scene_apply(t->seq_pos + n - length);
}

/* the scene now playing changed (a load, the ISR's switch): its name, its letter in the header, the pages */
static void msc_now(uint32_t n, const project_t *p)
{
    uint32_t k;
    msc.cur = (uint8_t)(n % 16u);
    if (p)
        proj_name_get(proj_name, (const uint8_t *)p->name);
    proj_cur = msc.cur;
    pat_from_set(msc.cur + 1u);
    for (k = 0; k < NTRK; k++)
        pat_sig[k] = ~steps_sig(&trk[k]);            /* (a scene's steps are the user's) */
    undo.trk = 0;                                     /* (the undo copy belongs to the scene before) */
    sync_reload = 1;
    ui.force = 1;
}
static void msc_load_now(uint32_t n)
{
    project_load(n);
    if (proj_cur == n % 16u) {
        msc_now(n, 0);
        msc_say(n, "");
    }
}

/* main loop, every frame: the ISR's switch done (the next of a chain), the next scene staged while playing, loaded at
 * once when stopped (one picked as it stopped; a chain's next waits for PLAY) */
static void mf_scene_poll(void)
{
    if (msc.fired) {
        msc.fired = 0;
        msc_now(msc.staged, &mf_stage);
        msc.next = msc.nchain == 2u ? msc.chain[msc.chain[0] == msc.cur] : MSC_NONE;
    }
    if (msc.next == MSC_NONE) {
        msc.ready = 0;
        return;
    }
    if (!transport_busy()) {
        msc.ready = 0;
        if (!msc.nchain) {
            uint32_t n = msc.next;
            msc.next = MSC_NONE;
            msc_load_now(n);
        }
        return;
    }
    if (msc.ready && msc.staged == msc.next)
        return;
    msc.ready = 0;                                    /* (the ISR leaves mf_stage alone from here) */
    if (!proj_import(&mf_stage, proj_store(msc.next), sizeof(project_store_t)) || !proj_engines_ok(&mf_stage)) {
        msc_say(msc.next, " EMPTY");
        msc.next = MSC_NONE;
        msc.nchain = 0;
        return;
    }
    proj_bound(&mf_stage);
    {
        uint32_t k, i;
        for (k = 0; k < NTRK; k++) {
            proj_trk_t *s = &mf_stage.t[k];
            s->engine = (uint8_t)(s->engine % NENGINES);
            for (i = P_SLEN; i <= P_SGATE; i++)
                s->p[i] = (int16_t)clamp(s->p[i], TP[i].min, TP[i].max);
            if (s->p[P_SLEN] < 1)
                s->p[P_SLEN] = 1;
        }
    }
    msc.staged = msc.next;
    RING_PUBLISH();
    msc.ready = 1;
}

/* pick scene n (SEQ + a white key, ALGORITHM); chain: a chain of the scene picked before and n */
static void mf_scene_pick(uint32_t n, int chain)
{
    n %= 16u;
    if (!mf_scene_info(n, 0)) {
        msc_say(n, transport_busy() ? " EMPTY" : " EMPTY: HOLD SAVES");
        return;
    }
    if (chain && msc.tap1 != MSC_NONE && msc.tap1 != n) {
        msc.chain[0] = msc.tap1;
        msc.chain[1] = (uint8_t)n;
        msc.nchain = 2;
        {
            char b[12] = "CHAIN x > x";
            b[6] = msc_letter(msc.chain[0]);
            b[10] = msc_letter(n);
            ui_message(b);
        }
        if (!transport_busy()) {                       /* stopped: the first now, the second next */
            if (msc.cur != msc.chain[0])
                msc_load_now(msc.chain[0]);
            msc.next = msc.chain[1];
            ui_message(proj_cur == msc.chain[0] ? "CHAIN READY" : "EMPTY SLOT");
            return;
        }
        msc.next = msc.cur == msc.chain[0] ? msc.chain[1] : msc.chain[0];
        return;
    }
    msc.nchain = 0;
    if (!transport_busy()) {
        msc.next = MSC_NONE;
        msc_load_now(n);
        return;
    }
    msc.next = (uint8_t)n;
    msc_say(n, " NEXT");
}

/* ALGORITHM with SEQ held: the stored scene after (s > 0) / before the one coming (or playing) */
static void mf_scene_turn(int32_t s)
{
    uint32_t base = msc.next != MSC_NONE ? msc.next : msc.cur != MSC_NONE ? msc.cur : (s > 0 ? 15u : 0u), i;
    for (i = 1; i <= 16u; i++) {
        uint32_t n = (base + 16u + (s > 0 ? i : 16u - i)) % 16u;
        if (mf_scene_info(n, 0)) {
            msc.tap1 = MSC_NONE;
            mf_scene_pick(n, 0);
            return;
        }
    }
    ui_message("NO SCENES SAVED");
}

static void mf_scene_saved(uint32_t n)
{
    msc.cur = (uint8_t)(n % 16u);
    msc.next = MSC_NONE;
    msc.nchain = 0;
    msc.ready = 0;
}
/* store the music as it is into scene n (stopped only; its name: the music's) */
static void mf_scene_save(uint32_t n)
{
    n %= 16u;
    if (transport_busy()) {
        ui_message("STOP TO SAVE");
        return;
    }
    if (!project_save_as(n, 0))
        msc_say(n, " SAVED");
}

/* the SEQ layer's white keys (ui_layer.c): k down (place p = the scene), each pass held or let go */
static void mf_scene_key(uint32_t k, uint32_t p)
{
    msc.key = (uint8_t)(k + 1u);
    msc.key_ms = fm1_ms;
    msc.saved = (uint8_t)p;                           /* (the scene; bit 7: stored already) */
}
static void mf_scene_keys(uint64_t held_keys)
{
    uint32_t k, p;
    if (!msc.key)
        return;
    k = msc.key - 1u;
    p = msc.saved & 15u;
    if ((held_keys >> k) & 1u) {
        if (!(msc.saved & 0x80u) && fm1_ms - msc.key_ms >= MSC_HOLD_MS) {
            msc.saved |= 0x80u;
            mf_scene_save(p);
        }
        return;
    }
    msc.key = 0;
    if (msc.saved & 0x80u)
        return;
    mf_scene_pick(p, 1);
    msc.tap1 = msc.nchain ? MSC_NONE : (uint8_t)p;    /* (a chain made: the next tap starts afresh) */
}
static void mf_scene_layer_open(void) { msc.tap1 = MSC_NONE; msc.key = 0; }
/* the SEQ layer's LEDs and map: the scene playing, the one coming / the chain */
static int mf_scene_is(uint32_t n) { return msc.cur == n; }
static int mf_scene_coming(uint32_t n) { return msc.next == n || (msc.nchain && (msc.chain[0] == n || msc.chain[1] == n)); }
static uint32_t mf_scene_sig(void)
{
    return (uint32_t)msc.cur * 31u + msc.next * 977u + msc.nchain * 7919u + msc.chain[0] * 13u + msc.chain[1] * 17u +
           mf_sc_used * 104729u + mf_sc_gen * 2654435761u;
}
