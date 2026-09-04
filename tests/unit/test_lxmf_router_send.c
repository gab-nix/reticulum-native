#include "reticulum/destination.h"
#include "reticulum/lxmf_router.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    const rns_identity *peer;
    uint8_t packet[500];
    size_t length;
    bool fail;
    size_t callbacks;
    lxmf_delivery_status_t last_status;
    size_t events;
    lxmf_router_event_t last_event;
    const uint8_t *ratchet;
    uint64_t now_ms;
} send_state_t;

static uint64_t clock_ms(void *context) {
    return ((send_state_t *)context)->now_ms;
}

static const rns_identity *resolve(void *context, const uint8_t hash[16]) {
    send_state_t *state = context;
    uint8_t expected[16];
    const char *aspects[] = {"delivery"};
    return rns_destination_hash(state->peer, "lxmf", aspects, 1u, expected) &&
                   memcmp(expected, hash, sizeof expected) == 0
               ? state->peer
               : NULL;
}

static bool resolve_ratchet(void *context, const uint8_t hash[16],
                            uint8_t ratchet_public[32]) {
    send_state_t *state = context;
    if (state->ratchet == NULL || resolve(context, hash) == NULL) return false;
    memcpy(ratchet_public, state->ratchet, 32u);
    return true;
}

static lxmf_status_t send_packet(void *context, const uint8_t *packet,
                                 size_t length) {
    send_state_t *state = context;
    assert(length <= sizeof state->packet);
    memcpy(state->packet, packet, length);
    state->length = length;
    return state->fail ? LXMF_ERR_CRYPTO : LXMF_OK;
}

static void delivery(void *context, const uint8_t id[32],
                     lxmf_delivery_status_t status, lxmf_status_t result) {
    send_state_t *state = context;
    (void)id;
    (void)result;
    state->callbacks++;
    state->last_status = status;
}

static void event(void *context, const lxmf_router_event_t *delivery_event) {
    send_state_t *state = context;
    assert(delivery_event != NULL);
    state->events++;
    state->last_event = *delivery_event;
}

typedef struct { size_t count; uint8_t id[32]; } incoming_state_t;

static void incoming(void *context, const lxmf_store_message_t *message) {
    incoming_state_t *state = context;
    assert(message->status == LXMF_DELIVERY_DELIVERED);
    memcpy(state->id, message->message_id, sizeof state->id);
    state->count++;
}

