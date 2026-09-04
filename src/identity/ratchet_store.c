#include "reticulum/ratchet_store.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "reticulum/crypto.h"
#include "reticulum/hal.h"

#define RATCHET_PACKED_MAX \
    (3u + RNS_RATCHET_STORE_MAX_RETAINED * (2u + RNS_RATCHET_PRIVATE_SIZE))
#define RATCHET_FILE_MAX (1u + 10u + 3u + 64u + 9u + 3u + RATCHET_PACKED_MAX)

struct rns_ratchet_store {
    char path[RNS_RATCHET_STORE_PATH_MAX + 1u];
    const rns_identity *identity;
    uint8_t *private_keys;
    size_t count;
    size_t retained;
    uint64_t interval;
    uint64_t latest_rotation;
};

typedef struct {
    const uint8_t *cursor;
    const uint8_t *end;
} reader_t;

static void put16(uint8_t *output, size_t value) {
    output[0] = (uint8_t)(value >> 8u);
    output[1] = (uint8_t)value;
}

static void put32(uint8_t *output, size_t value) {
    output[0] = (uint8_t)(value >> 24u);
    output[1] = (uint8_t)(value >> 16u);
    output[2] = (uint8_t)(value >> 8u);
    output[3] = (uint8_t)value;
}

static size_t pack_bin_header(uint8_t *output, size_t length) {
    if (length <= UINT8_MAX) {
        output[0] = 0xc4u;
        output[1] = (uint8_t)length;
        return 2u;
    }
    if (length <= UINT16_MAX) {
        output[0] = 0xc5u;
        put16(output + 1u, length);
        return 3u;
    }
    output[0] = 0xc6u;
    put32(output + 1u, length);
    return 5u;
}

static size_t pack_ratchets(const rns_ratchet_store_t *store,
                            uint8_t output[RATCHET_PACKED_MAX]) {
    size_t offset;
    if (store->count <= 15u) {
        output[0] = (uint8_t)(0x90u | store->count);
        offset = 1u;
    } else {
        output[0] = 0xdcu;
        put16(output + 1u, store->count);
        offset = 3u;
    }
    for (size_t index = 0u; index < store->count; ++index) {
        output[offset++] = 0xc4u;
        output[offset++] = RNS_RATCHET_PRIVATE_SIZE;
        memcpy(output + offset,
               store->private_keys + index * RNS_RATCHET_PRIVATE_SIZE,
               RNS_RATCHET_PRIVATE_SIZE);
        offset += RNS_RATCHET_PRIVATE_SIZE;
    }
    return offset;
}

static rns_status_t persist(const rns_ratchet_store_t *store) {
    uint8_t packed[RATCHET_PACKED_MAX];
    uint8_t file_data[RATCHET_FILE_MAX];
    uint8_t signature[64];
    size_t packed_length = pack_ratchets(store, packed);
    if (!rns_identity_sign(store->identity, packed, packed_length, signature))
        return RNS_ERROR_CRYPTO;
    size_t offset = 0u;
    file_data[offset++] = 0x82u;
    file_data[offset++] = 0xa9u;
    memcpy(file_data + offset, "signature", 9u);
    offset += 9u;
    offset += pack_bin_header(file_data + offset, sizeof signature);
    memcpy(file_data + offset, signature, sizeof signature);
    offset += sizeof signature;
    file_data[offset++] = 0xa8u;
    memcpy(file_data + offset, "ratchets", 8u);
    offset += 8u;
    offset += pack_bin_header(file_data + offset, packed_length);
    memcpy(file_data + offset, packed, packed_length);
    offset += packed_length;

    char temporary[RNS_RATCHET_STORE_PATH_MAX + 5u];
    int length = snprintf(temporary, sizeof temporary, "%s.tmp", store->path);
    if (length < 0 || (size_t)length >= sizeof temporary) {
        rns_hal_secure_zero(packed, sizeof packed);
        return RNS_ERROR_OVERFLOW;
    }
    FILE *file = fopen(temporary, "w+b");
    if (file == NULL) {
        rns_hal_secure_zero(packed, sizeof packed);
        return RNS_ERROR_IO;
    }
    rns_status_t status = RNS_OK;
    if (fwrite(file_data, 1u, offset, file) != offset || fflush(file) != 0 ||
        fsync(fileno(file)) != 0)
        status = RNS_ERROR_IO;
    if (fclose(file) != 0 && status == RNS_OK) status = RNS_ERROR_IO;
    if (status == RNS_OK && rename(temporary, store->path) != 0)
        status = RNS_ERROR_IO;
    if (status != RNS_OK) (void)unlink(temporary);
    rns_hal_secure_zero(signature, sizeof signature);
    rns_hal_secure_zero(packed, sizeof packed);
    rns_hal_secure_zero(file_data, sizeof file_data);
    return status;
}

