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
    size_t identified_count;
    uint8_t identified_public[RNS_IDENTITY_PUBLIC_SIZE];
    size_t request_handler_calls;
    size_t request_completed;
    uint8_t request_response[4096];
    size_t request_response_length;
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

static void link_identified(rns_runtime_link_t *link,
                            const rns_identity *identity, void *context) {
    link_observation_t *observation = context;
    assert(link == observation->accepted);
    assert(identity == rns_runtime_link_remote_identity(link));
    rns_identity_export_public(identity, observation->identified_public);
    observation->identified_count++;
}

static rns_status_t request_handled(
    rns_runtime_request_handler_t *handler, rns_runtime_link_t *link,
    const rns_request_view_t *request, const rns_identity *remote_identity,
    uint8_t *response, size_t response_capacity, size_t *response_length,
    void *context) {
    link_observation_t *observation = context;
    assert(link == observation->accepted);
    assert(remote_identity == rns_runtime_link_remote_identity(link));
    assert(request->data_msgpack_length == 1U &&
           request->data_msgpack[0] == 0xc0U);
    const char *path = rns_runtime_request_handler_path(handler);
    size_t body_length = strcmp(path, "/small") == 0 ? 5U : 2048U;
    size_t header_length = body_length <= UINT8_MAX ? 2U : 3U;
    assert(response_capacity >= header_length + body_length);
    if (header_length == 2U) {
        response[0] = 0xc4U;
        response[1] = (uint8_t)body_length;
    } else {
        response[0] = 0xc5U;
        response[1] = (uint8_t)(body_length >> 8U);
        response[2] = (uint8_t)body_length;
    }
    memset(response + header_length,
           strcmp(path, "/small") == 0 ? 's' : 'R', body_length);
    *response_length = header_length + body_length;
    observation->request_handler_calls++;
    return RNS_OK;
}

