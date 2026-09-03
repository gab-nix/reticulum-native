#include "reticulum/lxmf_store.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void put32(uint8_t *bytes, uint32_t value) {
    for (unsigned i = 0; i < 4u; i++) bytes[3u-i] = (uint8_t)(value >> (8u*i));
}

static void append_record(const char *path, uint8_t type,
                          const uint8_t *payload, size_t length) {
    uint8_t header[16] = {'L', 'X', 'M', 'S', 1u, 0u, 0u, 0u};
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < length; i++) {
        crc ^= payload[i];
        for (unsigned bit = 0; bit < 8u; bit++)
            crc = (crc >> 1) ^ (0xedb88320u & (~(crc & 1u) + 1u));
    }
    header[5] = type;
    put32(header + 8, (uint32_t)length);
    put32(header + 12, ~crc);
    FILE *file = fopen(path, "ab");
    assert(file);
    assert(fwrite(header, 1, sizeof header, file) == sizeof header);
    assert(fwrite(payload, 1, length, file) == length);
    assert(fclose(file) == 0);
}

static void migration_and_recovery(const char *path) {
    uint8_t legacy[132u + 3u + 5u] = {0};
    legacy[0] = 9u;
    legacy[72] = LXMF_DELIVERY_SENDING;
    put32(legacy + 73, 3u);
    legacy[77] = LXMF_SIGNATURE_VERIFIED;
    legacy[79] = 5u;
    legacy[80] = LXMF_DELIVERY_METHOD_DIRECT;
    put32(legacy + 84, 7u);
    memcpy(legacy + 132, "abc12345", 8u);
    append_record(path, 6u, legacy, sizeof legacy);
    /* A CRC-valid v4 record with an impossible packed size must not enter the
     * index, overflow arithmetic, or cause a huge allocation while scanning. */
    uint8_t malformed[134u] = {0};
    malformed[0] = 10u;
    put32(malformed + 78, UINT32_MAX);
    append_record(path, 8u, malformed, sizeof malformed);
    lxmf_store_t store = {0};
    uint8_t id[32] = {9u}, packed[5], content[3];
    size_t length;
    for (unsigned pass = 0; pass < 2u; pass++) {
        assert(lxmf_store_open(&store, path) == LXMF_OK);
        assert(lxmf_store_count(&store) == 1u);
        lxmf_store_message_t message;
        assert(lxmf_store_read(&store, id, &message, content, sizeof content) == LXMF_OK);
        assert(message.status == LXMF_DELIVERY_SENDING);
        assert(message.delivery.desired_method == LXMF_DELIVERY_METHOD_DIRECT);
        assert(message.delivery.attempts == 7u);
        assert(memcmp(content, "abc", 3u) == 0);
        assert(lxmf_store_read_packed(&store, id, packed, sizeof packed, &length) == LXMF_OK);
        assert(length == 5u && memcmp(packed, "12345", 5u) == 0);
        if (pass == 0u) assert(lxmf_store_compact(&store) == LXMF_OK);
        lxmf_store_close(&store);
    }
}

static void roundtrip(const char *path, size_t length) {
    uint8_t *packed = malloc(length), *copy = malloc(length);
    assert(packed && copy);
    /* Storage deliberately treats the representation as opaque; include all
     * byte values, NULs, and unknown-field-like data without normalization. */
    for (size_t i = 0; i < length; i++) packed[i] = (uint8_t)(i * 37u);
    lxmf_store_t store = {0};
    lxmf_store_message_t message = {0};
    message.message_id[0] = 1u;
    message.source[0] = 2u;
    message.destination[0] = 3u;
    message.timestamp = 1234.5;
    message.status = LXMF_DELIVERY_QUEUED;
    message.signature_state = LXMF_SIGNATURE_VERIFIED;
    message.content = (lxmf_slice_t){(const uint8_t *)"preview", 7u};
    message.packed = (lxmf_slice_t){packed, length};
    message.delivery.desired_method = LXMF_DELIVERY_METHOD_DIRECT;
    message.delivery.attempts = 3u;
    bool inserted;
    size_t size = 99u;
    assert(lxmf_store_open(&store, path) == LXMF_OK);
    assert(lxmf_store_packed_size(&store, message.message_id, &size) == LXMF_ERR_FORMAT);
    assert(size == 0u);
    assert(lxmf_store_put(&store, &message, &inserted) == LXMF_OK && inserted);
    assert(lxmf_store_packed_size(&store, message.message_id, &size) == LXMF_OK);
    assert(size == length);
    assert(lxmf_store_read_packed(&store, message.message_id, copy, length - 1u,
                                  &size) == LXMF_ERR_BOUNDS);
    assert(lxmf_store_update_status(&store, message.message_id,
                                    LXMF_DELIVERY_SENDING) == LXMF_OK);
    lxmf_store_close(&store);
    for (unsigned pass = 0; pass < 2u; pass++) {
        assert(lxmf_store_open(&store, path) == LXMF_OK);
        assert(lxmf_store_count(&store) == 1u);
        assert(lxmf_store_read_packed(&store, message.message_id, copy, length,
                                      &size) == LXMF_OK);
        assert(size == length && memcmp(copy, packed, length) == 0);
        uint8_t preview[7];
        lxmf_store_message_t read;
        assert(lxmf_store_read(&store, message.message_id, &read, preview,
                               sizeof preview) == LXMF_OK);
        assert(read.status == LXMF_DELIVERY_SENDING);
        assert(read.delivery.attempts == 3u);
        assert(read.timestamp == message.timestamp);
        assert(memcmp(preview, "preview", sizeof preview) == 0);
        if (pass == 0u) assert(lxmf_store_compact(&store) == LXMF_OK);
        lxmf_store_close(&store);
    }
    assert(lxmf_store_open(&store, path) == LXMF_OK);
    message.message_id[0] = 4u;
    message.packed.len = LXMF_STORE_MAX_PACKED + 1u;
    assert(lxmf_store_put(&store, &message, &inserted) == LXMF_ERR_ARGUMENT);
    assert(!inserted && lxmf_store_count(&store) == 1u);
    lxmf_store_close(&store);
    free(copy);
    free(packed);
}

int main(void) {
    char path[] = "/tmp/reticulum-store-large-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    migration_and_recovery(path);
    assert(unlink(path) == 0);
    roundtrip(path, 65537u); /* Crosses the previous journal uint16_t limit. */
    assert(unlink(path) == 0);
    roundtrip(path, LXMF_STORE_MAX_PACKED);
    assert(unlink(path) == 0);
    puts("large packed message store tests passed");
    return 0;
}
