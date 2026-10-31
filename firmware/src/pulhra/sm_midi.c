/* SPDX-License-Identifier: GPL-3.0-only */
/* PULHRA MIDI IN, stage 2: notes. The audio stream drains midi_in_q (usb.c: USB-MIDI 32-bit event packets from the
 * USB poll and, as the same packets, from the TRS UART, midi_uart.c) at the start of events_block, right after the commands:
 * the channels 1..4 are the tracks T1..T4, a note-on plays on the track, a note-off (or a note-on at velocity 0) lets it go.
 * Everything else (control change, program change, bend, other channels, clock and the realtime bytes, SysEx) is read and
 * dropped: CC and MIDI clock are a later stage.
 *
 * The sequencer and MIDI share the track's voices, and the core takes a note-off by note number only, so a note-off of
 * either would end the other's note of the same pitch. Each side therefore keeps its own book: the sequencer's notes are
 * sm_held (sm_seq.c), MIDI's a bit per note per track (sm_mn); a side lets go of a note on the core only when the other side
 * does not hold it too (sm_seq_release asks sm_midi_has, a MIDI note-off asks sm_seq_has). A MIDI note-off for a note MIDI
 * does not hold (it came while the track was muted, or after STOP) is dropped: it must never end the sequencer's note.
 * A muted track (smp.mute, or another track soloed: sm_mute_eff, read once per block) starts no MIDI note, and lets go of
 * those it holds at the next block, as the sequencer does. STOP, the panic and a queue overflow release every MIDI note.
 * The queue: one consumer (this), one producer (usb_poll / the UART, the timer interrupt); an overflow flag the producer sets
 * (midi_enqueue stops taking packets while it is set) is answered here: the queue dropped, MIDI's notes released, the flag
 * cleared, as Felucca's events_block does (the sequencer's notes are not touched: they are its own).
 * The keyboard mode (sm_input.c: OCT- + OCT+) plays through the same mechanism: its keys arrive as SMC_KEYON / SMC_KEYOFF
 * (sm_cmd.c) and have a third book, sm_kn, handled like MIDI's (sm_kbd_note): the three sides never end one another's note,
 * a muted track starts no keyboard note and lets go of those it holds, STOP and the panic forget them.
 * A track with an arpeggiator (MODE not OFF, sm_arp.c) takes its MIDI notes as the arp's input (sm_arp_midi_on / off) instead of playing
 * them; the arp keeps a fourth book (sm_arp_has), which the other sides leave alone and which leaves theirs alone. */
static uint32_t sm_mn[SM_NTRK][4];                        /* the notes MIDI holds, bit per note 0..127 */
static uint32_t sm_kn[SM_NTRK][4];                        /* the notes the keyboard mode holds (its own book: SMC_KEYON / KEYOFF) */
/* the MIDI page and the SETUP page (sm_input.c writes them, the drain and the screen read them): the channel each track
 * listens on (0 OFF: no external MIDI on any channel, 1..16 the channel) and the source to take (0 USB the port alone,
 * 1 INT both merged, 2 JACK the TRS jack alone). The defaults are the stage-2 routing: channel 1..4 -> T1..T4, both
 * inputs (a TRS-only rig works out of the box). */
static uint8_t sm_midi_ch[SM_NTRK] = {1, 2, 3, 4};
static uint8_t sm_midi_src = 1;
/* the SETUP page (SHIFT + HOME): K1 CLK, the clock's source: AUTO (the default: the jack when SRC is JACK, else the panel), INT
 * (the panel's tempo), USB, JACK: from that port MIDI clock is the tempo and the steps' phase (sm_ext_pulse); K3 TRNS, the
 * transport in: ON (the default) Start / Continue / Stop from the clock's port play and stop the sequencer, OFF not. The clock
 * and the transport come from their port whatever SRC (the notes' source) lets in. The project saves both (sm_bank.c, version
 * 3's byte after the source: CLK in bits 0..1, TRNS OFF bit 2; 0 = AUTO, ON) */
