/* SPDX-License-Identifier: GPL-3.0-only */
/* PULHRA screen, Elektron mk1 look: one ink (white) on black, a 5 x 7 pixel font at scale 2, no smoothing, no grain;
 * a selection is an inversion. 240 x 240, every band its own canvas:
 *   y   0..19   header: T1 SAW 03 (track, machine, preset) left, the track inverted while it is muted (the keyboard mode: T1 KEYS,
 *               KEYS inverted and the octave, KEYS +2, in place of the machine and preset); A01 120.0 > (pattern,
 *               tempo, transport) right; REC on: ARM (stopped: grid record) or REC (playing: recording), inverted, at the right
 *               corner in place of the pattern; SHIFT +2 / TRN -3 (the net of SHIFT + D#4 F#4 / D#5 F#5) for a second;
 *               the lock pool full: LOCKS FULL (inverted) alone for a second
 *   y  20..47   the page and the preset: EDIT 1 . SYNC LEAD (a layer open: its name instead); STEP: STEP 05 . X3, the step
 *               edited and its FLAG (none: STEP . DEFAULT, the track's trig defaults)
 *   y  48..159  the four knobs, a column of 60 each: the label, a ring of 16 dots, the value. A ring of a value locked on the
 *               held step is a filled disc; a dot in its centre: locked on another step
 *   y 168..187  SELECT: its label and value
 *   (STEP: the knobs NOTE VEL LEN COND, SELECT NUDGE (NUDGE +3; no step edited: the track's SWNG 58%), of the step edited, a ring filled where the step holds its own value,
 *   plain where it inherits the track's default; an empty step: "-"; none selected: the track's trig defaults)
 *   y  48..217  (a layer open: its key map over the knobs and SELECT, five rows of 34, the labels of the keys at scale 2;
 *               SEQ: the title LEN 32 large (scale 3), the grid A01 .. A16 in four rows (the one playing inverted, the one
 *               picked underlined, the one asked for framed, an empty one dim), the change mode and the master length)
 *   y 220..239  the 16 steps of the bank as dots only, 13 px each (a trig a dot, a ratchet stacked dots, a REST a hollow dot;
 *               a bar under the one playing, over the one held; nothing past the track's length), and right of them the
 *               track's length, or, on a track of two pages (over 16 steps), the page: P1 / P2 (P2 inverted); the panel LEDs carry the rest of the steps (sm_leds.c). HOME > MIXER: the four tracks, a
 *               row of dots each (smd_strip4), and the rings the four LEVELs (a muted track's dim, the selected one's label
 *               inverted)
 * The LCD transfer is the dear part: a zone is redrawn only when what it shows changed (a signature of its inputs). */
#define SMD_HDR_Y 0u
#define SM_WHITE_DIM 0xB596u                                /* the inverted header / strip ground: white 30 % dimmer */
#define SMD_HDR_H 20u
#define SMD_PAGE_Y 20u
#define SMD_PAGE_H 28u
#define SMD_KNOB_Y 48u
#define SMD_KNOB_W 60u
#define SMD_KNOB_H 112u                                   /* (the value's text ends at row 112: nothing below it to redraw) */
#define SMD_SEL_Y 168u
#define SMD_SEL_H 20u
#define SMD_RING_CX 30                                    /* the ring in its 60 px column: centre, radius (dots reach r + 1) */
#define SMD_RING_CY 62
#define SMD_RING_R 24                                     /* (dots to x 5..55: 9 px of ground to the next ring; 02:43) */
#define SMD_MAP_Y 48u                                     /* the layer map: five rows over the knobs and SELECT */
#define SMD_MAP_ROW_H 34u
#define SMD_MAP_ROWS 5u
#define SMD_STRIP_Y 220u
#define SMD_STRIP_H 20u
#define SMD_STEP_W 13u                                    /* (16 x 13 = 208: the track's length right of the steps) */
#define SMD_NZONE 8u                                      /* header, page, four knobs, SELECT, strip */
#define SMD_Z_KNOB0 2u
#define SMD_Z_SEL 6u
#define SMD_Z_STRIP 7u

static uint32_t sm_draw_px;                               /* the pixels the last frame sent (the test's budget) */
static uint32_t sm_zsig[SMD_NZONE];                       /* what each zone shows now (its inputs' signature) */
static uint8_t smd_mapped;                                /* a layer map is on the screen (the bands the knobs do not cover to be cleared) */
static uint8_t sm_zvalid;                                 /* the bit per zone: sm_zsig is on the screen */
static uint8_t sm_last_pos = 0xFFu, sm_last_play = 0xFFu; /* the playing step and transport seen by the last frame */
/* the playing step of the selected track and the transport, read ONCE per frame (the audio stream moves them at any time):
 * every signature and every zone of the frame uses these two, so what is drawn is what the signature said */
static uint8_t smd_pos, smd_play, smd_rec, smd_live;      /* (and REC / live: the REC key sets them from the main loop) */
static uint8_t smd_pos4[SM_NTRK];                         /* HOME > MIXER: every track's playing step, read once per frame too */
static uint32_t sm_last_pos4 = 0xFFFFFFFFu;

/* the colours of the look (gfx.c's ux, what the splash and the canvas read): ink, dim, ground; nothing else is ever drawn */
static void sm_ux_mk1(void)
{
    ux.bg = 0x0000u; ux.surf = 0x0000u; ux.raise = 0x0000u; ux.tint = 0x0000u; ux.lane = 0x0000u; ux.grid = 0x0000u;
    ux.text = 0xFFFFu; ux.theme = 0xFFFFu; ux.accent = 0xFFFFu; ux.rec = 0xFFFFu; ux.sel = 0xFFFFu; ux.key = 0xFFFFu;
    ux.mid = 0x4208u; ux.dim = 0x4208u; ux.line = 0x4208u;
    ux.ink = 0x0000u;
    ux.light = 0; ux.mono = 0; ux.style = ST_FLAT;
    ux.grime = 0;
    ux.gen++;                                             /* the text ramps follow */
}

/* the screen was painted over (the UBOOT countdown's box, the update's screens): every zone again at the next frame */
static void sm_redraw_all(void)
{
    sm_zvalid = 0;
    smd_mapped = 0;                                       /* (painted over: the map, if open, is drawn again whole) */
    sm_dirty = 1;
}

static uint32_t smd_mix(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }

/* a band of the screen: w x h at x, y on the canvas (coordinates below are the canvas's) */
static uint32_t smd_x, smd_y;
static void smd_begin(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t bg)
{
    smd_x = x;
    smd_y = y;
    cv_begin(w, h, bg);
    sm_draw_px += w * h;
}
static void smd_end(void) { cv_blit(smd_x, smd_y); }

static const sm_sound_t *smd_snd(void) { return &smp.pat[smp.cur % SM_NPAT].kit.snd[sm_ui.trk % SM_NTRK]; }

/* s upper-cased into d, at most n characters (the ring's label, the preset's name) */
static void smd_upper(char *d, const char *s, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n && s[i]; i++)
        d[i] = s[i] >= 'a' && s[i] <= 'z' ? (char)(s[i] - 32) : s[i];
    d[i] = 0;
}

static const char *smd_preset_name(const sm_sound_t *s)
{
    const engine_t *e = ENGINES[sm_engine_of(s->machine)];
    return e->npresets ? e->presets[s->preset % e->npresets].name : "";
}

