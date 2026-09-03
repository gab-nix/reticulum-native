#include "reticulum/destination.h"
#include "reticulum/hal.h"
#include "reticulum/packet.h"
#include "reticulum/runtime.h"
#include "reticulum/udp.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>

typedef struct receiver_context {
    rns_identity *identity;
    size_t packets;
    rns_status_t proof_status;
} receiver_context_t;

typedef struct receipt_context {
    size_t callbacks;
    rns_packet_receipt_state_t state;
    rns_status_t status;
} receipt_context_t;

static uint16_t reserve_udp_port(void) {
    rns_udp_endpoint_t *endpoint = NULL;
    rns_udp_address_t address;
    assert(rns_udp_endpoint_create(&endpoint, RNS_UDP_IPV4) == RNS_OK);
    assert(rns_udp_bind(endpoint, "127.0.0.1", 0U) == RNS_OK);
    assert(rns_udp_local_address(endpoint, &address) == RNS_OK);
    rns_udp_endpoint_destroy(endpoint);
    return address.port;
}

static void configure_udp(rns_config_t *config, const char *name,
                          uint16_t listen_port, uint16_t forward_port) {
    rns_config_init(config);
    config->interface_count = 1U;
    rns_config_interface_t *interface = &config->interfaces[0];
    (void)strcpy(interface->name, name);
    interface->type = RNS_CONFIG_UDP;
    interface->type_set = true;
    interface->enabled = true;
    (void)strcpy(interface->listen_ip, "127.0.0.1");
    (void)strcpy(interface->forward_ip, "127.0.0.1");
    interface->listen_port = listen_port;
    interface->forward_port = forward_port;
}

static void receive_and_prove(rns_runtime_t *runtime, const uint8_t *packet,
                              size_t packet_length,
                              const rns_node_result *result, void *context) {
    receiver_context_t *receiver = context;
    rns_packet decoded;
    assert(rns_packet_decode(&decoded, packet, packet_length));
    if (decoded.packet_type != 0U) return;
    receiver->packets++;
    receiver->proof_status =
        rns_runtime_prove_packet(runtime, result, receiver->identity, true);
}

static void receipt_changed(rns_packet_receipt_t *receipt,
                            rns_packet_receipt_state_t state,
                            rns_status_t status, void *context) {
    receipt_context_t *observed = context;
    assert(receipt != NULL);
    observed->callbacks++;
    observed->state = state;
    observed->status = status;
}

