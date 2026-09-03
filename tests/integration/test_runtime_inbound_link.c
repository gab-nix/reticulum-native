#include "reticulum/destination.h"
#include "reticulum/packet.h"
#include "reticulum/runtime.h"
#include "reticulum/udp.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>

typedef struct link_observation {
    rns_runtime_link_t *accepted;
    size_t accepted_count;
    size_t active_count;
    size_t closed_count;
    size_t packet_count;
    uint8_t context;
    uint8_t payload[64];
    size_t payload_length;
    bool prove_packets;
    bool receipt_delivered;
} link_observation_t;

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

static void link_state_changed(rns_runtime_link_t *link, rns_link_state state,
                               rns_status_t reason, void *context) {
    link_observation_t *observation = context;
    assert(link != NULL);
    assert(reason == RNS_OK);
    if (state == RNS_LINK_ACTIVE) observation->active_count++;
    if (state == RNS_LINK_CLOSED) observation->closed_count++;
}

static void link_packet_received(rns_runtime_link_t *link, uint8_t context,
                                 const uint8_t *plaintext,
                                 size_t plaintext_length, void *opaque) {
    link_observation_t *observation = opaque;
    assert(link == observation->accepted);
    assert(plaintext_length <= sizeof observation->payload);
    observation->context = context;
    memcpy(observation->payload, plaintext, plaintext_length);
    observation->payload_length = plaintext_length;
    observation->packet_count++;
    if (observation->prove_packets)
        assert(rns_runtime_link_prove_current_packet(link) == RNS_OK);
}

static void receipt_changed(rns_packet_receipt_t *receipt,
                            rns_packet_receipt_state_t state,
                            rns_status_t status, void *opaque) {
    link_observation_t *observation = opaque;
    assert(receipt != NULL);
    assert(status == RNS_OK);
    if (state == RNS_PACKET_RECEIPT_DELIVERED)
        observation->receipt_delivered = true;
}

static void inbound_link_accepted(rns_runtime_destination_t *destination,
                                  rns_runtime_link_t *link, void *context) {
    link_observation_t *observation = context;
    assert(destination != NULL && link != NULL);
    assert(rns_runtime_destination_hash(destination) != NULL);
    assert(rns_runtime_link_state(link) == RNS_LINK_HANDSHAKE);
    observation->accepted = link;
    observation->accepted_count++;
}

