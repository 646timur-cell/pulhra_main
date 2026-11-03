/* SPDX-License-Identifier: GPL-3.0-only */
/* Exercise a delayed producer against the real USB ring and rate controller.
 * A short render gap must be absorbed without repeating USB frames. */
#define main legacy_uac_main
#include "uac_test.c"
#undef main

int main(void)
{
    uint32_t before, before_rep, before_bad, phase;
    usb.config = 1;
    memset(&uac, 0, sizeof uac);
    uac_stream(1);
#ifdef TEST_RATE48
    uac_rate_set(48000);
#endif
    uac.flowing = 1;
    next_half = 300000;
    next_pkt = 1000000;
    run(2000, REND_MAX, 1, 1);
    before = uac.underruns + uac.overruns;
    before_rep = cons_rep;
    before_bad = cons_bad;
    for (phase = 0; phase < 20u; phase++) {
        run(500 + phase, REND_MAX, 1, 1);
        run(2, REND_MAX, 0, 1);
        run(500, REND_MAX, 1, 1);
    }
    printf("USB producer gaps (2 ms, 20 phases): underrun/overrun +%u, repeats +%u; band %u..%u\n",
           uac.underruns + uac.overruns - before, cons_rep - before_rep, UA_LO, UA_HI);
    return uac.underruns + uac.overruns != before || cons_rep != before_rep || cons_bad != before_bad;
}
