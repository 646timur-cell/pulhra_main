/* SPDX-License-Identifier: GPL-3.0-only */
/* SWAMP MACHINE screen, theme PIZDAKS (sm_theme.c). 240 x 240:
 *   y   0..21  header: T1 SAW . A01 . 120.0 . play/stop . REC .. 1/2 (AF_S), a 2 px blood frame under it
 *   y  24..47  the page's name (AF_M, a +1/+1 shadow in DIM)
 *   y  48..175 the four cells 2 x 2 of 120 x 64: the label (AF_S, rust) and its unit, the value (AF_L, bone)
 *   y 178..197 SELECT: its label and value (AF_S)
 *   y 202..239 the 16 steps of the bank: 15 x 30 cells, a trig bone-filled, an empty one a DIM frame, the step playing
 *              a 2 px blood frame
 * The LCD transfer is the dear part: a zone is redrawn only when what it shows changed (a signature of its inputs),
 * each zone its own canvas band (cv_begin .. cv_blit) from the rows it owns (the gaps between them belong to the zone
 * above, so the grain of the ground covers the whole screen). The ink - header, values, steps - is drawn clean;
 * cv_blit's grime touches only pixels of the flat ground colours. Text is Latin: AF_S/AF_M/AF_L have no Cyrillic. */
#define SMD_HDR_Y 0u
#define SMD_HDR_H 24u                                     /* the header (20) + its 2 px frame + 2 rows of ground */
#define SMD_PAGE_Y 24u
#define SMD_PAGE_H 24u
#define SMD_CELL_Y 48u
#define SMD_CELL_W 120u
#define SMD_CELL_H 64u
#define SMD_SEL_Y 176u
#define SMD_SEL_H 26u                                     /* the row at 178..197 and the ground below it */
#define SMD_STRIP_Y 202u
#define SMD_STRIP_H 38u
#define SMD_STEP_W 15u
#define SMD_NZONE 6u                                      /* header, page, two rows of cells, SELECT, strip */

static uint32_t sm_draw_px;                               /* the pixels the last frame sent (the test's budget) */
static uint8_t sm_rec;                                    /* recording (stage 2's recorder sets it): REC in the header */
static uint32_t sm_zsig[SMD_NZONE];                       /* what each zone shows now (its inputs' signature) */
static uint8_t sm_zvalid;                                 /* the bit per zone: sm_zsig is on the screen */
static uint8_t sm_last_pos = 0xFFu, sm_last_play = 0xFFu; /* the playing step and transport seen by the last frame */
/* the playing step of the selected track and the transport, read ONCE per frame (the audio stream moves them at any time):
 * every signature and every zone of the frame uses these two, so what is drawn is what the signature said */
static uint8_t smd_pos, smd_play;

/* the screen was painted over (the UBOOT countdown's box, the update's screens): every zone again at the next frame */
static void sm_redraw_all(void)
{
    sm_zvalid = 0;
    sm_dirty = 1;
}

static uint32_t smd_mix(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }

/* a band of the screen: rows y .. y + h of the ground, drawn on the canvas with the zone's own y subtracted */
static uint32_t smd_y;
static void smd_begin(uint32_t y, uint32_t h)
{
    smd_y = y;
    cv_begin(240u, h, T_BG);
    sm_draw_px += 240u * h;
}
static void smd_end(void) { cv_blit(0u, smd_y); }
static int32_t smd_row(int32_t y) { return y - (int32_t)smd_y; }              /* a screen row on the canvas */

/* a text on its own clean ground: SMT_CLEAN behind its ink box (and a margin), so no grain, dust or blood lands in or at the
 * ink; the ground around it keeps the photocopy. smd_text at the pen x, y; smd_text_r ending at xr */
static int32_t smd_text(int32_t x, int32_t y, const aafont_t *f, const char *s, uint16_t fg)
{
    int32_t b[4];
    text_ink(f, s, b);
    if (b[2] > b[0])
        cv_rect(x + b[0] - 2, y + b[1] - 2, b[2] - b[0] + 4, b[3] - b[1] + 4, SMT_CLEAN);
    return cv_text_on(x, y, f, s, fg, SMT_CLEAN);
}
static void smd_text_r(int32_t xr, int32_t y, const aafont_t *f, const char *s, uint16_t fg)
{
    smd_text(xr - text_w(f, s), y, f, s, fg);
}

static const sm_sound_t *smd_snd(void) { return &smp.pat[smp.cur % SM_NPAT].kit.snd[sm_ui.trk % SM_NTRK]; }

/* does every character of s have a glyph in f (AF_L is sparse: figures, + - . / # and most capitals) */
static int smd_has_glyphs(const aafont_t *f, const char *s)
{
    for (; *s; s++) {
        uint32_t c = (uint8_t)*s;
        if (c >= 'a' && c <= 'z' && f->last < 'a' && glyph_at(f, c) < 0)
            c -= 32u;
        if (glyph_at(f, c) < 0)
            return 0;
    }
    return 1;
}

