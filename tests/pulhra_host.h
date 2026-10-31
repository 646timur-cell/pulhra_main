/* SPDX-License-Identifier: GPL-3.0-only */
/* PULHRA host prologue: the sound core of the firmware (libc, core.h, engines, params, mod, voice, slicer, fx) as
 * hostsim.c has it, without seq.c / song_chain.c / ui*.c, and the HAL stubs of ui_test.c (input, LEDs, display).
 * pulhra_test.c includes it, then pulhra/sm_app.c. */
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define __attribute__(x)
#define memset felucca_memset
#define memcpy felucca_memcpy
#define memcmp felucca_memcmp
#include "felucca_tables.h"
#include "../firmware/src/libc.c"
#undef memset
#undef memcpy
#undef memcmp
static struct { volatile uint32_t notes, buttons, frames; uint32_t enc_dfr[8]; } fm1_in;
#include "../firmware/src/core.h"
static const uint8_t *host_xip(uint32_t off);          /* (pulhra_test.c: the simulated flash, the user slot read as XIP) */
#define SMP_USER_XIP(k) host_xip(SMP_USER_BASE + SMP_SLOT_OFF(k))
#include "../firmware/src/engines.c"
#include "../firmware/src/params.c"
#include "../firmware/src/mod.c"
#include "../firmware/src/voice.c"
#include "../firmware/src/slicer.c"
#include "../firmware/src/fx.c"

/* ------------------------------------------------------------ HAL stubs --- */
#define FM1_NCOL 11u
/* hal/fm1_input.h's key id at (physical column, packed row bit), -1 = none (a copy: that header is the hardware's) */
static const int8_t FM1_KEYMAP[6][FM1_NCOL] = {
    {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    { 5, 11,  4, 10,  3,  9,  2,  8, -1, -1, -1},
    {34, 35, 36, 37, 38, 40, 39, 13,  7,  6, 12},
    {23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33},
    { 0,  1, 15, 14, 17, 16, 19, 18, 20, 21, 22},
    {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
};
static uint8_t fm1_led[FM1_NCOL], fm1_led_dim[FM1_NCOL], fm1_led_breath[FM1_NCOL], fm1_led_mid[FM1_NCOL];
#define FM1_TICKS_PER_US 1u
static uint32_t host_ticks, host_pressed, host_notes;
static int32_t host_enc[7];
static uint32_t fm1_ticks(void) { return host_ticks; }
static uint32_t fm1_input_edges(int x) { uint32_t p = host_pressed; (void)x; host_pressed = 0; return p; }
static uint32_t fm1_input_note_edges(void) { uint32_t n = host_notes; host_notes = 0; return n; }
static int32_t fm1_enc_take(uint32_t e)
{
    int32_t s = host_enc[e % 7u];
    host_enc[e % 7u] = 0;
    return s;
}
static void fm1_wdt_feed(void) {}
static void fm1_irq_off(void) {}
static void fm1_irq_on(void) {}
static void fm1_delay_ms(uint32_t ms) { (void)ms; }
