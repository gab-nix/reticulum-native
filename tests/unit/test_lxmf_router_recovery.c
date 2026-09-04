#include "reticulum/lxmf_router.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const rns_identity *resolve_none(void *context,
                                        const uint8_t destination[16]) {
    (void)context;
    (void)destination;
    return NULL;
}

static lxmf_status_t send_unused(void *context, const uint8_t *packet,
                                 size_t packet_length) {
    (void)context;
    (void)packet;
    (void)packet_length;
    return LXMF_ERR_CRYPTO;
}

static uint64_t fake_monotonic(void *context) {
    return *(const uint64_t *)context;
}

static void put_message(lxmf_store_t *store, uint8_t id,
                        lxmf_delivery_status_t status, uint32_t attempts,
                        bool has_proof, lxmf_queue_reason_t reason,
                        uint64_t retry_at_ms) {
    lxmf_store_message_t message = {0};
    message.message_id[0] = id;
    message.destination[0] = 0x21u;
    message.source[0] = 0x22u;
    message.timestamp = 1.0;
    message.status = status;
    message.signature_state = LXMF_SIGNATURE_VERIFIED;
    message.content = (lxmf_slice_t){(const uint8_t *)"pending", 7u};
    message.delivery.desired_method = LXMF_DELIVERY_METHOD_DIRECT;
    message.delivery.actual_method = LXMF_DELIVERY_METHOD_DIRECT;
    message.delivery.attempts = attempts;
    message.delivery.queue_reason = reason;
    message.delivery.retry_at_ms = retry_at_ms;
    message.delivery.has_proof_id = has_proof;
    if (has_proof) message.delivery.proof_id[0] = id;
    bool inserted = false;
    assert(lxmf_store_put(store, &message, &inserted) == LXMF_OK && inserted);
}

static lxmf_store_message_t read_message(lxmf_store_t *store, uint8_t id,
                                         uint8_t content[16]) {
    uint8_t message_id[LXMF_MESSAGE_ID_LENGTH] = {0};
    message_id[0] = id;
    lxmf_store_message_t message;
    assert(lxmf_store_read(store, message_id, &message, content, 16u) ==
           LXMF_OK);
    return message;
}

int main(void) {
    char path[] = "/tmp/lxmf-router-recovery-XXXXXX";
    int descriptor = mkstemp(path);
    assert(descriptor >= 0);
    assert(close(descriptor) == 0);
    assert(unlink(path) == 0);
    lxmf_store_t store = {0};
    assert(lxmf_store_open(&store, path) == LXMF_OK);
    put_message(&store, 1u, LXMF_DELIVERY_SENDING, 2u, false,
                LXMF_QUEUE_REASON_NONE, 0U);
    put_message(&store, 2u, LXMF_DELIVERY_SENT, 3u, true,
                LXMF_QUEUE_REASON_NONE, 0U);
    put_message(&store, 3u, LXMF_DELIVERY_SENT, 0u, false,
                LXMF_QUEUE_REASON_NONE, 0U);
    put_message(&store, 4u, LXMF_DELIVERY_QUEUED, 2u, false,
                LXMF_QUEUE_REASON_RETRY_BACKOFF, UINT64_MAX - 1U);
    lxmf_store_close(&store);
    assert(lxmf_store_open(&store, path) == LXMF_OK);

    rns_identity identity;
    assert(rns_identity_generate(&identity));
    lxmf_router_t router;
    uint64_t monotonic_now = 1000U;
    lxmf_router_config_t config = {
        .identity = &identity,
        .store = &store,
        .resolve_identity = resolve_none,
        .send_packet = send_unused,
        .monotonic_clock = fake_monotonic,
        .monotonic_clock_context = &monotonic_now,
        .direct_retry_base_ms = 10U,
        .preferred_delivery_method = LXMF_DELIVERY_METHOD_DIRECT};
    assert(lxmf_router_init(&router, &config) == LXMF_OK);

    uint8_t content[16];
    lxmf_store_message_t sending = read_message(&store, 1u, content);
    assert(sending.status == LXMF_DELIVERY_QUEUED);
    assert(sending.delivery.attempts == 2u);
    assert(sending.delivery.queue_reason == LXMF_QUEUE_REASON_RETRY_BACKOFF);
    assert(!sending.delivery.has_proof_id);
    assert(sending.delivery.retry_at_ms == 1020U);
    lxmf_store_message_t awaiting = read_message(&store, 2u, content);
    assert(awaiting.status == LXMF_DELIVERY_QUEUED);
    assert(awaiting.delivery.attempts == 3u);
    assert(awaiting.delivery.queue_reason == LXMF_QUEUE_REASON_RETRY_BACKOFF);
    assert(!awaiting.delivery.has_proof_id);
    assert(awaiting.delivery.retry_at_ms == 1040U);
    lxmf_store_message_t historical = read_message(&store, 3u, content);
    assert(historical.status == LXMF_DELIVERY_SENT);
    lxmf_store_message_t backoff = read_message(&store, 4u, content);
    assert(backoff.status == LXMF_DELIVERY_QUEUED);
    assert(backoff.delivery.attempts == 2u);
    assert(backoff.delivery.queue_reason == LXMF_QUEUE_REASON_RETRY_BACKOFF);
    assert(backoff.delivery.retry_at_ms == 1020U);

    /* Waiting for an identity persists an actionable reason without inflating
     * the transmission-attempt count on every caller poll. */
    assert(lxmf_router_send_message(&router, sending.message_id) ==
           LXMF_ERR_PENDING);
    sending = read_message(&store, 1u, content);
    assert(sending.delivery.attempts == 2u);
    assert(sending.delivery.queue_reason == LXMF_QUEUE_REASON_RETRY_BACKOFF);
    assert(sending.delivery.retry_at_ms == 1020U);
    lxmf_router_destroy(&router);
    lxmf_store_close(&store);

    assert(lxmf_store_open(&store, path) == LXMF_OK);
    sending = read_message(&store, 1u, content);
    assert(sending.status == LXMF_DELIVERY_QUEUED);
    assert(sending.delivery.attempts == 2u);
    assert(sending.delivery.queue_reason == LXMF_QUEUE_REASON_RETRY_BACKOFF);
    assert(sending.delivery.retry_at_ms == 1020U);
    lxmf_store_close(&store);
    assert(unlink(path) == 0);
    return 0;
}