/* ------------------------------------------------------------ header --- */
static int32_t smd_len(const char *s)
{
    int32_t n = 0;
    while (s[n])
        n++;
    return n;
}
static uint32_t smd_muted(void)                           /* the selected track does not play (muted, or another one soloed) */
{
    return (sm_mute_eff() >> (sm_ui.trk % SM_NTRK)) & 1u;
}
static uint32_t smd_hdr_sig(void)
{
    const sm_sound_t *s = smd_snd();
    uint32_t h = 2166136261u;
    h = smd_mix(h, smd_muted()); h = smd_mix(h, sm_ui.trk); h = smd_mix(h, s->machine); h = smd_mix(h, s->preset); h = smd_mix(h, sm_gid_of(smp.cur));
    h = smd_mix(h, smp.bpm10); h = smd_mix(h, smd_play); h = smd_mix(h, sm_msg); h = smd_mix(h, (uint32_t)(uint16_t)sm_msg_val);
    h = smd_mix(h, (uint32_t)(uint8_t)sm_ui.oct);
    h = smd_mix(h, sm_ui.kbd); h = smd_mix(h, smp.nchain >= 2u);
    return smd_mix(smd_mix(h, smd_rec), smd_live);
}
static void smd_two(char *b, uint32_t v)
{
    v %= 100u;
    b[0] = (char)('0' + v / 10u);
    b[1] = (char)('0' + v % 10u);
}
/* pattern id g as A01 .. E08 (b: 4 bytes); short: A1 .. E8 (the GLO grid) */
static void smd_gname(char *b, uint32_t g, int short_)
{
    g %= SM_NGPAT;
    b[0] = (char)('A' + g / SM_BPAT);
    if (short_) {
        b[1] = (char)('1' + g % SM_BPAT);
        b[2] = 0;
        return;
    }
    smd_two(b + 1, g % SM_BPAT + 1u);
    b[3] = 0;
}
static void smd_lenstr(char *b, uint32_t v)               /* 1 .. 128 as text, no leading zero (b: 4 bytes) */
{
    uint32_t i = 0;
    v %= 1000u;
    if (v >= 100u)
        b[i++] = (char)('0' + v / 100u);
    if (v >= 10u)
        b[i++] = (char)('0' + v / 10u % 10u);
    b[i++] = (char)('0' + v % 10u);
    b[i] = 0;
}
static void smd_int(char *b, int32_t v, int plus);
static void smd_header(void)
{
    char l[8], a[4], t[12], nm[6];
    uint32_t rec = smd_rec;
    int32_t x;
    const sm_sound_t *s = smd_snd();
    smd_begin(0, SMD_HDR_Y, 240u, SMD_HDR_H, SM_WHITE_DIM);
    if (sm_msg) {                                         /* a message (LOCKS FULL, CLEARED, UNDO), inverted, the whole header for 1 s */
        char mb[16];
        const char *m = SM_MSG_TEXT[sm_msg % SM_MSG_COUNT];
        if (sm_msg == SM_MSG_SHIFT || sm_msg == SM_MSG_TRN) {   /* the text and its signed value: SHIFT +2, TRN -3 */
            uint32_t n = 0;
            while (m[n] && n < 6u) {
                mb[n] = m[n];
                n++;
            }
            mb[n++] = ' ';
            smd_int(mb + n, sm_msg_val, 1);
            m = mb;
        }
        sm_px_text((240 - 12 * smd_len(m)) / 2, 2, 2u, m, T_BG, 1);
        smd_end();
        return;
    }
    /* left: T1 SAW 03 - track, machine (four letters), preset; pieces a half pitch (6 px) apart */
    l[0] = 'T'; l[1] = (char)('1' + sm_ui.trk % SM_NTRK); l[2] = 0;
    x = sm_px_text(2, 2, 2u, l, T_BG, (int)smd_muted()) + 2 + 6;   /* (a muted track: inverted) */
    if (sm_ui.kbd) {                                      /* the keyboard mode: KEYS, inverted, where the machine and preset are */
        char o[4];                                        /* KEYS +2: the octave, -3 .. +3 (0 plain) */
        x += sm_px_text(x, 2, 2u, "KEYS", T_BG, 1) + 6;
        smd_int(o, sm_ui.oct, 1);
        sm_px_text(x, 2, 2u, o, T_BG, 0);
    } else {
        smd_upper(nm, ENGINES[sm_engine_of(s->machine)]->name, 4u);
        x += sm_px_text(x, 2, 2u, nm, T_BG, 0) + 6;
        smd_two(l, (uint32_t)s->preset + 1u);
        l[2] = 0;
        sm_px_text(x, 2, 2u, l, T_BG, 0);
    }
    /* right, from the right edge: A01 120.0 >   (while recording: 120.0 > REC, the pattern gives way) */
    fmt_fix(t, smp.bpm10, 1);
    smd_gname(a, sm_gid_of(smp.cur), 0);
    x = 240 - (rec ? 12 * 3 + 6 : 0) - 12;
    sm_px_text(x, 2, 2u, smd_play ? "\x10" : "\x11", T_BG, 0);
    x -= 6 + 12 * smd_len(t);
    sm_px_text(x, 2, 2u, t, T_BG, 0);
    if (rec)
        sm_px_text(240 - 36, 2, 2u, smd_live ? "REC" : "ARM", T_BG, 1);   /* (armed, grid record: ARM, even while playing; REC + PLAY: REC) */
    else
        sm_px_text(x - 6 - 36, 2, 2u, a, T_BG, smp.nchain >= 2u);   /* (a chain plays: the pattern inverted) */
    smd_end();
}