static bool read_length(reader_t *reader, uint8_t marker, size_t *length) {
    size_t bytes;
    if (marker == 0xc4u || marker == 0xd9u) bytes = 1u;
    else if (marker == 0xc5u || marker == 0xdau) bytes = 2u;
    else if (marker == 0xc6u || marker == 0xdbu) bytes = 4u;
    else return false;
    if ((size_t)(reader->end - reader->cursor) < bytes) return false;
    size_t value = 0u;
    for (size_t index = 0u; index < bytes; ++index)
        value = (value << 8u) | reader->cursor[index];
    reader->cursor += bytes;
    *length = value;
    return true;
}

static bool read_bin(reader_t *reader, const uint8_t **data, size_t *length) {
    if (reader->cursor == reader->end) return false;
    uint8_t marker = *reader->cursor++;
    if (!read_length(reader, marker, length) ||
        *length > (size_t)(reader->end - reader->cursor))
        return false;
    *data = reader->cursor;
    reader->cursor += *length;
    return true;
}

static bool read_string(reader_t *reader, const uint8_t **data,
                        size_t *length) {
    if (reader->cursor == reader->end) return false;
    uint8_t marker = *reader->cursor++;
    if ((marker & 0xe0u) == 0xa0u) {
        *length = marker & 0x1fu;
    } else {
        size_t bytes;
        if (marker == 0xd9u) bytes = 1u;
        else if (marker == 0xdau) bytes = 2u;
        else if (marker == 0xdbu) bytes = 4u;
        else return false;
        if ((size_t)(reader->end - reader->cursor) < bytes) return false;
        size_t value = 0u;
        for (size_t index = 0u; index < bytes; ++index)
            value = (value << 8u) | reader->cursor[index];
        reader->cursor += bytes;
        *length = value;
    }
    if (*length > (size_t)(reader->end - reader->cursor)) return false;
    *data = reader->cursor;
    reader->cursor += *length;
    return true;
}

static bool read_array_count(reader_t *reader, size_t *count) {
    if (reader->cursor == reader->end) return false;
    uint8_t marker = *reader->cursor++;
    if ((marker & 0xf0u) == 0x90u) {
        *count = marker & 0x0fu;
        return true;
    }
    size_t bytes = marker == 0xdcu ? 2u : marker == 0xddu ? 4u : 0u;
    if (bytes == 0u || (size_t)(reader->end - reader->cursor) < bytes)
        return false;
    size_t value = 0u;
    for (size_t index = 0u; index < bytes; ++index)
        value = (value << 8u) | reader->cursor[index];
    reader->cursor += bytes;
    *count = value;
    return true;
}