enum { SM_CLK_AUTO, SM_CLK_INT, SM_CLK_USB, SM_CLK_JACK, SM_CLK_COUNT };
static uint8_t sm_midi_clk = SM_CLK_AUTO, sm_midi_trnoff;
static uint8_t sm_midi_div, sm_ext_odd;                    /* SETUP's SELECT: the clock's every 2nd pulse only (Tim: KO II's THRU doubled it); the parity */
/* the CLK choices SETUP offers (Tim: no external clock of a port not in use): AUTO, INT, and the port MIDI SRC is (USB, the
 * jack); SRC INT: AUTO and INT only */
static int sm_midi_clk_ok(uint32_t c)
{
    return c == SM_CLK_AUTO || c == SM_CLK_INT || (c == SM_CLK_USB && sm_midi_src == 0u) || (c == SM_CLK_JACK && sm_midi_src == 2u);
}
static uint32_t sm_midi_clk_port(void)                   /* the port of the external clock: 1 USB, 2 the jack, 0 none (INT) */
{
    uint32_t c = sm_midi_clk == SM_CLK_AUTO ? (sm_midi_src == 2u ? SM_CLK_JACK : SM_CLK_INT) : sm_midi_clk;
    if (!sm_midi_clk_ok(c))
        c = SM_CLK_INT;
    return c == SM_CLK_USB ? 1u : c == SM_CLK_JACK ? 2u : 0u;
}
static volatile uint8_t sm_midi_ch_release[SM_NTRK];     /* a track's channel changed (sm_input.c sets its byte, the drain
                                                             clears it: a byte each side, no read-modify-write between the
                                                             main loop and the audio stream); the drain lets its MIDI notes go
                                                             once (a note-off on the old channel would not reach it anymore) */

static uint32_t sm_mn_has(uint32_t t, uint32_t note) { return (sm_mn[t][(note >> 5) & 3u] >> (note & 31u)) & 1u; }
static uint32_t sm_kn_has(uint32_t t, uint32_t note) { return (sm_kn[t][(note >> 5) & 3u] >> (note & 31u)) & 1u; }
/* a note some side other than the sequencer holds (MIDI IN or the keyboard): the sequencer lets go of its own only without it */
static uint32_t sm_midi_has(uint32_t t, uint32_t note) { return sm_mn_has(t, note) | sm_kn_has(t, note) | sm_arp_has(t, note); }

static uint32_t sm_seq_has(uint32_t t, uint32_t note)
{
    uint32_t i;
    for (i = 0; i < sm_nheld[t]; i++)
        if (sm_held[t][i] == note)
            return 1;
    return 0;
}

/* track t's MIDI and keyboard notes forgotten, the core's state not touched (STOP and the panic have let go of every note already) */
static void sm_midi_forget(uint32_t t)
{
    memset(sm_mn[t % SM_NTRK], 0, sizeof sm_mn[0]);
    memset(sm_kn[t % SM_NTRK], 0, sizeof sm_kn[0]);
}

/* track t's MIDI notes let go (those the sequencer or the keyboard holds too keep sounding: they are theirs) */
static void sm_midi_release(uint32_t t)
{
    uint32_t note;
    if (!(sm_mn[t][0] | sm_mn[t][1] | sm_mn[t][2] | sm_mn[t][3]))
        return;
    for (note = 0; note < 128u; note++)
        if (sm_mn_has(t, note) && !sm_seq_has(t, note) && !sm_kn_has(t, note) && !sm_arp_has(t, note))
            trk_note_off(&trk[t], note);
    memset(sm_mn[t], 0, sizeof sm_mn[0]);
}

/* a track's MIDI channel changed (sm_input.c, the MIDI page): the drain lets its MIDI notes go once */
static void sm_midi_release_req(uint32_t t)
{
    sm_midi_ch_release[t % SM_NTRK] = 1u;
}

/* track t's keyboard notes let go, the same way (a muted track) */
static void sm_kbd_release(uint32_t t)
{
    uint32_t note;
    if (!(sm_kn[t][0] | sm_kn[t][1] | sm_kn[t][2] | sm_kn[t][3]))
        return;
    for (note = 0; note < 128u; note++)
        if (sm_kn_has(t, note) && !sm_seq_has(t, note) && !sm_mn_has(t, note) && !sm_arp_has(t, note))
            trk_note_off(&trk[t], note);
    memset(sm_kn[t], 0, sizeof sm_kn[0]);
}

