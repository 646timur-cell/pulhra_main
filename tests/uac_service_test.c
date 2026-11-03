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
static uint32_t wire_channels(uint32_t nw)
{
    return FELUCCA_UAC_NCH == 5 && nw > 1u ? 5u : nw * 2u;
}
static uint16_t wire_sample(uint32_t index)
{
    const uint8_t *b = (const uint8_t *)saved + 2u * index;
    return (uint16_t)(b[0] | (uint16_t)b[1] << 8);
}
static uint16_t expected_sample(uint32_t count, uint32_t channel, uint32_t nw)
{
    static const uint8_t compact[5] = {2, 3, 4, 5, 0};
    uint32_t slot = FELUCCA_UAC_NCH == 5 && nw > 1u ? compact[channel] : channel;
    return (uint16_t)(frame(count, slot / 2u) >> (16u * (slot & 1u)));
}
static void descriptors(uint32_t nw)
{
    uint32_t off, fmt = 0, terminal = 0, endpoint = 0, interface = 0xFFu;
#if UA_W > 1
    uac_nw = nw;
#endif
    const uint8_t *config;
    uint16_t length;
    if (!get_desc(0x0200, &config, &length)) { bad++; return; }
    for (off = 0; off < length;) {
        const uint8_t *d = config + off;
        if (d[0] < 2 || off + d[0] > length) { bad++; break; }
        if (d[1] == 4) interface = d[2];
        if (interface == UAC_AS_IF && d[1] == 0x24 && d[0] == 11 && d[2] == 2 && d[3] == 1) {
            fmt++;
            if (d[4] != wire_channels(nw) || d[5] != 2 || d[6] != 16) bad++;
        }
        if (interface == UAC_AS_IF && d[1] == 0x24 && d[0] == 14 && d[2] == 2 && d[3] == 1) {
            fmt++;
            if (d[4] != wire_channels(nw) || d[5] != 2 || d[6] != 16) bad++;
        }
        if (d[1] == 0x24 && d[0] == 12 && d[2] == 2 && d[3] == 1) {
            terminal++;
            if (d[7] != wire_channels(nw)) bad++;
        }
        if (d[1] == 5 && d[2] == 0x84) {
            uint32_t maxp = d[4] | (uint32_t)d[5] << 8;
            endpoint++;
            if (maxp != UA_MAXF * wire_channels(nw) * 2u) bad++;
            if (FELUCCA_UAC_NCH == 5 && maxp > 512u) bad++;
        }
        off += d[0];
    }
    printf("descriptors %u ch: fmt %u terminal %u endpoint %u bad %u\n", wire_channels(nw), fmt, terminal, endpoint, bad);
    if (fmt != 1 || terminal != 1 || endpoint != 1) bad++;
#if FELUCCA_UAC_NCH == 5
    if (DEV_DESC_PLAIN[12] == DEV_DESC_SUM[12]) bad++;
#endif
}

static void transport(uint32_t nw, uint32_t rate48)
{
    uint32_t count = 0, turn, f, p, before, channels = wire_channels(nw), bytes_per_frame = channels * 2u;
    descriptors(nw);
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
        uac.fill_min = turn % 3u == 0u ? UA_HI + 1u : turn % 3u == 1u ? UA_LO - 1u : UA_PRIME;
#if FELUCCA_UAC_48K
        if (rate48) uac.fill_min = turn % 3u == 0u ? UA_HI48 + 1u : turn % 3u == 1u ? UA_LO48 - 1u : UA_PRIME48;
#endif
        regs[4][17] = 0; /* ready clears; previous packet's memory can still be read */
        uac_service();
        if (!dma_bytes || dma_bytes % bytes_per_frame) { bad++; break; }
        for (f = 0; f < dma_bytes / bytes_per_frame; f++, count++)
            for (p = 0; p < channels; p++)
                if (wire_sample(f * channels + p) != expected_sample(count, p, nw)) bad++;
        if (FELUCCA_UAC_NCH == 5 && dma_bytes > 460u) bad++;
        before = sends;
        uac_service(); /* TX still busy: must not queue or consume another packet */
        if (sends != before) bad++;
    }
    if (uac.underruns || uac.overruns || overwritten) bad++;
    printf("%u channels, %u Hz: %u ordered frames, %u in-flight overwrites\n", channels, rate48 ? 48000u : 44100u, count, overwritten);
    before = uac.stalls;
    for (turn = 0; turn < 198u; turn++) uac_service();
    if (uac.stalls != before) bad++;
    for (turn = 0; turn < 4u; turn++) uac_service();
    uac_render_start(); /* the producer sees the stopped reader and disarms the ring */
    if (uac.stalls != before + 1u || uac.go || uac.flowing) bad++;
}
static void kick_through_tap(void)
{
#if FELUCCA_UAC_NCH == 5
    int32_t output[64] = {0};
    uint32_t packet[UA_MAXF * UA_W], i, frames, bytes;
    memset(&uac, 0, sizeof uac);
    ua_r = ua_w = 0;
    uac_nw = UA_W;
    uac.go = uac.feed = 1;
    uac.fill_min = UA_PRIME;
    for (i = 0; i < 32u; i++) {
        uac_stem[0][i] = 12345u;
        uac_stem[1][i] = 0;
        uac_stem[2][i] = 0;
    }
    uac_tap(output, 32);
    uac_tap(output, 32);
    frames = uac_packet(packet);
    bytes = uac_wire_packet(packet, frames);
    for (i = 0; i < frames; i++) {
        const uint8_t *p = (const uint8_t *)packet + 10u * i + 8u;
        if ((p[0] | (uint16_t)p[1] << 8) != 12345u) bad++;
    }
    if (bytes != frames * 10u) bad++;
    printf("kick through tap -> ring -> packet -> physical channel 5: %u frames, errors %u\n", frames, bad);
#endif
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
    kick_through_tap();
    printf("uac service: %s\n", bad ? "FAIL" : "ok");
    return bad != 0;
}
