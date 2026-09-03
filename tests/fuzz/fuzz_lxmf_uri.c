#include "reticulum/lxmf_paper.h"
#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    uint8_t paper[LXMF_PAPER_MAX_SIZE], transient[32];
    size_t paper_length;
    if (size <= LXMF_URI_MAX_INPUT_LENGTH)
        (void)lxmf_uri_decode((const char *)data, size, paper, sizeof paper,
                              &paper_length, transient);
    return 0;
}
