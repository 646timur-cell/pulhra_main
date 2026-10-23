/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* PULHRA boot and main loop, after main.c: the same boot order (WDT first, boot-loop guard, fatal vectors, guards; then
 * LCD, input (TIMER5 IRQ, 10 kHz), audio (ALNK0 IRQ)), the same UBOOT and M-UPGRADE paths, a 15 ms loop of
 * sm_input_poll / sm_draw (pulhra/sm_app.c). No web editor, no console. The banks and the project: sm_bank.c (loaded at boot,
 * SAVE, the autosave from sm_input_poll). */
extern uint32_t _data_start[], _data_end[], _data_load[], _bss_start[], _bss_end[];
extern uint32_t _pool_start[], _pool_end[], _rt_start[], _rt_end[], _rt_load[];


/* TIMER5 outranks ALNK0, so the scan keeps its 100 us pace while a half buffer renders: before,
 * the ticks stopped for the whole render (0.7 ms idle, several ms loaded), the column lit when it
 * began stayed lit that long (a ~16 Hz flicker over all LEDs, beating with the scan) and the
 * encoders lost frames. Nested in ALNK0 it only scans (GPIO + fm1_in, nothing the audio ISR touches)
 * and counts ms; USB and UART polls wait for the first tick after the render, as they always did,
 * and the time spent nested is handed to the audio ISR so its load figures stay render-only.
 * The USB audio stream cannot wait for the render (one packet per 1 ms frame, a render takes up to
 * ~5 ms): uac_service also runs nested. It touches only EP4 (INDEX is set on every access) and the
 * consumer side of the audio ring, and usb_poll never runs nested, so the two never interleave. */
void fm1_timer5_irq(void)
{
    static uint32_t sub, owed;
    uint32_t t0 = fm1_ticks(), usb_due = sub % 5u == 0u;
    fm1_timer5_ack();
    felucca_dbg.timer_irqs++;
    fm1_input_tick();
    {   /* milliseconds from the 24 MHz TIMER4 (robust to a late tick) */
        static uint32_t last, acc;
        acc += t0 - last;
        last = t0;
        while (acc >= 1000u * FM1_TICKS_PER_US) {
            acc -= 1000u * FM1_TICKS_PER_US;
            fm1_ms++;
        }
    }
    if (usb_due)
        owed |= 1u;                             /* 2 kHz: all USB SIE traffic lives here */
    if (sub % 5u == 2u)
        owed |= 2u;                             /* 2 kHz: <= ~7 bytes per call at 31250 baud */
    if (++sub == 10u)
        sub = 0;
    if (felucca_dbg.in_audio) {
        felucca_dbg.nested++;
        if (usb_due)
            uac_service();
        t5_nested_ticks += fm1_ticks() - t0;
        return;
    }
    if (owed & 1u)
        usb_poll();
    if (owed & 2u)
        uart_midi_poll();
    owed = 0;
}
extern void isr_timer5(void);

/* everything the PULHRA build puts in .noinit (app.ld NOINIT, 0x3D50 B): the panel table and the settings (panel.c), the crash
 * record (fm1_irq.h), the boot guard (core.h), felucca_dbg (audio.c), and sm_edit.c's clipboard and UNDO snapshot; 16 B a
 * piece for the linker's alignment. A new .noinit object of PULHRA's goes in this sum */
_Static_assert(sizeof panel + sizeof settings + sizeof fm1_crash + sizeof bootguard + sizeof felucca_dbg + sizeof sm_clip +
               sizeof sm_undo_buf + 7u * 16u <= 0x3D50u, "PULHRA's .noinit fits app.ld's NOINIT");

static void timer5_start(void)                 /* OSC /4 = 6 MHz, PRD 600 -> 10 kHz */
{
    fm1_timer5_start(isr_timer5, 4);   /* above ALNK0 (3): the scan nests into the render (see fm1_timer5_irq) */
}

static void hexs(char *b, uint32_t v)
{
    uint32_t i;
    for (i = 0; i < 8u; i++)
        b[i] = "0123456789ABCDEF"[(v >> (28u - 4u * i)) & 15u];
    b[8] = 0;
}

