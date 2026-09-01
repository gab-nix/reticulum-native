#include "reticulum/buffer.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

int main(void) {
    static const uint8_t first[] = {0x01U, 0x02U};
    static const uint8_t second[] = {0x03U, 0x04U, 0x05U};
    static const uint8_t expected[] = {0x01U, 0x02U, 0x03U, 0x04U, 0x05U};
    rns_buffer_t buffer;

    rns_buffer_init(&buffer);
    assert(rns_buffer_append(&buffer, first, sizeof(first)) == RNS_OK);
    assert(rns_buffer_append(&buffer, second, sizeof(second)) == RNS_OK);
    assert(buffer.len == sizeof(expected));
    assert(buffer.capacity >= buffer.len);
    assert(memcmp(buffer.data, expected, sizeof(expected)) == 0);
    assert(rns_buffer_resize(&buffer, 8U) == RNS_OK);
    assert(buffer.data[5] == 0U && buffer.data[6] == 0U && buffer.data[7] == 0U);
    rns_buffer_clear(&buffer);
    assert(buffer.data == NULL && buffer.len == 0U && buffer.capacity == 0U);
    return 0;
}