int main(void) {
    char path[] = "/tmp/lxmf-router-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    unlink(path);

    rns_identity alice, bob;
    uint8_t alice_key[64], bob_key[64], source[16], destination[16], body[64];
    for (size_t i = 0; i < sizeof alice_key; i++) {
        alice_key[i] = (uint8_t)(i + 1u);
        bob_key[i] = (uint8_t)(i + 65u);
    }
    assert(rns_identity_from_private(&alice, alice_key));
    assert(rns_identity_from_private(&bob, bob_key));
    const char *aspects[] = {"delivery"};
    assert(rns_destination_hash(&alice, "lxmf", aspects, 1u, source));
    assert(rns_destination_hash(&bob, "lxmf", aspects, 1u, destination));

    lxmf_store_t store = {0};
    assert(lxmf_store_open(&store, path) == LXMF_OK);
    lxmf_store_message_t message = {0};
    message.message_id[0] = 1u;
    memcpy(message.destination, destination, sizeof destination);
    memcpy(message.source, source, sizeof source);
    message.timestamp = 1.0;
    message.status = LXMF_DELIVERY_QUEUED;
    message.content = (lxmf_slice_t){(const uint8_t *)"hello", 5u};
    bool inserted = false;
    assert(lxmf_store_put(&store, &message, &inserted) == LXMF_OK && inserted);

    char ratchet_path[] = "/tmp/lxmf-router-ratchets-XXXXXX";
    fd = mkstemp(ratchet_path);
    assert(fd >= 0);
    close(fd);
    unlink(ratchet_path);
    rns_ratchet_store_t *ratchet_store = NULL;
    assert(rns_ratchet_store_open(&ratchet_store, ratchet_path, &bob, 4u,
                                  30u) == RNS_OK);
    uint8_t ratchet_private[32], ratchet_public[32], ratchet_id[16];
    assert(rns_ratchet_store_current(ratchet_store, 100u, ratchet_private,
                                     ratchet_public, ratchet_id, NULL) ==
           RNS_OK);
    send_state_t state = {.peer = &bob, .ratchet = ratchet_public};
    lxmf_router_t router;
    lxmf_router_config_t config = {
        .identity = &alice, .store = &store, .resolve_identity = resolve,
        .resolve_context = &state, .resolve_ratchet = resolve_ratchet,
        .ratchet_context = &state, .send_packet = send_packet,
        .send_context = &state, .delivery_callback = delivery,
        .delivery_context = &state, .event_callback = event,
        .event_context = &state, .monotonic_clock = clock_ms,
        .monotonic_clock_context = &state, .direct_retry_limit = 2u,
        .direct_retry_base_ms = 10u};
    assert(lxmf_router_init(&router, &config) == LXMF_OK);
    assert(lxmf_router_send_message(&router, message.message_id) == LXMF_OK);
    assert(state.length > 0u && state.last_status == LXMF_DELIVERY_SENT);
    assert(state.events == 2u);
    assert(state.last_event.method == LXMF_DELIVERY_METHOD_OPPORTUNISTIC);
    assert(state.last_event.state == LXMF_DELIVERY_SENT);
    assert(state.last_event.queue_reason == LXMF_QUEUE_REASON_NONE);
    assert(state.last_event.attempt == 1u);
    lxmf_store_message_t got;
    assert(lxmf_store_read(&store, message.message_id, &got, body,
                           sizeof body) == LXMF_OK);
    assert(got.status == LXMF_DELIVERY_SENT);
    assert(got.delivery.desired_method == LXMF_DELIVERY_METHOD_OPPORTUNISTIC);
    assert(got.delivery.actual_method == LXMF_DELIVERY_METHOD_OPPORTUNISTIC);
    assert(got.delivery.attempts == 1u);
    assert(got.delivery.progress == LXMF_DELIVERY_PROGRESS_COMPLETE);
    assert(!got.delivery.has_proof_id);

    char receive_path[] = "/tmp/lxmf-router-receive-XXXXXX";
    fd = mkstemp(receive_path);
    assert(fd >= 0);
    close(fd);
    unlink(receive_path);
    lxmf_store_t received_store = {0};
    assert(lxmf_store_open(&received_store, receive_path) == LXMF_OK);
    send_state_t receiver_state = {.peer = &alice};
    incoming_state_t incoming_state = {0};
    lxmf_router_t receiver;
    lxmf_router_config_t receiver_config = {
        .identity = &bob, .store = &received_store,
        .ratchet_store = ratchet_store, .resolve_identity = resolve,
        .resolve_context = &receiver_state, .send_packet = send_packet,
        .send_context = &receiver_state, .message_callback = incoming,
        .message_context = &incoming_state};
    assert(lxmf_router_init(&receiver, &receiver_config) == LXMF_OK);
    assert(lxmf_router_receive_packet(&receiver, state.packet, state.length) == LXMF_OK);
    assert(incoming_state.count == 1u);
    assert(lxmf_store_read(&received_store, incoming_state.id, &got, body,
                           sizeof body) == LXMF_OK);
    assert(got.status == LXMF_DELIVERY_DELIVERED && got.content.len == 5u);
    assert(lxmf_router_receive_packet(&receiver, state.packet, state.length) == LXMF_OK);
    assert(incoming_state.count == 1u);

    message.message_id[0] = 3u;
    assert(lxmf_store_put(&store, &message, &inserted) == LXMF_OK && inserted);
    state.peer = NULL;
    size_t events_before_wait = state.events;
    assert(lxmf_router_send_message(&router, message.message_id) == LXMF_ERR_PENDING);
    assert(state.events == events_before_wait + 1u);
    assert(state.last_event.state == LXMF_DELIVERY_QUEUED);
    assert(state.last_event.queue_reason == LXMF_QUEUE_REASON_PEER_IDENTITY);
    assert(state.last_event.result == LXMF_ERR_PENDING);
    assert(lxmf_store_read(&store, message.message_id, &got, body,
                           sizeof body) == LXMF_OK);
    assert(got.status == LXMF_DELIVERY_QUEUED);
    assert(got.delivery.queue_reason == LXMF_QUEUE_REASON_PEER_IDENTITY);
    assert(got.delivery.attempts == 0u);
    lxmf_router_poll_result_t result;
    assert(lxmf_router_poll(&router, 1u, &result) == LXMF_OK);
    assert(result.attempted == 1u && result.deferred == 1u);
    assert(result.sent == 0u && result.failed == 0u);
    state.peer = &bob;
    assert(lxmf_store_update_status(&store, message.message_id,
                                    LXMF_DELIVERY_SENT) == LXMF_OK);

    assert(strcmp(lxmf_delivery_method_string(LXMF_DELIVERY_METHOD_DIRECT),
                  "direct") == 0);
    assert(strcmp(lxmf_queue_reason_string(LXMF_QUEUE_REASON_RETRY_BACKOFF),
                  "retry backoff") == 0);
    assert(strcmp(lxmf_status_string(LXMF_ERR_PENDING),
                  "waiting for delivery prerequisite") == 0);

    message.message_id[0] = 2u;
    assert(lxmf_store_put(&store, &message, &inserted) == LXMF_OK && inserted);
    state.fail = true;
    assert(lxmf_router_poll(&router, 1u, &result) == LXMF_OK);
    assert(result.attempted == 1u && result.failed == 1u);
    assert(state.last_status == LXMF_DELIVERY_QUEUED);
    assert(lxmf_store_read(&store, message.message_id, &got, body,
                           sizeof body) == LXMF_OK);
    assert(got.status == LXMF_DELIVERY_QUEUED);
    assert(got.delivery.queue_reason == LXMF_QUEUE_REASON_RETRY_BACKOFF);
    assert(got.delivery.attempts == 1u);
    assert(got.delivery.retry_at_ms == 10u);

    assert(lxmf_router_poll(&router, 1u, &result) == LXMF_OK);
    assert(result.attempted == 1u && result.deferred == 1u);
    state.now_ms = 10u;
    assert(lxmf_router_poll(&router, 1u, &result) == LXMF_OK);
    assert(result.attempted == 1u && result.failed == 1u);
    assert(lxmf_store_read(&store, message.message_id, &got, body,
                           sizeof body) == LXMF_OK);
    assert(got.status == LXMF_DELIVERY_FAILED);
    assert(got.delivery.attempts == 2u);
    assert(got.delivery.queue_reason == LXMF_QUEUE_REASON_RETRY_EXHAUSTED);
    assert(got.delivery.retry_at_ms == 0u);

    /* Exhausted records are not retried by polling. An explicit resend starts
     * a new attempt budget instead of failing immediately at attempt three. */
    state.fail = false;
    assert(lxmf_router_poll(&router, 1u, &result) == LXMF_OK);
    assert(result.attempted == 0u);
    assert(lxmf_router_send_message(&router, message.message_id) == LXMF_OK);
    assert(lxmf_store_read(&store, message.message_id, &got, body,
                           sizeof body) == LXMF_OK);
    assert(got.status == LXMF_DELIVERY_SENT);
    assert(got.delivery.attempts == 1u);
    assert(got.delivery.queue_reason == LXMF_QUEUE_REASON_NONE);
    lxmf_store_close(&received_store);
    unlink(receive_path);
    lxmf_store_close(&store);
    unlink(path);
    rns_ratchet_store_close(ratchet_store);
    unlink(ratchet_path);
    return 0;
}
