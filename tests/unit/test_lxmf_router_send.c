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
} send_state_t;

static const rns_identity *resolve(void *context, const uint8_t hash[16]) {
    send_state_t *state = context;
    uint8_t expected[16];
    const char *aspects[] = {"delivery"};
    return rns_destination_hash(state->peer, "lxmf", aspects, 1u, expected) &&
                   memcmp(expected, hash, sizeof expected) == 0
               ? state->peer
               : NULL;
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

    send_state_t state = {.peer = &bob};
    lxmf_router_t router;
    lxmf_router_config_t config = {
        .identity = &alice, .store = &store, .resolve_identity = resolve,
        .resolve_context = &state, .send_packet = send_packet,
        .send_context = &state, .delivery_callback = delivery,
        .delivery_context = &state};
    assert(lxmf_router_init(&router, &config) == LXMF_OK);
    assert(lxmf_router_send_message(&router, message.message_id) == LXMF_OK);
    assert(state.length > 0u && state.last_status == LXMF_DELIVERY_SENT);
    lxmf_store_message_t got;
    assert(lxmf_store_read(&store, message.message_id, &got, body,
                           sizeof body) == LXMF_OK);
    assert(got.status == LXMF_DELIVERY_SENT);

    message.message_id[0] = 2u;
    assert(lxmf_store_put(&store, &message, &inserted) == LXMF_OK && inserted);
    state.fail = true;
    lxmf_router_poll_result_t result;
    assert(lxmf_router_poll(&router, 1u, &result) == LXMF_OK);
    assert(result.attempted == 1u && result.failed == 1u);
    assert(state.last_status == LXMF_DELIVERY_FAILED);
    assert(lxmf_store_read(&store, message.message_id, &got, body,
                           sizeof body) == LXMF_OK);
    assert(got.status == LXMF_DELIVERY_FAILED);

    state.fail = false;
    assert(lxmf_router_poll(&router, 1u, &result) == LXMF_OK);
    assert(result.attempted == 1u && result.sent == 1u);
    assert(lxmf_store_read(&store, message.message_id, &got, body,
                           sizeof body) == LXMF_OK);
    assert(got.status == LXMF_DELIVERY_SENT);
    lxmf_store_close(&store);
    unlink(path);
    return 0;
}
