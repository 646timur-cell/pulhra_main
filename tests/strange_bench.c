/* SPDX-License-Identifier: GPL-3.0-only */
/* CPU of the mix with given sounds held (tests/strange_cpu.sh runs it under valgrind, twice, and takes the difference:
 * instructions per sample of the audio path alone).
 *   strange_bench BLOCKS T:ENGINE:PRESET:NOTES ...   (T 0..3; NOTES held from C3 up in fifths and thirds) */
#define FELUCCA_MODULE 1
#define UI_TEST_NO_MAIN 1
#include "ui_test.c"

int main(int argc, char **argv)
{
    int32_t buf[2 * CTL];
    uint32_t blocks = (uint32_t)atoi(argv[1]), i, k;
    static const uint8_t NOTES[] = {48, 55, 60, 64, 67, 71, 74, 79};
    ui_power_on();
    song.master_q12 = 4096;
    for (i = 2; i < (uint32_t)argc; i++) {
        unsigned t, e, p, n;
        if (sscanf(argv[i], "%u:%u:%u:%u", &t, &e, &p, &n) != 4 || t >= NTRK || e >= NENGINES)
            return 2;
        set_engine_of(&trk[t], e);
        apply_preset_to(&trk[t], p);
        trk[t].p[P_REL] = 127;                            /* (held: nothing ends while measured) */
        for (k = 0; k < 8u; k++)
            mix_block(buf, CTL);
        for (k = 0; k < n && k < 8u; k++)
            midi_event(0x90, t, NOTES[k] + (t ? 12u : 0u), 100);
    }
    for (k = 0; k < 64u; k++)                           /* (attacks done) */
        mix_block(buf, CTL);
    for (k = 0; k < blocks; k++)
        mix_block(buf, CTL);
    printf("voices %u\n", (unsigned)voices_busy());
    return 0;
}
