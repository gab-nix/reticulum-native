#include "reticulum/resource.h"

#include "reticulum/crypto.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef RETICULUM_HAVE_BZIP2
#include <bzlib.h>
#endif

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
} mp_reader_t;

struct rns_resource {
    rns_resource_advertisement_t advertisement;
    uint8_t *map_hashes;
    size_t known_hashes;
    uint8_t **parts;
    size_t *part_lengths;
    bool *requested;
    size_t received;
    size_t retries_used;
    bool waiting;
    bool assembled;
    uint8_t proof[RNS_RESOURCE_PROOF_SIZE];
    size_t max_size;
};

struct rns_resource_sender {
    uint8_t *source;
    size_t source_length;
    size_t segment_index;
    size_t total_segments;
    size_t total_data_parts;
    bool auto_compress;
    bool is_response;
    bool has_request_id;
    uint8_t request_id[RNS_RESOURCE_REQUEST_ID_SIZE];
    uint8_t flags;
    uint8_t random_hash[RNS_RESOURCE_RANDOM_HASH_SIZE];
    uint8_t hash[RNS_RESOURCE_HASH_SIZE];
    uint8_t original_hash[RNS_RESOURCE_HASH_SIZE];
    uint8_t *wire;
    size_t wire_length;
    size_t parts;
    uint8_t *map_hashes;
};

static bool valid_span(const void *data, size_t length) {
    return data != NULL || length == 0U;
}

static bool add_size(size_t a, size_t b, size_t *out) {
    if (a > SIZE_MAX - b) return false;
    *out = a + b;
    return true;
}

static bool mul_size(size_t a, size_t b, size_t *out) {
    if (a != 0U && b > SIZE_MAX / a) return false;
    *out = a * b;
    return true;
}

static bool mp_byte(mp_reader_t *reader, uint8_t *value) {
    if (reader->p == reader->end) return false;
    *value = *reader->p++;
    return true;
}

static bool mp_take(mp_reader_t *reader, size_t length, const uint8_t **data) {
    if ((size_t)(reader->end - reader->p) < length) return false;
    *data = reader->p;
    reader->p += length;
    return true;
}

static bool mp_uint(mp_reader_t *reader, uint64_t *value) {
    uint8_t tag;
    const uint8_t *p;
    if (!mp_byte(reader, &tag)) return false;
    if (tag <= 0x7fU) {
        *value = tag;
        return true;
    }
    size_t length;
    if (tag == 0xccU) length = 1U;
    else if (tag == 0xcdU) length = 2U;
    else if (tag == 0xceU) length = 4U;
    else if (tag == 0xcfU) length = 8U;
    else return false;
    if (!mp_take(reader, length, &p)) return false;
    uint64_t result = 0U;
    for (size_t i = 0U; i < length; ++i) result = (result << 8U) | p[i];
    *value = result;
    return true;
}

static bool mp_bin(mp_reader_t *reader, const uint8_t **data, size_t *length) {
    uint8_t tag;
    const uint8_t *p;
    if (!mp_byte(reader, &tag)) return false;
    uint64_t n = 0U;
    size_t width;
    if (tag == 0xc4U) width = 1U;
    else if (tag == 0xc5U) width = 2U;
    else if (tag == 0xc6U) width = 4U;
    else return false;
    if (!mp_take(reader, width, &p)) return false;
    for (size_t i = 0U; i < width; ++i) n = (n << 8U) | p[i];
    if (n > SIZE_MAX || !mp_take(reader, (size_t)n, data)) return false;
    *length = (size_t)n;
    return true;
}

static bool mp_key(mp_reader_t *reader, uint8_t *key) {
    uint8_t tag;
    return mp_byte(reader, &tag) && tag == 0xa1U && mp_byte(reader, key);
}

static bool uint_to_size(uint64_t value, size_t *out) {
    if (value > SIZE_MAX) return false;
    *out = (size_t)value;
    return true;
}

static rns_status_t validate_advertisement(
    const rns_resource_advertisement_t *advertisement) {
    if (advertisement->parts == 0U ||
        advertisement->parts > RNS_RESOURCE_MAX_PARTS ||
        advertisement->transfer_size == 0U ||
        advertisement->data_size > RNS_RESOURCE_MAX_SIZE ||
        advertisement->segment_index == 0U ||
        advertisement->segment_index > advertisement->total_segments ||
        advertisement->hashmap == NULL ||
        advertisement->hashmap_length % RNS_RESOURCE_MAPHASH_LEN != 0U ||
        advertisement->hashmap_length !=
            (advertisement->parts < RNS_RESOURCE_HASHMAP_MAX_ENTRIES
                 ? advertisement->parts : RNS_RESOURCE_HASHMAP_MAX_ENTRIES) *
                RNS_RESOURCE_MAPHASH_LEN)
        return RNS_ERROR_PROTOCOL;
    size_t maximum_wire;
    if (!mul_size(advertisement->parts, RNS_RESOURCE_PART_MAX,
                  &maximum_wire) ||
        advertisement->transfer_size > maximum_wire ||
        advertisement->parts > advertisement->transfer_size)
        return RNS_ERROR_PROTOCOL;
    if ((advertisement->flags & RNS_RESOURCE_FLAG_ENCRYPTED) != 0U &&
        (advertisement->transfer_size < 64U ||
         advertisement->transfer_size % 16U != 0U))
        return RNS_ERROR_PROTOCOL;
    if ((advertisement->flags & RNS_RESOURCE_FLAG_ENCRYPTED) == 0U &&
        advertisement->transfer_size < RNS_RESOURCE_RANDOM_HASH_SIZE)
        return RNS_ERROR_PROTOCOL;
    if (((advertisement->flags & RNS_RESOURCE_FLAG_SPLIT) != 0U) !=
        (advertisement->total_segments > 1U))
        return RNS_ERROR_PROTOCOL;
    if (advertisement->segment_index == 1U &&
        memcmp(advertisement->hash, advertisement->original_hash,
               RNS_RESOURCE_HASH_SIZE) != 0)
        return RNS_ERROR_PROTOCOL;
    return RNS_OK;
}

