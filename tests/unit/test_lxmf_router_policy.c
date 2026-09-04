#include "reticulum/destination.h"
#include "reticulum/lxmf_delivery.h"
#include "reticulum/lxmf_router.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    rns_identity sender, recipient;
    uint8_t sender_hash[16];
    bool known, blocked;
    size_t messages, signature_events, policy_calls;
    lxmf_router_event_t event;
} fixture_t;

static const rns_identity *resolve(void *context, const uint8_t hash[16]) {
    fixture_t *fixture = context;
    return fixture->known && memcmp(hash, fixture->sender_hash, 16) == 0
        ? &fixture->sender : NULL;
}

static bool blocked(void *context, const uint8_t hash[16]) {
    fixture_t *fixture = context;
    fixture->policy_calls++;
    return fixture->blocked && memcmp(hash, fixture->sender_hash, 16) == 0;
}

static lxmf_status_t no_send(void *context, const uint8_t *packet, size_t size) {
    (void)context; (void)packet; (void)size;
    return LXMF_OK;
}

static void received(void *context, const lxmf_store_message_t *message) {
    (void)message;
    ((fixture_t *)context)->messages++;
}

static void signature(void *context, const uint8_t id[32],
                        lxmf_signature_state_t state) {
    (void)id; (void)state;
    ((fixture_t *)context)->signature_events++;
}

static void event(void *context, const lxmf_router_event_t *value) {
    ((fixture_t *)context)->event = *value;
}

static size_t packet(fixture_t *fixture, double timestamp, uint8_t wire[500],
                      size_t *representation_length) {
    static const char *const aspects[] = {"delivery"};
    lxmf_message_t message = {.timestamp = timestamp,
        .content = {(const uint8_t *)"synthetic policy test", 21}};
    memcpy(message.source, fixture->sender_hash, 16);
    assert(rns_destination_hash(&fixture->recipient, "lxmf", aspects, 1,
                                message.destination));
    uint8_t representation[500];
    assert(lxmf_pack(&message, lxmf_identity_signer, &fixture->sender,
                     representation, sizeof representation,
                     representation_length) == LXMF_OK);
    size_t size;
    assert(lxmf_opportunistic_packet_pack(&message, &fixture->sender,
        &fixture->recipient, wire, 500, &size) == LXMF_OK);
    return size;
}