/* -------------------------------------------------------------- page --- */
/* the STEP page's step: the step edited (sm_step_edit), 0 none; *st its index */
static const sm_step_t *smd_step(int32_t *st)
{
    int32_t i = sm_step_edit();
    *st = i;
    return i < 0 ? 0 : &smp.pat[smp.cur % SM_NPAT].step[sm_ui.trk % SM_NTRK][i];
}
/* a trig held (shown: past the hold time, or turned) on a sound page that is not STEP / HOME / a layer: PRESETS is its FLAG */
static int smd_flag_held(void)
{
    return !sm_layer && !sm_page_is_step() && !sm_page_home() && sm_ui.held_step < SM_NSTEP && sm_trig_held() >= 0;
}
static const sm_step_t *smd_held_step(void)
{
    return &smp.pat[smp.cur % SM_NPAT].step[sm_ui.trk % SM_NTRK][sm_ui.held_step % SM_NSTEP];
}
static uint32_t smd_held_flags(void)
{
    const sm_step_t *p = smd_held_step();
    return p->n ? 0x100u + p->flags : 0u;
}
static uint32_t smd_page_sig(void)
{
    const sm_sound_t *s = smd_snd();
    uint32_t h = smd_mix(smd_mix(smd_mix(2166136261u, sm_ui.page), s->machine), s->preset);
    if (sm_page_is_step()) {
        int32_t st;
        const sm_step_t *p = smd_step(&st);
        h = smd_mix(smd_mix(smd_mix(h, (uint32_t)st), p ? p->n : 0u), p ? p->flags : 0u);
    }
    if (smd_flag_held())                                  /* (a trig held on a sound page: its FLAG is the line) */
        h = smd_mix(smd_mix(h, 0x200u + sm_ui.held_step), smd_held_flags());
    if (sm_map_open())                                    /* (the title of the layer: its page of machines, the track's length) */
        h = smd_mix(smd_mix(smd_mix(smd_mix(h, 0x100u + sm_layer), sm_lay_epage), s->len), sm_ui.trk * 8u + sm_glo_bank);
    return h;
}
static void smd_cat(char *d, uint32_t *n, const char *s)   /* s appended (upper case) at d[*n] */
{
    uint32_t i;
    for (i = 0; s[i] && *n < 20u; i++)
        d[(*n)++] = s[i] >= 'a' && s[i] <= 'z' ? (char)(s[i] - 32) : s[i];
    d[*n] = 0;
}
static void smd_page(void)
{
    char b[24];
    uint32_t n;
    smd_begin(0, SMD_PAGE_Y, 240u, SMD_PAGE_H, T_BG);
    if (sm_layer) {                                /* a layer open: its name in place of the page and the preset */
        n = 0;
        if (sm_layer == SML_EDIT) {
            smd_cat(b, &n, "MACHINE ");
            smd_cat(b, &n, MF_PAGE_NAME[sm_lay_epage % SM_NEPAGE]);
        } else if (sm_layer == SML_GLO) {                 /* BANK A MUTE SOLO (the bank in view) */
            char bn[2] = {(char)('A' + sm_glo_bank % SM_NBANK), 0};
            smd_cat(b, &n, "BANK ");
            smd_cat(b, &n, bn);
            smd_cat(b, &n, " MUTE SOLO");
        } else {                                          /* SEQ: LEN 32 large, SEQ T1 at the right */
            char l[8] = {'L', 'E', 'N', ' ', 0, 0, 0, 0};
            uint32_t len = smd_snd()->len % 100u;
            smd_cat(b, &n, "SEQ T");
            b[n++] = (char)('1' + sm_ui.trk % SM_NTRK);
            b[n] = 0;
            smd_lenstr(l + 4, len);
            sm_px_text(0, 3, 3u, l, T_TEXT, 0);
            sm_px_text(240 - 12 * (int32_t)n, 7, 2u, b, T_TEXT, 0);
            smd_end();
            return;
        }
    } else if (smd_flag_held()) {                       /* FLAG ACC (the step held) */
        const sm_step_t *p = smd_held_step();
        n = 0;
        smd_cat(b, &n, "FLAG ");
        smd_cat(b, &n, p->n ? SM_FLAGN[sm_flag_place(p->flags)] : "-");
    } else if (sm_page_is_step()) {                     /* STEP 05 . X3 (the step edited, its FLAG); STEP - none */
        int32_t st;
        const sm_step_t *p = smd_step(&st);
        n = 0;
        smd_cat(b, &n, "STEP ");
        if (!p) {                                         /* (none selected: the knobs are the track's trig defaults) */
            b[n++] = '\x12'; b[n++] = ' ';
            b[n] = 0;
            smd_cat(b, &n, "DEFAULT");
        } else {
            smd_two(b + n, (uint32_t)st + 1u);
            n += 2;
            b[n++] = ' '; b[n++] = '\x12'; b[n++] = ' ';
            b[n] = 0;
            smd_cat(b, &n, p->n ? SM_FLAGN[sm_flag_place(p->flags)] : "-");
        }
    } else if (sm_page_home() == SMH_EUCLID) {          /* EUCLID . OCT+ GEN (OCT+ generates the trigs) */
        n = 0;
        smd_cat(b, &n, "EUCLID \x12 OCT+ GEN");
    } else if (sm_page_home()) {                        /* MIXER, CLOCK, MENU: the page alone (the sound is not what the knobs edit) */
        smd_upper(b, sm_cur_page()->name, 8u);
    } else if (sm_cur_page()->fam == SMFAM_DEEP) {      /* DLY . GLOBAL (the deep pages are the project's, no preset) */
        n = 0;
        smd_cat(b, &n, sm_cur_page()->name);
        smd_cat(b, &n, " \x12 GLOBAL");
    } else {
        smd_upper(b, sm_cur_page()->name, 8u);
        for (n = 0; b[n]; n++) {}
        b[n++] = ' '; b[n++] = '\x12'; b[n++] = ' ';
        smd_upper(b + n, smd_preset_name(smd_snd()), 20u - n);     /* (the line holds twenty characters) */
    }
    sm_px_text(0, 2, 2u, b, T_TEXT, 0);
    smd_end();
}

/* ------------------------------------------------------------- knobs --- */
typedef struct {
    const param_desc_t *d;                                /* 0: no parameter on this knob */
    int on;                                               /* the knob is drawn (a parameter, or a STEP field) */
    char lab[8], val[12];
    uint32_t pos;                                         /* 0..256 on the ring */
    int filled, dot;                                      /* locked on the held step; locked elsewhere */
    int dim, inv;                                         /* MIXER: the track muted (a dim ring); the selected track (label inverted) */
    int32_t v;                                            /* the value shown: the lock's on the held step */
} smd_knob_t;
/* a note's name: C-1 .. G9 (b: 6 bytes) */
static void smd_note(char *b, uint32_t n)
{
    static const char NM[12][3] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    int32_t o = (int32_t)(n / 12u) - 1;
    uint32_t i = 0, k;
    for (k = 0; NM[n % 12u][k]; k++)
        b[i++] = NM[n % 12u][k];
    if (o < 0)
        b[i++] = '-';
    b[i++] = (char)('0' + (o < 0 ? -o : o));
    b[i] = 0;
}
static void smd_int(char *b, int32_t v, int plus)         /* a signed integer (plus: "+4") */
{
    uint32_t i = 0, a = (uint32_t)(v < 0 ? -v : v);
    if (v < 0)
        b[i++] = '-';
    else if (plus && v > 0)
        b[i++] = '+';
    if (a >= 100u)
        b[i++] = (char)('0' + a / 100u);
    if (a >= 10u)
        b[i++] = (char)('0' + a / 10u % 10u);
    b[i++] = (char)('0' + a % 10u);
    b[i] = 0;
}
/* the STEP page's knob k: its label and the field the trig plays (an empty step: "-", the ring at 0). A field the step holds
 * itself: the ring filled (as a lock); one it inherits from the track's default: the ring plain. No step selected: the
 * track's trig defaults */