rns_status_t rns_resource_advertisement_parse(
    const uint8_t *data, size_t length, rns_resource_advertisement_t *out) {
    if (!valid_span(data, length) || out == NULL || length == 0U)
        return RNS_ERROR_INVALID_ARGUMENT;
    mp_reader_t reader = {data, data + length};
    uint8_t map_tag;
    if (!mp_byte(&reader, &map_tag) || map_tag != 0x8bU)
        return RNS_ERROR_PROTOCOL;
    rns_resource_advertisement_t parsed;
    memset(&parsed, 0, sizeof parsed);
    uint16_t seen = 0U;
    for (size_t field = 0U; field < 11U; ++field) {
        uint8_t key;
        uint16_t bit;
        if (!mp_key(&reader, &key)) return RNS_ERROR_PROTOCOL;
        switch (key) {
        case 't': bit = 1U << 0U; break;
        case 'd': bit = 1U << 1U; break;
        case 'n': bit = 1U << 2U; break;
        case 'h': bit = 1U << 3U; break;
        case 'r': bit = 1U << 4U; break;
        case 'o': bit = 1U << 5U; break;
        case 'i': bit = 1U << 6U; break;
        case 'l': bit = 1U << 7U; break;
        case 'q': bit = 1U << 8U; break;
        case 'f': bit = 1U << 9U; break;
        case 'm': bit = 1U << 10U; break;
        default: return RNS_ERROR_PROTOCOL;
        }
        if ((seen & bit) != 0U) return RNS_ERROR_PROTOCOL;
        seen |= bit;
        if (key == 't' || key == 'd' || key == 'n' || key == 'i' ||
            key == 'l' || key == 'f') {
            uint64_t value;
            size_t converted;
            if (!mp_uint(&reader, &value) || !uint_to_size(value, &converted))
                return RNS_ERROR_PROTOCOL;
            if (key == 't') parsed.transfer_size = converted;
            else if (key == 'd') parsed.data_size = converted;
            else if (key == 'n') parsed.parts = converted;
            else if (key == 'i') parsed.segment_index = converted;
            else if (key == 'l') parsed.total_segments = converted;
            else {
                if (value > UINT8_MAX) return RNS_ERROR_PROTOCOL;
                parsed.flags = (uint8_t)value;
            }
        } else if (key == 'q') {
            if (reader.p == reader.end) return RNS_ERROR_PROTOCOL;
            if (*reader.p == 0xc0U) {
                reader.p++;
            } else {
                const uint8_t *value;
                size_t value_length;
                if (!mp_bin(&reader, &value, &value_length) ||
                    value_length != RNS_RESOURCE_REQUEST_ID_SIZE)
                    return RNS_ERROR_PROTOCOL;
                memcpy(parsed.request_id, value, value_length);
                parsed.has_request_id = true;
            }
        } else {
            const uint8_t *value;
            size_t value_length;
            if (!mp_bin(&reader, &value, &value_length))
                return RNS_ERROR_PROTOCOL;
            if (key == 'h') {
                if (value_length != RNS_RESOURCE_HASH_SIZE)
                    return RNS_ERROR_PROTOCOL;
                memcpy(parsed.hash, value, value_length);
            } else if (key == 'r') {
                if (value_length != RNS_RESOURCE_RANDOM_HASH_SIZE)
                    return RNS_ERROR_PROTOCOL;
                memcpy(parsed.random_hash, value, value_length);
            } else if (key == 'o') {
                if (value_length != RNS_RESOURCE_HASH_SIZE)
                    return RNS_ERROR_PROTOCOL;
                memcpy(parsed.original_hash, value, value_length);
            } else {
                parsed.hashmap = value;
                parsed.hashmap_length = value_length;
            }
        }
    }
    if (seen != 0x7ffU || reader.p != reader.end)
        return RNS_ERROR_PROTOCOL;
    parsed.encrypted = (parsed.flags & RNS_RESOURCE_FLAG_ENCRYPTED) != 0U;
    parsed.compressed = (parsed.flags & RNS_RESOURCE_FLAG_COMPRESSED) != 0U;
    parsed.split = (parsed.flags & RNS_RESOURCE_FLAG_SPLIT) != 0U;
    parsed.is_request = (parsed.flags & RNS_RESOURCE_FLAG_REQUEST) != 0U;
    parsed.is_response = (parsed.flags & RNS_RESOURCE_FLAG_RESPONSE) != 0U;
    parsed.has_metadata = (parsed.flags & RNS_RESOURCE_FLAG_METADATA) != 0U;
    if (validate_advertisement(&parsed) != RNS_OK)
        return RNS_ERROR_PROTOCOL;
    *out = parsed;
    return RNS_OK;
}

