#include "reticulum/resource.h"

#include "reticulum/crypto.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifdef RETICULUM_HAVE_BZIP2
#include <bzlib.h>
#endif

#define HASHMAP_IS_NOT_EXHAUSTED 0x00u
#define HASHMAP_IS_EXHAUSTED 0xffu

struct rns_resource {
    rns_resource_advertisement_t advertisement;
    uint8_t *hashmap;
    bool *hash_known;
    uint8_t *parts;
    uint16_t *part_length;
    bool *received;
    size_t part_stride;
    size_t hashmap_height;
    bool waiting_for_hashmap;
    size_t received_count;
    size_t consecutive_height;
    size_t max_size;
    uint8_t proof[RNS_RESOURCE_HASH_SIZE];
    bool assembled;
};

struct rns_resource_sender {
    uint8_t *source;
    size_t source_length;
    uint8_t *stream;
    size_t stream_length;
    size_t data_length;
    size_t parts;
    size_t part_size;
    uint8_t random_hash[RNS_RESOURCE_RANDOM_HASH_SIZE];
    uint8_t hash[RNS_RESOURCE_HASH_SIZE];
    uint8_t expected_proof[RNS_RESOURCE_HASH_SIZE];
    uint8_t *hashmap;
    uint8_t request_id[RNS_RESOURCE_REQUEST_ID_SIZE];
    bool has_request_id;
    bool compressed;
    bool auto_compress;
    bool is_response;
    size_t segment_index;
    size_t total_segments;
    size_t total_parts;
    uint8_t original_hash[RNS_RESOURCE_HASH_SIZE];
};

/* ------------------------------------------------------- msgpack decoding */

typedef struct {
    const uint8_t *data;
    size_t length;
    size_t offset;
} reader_t;

static bool take(reader_t *reader, size_t count, const uint8_t **out) {
    if (count > reader->length - reader->offset) return false;
    *out = reader->data + reader->offset;
    reader->offset += count;
    return true;
}

static bool read_byte(reader_t *reader, uint8_t *out) {
    const uint8_t *p;
    if (!take(reader, 1u, &p)) return false;
    *out = *p;
    return true;
}

static bool read_unsigned(reader_t *reader, uint8_t code, uint64_t *out) {
    const uint8_t *p;
    size_t width;
    if (code < 0x80u) { *out = code; return true; }
    switch (code) {
        case 0xccu: width = 1u; break;
        case 0xcdu: width = 2u; break;
        case 0xceu: width = 4u; break;
        case 0xcfu: width = 8u; break;
        default: return false;
    }
    if (!take(reader, width, &p)) return false;
    *out = 0u;
    for (size_t i = 0u; i < width; ++i) *out = (*out << 8u) | p[i];
    return true;
}

/* Reads a bin or str payload. */
static bool read_bytes(reader_t *reader, uint8_t code, const uint8_t **bytes,
                       size_t *length) {
    const uint8_t *p;
    uint64_t size;
    if ((code & 0xe0u) == 0xa0u) size = code & 0x1fu;
    else if (code == 0xc4u || code == 0xd9u) {
        uint8_t v;
        if (!read_byte(reader, &v)) return false;
        size = v;
    } else if (code == 0xc5u || code == 0xdau) {
        if (!take(reader, 2u, &p)) return false;
        size = ((uint64_t)p[0] << 8u) | p[1];
    } else if (code == 0xc6u || code == 0xdbu) {
        if (!take(reader, 4u, &p)) return false;
        size = ((uint64_t)p[0] << 24u) | ((uint64_t)p[1] << 16u) |
               ((uint64_t)p[2] << 8u) | p[3];
    } else return false;
    if (size > reader->length - reader->offset) return false;
    *bytes = reader->data + reader->offset;
    *length = (size_t)size;
    reader->offset += (size_t)size;
    return true;
}

