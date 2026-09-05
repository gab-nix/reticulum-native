#include "reticulum/path_store.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define PATH_STORE_HEADER_SIZE 32u
#define PATH_STORE_RECORD_BASE_SIZE 104u
#define PATH_STORE_RECORD_PREFIX_SIZE 2u
#define PATH_STORE_VERSION 2u
#define PATH_STORE_IDENTITY_SIZE 64u

static const uint8_t path_store_magic[8] = {'R', 'N', 'S', 'P', 'A', 'T', 'H', '1'};

static void put16(uint8_t *output, uint16_t value) {
    output[0] = (uint8_t)(value >> 8u);
    output[1] = (uint8_t)value;
}

static void put32(uint8_t *output, uint32_t value) {
    output[0] = (uint8_t)(value >> 24u);
    output[1] = (uint8_t)(value >> 16u);
    output[2] = (uint8_t)(value >> 8u);
    output[3] = (uint8_t)value;
}

static void put64(uint8_t *output, uint64_t value) {
    for (size_t i = 0u; i < 8u; ++i)
        output[i] = (uint8_t)(value >> (56u - 8u * i));
}

static uint16_t get16(const uint8_t *input) {
    return (uint16_t)(((uint16_t)input[0] << 8u) | input[1]);
}

static uint32_t get32(const uint8_t *input) {
    return ((uint32_t)input[0] << 24u) | ((uint32_t)input[1] << 16u) |
           ((uint32_t)input[2] << 8u) | input[3];
}

static uint64_t get64(const uint8_t *input) {
    uint64_t value = 0u;
    for (size_t i = 0u; i < 8u; ++i) value = (value << 8u) | input[i];
    return value;
}

static uint32_t crc32_bytes(const uint8_t *data, size_t length) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0u; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0u; bit < 8u; ++bit)
            crc = (crc >> 1u) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
    return ~crc;
}

static uint64_t blob_timebase(const uint8_t blob[RNS_TRANSPORT_RANDOM_BLOB_SIZE]) {
    uint64_t value = 0u;
    for (size_t i = 5u; i < RNS_TRANSPORT_RANDOM_BLOB_SIZE; ++i)
        value = (value << 8u) | blob[i];
    return value;
}

static bool seconds_to_ms(double seconds, uint64_t *milliseconds) {
    if (!isfinite(seconds) || seconds < 0.0 ||
        seconds > (double)UINT64_MAX / 1000.0) return false;
    *milliseconds = (uint64_t)(seconds * 1000.0);
    return true;
}