rns_status_t rns_resource_accept(rns_resource_t **out,
                                 const rns_resource_advertisement_t *adv,
                                 size_t max_size) {
    if (out == NULL || adv == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    *out = NULL;
    if (adv->hashmap == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    if (adv->parts > RNS_RESOURCE_MAX_PARTS) return RNS_ERROR_UNSUPPORTED;
    if (validate_advertisement(adv) != RNS_OK) return RNS_ERROR_PROTOCOL;
    if (max_size == 0U) max_size = RNS_RESOURCE_DEFAULT_MAX_SIZE;
    if (max_size > RNS_RESOURCE_MAX_SIZE) max_size = RNS_RESOURCE_MAX_SIZE;
    if (adv->data_size > max_size) return RNS_ERROR_OVERFLOW;
    rns_resource_t *resource = calloc(1U, sizeof *resource);
    if (resource == NULL) return RNS_ERROR_NO_MEMORY;
    size_t map_bytes;
    if (!mul_size(adv->parts, RNS_RESOURCE_MAPHASH_LEN, &map_bytes)) {
        free(resource);
        return RNS_ERROR_OVERFLOW;
    }
    resource->map_hashes = calloc(1U, map_bytes);
    resource->parts = calloc(adv->parts, sizeof *resource->parts);
    resource->part_lengths = calloc(adv->parts, sizeof *resource->part_lengths);
    resource->requested = calloc(adv->parts, sizeof *resource->requested);
    if (resource->map_hashes == NULL || resource->parts == NULL ||
        resource->part_lengths == NULL || resource->requested == NULL) {
        rns_resource_destroy(resource);
        return RNS_ERROR_NO_MEMORY;
    }
    resource->advertisement = *adv;
    resource->advertisement.encrypted =
        (adv->flags & RNS_RESOURCE_FLAG_ENCRYPTED) != 0U;
    resource->advertisement.compressed =
        (adv->flags & RNS_RESOURCE_FLAG_COMPRESSED) != 0U;
    resource->advertisement.split =
        (adv->flags & RNS_RESOURCE_FLAG_SPLIT) != 0U;
    resource->advertisement.is_request =
        (adv->flags & RNS_RESOURCE_FLAG_REQUEST) != 0U;
    resource->advertisement.is_response =
        (adv->flags & RNS_RESOURCE_FLAG_RESPONSE) != 0U;
    resource->advertisement.has_metadata =
        (adv->flags & RNS_RESOURCE_FLAG_METADATA) != 0U;
    resource->advertisement.hashmap = resource->map_hashes;
    memcpy(resource->map_hashes, adv->hashmap, adv->hashmap_length);
    resource->known_hashes = adv->hashmap_length / RNS_RESOURCE_MAPHASH_LEN;
    resource->max_size = max_size;
    *out = resource;
    return RNS_OK;
}

void rns_resource_destroy(rns_resource_t *resource) {
    if (resource == NULL) return;
    if (resource->parts != NULL) {
        for (size_t i = 0U; i < resource->advertisement.parts; ++i)
            free(resource->parts[i]);
    }
    free(resource->part_lengths);
    free(resource->parts);
    free(resource->requested);
    free(resource->map_hashes);
    free(resource);
}

size_t rns_resource_total_parts(const rns_resource_t *resource) {
    return resource != NULL ? resource->advertisement.parts : 0U;
}

size_t rns_resource_received_parts(const rns_resource_t *resource) {
    return resource != NULL ? resource->received : 0U;
}

size_t rns_resource_outstanding_parts(const rns_resource_t *resource) {
    if (resource == NULL) return 0U;
    size_t outstanding = 0U;
    for (size_t i = 0U; i < resource->known_hashes; ++i)
        if (resource->parts[i] == NULL && resource->requested[i]) outstanding++;
    return outstanding;
}

size_t rns_resource_retries_used(const rns_resource_t *resource) {
    return resource != NULL ? resource->retries_used : 0U;
}

bool rns_resource_parts_complete(const rns_resource_t *resource) {
    return resource != NULL && resource->received == resource->advertisement.parts;
}

bool rns_resource_waiting_for_hashmap(const rns_resource_t *resource) {
    return resource != NULL && resource->waiting;
}

static rns_status_t hash_part(const uint8_t *part, size_t length,
                              const uint8_t random_hash[4], uint8_t out[32]) {
    size_t input_length;
    if (!add_size(length, 4U, &input_length)) return RNS_ERROR_OVERFLOW;
    uint8_t *input = malloc(input_length);
    if (input == NULL) return RNS_ERROR_NO_MEMORY;
    memcpy(input, part, length);
    memcpy(input + length, random_hash, 4U);
    int ok = rns_sha256(input, input_length, out);
    free(input);
    return ok ? RNS_OK : RNS_ERROR_CRYPTO;
}

rns_status_t rns_resource_receive_part(rns_resource_t *resource,
                                       const uint8_t *part,
                                       size_t part_length) {
    if (resource == NULL || !valid_span(part, part_length) || part_length == 0U)
        return RNS_ERROR_INVALID_ARGUMENT;
    if (part_length > RNS_RESOURCE_PART_MAX) return RNS_ERROR_PROTOCOL;
    uint8_t digest[32];
    rns_status_t status = hash_part(part, part_length,
                                    resource->advertisement.random_hash, digest);
    if (status != RNS_OK) return status;
    for (size_t i = 0U; i < resource->known_hashes; ++i) {
        if (memcmp(digest, resource->map_hashes + i * 4U, 4U) != 0) continue;
        if (resource->parts[i] != NULL) {
            return resource->part_lengths[i] == part_length &&
                           memcmp(resource->parts[i], part, part_length) == 0
                       ? RNS_OK : RNS_ERROR_PROTOCOL;
        }
        uint8_t *copy = malloc(part_length);
        if (copy == NULL) return RNS_ERROR_NO_MEMORY;
        memcpy(copy, part, part_length);
        resource->parts[i] = copy;
        resource->part_lengths[i] = part_length;
        resource->requested[i] = false;
        resource->received++;
        resource->retries_used = 0U;
        return RNS_OK;
    }
    return RNS_ERROR_PROTOCOL;
}

rns_status_t rns_resource_build_request(rns_resource_t *resource,
                                        uint8_t *out, size_t capacity,
                                        size_t *out_length) {
    if (resource == NULL || out == NULL || out_length == NULL)
        return RNS_ERROR_INVALID_ARGUMENT;
    if (rns_resource_parts_complete(resource)) return RNS_ERROR_INVALID_STATE;
    size_t missing[RNS_RESOURCE_WINDOW];
    size_t count = 0U;
    size_t outstanding = rns_resource_outstanding_parts(resource);
    size_t available = outstanding < RNS_RESOURCE_WINDOW
                           ? RNS_RESOURCE_WINDOW - outstanding : 0U;
    for (size_t i = 0U;
         i < resource->known_hashes && count < available; ++i)
        if (resource->parts[i] == NULL && !resource->requested[i])
            missing[count++] = i;
    if (count != 0U) {
        size_t needed = 1U + RNS_RESOURCE_HASH_SIZE + count * 4U;
        if (capacity < needed) return RNS_ERROR_OVERFLOW;
        out[0] = 0x00U;
        memcpy(out + 1U, resource->advertisement.hash, RNS_RESOURCE_HASH_SIZE);
        for (size_t i = 0U; i < count; ++i) {
            memcpy(out + 33U + i * 4U,
                   resource->map_hashes + missing[i] * 4U, 4U);
            resource->requested[missing[i]] = true;
        }
        *out_length = needed;
        resource->waiting = false;
        return RNS_OK;
    }
    if (outstanding != 0U) return RNS_ERROR_INVALID_STATE;
    if (resource->known_hashes >= resource->advertisement.parts)
        return RNS_ERROR_INVALID_STATE;
    size_t repeat = resource->known_hashes < RNS_RESOURCE_WINDOW
                        ? resource->known_hashes : RNS_RESOURCE_WINDOW;
    size_t needed = 1U + 4U + RNS_RESOURCE_HASH_SIZE + repeat * 4U;
    if (capacity < needed) return RNS_ERROR_OVERFLOW;
    out[0] = 0xffU;
    memcpy(out + 1U, resource->map_hashes + (resource->known_hashes - 1U) * 4U,
           4U);
    memcpy(out + 5U, resource->advertisement.hash, RNS_RESOURCE_HASH_SIZE);
    size_t first = resource->known_hashes - repeat;
    memcpy(out + 37U, resource->map_hashes + first * 4U, repeat * 4U);
    *out_length = needed;
    resource->waiting = true;
    return RNS_OK;
}

rns_status_t rns_resource_build_retry_request(rns_resource_t *resource,
                                              uint8_t *out, size_t capacity,
                                              size_t *out_length) {
    if (resource == NULL || out == NULL || out_length == NULL)
        return RNS_ERROR_INVALID_ARGUMENT;
    if (resource->retries_used >= RNS_RESOURCE_MAX_RETRIES)
        return RNS_ERROR_TIMEOUT;
    size_t previous[RNS_RESOURCE_WINDOW];
    size_t previous_count = 0U;
    if (rns_resource_outstanding_parts(resource) > RNS_RESOURCE_WINDOW)
        return RNS_ERROR_PROTOCOL;
    for (size_t i = 0U; i < resource->known_hashes; ++i) {
        if (resource->parts[i] == NULL && resource->requested[i]) {
            if (previous_count < RNS_RESOURCE_WINDOW)
                previous[previous_count++] = i;
            resource->requested[i] = false;
        }
    }
    rns_status_t status = rns_resource_build_request(
        resource, out, capacity, out_length);
    if (status != RNS_OK) {
        for (size_t i = 0U; i < previous_count; ++i)
            resource->requested[previous[i]] = true;
        return status;
    }
    resource->retries_used++;
    return RNS_OK;
}

double rns_resource_retry_timeout(double rtt_seconds,
                                  double part_airtime_seconds,
                                  size_t outstanding_parts,
                                  size_t retries_used) {
    if (!isfinite(rtt_seconds) || rtt_seconds < 0.0) rtt_seconds = 0.0;
    if (!isfinite(part_airtime_seconds) || part_airtime_seconds < 0.0)
        part_airtime_seconds = 0.0;
    if (outstanding_parts > RNS_RESOURCE_WINDOW)
        outstanding_parts = RNS_RESOURCE_WINDOW;
    if (retries_used > RNS_RESOURCE_MAX_RETRIES)
        retries_used = RNS_RESOURCE_MAX_RETRIES;
    double round_trip = 2.0 * rtt_seconds;
    double airtime = 2.0 * part_airtime_seconds * (double)outstanding_parts;
    double transport = round_trip > airtime ? round_trip : airtime;
    double result = transport + RNS_RESOURCE_RETRY_GRACE_SECONDS +
                    (double)retries_used *
                        RNS_RESOURCE_RETRY_BACKOFF_SECONDS;
    if (!isfinite(result) || result > RNS_RESOURCE_RETRY_MAX_SECONDS)
        result = RNS_RESOURCE_RETRY_MAX_SECONDS;
    return result;
}

static rns_status_t parse_hashmap_update(const uint8_t *update,
                                         size_t update_length,
                                         const uint8_t hash[32],
                                         size_t *page, const uint8_t **map,
                                         size_t *map_length) {
    if (!valid_span(update, update_length) || update_length < 35U)
        return RNS_ERROR_PROTOCOL;
    if (memcmp(update, hash, 32U) != 0) return RNS_ERROR_PROTOCOL;
    mp_reader_t reader = {update + 32U, update + update_length};
    uint8_t tag;
    uint64_t page64;
    if (!mp_byte(&reader, &tag) || tag != 0x92U ||
        !mp_uint(&reader, &page64) || page64 > SIZE_MAX ||
        !mp_bin(&reader, map, map_length) || reader.p != reader.end)
        return RNS_ERROR_PROTOCOL;
    *page = (size_t)page64;
    return RNS_OK;
}

rns_status_t rns_resource_apply_hashmap_update(rns_resource_t *resource,
                                               const uint8_t *update,
                                               size_t update_length) {
    if (resource == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    size_t page;
    const uint8_t *map;
    size_t length;
    rns_status_t status = parse_hashmap_update(update, update_length,
                                               resource->advertisement.hash,
                                               &page, &map, &length);
    if (status != RNS_OK) return status;
    size_t page_start;
    if (!mul_size(page, RNS_RESOURCE_HASHMAP_MAX_ENTRIES, &page_start) ||
        page_start >= resource->advertisement.parts)
        return RNS_ERROR_PROTOCOL;
    size_t entries = resource->advertisement.parts - page_start;
    if (entries > RNS_RESOURCE_HASHMAP_MAX_ENTRIES)
        entries = RNS_RESOURCE_HASHMAP_MAX_ENTRIES;
    if (length == 0U || length != entries * 4U) return RNS_ERROR_PROTOCOL;
    if (page_start < resource->known_hashes) {
        return page_start + entries <= resource->known_hashes &&
                       memcmp(resource->map_hashes + page_start * 4U, map,
                              length) == 0
                   ? RNS_OK : RNS_ERROR_PROTOCOL;
    }
    if (!resource->waiting || page_start != resource->known_hashes)
        return RNS_ERROR_PROTOCOL;
    memcpy(resource->map_hashes + page_start * 4U, map, length);
    resource->known_hashes += entries;
    resource->waiting = false;
    resource->retries_used = 0U;
    return RNS_OK;
}

bool rns_resource_decompression_available(void) {
#ifdef RETICULUM_HAVE_BZIP2
    return true;
#else
    return false;
#endif
}

rns_status_t rns_resource_assemble(rns_resource_t *resource,
                                   const rns_link *link, uint8_t *out,
                                   size_t capacity, size_t *out_length) {
    if (resource == NULL || out == NULL || out_length == NULL)
        return RNS_ERROR_INVALID_ARGUMENT;
    if (!rns_resource_parts_complete(resource)) return RNS_ERROR_INVALID_STATE;
    size_t wire_length = 0U;
    for (size_t i = 0U; i < resource->advertisement.parts; ++i) {
        if (!add_size(wire_length, resource->part_lengths[i], &wire_length))
            return RNS_ERROR_OVERFLOW;
    }
    if (wire_length != resource->advertisement.transfer_size)
        return RNS_ERROR_PROTOCOL;
    uint8_t *wire = malloc(wire_length);
    if (wire == NULL) return RNS_ERROR_NO_MEMORY;
    size_t offset = 0U;
    for (size_t i = 0U; i < resource->advertisement.parts; ++i) {
        memcpy(wire + offset, resource->parts[i], resource->part_lengths[i]);
        offset += resource->part_lengths[i];
    }
    uint8_t *plain = wire;
    size_t plain_length = wire_length;
    uint8_t *decrypted = NULL;
    if (resource->advertisement.encrypted) {
        if (link == NULL) {
            free(wire);
            return RNS_ERROR_INVALID_STATE;
        }
        decrypted = malloc(wire_length);
        if (decrypted == NULL) {
            free(wire);
            return RNS_ERROR_NO_MEMORY;
        }
        if (!rns_link_decrypt(link, wire, wire_length, decrypted, wire_length,
                              &plain_length)) {
            free(decrypted);
            free(wire);
            return RNS_ERROR_CRYPTO;
        }
        plain = decrypted;
    }
    /* Pinned peers can use an independently randomised four-byte prefix inside
     * the authenticated encrypted stream. Advertisement r still salts the map
     * and final Resource hashes. Plaintext Resources retain exact r-prefix
     * validation because they do not have the token authentication layer. */
    if (plain_length < RNS_RESOURCE_RANDOM_HASH_SIZE ||
        (!resource->advertisement.encrypted &&
         memcmp(plain, resource->advertisement.random_hash,
                RNS_RESOURCE_RANDOM_HASH_SIZE) != 0)) {
        free(decrypted);
        free(wire);
        return RNS_ERROR_PROTOCOL;
    }
    const uint8_t *processed = plain + 4U;
    size_t processed_length = plain_length - 4U;
    size_t result_length = processed_length;
    rns_status_t result = RNS_OK;
    if (resource->advertisement.compressed) {
#ifdef RETICULUM_HAVE_BZIP2
        size_t output_limit = capacity;
        if (output_limit > resource->max_size) output_limit = resource->max_size;
        if (output_limit > resource->advertisement.data_size)
            output_limit = resource->advertisement.data_size;
        if (output_limit > UINT_MAX) {
            result = RNS_ERROR_OVERFLOW;
        } else if (processed_length > UINT_MAX) {
            result = RNS_ERROR_PROTOCOL;
        } else {
            unsigned int destination_length = (unsigned int)output_limit;
            int bz = BZ2_bzBuffToBuffDecompress(
                (char *)out, &destination_length, (char *)(uintptr_t)processed,
                (unsigned int)processed_length, 0, 0);
            if (bz == BZ_OUTBUFF_FULL) result = RNS_ERROR_OVERFLOW;
            else if (bz != BZ_OK) result = RNS_ERROR_PROTOCOL;
            else result_length = destination_length;
        }
#else
        result = RNS_ERROR_UNSUPPORTED;
#endif
    } else if (capacity < processed_length ||
               processed_length > resource->max_size ||
               processed_length > resource->advertisement.data_size) {
        result = RNS_ERROR_OVERFLOW;
    } else {
        memcpy(out, processed, processed_length);
    }
    if (result == RNS_OK &&
        (result_length > resource->max_size ||
         result_length > resource->advertisement.data_size))
        result = RNS_ERROR_OVERFLOW;
    if (result == RNS_OK) {
        size_t verify_length;
        if (!add_size(result_length, 4U, &verify_length)) result = RNS_ERROR_OVERFLOW;
        else {
            uint8_t *verify = malloc(verify_length);
            uint8_t digest[32];
            if (verify == NULL) result = RNS_ERROR_NO_MEMORY;
            else {
                memcpy(verify, out, result_length);
                memcpy(verify + result_length,
                       resource->advertisement.random_hash, 4U);
                if (!rns_sha256(verify, verify_length, digest))
                    result = RNS_ERROR_CRYPTO;
                else if (memcmp(digest, resource->advertisement.hash, 32U) != 0)
                    result = RNS_ERROR_PROTOCOL;
                free(verify);
            }
        }
    }
    if (result == RNS_OK) {
        uint8_t *proof_input;
        size_t proof_input_length;
        if (!add_size(result_length, 32U, &proof_input_length) ||
            (proof_input = malloc(proof_input_length)) == NULL) {
            result = RNS_ERROR_NO_MEMORY;
        } else {
            memcpy(proof_input, out, result_length);
            memcpy(proof_input + result_length, resource->advertisement.hash,
                   32U);
            memcpy(resource->proof, resource->advertisement.hash, 32U);
            if (!rns_sha256(proof_input, proof_input_length,
                            resource->proof + 32U))
                result = RNS_ERROR_CRYPTO;
            free(proof_input);
            if (result == RNS_OK) {
                resource->assembled = true;
                *out_length = result_length;
            }
        }
    }
    free(decrypted);
    free(wire);
    return result;
}

rns_status_t rns_resource_build_proof(const rns_resource_t *resource,
                                      uint8_t out[RNS_RESOURCE_PROOF_SIZE]) {
    if (resource == NULL || out == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    if (!resource->assembled) return RNS_ERROR_INVALID_STATE;
    memcpy(out, resource->proof, RNS_RESOURCE_PROOF_SIZE);
    return RNS_OK;
}

static size_t segment_source_length(const rns_resource_sender_t *sender,
                                    size_t segment_index) {
    size_t offset = (segment_index - 1U) * RNS_RESOURCE_SINGLE_SEGMENT_MAX_SIZE;
    size_t remaining = sender->source_length - offset;
    return remaining < RNS_RESOURCE_SINGLE_SEGMENT_MAX_SIZE
               ? remaining : RNS_RESOURCE_SINGLE_SEGMENT_MAX_SIZE;
}

static rns_status_t compressed_copy(const uint8_t *source, size_t length,
                                    uint8_t **copy, size_t *copy_length) {
    *copy = NULL;
    *copy_length = length;
#ifdef RETICULUM_HAVE_BZIP2
    if (length > UINT_MAX || length > SIZE_MAX - length / 100U - 601U)
        return RNS_ERROR_OVERFLOW;
    size_t bound = length + length / 100U + 601U;
    if (bound > UINT_MAX) return RNS_ERROR_OVERFLOW;
    uint8_t *candidate = malloc(bound);
    if (candidate == NULL) return RNS_ERROR_NO_MEMORY;
    unsigned int destination_length = (unsigned int)bound;
    int bz = BZ2_bzBuffToBuffCompress((char *)candidate, &destination_length,
                                     (char *)(uintptr_t)source,
                                     (unsigned int)length, 9, 0, 30);
    if (bz != BZ_OK) {
        free(candidate);
        return RNS_ERROR_PROTOCOL;
    }
    if ((size_t)destination_length < length) {
        *copy = candidate;
        *copy_length = destination_length;
    } else {
        free(candidate);
    }
#else
    (void)source;
#endif
    return RNS_OK;
}

static rns_status_t sender_prepare_segment_attempt(rns_resource_sender_t *sender,
                                           const rns_link *link, bool *collision) {
    *collision = false;
    free(sender->wire);
    free(sender->map_hashes);
    sender->wire = NULL;
    sender->map_hashes = NULL;
    size_t segment_length = segment_source_length(sender, sender->segment_index);
    size_t offset = (sender->segment_index - 1U) *
                    RNS_RESOURCE_SINGLE_SEGMENT_MAX_SIZE;
    const uint8_t *segment = sender->source + offset;
    if (!rns_random_bytes(sender->random_hash, 4U)) return RNS_ERROR_CRYPTO;
    size_t hash_input_length;
    if (!add_size(segment_length, 4U, &hash_input_length)) return RNS_ERROR_OVERFLOW;
    uint8_t *hash_input = malloc(hash_input_length);
    if (hash_input == NULL) return RNS_ERROR_NO_MEMORY;
    memcpy(hash_input, segment, segment_length);
    memcpy(hash_input + segment_length, sender->random_hash, 4U);
    int hash_ok = rns_sha256(hash_input, hash_input_length, sender->hash);
    free(hash_input);
    if (!hash_ok) return RNS_ERROR_CRYPTO;
    if (sender->segment_index == 1U)
        memcpy(sender->original_hash, sender->hash, 32U);
    uint8_t *compressed = NULL;
    size_t processed_length = segment_length;
    rns_status_t status = RNS_OK;
    if (sender->auto_compress)
        status = compressed_copy(segment, segment_length, &compressed,
                                 &processed_length);
    if (status != RNS_OK) return status;
    sender->flags = RNS_RESOURCE_FLAG_ENCRYPTED;
    if (compressed != NULL) sender->flags |= RNS_RESOURCE_FLAG_COMPRESSED;
    if (sender->total_segments > 1U) sender->flags |= RNS_RESOURCE_FLAG_SPLIT;
    if (sender->is_response) sender->flags |= RNS_RESOURCE_FLAG_RESPONSE;
    size_t token_plain_length;
    if (!add_size(processed_length, 4U, &token_plain_length)) {
        free(compressed);
        return RNS_ERROR_OVERFLOW;
    }
    uint8_t *token_plain = malloc(token_plain_length);
    if (token_plain == NULL) {
        free(compressed);
        return RNS_ERROR_NO_MEMORY;
    }
    memcpy(token_plain, sender->random_hash, 4U);
    memcpy(token_plain + 4U, compressed != NULL ? compressed : segment,
           processed_length);
    free(compressed);
    if (token_plain_length > SIZE_MAX - 64U) {
        free(token_plain);
        return RNS_ERROR_OVERFLOW;
    }
    size_t wire_capacity = 16U + (token_plain_length / 16U + 1U) * 16U + 32U;
    sender->wire = malloc(wire_capacity);
    if (sender->wire == NULL) {
        free(token_plain);
        return RNS_ERROR_NO_MEMORY;
    }
    if (!rns_link_encrypt(link, token_plain, token_plain_length, sender->wire,
                          wire_capacity, &sender->wire_length)) {
        free(token_plain);
        free(sender->wire);
        sender->wire = NULL;
        return RNS_ERROR_CRYPTO;
    }
    free(token_plain);
    sender->parts = (sender->wire_length + RNS_RESOURCE_PART_MAX - 1U) /
                    RNS_RESOURCE_PART_MAX;
    if (sender->parts == 0U || sender->parts > RNS_RESOURCE_MAX_PARTS)
        return RNS_ERROR_OVERFLOW;
    sender->map_hashes = malloc(sender->parts * 4U);
    if (sender->map_hashes == NULL) return RNS_ERROR_NO_MEMORY;
    for (size_t i = 0U; i < sender->parts; ++i) {
        size_t part_offset = i * RNS_RESOURCE_PART_MAX;
        size_t part_length = sender->wire_length - part_offset;
        if (part_length > RNS_RESOURCE_PART_MAX) part_length = RNS_RESOURCE_PART_MAX;
        uint8_t digest[32];
        status = hash_part(sender->wire + part_offset, part_length,
                           sender->random_hash, digest);
        if (status != RNS_OK) return status;
        for (size_t previous = 0U; previous < i; ++previous) {
            if (memcmp(sender->map_hashes + previous * 4U, digest, 4U) == 0) {
                *collision = true;
                return RNS_OK;
            }
        }
        memcpy(sender->map_hashes + i * 4U, digest, 4U);
    }
    return RNS_OK;
}

static rns_status_t sender_prepare_segment(rns_resource_sender_t *sender,
                                           const rns_link *link) {
    /* Truncated map hashes must identify parts unambiguously. Regenerate
     * salt, resource hash and encrypted stream before advertising a collision.
     * Bound retries even if a broken crypto provider repeatedly collides. */
    for (unsigned attempt = 0U; attempt < 8U; ++attempt) {
        bool collision = false;
        rns_status_t status = sender_prepare_segment_attempt(sender, link, &collision);
        if (status != RNS_OK || !collision) return status;
    }
    return RNS_ERROR_CRYPTO;
}

static rns_status_t estimate_parts(const uint8_t *source, size_t length,
                                   bool compress, size_t *parts) {
    uint8_t *compressed = NULL;
    size_t processed_length = length;
    rns_status_t status = compress
        ? compressed_copy(source, length, &compressed, &processed_length) : RNS_OK;
    free(compressed);
    if (status != RNS_OK) return status;
    size_t plain_length;
    if (!add_size(processed_length, 4U, &plain_length) ||
        plain_length > SIZE_MAX - 64U)
        return RNS_ERROR_OVERFLOW;
    size_t wire_length = 16U + (plain_length / 16U + 1U) * 16U + 32U;
    *parts = (wire_length + RNS_RESOURCE_PART_MAX - 1U) /
             RNS_RESOURCE_PART_MAX;
    return RNS_OK;
}

rns_status_t rns_resource_sender_create(
    rns_resource_sender_t **out, const rns_link *link, const uint8_t *source,
    size_t source_length, const rns_resource_sender_options_t *options) {
    if (out == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    *out = NULL;
    if (link == NULL || link->state != RNS_LINK_ACTIVE ||
        !valid_span(source, source_length) || source_length == 0U ||
        source_length > RNS_RESOURCE_MAX_SIZE)
        return RNS_ERROR_INVALID_ARGUMENT;
    bool auto_compress = options == NULL ? true : options->auto_compress;
    bool response = options != NULL && options->is_response;
    if (response && options->request_id == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    rns_resource_sender_t *sender = calloc(1U, sizeof *sender);
    if (sender == NULL) return RNS_ERROR_NO_MEMORY;
    sender->source = malloc(source_length);
    if (sender->source == NULL) {
        free(sender);
        return RNS_ERROR_NO_MEMORY;
    }
    memcpy(sender->source, source, source_length);
    sender->source_length = source_length;
    sender->segment_index = 1U;
    sender->total_segments =
        (source_length + RNS_RESOURCE_SINGLE_SEGMENT_MAX_SIZE - 1U) /
        RNS_RESOURCE_SINGLE_SEGMENT_MAX_SIZE;
    sender->auto_compress = auto_compress;
    sender->is_response = response;
    sender->has_request_id = response;
    if (response) memcpy(sender->request_id, options->request_id, 16U);
    for (size_t i = 0U; i < sender->total_segments; ++i) {
        size_t offset = i * RNS_RESOURCE_SINGLE_SEGMENT_MAX_SIZE;
        size_t length = source_length - offset;
        if (length > RNS_RESOURCE_SINGLE_SEGMENT_MAX_SIZE)
            length = RNS_RESOURCE_SINGLE_SEGMENT_MAX_SIZE;
        size_t parts;
        rns_status_t status = estimate_parts(sender->source + offset, length,
                                             auto_compress, &parts);
        if (status != RNS_OK || sender->total_data_parts > SIZE_MAX - parts) {
            rns_resource_sender_destroy(sender);
            return status != RNS_OK ? status : RNS_ERROR_OVERFLOW;
        }
        sender->total_data_parts += parts;
    }
    rns_status_t status = sender_prepare_segment(sender, link);
    if (status != RNS_OK) {
        rns_resource_sender_destroy(sender);
        return status;
    }
    *out = sender;
    return RNS_OK;
}

void rns_resource_sender_destroy(rns_resource_sender_t *sender) {
    if (sender == NULL) return;
    free(sender->map_hashes);
    free(sender->wire);
    free(sender->source);
    free(sender);
}

static size_t mp_uint_size(size_t value) {
    if (value <= 0x7fU) return 1U;
    if (value <= UINT8_MAX) return 2U;
    if (value <= UINT16_MAX) return 3U;
    if (value <= UINT32_MAX) return 5U;
    return 9U;
}

static uint8_t *mp_put_uint(uint8_t *out, size_t value) {
    size_t width;
    if (value <= 0x7fU) {
        *out++ = (uint8_t)value;
        return out;
    } else if (value <= UINT8_MAX) {
        *out++ = 0xccU; width = 1U;
    } else if (value <= UINT16_MAX) {
        *out++ = 0xcdU; width = 2U;
    } else if (value <= UINT32_MAX) {
        *out++ = 0xceU; width = 4U;
    } else {
        *out++ = 0xcfU; width = 8U;
    }
    for (size_t i = width; i > 0U; --i)
        *out++ = (uint8_t)(value >> ((i - 1U) * 8U));
    return out;
}

static size_t mp_bin_header_size(size_t length) {
    return length <= UINT8_MAX ? 2U : (length <= UINT16_MAX ? 3U : 5U);
}

static uint8_t *mp_put_bin(uint8_t *out, const uint8_t *data, size_t length) {
    if (length <= UINT8_MAX) {
        *out++ = 0xc4U; *out++ = (uint8_t)length;
    } else if (length <= UINT16_MAX) {
        *out++ = 0xc5U; *out++ = (uint8_t)(length >> 8U);
        *out++ = (uint8_t)length;
    } else {
        *out++ = 0xc6U;
        for (size_t i = 4U; i > 0U; --i)
            *out++ = (uint8_t)(length >> ((i - 1U) * 8U));
    }
    memcpy(out, data, length);
    return out + length;
}

static uint8_t *mp_put_key(uint8_t *out, uint8_t key) {
    *out++ = 0xa1U;
    *out++ = key;
    return out;
}

rns_status_t rns_resource_sender_advertisement(
    const rns_resource_sender_t *sender, uint8_t *out, size_t capacity,
    size_t *out_length) {
    if (sender == NULL || out == NULL || out_length == NULL)
        return RNS_ERROR_INVALID_ARGUMENT;
    size_t map_entries = sender->parts < RNS_RESOURCE_HASHMAP_MAX_ENTRIES
                             ? sender->parts : RNS_RESOURCE_HASHMAP_MAX_ENTRIES;
    size_t map_length = map_entries * 4U;
    size_t needed = 1U + 11U * 2U +
        mp_uint_size(sender->wire_length) + mp_uint_size(sender->source_length) +
        mp_uint_size(sender->parts) + mp_bin_header_size(32U) + 32U +
        mp_bin_header_size(4U) + 4U + mp_bin_header_size(32U) + 32U +
        mp_uint_size(sender->segment_index) + mp_uint_size(sender->total_segments) +
        (sender->has_request_id ? mp_bin_header_size(16U) + 16U : 1U) +
        mp_uint_size(sender->flags) + mp_bin_header_size(map_length) + map_length;
    if (capacity < needed) return RNS_ERROR_OVERFLOW;
    uint8_t *p = out;
    *p++ = 0x8bU;
    p = mp_put_key(p, 't'); p = mp_put_uint(p, sender->wire_length);
    p = mp_put_key(p, 'd'); p = mp_put_uint(p, sender->source_length);
    p = mp_put_key(p, 'n'); p = mp_put_uint(p, sender->parts);
    p = mp_put_key(p, 'h'); p = mp_put_bin(p, sender->hash, 32U);
    p = mp_put_key(p, 'r'); p = mp_put_bin(p, sender->random_hash, 4U);
    p = mp_put_key(p, 'o'); p = mp_put_bin(p, sender->original_hash, 32U);
    p = mp_put_key(p, 'i'); p = mp_put_uint(p, sender->segment_index);
    p = mp_put_key(p, 'l'); p = mp_put_uint(p, sender->total_segments);
    p = mp_put_key(p, 'q');
    if (sender->has_request_id) p = mp_put_bin(p, sender->request_id, 16U);
    else *p++ = 0xc0U;
    p = mp_put_key(p, 'f'); p = mp_put_uint(p, sender->flags);
    p = mp_put_key(p, 'm'); p = mp_put_bin(p, sender->map_hashes, map_length);
    *out_length = (size_t)(p - out);
    return *out_length == needed ? RNS_OK : RNS_ERROR_PROTOCOL;
}

static rns_status_t parse_request(const rns_resource_sender_t *sender,
                                  const uint8_t *request, size_t length,
                                  size_t *hash_offset, size_t *maps_offset) {
    if (sender == NULL || !valid_span(request, length) || length == 0U)
        return RNS_ERROR_INVALID_ARGUMENT;
    if (request[0] == 0x00U) {
        if (length < 33U || (length - 33U) % 4U != 0U) return RNS_ERROR_PROTOCOL;
        *hash_offset = 1U;
        *maps_offset = 33U;
    } else if (request[0] == 0xffU) {
        if (length < 37U || (length - 37U) % 4U != 0U) return RNS_ERROR_PROTOCOL;
        *hash_offset = 5U;
        *maps_offset = 37U;
    } else return RNS_ERROR_PROTOCOL;
    if (memcmp(request + *hash_offset, sender->hash, 32U) != 0)
        return RNS_ERROR_PROTOCOL;
    return RNS_OK;
}

rns_status_t rns_resource_sender_requested_parts(
    const rns_resource_sender_t *sender, const uint8_t *request,
    size_t request_length, size_t *indexes, size_t indexes_capacity,
    size_t *count) {
    if (indexes == NULL || count == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    *count = 0U;
    size_t hash_offset, maps_offset;
    rns_status_t status = parse_request(sender, request, request_length,
                                        &hash_offset, &maps_offset);
    (void)hash_offset;
    if (status != RNS_OK) return status;
    size_t requested = (request_length - maps_offset) / 4U;
    if (requested > RNS_RESOURCE_WINDOW_MAX) return RNS_ERROR_PROTOCOL;
    for (size_t j = 0U; j < requested; ++j) {
        const uint8_t *wanted = request + maps_offset + j * 4U;
        for (size_t i = 0U; i < sender->parts; ++i) {
            if (memcmp(wanted, sender->map_hashes + i * 4U, 4U) == 0) {
                if (*count >= indexes_capacity) return RNS_ERROR_OVERFLOW;
                indexes[(*count)++] = i;
                break;
            }
        }
    }
    return RNS_OK;
}

rns_status_t rns_resource_sender_hashmap_update(
    const rns_resource_sender_t *sender, const uint8_t *request,
    size_t request_length, uint8_t *out, size_t capacity,
    size_t *out_length) {
    if (out == NULL || out_length == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    size_t hash_offset, maps_offset;
    rns_status_t status = parse_request(sender, request, request_length,
                                        &hash_offset, &maps_offset);
    (void)hash_offset;
    (void)maps_offset;
    if (status != RNS_OK) return status;
    if (request[0] != 0xffU) return RNS_ERROR_NOT_FOUND;
    size_t anchor_index = SIZE_MAX;
    for (size_t i = 0U; i < sender->parts; ++i) {
        if (memcmp(request + 1U, sender->map_hashes + i * 4U, 4U) == 0) {
            anchor_index = i;
            break;
        }
    }
    if (anchor_index == SIZE_MAX ||
        (anchor_index + 1U) % RNS_RESOURCE_HASHMAP_MAX_ENTRIES != 0U)
        return RNS_ERROR_PROTOCOL;
    size_t page = (anchor_index + 1U) / RNS_RESOURCE_HASHMAP_MAX_ENTRIES;
    size_t start = page * RNS_RESOURCE_HASHMAP_MAX_ENTRIES;
    if (start >= sender->parts) return RNS_ERROR_NOT_FOUND;
    size_t entries = sender->parts - start;
    if (entries > RNS_RESOURCE_HASHMAP_MAX_ENTRIES)
        entries = RNS_RESOURCE_HASHMAP_MAX_ENTRIES;
    size_t map_length = entries * 4U;
    size_t needed = 32U + 1U + mp_uint_size(page) +
                    mp_bin_header_size(map_length) + map_length;
    if (capacity < needed) return RNS_ERROR_OVERFLOW;
    memcpy(out, sender->hash, 32U);
    uint8_t *p = out + 32U;
    *p++ = 0x92U;
    p = mp_put_uint(p, page);
    p = mp_put_bin(p, sender->map_hashes + start * 4U, map_length);
    *out_length = (size_t)(p - out);
    return RNS_OK;
}

rns_status_t rns_resource_sender_part(const rns_resource_sender_t *sender,
                                      size_t index, const uint8_t **part,
                                      size_t *part_length) {
    if (sender == NULL || part == NULL || part_length == NULL)
        return RNS_ERROR_INVALID_ARGUMENT;
    if (index >= sender->parts) return RNS_ERROR_NOT_FOUND;
    size_t offset = index * RNS_RESOURCE_PART_MAX;
    size_t length = sender->wire_length - offset;
    if (length > RNS_RESOURCE_PART_MAX) length = RNS_RESOURCE_PART_MAX;
    *part = sender->wire + offset;
    *part_length = length;
    return RNS_OK;
}

rns_status_t rns_resource_sender_validate_proof(
    const rns_resource_sender_t *sender, const uint8_t *proof,
    size_t proof_length) {
    if (sender == NULL || proof == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    if (proof_length != RNS_RESOURCE_PROOF_SIZE ||
        memcmp(proof, sender->hash, 32U) != 0)
        return RNS_ERROR_CRYPTO;
    size_t segment_length = segment_source_length(sender, sender->segment_index);
    size_t offset = (sender->segment_index - 1U) *
                    RNS_RESOURCE_SINGLE_SEGMENT_MAX_SIZE;
    size_t input_length;
    if (!add_size(segment_length, 32U, &input_length)) return RNS_ERROR_OVERFLOW;
    uint8_t *input = malloc(input_length);
    if (input == NULL) return RNS_ERROR_NO_MEMORY;
    memcpy(input, sender->source + offset, segment_length);
    memcpy(input + segment_length, sender->hash, 32U);
    uint8_t digest[32];
    int ok = rns_sha256(input, input_length, digest);
    free(input);
    if (!ok) return RNS_ERROR_CRYPTO;
    return memcmp(digest, proof + 32U, 32U) == 0
               ? RNS_OK : RNS_ERROR_CRYPTO;
}

rns_status_t rns_resource_sender_advance_segment(rns_resource_sender_t *sender,
                                                 const rns_link *link) {
    if (sender == NULL || link == NULL || link->state != RNS_LINK_ACTIVE)
        return RNS_ERROR_INVALID_ARGUMENT;
    if (sender->segment_index >= sender->total_segments)
        return RNS_ERROR_INVALID_STATE;
    rns_resource_sender_t next = *sender;
    next.segment_index++;
    next.wire = NULL;
    next.wire_length = 0U;
    next.parts = 0U;
    next.map_hashes = NULL;
    rns_status_t status = sender_prepare_segment(&next, link);
    if (status != RNS_OK) {
        free(next.map_hashes);
        free(next.wire);
        return status;
    }
    free(sender->map_hashes);
    free(sender->wire);
    *sender = next;
    return RNS_OK;
}

const uint8_t *rns_resource_sender_hash(const rns_resource_sender_t *sender) {
    return sender != NULL ? sender->hash : NULL;
}

size_t rns_resource_sender_data_size(const rns_resource_sender_t *sender) {
    return sender != NULL ? sender->source_length : 0U;
}

size_t rns_resource_sender_transfer_size(const rns_resource_sender_t *sender) {
    return sender != NULL ? sender->wire_length : 0U;
}

size_t rns_resource_sender_total_parts(const rns_resource_sender_t *sender) {
    return sender != NULL ? sender->parts : 0U;
}

size_t rns_resource_sender_total_data_parts(const rns_resource_sender_t *sender) {
    return sender != NULL ? sender->total_data_parts : 0U;
}

size_t rns_resource_sender_segment_index(const rns_resource_sender_t *sender) {
    return sender != NULL ? sender->segment_index : 0U;
}

size_t rns_resource_sender_total_segments(const rns_resource_sender_t *sender) {
    return sender != NULL ? sender->total_segments : 0U;
}
