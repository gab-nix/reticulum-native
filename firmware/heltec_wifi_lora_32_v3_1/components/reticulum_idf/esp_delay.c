/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "esp_delay.h"

#include <stddef.h>

rns_status_t rns_esp_delay_chunk(uint64_t remaining_ms, uint32_t tick_rate_hz,
                                 uint32_t maximum_ticks, uint32_t *ticks,
                                 uint64_t *consumed_ms) {
    uint64_t maximum_ms;
    uint64_t chunk_ms;
    uint64_t tick_count;
    if (ticks == NULL || consumed_ms == NULL || tick_rate_hz == 0U ||
        maximum_ticks == 0U)
        return RNS_ERROR_INVALID_ARGUMENT;
    *ticks = 0U;
    *consumed_ms = 0U;
    if (remaining_ms == 0U) return RNS_OK;

    maximum_ms = ((uint64_t)maximum_ticks * 1000U) / tick_rate_hz;
    chunk_ms = maximum_ms == 0U ? 1U : maximum_ms;
    if (chunk_ms > remaining_ms) chunk_ms = remaining_ms;
    tick_count = (chunk_ms * tick_rate_hz + 999U) / 1000U;
    if (tick_count == 0U || tick_count > maximum_ticks)
        return RNS_ERROR_OVERFLOW;
    *ticks = (uint32_t)tick_count;
    *consumed_ms = chunk_ms;
    return RNS_OK;
}
