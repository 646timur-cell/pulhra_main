/* SPDX-License-Identifier: GPL-3.0-only */
/* Exercise the real packet service against a controller whose TX-ready flag clears
 * before the old DMA read has drained. Never overwrite that buffer while preparing
 * the next packet. Ring-only tests intentionally never touch this path. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../firmware/hal/fm1_usb.h"
#define FELUCCA_OTA 0
#define FELUCCA_CDC 0
#define FELUCCA_UAC 1
#define FELUCCA_UAC_FAST_POLL 1
#define HALF_FRAMES 128u
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
static uint32_t regs[8][24], index_reg, read_value;
static const void *dma;
static uint32_t dma_bytes, sends, overwritten, bad;
static uint32_t saved[256];
static void mock_wr(uint32_t reg, uint32_t value)
{
    if (reg == 14u) index_reg = value;
    else regs[index_reg][reg] = value;
}
static void mock_rd(uint32_t reg) { read_value = reg == 14u ? index_reg : regs[index_reg][reg]; }
static void mock_send(void *p, uint32_t n)
{
    if (dma && memcmp(dma, saved, dma_bytes)) overwritten++;
    dma = p; dma_bytes = n; sends++;
    if (n > sizeof saved) { bad++; return; }
    memcpy(saved, p, n);
}
#define fm1_usb_sie_on() 1u
#define fm1_usb_sie_wr_start(r, v) mock_wr(r, v)
#define fm1_usb_sie_rd_start(r) mock_rd(r)
#define fm1_usb_sie_done() 1u
#define fm1_usb_sie_data() read_value
#define fm1_usb_ep4_send(p, n) mock_send(p, n)
static void fm1_delay_ms(uint32_t ms) { (void)ms; }
#ifndef FELUCCA_UAC_NCH
#define FELUCCA_UAC_NCH 2
#endif
#if FELUCCA_UAC_NCH > 2
#define UAC_MASTER_PAIR 0xFFu
static uint32_t uac_stem[5][32];
#endif
#define __attribute__(x)
#include "../firmware/src/usb.c"

static uint32_t frame(uint32_t i, uint32_t pair) { return (i * 7919u) ^ (pair * 0x1234567u); }
static void transport(uint32_t nw, uint32_t rate48)
{
    uint32_t count = 0, turn, f, p, before;
    memset(&usb, 0, sizeof usb); memset(&uac, 0, sizeof uac);
    memset(regs, 0, sizeof regs);
    dma = 0; dma_bytes = 0; overwritten = 0; ua_r = ua_w = 0;
#if UA_W > 1
    uac_nw = nw;
#endif
    usb.up = usb.config = uac.alt = uac.go = uac.flowing = 1;
    uac.fill_min = UA_PRIME;
#if FELUCCA_UAC_48K
    uac.r48 = uac.ring48 = rate48;
    if (rate48) uac.fill_min = UA_PRIME48;
#else
    (void)rate48;
#endif
    for (turn = 0; turn < 2000u; turn++) {
        while (ua_w - ua_r < 256u) {
            for (p = 0; p < UA_W; p++) ua_ring[(ua_w & (UA_N - 1u)) * UA_W + p] = frame(ua_w, p);
            ua_w++;
        }
        regs[4][17] = 0; /* ready clears; previous packet's memory can still be read */
        uac_service();
        if (!dma_bytes || dma_bytes % (4u * nw)) { bad++; break; }
        for (f = 0; f < dma_bytes / (4u * nw); f++, count++)
            for (p = 0; p < nw; p++)
                if (((const uint32_t *)saved)[f * nw + p] != frame(count, p)) bad++;
        before = sends;
        uac_service(); /* TX still busy: must not queue or consume another packet */
        if (sends != before) bad++;
    }
    if (uac.underruns || uac.overruns || overwritten) bad++;
    printf("%u channels, %u Hz: %u ordered frames, %u in-flight overwrites\n", nw * 2u, rate48 ? 48000u : 44100u, count, overwritten);
    before = uac.stalls;
    for (turn = 0; turn < 198u; turn++) uac_service();
    if (uac.stalls != before) bad++;
    for (turn = 0; turn < 4u; turn++) uac_service();
    uac_render_start(); /* the producer sees the stopped reader and disarms the ring */
    if (uac.stalls != before + 1u || uac.go || uac.flowing) bad++;
}
int main(void)
{
    transport(UA_W, 0);
#if FELUCCA_UAC_48K
    transport(1u, 1);
#endif
#if UA_W > 1
    transport(1u, 0); /* SUM: same storage stride, just the master pair on the wire */
#endif
    printf("uac service: %s\n", bad ? "FAIL" : "ok");
    return bad != 0;
}
