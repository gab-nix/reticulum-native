#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../fixtures/lxmf_paper_vectors.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int main(void) {
    const paper_fixture *fixture = &paper_fixtures[0];
    uint8_t mutated[512];
    assert(fixture->uri_len < sizeof mutated);
    for (size_t length = 0; length <= fixture->uri_len; ++length)
        assert(LLVMFuzzerTestOneInput((const uint8_t *)fixture->uri, length) == 0);
    for (size_t i = 0; i < fixture->uri_len; ++i) {
        memcpy(mutated, fixture->uri, fixture->uri_len);
        mutated[i] ^= (uint8_t)(0x21u + i * 17u);
        assert(LLVMFuzzerTestOneInput(mutated, fixture->uri_len) == 0);
    }
    uint32_t state = 0x7f4a7c15u;
    for (size_t trial = 0; trial < 256u; ++trial) {
        state = state * 1664525u + 1013904223u;
        size_t length = state % sizeof mutated;
        for (size_t i = 0; i < length; ++i) {
            state = state * 1664525u + 1013904223u;
            mutated[i] = (uint8_t)(state >> 24);
        }
        assert(LLVMFuzzerTestOneInput(mutated, length) == 0);
    }
    return 0;
}