static void smd_upper(char *d, const char *s, uint32_t n)
{
    uint32_t i;
    for (i = 0; i + 1u < n && s[i]; i++)
        d[i] = s[i] >= 'a' && s[i] <= 'z' ? (char)(s[i] - 32) : s[i];
    d[i] = 0;
}

/* parameter id of the current page on the track: its descriptor, value text and unit (d == 0: none) */
static const param_desc_t *smd_param(uint32_t id, char *val, const char **unit)
{
    const sm_sound_t *s = smd_snd();
    const param_desc_t *d;
    *unit = "";
    val[0] = 0;
    if (id == SM_NOPARAM)
        return 0;
    d = sm_desc_safe(sm_engine_of(s->machine), id);
    if (!d)
        return 0;
    param_format(d, clamp(s->p[id], d->min, d->max), val, unit);
    return d;
}

/* ------------------------------------------------------------ header --- */
static void smd_play_icon(int32_t x, int32_t y, int playing)    /* 9 x 9 at x, y (screen rows of the band) */
{
    int32_t r;
    if (!playing) {
        cv_rect(x, y, 9, 9, T_TEXT);
        return;
    }
    for (r = 0; r < 9; r++)
        cv_rect(x, y + r, (r < 5 ? r + 1 : 9 - r) * 2, 1, T_TEXT);
}
static uint32_t smd_hdr_sig(void)
{
    const sm_sound_t *s = smd_snd();
    uint32_t h = 2166136261u;
    h = smd_mix(h, sm_ui.trk); h = smd_mix(h, s->machine); h = smd_mix(h, smp.cur);
    h = smd_mix(h, smp.bpm10); h = smd_mix(h, smd_play); h = smd_mix(h, sm_rec != 0);
    return smd_mix(h, sm_ui.bank != 0);
}
static void smd_header(void)
{
    char b[16];
    int32_t x = 6, ty;
    const sm_sound_t *s = smd_snd();
    smd_begin(SMD_HDR_Y, SMD_HDR_H);
    ty = smd_row(0) + CAP_IN(S, 20);
    b[0] = 'T'; b[1] = (char)('1' + sm_ui.trk % SM_NTRK); b[2] = ' '; b[3] = 0;
    smd_upper(b + 3, ENGINES[sm_engine_of(s->machine)]->name, sizeof b - 3u);        /* T1 SAW */
    x = smd_text(x, ty, &AF_S, b, T_TEXT) + 7;
    cv_rect(x, ty + 6, 2, 2, T_MID);
    x += 9;
    b[0] = 'A'; b[1] = (char)('0' + ((smp.cur % SM_NPAT) + 1u) / 10u); b[2] = (char)('0' + ((smp.cur % SM_NPAT) + 1u) % 10u); b[3] = 0;
    x = smd_text(x, ty, &AF_S, b, T_TEXT) + 7;
    cv_rect(x, ty + 6, 2, 2, T_MID);
    x += 9;
    fmt_fix(b, smp.bpm10, 1);
    x = smd_text(x, ty, &AF_S, b, T_TEXT) + 7;
    cv_rect(x, ty + 6, 2, 2, T_MID);
    x += 9;
    smd_play_icon(x, ty + 3, smd_play);
    x += 10 + 9;
    if (sm_rec)
        smd_text(x, ty, &AF_S, "REC", T_REC);
    smd_text_r(234, ty, &AF_S, sm_ui.bank ? "2/2" : "1/2", T_TEXT);
    cv_rect(0, smd_row(20), 240, 2, T_ACCENT);
    smd_end();
}

/* -------------------------------------------------------------- page --- */
static void smd_page(void)
{
    const char *n = sm_cur_page()->name;
    smd_begin(SMD_PAGE_Y, SMD_PAGE_H);
    {   /* the shadow's ground and the name's first, then the two texts (the second's ground would cover the first) */
        int32_t b[4];
        text_ink(&AF_M, n, b);
        cv_rect(7 + b[0] - 2, 3 + b[1] - 2, b[2] - b[0] + 4, b[3] - b[1] + 4, SMT_CLEAN);
        cv_rect(6 + b[0] - 2, 2 + b[1] - 2, b[2] - b[0] + 4, b[3] - b[1] + 4, SMT_CLEAN);
        cv_text_on(7, 3, &AF_M, n, T_DIM, SMT_CLEAN);     /* the shadow */
        cv_text_on(6, 2, &AF_M, n, T_TEXT, SMT_CLEAN);
    }
    smd_end();
}

