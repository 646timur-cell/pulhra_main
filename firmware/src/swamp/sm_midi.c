/* SPDX-License-Identifier: GPL-3.0-only */
/* SWAMP MACHINE MIDI IN, stage 2: notes. The audio stream drains midi_in_q (usb.c: USB-MIDI 32-bit event packets from the
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
 * cleared, as Felucca's events_block does (the sequencer's notes are not touched: they are its own). */
static uint32_t sm_mn[SM_NTRK][4];                        /* the notes MIDI holds, bit per note 0..127 */

static uint32_t sm_midi_has(uint32_t t, uint32_t note) { return (sm_mn[t][(note >> 5) & 3u] >> (note & 31u)) & 1u; }

static uint32_t sm_seq_has(uint32_t t, uint32_t note)
{
    uint32_t i;
    for (i = 0; i < sm_nheld[t]; i++)
        if (sm_held[t][i] == note)
            return 1;
    return 0;
}

/* track t's MIDI notes forgotten, the core's state not touched (STOP and the panic have let go of every note already) */
static void sm_midi_forget(uint32_t t) { memset(sm_mn[t % SM_NTRK], 0, sizeof sm_mn[0]); }

/* track t's MIDI notes let go (those the sequencer holds too keep sounding: they are its) */
static void sm_midi_release(uint32_t t)
{
    uint32_t note;
    if (!(sm_mn[t][0] | sm_mn[t][1] | sm_mn[t][2] | sm_mn[t][3]))
        return;
    for (note = 0; note < 128u; note++)
        if (sm_midi_has(t, note) && !sm_seq_has(t, note))
            trk_note_off(&trk[t], note);
    sm_midi_forget(t);
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
        if ((mute >> t) & 1u)
            sm_midi_release(t);
    while (mi_r != mi_w) {
        uint32_t pkt = midi_in_q[mi_r % MQ], status = (pkt >> 8) & 0xFFu, hi = status & 0xF0u, ch = status & 0x0Fu;
        uint32_t note = (pkt >> 16) & 0x7Fu, vel = (pkt >> 24) & 0x7Fu;
        RING_PUBLISH();
        mi_r++;
        if ((hi != 0x80u && hi != 0x90u) || (pkt & 0x0Fu) != hi >> 4 || ch >= SM_NTRK)
            continue;                                     /* (a SysEx byte, realtime, CC .., another channel) */
        if (hi == 0x90u && vel) {
            if ((mute >> ch) & 1u)
                continue;
            sm_mn[ch][note >> 5] |= 1u << (note & 31u);
            trk_note_on(&trk[ch], note, vel);
        } else if (sm_midi_has(ch, note)) {
            sm_mn[ch][note >> 5] &= ~(1u << (note & 31u));
            if (!sm_seq_has(ch, note))
                trk_note_off(&trk[ch], note);
        }
    }
}
