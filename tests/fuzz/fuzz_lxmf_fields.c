#include "reticulum/lxmf_fields.h"

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    lxmf_standard_fields_t fields;
    if (size <= LXMF_MAX_MESSAGE_SIZE &&
        lxmf_standard_fields_parse(data, size, &fields) == LXMF_OK) {
        uint8_t output[512];
        size_t output_length = 0u;
        (void)lxmf_standard_fields_merge(
            data, size, &fields, 0u, fields.present_mask, output,
            sizeof output, &output_length);
    }
    return 0;
}
