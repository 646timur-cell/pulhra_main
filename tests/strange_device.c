/* SPDX-License-Identifier: GPL-3.0-only */
/* A simulated FM-1 running the firmware's editor handler (editor.c, as tests/editor_test.c builds it) for the STRANGE
 * editor's tests (web/test_strange_editor.mjs): one request a line on stdin, in hex (F0 .. F7), its reply a line on
 * stdout (hex, "-" for none). "M st d1 d2" (hex) delivers a MIDI message as the USB port would and runs a few blocks.
 *   cc -O1 -w -DFELUCCA_STRANGE=1 -Ibuild/gen -Ifirmware/src -o build/host/strange_device tests/strange_device.c -lm */
#ifndef FELUCCA_STRANGE
#define FELUCCA_STRANGE 1
#endif
#define FELUCCA_MODULE 1
#define EDITOR_TEST_NO_MAIN 1
#include "editor_test.c"

int main(void)
{
    char line[8192];
    uint32_t t;
    reset();
    for (t = 0; t < NTRK; t++) {                         /* power-on: STRANGE's parts */
        set_engine_of(&trk[t], TRK_DEF[t][0]);
        apply_preset_to(&trk[t], TRK_DEF[t][1]);
        trk[t].engine = trk[t].eng_req;
    }
    setvbuf(stdout, 0, _IOLBF, 0);
    while (fgets(line, sizeof line, stdin)) {
        uint8_t b[4096];
        uint32_t n = 0, i;
        char *p = line;
        if (line[0] == 'M') {
            unsigned st, d1, d2;
            int32_t buf[2 * CTL];
            if (sscanf(line + 1, "%x %x %x", &st, &d1, &d2) == 3)
                midi_event(st & 0xF0u, st & 15u, d1, d2);
            for (i = 0; i < 4u; i++)
                mix_block(buf, CTL);
            printf("ok %d %d %d %d\n", xy_val[0][0], xy_val[0][1], xy_val[1][0], xy_val[1][1]);
            continue;
        }
        while (*p && n < sizeof b) {
            unsigned v;
            int k;
            if (sscanf(p, "%2x%n", &v, &k) == 1) {
                b[n++] = (uint8_t)v;
                p += k;
            } else {
                p++;
            }
        }
        host_wire_n = 0;
        ota_frame_done();
        for (i = 0; i < n; i++)
            sysex_byte(b[i]);
        ed_service();
        host_drain();
        fm1_ms += 5;
        if (!host_wire_n) {
            printf("-\n");
            continue;
        }
        for (i = 0; i < host_wire_n; i++)
            printf("%02x", host_wire[i]);
        printf("\n");
    }
    return 0;
}
