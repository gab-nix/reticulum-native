/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HELTEC_CPU_USAGE_H
#define HELTEC_CPU_USAGE_H
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    uint32_t previous_total, previous_idle[2];
    uint64_t busy[5], elapsed[5];
    unsigned count, cursor, percent;
    bool initialized, valid;
} heltec_cpu_usage;
/* Same timebase for total wall runtime and each core's idle runtime.
 * Unsigned subtraction handles one 32-bit counter rollover per sample. */
void heltec_cpu_sample(heltec_cpu_usage *state, uint32_t total,
                      const uint32_t idle[2], bool available);
#endif
