/* SPDX-License-Identifier: GPL-3.0-only */
/* SWAMP MACHINE screen, Elektron mk1 look: one ink (white) on black, a 5 x 7 pixel font at scale 2, no smoothing, no grain;
 * a selection is an inversion. 240 x 240, every band its own canvas:
 *   y   0..19   header: T1 SAW 03 (track, machine, preset) left, the track inverted while it is muted; A01 120.0 > (pattern,
 *               tempo, transport) right; while recording REC (inverted) at the right corner in place of the pattern;
 *               the lock pool full: LOCKS FULL (inverted) alone for a second
 *   y  20..47   the page and the preset: EDIT 1 . SYNC LEAD (a layer open: its name instead); STEP: STEP 05 . X3, the step
 *               edited and its FLAG (none: STEP -)
 *   y  48..159  the four knobs, a column of 60 each: the label, a ring of 16 dots, the value. A ring of a value locked on the
 *               held step is a filled disc; a dot in its centre: locked on another step
 *   y 168..187  SELECT: its label and value
 *   (STEP: the knobs NOTE VEL LEN COND, SELECT NUDGE, of the step edited; an empty one or none: "-", the rings empty)
 *   y  48..217  (a layer open: its key map over the knobs and SELECT, five rows of 34, the labels of the keys at scale 2;
 *               SEQ: the title LEN 32 large (scale 3), the grid A01 .. A16 in four rows (the one playing inverted, the one
 *               picked underlined, the one asked for framed, an empty one dim), the change mode and the master length)
 *   y 220..239  the 16 steps of the bank as dots only, 13 px each (a trig a dot, a ratchet stacked dots, a REST a hollow dot;
 *               a bar under the one playing, over the one held; nothing past the track's length), and right of them the
 *               track's length; the panel LEDs carry the rest of the steps (sm_leds.c)
 * The LCD transfer is the dear part: a zone is redrawn only when what it shows changed (a signature of its inputs). */