static rns_status_t load(rns_ratchet_store_t *store) {
    FILE *file = fopen(store->path, "rb");
    if (file == NULL) return errno == ENOENT ? RNS_ERROR_NOT_FOUND : RNS_ERROR_IO;
    uint8_t bytes[RATCHET_FILE_MAX + 1u];
    size_t length = fread(bytes, 1u, sizeof bytes, file);
    bool io_error = ferror(file) != 0;
    if (fclose(file) != 0 || io_error) return RNS_ERROR_IO;
    if (length == sizeof bytes || length == 0u) return RNS_ERROR_PROTOCOL;
    reader_t reader = {bytes, bytes + length};
    if (*reader.cursor++ != 0x82u) return RNS_ERROR_PROTOCOL;
    const uint8_t *signature = NULL;
    const uint8_t *packed = NULL;
    size_t signature_length = 0u;
    size_t packed_length = 0u;
    for (size_t field = 0u; field < 2u; ++field) {
        const uint8_t *key;
        size_t key_length;
        const uint8_t *value;
        size_t value_length;
        if (!read_string(&reader, &key, &key_length) ||
            !read_bin(&reader, &value, &value_length))
            return RNS_ERROR_PROTOCOL;
        if (key_length == 9u && memcmp(key, "signature", 9u) == 0) {
            signature = value;
            signature_length = value_length;
        } else if (key_length == 8u && memcmp(key, "ratchets", 8u) == 0) {
            packed = value;
            packed_length = value_length;
        } else {
            return RNS_ERROR_PROTOCOL;
        }
    }
    if (reader.cursor != reader.end || signature == NULL || packed == NULL ||
        signature_length != 64u || packed_length > RATCHET_PACKED_MAX ||
        !rns_identity_verify(store->identity, packed, packed_length, signature))
        return RNS_ERROR_CRYPTO;
    reader_t ratchets = {packed, packed + packed_length};
    size_t count;
    if (!read_array_count(&ratchets, &count) ||
        count > RNS_RATCHET_STORE_MAX_RETAINED)
        return RNS_ERROR_PROTOCOL;
    for (size_t index = 0u; index < count; ++index) {
        const uint8_t *key;
        size_t key_length;
        if (!read_bin(&ratchets, &key, &key_length) ||
            key_length != RNS_RATCHET_PRIVATE_SIZE)
            return RNS_ERROR_PROTOCOL;
        if (index < store->retained)
            memcpy(store->private_keys + index * RNS_RATCHET_PRIVATE_SIZE, key,
                   RNS_RATCHET_PRIVATE_SIZE);
    }
    if (ratchets.cursor != ratchets.end) return RNS_ERROR_PROTOCOL;
    store->count = count < store->retained ? count : store->retained;
    return RNS_OK;
}

rns_status_t rns_ratchet_store_open(rns_ratchet_store_t **output,
                                    const char *path,
                                    const rns_identity *identity,
                                    size_t retained,
                                    uint64_t interval) {
    if (output == NULL || path == NULL || *path == '\0' || identity == NULL ||
        !identity->has_private || strlen(path) > RNS_RATCHET_STORE_PATH_MAX ||
        retained > RNS_RATCHET_STORE_MAX_RETAINED)
        return RNS_ERROR_INVALID_ARGUMENT;
    if (retained == 0u) retained = RNS_RATCHET_STORE_DEFAULT_RETAINED;
    if (interval == 0u) interval = RNS_RATCHET_STORE_DEFAULT_INTERVAL;
    *output = NULL;
    rns_ratchet_store_t *store = calloc(1u, sizeof *store);
    if (store == NULL) return RNS_ERROR_NO_MEMORY;
    store->private_keys = calloc(retained, RNS_RATCHET_PRIVATE_SIZE);
    if (store->private_keys == NULL) {
        free(store);
        return RNS_ERROR_NO_MEMORY;
    }
    memcpy(store->path, path, strlen(path) + 1u);
    store->identity = identity;
    store->retained = retained;
    store->interval = interval;
    rns_status_t status = load(store);
    if (status == RNS_ERROR_NOT_FOUND) status = persist(store);
    if (status != RNS_OK) {
        rns_ratchet_store_close(store);
        return status;
    }
    *output = store;
    return RNS_OK;
}

