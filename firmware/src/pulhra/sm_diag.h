/* SPDX-License-Identifier: GPL-3.0-only */
/* Read-only audio diagnostic snapshot, version 1. Values are unsigned 32-bit. */
#pragma once
enum {
    SM_D_TICKS, SM_D_HALVES, SM_D_LATE, SM_D_MAX_US, SM_D_LAST_US,
    SM_D_CPU_Q8, SM_D_TIMER_IRQS, SM_D_PACKETS, SM_D_FRAMES,
    SM_D_RING_WRITE, SM_D_RING_READ, SM_D_FILL, SM_D_UNDERRUNS,
    SM_D_OVERRUNS, SM_D_MISSED, SM_D_STALLS, SM_D_ADJ_UP,
    SM_D_ADJ_DOWN, SM_D_STARTS, SM_D_HALF_FRAMES, SM_D_TICKS_PER_US,
    SM_D_COUNT
};
