/* SPDX-License-Identifier: GPL-3.0-only */
/* SWAMP MACHINE screen, Elektron mk1 look: one ink (white) on black, a 5 x 7 pixel font at scale 2, no smoothing, no grain;
 * a selection is an inversion. 240 x 240, every band its own canvas:
 *   y   0..19   header: T1 SAW 03 (track, machine, preset) left; A01 120.0 > (pattern, tempo, transport) right; while
 *               recording REC (inverted) at the right corner in place of the pattern
 *   y  20..47   the page and the preset: EDIT 1 . SYNC LEAD
 *   y  48..159  the four knobs, a column of 60 each: the label, a ring of 16 dots, the value. A ring of a value locked on the
 *               held step is a filled disc; a dot in its centre: locked on another step
 *   y 160..199  SELECT: its label and value
 *   y 200..239  the 16 steps of the bank: 14 x 30 cells (a trig solid, empty a 1 px frame, the one playing inverted, the one
 *               held a 3 px frame), under them the bank bar
 * The LCD transfer is the dear part: a zone is redrawn only when what it shows changed (a signature of its inputs). */
#define SMD_HDR_Y 0u
#define SMD_HDR_H 20u
#define SMD_PAGE_Y 20u
#define SMD_PAGE_H 28u
#define SMD_KNOB_Y 48u
#define SMD_KNOB_W 60u
#define SMD_KNOB_H 112u
#define SMD_SEL_Y 160u
#define SMD_SEL_H 40u
#define SMD_STRIP_Y 200u
#define SMD_STRIP_H 40u
#define SMD_STEP_W 15u
#define SMD_NZONE 8u                                      /* header, page, four knobs, SELECT, strip */
#define SMD_Z_KNOB0 2u
#define SMD_Z_SEL 6u
#define SMD_Z_STRIP 7u

