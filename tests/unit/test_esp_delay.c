#include "esp_delay.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

int main(void) {
    uint32_t ticks = 99U;
    uint64_t consumed = 99U;

    assert(rns_esp_delay_chunk(0U, 100U, UINT32_MAX, &ticks, &consumed) ==
           RNS_OK);
    assert(ticks == 0U && consumed == 0U);
    assert(rns_esp_delay_chunk(1U, 100U, UINT32_MAX, &ticks, &consumed) ==
           RNS_OK);
    assert(ticks == 1U && consumed == 1U);
    assert(rns_esp_delay_chunk(1001U, 100U, UINT32_MAX, &ticks, &consumed) ==
           RNS_OK);
    assert(ticks == 101U && consumed == 1001U);

    assert(rns_esp_delay_chunk(UINT64_MAX, 1000U, UINT16_MAX, &ticks,
                               &consumed) == RNS_OK);
    assert(ticks == UINT16_MAX && consumed == UINT16_MAX);
    assert(rns_esp_delay_chunk(1U, 2000U, 1U, &ticks, &consumed) ==
           RNS_ERROR_OVERFLOW);
    assert(rns_esp_delay_chunk(1U, 0U, 1U, &ticks, &consumed) ==
           RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_esp_delay_chunk(1U, 100U, 0U, &ticks, &consumed) ==
           RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_esp_delay_chunk(1U, 100U, 1U, NULL, &consumed) ==
           RNS_ERROR_INVALID_ARGUMENT);
    return 0;
}