int main(void) {
    uint16_t alice_port = reserve_udp_port();
    uint16_t bob_port = reserve_udp_port();
    assert(alice_port != bob_port);
    rns_config_t alice_config, bob_config;
    configure_udp(&alice_config, "alice", alice_port, bob_port);
    configure_udp(&bob_config, "bob", bob_port, alice_port);

    rns_identity bob;
    assert(rns_identity_generate(&bob));
    receiver_context_t receiver = {.identity = &bob,
                                   .proof_status = RNS_ERROR_INVALID_STATE};
    rns_runtime_options_t bob_options = {
        .packet_callback = receive_and_prove,
        .callback_context = &receiver};
    rns_runtime_t *alice_runtime = NULL;
    rns_runtime_t *bob_runtime = NULL;
    assert(rns_runtime_create(&alice_runtime, &alice_config, NULL) == RNS_OK);
    assert(rns_runtime_create(&bob_runtime, &bob_config, &bob_options) == RNS_OK);

    static const char *const aspects[] = {"delivery"};
    uint8_t bob_destination[16];
    assert(rns_destination_hash(&bob, "lxmf", aspects, 1U, bob_destination));
    assert(rns_runtime_register_destination(bob_runtime, bob_destination) == RNS_OK);
    assert(rns_runtime_announce(bob_runtime, &bob, "lxmf", aspects, 1U,
                                NULL, 0U) == RNS_OK);
    bool path_known = false;
    for (size_t attempt = 0U; attempt < 1000U && !path_known; ++attempt) {
        size_t processed = 0U;
        assert(rns_runtime_poll(alice_runtime, 8U, &processed) == RNS_OK);
        rns_path_entry path;
        path_known = rns_runtime_path_lookup(alice_runtime, bob_destination,
                                             &path) == RNS_OK;
    }
    assert(path_known);

    static const uint8_t body[] = "prove this packet";
    rns_packet packet = {0};
    memcpy(packet.destination_hash, bob_destination, 16U);
    packet.data = body;
    packet.data_length = sizeof body - 1U;
    uint8_t raw[RNS_MTU];
    size_t raw_length = 0U;
    assert(rns_packet_encode(&packet, raw, sizeof raw, &raw_length));

    receipt_context_t observed = {0};
    rns_packet_receipt_options_t receipt_options = {
        .timeout_seconds = 2.0,
        .callback = receipt_changed,
        .callback_context = &observed};
    rns_packet_receipt_t *receipt = NULL;
    assert(rns_runtime_send_routed_with_receipt(
               alice_runtime, raw, raw_length, &bob, &receipt_options,
               &receipt) == RNS_OK);
    assert(receipt != NULL);
    assert(rns_packet_receipt_state(receipt) == RNS_PACKET_RECEIPT_PENDING);
    assert(rns_packet_receipt_hash(receipt) != NULL);
    bool delivered = false;
    for (size_t attempt = 0U; attempt < 1000U && !delivered; ++attempt) {
        size_t processed = 0U;
        assert(rns_runtime_poll(bob_runtime, 8U, &processed) == RNS_OK);
        assert(rns_runtime_poll(alice_runtime, 8U, &processed) == RNS_OK);
        delivered = rns_packet_receipt_state(receipt) ==
                    RNS_PACKET_RECEIPT_DELIVERED;
    }
    assert(delivered);
    assert(receiver.packets == 1U && receiver.proof_status == RNS_OK);
    assert(observed.callbacks == 1U &&
           observed.state == RNS_PACKET_RECEIPT_DELIVERED &&
           observed.status == RNS_OK);
    assert(rns_packet_receipt_rtt(receipt) >= 0.0);
    rns_packet_receipt_destroy(receipt);

    /* A sent packet is not delivered merely because the interface accepted
     * it; without polling the receiver it reaches the timeout callback. */
    receipt = NULL;
    memset(&observed, 0, sizeof observed);
    receipt_options.timeout_seconds = 0.001;
    packet.context = 1U;
    assert(rns_packet_encode(&packet, raw, sizeof raw, &raw_length));
    assert(rns_runtime_send_routed_with_receipt(
               alice_runtime, raw, raw_length, &bob, &receipt_options,
               &receipt) == RNS_OK);
    assert(rns_hal_sleep_ms(5U) == RNS_OK);
    size_t processed = 0U;
    assert(rns_runtime_poll(alice_runtime, 8U, &processed) == RNS_OK);
    assert(rns_packet_receipt_state(receipt) == RNS_PACKET_RECEIPT_FAILED);
    assert(observed.callbacks == 1U &&
           observed.state == RNS_PACKET_RECEIPT_FAILED &&
           observed.status == RNS_ERROR_TIMEOUT);
    rns_packet_receipt_destroy(receipt);

    /* Cancellation is terminal and reported exactly once. */
    receipt = NULL;
    memset(&observed, 0, sizeof observed);
    receipt_options.timeout_seconds = 2.0;
    packet.context = 2U;
    assert(rns_packet_encode(&packet, raw, sizeof raw, &raw_length));
    assert(rns_runtime_send_routed_with_receipt(
               alice_runtime, raw, raw_length, &bob, &receipt_options,
               &receipt) == RNS_OK);
    rns_packet_receipt_cancel(receipt);
    rns_packet_receipt_cancel(receipt);
    assert(observed.callbacks == 1U &&
           observed.state == RNS_PACKET_RECEIPT_CANCELLED);
    rns_packet_receipt_destroy(receipt);

    rns_runtime_destroy(bob_runtime);
    rns_runtime_destroy(alice_runtime);
    return 0;
}
