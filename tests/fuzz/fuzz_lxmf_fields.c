#include "reticulum/lxmf_fields.h"

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size <= LXMF_MAX_MESSAGE_SIZE) {
        uint8_t output[512];
        size_t output_length = 0u;
        lxmf_ticket_field_t ticket = {.present = true, .expires_at = 1000u};
        (void)lxmf_fields_merge_ticket(data, size, &ticket, output, sizeof output, &output_length);
        (void)lxmf_fields_merge_ticket(data, size, NULL, output, sizeof output, &output_length);
    }
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
