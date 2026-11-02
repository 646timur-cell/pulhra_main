/* SPDX-License-Identifier: GPL-3.0-only */
/* The actual PULHRA TIMER5 ISR: packet-service deadlines must hold during audio rendering too. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define FM1_TICKS_PER_US 1u
#define UAC_POLL_ALL 1
static uint32_t now, fm1_ms, t5_nested_ticks, input_ticks, polls, packets, uart;
static struct { uint32_t timer_irqs, in_audio, nested; } felucca_dbg;
static uint32_t fm1_ticks(void) { return now; }
static void fm1_timer5_ack(void) {}
static void fm1_input_tick(void) { input_ticks++; }
static void uac_service(void) { packets++; }
static void usb_poll(void) { polls++; uac_service(); }
static void uart_midi_poll(void) { uart++; }
#include "../firmware/src/pulhra/sm_timer.c"
int main(void)
{
    uint32_t i, bad = 0;
    for (i = 0; i < 100; i++) { now += 100; fm1_timer5_irq(); }
    printf("idle: %u packet services, %u full USB polls, %u UART polls\n", packets, polls, uart);
    bad += packets != 100u || polls != 20u || uart != 20u;
    packets = polls = uart = 0;
    felucca_dbg.in_audio = 1;
    for (i = 0; i < 100; i++) { now += 100; fm1_timer5_irq(); }
    printf("rendering: %u packet services, %u full USB polls, %u UART polls\n", packets, polls, uart);
    bad += packets != 100u || polls || uart;
    felucca_dbg.in_audio = 0;
    now += 100; fm1_timer5_irq();
    bad += polls != 1u || uart != 1u || input_ticks != 201u || fm1_ms != 20u;
    printf("pulhra timer: %s\n", bad ? "FAIL" : "ok");
    return bad != 0;
}
