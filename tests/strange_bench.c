/* SPDX-License-Identifier: GPL-3.0-only */
/* CPU of the mix with given sounds held (tests/strange_cpu.sh runs it under valgrind, twice, and takes the difference:
 * instructions per sample of the audio path alone).
 *   strange_bench BLOCKS T:ENGINE:PRESET:NOTES ...   (T 0..3; NOTES held from C3 up in fifths and thirds)
 * GLKIT=1: GLITCH plays USR1, one zone of the kit's slices end to end (read from flash through its index) */
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
    if (getenv("GLKIT")) {
        smp_user_hdr_t *h = (smp_user_hdr_t *)host_slots;
        uint32_t len = 0, nn = 0;
        memset(h, 0, sizeof *h);
        for (k = 0; k < GLT_NZ; k++) {
            uint32_t nb = GLT_ZONES[k].n / 2u;
            memcpy((uint8_t *)h + SMP_USER_DATA + len, &SMP_DATA[GLT_ZONES[k].off], nb);
            len += nb, nn += nb * 2u;
        }
        h->magic = SMP_USER_MAGIC, h->version = 1, h->nz = 1, h->data_len = len;
        h->zone[0] = GLT_ZONES[0];
        h->zone[0].off = 0, h->zone[0].n = nn, h->zone[0].le = nn - 1u, h->zone[0].hi = 127, h->zone[0].lo = 0;
        smp_user_scan(0);
    }
    for (i = 2; i < (uint32_t)argc; i++) {
        unsigned t, e, p, n;
        if (sscanf(argv[i], "%u:%u:%u:%u", &t, &e, &p, &n) != 4 || t >= NTRK || e >= NENGINES)
            return 2;
        set_engine_of(&trk[t], e);
        apply_preset_to(&trk[t], p);
        if (getenv("GLKIT") && e == ENGI_GLITCH)
            trk[t].p[P_FM3_REL] = 1;
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