void rns_ratchet_store_close(rns_ratchet_store_t *store) {
    if (store == NULL) return;
    rns_hal_secure_zero(store->private_keys,
                        store->retained * RNS_RATCHET_PRIVATE_SIZE);
    free(store->private_keys);
    rns_hal_secure_zero(store, sizeof *store);
    free(store);
}

rns_status_t rns_ratchet_store_current(rns_ratchet_store_t *store,
                                       uint64_t now, uint8_t private_key[32],
                                       uint8_t public_key[32],
                                       uint8_t ratchet_id[16], bool *rotated) {
    if (store == NULL || private_key == NULL || public_key == NULL ||
        ratchet_id == NULL)
        return RNS_ERROR_INVALID_ARGUMENT;
    if (rotated != NULL) *rotated = false;
    bool should_rotate = store->count == 0u ||
        (now > store->latest_rotation &&
         now - store->latest_rotation > store->interval);
    if (should_rotate) {
        uint8_t next_private[32];
        uint8_t next_public[32];
        uint8_t next_id[16];
        uint8_t dropped_private[32];
        memcpy(dropped_private,
               store->private_keys +
                   (store->retained - 1u) * RNS_RATCHET_PRIVATE_SIZE,
               sizeof dropped_private);
        if (!rns_identity_ratchet_generate(next_private, next_public, next_id))
            return RNS_ERROR_CRYPTO;
        size_t keep = store->count < store->retained - 1u
                          ? store->count
                          : store->retained - 1u;
        memmove(store->private_keys + RNS_RATCHET_PRIVATE_SIZE,
                store->private_keys, keep * RNS_RATCHET_PRIVATE_SIZE);
        memcpy(store->private_keys, next_private, sizeof next_private);
        size_t old_count = store->count;
        uint64_t old_rotation = store->latest_rotation;
        if (store->count < store->retained) ++store->count;
        store->latest_rotation = now;
        rns_status_t status = persist(store);
        if (status != RNS_OK) {
            memmove(store->private_keys,
                    store->private_keys + RNS_RATCHET_PRIVATE_SIZE,
                    keep * RNS_RATCHET_PRIVATE_SIZE);
            if (old_count == store->retained)
                memcpy(store->private_keys +
                           (store->retained - 1u) * RNS_RATCHET_PRIVATE_SIZE,
                       dropped_private, sizeof dropped_private);
            store->count = old_count;
            store->latest_rotation = old_rotation;
            rns_hal_secure_zero(next_private, sizeof next_private);
            rns_hal_secure_zero(dropped_private, sizeof dropped_private);
            return status;
        }
        if (rotated != NULL) *rotated = true;
        rns_hal_secure_zero(next_private, sizeof next_private);
        rns_hal_secure_zero(dropped_private, sizeof dropped_private);
    }
    memcpy(private_key, store->private_keys, RNS_RATCHET_PRIVATE_SIZE);
    if (!rns_x25519_public_from_private(private_key, public_key)) {
        rns_hal_secure_zero(private_key, RNS_RATCHET_PRIVATE_SIZE);
        return RNS_ERROR_CRYPTO;
    }
    rns_identity_ratchet_id(public_key, ratchet_id);
    return RNS_OK;
}

rns_status_t rns_ratchet_store_copy_private(
    const rns_ratchet_store_t *store, uint8_t *private_keys,
    size_t capacity_keys, size_t *key_count) {
    if (store == NULL || key_count == NULL ||
        (capacity_keys != 0u && private_keys == NULL))
        return RNS_ERROR_INVALID_ARGUMENT;
    *key_count = store->count;
    if (capacity_keys < store->count) return RNS_ERROR_OVERFLOW;
    memcpy(private_keys, store->private_keys,
           store->count * RNS_RATCHET_PRIVATE_SIZE);
    return RNS_OK;
}

size_t rns_ratchet_store_count(const rns_ratchet_store_t *store) {
    return store != NULL ? store->count : 0u;
}
