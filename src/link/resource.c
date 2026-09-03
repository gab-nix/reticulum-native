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
    uint8_t hashmap[RNS_RESOURCE_MAX_PARTS * RNS_RESOURCE_MAPHASH_LEN];
    uint8_t *parts;
    uint16_t part_length[RNS_RESOURCE_MAX_PARTS];
    bool received[RNS_RESOURCE_MAX_PARTS];
    size_t received_count;
    size_t consecutive_height;
    size_t max_size;
    uint8_t proof[RNS_RESOURCE_HASH_SIZE];
    bool assembled;
};

struct rns_resource_sender {
    uint8_t *stream;
    size_t stream_length;
    size_t data_length;
    size_t parts;
    size_t part_size;
    uint8_t random_hash[RNS_RESOURCE_RANDOM_HASH_SIZE];
    uint8_t hash[RNS_RESOURCE_HASH_SIZE];
    uint8_t expected_proof[RNS_RESOURCE_HASH_SIZE];
    uint8_t hashmap[RNS_RESOURCE_MAX_PARTS * RNS_RESOURCE_MAPHASH_LEN];
    uint8_t request_id[RNS_RESOURCE_REQUEST_ID_SIZE];
    bool has_request_id;
    bool compressed;
    bool is_response;
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

static void copy_fixed(uint8_t *destination, size_t destination_size,
                       const uint8_t *source, size_t source_size) {
    memcpy(destination, source, source_size < destination_size ? source_size
                                                               : destination_size);
}

rns_status_t rns_resource_advertisement_parse(
    const uint8_t *msgpack, size_t length,
    rns_resource_advertisement_t *advertisement) {
    reader_t reader = {msgpack, length, 0u};
    uint8_t code = 0u;
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
        if (code == 0xc4u || code == 0xc5u || code == 0xc6u || code == 0xd9u ||
            code == 0xdau || code == 0xdbu || (code & 0xe0u) == 0xa0u) {
            const uint8_t *value;
            size_t value_length;
            if (!read_bytes(&reader, code, &value, &value_length))
                return RNS_ERROR_PROTOCOL;
            switch (name) {
                case 'h': copy_fixed(advertisement->hash, sizeof advertisement->hash,
                                     value, value_length); break;
                case 'o': copy_fixed(advertisement->original_hash,
                                     sizeof advertisement->original_hash,
                                     value, value_length); break;
                case 'r': copy_fixed(advertisement->random_hash,
                                     sizeof advertisement->random_hash,
                                     value, value_length); break;
                case 'q':
                    if (value_length == RNS_RESOURCE_REQUEST_ID_SIZE) {
                        memcpy(advertisement->request_id, value, value_length);
                        advertisement->has_request_id = true;
                    }
                    break;
                case 'm':
                    advertisement->hashmap = value;
                    advertisement->hashmap_length = value_length;
                    break;
                default: break;
            }
        } else if (code == 0xc0u) {
            /* nil, for example an absent request id */
        } else if (code == 0xc2u || code == 0xc3u) {
            if (name == 'c' && code == 0xc3u) advertisement->compressed = true;
            if (name == 'e' && code == 0xc3u) advertisement->encrypted = true;
        } else {
            uint64_t value;
            if (!read_unsigned(&reader, code, &value)) return RNS_ERROR_PROTOCOL;
            switch (name) {
                case 't': advertisement->transfer_size = (size_t)value; break;
                case 'd': advertisement->data_size = (size_t)value; break;
                case 'n': advertisement->parts = (size_t)value; break;
                case 'i': advertisement->segment_index = (size_t)value; break;
                case 'l': advertisement->total_segments = (size_t)value; break;
                case 'f': advertisement->flags = (uint8_t)value; break;
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
    if (advertisement->parts == 0u) return RNS_ERROR_PROTOCOL;
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
    if (advertisement->data_size > effective_max)
        return RNS_ERROR_OVERFLOW;
    if (advertisement->parts == 0u || advertisement->parts > RNS_RESOURCE_MAX_PARTS)
        return RNS_ERROR_UNSUPPORTED;
    /* Without a complete hashmap the missing part hashes need an HMU exchange. */
    if (advertisement->hashmap == NULL ||
        advertisement->hashmap_length != advertisement->parts * RNS_RESOURCE_MAPHASH_LEN)
        return RNS_ERROR_UNSUPPORTED;
    if (advertisement->split || advertisement->total_segments > 1u)
        return RNS_ERROR_UNSUPPORTED;
    rns_resource_t *resource = calloc(1u, sizeof *resource);
    if (resource == NULL) return RNS_ERROR_NO_MEMORY;
    resource->parts = calloc(advertisement->parts, RNS_RESOURCE_PART_MAX);
    if (resource->parts == NULL) {
        free(resource);
        return RNS_ERROR_NO_MEMORY;
    }
    resource->advertisement = *advertisement;
    memcpy(resource->hashmap, advertisement->hashmap, advertisement->hashmap_length);
    resource->advertisement.hashmap = resource->hashmap;
    resource->max_size = effective_max;
    *output = resource;
    return RNS_OK;
}

void rns_resource_destroy(rns_resource_t *resource) {
    if (resource == NULL) return;
    free(resource->parts);
    free(resource);
}

rns_status_t rns_resource_build_request(const rns_resource_t *resource,
                                        uint8_t *output, size_t capacity,
                                        size_t *output_length) {
    if (resource == NULL || output == NULL || output_length == NULL)
        return RNS_ERROR_INVALID_ARGUMENT;
    size_t offset = 0u;
    if (capacity < 1u + RNS_RESOURCE_HASH_SIZE) return RNS_ERROR_OVERFLOW;
    output[offset++] = HASHMAP_IS_NOT_EXHAUSTED;
    memcpy(output + offset, resource->advertisement.hash, RNS_RESOURCE_HASH_SIZE);
    offset += RNS_RESOURCE_HASH_SIZE;
    size_t requested = 0u;
    size_t start = resource->consecutive_height;
    for (size_t i = start; i < resource->advertisement.parts &&
                           requested < RNS_RESOURCE_WINDOW; ++i) {
        if (resource->received[i]) continue;
        if (offset + RNS_RESOURCE_MAPHASH_LEN > capacity) break;
        memcpy(output + offset, resource->hashmap + i * RNS_RESOURCE_MAPHASH_LEN,
               RNS_RESOURCE_MAPHASH_LEN);
        offset += RNS_RESOURCE_MAPHASH_LEN;
        requested++;
    }
    if (requested == 0u) return RNS_ERROR_INVALID_STATE;
    *output_length = offset;
    return RNS_OK;
}

rns_status_t rns_resource_receive_part(rns_resource_t *resource,
                                       const uint8_t *data, size_t length) {
    uint8_t digest[RNS_SHA256_SIZE];
    uint8_t buffer[RNS_RESOURCE_PART_MAX + RNS_RESOURCE_RANDOM_HASH_SIZE];
    if (resource == NULL || data == NULL || length == 0u)
        return RNS_ERROR_INVALID_ARGUMENT;
    if (length > RNS_RESOURCE_PART_MAX) return RNS_ERROR_OVERFLOW;
    memcpy(buffer, data, length);
    memcpy(buffer + length, resource->advertisement.random_hash,
           RNS_RESOURCE_RANDOM_HASH_SIZE);
    if (!rns_sha256(buffer, length + RNS_RESOURCE_RANDOM_HASH_SIZE, digest))
        return RNS_ERROR_CRYPTO;
    for (size_t i = 0u; i < resource->advertisement.parts; ++i) {
        if (resource->received[i]) continue;
        if (memcmp(digest, resource->hashmap + i * RNS_RESOURCE_MAPHASH_LEN,
                   RNS_RESOURCE_MAPHASH_LEN) != 0) continue;
        memcpy(resource->parts + i * RNS_RESOURCE_PART_MAX, data, length);
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
        memcpy(stream + offset, resource->parts + i * RNS_RESOURCE_PART_MAX,
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
            for (size_t previous = 0u; previous < i; ++previous)
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

rns_status_t rns_resource_sender_create(
    rns_resource_sender_t **output, const rns_link *link,
    const uint8_t *data, size_t data_length,
    const rns_resource_sender_options_t *options) {
    if (output == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    *output = NULL;
    if (link == NULL || data == NULL || data_length == 0u ||
        data_length > RNS_RESOURCE_SINGLE_SEGMENT_MAX_SIZE)
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
    uint8_t *prepared = NULL;
    size_t prepared_length = 0u;
    rns_status_t status = prepare_compressed(
        data, data_length, auto_compress, &prepared, &prepared_length,
        &sender->compressed);
    if (status != RNS_OK) goto failed;
    if (prepared_length > SIZE_MAX - RNS_RESOURCE_RANDOM_HASH_SIZE) {
        status = RNS_ERROR_OVERFLOW;
        goto failed;
    }
    size_t clear_length = prepared_length + RNS_RESOURCE_RANDOM_HASH_SIZE;
    uint8_t *clear = malloc(clear_length);
    if (clear == NULL) {
        status = RNS_ERROR_NO_MEMORY;
        goto failed;
    }
    if (!rns_random_bytes(clear, RNS_RESOURCE_RANDOM_HASH_SIZE)) {
        free(clear);
        status = RNS_ERROR_CRYPTO;
        goto failed;
    }
    memcpy(clear + RNS_RESOURCE_RANDOM_HASH_SIZE, prepared, prepared_length);
    size_t encrypted_capacity = clear_length + 128u;
    sender->stream = malloc(encrypted_capacity);
    if (sender->stream == NULL) {
        free(clear);
        status = RNS_ERROR_NO_MEMORY;
        goto failed;
    }
    if (!rns_link_encrypt(link, clear, clear_length, sender->stream,
                          encrypted_capacity, &sender->stream_length)) {
        free(clear);
        status = RNS_ERROR_CRYPTO;
        goto failed;
    }
    free(clear);
    sender->parts = (sender->stream_length + sender->part_size - 1u) /
                    sender->part_size;
    if (sender->parts == 0u || sender->parts > RNS_RESOURCE_MAX_PARTS) {
        status = RNS_ERROR_UNSUPPORTED;
        goto failed;
    }
    sender->data_length = data_length;
    if (options != NULL && options->request_id != NULL) {
        memcpy(sender->request_id, options->request_id,
               sizeof sender->request_id);
        sender->has_request_id = true;
        sender->is_response = options->is_response;
    }
    status = sender_compute_hashes(sender, data, data_length);
    if (status != RNS_OK) goto failed;
    free(prepared);
    *output = sender;
    return RNS_OK;

failed:
    free(prepared);
    rns_resource_sender_destroy(sender);
    return status;
}

void rns_resource_sender_destroy(rns_resource_sender_t *sender) {
    if (sender == NULL) return;
    free(sender->stream);
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
    if (sender->has_request_id)
        flags |= sender->is_response ? RNS_RESOURCE_FLAG_RESPONSE
                                     : RNS_RESOURCE_FLAG_REQUEST;
    size_t hashmap_length = sender->parts * RNS_RESOURCE_MAPHASH_LEN;
    if (!write_u8(&writer, 0x8bu) ||
        !write_key(&writer, 't') || !write_uint(&writer, sender->stream_length) ||
        !write_key(&writer, 'd') || !write_uint(&writer, sender->data_length) ||
        !write_key(&writer, 'n') || !write_uint(&writer, sender->parts) ||
        !write_key(&writer, 'h') || !write_bin(&writer, sender->hash, sizeof sender->hash) ||
        !write_key(&writer, 'r') || !write_bin(&writer, sender->random_hash, sizeof sender->random_hash) ||
        !write_key(&writer, 'o') || !write_bin(&writer, sender->hash, sizeof sender->hash) ||
        !write_key(&writer, 'i') || !write_uint(&writer, 1u) ||
        !write_key(&writer, 'l') || !write_uint(&writer, 1u) ||
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
    if (request_length < 1u + RNS_RESOURCE_HASH_SIZE + RNS_RESOURCE_MAPHASH_LEN ||
        request[0] != HASHMAP_IS_NOT_EXHAUSTED ||
        (request_length - 1u - RNS_RESOURCE_HASH_SIZE) %
                RNS_RESOURCE_MAPHASH_LEN !=
            0u ||
        memcmp(request + 1u, sender->hash, sizeof sender->hash) != 0)
        return RNS_ERROR_PROTOCOL;
    size_t hashes = (request_length - 1u - RNS_RESOURCE_HASH_SIZE) /
                    RNS_RESOURCE_MAPHASH_LEN;
    for (size_t requested = 0u; requested < hashes; ++requested) {
        const uint8_t *map_hash = request + 1u + RNS_RESOURCE_HASH_SIZE +
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
    return *part_count != 0u ? RNS_OK : RNS_ERROR_NOT_FOUND;
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
