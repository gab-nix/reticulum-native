/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cpu_usage.h"
#include <assert.h>
int main(void) {
    heltec_cpu_usage s = {0}; uint32_t idle[2] = {0};
    heltec_cpu_sample(&s, 0, idle, true);
    for (unsigned i = 1; i <= 5; ++i) {
        idle[0] = i*1000000U; idle[1] = 0;
        heltec_cpu_sample(&s, i*1000000U, idle, true);
    }
    assert(s.valid && s.percent == 50U);
    heltec_cpu_sample(&s, 5000000U, idle, true); assert(!s.valid);
    heltec_cpu_sample(&s, 0, idle, false); assert(!s.initialized);
    idle[0] = idle[1] = UINT32_MAX-500000U;
    heltec_cpu_sample(&s, UINT32_MAX-500000U, idle, true);
    idle[0] += 1000000U; idle[1] += 1000000U;
    heltec_cpu_sample(&s, 499999U, idle, true);
    assert(s.count == 1U && s.percent == 0U);
    heltec_cpu_sample(&s, 0, idle, false);
    idle[0] = idle[1] = 0;
    heltec_cpu_sample(&s, 0, idle, true);
    for (unsigned i = 1; i <= 5; ++i) heltec_cpu_sample(&s, i*1000000U, idle, true);
    assert(s.valid && s.percent == 100U);
    idle[0] = idle[1] = 9000000U;
    heltec_cpu_sample(&s, 6000000U, idle, true);
    assert(!s.valid && s.count == 0U);
    return 0;
}
