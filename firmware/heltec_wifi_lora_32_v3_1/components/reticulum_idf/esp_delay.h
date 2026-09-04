#ifndef RETICULUM_ESP_DELAY_H
#define RETICULUM_ESP_DELAY_H

#include <stdint.h>

#include "reticulum/status.h"

/* Return one bounded delay chunk. The conversion rounds up so a non-zero
 * millisecond request never silently becomes a zero-tick delay. */
rns_status_t rns_esp_delay_chunk(uint64_t remaining_ms, uint32_t tick_rate_hz,
                                 uint32_t maximum_ticks, uint32_t *ticks,
                                 uint64_t *consumed_ms);

#endif