static void smd_step_knob(uint32_t k, smd_knob_t *o)
{
    static const char *const LAB[4] = {"NOTE", "VEL", "LEN", "COND"};
    int32_t st;
    const sm_step_t *p = smd_step(&st);
    const sm_sound_t *d = smd_snd();
    sm_step_t e;
    uint32_t i;
    o->on = 1;
    for (i = 0; LAB[k][i]; i++)
        o->lab[i] = LAB[k][i];
    o->lab[i] = 0;
    o->val[0] = '-';
    o->val[1] = 0;
    if (p && !p->n)
        return;
    if (p) {
        sm_step_eff(p, d, &e);
        o->filled = (k == 0u ? p->note[0] : k == 1u ? p->vel : k == 2u ? p->len : p->cond) != SM_INH;
    } else {
        memset(&e, 0, sizeof e);
        e.note[0] = d->dnote;
        e.vel = d->dvel;
        e.len = d->dlen;
        e.cond = d->dcond;
    }
    switch (k) {
    case 0: smd_note(o->val, e.note[0] & 127u); o->pos = (uint32_t)(e.note[0] & 127u) * 256u / 127u; break;
    case 1: smd_int(o->val, e.vel, 0); o->pos = (uint32_t)(e.vel > 1u ? e.vel - 1u : 0u) * 256u / 126u; break;
    case 2: i = e.len < SM_NLEN ? e.len : 0u; smd_upper(o->val, SM_LEN_N[i], 5u); o->pos = i * 256u / (SM_NLEN - 1u); break;
    default: sm_cond_name(e.cond, o->val); o->pos = sm_cond_place(e.cond) * 256u / (SM_COND_PLACES - 1u); break;
    }
    o->v = (int32_t)o->pos;
}
/* the HOME pages' knob k: MIXER the LEVEL of T(k+1) (label T1..T4); CLOCK K1 BPM "120.0" (30.0 .. 300.0), K2 SWING "57%" */
static void smd_home_knob(uint32_t k, smd_knob_t *o)
{
    uint32_t i;
    o->on = 1;
    if (sm_page_home() == SMH_EUCLID) {                   /* K1 STEPS, K2 HITS, K3 ROT */
        static const char *const L[3] = {"STEPS", "HITS", "ROT"};
        uint32_t v = k == 0u ? sm_eu_steps : k == 1u ? sm_eu_hits : sm_eu_rot, mx = k == 0u ? SM_NSTEP : k == 1u ? sm_eu_steps : sm_eu_steps - 1u;
        if (k > 2u) {
            o->on = 0;
            return;
        }
        for (i = 0; L[k][i]; i++)
            o->lab[i] = L[k][i];
        o->lab[i] = 0;
        smd_int(o->val, (int32_t)v, 0);
        o->v = (int32_t)v;
        o->pos = (k == 0u ? (v - 1u) * 256u / (SM_NSTEP - 1u) : mx ? v * 256u / mx : 0u);
        return;
    }
    if (sm_page_home() == SMH_MIDI) {                     /* K1..K4: the MIDI channel of T1..T4, OFF (0) + 1..16 */
        o->on = 1;
        o->lab[0] = 'T'; o->lab[1] = (char)('1' + k); o->lab[2] = 0;
        if (sm_midi_ch[k])
            smd_int(o->val, (int32_t)sm_midi_ch[k], 0);
        else {
            o->val[0] = 'O'; o->val[1] = 'F'; o->val[2] = 'F'; o->val[3] = 0;
        }
        o->v = (int32_t)sm_midi_ch[k];
        o->pos = (uint32_t)sm_midi_ch[k] * 256u / 16u;
        o->dim = 0; o->inv = 0;
        return;
    }
    if (sm_page_home() == SMH_SETUP) {
        o->on = 1;
        if ((k & 3u) == 0u) {
            o->lab[0] = 'M'; o->lab[1] = 'O'; o->lab[2] = 'D'; o->lab[3] = 'E'; o->lab[4] = 0;
            static const char *const PM_N[] = {"SEQ", "DIR.S", "DIR.J"};
            smd_upper(o->val, PM_N[smp.mode % SM_PM_COUNT], 5u);
            o->v = (int32_t)smp.mode;
            o->pos = smp.mode * 256u / (SM_PM_COUNT - 1u);
            o->dim = 0; o->inv = 0;
        } else if ((k & 3u) == 1u) {
            o->lab[0] = 'S'; o->lab[1] = 'R'; o->lab[2] = 'C'; o->lab[3] = 0;
            static const char *const SRC_N[] = {"USB", "INT", "JACK"};
            smd_upper(o->val, SRC_N[sm_midi_src % 3], 5u);
            o->v = (int32_t)sm_midi_src;
            o->pos = sm_midi_src * 256u / 2u;
            o->dim = 0; o->inv = 0;
        } else { o->on = 0; }
        return;
    }
    if (sm_page_home() == SMH_MIXER) {
        const sm_sound_t *s = &smp.pat[smp.cur % SM_NPAT].kit.snd[k % SM_NTRK];
        const param_desc_t *d = sm_desc_safe(sm_engine_of(s->machine), P_LEVEL);
        const char *unit;
        o->lab[0] = 'T'; o->lab[1] = (char)('1' + k % SM_NTRK); o->lab[2] = 0;
        o->on = d != 0;
        o->dim = (int)((sm_mute_eff() >> (k % SM_NTRK)) & 1u);   /* (muted, or another track soloed: it is not heard) */
        o->inv = k % SM_NTRK == sm_ui.trk % SM_NTRK;
        if (!d)
            return;
        o->d = d;
        o->v = clamp(s->p[P_LEVEL], d->min, d->max);
        o->pos = d->max > d->min ? (uint32_t)((o->v - d->min) * 256 / (d->max - d->min)) : 0u;
        param_format(d, o->v, o->val, &unit);
        o->val[5] = 0;
    } else if (k == 0u) {
        o->lab[0] = 'B'; o->lab[1] = 'P'; o->lab[2] = 'M'; o->lab[3] = 0;
        fmt_fix(o->val, smp.bpm10, 1);
        o->v = smp.bpm10;
        o->pos = ((uint32_t)clamp(smp.bpm10, 300, 3000) - 300u) * 256u / 2700u;
    } else if (k == 1u) {
        const char *w = "SWING";
        for (i = 0; w[i]; i++)
            o->lab[i] = w[i];
        o->lab[i] = 0;
        smd_int(o->val, clamp(smp.swing, 50, 75), 0);
        for (i = 0; o->val[i]; i++) {}
        o->val[i] = '%';
        o->val[i + 1u] = 0;
        o->v = smp.swing;
        o->pos = ((uint32_t)clamp(smp.swing, 50, 75) - 50u) * 256u / 25u;
    } else {
        o->on = 0;
    }
}
/* a parameter's label on the screen: params.c names P_AHOLD "HOLD" (the module's "LATCH" is behind FELUCCA_MODULE); the ARP 2 page
 * says LATCH, and ORDER is spelled out */