static void fm1_fault(const fm1_crash_t *c)
{
    char b[12];
    uint32_t t0;
    fm1_audio_stop();
    lcd_wake_now();                                   /* (SCREEN OFF: the panel on first) */
    lcd_fill(0, 0, 240, 240, UI_CRASH_BG);            /* fixed, outside the palettes */
    draw_text_line(0, 8, 240, &AF_M, "PULHRA CRASH", UI_CRASH_INK, UI_CRASH_BG, 1);
    hexs(b, c->vec);
    draw_text_line(10, 40, 220, &AF_M, b, UI_CRASH_INK, UI_CRASH_BG, 0);
    hexs(b, c->pc);
    draw_text_line(10, 60, 220, &AF_M, b, UI_CRASH_INK, UI_CRASH_BG, 0);
    hexs(b, c->emu);
    draw_text_line(10, 84, 220, &AF_M, b, UI_CRASH_INK, UI_CRASH_BG, 0);
    hexs(b, c->dbg);
    draw_text_line(10, 102, 220, &AF_M, b, UI_CRASH_INK, UI_CRASH_BG, 0);
    hexs(b, c->rets);
    draw_text_line(10, 120, 220, &AF_M, b, UI_CRASH_INK, UI_CRASH_BG, 0);
    t0 = fm1_ticks();
    while ((uint32_t)(fm1_ticks() - t0) < 4000u * 1000u * FM1_TICKS_PER_US)
        ;
    fm1_reboot();
}

/* the flash: the part must be the expected one (else RAM-only, as project.c persist_boot); the user sample slots are
 * scanned (the sample machines play from them); the settings record is read for the learned panel table (so the knobs
 * and buttons map as on a calibrated unit) and the HOLD / LEDS / palette settings (sm_settings_load). PULHRA writes no
 * settings: the record stays as Felucca left it */
static void persist_boot(void)
{
    uint32_t f = irq_save(), k;
    flash_ok = FL_FAR(fl_jedec_ram)() == 0x856014u;
    irq_restore(f);
    if (!flash_ok)
        return;
    fl_plain_window_init();                        /* flash above 0x93000 reads as plaintext through XIP */
    for (k = 0; k < SMP_USER_SLOTS; k++)
        smp_user_scan(k);
    sm_settings_load();
}

static uint8_t sm_uboot;                              /* the UBOOT countdown shown (seconds left, 0 = none) */

/* PULHRA's boot splash, minimal: "pulhra" in big clean pixel letters (the 5 x 7 font at scale 4), typed in one by one —
 * complete by about a second of the one and a half, then it stands — with a few large random characters drifting
 * silently down the margins (dim, sparse, one bright at a time), the ASCII spinner and the version below.
 * t: 0..100 the progress. The emulator's web_ms drives it too (web/emu/pulhra_web.c).
 * Drawn on the canvas in two bands of 240 x 120 (the canvas holds at most 240 x 124: gfx.c CV_MAX), each blitted whole: no
 * full-screen fill a frame (it flickered, and every frame cost a fill and a blit) and nothing past the canvas (one 240 x 240
 * band was cut to 124 rows: the spinner and the version never showed) */
static void sm_splash_band(uint32_t t, int32_t y0)
{
    static const char *const W = "pulhra";
    static const char spin[4] = {'|', '/', '-', '\\'};
    static const char *const CH = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ@#$%&*+";
    char b[8];
    uint32_t i, c0, a, on, s = t * 2654435761u + 0x9E3779B9u;
    smd_begin(0, (uint32_t)y0, 240u, 120u, T_BG);
    /* the minimal shimmer: six large random characters (scale 3) drift down the two margins, dim; one bright turns */
    for (c0 = 0; c0 < 6u; c0++) {
        s = s * 1664525u + 1013904223u;
        b[0] = CH[(s >> 8) % 40u]; b[1] = 0;
        sm_px_text((int32_t)((c0 & 1u) ? 204 : 12), (int32_t)(4 + (c0 * 37u + t * 3u / 2u) % 196) - y0, 3u, b,
                   (uint16_t)((c0 == (t / 15u) % 6u) ? T_TEXT : SM_DIM_INK), 0);
    }
    /* "pulhra": the letters typed in (complete by ~67 % of the time, so the word stands before the screen changes) */
    a = t * 3u / 2u;
    on = 6u * (a > 100u ? 100u : a) / 100u;
    for (i = 0; i < 6u && i < on; i++)
        b[i] = W[i];
    b[i] = 0;
    sm_px_text((240 - 24 * (int32_t)i) / 2, 88 - y0, 4u, b, T_TEXT, 0);
    if (i == 0u || (i < 6u && ((t / 15u) & 1u)))       /* the blinking typing cursor */
        cv_rect((240 - 24 * (int32_t)i) / 2 + 24 * (int32_t)i - 6, 114 - y0, 12, 8, T_TEXT);
    b[0] = spin[(t / 25u) & 3u];                       /* the ASCII spinner: | / - \ */
    b[1] = 0;
    sm_px_text(114, 196 - y0, 2u, b, T_TEXT, 0);
    sm_px_text((240 - (int32_t)(12 * smd_len(FELUCCA_VERSION))) / 2, 214 - y0, 2u, FELUCCA_VERSION, T_TEXT, 0);
    smd_end();
}
void sm_splash_paint(uint32_t t)
{
    t = t > 100u ? 100u : t;
    sm_splash_band(t, 0);
    sm_splash_band(t, 120);
    lcd_sync();
}

