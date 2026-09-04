#include "reticulum/storage_record.h"

#include <assert.h>
#include <string.h>

int main(void) {
    static const uint8_t payload[] = {0x00U, 0x01U, 0x7fU, 0x80U, 0xffU};
    uint8_t record[RNS_STORAGE_RECORD_HEADER_SIZE + sizeof(payload)];
    uint8_t decoded[sizeof(payload)];
    size_t encoded_length = 0U;
    size_t decoded_length = 0U;
    uint32_t generation = 0U;
    uint32_t next_generation = 0U;
    char slot = '\0';

    assert(rns_storage_record_encoded_size(sizeof(payload), &encoded_length) == RNS_OK);
    assert(encoded_length == sizeof(record));
    assert(rns_storage_record_encode("identity", 42U, payload, sizeof(payload),
                                     record, sizeof(record), &encoded_length) == RNS_OK);
    assert(record[0] == 'R' && record[1] == 'N' &&
           record[2] == 'S' && record[3] == 'N');
    assert(record[8] == 0U && record[9] == 0U &&
           record[10] == 0U && record[11] == 42U);
    assert(record[12] == 0x6aU && record[13] == 0x95U &&
           record[14] == 0xe9U && record[15] == 0xc4U);
    assert(record[20] == 0xa2U && record[21] == 0xb9U &&
           record[22] == 0x46U && record[23] == 0xf9U);
    assert(rns_storage_record_decode("identity", record, encoded_length,
                                     decoded, sizeof(decoded), &decoded_length,
                                     &generation) == RNS_OK);
    assert(generation == 42U && decoded_length == sizeof(payload));
    assert(memcmp(payload, decoded, sizeof(payload)) == 0);

    assert(rns_storage_record_decode("config", record, encoded_length,
                                     decoded, sizeof(decoded), &decoded_length,
                                     &generation) == RNS_ERROR_PROTOCOL);
    record[sizeof(record) - 1U] ^= 0x01U;
    assert(rns_storage_record_decode("identity", record, encoded_length,
                                     decoded, sizeof(decoded), &decoded_length,
                                     &generation) == RNS_ERROR_PROTOCOL);
    record[sizeof(record) - 1U] ^= 0x01U;
    assert(rns_storage_record_decode("identity", record, encoded_length,
                                     NULL, 0U, &decoded_length,
                                     &generation) == RNS_ERROR_OVERFLOW);
    assert(decoded_length == sizeof(payload) && generation == 42U);
    assert(rns_storage_record_decode("identity", record, encoded_length - 1U,
                                     decoded, sizeof(decoded), &decoded_length,
                                     &generation) == RNS_ERROR_PROTOCOL);
    assert(rns_storage_record_encode("identity", 0U, payload, sizeof(payload),
                                     record, sizeof(record), &encoded_length) ==
           RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_storage_record_encode("identity", 1U, payload, sizeof(payload),
                                     record, sizeof(record) - 1U, &encoded_length) ==
           RNS_ERROR_OVERFLOW);
    assert(encoded_length == sizeof(record));
    assert(rns_storage_record_encoded_size(RNS_STORAGE_RECORD_MAX_PAYLOAD + 1U,
                                           &encoded_length) == RNS_ERROR_OVERFLOW);

    assert(rns_storage_record_select_slot(1, 8U, 1, 9U, &slot) == RNS_OK &&
           slot == 'b');
    assert(rns_storage_record_select_slot(1, 8U, 0, 0U, &slot) == RNS_OK &&
           slot == 'a');
    assert(rns_storage_record_select_slot(0, 0U, 0, 0U, &slot) ==
           RNS_ERROR_NOT_FOUND);
    assert(rns_storage_record_next_slot(0, 0U, 0, 0U, &slot,
                                        &next_generation) == RNS_OK &&
           slot == 'a' && next_generation == 1U);
    assert(rns_storage_record_next_slot(1, 8U, 1, 9U, &slot,
                                        &next_generation) == RNS_OK &&
           slot == 'a' && next_generation == 10U);
    assert(rns_storage_record_next_slot(1, UINT32_MAX, 0, 0U, &slot,
                                        &next_generation) ==
           RNS_ERROR_INVALID_STATE);
    return 0;
}