static const char *smd_label(uint32_t id, const param_desc_t *d)
{
    if (id == P_DIST && sm_cur_page()->id[0] == P_DTYPE)  /* DIST's page: the send is the drive */
        return "DRIVE";
    return id == P_AHOLD ? "LATCH" : id == P_AORDER ? "ORDER" : id == P_DOUT ? "LVL" : id == P_ITYPE ? "TYPE" :
           id == (SM_GLB | G_DCOLOR) ? "COLOR" : id == (SM_GLB | G_CRATE) ? "RATE" : id == (SM_GLB | G_CDEPTH) ? "DEPTH" : d->label;
}
static uint32_t smd_itype_place(int32_t v)                /* the place of an INSERT TYPE in the track's list (0..4) */
{
    uint32_t i;
    for (i = 0; i < 4u && (int32_t)SM_ITYPES[i] < v; i++) {}
    return i;
}
static void smd_knob_get(uint32_t k, smd_knob_t *o)
{
    const sm_sound_t *s = smd_snd();
    const sm_pattern_t *pt = &smp.pat[smp.cur % SM_NPAT];
    uint32_t id = sm_page_param(k & 3u), t = sm_ui.trk % SM_NTRK;
    const char *unit;
    o->d = 0;
    o->lab[0] = o->val[0] = 0;
    o->pos = 0;
    o->filled = o->dot = 0;
    o->dim = o->inv = 0;
    o->v = 0;
    o->on = 0;
    if (sm_page_is_step()) {
        smd_step_knob(k & 3u, o);
        return;
    }
    if (sm_page_home()) {
        smd_home_knob(k & 3u, o);
        return;
    }
    if (id == SM_NOPARAM)
        return;
    if (id & SM_GLB) {                                    /* a global: song.g, no locks */
        o->d = &GP[(id & ~SM_GLB) % G_COUNT];
        o->on = 1;
        o->v = clamp(song.g[(id & ~SM_GLB) % G_COUNT], o->d->min, o->d->max);
        o->pos = o->d->max > o->d->min ? (uint32_t)((o->v - o->d->min) * 256 / (o->d->max - o->d->min)) : 0u;
        smd_upper(o->lab, smd_label(id, o->d), 5u);
        param_format(o->d, o->v, o->val, &unit);
        o->val[5] = 0;
        return;
    }
    o->d = sm_desc_trk(s, id);
    if (!o->d)
        return;
    o->on = 1;
    o->v = s->p[id];
    if (sm_ui.held_step < SM_NSTEP) {
        int32_t li = sm_lock_find(pt, t, sm_ui.held_step, id);
        if (li >= 0) {
            o->filled = 1;
            o->v = pt->lock[li].val;
        }
    }
    o->dot = !o->filled && sm_param_locked_any(t, id);
    o->v = clamp(o->v, o->d->min, o->d->max);
    if (o->d->max > o->d->min)
        o->pos = (uint32_t)((o->v - o->d->min) * 256 / (o->d->max - o->d->min));
    if (id == P_ITYPE)                                    /* (the track's INSERT list is five of the nine: the ring walks it) */
        o->pos = smd_itype_place(o->v) * 256u / 4u;
    smd_upper(o->lab, smd_label(id, o->d), 5u);
    param_format(o->d, o->v, o->val, &unit);
    o->val[5] = 0;
}
static uint32_t smd_knob_sig(uint32_t k)
{
    smd_knob_t o;
    uint32_t h = 2166136261u;
    uint32_t i;
    smd_knob_get(k, &o);
    h = smd_mix(h, sm_ui.page); h = smd_mix(h, smd_snd()->machine); h = smd_mix(h, (uint32_t)o.on);
    h = smd_mix(h, (uint32_t)o.v); h = smd_mix(h, (uint32_t)o.filled); h = smd_mix(h, (uint32_t)o.dot);
    h = smd_mix(h, (uint32_t)(o.dim * 2 + o.inv));
    for (i = 0; o.val[i]; i++)                            /* (the STEP page: the text is the value) */
        h = smd_mix(h, (uint8_t)o.val[i]);
    return h;
}
static void smd_knob(uint32_t k)
{
    smd_knob_t o;
    smd_knob_get(k, &o);
    smd_begin(k * SMD_KNOB_W, SMD_KNOB_Y, SMD_KNOB_W, SMD_KNOB_H, T_BG);
    if (o.on) {
        sm_px_text((60 - 12 * smd_len(o.lab)) / 2, 4, 2u, o.lab, T_TEXT, o.inv);
        sm_px_ring(SMD_RING_CX, SMD_RING_CY, SMD_RING_R, o.pos, o.filled, o.dot, o.dim);
        sm_px_text((60 - 12 * smd_len(o.val)) / 2, 98, 2u, o.val, T_TEXT, 0);
    }
    smd_end();
}

/* ------------------------------------------------------------ SELECT --- */
static int32_t smd_sel_val(uint32_t id)                  /* SELECT's value: the held step's lock of it, else the base */
{
    const sm_pattern_t *pt = &smp.pat[smp.cur % SM_NPAT];
    int32_t li = sm_ui.held_step < SM_NSTEP ? sm_lock_find(pt, sm_ui.trk % SM_NTRK, sm_ui.held_step, id) : -1;
    if (id & SM_GLB)
        return song.g[(id & ~SM_GLB) % G_COUNT];
    return li >= 0 ? pt->lock[li].val : smd_snd()->p[id];
}
/* the STEP page's SELECT: a step edited, NUDGE and its value ("-" an empty step); none, SWNG and the track's swing ("58%");
 * returns the label */
static const char *smd_step_nudge(char *b)
{
    int32_t st;
    const sm_step_t *p = smd_step(&st);
    if (!p) {
        uint32_t w = smd_snd()->swing;
        smd_int(b, 50 + (int32_t)(w < 25u ? w : 25u), 0);
        b[2] = '%';
        b[3] = 0;
        return "SWNG";
    }
    if (p->n)
        smd_int(b, p->nudge, 1);
    else {
        b[0] = '-';
        b[1] = 0;
    }
    return "NUDGE";
}
static uint32_t smd_sel_sig(void)
{
    const sm_sound_t *s = smd_snd();
    uint32_t id = sm_page_param(4);
    uint32_t h = smd_mix(smd_mix(2166136261u, sm_ui.page), s->machine);
    if (sm_page_is_step())
        return smd_mix(h, (uint32_t)clamp(s->p[P_LEVEL], TP[P_LEVEL].min, TP[P_LEVEL].max));
    if (sm_page_home() && sm_page_home() != SMH_MIDI && sm_page_home() != SMH_SETUP)   /* (the selected track's volume) */
        return smd_mix(h, (uint32_t)clamp(s->p[P_LEVEL], TP[P_LEVEL].min, TP[P_LEVEL].max));
    return id == SM_NOPARAM ? h : smd_mix(smd_mix(h, id), (uint32_t)smd_sel_val(id));
}
static void smd_select(void)
{
    const sm_sound_t *s = smd_snd();
    uint32_t id = sm_page_param(4);
    const param_desc_t *d = id == SM_NOPARAM ? 0 : (id & SM_GLB) ? &GP[(id & ~SM_GLB) % G_COUNT] : sm_desc_trk(s, id);
    smd_begin(0, SMD_SEL_Y, 240u, SMD_SEL_H, T_BG);
    if (sm_page_is_step()) {
        char lab[8], val[12];
        const char *unit;
        int32_t x;
        smd_upper(lab, "LEVEL", 5u);
        param_format(&TP[P_LEVEL], clamp(s->p[P_LEVEL], TP[P_LEVEL].min, TP[P_LEVEL].max), val, &unit);
        x = sm_px_text(4, 2, 2u, lab, T_TEXT, 0) + 4 + 12;
        val[(240 - x) / 12 < 11 ? (240 - x) / 12 : 11] = 0;
        sm_px_text(x, 2, 2u, val, T_TEXT, 1);
    } else if (sm_page_home() == SMH_EUCLID || sm_page_home() == SMH_MIDI || sm_page_home() == SMH_SETUP) {
        /* (nothing on SELECT for EUCLID / MIDI / SETUP) */
    } else if (sm_page_home()) {                           /* all HOME pages: SELECT is the selected track's volume */
        char lab[8], val[12];
        const char *unit;
        int32_t x;
        const sm_sound_t *s = smd_snd();
        smd_upper(lab, "LEVEL", 5u);
        param_format(&TP[P_LEVEL], clamp(s->p[P_LEVEL], TP[P_LEVEL].min, TP[P_LEVEL].max), val, &unit);
        x = sm_px_text(4, 2, 2u, lab, T_TEXT, 0) + 4 + 12;
        val[(240 - x) / 12 < 11 ? (240 - x) / 12 : 11] = 0;
        sm_px_text(x, 2, 2u, val, T_TEXT, 1);
        sm_px_text(x, 2, 2u, val, T_TEXT, 1);
    } else if (d) {
        char lab[8], val[12];
        const char *unit;
        int32_t x;
        smd_upper(lab, id == P_LD_FLT ? "LFO>FLT" : smd_label(id, d), 7u);   /* (FILTER's SELECT: the LFO's depth, not a filter's FLT) */
        param_format(d, clamp(smd_sel_val(id), d->min, d->max), val, &unit);
        x = sm_px_text(4, 2, 2u, lab, T_TEXT, 0) + 4 + 12;
        val[(240 - x) / 12 < 11 ? (240 - x) / 12 : 11] = 0;   /* (what fits the row) */
        sm_px_text(x, 2, 2u, val, T_TEXT, 1);
    }
    smd_end();
}