static void request_completed(rns_request_receipt_t *receipt,
                              rns_request_state_t state, rns_status_t status,
                              const uint8_t *response,
                              size_t response_length, void *context) {
    link_observation_t *observation = context;
    assert(receipt != NULL);
    if (state == RNS_REQUEST_CANCELLED) {
        assert(status == RNS_OK && response == NULL && response_length == 0U);
        return;
    }
    assert(state == RNS_REQUEST_COMPLETE && status == RNS_OK);
    assert(response_length <= sizeof observation->request_response);
    memcpy(observation->request_response, response, response_length);
    observation->request_response_length = response_length;
    observation->request_completed++;
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
        .prove_data_packets = true,
        .state_callback = link_state_changed,
        .packet_callback = link_packet_received,
        .identified_callback = link_identified,
        .callback_context = &responder};
    rns_runtime_destination_t *registration = NULL;
    assert(rns_runtime_register_link_destination(
               responder_runtime, destination_hash, &responder_identity,
               &responder_options, inbound_link_accepted, &responder,
               &registration) == RNS_OK);
    rns_runtime_request_handler_options_t handler_options = {
        .access = RNS_REQUEST_ALLOW_IDENTIFIED,
        .max_response_size = 4096U,
        .callback = request_handled,
        .callback_context = &responder};
    rns_runtime_request_handler_t *small_handler = NULL;
    rns_runtime_request_handler_t *large_handler = NULL;
    rns_runtime_request_handler_t *blocked_handler = NULL;
    assert(rns_runtime_destination_register_request_handler(
               registration, "/small", &handler_options, &small_handler) ==
           RNS_OK);
    assert(rns_runtime_destination_register_request_handler(
               registration, "/large", &handler_options, &large_handler) ==
           RNS_OK);
    handler_options.access = RNS_REQUEST_ALLOW_NONE;
    assert(rns_runtime_destination_register_request_handler(
               registration, "/blocked", &handler_options,
               &blocked_handler) == RNS_OK);
    assert(strcmp(rns_runtime_request_handler_path(small_handler),
                  "/small") == 0);

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
    assert(rns_runtime_link_remote_identity(responder.accepted) == NULL);
    assert(rns_runtime_link_remote_identity(outbound) != NULL);

    rns_identity initiator_identity;
    uint8_t initiator_public[RNS_IDENTITY_PUBLIC_SIZE];
    assert(rns_identity_generate(&initiator_identity));
    rns_identity_export_public(&initiator_identity, initiator_public);
    assert(rns_runtime_link_identify(outbound, &initiator_identity) == RNS_OK);
    for (size_t attempt = 0U; attempt < 1000U &&
         responder.identified_count == 0U; ++attempt)
        assert(rns_runtime_poll(responder_runtime, 8U, &processed) == RNS_OK);
    assert(responder.identified_count == 1U);
    assert(memcmp(responder.identified_public, initiator_public,
                  sizeof initiator_public) == 0);

    rns_request_options_t request_options = {
        .timeout_seconds = 2.0,
        .max_response_size = sizeof initiator.request_response,
        .callback = request_completed,
        .callback_context = &initiator};
    rns_request_receipt_t *request_receipt = NULL;
    assert(rns_runtime_link_request(outbound, "/small", NULL, 0U,
                                    &request_options, &request_receipt) ==
           RNS_OK);
    for (size_t attempt = 0U; attempt < 1000U &&
         initiator.request_completed == 0U; ++attempt) {
        assert(rns_runtime_poll(responder_runtime, 8U, &processed) == RNS_OK);
        assert(rns_runtime_poll(initiator_runtime, 8U, &processed) == RNS_OK);
    }
    assert(responder.request_handler_calls == 1U);
    assert(initiator.request_response_length == 5U);
    assert(memcmp(initiator.request_response, "sssss", 5U) == 0);
    rns_request_receipt_destroy(request_receipt);

    request_receipt = NULL;
    assert(rns_runtime_link_request(outbound, "/blocked", NULL, 0U,
                                    &request_options, &request_receipt) ==
           RNS_OK);
    for (size_t attempt = 0U; attempt < 8U; ++attempt) {
        assert(rns_runtime_poll(responder_runtime, 8U, &processed) == RNS_OK);
        assert(rns_runtime_poll(initiator_runtime, 8U, &processed) == RNS_OK);
    }
    assert(responder.request_handler_calls == 1U);
    assert(initiator.request_completed == 1U);
    rns_request_receipt_cancel(request_receipt);
    assert(rns_request_receipt_state(request_receipt) ==
           RNS_REQUEST_CANCELLED);
    rns_request_receipt_destroy(request_receipt);

    request_receipt = NULL;
    assert(rns_runtime_link_request(outbound, "/large", NULL, 0U,
                                    &request_options, &request_receipt) ==
           RNS_OK);
    for (size_t attempt = 0U; attempt < 4000U &&
         initiator.request_completed == 1U; ++attempt) {
        assert(rns_runtime_poll(responder_runtime, 8U, &processed) == RNS_OK);
        assert(rns_runtime_poll(initiator_runtime, 8U, &processed) == RNS_OK);
    }
    assert(responder.request_handler_calls == 2U);
    assert(initiator.request_completed == 2U);
    assert(initiator.request_response_length == 2048U);
    for (size_t i = 0U; i < initiator.request_response_length; ++i)
        assert(initiator.request_response[i] == 'R');
    rns_request_receipt_destroy(request_receipt);

    static const uint8_t message[] = "authenticated link packet";
    assert(rns_runtime_link_send(outbound, 0U, message,
                                 sizeof message - 1U) == RNS_OK);
    for (size_t attempt = 0U; attempt < 1000U && responder.packet_count == 0U;
         ++attempt)
        assert(rns_runtime_poll(responder_runtime, 8U, &processed) == RNS_OK);
    assert(responder.packet_count == 1U && responder.context == 0U);
    assert(responder.payload_length == sizeof message - 1U);
    assert(memcmp(responder.payload, message, sizeof message - 1U) == 0);

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
    rns_runtime_request_handler_destroy(blocked_handler);
    rns_runtime_request_handler_destroy(large_handler);
    rns_runtime_request_handler_destroy(small_handler);
    rns_runtime_destination_destroy(registration);
    rns_runtime_destroy(responder_runtime);
    rns_runtime_destroy(initiator_runtime);
    return 0;
}