/* a key of the keyboard mode (SMC_KEYON / KEYOFF, the audio stream's side): vel > 0 plays note on track t unless the track is
 * muted (then it is not booked either, and its note-off is dropped); vel 0 lets it go if the keyboard holds it, and on the
 * core only if neither the sequencer nor MIDI holds it too. A note-off for a note the keyboard does not hold ends nothing */
static void sm_kbd_note(uint32_t t, uint32_t note, uint32_t vel)
{
    if (t >= SM_NTRK)
        return;
    note &= 0x7Fu;
    if (vel) {
        if ((sm_mute_eff() >> t) & 1u)
            return;
        sm_kn[t][note >> 5] |= 1u << (note & 31u);
        trk_note_on(&trk[t], note, vel > 127u ? 127u : vel);
    } else if (sm_kn_has(t, note)) {
        sm_kn[t][note >> 5] &= ~(1u << (note & 31u));
        if (!sm_seq_has(t, note) && !sm_mn_has(t, note) && !sm_arp_has(t, note))
            trk_note_off(&trk[t], note);
    }
}

/* the external clock (24 pulses a beat, 6 a 1/16): the tempo from the time of a whole beat of pulses (the stamps are the
 * USB poll's / the UART's milliseconds: a beat averages their jitter, then a quarter of each new estimate), published for
 * sm_bpm_eff; and the phase: on the pulse that starts a step of the pattern's grid (6 x 8 / m pulses: *, /2 .. /8 and *2;
 * *4 and *8 fall between pulses: tempo only), the step due within a quarter step is started now, one started up to a quarter
 * step early has its next start moved to a whole step from now. A gap of SM_EXT_TIMEOUT_MS starts the measuring over */
static uint32_t sm_ext_ts[25], sm_ext_n, sm_ext_cnt;
#define SM_EXT_DUP_MS 3u                                  /* (a pulse is 8.3 ms apart at 300 BPM: closer on the jack is a copy) */
static void sm_ext_pulse(uint32_t ms, uint32_t src)
{
    uint32_t m8, per, step_q8, q;
    if (src == 2u && sm_ext_n && (uint32_t)(ms - sm_ext_ts[(sm_ext_n - 1u) % 25u]) < SM_EXT_DUP_MS)
        return;                                           /* (Tim: KO II's MIDI THRU sent each pulse twice, its own and the one
                                                             through: 240 BPM for 120. A copy within 3 ms is dropped) */
    if (sm_midi_div && (sm_ext_odd ^= 1u) == 0u)
        return;                                           /* (DIV 1:2: every second pulse, the first after Start counted) */
    if (sm_ext_n && (uint32_t)(ms - sm_ext_ts[(sm_ext_n - 1u) % 25u]) >= SM_EXT_TIMEOUT_MS)
        sm_ext_n = 0;                                     /* (a gap: measure anew) */
    sm_ext_ts[sm_ext_n % 25u] = ms;
    sm_ext_n++;
    if (sm_ext_n > 24u) {
        uint32_t dt = ms - sm_ext_ts[(sm_ext_n - 25u) % 25u];   /* (a beat: 24 pulses ago) */
        if (dt >= 200u && dt <= 2000u) {                  /* (30 .. 300 BPM) */
            uint32_t b = (6000000u / dt + 5u) / 10u, e = sm_ext_bpm10;
            sm_ext_bpm10 = (uint16_t)(e ? (e * 3u + b + 2u) / 4u : b);
        }
    }
    sm_ext_last_ms = ms;
    if (!sm_playing)
        return;
    q = sm_ext_cnt++;                                     /* (the first pulse after Start is the downbeat: pulse 0) */
    (void)m8;                                             /* (the grid's 1x step: six pulses; the tracks' speeds ride its ticks) */
    if (!sm_ext_bpm10)
        return;
    per = 6u;
    if (q % per)
        return;
    step_q8 = sm_tick_q8();                               /* (the tick: the grid's 1x step is eight of them) */
    q = step_q8 / 4u;
    if (sm_tick & 7u) {                                   /* (not on a 1x step's boundary: the pulse is early or late by a tick) */
        if ((sm_tick & 7u) == 1u && sm_next_q8 >= step_q8 - q && sm_next_q8 < step_q8)
            sm_next_q8 = step_q8;                         /* (the boundary's tick a little early: the next a whole tick on) */
        return;
    }

    if (sm_next_q8 <= q)
        sm_next_q8 = 0;                                   /* (late: the step starts now) */
    else if (sm_next_q8 >= step_q8 - q && sm_next_q8 < step_q8)
        sm_next_q8 = step_q8;                             /* (early: its next a whole step from this pulse) */
}