/* -------------------------------------------------------- layer map --- */
/* an open layer's map: five rows of 34 px over the knobs and SELECT (y 48 .. 217), each row either one text across (w) or four
 * cells of 60 px (c), a cell inverted for what is on (the machine of the track, a mute, a solo). The labels are scale 2 */
typedef struct { char w[22]; char c[5][6]; uint8_t inv, cells, winv, dim, und, box, cols; } smd_mrow_t;   /* (bits per cell;
                                                                                         cols: 4, or 5 cells of 48 px) */
static void smd_cell(smd_mrow_t *o, uint32_t i, const char *s, uint32_t inv)
{
    smd_upper(o->c[i], s, 4u);
    o->inv |= (uint8_t)((inv ? 1u : 0u) << i);
    o->cells = (uint8_t)(i + 1u > o->cells ? i + 1u : o->cells);
}
/* the pattern asked for (its id), SM_PAT_NONE none */
static uint32_t smd_next_g(void) { return smp.next < SM_NPAT ? sm_gid_of(smp.next) : SM_PAT_NONE; }
/* the banks A .. E, five cells, the bank in view vb inverted, the current pattern's bank underlined */
static void smd_bank_row(smd_mrow_t *o, uint32_t vb)
{
    static const char *const BN[SM_NBANK] = {"A", "B", "C", "D", "E"};
    uint32_t b, cur = sm_gid_of(smp.cur);
    o->cols = 5;
    for (b = 0; b < SM_NBANK; b++) {
        smd_cell(o, b, BN[b], b == vb);
        o->und |= (uint8_t)((cur < SM_NGPAT && cur / SM_BPAT == b ? 1u : 0u) << b);
    }
}
static void smd_map_get(uint32_t r, smd_mrow_t *o)
{
    static const char *const TN[4] = {"T1", "T2", "T3", "T4"};
    uint32_t i, c, n = 0;
    memset(o, 0, sizeof *o);
    if (sm_layer == SML_EDIT) {
        const sm_sound_t *s = smd_snd();
        uint32_t pg = sm_lay_epage % SM_NEPAGE, cnt = sm_epage_n(pg);
        if (r == 4u) {
            smd_cat(o->w, &n, "OCT+ NEXT PAGE");
            return;
        }
        for (c = 0; c < 4u; c++) {
            i = r * 4u + c;
            if (i == SM_LAY_INIT_KEY)
                smd_cell(o, c, "INIT", 0);
            else if (i < cnt)
                smd_cell(o, c, ENGINES[sm_engine_of(sm_epage_first(pg) + i)]->name, sm_epage_first(pg) + i == s->machine);
        }
        o->cells = 4;
    } else if (sm_layer == SML_GLO) {                     /* the bank in view: patterns 1..8 (two rows), the banks, mute, solo */
        if (r < 2u) {
            uint32_t cur = sm_gid_of(smp.cur);
            for (c = 0; c < 4u; c++) {
                char a[4];
                uint32_t g = sm_glo_bank * SM_BPAT + r * 4u + c;
                smd_gname(a, g, 1);
                smd_cell(o, c, a, g == cur);
                o->dim |= (uint8_t)((sm_pat_used(g) ? 0u : 1u) << c);
                o->und |= (uint8_t)((sm_glo_key == r * 4u + c ? 1u : 0u) << c);
                o->box |= (uint8_t)((g == sm_want || g == smd_next_g() ? 1u : 0u) << c);
            }
        } else if (r == 2u) {
            smd_bank_row(o, sm_glo_bank);
        } else {
            uint32_t on = r == 3u ? smp.mute : sm_solo;
            o->cols = 5;
            smd_cell(o, 0, r == 3u ? "M" : "S", 0);
            for (c = 0; c < 4u; c++)
                smd_cell(o, c + 1u, TN[c], (on >> c) & 1u);
        }
    } else if (sm_layer == SML_SEQ) {
        uint32_t play = sm_gid_of(smp.cur), vb = (sm_lay_pick < SM_NGPAT ? sm_lay_pick : play % SM_NGPAT) / SM_BPAT;
        if (r < 2u) {                                     /* the grid: the bank of the pick (none: of the one playing), A01 .. A08 */
            for (c = 0; c < 4u; c++) {
                char a[4];
                i = vb * SM_BPAT + r * 4u + c;
                smd_gname(a, i, 0);
                smd_cell(o, c, a, i == play);
                o->dim |= (uint8_t)((sm_pat_used(i) ? 0u : 1u) << c);
                o->und |= (uint8_t)((i == sm_lay_pick ? 1u : 0u) << c);
                o->box |= (uint8_t)((i == sm_want || i == smd_next_g() ? 1u : 0u) << c);
            }
        } else if (r == 2u) {
            smd_bank_row(o, vb);
        } else if (r == 3u) {
        } else if (sm_lay_cleared) {
            smd_cat(o->w, &n, "CLEARED");
            o->winv = 1;
        } else if (sm_lay_nch) {                          /* the chain being built: + A01 A01 B02 (its last four links) */
            char l[4];
            smd_cat(o->w, &n, "+");
            for (i = sm_lay_nch > 4u ? sm_lay_nch - 4u : 0u; i < sm_lay_nch; i++) {
                smd_gname(l, sm_lay_chain[i], 0);
                smd_cat(o->w, &n, " ");
                smd_cat(o->w, &n, l);
            }
        } else if (smp.nchain >= 2u) {                    /* the chain playing: CHAIN 2/3 NEXT A02 */
            uint32_t nc = smp.nchain <= SM_NCHAIN ? smp.nchain : SM_NCHAIN, cp = sm_cpos % nc;
            char l[4];
            smd_cat(o->w, &n, "CHAIN ");
            smd_lenstr(l, cp + 1u);
            smd_cat(o->w, &n, l);
            smd_cat(o->w, &n, "/");
            smd_lenstr(l, nc);
            smd_cat(o->w, &n, l);
            smd_cat(o->w, &n, " NEXT ");
            smd_gname(l, smp.chain[(cp + 1u) % nc], 0);
            smd_cat(o->w, &n, l);
        } else {                                          /* the change mode, the master length */
            static const char *const M[SM_PM_COUNT] = {"SEQUENTIAL", "DIR START", "DIR JUMP"};
            uint32_t ml = smp.pat[smp.cur % SM_NPAT].mlen;
            char l[4];
            smd_cat(o->w, &n, M[smp.mode % SM_PM_COUNT]);
            smd_cat(o->w, &n, " ML ");
            smd_lenstr(l, ml);
            smd_cat(o->w, &n, ml ? l : "AUTO");
        }
    }
}
static uint32_t smd_map_sig(uint32_t r)
{
    smd_mrow_t o;
    uint32_t h = smd_mix(2166136261u, 0x4D4150u + sm_layer), i;
    smd_map_get(r, &o);
    for (i = 0; o.w[i]; i++)
        h = smd_mix(h, (uint8_t)o.w[i]);
    for (i = 0; i < 30u; i++)
        h = smd_mix(h, (uint8_t)o.c[i / 6u][i % 6u]);
    h = smd_mix(smd_mix(smd_mix(smd_mix(h, o.dim), o.und), o.box), o.cols);
    return smd_mix(smd_mix(smd_mix(h, o.inv), o.cells), o.winv);
}
static void smd_map_row(uint32_t r)
{
    smd_mrow_t o;
    uint32_t c, cw;
    smd_map_get(r, &o);
    cw = o.cols == 5u ? 48u : 60u;
    smd_begin(0, SMD_MAP_Y + r * SMD_MAP_ROW_H, 240u, SMD_MAP_ROW_H, T_BG);
    if (o.w[0])
        sm_px_text(4, 10, 2u, o.w, T_TEXT, o.winv);
    for (c = 0; c < o.cells; c++) {
        int32_t x = (int32_t)(c * cw) + ((int32_t)cw - 12 * smd_len(o.c[c])) / 2, cx = (int32_t)(c * cw);
        uint32_t inv = (o.inv >> c) & 1u;
        if (!o.c[c][0])
            continue;
        sm_px_text(x, 10, 2u, o.c[c], (uint16_t)(((o.dim >> c) & 1u) && !inv ? SM_DIM_INK : T_TEXT), (int)inv);
        if ((o.und >> c) & 1u)                            /* picked: a bar under the label */
            cv_rect(x, 28, 12 * smd_len(o.c[c]), 2, T_TEXT);
        if ((o.box >> c) & 1u) {                          /* asked for: a frame round the cell */
            cv_rect(cx + 3, 3, (int32_t)cw - 6, 1, T_TEXT);
            cv_rect(cx + 3, 30, (int32_t)cw - 6, 1, T_TEXT);
            cv_rect(cx + 3, 3, 1, 28, T_TEXT);
            cv_rect(cx + (int32_t)cw - 4, 3, 1, 28, T_TEXT);
        }
    }
    smd_end();
    smd_mapped = 1;
}
/* the map closed: what the knobs and SELECT do not cover again of the rows (y 160 .. 167 and 188 .. 217) cleared */
static void smd_unmap(void)
{
    smd_begin(0, SMD_KNOB_Y + SMD_KNOB_H, 240u, SMD_SEL_Y - SMD_KNOB_Y - SMD_KNOB_H, T_BG);
    smd_end();
    smd_begin(0, SMD_SEL_Y + SMD_SEL_H, 240u, SMD_MAP_Y + SMD_MAP_ROWS * SMD_MAP_ROW_H - SMD_SEL_Y - SMD_SEL_H, T_BG);
    smd_end();
    smd_mapped = 0;
}