int main(void) {
    uint16_t initiator_port = reserve_udp_port();
    uint16_t responder_port = reserve_udp_port();
    assert(initiator_port != responder_port);
    rns_config_t initiator_config, responder_config;
    configure_udp(&initiator_config, "initiator", initiator_port,
                  responder_port);
    configure_udp(&responder_config, "responder", responder_port,
                  initiator_port);

    rns_runtime_t *initiator_runtime = NULL;
    rns_runtime_t *responder_runtime = NULL;
    assert(rns_runtime_create(&initiator_runtime, &initiator_config, NULL) ==
           RNS_OK);
    assert(rns_runtime_create(&responder_runtime, &responder_config, NULL) ==
           RNS_OK);

    rns_identity responder_identity;
    assert(rns_identity_generate(&responder_identity));
    static const char *const aspects[] = {"delivery"};
    uint8_t destination_hash[16];
    assert(rns_destination_hash(&responder_identity, "lxmf", aspects, 1U,
                                destination_hash));

    link_observation_t responder = {0};
    rns_runtime_link_options_t responder_options = {
        .timeout_seconds = 2.0,
        .state_callback = link_state_changed,
        .packet_callback = link_packet_received,
        .callback_context = &responder};
    rns_runtime_destination_t *registration = NULL;
    assert(rns_runtime_register_link_destination(
               responder_runtime, destination_hash, &responder_identity,
               &responder_options, inbound_link_accepted, &responder,
               &registration) == RNS_OK);

    /* A malformed request to a registered destination is consumed without
     * allocating a responder link or reaching application packet delivery. */
    static const uint8_t malformed[] = {0x01U};
    rns_packet malformed_packet = {0};
    malformed_packet.packet_type = 2U;
    memcpy(malformed_packet.destination_hash, destination_hash, 16U);
    malformed_packet.data = malformed;
    malformed_packet.data_length = sizeof malformed;
    uint8_t malformed_raw[RNS_MTU];
    size_t malformed_length = 0U;
    assert(rns_packet_encode(&malformed_packet, malformed_raw,
                             sizeof malformed_raw, &malformed_length));
    assert(rns_runtime_send(initiator_runtime, 0U, malformed_raw,
                            malformed_length) == RNS_OK);
    size_t processed = 0U;
    assert(rns_runtime_poll(responder_runtime, 8U, &processed) == RNS_OK);
    assert(responder.accepted_count == 0U);

    assert(rns_runtime_announce(responder_runtime, &responder_identity, "lxmf",
                                aspects, 1U, NULL, 0U) == RNS_OK);
    bool path_known = false;
    for (size_t attempt = 0U; attempt < 1000U && !path_known; ++attempt) {
        assert(rns_runtime_poll(initiator_runtime, 8U, &processed) == RNS_OK);
        rns_path_entry path;
        path_known = rns_runtime_path_lookup(initiator_runtime, destination_hash,
                                             &path) == RNS_OK;
    }
    assert(path_known);

    link_observation_t initiator = {0};
    rns_runtime_link_options_t initiator_options = {
        .timeout_seconds = 2.0,
        .state_callback = link_state_changed,
        .callback_context = &initiator};
    rns_runtime_link_t *outbound = NULL;
    assert(rns_runtime_link_open(initiator_runtime, destination_hash,
                                 &responder_identity, &initiator_options,
                                 &outbound) == RNS_OK);
    for (size_t attempt = 0U; attempt < 1000U &&
         (rns_runtime_link_state(outbound) != RNS_LINK_ACTIVE ||
          responder.accepted == NULL ||
          rns_runtime_link_state(responder.accepted) != RNS_LINK_ACTIVE);
         ++attempt) {
        assert(rns_runtime_poll(responder_runtime, 8U, &processed) == RNS_OK);
        assert(rns_runtime_poll(initiator_runtime, 8U, &processed) == RNS_OK);
    }
    assert(responder.accepted_count == 1U && responder.active_count == 1U);
    assert(initiator.active_count == 1U);
    assert(rns_runtime_link_state(outbound) == RNS_LINK_ACTIVE);
    assert(rns_runtime_link_state(responder.accepted) == RNS_LINK_ACTIVE);
    assert(memcmp(rns_runtime_link_id(outbound),
                  rns_runtime_link_id(responder.accepted), 16U) == 0);

    static const uint8_t message[] = "authenticated link packet";
    assert(rns_runtime_link_send(outbound, 0U, message,
                                 sizeof message - 1U) == RNS_OK);
    for (size_t attempt = 0U; attempt < 1000U && responder.packet_count == 0U;
         ++attempt)
        assert(rns_runtime_poll(responder_runtime, 8U, &processed) == RNS_OK);
    assert(responder.packet_count == 1U && responder.context == 0U);
    assert(responder.payload_length == sizeof message - 1U);
    assert(memcmp(responder.payload, message, sizeof message - 1U) == 0);

    responder.prove_packets = true;
    static const uint8_t proved_message[] = "proved link packet";
    rns_packet_receipt_t *receipt = NULL;
    rns_packet_receipt_options_t receipt_options = {
        .timeout_seconds = 2.0,
        .callback = receipt_changed,
        .callback_context = &initiator};
    assert(rns_runtime_link_send_with_receipt(
               outbound, 0U, proved_message, sizeof proved_message - 1U,
               &receipt_options, &receipt) == RNS_OK);
    assert(receipt != NULL);
    assert(rns_runtime_link_prove_current_packet(outbound) ==
           RNS_ERROR_INVALID_STATE);
    for (size_t attempt = 0U; attempt < 1000U &&
         !initiator.receipt_delivered; ++attempt) {
        assert(rns_runtime_poll(responder_runtime, 8U, &processed) == RNS_OK);
        assert(rns_runtime_poll(initiator_runtime, 8U, &processed) == RNS_OK);
    }
    assert(responder.packet_count == 2U);
    assert(initiator.receipt_delivered);
    assert(rns_packet_receipt_state(receipt) ==
           RNS_PACKET_RECEIPT_DELIVERED);
    assert(rns_packet_receipt_rtt(receipt) >= 0.0);
    rns_packet_receipt_destroy(receipt);

    /* Link keepalives are raw protocol bytes, not token-encrypted payloads,
     * and are never dispatched to the application callback. */
    static const uint8_t keepalive_request = 0xffU;
    static const uint8_t keepalive_response = 0xfeU;
    assert(rns_runtime_link_send(outbound, RNS_LINK_CONTEXT_KEEPALIVE,
                                 &keepalive_request, 1U) == RNS_OK);
    assert(rns_runtime_link_send(outbound, RNS_LINK_CONTEXT_KEEPALIVE,
                                 &keepalive_response, 1U) ==
           RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_runtime_poll(responder_runtime, 8U, &processed) == RNS_OK);
    assert(responder.packet_count == 2U);
    assert(rns_runtime_link_send(responder.accepted,
                                 RNS_LINK_CONTEXT_KEEPALIVE,
                                 &keepalive_response, 1U) == RNS_OK);
    assert(rns_runtime_poll(initiator_runtime, 8U, &processed) == RNS_OK);
    assert(rns_runtime_link_state(outbound) == RNS_LINK_ACTIVE);

    rns_runtime_link_destroy(outbound);
    for (size_t attempt = 0U; attempt < 1000U && responder.closed_count == 0U;
         ++attempt)
        assert(rns_runtime_poll(responder_runtime, 8U, &processed) == RNS_OK);
    assert(responder.closed_count == 1U);
    rns_runtime_link_destroy(responder.accepted);
    rns_runtime_destination_destroy(registration);
    rns_runtime_destroy(responder_runtime);
    rns_runtime_destroy(initiator_runtime);
    return 0;
}