/* ------------------------------------------------------------- cells --- */
static uint32_t smd_row_sig(uint32_t r)
{
    const sm_page_t *pg = sm_cur_page();
    const sm_sound_t *s = smd_snd();
    uint32_t h = 2166136261u, c;
    h = smd_mix(h, sm_ui.page); h = smd_mix(h, s->machine);
    for (c = 0; c < 2u; c++) {
        uint32_t id = pg->id[r * 2u + c];
        h = smd_mix(h, id);
        if (id != SM_NOPARAM)
            h = smd_mix(h, (uint8_t)s->p[id]);
    }
    return h;
}
static void smd_cells(uint32_t r)                         /* the cells r * 2 and r * 2 + 1 */
{
    uint32_t c;
    smd_begin(SMD_CELL_Y + r * SMD_CELL_H, SMD_CELL_H);
    for (c = 0; c < 2u; c++) {
        char val[12], lab[16];
        const char *unit;
        const param_desc_t *d = smd_param(sm_cur_page()->id[r * 2u + c], val, &unit);
        int32_t x = (int32_t)(c * SMD_CELL_W);
        const aafont_t *vf = &AF_L;
        if (!d)
            continue;
        cv_rect(x, 4, 2, 56, T_DIM);                      /* a thick bar down the cell's edge */
        smd_upper(lab, d->label, sizeof lab);
        smd_text(x + 8, 4, &AF_S, lab, T_MID);
        if (unit[0])
            smd_text_r(x + (int32_t)SMD_CELL_W - 6, 4, &AF_S, unit, T_MID);
        if (!smd_has_glyphs(vf, val) || text_w(vf, val) > (int32_t)SMD_CELL_W - 18)
            vf = &AF_M;                                   /* a word AF_L has no letters for, or too wide */
        if (text_w(vf, val) > (int32_t)SMD_CELL_W - 18)
            vf = &AF_S;
        smd_text(x + 8, vf == &AF_L ? 22 : vf == &AF_M ? 28 : 31, vf, val, T_TEXT);
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
    char val[12], lab[16];
    const char *unit;
    const param_desc_t *d = smd_param(sm_cur_page()->sel, val, &unit);
    smd_begin(SMD_SEL_Y, SMD_SEL_H);
    if (d) {
        int32_t x = 8, ty = smd_row(178) + CAP_IN(S, 20);   /* (after smd_begin: the band's own y) */
        cv_rect(0, smd_row(178), 2, 20, T_DIM);
        smd_upper(lab, d->label, sizeof lab);
        x = smd_text(x, ty, &AF_S, lab, T_MID) + 10;
        x = smd_text(x, ty, &AF_S, val, T_TEXT);
        if (unit[0])
            smd_text(x + 6, ty, &AF_S, unit, T_MID);
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
    smd_begin(SMD_STRIP_Y, SMD_STRIP_H);
    for (i = 0; i < 16u; i++) {
        int32_t x = (int32_t)(i * SMD_STEP_W), y = smd_row(206);
        uint32_t s = b + i;
        if (pt->step[t][s].n) {
            cv_rect(x, y, 14, 30, T_TEXT);
        } else {
            cv_rect(x, y, 14, 30, T_DIM);
            cv_rect(x + 1, y + 1, 12, 28, SMT_CLEAN);         /* (not a flat ground colour: no grain inside the strip) */
        }
        if (ph == (int32_t)s) {                           /* the step playing: a 2 px blood frame, over a trig too */
            cv_rect(x, y, 14, 2, T_ACCENT);
            cv_rect(x, y + 28, 14, 2, T_ACCENT);
            cv_rect(x, y + 2, 2, 26, T_ACCENT);
            cv_rect(x + 12, y + 2, 2, 26, T_ACCENT);
        } else if (sm_ui.held_step == s) {                /* the step under a key held: a rust frame */
            cv_rect(x, y, 14, 2, T_MID);
            cv_rect(x, y + 28, 14, 2, T_MID);
            cv_rect(x, y + 2, 2, 26, T_MID);
            cv_rect(x + 12, y + 2, 2, 26, T_MID);
        }
    }
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
    if (!sm_dirty && smd_pos == sm_last_pos && smd_play == sm_last_play)
        return;
    sm_dirty = 0;
    sm_last_pos = smd_pos;
    sm_last_play = smd_play;
    sig[0] = smd_hdr_sig();
    sig[1] = smd_mix(2166136261u, sm_ui.page);
    sig[2] = smd_row_sig(0);
    sig[3] = smd_row_sig(1);
    sig[4] = smd_sel_sig();
    sig[5] = smd_strip_sig();
    for (z = 0; z < SMD_NZONE; z++) {
        if ((sm_zvalid & (1u << z)) && sm_zsig[z] == sig[z])
            continue;
        switch (z) {
        case 0: smd_header(); break;
        case 1: smd_page(); break;
        case 2: smd_cells(0); break;
        case 3: smd_cells(1); break;
        case 4: smd_select(); break;
        default: smd_strip(); break;
        }
        sm_zsig[z] = sig[z];
        sm_zvalid |= (uint8_t)(1u << z);
    }
}
