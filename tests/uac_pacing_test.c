/* SPDX-License-Identifier: GPL-3.0-only */
/* Real five-channel producer/packet controller with variable render and USB phases.
 * Keep 44.1 kHz packets at 44/45 frames while preserving every generated frame. */
#define main service_main
#include "uac_service_test.c"
#undef main
static uint32_t random_state = 12u;
static uint32_t rnd(void) { random_state = random_state * 1664525u + 1013904223u; return random_state; }
static int stream(double rate)
{
    double now = 0, next_half = 300.0, next_block = 0, next_packet = 1000, render = 0;
    uint32_t left = 0, produced = 0, consumed = 0, faults = 0, nonstandard = 0, data_seen = 0;
    uint32_t packet[UA_MAXF * UA_W];
    memset(&uac, 0, sizeof uac); ua_r = ua_w = 0;
    uac_nw = UA_W; usb.config = 1; uac.alt = uac.flowing = 1;
    while (now < 6000000.0) {
        double next = next_half < next_packet ? next_half : next_packet;
        if (left && next_block < next) next = next_block;
        now = next;
        if (now == next_half) {
            next_half += 128.0 / rate * 1e6;
            uac_render_start();
            left = 4;
            render = 200.0 + rnd() % 2300u;
            next_block = now + render / 4;
        } else if (left && now == next_block) {
            int32_t output[64] = {0};
            for (uint32_t i = 0; i < 32u; i++, produced++)
                for (uint32_t p = 0; p < UA_W; p++)
                    uac_stem[p][i] = (produced & 0xFFFFu) | ((~produced & 0xFFFFu) << 16);
            uac_tap(output, 32);
            left--; next_block += render / 4;
        } else {
            uint32_t n = uac_packet(packet);
            if (n < 44u || n > 45u) nonstandard++;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t value = packet[i * UA_W];
                if (!data_seen && value == 0) continue;
                data_seen = 1;
                uint32_t expected = (consumed & 0xFFFFu) | ((~consumed & 0xFFFFu) << 16);
                for (uint32_t p = 0; p < UA_W; p++) if (packet[i * UA_W + p] != expected) faults++;
                consumed++;
            }
            next_packet = ((uint32_t)(now / 1000) + 1u) * 1000.0 + rnd() % 500u;
        }
    }
    printf("five-channel %.1f Hz: %u frames, corrupt %u, outside 44/45 %u, underruns %u, overruns %u\n", rate, consumed, faults, nonstandard, uac.underruns, uac.overruns);
    return faults || nonstandard || uac.underruns || uac.overruns;
}
int main(void) { return stream(44117.6) | stream(44085.0) | stream(44150.0); }