/* -------------------------------------------------------------- steps --- */
/* the strip is dots only (the steps' story is on the panel LEDs): 16 places of 15 px, 20 px high. A trig is a filled dot
 * 3 x 3, a ratchet x2 / x3 / x4 that many dots stacked (pitch 4), a REST a hollow dot 4 x 4, an empty step nothing (a dim
 * 1 x 1 on the strong beats 1 / 5 / 9 / 13 to count by). The step playing: a bar 8 x 1 under its place (row 19); the one
 * held: a bar 8 x 2 over it (rows 0, 1). The stack grows up from the bottom dot (rows 15..17); a x4 stack takes rows 3..17,
 * so a 1 px gap is left to both bars (rows 2 and 18) */
static int32_t smd_playhead(void)                         /* the step playing on the selected track, -1: none */
{
    return smd_play ? (int32_t)smd_pos : -1;
}
static uint32_t smd_mark(void)                            /* the bar over a step: the one held, on STEP the one edited */
{
    int32_t st = sm_page_is_step() ? sm_step_edit() : -1;
    return sm_ui.held_step < SM_NSTEP ? sm_ui.held_step : st >= 0 ? (uint32_t)st : 0xFFu;
}
static uint32_t smd_dots(const sm_step_t *st)             /* 0 empty, 1..4 a trig with that many dots, 5 a REST */
{
    return (st->flags & SMF_REST) ? 5u : st->n ? 1u + SMF_RAT(st->flags) : 0u;
}
static uint32_t smd_strip_len(void) { return (uint32_t)clamp(smd_snd()->len, 1, SM_NSTEP); }
/* HOME > MIXER: the strip is the four tracks, a row of dots each (rows 0..2, 5..7, 10..12, 15..17): a trig a 3 x 3 dot (a
 * REST hollow, the stack of a ratchet left out: no room), an empty strong beat a dim 1 x 1, nothing past the track's
 * length; the step playing a bar 5 x 1 under its place (rows 3, 8, 13, 18); a track not heard (muted, another soloed) dim;
 * the selected track a tick at the right of its row. The bank in view (OCT) for all four */