#define SMD_HDR_Y 0u
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
#define SMD_RING_R 28
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
static uint8_t smd_pos, smd_play, smd_rec;                /* (and REC, which the REC key sets from the main loop) */

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
static void smd_begin(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    smd_x = x;
    smd_y = y;
    cv_begin(w, h, T_BG);
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
    h = smd_mix(h, smd_muted()); h = smd_mix(h, sm_ui.trk); h = smd_mix(h, s->machine); h = smd_mix(h, s->preset); h = smd_mix(h, smp.cur);
    h = smd_mix(h, smp.bpm10); h = smd_mix(h, smd_play); h = smd_mix(h, sm_lock_msg);
    return smd_mix(h, smd_rec);
}
static void smd_two(char *b, uint32_t v)
{
    v %= 100u;
    b[0] = (char)('0' + v / 10u);
    b[1] = (char)('0' + v % 10u);
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
static int smd_pat_empty(uint32_t p)                      /* pattern p holds no trig on any track */
{
    const sm_pattern_t *pt = &smp.pat[p % SM_NPAT];
    uint32_t t, s;
    for (t = 0; t < SM_NTRK; t++)
        for (s = 0; s < SM_NSTEP; s++)
            if (pt->step[t][s].n)
                return 0;
    return 1;
}
static void smd_header(void)
{
    char l[8], a[4], t[12], nm[6];
    uint32_t rec = smd_rec;
    int32_t x;
    const sm_sound_t *s = smd_snd();
    smd_begin(0, SMD_HDR_Y, 240u, SMD_HDR_H);
    if (sm_lock_msg) {                                    /* the pool full: LOCKS FULL, inverted, the whole header for 1 s */
        sm_px_text((240 - 12 * 10) / 2, 2, 2u, "LOCKS FULL", T_TEXT, 1);
        smd_end();
        return;
    }
    /* left: T1 SAW 03 - track, machine (four letters), preset; pieces a half pitch (6 px) apart */
    l[0] = 'T'; l[1] = (char)('1' + sm_ui.trk % SM_NTRK); l[2] = 0;
    x = sm_px_text(2, 2, 2u, l, T_TEXT, (int)smd_muted()) + 2 + 6;   /* (a muted track: inverted) */
    smd_upper(nm, ENGINES[sm_engine_of(s->machine)]->name, 4u);
    x += sm_px_text(x, 2, 2u, nm, T_TEXT, 0) + 6;
    smd_two(l, (uint32_t)s->preset + 1u);
    l[2] = 0;
    sm_px_text(x, 2, 2u, l, T_TEXT, 0);
    /* right, from the right edge: A01 120.0 >   (while recording: 120.0 > REC, the pattern gives way) */
    fmt_fix(t, smp.bpm10, 1);
    a[0] = 'A'; smd_two(a + 1, (uint32_t)smp.cur % SM_NPAT + 1u); a[3] = 0;
    x = 240 - (rec ? 12 * 3 + 6 : 0) - 12;
    sm_px_text(x, 2, 2u, smd_play ? "\x10" : "\x11", T_TEXT, 0);
    x -= 6 + 12 * smd_len(t);
    sm_px_text(x, 2, 2u, t, T_TEXT, 0);
    if (rec)
        sm_px_text(240 - 36, 2, 2u, "REC", T_TEXT, 1);
    else
        sm_px_text(x - 6 - 36, 2, 2u, a, T_TEXT, 0);
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
    if (sm_layer)                                         /* (the title of the layer: its page of machines, the track's length) */
        h = smd_mix(smd_mix(smd_mix(smd_mix(h, 0x100u + sm_layer), sm_lay_epage), s->len), sm_ui.trk);
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
    smd_begin(0, SMD_PAGE_Y, 240u, SMD_PAGE_H);
    if (sm_layer) {                                       /* a layer open: its name in place of the page and the preset */
        n = 0;
        if (sm_layer == SML_EDIT) {
            smd_cat(b, &n, "MACHINE ");
            smd_cat(b, &n, MF_PAGE_NAME[sm_lay_epage % SM_NEPAGE]);
        } else if (sm_layer == SML_GLO) {
            smd_cat(b, &n, "MUTE SOLO");
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
        if (!p) {
            smd_cat(b, &n, "-");
        } else {
            smd_two(b + n, (uint32_t)st + 1u);
            n += 2;
            b[n++] = ' '; b[n++] = '\x12'; b[n++] = ' ';
            b[n] = 0;
            smd_cat(b, &n, p->n ? SM_FLAGN[sm_flag_place(p->flags)] : "-");
        }
    } else if (sm_page_home()) {                        /* MIXER, CLOCK: the page alone (the sound is not what the knobs edit) */
        smd_upper(b, sm_cur_page()->name, 8u);
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
/* the STEP page's knob k: its label, the step's field (empty or none: "-", the ring at 0) */
static void smd_step_knob(uint32_t k, smd_knob_t *o)
{
    static const char *const LAB[4] = {"NOTE", "VEL", "LEN", "COND"};
    int32_t st;
    const sm_step_t *p = smd_step(&st);
    uint32_t i;
    o->on = 1;
    for (i = 0; LAB[k][i]; i++)
        o->lab[i] = LAB[k][i];
    o->lab[i] = 0;
    o->val[0] = '-';
    o->val[1] = 0;
    if (!p || !p->n)
        return;
    switch (k) {
    case 0: smd_note(o->val, p->note[0] & 127u); o->pos = (uint32_t)(p->note[0] & 127u) * 256u / 127u; break;
    case 1: smd_int(o->val, p->vel, 0); o->pos = (uint32_t)(p->vel > 1u ? p->vel - 1u : 0u) * 256u / 126u; break;
    case 2: i = p->len < SM_NLEN ? p->len : 0u; smd_upper(o->val, SM_LEN_N[i], 5u); o->pos = i * 256u / (SM_NLEN - 1u); break;
    default: sm_cond_name(p->cond, o->val); o->pos = sm_cond_place(p->cond) * 256u / (SM_COND_PLACES - 1u); break;
    }
    o->v = (int32_t)o->pos;
}
/* the HOME pages' knob k: MIXER the LEVEL of T(k+1) (label T1..T4); CLOCK K1 BPM "120.0" (30.0 .. 300.0), K2 SWING "57%" */
static void smd_home_knob(uint32_t k, smd_knob_t *o)
{
    uint32_t i;
    o->on = 1;
    if (sm_page_home() == SMH_MIXER) {
        const sm_sound_t *s = &smp.pat[smp.cur % SM_NPAT].kit.snd[k % SM_NTRK];
        const param_desc_t *d = sm_desc_safe(sm_engine_of(s->machine), P_LEVEL);
        const char *unit;
        o->lab[0] = 'T'; o->lab[1] = (char)('1' + k % SM_NTRK); o->lab[2] = 0;
        o->on = d != 0;
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
    o->d = sm_desc_safe(sm_engine_of(s->machine), id);
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
    smd_upper(o->lab, o->d->label, 5u);
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
    for (i = 0; o.val[i]; i++)                            /* (the STEP page: the text is the value) */
        h = smd_mix(h, (uint8_t)o.val[i]);
    return h;
}
static void smd_knob(uint32_t k)
{
    smd_knob_t o;
    smd_knob_get(k, &o);
    smd_begin(k * SMD_KNOB_W, SMD_KNOB_Y, SMD_KNOB_W, SMD_KNOB_H);
    if (o.on) {
        sm_px_text((60 - 12 * smd_len(o.lab)) / 2, 4, 2u, o.lab, T_TEXT, 0);
        sm_px_ring(SMD_RING_CX, SMD_RING_CY, SMD_RING_R, o.pos, o.filled, o.dot);
        sm_px_text((60 - 12 * smd_len(o.val)) / 2, 98, 2u, o.val, T_TEXT, 0);
    }
    smd_end();
}

/* ------------------------------------------------------------ SELECT --- */
static int32_t smd_sel_val(uint32_t id)                  /* SELECT's value: the held step's lock of it, else the base */
{
    const sm_pattern_t *pt = &smp.pat[smp.cur % SM_NPAT];
    int32_t li = sm_ui.held_step < SM_NSTEP ? sm_lock_find(pt, sm_ui.trk % SM_NTRK, sm_ui.held_step, id) : -1;
    return li >= 0 ? pt->lock[li].val : smd_snd()->p[id];
}
static void smd_step_nudge(char *b)                       /* the STEP page's SELECT: the step's NUDGE, "-" none */
{
    int32_t st;
    const sm_step_t *p = smd_step(&st);
    if (p && p->n)
        smd_int(b, p->nudge, 1);
    else {
        b[0] = '-';
        b[1] = 0;
    }
}
static uint32_t smd_sel_sig(void)
{
    const sm_sound_t *s = smd_snd();
    uint32_t id = sm_cur_page()->sel;
    uint32_t h = smd_mix(smd_mix(2166136261u, sm_ui.page), s->machine);
    if (sm_page_is_step()) {
        char b[8];
        uint32_t i;
        smd_step_nudge(b);
        for (i = 0; b[i]; i++)
            h = smd_mix(h, (uint8_t)b[i]);
        return h;
    }
    if (sm_page_home())                                   /* (the tempo) */
        return smd_mix(h, smp.bpm10);
    return id == SM_NOPARAM ? h : smd_mix(smd_mix(h, id), (uint32_t)smd_sel_val(id));
}
static void smd_select(void)
{
    const sm_sound_t *s = smd_snd();
    uint32_t id = sm_cur_page()->sel;
    const param_desc_t *d = id == SM_NOPARAM ? 0 : sm_desc_safe(sm_engine_of(s->machine), id);
    smd_begin(0, SMD_SEL_Y, 240u, SMD_SEL_H);
    if (sm_page_is_step()) {
        char val[8];
        int32_t x;
        smd_step_nudge(val);
        x = sm_px_text(4, 2, 2u, "NUDGE", T_TEXT, 0) + 4 + 12;
        sm_px_text(x, 2, 2u, val, T_TEXT, 1);
    } else if (sm_page_home()) {                          /* HOME: SELECT is the tempo */
        char val[8];
        int32_t x = sm_px_text(4, 2, 2u, "TEMPO", T_TEXT, 0) + 4 + 12;
        fmt_fix(val, smp.bpm10, 1);
        sm_px_text(x, 2, 2u, val, T_TEXT, 1);
    } else if (d) {
        char lab[8], val[12];
        const char *unit;
        int32_t x;
        smd_upper(lab, d->label, 6u);
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
typedef struct { char w[22]; char c[4][6]; uint8_t inv, cells, winv, dim, und, box; } smd_mrow_t;   /* (bits per cell) */
static void smd_cell(smd_mrow_t *o, uint32_t i, const char *s, uint32_t inv)
{
    smd_upper(o->c[i], s, 4u);
    o->inv |= (uint8_t)((inv ? 1u : 0u) << i);
    o->cells = (uint8_t)(i + 1u > o->cells ? i + 1u : o->cells);
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
    } else if (sm_layer == SML_GLO) {
        if (r == 0u || r == 3u) {
            smd_cat(o->w, &n, r ? "SOLO" : "MUTE");
        } else if (r == 1u || r == 4u) {
            uint32_t on = r == 1u ? smp.mute : sm_solo;
            for (c = 0; c < 4u; c++)
                smd_cell(o, c, TN[c], (on >> c) & 1u);
            o->cells = 4;
        }
    } else if (sm_layer == SML_SEQ) {
        if (r < 4u) {                                     /* the grid: A01 .. A16, four a row */
            uint32_t play = smp.cur % SM_NPAT;
            for (c = 0; c < 4u; c++) {
                char a[4];
                i = r * 4u + c;
                a[0] = 'A';
                smd_two(a + 1, i + 1u);
                a[3] = 0;
                smd_cell(o, c, a, i == play);
                o->dim |= (uint8_t)((smd_pat_empty(i) ? 1u : 0u) << c);
                o->und |= (uint8_t)((i == sm_lay_pick ? 1u : 0u) << c);
                o->box |= (uint8_t)((i == smp.next ? 1u : 0u) << c);
            }
        } else if (sm_lay_cleared) {
            smd_cat(o->w, &n, "CLEARED");
            o->winv = 1;
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
    for (i = 0; i < 24u; i++)
        h = smd_mix(h, (uint8_t)o.c[i / 6u][i % 6u]);
    h = smd_mix(smd_mix(smd_mix(h, o.dim), o.und), o.box);
    return smd_mix(smd_mix(smd_mix(h, o.inv), o.cells), o.winv);
}
static void smd_map_row(uint32_t r)
{
    smd_mrow_t o;
    uint32_t c;
    smd_map_get(r, &o);
    smd_begin(0, SMD_MAP_Y + r * SMD_MAP_ROW_H, 240u, SMD_MAP_ROW_H);
    if (o.w[0])
        sm_px_text(4, 10, 2u, o.w, T_TEXT, o.winv);
    for (c = 0; c < o.cells; c++) {
        int32_t x = (int32_t)(c * 60u) + (60 - 12 * smd_len(o.c[c])) / 2, cx = (int32_t)(c * 60u);
        uint32_t inv = (o.inv >> c) & 1u;
        if (!o.c[c][0])
            continue;
        sm_px_text(x, 10, 2u, o.c[c], (uint16_t)(((o.dim >> c) & 1u) && !inv ? SM_DIM_INK : T_TEXT), (int)inv);
        if ((o.und >> c) & 1u)                            /* picked: a bar under the label */
            cv_rect(x, 28, 12 * smd_len(o.c[c]), 2, T_TEXT);
        if ((o.box >> c) & 1u) {                          /* asked for: a frame round the cell */
            cv_rect(cx + 3, 3, 54, 1, T_TEXT);
            cv_rect(cx + 3, 30, 54, 1, T_TEXT);
            cv_rect(cx + 3, 3, 1, 28, T_TEXT);
            cv_rect(cx + 56, 3, 1, 28, T_TEXT);
        }
    }
    smd_end();
    smd_mapped = 1;
}
/* the map closed: what the knobs and SELECT do not cover again of the rows (y 160 .. 167 and 188 .. 217) cleared */
static void smd_unmap(void)
{
    smd_begin(0, SMD_KNOB_Y + SMD_KNOB_H, 240u, SMD_SEL_Y - SMD_KNOB_Y - SMD_KNOB_H);
    smd_end();
    smd_begin(0, SMD_SEL_Y + SMD_SEL_H, 240u, SMD_MAP_Y + SMD_MAP_ROWS * SMD_MAP_ROW_H - SMD_SEL_Y - SMD_SEL_H);
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
static uint32_t smd_strip_sig(void)
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
static void smd_strip(void)
{
    const sm_pattern_t *pt = &smp.pat[smp.cur % SM_NPAT];
    uint32_t t = sm_ui.trk % SM_NTRK, b = sm_ui.bank ? 16u : 0u, i, j;
    int32_t ph = smd_playhead();
    uint32_t len = smd_strip_len();
    char l[4];
    smd_begin(0, SMD_STRIP_Y, 240u, SMD_STRIP_H);
    for (i = 0; i < 16u; i++) {
        int32_t x = (int32_t)(i * SMD_STEP_W) + (int32_t)(SMD_STEP_W / 2u);   /* (x: the centre of the place) */
        uint32_t s = b + i, d = smd_dots(&pt->step[t][s]);
        if (s >= len)                                     /* past the track's length: nothing */
            continue;
        if (d == 0) {
            if (i % 4u == 0)
                cv_rect(x, 16, 1, 1, (uint16_t)SM_DIM_INK);
        } else if (d == 5u) {                             /* REST: a hollow dot */
            cv_rect(x - 2, 14, 4, 4, T_TEXT);
            cv_rect(x - 1, 15, 2, 2, T_BG);
        } else {
            for (j = 0; j < d; j++)                       /* (the lowest dot: rows 15..17) */
                cv_rect(x - 1, 15 - (int32_t)j * 4, 3, 3, T_TEXT);
        }
        if (ph == (int32_t)s)
            cv_rect(x - 4, 19, 8, 1, T_TEXT);
        if (smd_mark() == s)
            cv_rect(x - 4, 0, 8, 2, T_TEXT);
    }
    smd_lenstr(l, len);                                   /* the length, right of the steps */
    sm_px_text(238 - 12 * smd_len(l), 3, 2u, l, T_TEXT, 0);
    smd_end();
}

/* ------------------------------------------------------------- frame --- */
/* one frame of the main loop: the zones whose signature changed since they were drawn. Runs when the model or keys
 * changed (sm_dirty) or the playing step or the transport moved (the sequencer, in the audio stream, sets no flag) */
static void sm_draw(void)
{
    uint32_t sig[SMD_NZONE], z;
    sm_draw_px = 0;
    smd_play = *(volatile uint8_t *)&sm_playing != 0;     /* the snapshot of the frame (the audio stream moves both) */
    smd_pos = *(volatile uint8_t *)&sm_pos[sm_ui.trk % SM_NTRK];
    smd_rec = *(volatile uint8_t *)&sm_rec != 0;
    if (!sm_dirty && smd_pos == sm_last_pos && smd_play == sm_last_play)
        return;
    sm_dirty = 0;
    sm_last_pos = smd_pos;
    sm_last_play = smd_play;
    sig[0] = smd_hdr_sig();
    sig[1] = smd_page_sig();
    for (z = 0; z < 4u; z++)
        sig[SMD_Z_KNOB0 + z] = sm_layer ? smd_map_sig(z) : smd_knob_sig(z);
    sig[SMD_Z_SEL] = sm_layer ? smd_map_sig(4u) : smd_sel_sig();      /* (a layer open: the five rows of its map) */
    sig[SMD_Z_STRIP] = smd_strip_sig();
    for (z = 0; z < SMD_NZONE; z++) {
        if ((sm_zvalid & (1u << z)) && sm_zsig[z] == sig[z])
            continue;
        if (z == 0)
            smd_header();
        else if (z == 1)
            smd_page();
        else if (z <= SMD_Z_SEL && sm_layer)
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
    if (!sm_layer && smd_mapped)
        smd_unmap();
}