rns_status_t rns_path_store_encode(const rns_transport *transport,
                                   uint64_t wall_time_ms,
                                   uint8_t *output, size_t output_capacity,
                                   size_t *output_length,
                                   size_t *encoded_count) {
    if (output_length == NULL || encoded_count == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    *output_length = 0u;
    *encoded_count = 0u;
    if (transport == NULL || transport->paths == NULL ||
        transport->config.clock == NULL ||
        (output == NULL && output_capacity != 0u)) return RNS_ERROR_INVALID_ARGUMENT;
    double now = transport->config.clock(transport->config.clock_context);
    if (!isfinite(now)) return RNS_ERROR_INVALID_STATE;
    size_t required = PATH_STORE_HEADER_SIZE;
    size_t count = 0u;
    for (size_t i = 0u; i < transport->config.path_capacity; ++i) {
        const rns_path_entry *entry = &transport->paths[i];
        if (!entry->occupied || entry->expires_at <= now) continue;
        if (entry->random_blob_count == 0u ||
            entry->random_blob_count > RNS_TRANSPORT_MAX_RANDOM_BLOBS)
            return RNS_ERROR_INVALID_STATE;
        size_t record_size = PATH_STORE_RECORD_BASE_SIZE +
                             entry->random_blob_count * RNS_TRANSPORT_RANDOM_BLOB_SIZE +
                             (entry->has_identity ? PATH_STORE_IDENTITY_SIZE : 0u);
        if (record_size > UINT16_MAX || required > SIZE_MAX - record_size - 2u)
            return RNS_ERROR_OVERFLOW;
        required += PATH_STORE_RECORD_PREFIX_SIZE + record_size;
        ++count;
    }
    if (count > UINT32_MAX || required - PATH_STORE_HEADER_SIZE > UINT32_MAX)
        return RNS_ERROR_OVERFLOW;
    *output_length = required;
    *encoded_count = count;
    if (required > output_capacity || output == NULL) return RNS_ERROR_OVERFLOW;
    memset(output, 0, PATH_STORE_HEADER_SIZE);
    memcpy(output, path_store_magic, sizeof path_store_magic);
    put16(output + 8u, PATH_STORE_VERSION);
    put16(output + 10u, PATH_STORE_HEADER_SIZE);
    put32(output + 12u, (uint32_t)count);
    put32(output + 16u, (uint32_t)(required - PATH_STORE_HEADER_SIZE));
    put64(output + 20u, wall_time_ms);
    uint8_t *cursor = output + PATH_STORE_HEADER_SIZE;
    for (size_t i = 0u; i < transport->config.path_capacity; ++i) {
        const rns_path_entry *entry = &transport->paths[i];
        if (!entry->occupied || entry->expires_at <= now) continue;
        uint64_t remaining_ms;
        uint64_t age_ms;
        if (!seconds_to_ms(entry->expires_at - now, &remaining_ms) ||
            !seconds_to_ms(now > entry->updated_at ? now - entry->updated_at : 0.0,
                           &age_ms)) return RNS_ERROR_INVALID_STATE;
        size_t record_size = PATH_STORE_RECORD_BASE_SIZE +
                             entry->random_blob_count * RNS_TRANSPORT_RANDOM_BLOB_SIZE +
                             (entry->has_identity ? PATH_STORE_IDENTITY_SIZE : 0u);
        put16(cursor, (uint16_t)record_size);
        cursor += 2u;
        memcpy(cursor, entry->destination_hash, 16u); cursor += 16u;
        memcpy(cursor, entry->next_hop, 16u); cursor += 16u;
        memcpy(cursor, entry->announce_packet_hash, 32u); cursor += 32u;
        put64(cursor, entry->announce_timebase); cursor += 8u;
        put64(cursor, entry->interface_id); cursor += 8u;
        uint32_t gravity;
        memcpy(&gravity, &entry->interface_gravity, sizeof gravity);
        put32(cursor, gravity); cursor += 4u;
        put64(cursor, remaining_ms); cursor += 8u;
        put64(cursor, age_ms); cursor += 8u;
        *cursor++ = entry->hops;
        *cursor++ = entry->unresponsive != 0 ? 1u : 0u;
        *cursor++ = (uint8_t)entry->random_blob_count;
        *cursor++ = entry->has_identity ? 1u : 0u;
        memcpy(cursor, entry->random_blobs,
               entry->random_blob_count * RNS_TRANSPORT_RANDOM_BLOB_SIZE);
        cursor += entry->random_blob_count * RNS_TRANSPORT_RANDOM_BLOB_SIZE;
        if (entry->has_identity) {
            memcpy(cursor, entry->identity_public_key, PATH_STORE_IDENTITY_SIZE);
            cursor += PATH_STORE_IDENTITY_SIZE;
        }
    }
    put32(output + 28u, crc32_bytes(output + PATH_STORE_HEADER_SIZE,
                                    required - PATH_STORE_HEADER_SIZE));
    return RNS_OK;
}

static bool duplicate_destination(const rns_path_entry *entries, size_t count,
                                  const uint8_t destination[16]) {
    for (size_t i = 0u; i < count; ++i)
        if (memcmp(entries[i].destination_hash, destination, 16u) == 0) return true;
    return false;
}

rns_status_t rns_path_store_decode(rns_transport *transport,
                                   uint64_t wall_time_ms,
                                   const uint8_t *input, size_t input_length,
                                   size_t *decoded_count) {
    if (decoded_count == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    *decoded_count = 0u;
    if (transport == NULL || transport->paths == NULL ||
        transport->config.clock == NULL || input == NULL)
        return RNS_ERROR_INVALID_ARGUMENT;
    if (input_length < PATH_STORE_HEADER_SIZE ||
        memcmp(input, path_store_magic, sizeof path_store_magic) != 0 ||
        (get16(input + 8u) != 1u && get16(input + 8u) != PATH_STORE_VERSION) ||
        get16(input + 10u) != PATH_STORE_HEADER_SIZE)
        return RNS_ERROR_PROTOCOL;
    uint16_t version = get16(input + 8u);
    uint32_t declared_count = get32(input + 12u);
    uint32_t payload_length = get32(input + 16u);
    if (declared_count > transport->config.path_capacity ||
        payload_length != input_length - PATH_STORE_HEADER_SIZE ||
        crc32_bytes(input + PATH_STORE_HEADER_SIZE, payload_length) !=
            get32(input + 28u)) return RNS_ERROR_PROTOCOL;
    double now = transport->config.clock(transport->config.clock_context);
    if (!isfinite(now)) return RNS_ERROR_INVALID_STATE;
    uint64_t saved_wall_ms = get64(input + 20u);
    uint64_t offline_ms = wall_time_ms >= saved_wall_ms
                              ? wall_time_ms - saved_wall_ms : 0u;
    rns_path_entry *candidate = calloc(transport->config.path_capacity,
                                       sizeof(*candidate));
    if (candidate == NULL) return RNS_ERROR_NO_MEMORY;
    uint64_t maximum_lifetime_ms = 0u;
    if (!seconds_to_ms(transport->config.path_lifetime,
                       &maximum_lifetime_ms)) {
        free(candidate);
        return RNS_ERROR_INVALID_STATE;
    }
    const uint8_t *cursor = input + PATH_STORE_HEADER_SIZE;
    size_t remaining = payload_length;
    size_t retained = 0u;
    rns_status_t status = RNS_OK;
    for (uint32_t i = 0u; i < declared_count && status == RNS_OK; ++i) {
        if (remaining < 2u) { status = RNS_ERROR_PROTOCOL; break; }
        size_t record_size = get16(cursor); cursor += 2u; remaining -= 2u;
        if (record_size < PATH_STORE_RECORD_BASE_SIZE || record_size > remaining) {
            status = RNS_ERROR_PROTOCOL; break;
        }
        const uint8_t *record = cursor;
        uint8_t blob_count = record[102u];
        bool has_identity = version >= 2u && record[103u] == 1u;
        if (record[101u] > 1u || record[103u] > (version >= 2u ? 1u : 0u) || blob_count == 0u ||
            blob_count > RNS_TRANSPORT_MAX_RANDOM_BLOBS ||
            record_size != PATH_STORE_RECORD_BASE_SIZE +
                           (size_t)blob_count * RNS_TRANSPORT_RANDOM_BLOB_SIZE +
                           (has_identity ? PATH_STORE_IDENTITY_SIZE : 0u) ||
            duplicate_destination(candidate, retained, record)) {
            status = RNS_ERROR_PROTOCOL; break;
        }
        uint64_t remaining_ms = get64(record + 84u);
        uint64_t age_ms = get64(record + 92u);
        uint64_t latest_timebase = 0u;
        for (size_t blob = 0u; blob < blob_count; ++blob) {
            uint64_t value = blob_timebase(
                record + PATH_STORE_RECORD_BASE_SIZE +
                blob * RNS_TRANSPORT_RANDOM_BLOB_SIZE);
            if (value > latest_timebase) latest_timebase = value;
        }
        if (remaining_ms > maximum_lifetime_ms ||
            get64(record + 64u) != latest_timebase) {
            status = RNS_ERROR_PROTOCOL;
            break;
        }
        if (remaining_ms > offline_ms) {
            rns_path_entry *entry = &candidate[retained++];
            memcpy(entry->destination_hash, record, 16u);
            memcpy(entry->next_hop, record + 16u, 16u);
            memcpy(entry->announce_packet_hash, record + 32u, 32u);
            entry->announce_timebase = get64(record + 64u);
            entry->interface_id = get64(record + 72u);
            uint32_t gravity = get32(record + 80u);
            memcpy(&entry->interface_gravity, &gravity, sizeof gravity);
            entry->hops = record[100u];
            entry->unresponsive = record[101u] != 0u;
            entry->random_blob_count = blob_count;
            memcpy(entry->random_blobs, record + PATH_STORE_RECORD_BASE_SIZE,
                   (size_t)blob_count * RNS_TRANSPORT_RANDOM_BLOB_SIZE);
            if (has_identity) {
                memcpy(entry->identity_public_key,
                       record + PATH_STORE_RECORD_BASE_SIZE +
                           (size_t)blob_count * RNS_TRANSPORT_RANDOM_BLOB_SIZE,
                       PATH_STORE_IDENTITY_SIZE);
                entry->has_identity = 1;
            }
            double total_age = (double)age_ms / 1000.0 +
                               (double)offline_ms / 1000.0;
            entry->updated_at = now - total_age;
            entry->expires_at = now +
                (double)(remaining_ms - offline_ms) / 1000.0;
            entry->occupied = 1;
        }
        cursor += record_size;
        remaining -= record_size;
    }
    if (status == RNS_OK && remaining != 0u) status = RNS_ERROR_PROTOCOL;
    if (status == RNS_OK) {
        memcpy(transport->paths, candidate,
               transport->config.path_capacity * sizeof(*candidate));
        *decoded_count = retained;
    }
    free(candidate);
    return status;
}