int main(void) {
    fixture_t fixture = {.known = true, .blocked = true};
    uint8_t a[64], b[64];
    for (size_t i = 0; i < sizeof a; ++i) {
        a[i] = (uint8_t)i;
        b[i] = (uint8_t)(i + 64);
    }
    assert(rns_identity_from_private(&fixture.sender, a));
    assert(rns_identity_from_private(&fixture.recipient, b));
    static const char *const aspects[] = {"delivery"};
    assert(rns_destination_hash(&fixture.sender, "lxmf", aspects, 1,
                                fixture.sender_hash));
    char path[] = "/tmp/lxmf-router-policy-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    lxmf_store_t store = {0};
    assert(lxmf_store_open(&store, path) == LXMF_OK);
    lxmf_router_config_t config = {.identity = &fixture.recipient,
        .store = &store, .resolve_identity = resolve,
        .resolve_context = &fixture, .send_packet = no_send,
        .is_source_blocked = blocked, .source_policy_context = &fixture,
        .message_callback = received, .message_context = &fixture,
        .signature_callback = signature, .signature_context = &fixture,
        .event_callback = event, .event_context = &fixture};
    lxmf_router_t router;
    config.max_incoming_message_size = LXMF_STORE_MAX_PACKED + 1u;
    assert(lxmf_router_init(&router, &config) == LXMF_ERR_ARGUMENT);
    config.max_incoming_message_size = 0;
    assert(lxmf_router_init(&router, &config) == LXMF_OK);
    uint8_t wire[500];
    size_t packed_size, wire_size = packet(&fixture, 1, wire, &packed_size);
    assert(lxmf_router_receive_packet(&router, wire, wire_size) == LXMF_ERR_BLOCKED);
    assert(fixture.messages == 0 && fixture.signature_events == 0 &&
           lxmf_store_count(&store) == 0 && fixture.event.result == LXMF_ERR_BLOCKED);
    assert(strcmp(lxmf_status_string(LXMF_ERR_BLOCKED),
                   "source blocked by inbound policy") == 0);
    /* Block by claimed source even without the public identity. */
    fixture.known = false;
    assert(lxmf_router_receive_packet(&router, wire, wire_size) == LXMF_ERR_BLOCKED);
    assert(lxmf_store_count(&store) == 0);
    /* Accepting a source does not exempt it from stamp enforcement. */
    fixture.blocked = false;
    fixture.known = true;
    assert(lxmf_router_set_inbound_stamp_cost(&router, 1) == LXMF_OK);
    assert(lxmf_router_receive_packet(&router, wire, wire_size) == LXMF_ERR_STAMP);
    assert(lxmf_store_count(&store) == 0 && fixture.messages == 0);
    assert(lxmf_router_set_inbound_stamp_cost(&router, 0) == LXMF_OK);
    fixture.known = false;
    assert(lxmf_router_receive_packet(&router, wire, wire_size) == LXMF_OK);
    assert(lxmf_store_count(&store) == 1 && fixture.messages == 1);
    /* New block policy is applied during deferred verification, including
     * before the identity has arrived. It is not a signature-failure event. */
    fixture.blocked = true;
    lxmf_router_verify_result_t verified;
    assert(lxmf_router_verify_pending(&router, NULL, &verified) == LXMF_OK &&
           verified.rejected == 1 && verified.verified == 0 && verified.pending == 0);
    assert(lxmf_store_count(&store) == 0 && fixture.signature_events == 0 &&
           fixture.event.result == LXMF_ERR_BLOCKED);
    fixture.blocked = false;
    wire_size = packet(&fixture, 2, wire, &packed_size);
    assert(lxmf_router_receive_packet(&router, wire, wire_size) == LXMF_OK);
    fixture.known = true;
    fixture.blocked = true;
    assert(lxmf_router_verify_pending(&router, fixture.sender_hash, &verified) ==
           LXMF_OK && verified.rejected == 1 && fixture.signature_events == 0);
    fixture.blocked = false;
    fixture.known = false;
    wire_size = packet(&fixture, 3, wire, &packed_size);
    assert(lxmf_router_receive_packet(&router, wire, wire_size) == LXMF_OK);
    fixture.known = true;
    assert(lxmf_router_verify_pending(&router, NULL, &verified) == LXMF_OK &&
           verified.verified == 1 && fixture.signature_events == 1);
    lxmf_router_destroy(&router);
    /* Exact packed-size policy applies to encrypted packets as well as
     * Resources; the wire ciphertext length is not the policy measurement. */
    config.max_incoming_message_size = packed_size - 1;
    assert(lxmf_router_init(&router, &config) == LXMF_OK);
    wire_size = packet(&fixture, 4, wire, &packed_size);
    assert(lxmf_router_receive_packet(&router, wire, wire_size) == LXMF_ERR_BOUNDS);
    assert(lxmf_store_count(&store) == 1);
    lxmf_router_destroy(&router);
    config.max_incoming_message_size = packed_size;
    assert(lxmf_router_init(&router, &config) == LXMF_OK);
    fixture.known = false;
    assert(lxmf_router_receive_packet(&router, wire, wire_size) == LXMF_OK);
    lxmf_router_destroy(&router);
    /* Tightening the size policy also cleans up oversized retained unknown
     * senders without claiming their signatures are bad. */
    config.max_incoming_message_size = packed_size - 1;
    assert(lxmf_router_init(&router, &config) == LXMF_OK);
    assert(lxmf_router_verify_pending(&router, NULL, &verified) == LXMF_OK &&
           verified.rejected == 1 && fixture.signature_events == 1 &&
           fixture.event.result == LXMF_ERR_BOUNDS);
    lxmf_router_destroy(&router);
    lxmf_store_close(&store);
    unlink(path);
    return 0;
}
