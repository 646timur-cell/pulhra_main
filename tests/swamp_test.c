/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test of SWAMP MACHINE (firmware/src/swamp.c: the MONOFM sound core under its own app, swamp/sm_*.c).
 *   CORE   the sound core alone (no seq.c, no ui*.c), from sm_core_init (the firmware's power-on): silent tracks mix to silence
 *   cc -O2 -w -DFELUCCA_SWAMP=1 -DFELUCCA_MONOFM=1 -DFELUCCA_MODULE=0 -Ibuild/gen -Ifirmware/src -o build/host/swamp_test tests/swamp_test.c -lm */
#include "swamp_host.h"
#include "../firmware/src/swamp/sm_app.c"

static int check(const char *what, int ok)
{
    printf("swamp: %-74s %s\n", what, ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}

static int test_core_silent(void)
{
    int32_t out[2 * CTL];
    uint32_t b, i, peak = 0;
    sm_core_init();
    for (b = 0; b < 100u; b++) {
        mix_block(out, CTL);
        for (i = 0; i < 2u * CTL; i++) {
            uint32_t a = (uint32_t)(out[i] < 0 ? -out[i] : out[i]);
            peak = a > peak ? a : peak;
        }
    }
    printf("swamp: peak %u\n", peak);
    return check("CORE: 100 blocks of silent tracks, |x| <= 1", peak <= 1u);
}

int main(void)
{
    int bad = 0;
    bad += test_core_silent();
    printf("swamp: %d failed\n", bad);
    return bad != 0;
}
