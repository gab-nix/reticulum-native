#include "reticulum/destination.h"
#include "reticulum/hal.h"
#include "reticulum/lxmf_delivery.h"
#include "reticulum/packet.h"
#include "reticulum/udp.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    const rns_identity *source;
    uint8_t source_hash[16];
    const rns_identity *local;
    int received;
} test_context;

static void delivery_hash(const rns_identity *identity, uint8_t out[16]) {
    const char *aspects[] = {"delivery"};
    assert(rns_destination_hash(identity, "lxmf", aspects, 1, out));
}

static const rns_identity *resolve(void *context, const uint8_t source[16]) {
    test_context *state = context;
    return memcmp(source, state->source_hash, 16) == 0 ? state->source : NULL;
}

static rns_status_t receive_packet(const uint8_t *packet, size_t packet_length,
                                   const rns_udp_address_t *source, void *context) {
    test_context *state = context;
    lxmf_identity_verifier_context_t verifier = {resolve, state};
    lxmf_message_t message;
    uint8_t plaintext[RNS_MTU];
    size_t plaintext_length;
    (void)source;
    assert(lxmf_opportunistic_packet_unpack(packet, packet_length, state->local,
                                            lxmf_identity_verifier, &verifier,
                                            plaintext, sizeof(plaintext),
                                            &plaintext_length, &message) == LXMF_OK);
    assert(message.content.len == 12);
    assert(memcmp(message.content.data, "network hello", 12) == 0);
    state->received = 1;
    return RNS_OK;
}

static bool receive_until(rns_udp_endpoint_t *receiver, test_context *state,
                          uint64_t timeout_ms) {
    uint64_t started_ms;
    uint64_t current_ms;

    assert(rns_hal_monotonic_ms(&started_ms) == RNS_OK);
    do {
        size_t received = 0U;
        rns_status_t status = rns_udp_poll(receiver, 1U, receive_packet, state, &received);
        assert(status == RNS_OK);
        assert(received <= 1U);
        if (state->received) {
            return true;
        }
        assert(rns_hal_monotonic_ms(&current_ms) == RNS_OK);
    } while (current_ms - started_ms < timeout_ms);
    return false;
}

int main(void) {
    rns_identity alice, bob, bob_public;
    uint8_t public_bytes[64];
    lxmf_message_t message = {0};
    uint8_t packet[RNS_MTU];
    size_t packet_length;
    rns_udp_endpoint_t *sender = NULL, *receiver = NULL;
    rns_udp_address_t receiver_address;
    test_context state = {0};

    assert(rns_identity_generate(&alice));
    assert(rns_identity_generate(&bob));
    rns_identity_export_public(&bob, public_bytes);
    assert(rns_identity_from_public(&bob_public, public_bytes));
    delivery_hash(&alice, message.source);
    delivery_hash(&bob, message.destination);
    message.timestamp = 42.0;
    message.content.data = (const uint8_t *)"network hello";
    message.content.len = 12;
    assert(lxmf_opportunistic_packet_pack(&message, &alice, &bob_public, packet,
                                          sizeof(packet), &packet_length) == LXMF_OK);

    assert(rns_udp_endpoint_create(&receiver, RNS_UDP_IPV4) == RNS_OK);
    assert(rns_udp_endpoint_create(&sender, RNS_UDP_IPV4) == RNS_OK);
    assert(rns_udp_bind(receiver, "127.0.0.1", 0) == RNS_OK);
    assert(rns_udp_local_address(receiver, &receiver_address) == RNS_OK);
    assert(rns_udp_send_to(sender, &receiver_address, packet, packet_length) == RNS_OK);

    state.source = &alice;
    state.local = &bob;
    delivery_hash(&alice, state.source_hash);
    assert(receive_until(receiver, &state, 1000U));
    rns_udp_endpoint_destroy(sender);
    rns_udp_endpoint_destroy(receiver);
    return 0;
}