static void sm_midi_drain(void)
{
    uint32_t t, mute = sm_mute_eff();
    if (midi_in_overflow) {                               /* a lost note-off must never leave a held note */
        mi_r = mi_w;
        for (t = 0; t < SM_NTRK; t++)
            sm_midi_release(t);
        RING_PUBLISH();
        midi_in_overflow = 0;
    }
    for (t = 0; t < SM_NTRK; t++)
        if ((mute >> t) & 1u) {
            sm_midi_release(t);
            sm_kbd_release(t);
        }
    for (t = 0; t < SM_NTRK; t++)                         /* a track's channel changed: its MIDI notes let go */
        if (sm_midi_ch_release[t]) {
            sm_midi_release(t);
            sm_midi_ch_release[t] = 0;
        }
    while (mi_r != mi_w) {
        uint32_t at = mi_r % MQ, pkt = midi_in_q[at];
        uint32_t src = midi_in_source[at] ? midi_in_source[at] : 1u;   /* (0: unknown, as USB, as seq.c reads it) */
        uint32_t status = (pkt >> 8) & 0xFFu, hi = status & 0xF0u, chan = (status & 0x0Fu) + 1u;
        uint32_t note = (pkt >> 16) & 0x7Fu, vel = (pkt >> 24) & 0x7Fu, ch;
        RING_PUBLISH();
        mi_r++;
        if ((pkt & 0x0Fu) == 0x0Fu && status == 0xF8u) {  /* MIDI clock from the clock's port: the tempo, the steps' phase */
            if (src == sm_midi_clk_port())
                sm_ext_pulse(midi_in_ms[at], src);
            continue;
        }
        if ((pkt & 0x0Fu) == 0x0Fu && status >= 0xFAu && status <= 0xFCu) {   /* realtime from the clock's port: the transport in */
            if (src == sm_midi_clk_port() && !sm_midi_trnoff) {
                if (status == 0xFCu) {
                    sm_seq_stop();
                } else {
                    if (status == 0xFAu && sm_playing)
                        sm_seq_stop();                    /* (Start: from the top, playing or not; Continue: on if stopped) */
                    sm_seq_play();
                    sm_ext_cnt = 0;                       /* (the pulses count the grid from here) */
                    sm_ext_odd = 0;
                }
            }
            continue;
        }
        /* the SETUP page's MIDI source (the notes): USB (0) the USB port alone, INT (1) both merged, JACK (2) the TRS jack alone */
        if ((sm_midi_src == 0u && src != 1u) || (sm_midi_src == 2u && src != 2u))
            continue;
        if ((hi != 0x80u && hi != 0x90u) || (pkt & 0x0Fu) != hi >> 4)
            continue;                                     /* (a SysEx byte, realtime, CC ..) */
        for (ch = 0; ch < SM_NTRK; ch++) {                /* the MIDI page: every track on the channel plays it (a layer) */
            if (sm_midi_ch[ch] != chan)
                continue;
            if (hi == 0x90u && vel) {
                if (sm_arp_mode(ch)) {                    /* (an arpeggiator on the track: the note is its input, it records its own) */
                    if (!((mute >> ch) & 1u))
                        sm_arp_midi_on(ch, note, vel);
                    continue;
                }
                if (ch == sm_rec_mtrk)                    /* (REC on, the selected track's channel: recorded while playing) */
                    sm_rec_on(ch, note, vel);
                if ((mute >> ch) & 1u)
                    continue;
                sm_mn[ch][note >> 5] |= 1u << (note & 31u);
                trk_note_on(&trk[ch], note, vel);
                continue;
            }
            if (sm_arp_midi_off(ch, note))
                continue;
            sm_rec_off(ch, note);
            if (sm_mn_has(ch, note)) {
                sm_mn[ch][note >> 5] &= ~(1u << (note & 31u));
                if (!sm_seq_has(ch, note) && !sm_kn_has(ch, note) && !sm_arp_has(ch, note))
                    trk_note_off(&trk[ch], note);
            }
        }
    }
}