static uint32_t sm_draw_px;                               /* the pixels the last frame sent (the test's budget) */
static volatile uint8_t sm_rec;                           /* recording (the REC key, stage 2 task 4): REC in the header */
static uint32_t sm_zsig[SMD_NZONE];                       /* what each zone shows now (its inputs' signature) */
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
static uint32_t smd_hdr_sig(void)
{
    const sm_sound_t *s = smd_snd();
    uint32_t h = 2166136261u;
    h = smd_mix(h, sm_ui.trk); h = smd_mix(h, s->machine); h = smd_mix(h, s->preset); h = smd_mix(h, smp.cur);
    h = smd_mix(h, smp.bpm10); h = smd_mix(h, smd_play);
    return smd_mix(h, smd_rec);
}
static void smd_two(char *b, uint32_t v)
{
    v %= 100u;
    b[0] = (char)('0' + v / 10u);
    b[1] = (char)('0' + v % 10u);
}
static void smd_header(void)
{
    char l[8], a[4], t[12], nm[6];
    uint32_t rec = smd_rec;
    int32_t x;
    const sm_sound_t *s = smd_snd();
    smd_begin(0, SMD_HDR_Y, 240u, SMD_HDR_H);
    /* left: T1 SAW 03 - track, machine (four letters), preset; pieces a half pitch (6 px) apart */
    l[0] = 'T'; l[1] = (char)('1' + sm_ui.trk % SM_NTRK); l[2] = 0;
    x = sm_px_text(0, 2, 2u, l, T_TEXT, 0) + 6;
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
static uint32_t smd_page_sig(void)
{
    const sm_sound_t *s = smd_snd();
    return smd_mix(smd_mix(smd_mix(2166136261u, sm_ui.page), s->machine), s->preset);
}
static void smd_page(void)
{
    char b[24];
    uint32_t n;
    smd_begin(0, SMD_PAGE_Y, 240u, SMD_PAGE_H);
    smd_upper(b, sm_cur_page()->name, 8u);
    for (n = 0; b[n]; n++) {}
    b[n++] = ' '; b[n++] = '\x12'; b[n++] = ' ';
    smd_upper(b + n, smd_preset_name(smd_snd()), 20u - n);     /* (the line holds twenty characters) */
    sm_px_text(0, 2, 2u, b, T_TEXT, 0);
    smd_end();
}

/* ------------------------------------------------------------- knobs --- */
typedef struct {
    const param_desc_t *d;                                /* 0: no parameter on this knob */
    char lab[8], val[12];
    uint32_t pos;                                         /* 0..256 on the ring */
    int filled, dot;                                      /* locked on the held step; locked elsewhere */
    int32_t v;                                            /* the value shown: the lock's on the held step */
} smd_knob_t;
static void smd_knob_get(uint32_t k, smd_knob_t *o)
{
    const sm_sound_t *s = smd_snd();
    const sm_pattern_t *pt = &smp.pat[smp.cur % SM_NPAT];
    uint32_t id = sm_cur_page()->id[k & 3u], t = sm_ui.trk % SM_NTRK;
    const char *unit;
    o->d = 0;
    o->lab[0] = o->val[0] = 0;
    o->pos = 0;
    o->filled = o->dot = 0;
    o->v = 0;
    if (id == SM_NOPARAM)
        return;
    o->d = sm_desc_safe(sm_engine_of(s->machine), id);
    if (!o->d)
        return;
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
    smd_knob_get(k, &o);
    h = smd_mix(h, sm_ui.page); h = smd_mix(h, smd_snd()->machine); h = smd_mix(h, o.d != 0);
    h = smd_mix(h, (uint32_t)o.v); h = smd_mix(h, (uint32_t)o.filled); h = smd_mix(h, (uint32_t)o.dot);
    return h;
}
static void smd_knob(uint32_t k)
{
    smd_knob_t o;
    smd_knob_get(k, &o);
    smd_begin(k * SMD_KNOB_W, SMD_KNOB_Y, SMD_KNOB_W, SMD_KNOB_H);
    if (o.d) {
        sm_px_text((60 - 12 * smd_len(o.lab)) / 2, 4, 2u, o.lab, T_TEXT, 0);
        sm_px_ring(30, 46, 22, o.pos, o.filled, o.dot);
        sm_px_text((60 - 12 * smd_len(o.val)) / 2, 76, 2u, o.val, T_TEXT, 0);
    }
    smd_end();
}

/* ------------------------------------------------------------ SELECT --- */
static uint32_t smd_sel_sig(void)
{
    const sm_sound_t *s = smd_snd();
    uint32_t id = sm_cur_page()->sel;
    uint32_t h = smd_mix(smd_mix(2166136261u, sm_ui.page), s->machine);
    return id == SM_NOPARAM ? h : smd_mix(smd_mix(h, id), (uint8_t)s->p[id]);
}
static void smd_select(void)
{
    const sm_sound_t *s = smd_snd();
    uint32_t id = sm_cur_page()->sel;
    const param_desc_t *d = id == SM_NOPARAM ? 0 : sm_desc_safe(sm_engine_of(s->machine), id);
    smd_begin(0, SMD_SEL_Y, 240u, SMD_SEL_H);
    if (d) {
        char lab[8], val[12];
        const char *unit;
        int32_t x;
        smd_upper(lab, d->label, 6u);
        param_format(d, clamp(s->p[id], d->min, d->max), val, &unit);
        x = sm_px_text(4, 2, 2u, lab, T_TEXT, 0) + 4 + 12;
        val[(240 - x) / 12 < 11 ? (240 - x) / 12 : 11] = 0;   /* (what fits the row) */
        sm_px_text(x, 2, 2u, val, T_TEXT, 1);
    }
    smd_end();
}

/* -------------------------------------------------------------- steps --- */
static int32_t smd_playhead(void)                         /* the step playing on the selected track, -1: none */
{
    return smd_play ? (int32_t)smd_pos : -1;
}
static uint32_t smd_strip_sig(void)
{
    const sm_pattern_t *pt = &smp.pat[smp.cur % SM_NPAT];
    uint32_t t = sm_ui.trk % SM_NTRK, b = sm_ui.bank ? 16u : 0u, i, h = 2166136261u;
    int32_t ph = smd_playhead();
    for (i = 0; i < 16u; i++)
        h = smd_mix(h, pt->step[t][b + i].n != 0);
    h = smd_mix(h, ph >= (int32_t)b && ph < (int32_t)b + 16 ? (uint32_t)ph : 0xFFu);
    h = smd_mix(h, sm_ui.held_step >= b && sm_ui.held_step < b + 16u ? sm_ui.held_step : 0xFFu);
    return smd_mix(h, t * 2u + (b != 0));
}
static void smd_strip(void)
{
    const sm_pattern_t *pt = &smp.pat[smp.cur % SM_NPAT];
    uint32_t t = sm_ui.trk % SM_NTRK, b = sm_ui.bank ? 16u : 0u, i;
    int32_t ph = smd_playhead();
    smd_begin(0, SMD_STRIP_Y, 240u, SMD_STRIP_H);
    for (i = 0; i < 16u; i++) {
        int32_t x = (int32_t)(i * SMD_STEP_W), y = 2;
        uint32_t s = b + i;
        int trig = pt->step[t][s].n != 0;
        if (sm_ui.held_step == s) {                       /* held: a 3 px frame, a gap, then the cell inside */
            cv_rect(x, y, 14, 30, T_TEXT);
            cv_rect(x + 3, y + 3, 8, 24, T_BG);
            if (trig)
                cv_rect(x + 4, y + 4, 6, 22, T_TEXT);
        } else if (ph == (int32_t)s) {                    /* playing: the cell inverted */
            if (trig) {
                cv_rect(x, y, 14, 30, T_TEXT);
                cv_rect(x + 1, y + 1, 12, 28, T_BG);
            } else {
                cv_rect(x, y, 14, 30, (uint16_t)SM_DIM_INK);
            }
        } else if (trig) {
            cv_rect(x, y, 14, 30, T_TEXT);
        } else {
            cv_rect(x, y, 14, 30, (uint16_t)SM_DIM_INK);
            cv_rect(x + 1, y + 1, 12, 28, T_BG);
        }
    }
    cv_rect(b ? 120 : 0, 36, 120, 2, T_TEXT);             /* the bank bar: the half of the strip's 32 steps shown */
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
        sig[SMD_Z_KNOB0 + z] = smd_knob_sig(z);
    sig[SMD_Z_SEL] = smd_sel_sig();
    sig[SMD_Z_STRIP] = smd_strip_sig();
    for (z = 0; z < SMD_NZONE; z++) {
        if ((sm_zvalid & (1u << z)) && sm_zsig[z] == sig[z])
            continue;
        if (z == 0)
            smd_header();
        else if (z == 1)
            smd_page();
        else if (z < SMD_Z_SEL)
            smd_knob(z - SMD_Z_KNOB0);
        else if (z == SMD_Z_SEL)
            smd_select();
        else
            smd_strip();
        sm_zsig[z] = sig[z];
        sm_zvalid |= (uint8_t)(1u << z);
    }
}