static void fm1_main(void)
{
    uint32_t ms;
    persist_boot();
    if (flash_ok)
        ota_boot_cleanup();                             /* staging area left by an update */
    settings_init();
    fm1_led_dim_level(settings_leds == LEDS_DIM_LO);    /* the glow level of the LEDS setting (Felucca's boot_leds) */
    sm_ux_mk1();                                        /* white on black over whatever palette the settings picked */
    lcd_init();
    sm_splash_paint(100u);                              /* the first frame at once: the whole word */
    if (felucca_dbg.magic != DBG_MAGIC) {
        memset(&felucca_dbg, 0, sizeof felucca_dbg);
        felucca_dbg.magic = DBG_MAGIC;
    }
    felucca_dbg.boots++;
    felucca_dbg.max_us = 0;
    felucca_dbg.in_audio = 0;                       /* .noinit: a reset inside the audio ISR left it set, and
                                                       TIMER5 would treat every tick as nested (no USB poll) */
    felucca_dbg.prev_stage = felucca_dbg.stage;     /* a WDT reset leaves the last breadcrumb here */
    felucca_dbg.prev_page = felucca_dbg.page;
    felucca_dbg.prev_home = felucca_dbg.home;
    felucca_dbg.prev_frames = felucca_dbg.ui_frames;
    felucca_dbg.prev_rst = fm1_boot.p3_rst;
    fm1_input_init();
    fm1_adc_init();
    panel_init();
    sm_core_init();
    sm_store_boot();                                    /* (the banks and the project from the flash: sm_bank.c) */
    master_boot();
    audio_init();
    usb_start();
    uart_midi_init();
    timer5_start();
    fm1_guard_lock_top();
    fm1_irq_enable_all();
    fm1_delay_ms(30);
    /* the animated splash: "pulhra" typed in, the large characters drifting softly along the margins; the pot followed.
     * 1.5 s by the clock, not by a frame count (a frame's LCD transfer is not free), the watchdog fed every frame, and at
     * most 300 frames should the clock not run */
    {
        uint32_t t0 = fm1_ms, el;
        for (ms = 0; ms < 300u; ms++) {
            fm1_wdt_feed();
            master_poll();
            el = (uint32_t)(fm1_ms - t0);
            if (el >= 1500u)
                break;
            sm_splash_paint(100u * el / 1500u);
            fm1_delay_ms(10);
        }
        fm1_wdt_feed();
    }
    lcd_fill(0, 0, 240, 240, T_BG);

    for (;;) {
        uint32_t m = fm1_ms;
        fm1_wdt_feed();
        usb_retry(fm1_ms);
        if (fm1_ms > 30000u && bootguard.pending) {     /* a crash or hang in the first 30 s counts */
            bootguard.pending = 0;
            bootguard.failed = 0;
        }
        master_poll();
        {   /* OCT- + OCT+ held 5 s: enter UBOOT with RAM intact (debug / update); a countdown
             * shows from 2 s, letting go cancels it */
            static uint32_t t0;
            uint32_t both = (1u << panel.btn[B_OCTDN]) | (1u << panel.btn[B_OCTUP]);
            if ((fm1_in.buttons & both) != both) {
                if (sm_uboot) {
                    sm_uboot = 0;
                    lcd_fill(0, 100, 240, 40, T_BG);    /* what the countdown covered */
                    sm_redraw_all();                    /* (the zones it covered, drawn again) */
                }
                t0 = fm1_ms;
            } else if (fm1_ms - t0 > 2000u && fm1_ms - t0 <= 5000u) {
                uint32_t left = (5000u - (fm1_ms - t0) + 999u) / 1000u;
                if (left != sm_uboot) {
                    char s[8] = {'U', 'B', 'O', 'O', 'T', ' ', (char)('0' + left), 0};
                    sm_uboot = (uint8_t)left;
                    draw_text_box(0, 110, 240, &AF_M, s, T_THEME, 1);
                    lcd_sync();
                }
            } else if (fm1_ms - t0 > 5000u) {
                fm1_audio_stop();
                lcd_wake_now();
                lcd_fill(0, 0, 240, 240, T_BG);
                draw_text_box(0, 110, 240, &AF_M, "UBOOT", T_THEME, 1);
                usb_detach();
                fm1_delay_ms(30);
                bootguard.pending = 0;                  /* intentional reset: not a failed boot */
                fm1_enter_uboot();
            }
        }
        ota_service();                                  /* M-UPGRADE handshake */
        if (usb.ota_req) {                              /* M-UPGRADE upgrade command */
            usb.ota_req = 0;
            panic_req = (1u << NTRK) - 1u;               /* every track (bit per track) */
            {                                           /* and the transport stopped: no new note during the flash */
                sm_cmd_t c = {SMC_STOP, 0, 0, 0, 0};    /* writes (a full queue empties within a block: a few tries) */
                uint32_t k;
                for (k = 0; k < 10u && !sm_cmd_push(c); k++)
                    fm1_delay_ms(1);
            }
            lcd_wake_now();                             /* (its progress on the screen) */
            if (flash_ok)
                ota_session();                          /* returns only if nothing was committed */
            lcd_fill(0, 0, 240, 240, T_BG);
            sm_redraw_all();                            /* (the update's screens gone: the whole screen again) */
        }
        if (usb.uboot_req) {                            /* SysEx F0 22 24 35 7D F7 from the host */
            fm1_audio_stop();
            lcd_wake_now();
            lcd_fill(0, 0, 240, 240, T_BG);
            draw_text_box(0, 110, 240, &AF_M, "UBOOT (USB)", T_THEME, 1);
            fm1_delay_ms(20);
            usb_detach();
            fm1_delay_ms(30);
            bootguard.pending = 0;
            fm1_enter_uboot();
        }
        felucca_dbg.ui_frames++;
        felucca_dbg.stage = 1;
        sm_input_poll();
        felucca_dbg.stage = 2;
        sm_draw();
        sm_leds();
        felucca_dbg.stage = 9;
        while (fm1_ms - m < 15u)                        /* ~60 frames/s at most */
            sm_input_poll();
    }
}

