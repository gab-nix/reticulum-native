#include "reticulum/lxmf_store.h"

#include <assert.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static uint32_t crc32_bytes(const uint8_t *bytes, size_t length) {
    uint32_t crc = 0xffffffffu;
    while (length--) {
        crc ^= *bytes++;
        for (unsigned bit = 0; bit < 8u; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (~(crc & 1u) + 1u));
    }
    return ~crc;
}

static void put16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static void put32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

static void append_v2_message(const char *path) {
    static const uint8_t body[] = "legacy-v2";
    uint8_t payload[80u + sizeof body - 1u];
    uint8_t header[16] = {'L', 'X', 'M', 'S', 1u, 3u, 0u, 0u};
    memset(payload, 0, sizeof payload);
    payload[0] = 0x91u;
    payload[32] = 0x92u;
    payload[48] = 0x93u;
    payload[72] = (uint8_t)LXMF_DELIVERY_SENT;
    put32(payload + 73, sizeof body - 1u);
    payload[77] = (uint8_t)LXMF_SIGNATURE_VERIFIED;
    put16(payload + 78, 0u);
    memcpy(payload + 80, body, sizeof body - 1u);
    put32(header + 8, sizeof payload);
    put32(header + 12, crc32_bytes(payload, sizeof payload));
    int descriptor = open(path, O_WRONLY | O_APPEND);
    assert(descriptor >= 0);
    assert(write(descriptor, header, sizeof header) == (ssize_t)sizeof header);
    assert(write(descriptor, payload, sizeof payload) ==
           (ssize_t)sizeof payload);
    assert(close(descriptor) == 0);
}

static void assert_metadata(const lxmf_delivery_metadata_t *delivery,
                            uint32_t attempts,
                            lxmf_queue_reason_t queue_reason) {
    assert(delivery->desired_method == LXMF_DELIVERY_METHOD_PROPAGATED);
    assert(delivery->actual_method == LXMF_DELIVERY_METHOD_DIRECT);
    assert(delivery->attempts == attempts);
    assert(delivery->queue_reason == queue_reason);
    assert(delivery->retry_at_ms == UINT64_C(987654321));
    assert(delivery->progress == 456789u);
    assert(delivery->has_proof_id);
    assert(delivery->proof_id[0] == 0xa5u && delivery->proof_id[31] == 0x5au);
}

int main(void) {
    char path[] = "/tmp/lxmf-delivery-store-XXXXXX";
    int descriptor = mkstemp(path);
    assert(descriptor >= 0);
    assert(close(descriptor) == 0);

    append_v2_message(path);
    lxmf_store_t store = {0};
    assert(lxmf_store_open(&store, path) == LXMF_OK);
    uint8_t legacy_id[LXMF_MESSAGE_ID_LENGTH] = {0x91u};
    uint8_t content[32];
    lxmf_store_message_t legacy;
    assert(lxmf_store_read(&store, legacy_id, &legacy, content,
                           sizeof content) == LXMF_OK);
    assert(legacy.status == LXMF_DELIVERY_SENT);
    assert(legacy.delivery.desired_method == LXMF_DELIVERY_METHOD_UNKNOWN);
    assert(legacy.delivery.attempts == 0u && !legacy.delivery.has_proof_id);

    lxmf_store_message_t message = {0};
    message.message_id[0] = 0x41u;
    message.destination[0] = 0x42u;
    message.source[0] = 0x43u;
    message.timestamp = 42.5;
    message.status = LXMF_DELIVERY_SENDING;
    message.signature_state = LXMF_SIGNATURE_VERIFIED;
    message.content = (lxmf_slice_t){(const uint8_t *)"durable", 7u};
    message.delivery.desired_method = LXMF_DELIVERY_METHOD_PROPAGATED;
    message.delivery.actual_method = LXMF_DELIVERY_METHOD_DIRECT;
    message.delivery.attempts = 3u;
    message.delivery.queue_reason = LXMF_QUEUE_REASON_LINK;
    message.delivery.retry_at_ms = UINT64_C(987654321);
    message.delivery.progress = 456789u;
    message.delivery.has_proof_id = true;
    message.delivery.proof_id[0] = 0xa5u;
    message.delivery.proof_id[31] = 0x5au;
    bool inserted = false;
    assert(lxmf_store_put(&store, &message, &inserted) == LXMF_OK && inserted);
    assert(lxmf_store_read(&store, message.message_id, &message, content,
                           sizeof content) == LXMF_OK);
    assert_metadata(&message.delivery, 3u, LXMF_QUEUE_REASON_LINK);

    lxmf_delivery_metadata_t invalid = message.delivery;
    invalid.progress = LXMF_DELIVERY_PROGRESS_COMPLETE + 1u;
    assert(lxmf_store_update_delivery(&store, message.message_id, &invalid) ==
           LXMF_ERR_ARGUMENT);
    invalid = message.delivery;
    invalid.actual_method = (lxmf_delivery_method_t)99;
    assert(lxmf_store_update_delivery(&store, message.message_id, &invalid) ==
           LXMF_ERR_ARGUMENT);

    message.delivery.attempts = 4u;
    message.delivery.queue_reason = LXMF_QUEUE_REASON_RETRY_BACKOFF;
    assert(lxmf_store_update_delivery(&store, message.message_id,
                                      &message.delivery) == LXMF_OK);
    lxmf_store_close(&store);

    assert(lxmf_store_open(&store, path) == LXMF_OK);
    assert(lxmf_store_read(&store, message.message_id, &message, content,
                           sizeof content) == LXMF_OK);
    assert_metadata(&message.delivery, 4u, LXMF_QUEUE_REASON_RETRY_BACKOFF);
    assert(lxmf_store_compact(&store) == LXMF_OK);
    lxmf_store_close(&store);
    assert(lxmf_store_open(&store, path) == LXMF_OK);
    assert(lxmf_store_read(&store, message.message_id, &message, content,
                           sizeof content) == LXMF_OK);
    assert_metadata(&message.delivery, 4u, LXMF_QUEUE_REASON_RETRY_BACKOFF);
    assert(lxmf_store_read(&store, legacy_id, &legacy, content,
                           sizeof content) == LXMF_OK);
    assert(legacy.delivery.desired_method == LXMF_DELIVERY_METHOD_UNKNOWN);
    lxmf_store_close(&store);
    assert(unlink(path) == 0);
    return 0;
}