rns_status_t rns_resource_advertisement_parse(
    const uint8_t *msgpack, size_t length,
    rns_resource_advertisement_t *advertisement) {
    reader_t reader = {msgpack, length, 0u};
    uint8_t code = 0u;
    bool transfer_seen = false, data_seen = false, parts_seen = false;
    bool hash_seen = false, original_seen = false, random_seen = false;
    bool segment_seen = false, segments_seen = false, flags_seen = false;
    bool request_seen = false, hashmap_seen = false;
    if (msgpack == NULL || advertisement == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    memset(advertisement, 0, sizeof *advertisement);
    if (!read_byte(&reader, &code) || (code & 0xf0u) != 0x80u)
        return RNS_ERROR_PROTOCOL;
    size_t entries = code & 0x0fu;
    for (size_t i = 0u; i < entries; ++i) {
        const uint8_t *key;
        size_t key_length;
        uint8_t key_code;
        if (!read_byte(&reader, &key_code) ||
            !read_bytes(&reader, key_code, &key, &key_length) || key_length != 1u)
            return RNS_ERROR_PROTOCOL;
        if (!read_byte(&reader, &code)) return RNS_ERROR_PROTOCOL;
        char name = (char)key[0];
        bool *seen = NULL;
        switch (name) {
            case 't': seen = &transfer_seen; break;
            case 'd': seen = &data_seen; break;
            case 'n': seen = &parts_seen; break;
            case 'h': seen = &hash_seen; break;
            case 'r': seen = &random_seen; break;
            case 'o': seen = &original_seen; break;
            case 'i': seen = &segment_seen; break;
            case 'l': seen = &segments_seen; break;
            case 'q': seen = &request_seen; break;
            case 'f': seen = &flags_seen; break;
            case 'm': seen = &hashmap_seen; break;
            default: break;
        }
        if (seen != NULL && *seen) return RNS_ERROR_PROTOCOL;
        if (seen != NULL) *seen = true;
        bool bytes_expected = name == 'h' || name == 'r' || name == 'o' ||
                              name == 'q' || name == 'm';
        bool unsigned_expected = name == 't' || name == 'd' || name == 'n' ||
                                 name == 'i' || name == 'l' || name == 'f';
        if (code == 0xc4u || code == 0xc5u || code == 0xc6u || code == 0xd9u ||
            code == 0xdau || code == 0xdbu || (code & 0xe0u) == 0xa0u) {
            if (unsigned_expected) return RNS_ERROR_PROTOCOL;
            if (bytes_expected && code != 0xc4u && code != 0xc5u &&
                code != 0xc6u)
                return RNS_ERROR_PROTOCOL;
            const uint8_t *value;
            size_t value_length;
            if (!read_bytes(&reader, code, &value, &value_length))
                return RNS_ERROR_PROTOCOL;
            switch (name) {
                case 'h':
                    if (value_length != sizeof advertisement->hash)
                        return RNS_ERROR_PROTOCOL;
                    memcpy(advertisement->hash, value, value_length);
                    break;
                case 'o':
                    if (value_length != sizeof advertisement->original_hash)
                        return RNS_ERROR_PROTOCOL;
                    memcpy(advertisement->original_hash, value, value_length);
                    break;
                case 'r':
                    if (value_length != sizeof advertisement->random_hash)
                        return RNS_ERROR_PROTOCOL;
                    memcpy(advertisement->random_hash, value, value_length);
                    break;
                case 'q':
                    if (value_length != RNS_RESOURCE_REQUEST_ID_SIZE)
                        return RNS_ERROR_PROTOCOL;
                    memcpy(advertisement->request_id, value, value_length);
                    advertisement->has_request_id = true;
                    break;
                case 'm':
                    advertisement->hashmap = value;
                    advertisement->hashmap_length = value_length;
                    break;
                default: break;
            }
        } else if (code == 0xc0u) {
            /* nil, for example an absent request id */
            if (name != 'q' && seen != NULL) return RNS_ERROR_PROTOCOL;
        } else if (code == 0xc2u || code == 0xc3u) {
            if (seen != NULL) return RNS_ERROR_PROTOCOL;
            if (name == 'c' && code == 0xc3u) advertisement->compressed = true;
            if (name == 'e' && code == 0xc3u) advertisement->encrypted = true;
        } else {
            if (bytes_expected) return RNS_ERROR_PROTOCOL;
            uint64_t value;
            if (!read_unsigned(&reader, code, &value)) return RNS_ERROR_PROTOCOL;
            switch (name) {
                case 't':
                    if (value > SIZE_MAX) return RNS_ERROR_OVERFLOW;
                    advertisement->transfer_size = (size_t)value;
                    break;
                case 'd':
                    if (value > SIZE_MAX) return RNS_ERROR_OVERFLOW;
                    advertisement->data_size = (size_t)value;
                    break;
                case 'n':
                    if (value > SIZE_MAX) return RNS_ERROR_OVERFLOW;
                    advertisement->parts = (size_t)value;
                    break;
                case 'i':
                    if (value > SIZE_MAX) return RNS_ERROR_OVERFLOW;
                    advertisement->segment_index = (size_t)value;
                    break;
                case 'l':
                    if (value > SIZE_MAX) return RNS_ERROR_OVERFLOW;
                    advertisement->total_segments = (size_t)value;
                    break;
                case 'f':
                    if (value > UINT8_MAX) return RNS_ERROR_OVERFLOW;
                    advertisement->flags = (uint8_t)value;
                    break;
                default: break;
            }
        }
    }
    if (reader.offset != reader.length) return RNS_ERROR_PROTOCOL;
    uint8_t flags = advertisement->flags;
    advertisement->encrypted = (flags & RNS_RESOURCE_FLAG_ENCRYPTED) != 0u;
    advertisement->compressed = (flags & RNS_RESOURCE_FLAG_COMPRESSED) != 0u;
    advertisement->split = (flags & RNS_RESOURCE_FLAG_SPLIT) != 0u;
    advertisement->is_request = (flags & RNS_RESOURCE_FLAG_REQUEST) != 0u;
    advertisement->is_response = (flags & RNS_RESOURCE_FLAG_RESPONSE) != 0u;
    advertisement->has_metadata = (flags & RNS_RESOURCE_FLAG_METADATA) != 0u;
    if (!transfer_seen || !data_seen || !parts_seen || !hash_seen ||
        !original_seen || !random_seen || !segment_seen || !segments_seen ||
        !request_seen || !flags_seen || !hashmap_seen ||
        advertisement->parts == 0u ||
        advertisement->transfer_size == 0u ||
        advertisement->data_size == 0u || advertisement->segment_index == 0u ||
        advertisement->total_segments == 0u ||
        advertisement->segment_index > advertisement->total_segments)
        return RNS_ERROR_PROTOCOL;
    return RNS_OK;
}

/* ------------------------------------------------------------- receiving */

rns_status_t rns_resource_accept(rns_resource_t **output,
                                 const rns_resource_advertisement_t *advertisement,
                                 size_t max_size) {
    if (output == NULL || advertisement == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    *output = NULL;
    size_t effective_max =
        max_size != 0u ? max_size : RNS_RESOURCE_DEFAULT_MAX_SIZE;
    if (effective_max > RNS_RESOURCE_MAX_SIZE)
        effective_max = RNS_RESOURCE_MAX_SIZE;
    if (advertisement->data_size > effective_max ||
        advertisement->transfer_size >
            RNS_RESOURCE_SINGLE_SEGMENT_MAX_SIZE + 128u)
        return RNS_ERROR_OVERFLOW;
    size_t maximum_segments =
        (effective_max + RNS_RESOURCE_SINGLE_SEGMENT_MAX_SIZE - 1u) /
        RNS_RESOURCE_SINGLE_SEGMENT_MAX_SIZE;
    if (advertisement->segment_index == 0u ||
        advertisement->total_segments == 0u ||
        advertisement->segment_index > advertisement->total_segments ||
        advertisement->total_segments > maximum_segments)
        return RNS_ERROR_PROTOCOL;
    if (advertisement->parts == 0u || advertisement->parts > RNS_RESOURCE_MAX_PARTS)
        return RNS_ERROR_UNSUPPORTED;
    size_t minimum_parts =
        (advertisement->transfer_size + RNS_RESOURCE_PART_MAX - 1u) /
        RNS_RESOURCE_PART_MAX;
    if (advertisement->parts < minimum_parts ||
        advertisement->parts > advertisement->transfer_size)
        return RNS_ERROR_PROTOCOL;
    if (advertisement->hashmap == NULL ||
        advertisement->hashmap_length == 0u ||
        advertisement->hashmap_length % RNS_RESOURCE_MAPHASH_LEN != 0u ||
        advertisement->hashmap_length >
            RNS_RESOURCE_HASHMAP_MAX_ENTRIES * RNS_RESOURCE_MAPHASH_LEN ||
        advertisement->hashmap_length >
            advertisement->parts * RNS_RESOURCE_MAPHASH_LEN)
        return RNS_ERROR_PROTOCOL;
    size_t stride = RNS_RESOURCE_PART_MAX;
    rns_resource_t *resource = calloc(1u, sizeof *resource);
    if (resource == NULL) return RNS_ERROR_NO_MEMORY;
    resource->hashmap = calloc(advertisement->parts, RNS_RESOURCE_MAPHASH_LEN);
    resource->hash_known = calloc(advertisement->parts, sizeof *resource->hash_known);
    resource->parts = calloc(advertisement->parts, stride);
    resource->part_length = calloc(advertisement->parts,
                                   sizeof *resource->part_length);
    resource->received = calloc(advertisement->parts,
                                sizeof *resource->received);
    if (resource->hashmap == NULL || resource->hash_known == NULL ||
        resource->parts == NULL || resource->part_length == NULL ||
        resource->received == NULL) {
        rns_resource_destroy(resource);
        return RNS_ERROR_NO_MEMORY;
    }
    resource->advertisement = *advertisement;
    memcpy(resource->hashmap, advertisement->hashmap, advertisement->hashmap_length);
    size_t initial_hashes = advertisement->hashmap_length /
                            RNS_RESOURCE_MAPHASH_LEN;
    for (size_t i = 0u; i < initial_hashes; ++i) resource->hash_known[i] = true;
    resource->hashmap_height = initial_hashes;
    resource->advertisement.hashmap = resource->hashmap;
    resource->advertisement.hashmap_length =
        advertisement->parts * RNS_RESOURCE_MAPHASH_LEN;
    resource->part_stride = stride;
    resource->max_size = effective_max;
    *output = resource;
    return RNS_OK;
}

void rns_resource_destroy(rns_resource_t *resource) {
    if (resource == NULL) return;
    free(resource->hashmap);
    free(resource->hash_known);
    free(resource->parts);
    free(resource->part_length);
    free(resource->received);
    free(resource);
}

rns_status_t rns_resource_build_request(rns_resource_t *resource,
                                        uint8_t *output, size_t capacity,
                                        size_t *output_length) {
    if (resource == NULL || output == NULL || output_length == NULL)
        return RNS_ERROR_INVALID_ARGUMENT;
    size_t offset = 1u;
    if (capacity < 1u + RNS_RESOURCE_HASH_SIZE) return RNS_ERROR_OVERFLOW;
    memcpy(output + offset, resource->advertisement.hash, RNS_RESOURCE_HASH_SIZE);
    offset += RNS_RESOURCE_HASH_SIZE;
    size_t requested = 0u;
    size_t start = resource->consecutive_height;
    bool exhausted = false;
    for (size_t i = start; i < resource->advertisement.parts &&
                           requested < RNS_RESOURCE_WINDOW; ++i) {
        if (resource->received[i]) continue;
        if (!resource->hash_known[i]) {
            exhausted = true;
            break;
        }
        if (offset + RNS_RESOURCE_MAPHASH_LEN > capacity) break;
        memcpy(output + offset, resource->hashmap + i * RNS_RESOURCE_MAPHASH_LEN,
               RNS_RESOURCE_MAPHASH_LEN);
        offset += RNS_RESOURCE_MAPHASH_LEN;
        requested++;
    }
    if (exhausted) {
        if (resource->hashmap_height == 0u ||
            capacity - offset < RNS_RESOURCE_MAPHASH_LEN)
            return RNS_ERROR_OVERFLOW;
        memmove(output + 1u + RNS_RESOURCE_MAPHASH_LEN, output + 1u,
                offset - 1u);
        memcpy(output + 1u,
               resource->hashmap +
                   (resource->hashmap_height - 1u) * RNS_RESOURCE_MAPHASH_LEN,
               RNS_RESOURCE_MAPHASH_LEN);
        offset += RNS_RESOURCE_MAPHASH_LEN;
        output[0] = HASHMAP_IS_EXHAUSTED;
        resource->waiting_for_hashmap = true;
    } else {
        output[0] = HASHMAP_IS_NOT_EXHAUSTED;
    }
    if (requested == 0u && !exhausted) return RNS_ERROR_INVALID_STATE;
    *output_length = offset;
    return RNS_OK;
}

rns_status_t rns_resource_receive_part(rns_resource_t *resource,
                                       const uint8_t *data, size_t length) {
    uint8_t digest[RNS_SHA256_SIZE];
    uint8_t buffer[RNS_RESOURCE_PART_MAX + RNS_RESOURCE_RANDOM_HASH_SIZE];
    if (resource == NULL || data == NULL || length == 0u)
        return RNS_ERROR_INVALID_ARGUMENT;
    if (length > resource->part_stride) return RNS_ERROR_OVERFLOW;
    memcpy(buffer, data, length);
    memcpy(buffer + length, resource->advertisement.random_hash,
           RNS_RESOURCE_RANDOM_HASH_SIZE);
    if (!rns_sha256(buffer, length + RNS_RESOURCE_RANDOM_HASH_SIZE, digest))
        return RNS_ERROR_CRYPTO;
    for (size_t i = 0u; i < resource->advertisement.parts; ++i) {
        if (!resource->hash_known[i]) continue;
        if (memcmp(digest, resource->hashmap + i * RNS_RESOURCE_MAPHASH_LEN,
                   RNS_RESOURCE_MAPHASH_LEN) != 0) continue;
        if (resource->received[i]) return RNS_OK;
        memcpy(resource->parts + i * resource->part_stride, data, length);
        resource->part_length[i] = (uint16_t)length;
        resource->received[i] = true;
        resource->received_count++;
        while (resource->consecutive_height < resource->advertisement.parts &&
               resource->received[resource->consecutive_height])
            resource->consecutive_height++;
        return RNS_OK;
    }
    return RNS_ERROR_NOT_FOUND;
}

rns_status_t rns_resource_apply_hashmap_update(rns_resource_t *resource,
                                               const uint8_t *update,
                                               size_t update_length) {
    if (resource == NULL || update == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    if (update_length <= RNS_RESOURCE_HASH_SIZE ||
        memcmp(update, resource->advertisement.hash,
               RNS_RESOURCE_HASH_SIZE) != 0)
        return RNS_ERROR_PROTOCOL;
    reader_t reader = {update + RNS_RESOURCE_HASH_SIZE,
                       update_length - RNS_RESOURCE_HASH_SIZE, 0u};
    uint8_t code;
    uint64_t page;
    const uint8_t *hashes;
    size_t hashes_length;
    if (!read_byte(&reader, &code) || code != 0x92u ||
        !read_byte(&reader, &code) || !read_unsigned(&reader, code, &page) ||
        !read_byte(&reader, &code) ||
        !read_bytes(&reader, code, &hashes, &hashes_length) ||
        reader.offset != reader.length || hashes_length == 0u ||
        hashes_length % RNS_RESOURCE_MAPHASH_LEN != 0u ||
        hashes_length >
            RNS_RESOURCE_HASHMAP_MAX_ENTRIES * RNS_RESOURCE_MAPHASH_LEN ||
        page > SIZE_MAX / RNS_RESOURCE_HASHMAP_MAX_ENTRIES)
        return RNS_ERROR_PROTOCOL;
    size_t start = (size_t)page * RNS_RESOURCE_HASHMAP_MAX_ENTRIES;
    size_t count = hashes_length / RNS_RESOURCE_MAPHASH_LEN;
    if (start >= resource->advertisement.parts ||
        count > resource->advertisement.parts - start)
        return RNS_ERROR_PROTOCOL;
    for (size_t i = 0u; i < count; ++i) {
        size_t index = start + i;
        const uint8_t *map_hash = hashes + i * RNS_RESOURCE_MAPHASH_LEN;
        if (resource->hash_known[index] &&
            memcmp(resource->hashmap + index * RNS_RESOURCE_MAPHASH_LEN,
                   map_hash, RNS_RESOURCE_MAPHASH_LEN) != 0)
            return RNS_ERROR_PROTOCOL;
    }
    for (size_t i = 0u; i < count; ++i) {
        size_t index = start + i;
        if (!resource->hash_known[index]) resource->hash_known[index] = true;
        memcpy(resource->hashmap + index * RNS_RESOURCE_MAPHASH_LEN,
               hashes + i * RNS_RESOURCE_MAPHASH_LEN,
               RNS_RESOURCE_MAPHASH_LEN);
    }
    while (resource->hashmap_height < resource->advertisement.parts &&
           resource->hash_known[resource->hashmap_height])
        resource->hashmap_height++;
    resource->waiting_for_hashmap = false;
    return RNS_OK;
}

bool rns_resource_waiting_for_hashmap(const rns_resource_t *resource) {
    return resource != NULL && resource->waiting_for_hashmap;
}

bool rns_resource_parts_complete(const rns_resource_t *resource) {
    return resource != NULL &&
           resource->received_count == resource->advertisement.parts;
}

size_t rns_resource_received_parts(const rns_resource_t *resource) {
    return resource != NULL ? resource->received_count : 0u;
}

size_t rns_resource_total_parts(const rns_resource_t *resource) {
    return resource != NULL ? resource->advertisement.parts : 0u;
}

const uint8_t *rns_resource_request_id(const rns_resource_t *resource) {
    return resource != NULL && resource->advertisement.has_request_id
               ? resource->advertisement.request_id : NULL;
}

bool rns_resource_is_response(const rns_resource_t *resource) {
    return resource != NULL && resource->advertisement.is_response;
}

bool rns_resource_decompression_available(void) {
#ifdef RETICULUM_HAVE_BZIP2
    return true;
#else
    return false;
#endif
}

static rns_status_t decompress(const uint8_t *input, size_t input_length,
                               uint8_t *output, size_t capacity,
                               size_t *output_length) {
#ifdef RETICULUM_HAVE_BZIP2
    unsigned int produced = capacity > UINT_MAX ? UINT_MAX : (unsigned int)capacity;
    int result = BZ2_bzBuffToBuffDecompress((char *)output, &produced,
                                            (char *)(uintptr_t)input,
                                            input_length > UINT_MAX
                                                ? UINT_MAX : (unsigned int)input_length,
                                            0, 0);
    if (result == BZ_OUTBUFF_FULL) return RNS_ERROR_OVERFLOW;
    if (result != BZ_OK) return RNS_ERROR_PROTOCOL;
    *output_length = produced;
    return RNS_OK;
#else
    (void)input; (void)input_length; (void)output; (void)capacity; (void)output_length;
    return RNS_ERROR_UNSUPPORTED;
#endif
}

rns_status_t rns_resource_assemble(rns_resource_t *resource, const rns_link *link,
                                   uint8_t *output, size_t capacity,
                                   size_t *output_length) {
    if (resource == NULL || output == NULL || output_length == NULL)
        return RNS_ERROR_INVALID_ARGUMENT;
    if (!rns_resource_parts_complete(resource)) return RNS_ERROR_INVALID_STATE;

    size_t stream_length = 0u;
    for (size_t i = 0u; i < resource->advertisement.parts; ++i)
        stream_length += resource->part_length[i];
    uint8_t *stream = malloc(stream_length != 0u ? stream_length : 1u);
    if (stream == NULL) return RNS_ERROR_NO_MEMORY;
    size_t offset = 0u;
    for (size_t i = 0u; i < resource->advertisement.parts; ++i) {
        memcpy(stream + offset, resource->parts + i * resource->part_stride,
               resource->part_length[i]);
        offset += resource->part_length[i];
    }

    rns_status_t status = RNS_OK;
    uint8_t *plain = stream;
    size_t plain_length = stream_length;
    uint8_t *decrypted = NULL;
    if (resource->advertisement.encrypted) {
        if (link == NULL) { status = RNS_ERROR_INVALID_ARGUMENT; goto done; }
        decrypted = malloc(stream_length != 0u ? stream_length : 1u);
        if (decrypted == NULL) { status = RNS_ERROR_NO_MEMORY; goto done; }
        size_t decrypted_length = 0u;
        if (!rns_link_decrypt(link, stream, stream_length, decrypted, stream_length,
                              &decrypted_length)) {
            status = RNS_ERROR_CRYPTO;
            goto done;
        }
        plain = decrypted;
        plain_length = decrypted_length;
    }
    if (plain_length < RNS_RESOURCE_RANDOM_HASH_SIZE) {
        status = RNS_ERROR_PROTOCOL;
        goto done;
    }
    /* The sender prefixes the payload with an independent random value. */
    plain += RNS_RESOURCE_RANDOM_HASH_SIZE;
    plain_length -= RNS_RESOURCE_RANDOM_HASH_SIZE;

    if (resource->advertisement.compressed) {
        size_t produced = 0u;
        status = decompress(plain, plain_length, output,
                            capacity < resource->max_size ? capacity : resource->max_size,
                            &produced);
        if (status != RNS_OK) goto done;
        *output_length = produced;
    } else {
        if (plain_length > capacity || plain_length > resource->max_size) {
            status = RNS_ERROR_OVERFLOW;
            goto done;
        }
        memcpy(output, plain, plain_length);
        *output_length = plain_length;
    }

    /* The advertised hash covers the plaintext followed by the random hash. */
    uint8_t *verify = malloc(*output_length + RNS_RESOURCE_RANDOM_HASH_SIZE);
    if (verify == NULL) { status = RNS_ERROR_NO_MEMORY; goto done; }
    memcpy(verify, output, *output_length);
    memcpy(verify + *output_length, resource->advertisement.random_hash,
           RNS_RESOURCE_RANDOM_HASH_SIZE);
    uint8_t digest[RNS_SHA256_SIZE];
    bool hashed = rns_sha256(verify, *output_length + RNS_RESOURCE_RANDOM_HASH_SIZE,
                             digest) != 0;
    free(verify);
    if (!hashed) { status = RNS_ERROR_CRYPTO; goto done; }
    if (memcmp(digest, resource->advertisement.hash, RNS_RESOURCE_HASH_SIZE) != 0) {
        status = RNS_ERROR_PROTOCOL;
        goto done;
    }
    /* The proof commits to the plaintext under the advertised resource hash. */
    uint8_t *proof_input = malloc(*output_length + RNS_RESOURCE_HASH_SIZE);
    if (proof_input == NULL) { status = RNS_ERROR_NO_MEMORY; goto done; }
    memcpy(proof_input, output, *output_length);
    memcpy(proof_input + *output_length, resource->advertisement.hash,
           RNS_RESOURCE_HASH_SIZE);
    bool proved = rns_sha256(proof_input, *output_length + RNS_RESOURCE_HASH_SIZE,
                             resource->proof) != 0;
    free(proof_input);
    if (!proved) { status = RNS_ERROR_CRYPTO; goto done; }
    resource->assembled = true;
done:
    free(decrypted);
    free(stream);
    return status;
}

rns_status_t rns_resource_build_proof(const rns_resource_t *resource,
                                      uint8_t output[RNS_RESOURCE_PROOF_SIZE]) {
    if (resource == NULL || output == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    if (!resource->assembled) return RNS_ERROR_INVALID_STATE;
    memcpy(output, resource->advertisement.hash, RNS_RESOURCE_HASH_SIZE);
    memcpy(output + RNS_RESOURCE_HASH_SIZE, resource->proof, RNS_RESOURCE_HASH_SIZE);
    return RNS_OK;
}

/* --------------------------------------------------------------- sending */

static rns_status_t hash_join(const uint8_t *first, size_t first_length,
                              const uint8_t *second, size_t second_length,
                              uint8_t digest[RNS_SHA256_SIZE]) {
    if (first_length > SIZE_MAX - second_length) return RNS_ERROR_OVERFLOW;
    size_t length = first_length + second_length;
    uint8_t *joined = malloc(length != 0u ? length : 1u);
    if (joined == NULL) return RNS_ERROR_NO_MEMORY;
    if (first_length != 0u) memcpy(joined, first, first_length);
    if (second_length != 0u) memcpy(joined + first_length, second, second_length);
    bool ok = rns_sha256(joined, length, digest) != 0;
    free(joined);
    return ok ? RNS_OK : RNS_ERROR_CRYPTO;
}

static rns_status_t prepare_compressed(const uint8_t *data, size_t data_length,
                                       bool enabled, uint8_t **prepared,
                                       size_t *prepared_length,
                                       bool *compressed) {
    *prepared = NULL;
    *prepared_length = 0u;
    *compressed = false;
#ifdef RETICULUM_HAVE_BZIP2
    if (enabled && data_length <= UINT_MAX) {
        size_t bound = data_length + data_length / 100u + 601u;
        if (bound <= UINT_MAX) {
            uint8_t *candidate = malloc(bound != 0u ? bound : 1u);
            if (candidate == NULL) return RNS_ERROR_NO_MEMORY;
            unsigned int produced = (unsigned int)bound;
            int result = BZ2_bzBuffToBuffCompress(
                (char *)candidate, &produced, (char *)(uintptr_t)data,
                (unsigned int)data_length, 9, 0, 30);
            if (result == BZ_OK && (size_t)produced < data_length) {
                *prepared = candidate;
                *prepared_length = (size_t)produced;
                *compressed = true;
                return RNS_OK;
            }
            free(candidate);
        }
    }
#else
    (void)enabled;
#endif
    uint8_t *copy = malloc(data_length != 0u ? data_length : 1u);
    if (copy == NULL) return RNS_ERROR_NO_MEMORY;
    if (data_length != 0u) memcpy(copy, data, data_length);
    *prepared = copy;
    *prepared_length = data_length;
    return RNS_OK;
}

static rns_status_t sender_compute_hashes(rns_resource_sender_t *sender,
                                          const uint8_t *data,
                                          size_t data_length) {
    uint8_t digest[RNS_SHA256_SIZE];
    for (size_t attempt = 0u; attempt < 32u; ++attempt) {
        if (!rns_random_bytes(sender->random_hash,
                              sizeof sender->random_hash))
            return RNS_ERROR_CRYPTO;
        rns_status_t status = hash_join(data, data_length, sender->random_hash,
                                        sizeof sender->random_hash,
                                        sender->hash);
        if (status != RNS_OK) return status;
        bool collision = false;
        for (size_t i = 0u; i < sender->parts; ++i) {
            size_t offset = i * sender->part_size;
            size_t length = sender->stream_length - offset;
            if (length > sender->part_size) length = sender->part_size;
            status = hash_join(sender->stream + offset, length,
                               sender->random_hash,
                               sizeof sender->random_hash, digest);
            if (status != RNS_OK) return status;
            size_t guard_start = i >
                    (2u * 75u + RNS_RESOURCE_HASHMAP_MAX_ENTRIES)
                ? i - (2u * 75u + RNS_RESOURCE_HASHMAP_MAX_ENTRIES)
                : 0u;
            for (size_t previous = guard_start; previous < i; ++previous)
                if (memcmp(sender->hashmap +
                               previous * RNS_RESOURCE_MAPHASH_LEN,
                           digest, RNS_RESOURCE_MAPHASH_LEN) == 0) {
                    collision = true;
                    break;
                }
            if (collision) break;
            memcpy(sender->hashmap + i * RNS_RESOURCE_MAPHASH_LEN, digest,
                   RNS_RESOURCE_MAPHASH_LEN);
        }
        if (!collision)
            return hash_join(data, data_length, sender->hash,
                             sizeof sender->hash, sender->expected_proof);
    }
    return RNS_ERROR_CRYPTO;
}

static void sender_clear_segment(rns_resource_sender_t *sender) {
    free(sender->stream);
    sender->stream = NULL;
    sender->stream_length = 0u;
    free(sender->hashmap);
    sender->hashmap = NULL;
    sender->parts = 0u;
}

static rns_status_t sender_prepare_segment(rns_resource_sender_t *sender,
                                           const rns_link *link) {
    sender_clear_segment(sender);
    size_t source_offset = (sender->segment_index - 1u) *
                           RNS_RESOURCE_SINGLE_SEGMENT_MAX_SIZE;
    size_t segment_length = sender->source_length - source_offset;
    if (segment_length > RNS_RESOURCE_SINGLE_SEGMENT_MAX_SIZE)
        segment_length = RNS_RESOURCE_SINGLE_SEGMENT_MAX_SIZE;
    const uint8_t *segment = sender->source + source_offset;
    uint8_t *prepared = NULL;
    size_t prepared_length = 0u;
    rns_status_t status = prepare_compressed(
        segment, segment_length, sender->auto_compress, &prepared,
        &prepared_length, &sender->compressed);
    if (status != RNS_OK) return status;
    if (prepared_length > SIZE_MAX - RNS_RESOURCE_RANDOM_HASH_SIZE) {
        free(prepared);
        return RNS_ERROR_OVERFLOW;
    }
    size_t clear_length = prepared_length + RNS_RESOURCE_RANDOM_HASH_SIZE;
    uint8_t *clear = malloc(clear_length);
    if (clear == NULL) {
        free(prepared);
        return RNS_ERROR_NO_MEMORY;
    }
    if (!rns_random_bytes(clear, RNS_RESOURCE_RANDOM_HASH_SIZE)) {
        free(clear);
        free(prepared);
        return RNS_ERROR_CRYPTO;
    }
    memcpy(clear + RNS_RESOURCE_RANDOM_HASH_SIZE, prepared, prepared_length);
    size_t encrypted_capacity = clear_length + 128u;
    sender->stream = malloc(encrypted_capacity);
    if (sender->stream == NULL) {
        free(clear);
        free(prepared);
        return RNS_ERROR_NO_MEMORY;
    }
    if (!rns_link_encrypt(link, clear, clear_length, sender->stream,
                          encrypted_capacity, &sender->stream_length)) {
        free(clear);
        free(prepared);
        sender_clear_segment(sender);
        return RNS_ERROR_CRYPTO;
    }
    free(clear);
    free(prepared);
    sender->parts = (sender->stream_length + sender->part_size - 1u) /
                    sender->part_size;
    if (sender->parts == 0u || sender->parts > RNS_RESOURCE_MAX_PARTS) {
        sender_clear_segment(sender);
        return RNS_ERROR_UNSUPPORTED;
    }
    sender->hashmap = calloc(sender->parts, RNS_RESOURCE_MAPHASH_LEN);
    if (sender->hashmap == NULL) {
        sender_clear_segment(sender);
        return RNS_ERROR_NO_MEMORY;
    }
    status = sender_compute_hashes(sender, segment, segment_length);
    if (status != RNS_OK) {
        sender_clear_segment(sender);
        return status;
    }
    if (sender->segment_index == 1u)
        memcpy(sender->original_hash, sender->hash,
               sizeof sender->original_hash);
    sender->total_parts += sender->parts;
    return RNS_OK;
}

rns_status_t rns_resource_sender_create(
    rns_resource_sender_t **output, const rns_link *link,
    const uint8_t *data, size_t data_length,
    const rns_resource_sender_options_t *options) {
    if (output == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    *output = NULL;
    if (link == NULL || data == NULL || data_length == 0u ||
        data_length > RNS_RESOURCE_MAX_SIZE)
        return RNS_ERROR_INVALID_ARGUMENT;
    bool auto_compress = options == NULL || options->auto_compress;
    if (options != NULL && options->is_response && options->request_id == NULL)
        return RNS_ERROR_INVALID_ARGUMENT;
    rns_resource_sender_t *sender = calloc(1u, sizeof *sender);
    if (sender == NULL) return RNS_ERROR_NO_MEMORY;
    if (link->mtu <= RNS_RESOURCE_WIRE_OVERHEAD || link->mtu > 500u) {
        rns_resource_sender_destroy(sender);
        return RNS_ERROR_UNSUPPORTED;
    }
    sender->part_size = (size_t)link->mtu - RNS_RESOURCE_WIRE_OVERHEAD;
    sender->source = malloc(data_length);
    if (sender->source == NULL) {
        rns_resource_sender_destroy(sender);
        return RNS_ERROR_NO_MEMORY;
    }
    memcpy(sender->source, data, data_length);
    sender->source_length = data_length;
    sender->data_length = data_length;
    sender->auto_compress = auto_compress;
    sender->segment_index = 1u;
    sender->total_segments =
        (data_length + RNS_RESOURCE_SINGLE_SEGMENT_MAX_SIZE - 1u) /
        RNS_RESOURCE_SINGLE_SEGMENT_MAX_SIZE;
    if (options != NULL && options->request_id != NULL) {
        memcpy(sender->request_id, options->request_id,
               sizeof sender->request_id);
        sender->has_request_id = true;
        sender->is_response = options->is_response;
    }
    rns_status_t status = sender_prepare_segment(sender, link);
    if (status != RNS_OK) goto failed;
    *output = sender;
    return RNS_OK;

failed:
    rns_resource_sender_destroy(sender);
    return status;
}

void rns_resource_sender_destroy(rns_resource_sender_t *sender) {
    if (sender == NULL) return;
    sender_clear_segment(sender);
    free(sender->source);
    free(sender);
}

typedef struct resource_writer {
    uint8_t *data;
    size_t capacity;
    size_t offset;
} resource_writer_t;

static bool write_data(resource_writer_t *writer, const void *data,
                       size_t length) {
    if (length > writer->capacity - writer->offset) return false;
    memcpy(writer->data + writer->offset, data, length);
    writer->offset += length;
    return true;
}

static bool write_u8(resource_writer_t *writer, uint8_t value) {
    return write_data(writer, &value, 1u);
}

static bool write_uint(resource_writer_t *writer, size_t value) {
    if (value <= 0x7fu) return write_u8(writer, (uint8_t)value);
    if (value <= UINT8_MAX) {
        uint8_t encoded[2] = {0xccu, (uint8_t)value};
        return write_data(writer, encoded, sizeof encoded);
    }
    if (value <= UINT16_MAX) {
        uint8_t encoded[3] = {0xcdu, (uint8_t)(value >> 8u), (uint8_t)value};
        return write_data(writer, encoded, sizeof encoded);
    }
    if (value <= UINT32_MAX) {
        uint8_t encoded[5] = {0xceu, (uint8_t)(value >> 24u),
                              (uint8_t)(value >> 16u),
                              (uint8_t)(value >> 8u), (uint8_t)value};
        return write_data(writer, encoded, sizeof encoded);
    }
    return false;
}

static bool write_key(resource_writer_t *writer, char key) {
    uint8_t encoded[2] = {0xa1u, (uint8_t)key};
    return write_data(writer, encoded, sizeof encoded);
}

static bool write_bin(resource_writer_t *writer, const uint8_t *data,
                      size_t length) {
    if (length <= UINT8_MAX) {
        uint8_t header[2] = {0xc4u, (uint8_t)length};
        if (!write_data(writer, header, sizeof header)) return false;
    } else if (length <= UINT16_MAX) {
        uint8_t header[3] = {0xc5u, (uint8_t)(length >> 8u), (uint8_t)length};
        if (!write_data(writer, header, sizeof header)) return false;
    } else return false;
    return write_data(writer, data, length);
}

rns_status_t rns_resource_sender_advertisement(
    const rns_resource_sender_t *sender, uint8_t *output, size_t capacity,
    size_t *output_length) {
    if (sender == NULL || output == NULL || output_length == NULL)
        return RNS_ERROR_INVALID_ARGUMENT;
    resource_writer_t writer = {output, capacity, 0u};
    uint8_t flags = RNS_RESOURCE_FLAG_ENCRYPTED;
    if (sender->compressed) flags |= RNS_RESOURCE_FLAG_COMPRESSED;
    if (sender->total_segments > 1u) flags |= RNS_RESOURCE_FLAG_SPLIT;
    if (sender->has_request_id)
        flags |= sender->is_response ? RNS_RESOURCE_FLAG_RESPONSE
                                     : RNS_RESOURCE_FLAG_REQUEST;
    size_t advertised_parts = sender->parts;
    if (advertised_parts > RNS_RESOURCE_HASHMAP_MAX_ENTRIES)
        advertised_parts = RNS_RESOURCE_HASHMAP_MAX_ENTRIES;
    size_t hashmap_length = advertised_parts * RNS_RESOURCE_MAPHASH_LEN;
    if (!write_u8(&writer, 0x8bu) ||
        !write_key(&writer, 't') || !write_uint(&writer, sender->stream_length) ||
        !write_key(&writer, 'd') || !write_uint(&writer, sender->source_length) ||
        !write_key(&writer, 'n') || !write_uint(&writer, sender->parts) ||
        !write_key(&writer, 'h') || !write_bin(&writer, sender->hash, sizeof sender->hash) ||
        !write_key(&writer, 'r') || !write_bin(&writer, sender->random_hash, sizeof sender->random_hash) ||
        !write_key(&writer, 'o') || !write_bin(&writer, sender->original_hash, sizeof sender->original_hash) ||
        !write_key(&writer, 'i') || !write_uint(&writer, sender->segment_index) ||
        !write_key(&writer, 'l') || !write_uint(&writer, sender->total_segments) ||
        !write_key(&writer, 'q') ||
        !(sender->has_request_id
              ? write_bin(&writer, sender->request_id, sizeof sender->request_id)
              : write_u8(&writer, 0xc0u)) ||
        !write_key(&writer, 'f') || !write_uint(&writer, flags) ||
        !write_key(&writer, 'm') ||
        !write_bin(&writer, sender->hashmap, hashmap_length))
        return RNS_ERROR_OVERFLOW;
    *output_length = writer.offset;
    return RNS_OK;
}

rns_status_t rns_resource_sender_requested_parts(
    const rns_resource_sender_t *sender, const uint8_t *request,
    size_t request_length, size_t *part_indexes, size_t capacity,
    size_t *part_count) {
    if (sender == NULL || request == NULL || part_indexes == NULL ||
        part_count == NULL)
        return RNS_ERROR_INVALID_ARGUMENT;
    *part_count = 0u;
    bool exhausted = request_length > 0u &&
                     request[0] == HASHMAP_IS_EXHAUSTED;
    size_t pad = exhausted ? 1u + RNS_RESOURCE_MAPHASH_LEN : 1u;
    if (request_length < pad + RNS_RESOURCE_HASH_SIZE ||
        (request[0] != HASHMAP_IS_NOT_EXHAUSTED && !exhausted) ||
        (request_length - pad - RNS_RESOURCE_HASH_SIZE) %
                RNS_RESOURCE_MAPHASH_LEN !=
            0u ||
        memcmp(request + pad, sender->hash, sizeof sender->hash) != 0)
        return RNS_ERROR_PROTOCOL;
    size_t hashes = (request_length - pad - RNS_RESOURCE_HASH_SIZE) /
                    RNS_RESOURCE_MAPHASH_LEN;
    for (size_t requested = 0u; requested < hashes; ++requested) {
        const uint8_t *map_hash = request + pad + RNS_RESOURCE_HASH_SIZE +
                                  requested * RNS_RESOURCE_MAPHASH_LEN;
        for (size_t part = 0u; part < sender->parts; ++part) {
            if (memcmp(map_hash,
                       sender->hashmap + part * RNS_RESOURCE_MAPHASH_LEN,
                       RNS_RESOURCE_MAPHASH_LEN) != 0)
                continue;
            bool duplicate = false;
            for (size_t i = 0u; i < *part_count; ++i)
                if (part_indexes[i] == part) duplicate = true;
            if (!duplicate) {
                if (*part_count == capacity) return RNS_ERROR_OVERFLOW;
                part_indexes[(*part_count)++] = part;
            }
            break;
        }
    }
    return *part_count != 0u || exhausted ? RNS_OK : RNS_ERROR_NOT_FOUND;
}

rns_status_t rns_resource_sender_hashmap_update(
    const rns_resource_sender_t *sender, const uint8_t *request,
    size_t request_length, uint8_t *output, size_t capacity,
    size_t *output_length) {
    if (sender == NULL || request == NULL || output == NULL ||
        output_length == NULL)
        return RNS_ERROR_INVALID_ARGUMENT;
    *output_length = 0u;
    if (request_length < 1u + RNS_RESOURCE_MAPHASH_LEN +
                             RNS_RESOURCE_HASH_SIZE ||
        request[0] != HASHMAP_IS_EXHAUSTED ||
        (request_length - 1u - RNS_RESOURCE_MAPHASH_LEN -
         RNS_RESOURCE_HASH_SIZE) % RNS_RESOURCE_MAPHASH_LEN != 0u ||
        memcmp(request + 1u + RNS_RESOURCE_MAPHASH_LEN, sender->hash,
               RNS_RESOURCE_HASH_SIZE) != 0)
        return RNS_ERROR_NOT_FOUND;
    const uint8_t *last_hash = request + 1u;
    size_t page = 0u;
    bool found = false;
    for (size_t boundary = RNS_RESOURCE_HASHMAP_MAX_ENTRIES;
         boundary < sender->parts + RNS_RESOURCE_HASHMAP_MAX_ENTRIES;
         boundary += RNS_RESOURCE_HASHMAP_MAX_ENTRIES) {
        if (boundary > sender->parts) break;
        if (memcmp(sender->hashmap +
                       (boundary - 1u) * RNS_RESOURCE_MAPHASH_LEN,
                   last_hash, RNS_RESOURCE_MAPHASH_LEN) == 0) {
            if (found) return RNS_ERROR_PROTOCOL;
            page = boundary / RNS_RESOURCE_HASHMAP_MAX_ENTRIES;
            found = true;
        }
    }
    if (!found) return RNS_ERROR_PROTOCOL;
    size_t start = page * RNS_RESOURCE_HASHMAP_MAX_ENTRIES;
    if (start >= sender->parts) return RNS_ERROR_PROTOCOL;
    size_t count = sender->parts - start;
    if (count > RNS_RESOURCE_HASHMAP_MAX_ENTRIES)
        count = RNS_RESOURCE_HASHMAP_MAX_ENTRIES;
    resource_writer_t writer = {output, capacity, 0u};
    if (!write_data(&writer, sender->hash, sizeof sender->hash) ||
        !write_u8(&writer, 0x92u) || !write_uint(&writer, page) ||
        !write_bin(&writer,
                   sender->hashmap + start * RNS_RESOURCE_MAPHASH_LEN,
                   count * RNS_RESOURCE_MAPHASH_LEN))
        return RNS_ERROR_OVERFLOW;
    *output_length = writer.offset;
    return RNS_OK;
}

rns_status_t rns_resource_sender_part(
    const rns_resource_sender_t *sender, size_t part_index,
    const uint8_t **data, size_t *length) {
    if (sender == NULL || data == NULL || length == NULL ||
        part_index >= sender->parts)
        return RNS_ERROR_INVALID_ARGUMENT;
    size_t offset = part_index * sender->part_size;
    *length = sender->stream_length - offset;
    if (*length > sender->part_size) *length = sender->part_size;
    *data = sender->stream + offset;
    return RNS_OK;
}

rns_status_t rns_resource_sender_validate_proof(
    const rns_resource_sender_t *sender, const uint8_t *proof,
    size_t proof_length) {
    if (sender == NULL || proof == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    if (proof_length != RNS_RESOURCE_PROOF_SIZE ||
        memcmp(proof, sender->hash, RNS_RESOURCE_HASH_SIZE) != 0 ||
        memcmp(proof + RNS_RESOURCE_HASH_SIZE, sender->expected_proof,
               RNS_RESOURCE_HASH_SIZE) != 0)
        return RNS_ERROR_CRYPTO;
    return RNS_OK;
}

const uint8_t *rns_resource_sender_hash(const rns_resource_sender_t *sender) {
    return sender != NULL ? sender->hash : NULL;
}

size_t rns_resource_sender_total_parts(const rns_resource_sender_t *sender) {
    return sender != NULL ? sender->parts : 0u;
}

size_t rns_resource_sender_data_size(const rns_resource_sender_t *sender) {
    return sender != NULL ? sender->data_length : 0u;
}

size_t rns_resource_sender_transfer_size(const rns_resource_sender_t *sender) {
    return sender != NULL ? sender->stream_length : 0u;
}

size_t rns_resource_sender_segment_index(const rns_resource_sender_t *sender) {
    return sender != NULL ? sender->segment_index : 0u;
}

size_t rns_resource_sender_total_segments(const rns_resource_sender_t *sender) {
    return sender != NULL ? sender->total_segments : 0u;
}

size_t rns_resource_sender_total_data_parts(const rns_resource_sender_t *sender) {
    return sender != NULL ? sender->total_parts : 0u;
}

rns_status_t rns_resource_sender_advance_segment(rns_resource_sender_t *sender,
                                                 const rns_link *link) {
    if (sender == NULL || link == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    if (sender->segment_index >= sender->total_segments)
        return RNS_ERROR_INVALID_STATE;
    sender->segment_index++;
    rns_status_t status = sender_prepare_segment(sender, link);
    if (status != RNS_OK) sender->segment_index--;
    return status;
}
