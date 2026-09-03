#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../fixtures/lxmf_standard_fields_fixture.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int main(void) {
    uint8_t mutated[512];
    for (size_t length = 0u;
         length <= sizeof lxmf_standard_fields_fixture; ++length)
        assert(LLVMFuzzerTestOneInput(lxmf_standard_fields_fixture, length) ==
               0);
    for (size_t i = 0u; i < sizeof lxmf_standard_fields_fixture; ++i) {
        memcpy(mutated, lxmf_standard_fields_fixture,
               sizeof lxmf_standard_fields_fixture);
        mutated[i] ^= (uint8_t)(0x31u + i * 13u);
        assert(LLVMFuzzerTestOneInput(
                   mutated, sizeof lxmf_standard_fields_fixture) == 0);
    }
    uint32_t state = UINT32_C(0x26d50f49);
    for (size_t trial = 0u; trial < 512u; ++trial) {
        state = state * UINT32_C(1664525) + UINT32_C(1013904223);
        size_t length = state % sizeof mutated;
        for (size_t i = 0u; i < length; ++i) {
            state = state * UINT32_C(1664525) + UINT32_C(1013904223);
            mutated[i] = (uint8_t)(state >> 24u);
        }
        assert(LLVMFuzzerTestOneInput(mutated, length) == 0);
    }
    return 0;
}