static uint32_t smd_mix4_dot(const sm_step_t *st) { return (st->flags & SMF_REST) ? 2u : st->n ? 1u : 0u; }
static uint32_t smd_strip4_sig(void)
{
    const sm_pattern_t *pt = &smp.pat[smp.cur % SM_NPAT];
    uint32_t b = sm_ui.bank ? 16u : 0u, t, i, h = smd_mix(2166136261u, 0x4D4958u + b);
    for (t = 0; t < SM_NTRK; t++) {
        uint32_t len = (uint32_t)clamp(pt->kit.snd[t].len, 1, SM_NSTEP);
        for (i = 0; i < 16u; i++)
            h = smd_mix(h, b + i < len ? smd_mix4_dot(&pt->step[t][b + i]) : 9u);
        h = smd_mix(smd_mix(h, len), smd_play && smd_pos4[t] >= b && smd_pos4[t] < b + 16u ? smd_pos4[t] : 0xFFu);
    }
    return smd_mix(smd_mix(h, sm_mute_eff()), sm_ui.trk);
}
static void smd_strip4(void)
{
    const sm_pattern_t *pt = &smp.pat[smp.cur % SM_NPAT];
    uint32_t b = sm_ui.bank ? 16u : 0u, t, i, mute = sm_mute_eff();
    smd_begin(0, SMD_STRIP_Y, 240u, SMD_STRIP_H, SM_WHITE_DIM);
    for (t = 0; t < SM_NTRK; t++) {
        uint32_t len = (uint32_t)clamp(pt->kit.snd[t].len, 1, SM_NSTEP);
        int32_t y = (int32_t)t * 5;
        uint16_t ink = (uint16_t)(((mute >> t) & 1u) ? SM_DIM_INK : T_BG);
        for (i = 0; i < 16u && b + i < len; i++) {
            int32_t x = (int32_t)(i * SMD_STEP_W) + (int32_t)(SMD_STEP_W / 2u);
            uint32_t d = smd_mix4_dot(&pt->step[t][b + i]);
            if (d == 1u) {
                cv_rect(x - 1, y, 3, 3, ink);
            } else if (d == 2u) {                         /* REST: a hollow dot (the corners and the edges, no centre) */
                cv_rect(x - 1, y, 3, 3, ink);
                cv_rect(x, y + 1, 1, 1, T_TEXT);
            } else if (i % 4u == 0) {
                cv_rect(x, y + 1, 1, 1, (uint16_t)SM_DIM_INK);
            }
            if (smd_play && smd_pos4[t] == b + i)
                cv_rect(x - 2, y + 3, 5, 1, ink);
        }
        if (t == sm_ui.trk % SM_NTRK)                     /* the selected track: a tick at the right */
            cv_rect(226, y, 10, 3, T_BG);
    }
    smd_end();
}
static uint32_t smd_strip1_sig(void)                     /* the selected track's strip */
{
    const sm_pattern_t *pt = &smp.pat[smp.cur % SM_NPAT];
    uint32_t t = sm_ui.trk % SM_NTRK, b = sm_ui.bank ? 16u : 0u, i, h = 2166136261u;
    int32_t ph = smd_playhead();
    for (i = 0; i < 16u; i++)
        h = smd_mix(h, b + i < smd_strip_len() ? smd_dots(&pt->step[t][b + i]) : 9u);
    h = smd_mix(h, smd_strip_len());
    h = smd_mix(h, ph >= (int32_t)b && ph < (int32_t)b + 16 ? (uint32_t)ph : 0xFFu);
    h = smd_mix(h, smd_mark() >= b && smd_mark() < b + 16u ? smd_mark() : 0xFFu);
    return smd_mix(h, t * 2u + (b != 0));
}
static uint32_t smd_strip_sig(void) { return sm_page_home() == SMH_MIXER ? smd_strip4_sig() : smd_strip1_sig(); }
static void smd_strip1(void)
{
    const sm_pattern_t *pt = &smp.pat[smp.cur % SM_NPAT];
    uint32_t t = sm_ui.trk % SM_NTRK, b = sm_ui.bank ? 16u : 0u, i, j;
    int32_t ph = smd_playhead();
    uint32_t len = smd_strip_len();
    char l[4];
    smd_begin(0, SMD_STRIP_Y, 240u, SMD_STRIP_H, SM_WHITE_DIM);
    for (i = 0; i < 16u; i++) {
        int32_t x = (int32_t)(i * SMD_STEP_W) + (int32_t)(SMD_STEP_W / 2u);   /* (x: the centre of the place) */
        uint32_t s = b + i, d = smd_dots(&pt->step[t][s]);
        if (s >= len)                                     /* past the track's length: nothing */
            continue;
        if (d == 0) {
            if (i % 4u == 0)
                cv_rect(x, 16, 1, 1, (uint16_t)SM_DIM_INK);
        } else if (d == 5u) {                             /* REST: a hollow dot */
            cv_rect(x - 2, 14, 4, 4, T_BG);
            cv_rect(x - 1, 15, 2, 2, T_TEXT);
        } else {
            for (j = 0; j < d; j++)                       /* (the lowest dot: rows 15..17) */
                cv_rect(x - 1, 15 - (int32_t)j * 4, 3, 3, T_BG);
        }
        if (ph == (int32_t)s)
            cv_rect(x - 4, 19, 8, 1, T_BG);
        if (smd_mark() == s)
            cv_rect(x - 4, 0, 8, 2, T_BG);
    }
    if (len > 16u) {                                      /* two pages of steps: the page number, large (P2 inverted) */
        l[0] = 'P';
        l[1] = (char)('1' + (b ? 1 : 0));
        l[2] = 0;
        sm_px_text(238 - 24, 3, 2u, l, T_BG, b ? 1 : 0);
    } else {
        smd_lenstr(l, len);                               /* the length, right of the steps */
        sm_px_text(238 - 12 * smd_len(l), 3, 2u, l, T_BG, 0);
    }
    smd_end();
}
static void smd_strip(void)
{
    if (sm_page_home() == SMH_MIXER)
        smd_strip4();
    else
        smd_strip1();
}

/* ------------------------------------------------------------- frame --- */
/* one frame of the main loop: the zones whose signature changed since they were drawn. Runs when the model or keys
 * changed (sm_dirty) or the playing step or the transport moved (the sequencer, in the audio stream, sets no flag) */
static void sm_draw(void)
{
    uint32_t sig[SMD_NZONE], z, pk;
    sm_draw_px = 0;
    smd_play = *(volatile uint8_t *)&sm_playing != 0;     /* the snapshot of the frame (the audio stream moves both) */
    smd_pos = *(volatile uint8_t *)&sm_pos[sm_ui.trk % SM_NTRK];
    smd_rec = *(volatile uint8_t *)&sm_rec != 0;
    smd_live = *(volatile uint8_t *)&sm_live != 0;
    for (z = 0; z < SM_NTRK; z++)                         /* (the selected track's: the one read above, the same in both) */
        smd_pos4[z] = z == sm_ui.trk % SM_NTRK ? smd_pos : *(volatile uint8_t *)&sm_pos[z];
    for (z = 0, pk = 0; z < SM_NTRK; z++)
        pk = pk << 8 | smd_pos4[z];
    if (sm_page_home() != SMH_MIXER)
        pk = sm_last_pos4;                                /* (the other tracks' steps are drawn on MIXER only) */
    if (!sm_dirty && smd_pos == sm_last_pos && smd_play == sm_last_play && pk == sm_last_pos4)
        return;
    sm_dirty = 0;
    sm_last_pos4 = pk;
    sm_last_pos = smd_pos;
    sm_last_play = smd_play;
    sig[0] = smd_hdr_sig();
    sig[1] = smd_page_sig();
    for (z = 0; z < 4u; z++)
        sig[SMD_Z_KNOB0 + z] = sm_map_open() ? smd_map_sig(z) : smd_knob_sig(z);
    sig[SMD_Z_SEL] = sm_map_open() ? smd_map_sig(4u) : smd_sel_sig();      /* (a layer open: the five rows of its map) */
    sig[SMD_Z_STRIP] = smd_strip_sig();
    for (z = 0; z < SMD_NZONE; z++) {
        if ((sm_zvalid & (1u << z)) && sm_zsig[z] == sig[z])
            continue;
        if (z == 0)
            smd_header();
        else if (z == 1)
            smd_page();
        else if (z <= SMD_Z_SEL && sm_map_open())
            smd_map_row(z - SMD_Z_KNOB0);
        else if (z < SMD_Z_SEL)
            smd_knob(z - SMD_Z_KNOB0);
        else if (z == SMD_Z_SEL)
            smd_select();
        else
            smd_strip();
        sm_zsig[z] = sig[z];
        sm_zvalid |= (uint8_t)(1u << z);
    }
    if (!sm_map_open() && smd_mapped)
        smd_unmap();
}
