#include "reticulum/storage_record.h"

#include <string.h>

#define RNS_STORAGE_KEY_MAX 64U
#define RECORD_MAGIC_0 ((uint8_t)'R')
#define RECORD_MAGIC_1 ((uint8_t)'N')
#define RECORD_MAGIC_2 ((uint8_t)'S')
#define RECORD_MAGIC_3 ((uint8_t)'N')

static void put_u16(uint8_t *output, uint16_t value) {
    output[0] = (uint8_t)(value >> 8U);
    output[1] = (uint8_t)value;
}

static void put_u32(uint8_t *output, uint32_t value) {
    output[0] = (uint8_t)(value >> 24U);
    output[1] = (uint8_t)(value >> 16U);
    output[2] = (uint8_t)(value >> 8U);
    output[3] = (uint8_t)value;
}

static uint16_t get_u16(const uint8_t *input) {
    return (uint16_t)(((uint16_t)input[0] << 8U) | input[1]);
}

static uint32_t get_u32(const uint8_t *input) {
    return ((uint32_t)input[0] << 24U) | ((uint32_t)input[1] << 16U) |
           ((uint32_t)input[2] << 8U) | (uint32_t)input[3];
}

static int valid_key(const char *key, size_t *length) {
    size_t value = 0U;
    if (key == NULL) return 0;
    while (value <= RNS_STORAGE_KEY_MAX && key[value] != '\0') value++;
    if (value == 0U || value > RNS_STORAGE_KEY_MAX) return 0;
    if (length != NULL) *length = value;
    return 1;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t length) {
    size_t offset;
    for (offset = 0U; offset < length; offset++) {
        unsigned bit;
        crc ^= data[offset];
        for (bit = 0U; bit < 8U; bit++) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return crc;
}

static uint32_t key_tag(const char *key, size_t length) {
    return ~crc32_update(UINT32_MAX, (const uint8_t *)key, length);
}

rns_status_t rns_storage_record_encoded_size(size_t payload_length,
                                             size_t *encoded_length) {
    if (encoded_length == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    *encoded_length = 0U;
    if (payload_length > RNS_STORAGE_RECORD_MAX_PAYLOAD)
        return RNS_ERROR_OVERFLOW;
    *encoded_length = RNS_STORAGE_RECORD_HEADER_SIZE + payload_length;
    return RNS_OK;
}

rns_status_t rns_storage_record_encode(const char *key, uint32_t generation,
                                       const uint8_t *payload,
                                       size_t payload_length,
                                       uint8_t *output, size_t capacity,
                                       size_t *encoded_length) {
    size_t key_length;
    size_t required;
    uint32_t checksum;
    rns_status_t status;
    if (encoded_length == NULL || !valid_key(key, &key_length) || generation == 0U ||
        (payload == NULL && payload_length != 0U) || output == NULL)
        return RNS_ERROR_INVALID_ARGUMENT;
    status = rns_storage_record_encoded_size(payload_length, &required);
    if (status != RNS_OK) return status;
    *encoded_length = required;
    if (capacity < required) return RNS_ERROR_OVERFLOW;

    output[0] = RECORD_MAGIC_0;
    output[1] = RECORD_MAGIC_1;
    output[2] = RECORD_MAGIC_2;
    output[3] = RECORD_MAGIC_3;
    put_u16(output + 4U, RNS_STORAGE_RECORD_VERSION);
    put_u16(output + 6U, RNS_STORAGE_RECORD_HEADER_SIZE);
    put_u32(output + 8U, generation);
    put_u32(output + 12U, key_tag(key, key_length));
    put_u32(output + 16U, (uint32_t)payload_length);
    put_u32(output + 20U, 0U);
    if (payload_length != 0U)
        memcpy(output + RNS_STORAGE_RECORD_HEADER_SIZE, payload, payload_length);
    checksum = crc32_update(UINT32_MAX, output, 20U);
    checksum = crc32_update(checksum, output + RNS_STORAGE_RECORD_HEADER_SIZE,
                            payload_length);
    put_u32(output + 20U, ~checksum);
    return RNS_OK;
}

rns_status_t rns_storage_record_decode(const char *key, const uint8_t *record,
                                       size_t record_length, uint8_t *output,
                                       size_t capacity, size_t *payload_length,
                                       uint32_t *generation) {
    size_t key_length;
    size_t length;
    uint32_t checksum;
    if (payload_length == NULL || generation == NULL ||
        !valid_key(key, &key_length) || record == NULL ||
        (output == NULL && capacity != 0U)) return RNS_ERROR_INVALID_ARGUMENT;
    *payload_length = 0U;
    *generation = 0U;
    if (record_length < RNS_STORAGE_RECORD_HEADER_SIZE ||
        record[0] != RECORD_MAGIC_0 || record[1] != RECORD_MAGIC_1 ||
        record[2] != RECORD_MAGIC_2 || record[3] != RECORD_MAGIC_3 ||
        get_u16(record + 4U) != RNS_STORAGE_RECORD_VERSION ||
        get_u16(record + 6U) != RNS_STORAGE_RECORD_HEADER_SIZE ||
        get_u32(record + 12U) != key_tag(key, key_length))
        return RNS_ERROR_PROTOCOL;
    length = get_u32(record + 16U);
    if (length > RNS_STORAGE_RECORD_MAX_PAYLOAD ||
        record_length != RNS_STORAGE_RECORD_HEADER_SIZE + length)
        return RNS_ERROR_PROTOCOL;
    checksum = crc32_update(UINT32_MAX, record, 20U);
    checksum = crc32_update(checksum, record + RNS_STORAGE_RECORD_HEADER_SIZE, length);
    if ((~checksum) != get_u32(record + 20U)) return RNS_ERROR_PROTOCOL;
    *generation = get_u32(record + 8U);
    *payload_length = length;
    if (*generation == 0U) return RNS_ERROR_PROTOCOL;
    if (capacity < length || (length != 0U && output == NULL))
        return RNS_ERROR_OVERFLOW;
    if (length != 0U) memcpy(output, record + RNS_STORAGE_RECORD_HEADER_SIZE, length);
    return RNS_OK;
}

rns_status_t rns_storage_record_select_slot(int valid_a, uint32_t generation_a,
                                            int valid_b, uint32_t generation_b,
                                            char *selected_slot) {
    if (selected_slot == NULL || (valid_a != 0 && generation_a == 0U) ||
        (valid_b != 0 && generation_b == 0U)) return RNS_ERROR_INVALID_ARGUMENT;
    *selected_slot = '\0';
    if (valid_a == 0 && valid_b == 0) return RNS_ERROR_NOT_FOUND;
    *selected_slot = valid_b == 0 ||
                     (valid_a != 0 && generation_a >= generation_b) ? 'a' : 'b';
    return RNS_OK;
}

rns_status_t rns_storage_record_next_slot(int valid_a, uint32_t generation_a,
                                          int valid_b, uint32_t generation_b,
                                          char *write_slot,
                                          uint32_t *next_generation) {
    uint32_t newest;
    if (write_slot == NULL || next_generation == NULL ||
        (valid_a != 0 && generation_a == 0U) ||
        (valid_b != 0 && generation_b == 0U)) return RNS_ERROR_INVALID_ARGUMENT;
    if (valid_a == 0 && valid_b == 0) {
        *write_slot = 'a';
        *next_generation = 1U;
        return RNS_OK;
    }
    newest = valid_b == 0 || (valid_a != 0 && generation_a >= generation_b)
                 ? generation_a : generation_b;
    if (newest == UINT32_MAX) return RNS_ERROR_INVALID_STATE;
    *write_slot = valid_a == 0 ? 'a' : (valid_b == 0 ? 'b' :
                  (generation_a <= generation_b ? 'a' : 'b'));
    *next_generation = newest + 1U;
    return RNS_OK;
}
