/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cpu_usage.h"
#include <string.h>
void heltec_cpu_sample(heltec_cpu_usage *s, uint32_t total,
                      const uint32_t idle[2], bool available) {
    if (!available) { memset(s, 0, sizeof(*s)); return; }
    uint32_t elapsed = total-s->previous_total;
    uint64_t idle_sum = (uint64_t)(uint32_t)(idle[0]-s->previous_idle[0]) +
                        (uint32_t)(idle[1]-s->previous_idle[1]);
    bool baseline = s->initialized;
    s->initialized = true; s->previous_total = total;
    s->previous_idle[0] = idle[0]; s->previous_idle[1] = idle[1];
    if (!baseline) return;
    /* Reject stale, implausible or discontinuous snapshots, not fabricated 0%. */
    if (!elapsed || elapsed > 10000000U || idle_sum > (uint64_t)elapsed*2U) {
        s->valid = false; s->count = 0; s->cursor = 0; return;
    }
    s->elapsed[s->cursor] = (uint64_t)elapsed*2U;
    s->busy[s->cursor] = (uint64_t)elapsed*2U-idle_sum;
    s->cursor = (s->cursor+1U)%5U;
    if (s->count < 5U) ++s->count;
    uint64_t busy = 0, duration = 0;
    for (unsigned i = 0; i < s->count; ++i) { busy += s->busy[i]; duration += s->elapsed[i]; }
    s->percent = (unsigned)((busy*100U+duration/2U)/duration);
    s->valid = s->count == 5U;
}