void fm1_cstart(void)
{
    uint32_t *s, *d, p3, src, wdt;
    fm1_time_init();
    fm1_reset_reason();
    p3 = fm1_boot.p3_rst;
    src = fm1_boot.rst_src;
    wdt = fm1_boot.wdt_con;
    fm1_wdt_arm(0x0D);
    if (bootguard.magic != BOOTGUARD_MAGIC) {
        bootguard.magic = BOOTGUARD_MAGIC;
        bootguard.failed = 0;
        bootguard.pending = 0;
    }
    if (bootguard.pending)
        bootguard.failed++;
    bootguard.pending = 1;
    if (bootguard.failed >= 2u) {
        bootguard.failed = 0;
        bootguard.pending = 0;
        fm1_enter_uboot();
    }
    fm1_irq_init();
    for (d = _bss_start; d < _bss_end; d++)
        *d = 0;
    for (d = _pool_start; d < _pool_end; d++)
        *d = 0;
    for (s = _data_load, d = _data_start; d < _data_end; s++, d++)
        *d = *s;
    for (s = _rt_load, d = _rt_start; d < _rt_end; s++, d++)
        *d = *s;                                /* flash driver code that must run from RAM */
    fm1_mailbox_clear();
    fm1_guard_enable(FM1_GUARD_STACK | FM1_GUARD_WRITE | FM1_GUARD_BUS | FM1_GUARD_PC);
    fm1_boot.p3_rst = (uint8_t)p3;
    fm1_boot.rst_src = src;
    fm1_boot.wdt_con = (uint8_t)wdt;
    fm1_main();
    for (;;)
        ;
}
