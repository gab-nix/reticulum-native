#define _POSIX_C_SOURCE 200809L

#include "reticulum/destination.h"
#include "reticulum/runtime.h"
#include "reticulum/udp.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

typedef struct observation {
    rns_runtime_link_t *accepted_link;
    bool accept_resources;
    size_t accepted;
    size_t received;
    size_t receive_failures;
    size_t packet_callbacks;
    uint8_t received_data[2048];
    size_t received_length;
    size_t transfer_callbacks;
    rns_runtime_resource_state_t transfer_state;
    rns_status_t transfer_status;
} observation_t;

typedef struct fixture {
    rns_runtime_t *initiator;
    rns_runtime_t *responder;
    rns_runtime_link_t *outbound;
    rns_runtime_destination_t *destination;
    observation_t sender;
    observation_t receiver;
} fixture_t;

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

static bool accept_resource(rns_runtime_link_t *link,
                            const rns_resource_advertisement_t *advertisement,
                            void *context) {
    observation_t *observation = context;
    assert(link == observation->accepted_link);
    assert(advertisement->data_size <= sizeof observation->received_data);
    observation->accepted++;
    return observation->accept_resources;
}

static void receive_resource(rns_runtime_link_t *link,
                             const uint8_t resource_hash[32],
                             rns_status_t status, const uint8_t *data,
                             size_t data_length, void *context) {
    observation_t *observation = context;
    assert(link == observation->accepted_link);
    assert(resource_hash != NULL);
    if (status != RNS_OK) {
        observation->receive_failures++;
        assert(data == NULL && data_length == 0U);
        return;
    }
    assert(data != NULL && data_length <= sizeof observation->received_data);
    memcpy(observation->received_data, data, data_length);
    observation->received_length = data_length;
    observation->received++;
}

static void packet_received(rns_runtime_link_t *link, uint8_t context,
                            const uint8_t *data, size_t data_length,
                            void *opaque) {
    observation_t *observation = opaque;
    (void)link;
    (void)context;
    (void)data;
    (void)data_length;
    observation->packet_callbacks++;
}

static void accepted(rns_runtime_destination_t *destination,
                     rns_runtime_link_t *link, void *context) {
    observation_t *observation = context;
    assert(destination != NULL && link != NULL);
    observation->accepted_link = link;
}

static void transfer_changed(rns_runtime_resource_transfer_t *transfer,
                             rns_runtime_resource_state_t state,
                             rns_status_t status, size_t sent_parts,
                             size_t total_parts, void *context) {
    observation_t *observation = context;
    assert(transfer != NULL && sent_parts <= total_parts);
    observation->transfer_callbacks++;
    observation->transfer_state = state;
    observation->transfer_status = status;
}

static void fixture_init(fixture_t *fixture, rns_identity *responder_identity,
                         uint8_t destination_hash[16]) {
    memset(fixture, 0, sizeof *fixture);
    fixture->receiver.accept_resources = true;
    uint16_t initiator_port = reserve_udp_port();
    uint16_t responder_port = reserve_udp_port();
    assert(initiator_port != responder_port);
    rns_config_t initiator_config, responder_config;
    configure_udp(&initiator_config, "resource initiator", initiator_port,
                  responder_port);
    configure_udp(&responder_config, "resource responder", responder_port,
                  initiator_port);
    assert(rns_runtime_create(&fixture->initiator, &initiator_config, NULL) ==
           RNS_OK);
    assert(rns_runtime_create(&fixture->responder, &responder_config, NULL) ==
           RNS_OK);
    assert(rns_identity_generate(responder_identity));
    static const char *const aspects[] = {"delivery"};
    assert(rns_destination_hash(responder_identity, "lxmf", aspects, 1U,
                                destination_hash));
    rns_runtime_link_options_t responder_options = {
        .timeout_seconds = 1.0,
        .packet_callback = packet_received,
        .resource_accept_callback = accept_resource,
        .resource_receive_callback = receive_resource,
        .max_incoming_resource_size = sizeof fixture->receiver.received_data,
        .callback_context = &fixture->receiver};
    assert(rns_runtime_register_link_destination(
               fixture->responder, destination_hash, responder_identity,
               &responder_options, accepted, &fixture->receiver,
               &fixture->destination) == RNS_OK);
    assert(rns_runtime_announce(fixture->responder, responder_identity, "lxmf",
                                aspects, 1U, NULL, 0U) == RNS_OK);
    size_t processed = 0U;
    rns_path_entry path;
    for (size_t i = 0U;
         i < 1000U && rns_runtime_path_lookup(fixture->initiator,
                                              destination_hash, &path) != RNS_OK;
         ++i)
        assert(rns_runtime_poll(fixture->initiator, 8U, &processed) == RNS_OK);
    assert(rns_runtime_path_lookup(fixture->initiator, destination_hash,
                                   &path) == RNS_OK);
    rns_runtime_link_options_t initiator_options = {.timeout_seconds = 1.0};
    assert(rns_runtime_link_open(fixture->initiator, destination_hash,
                                 responder_identity, &initiator_options,
                                 &fixture->outbound) == RNS_OK);
    for (size_t i = 0U; i < 1000U &&
         (rns_runtime_link_state(fixture->outbound) != RNS_LINK_ACTIVE ||
          fixture->receiver.accepted_link == NULL ||
          rns_runtime_link_state(fixture->receiver.accepted_link) !=
              RNS_LINK_ACTIVE); ++i) {
        assert(rns_runtime_poll(fixture->responder, 8U, &processed) == RNS_OK);
        assert(rns_runtime_poll(fixture->initiator, 8U, &processed) == RNS_OK);
    }
    assert(rns_runtime_link_state(fixture->outbound) == RNS_LINK_ACTIVE);
    assert(fixture->receiver.accepted_link != NULL);
}

static void fixture_poll(fixture_t *fixture) {
    size_t processed = 0U;
    assert(rns_runtime_poll(fixture->responder, 8U, &processed) == RNS_OK);
    assert(rns_runtime_poll(fixture->initiator, 8U, &processed) == RNS_OK);
}

static void fixture_destroy(fixture_t *fixture) {
    if (fixture->outbound != NULL)
        rns_runtime_link_destroy(fixture->outbound);
    fixture->outbound = NULL;
    rns_runtime_destination_destroy(fixture->destination);
    rns_runtime_destroy(fixture->initiator);
    rns_runtime_destroy(fixture->responder);
}

static void test_success_reject_cancel_and_malformed(void) {
    fixture_t fixture;
    rns_identity identity;
    uint8_t destination_hash[16];
    fixture_init(&fixture, &identity, destination_hash);
    uint8_t message[900];
    for (size_t i = 0U; i < sizeof message; ++i)
        message[i] = (uint8_t)(i * 29U + 7U);
    rns_runtime_resource_options_t options = {
        .timeout_seconds = 1.0,
        .callback = transfer_changed,
        .callback_context = &fixture.sender};
    rns_runtime_resource_transfer_t *transfer = NULL;
    assert(rns_runtime_link_send_resource(fixture.outbound, message,
                                           sizeof message, &options,
                                           &transfer) == RNS_OK);
    for (size_t i = 0U; i < 1000U &&
         rns_runtime_resource_transfer_state(transfer) !=
             RNS_RUNTIME_RESOURCE_COMPLETE; ++i)
        fixture_poll(&fixture);
    assert(rns_runtime_resource_transfer_state(transfer) ==
           RNS_RUNTIME_RESOURCE_COMPLETE);
    assert(fixture.sender.transfer_callbacks == 2U);
    assert(fixture.sender.transfer_status == RNS_OK);
    assert(fixture.receiver.received == 1U);
    assert(fixture.receiver.received_length == sizeof message);
    assert(memcmp(fixture.receiver.received_data, message, sizeof message) == 0);
    assert(rns_runtime_resource_transfer_sent_parts(transfer) >=
           rns_runtime_resource_transfer_total_parts(transfer));
    assert(rns_runtime_resource_transfer_hash(transfer) != NULL);
    rns_runtime_resource_transfer_destroy(transfer);

    fixture.receiver.accept_resources = false;
    fixture.sender.transfer_callbacks = 0U;
    assert(rns_runtime_link_send_resource(fixture.outbound, message, 32U,
                                           &options, &transfer) == RNS_OK);
    for (size_t i = 0U; i < 1000U &&
         rns_runtime_resource_transfer_state(transfer) ==
             RNS_RUNTIME_RESOURCE_ADVERTISED; ++i)
        fixture_poll(&fixture);
    assert(rns_runtime_resource_transfer_state(transfer) ==
           RNS_RUNTIME_RESOURCE_REJECTED);
    assert(fixture.sender.transfer_callbacks == 1U);
    rns_runtime_resource_transfer_destroy(transfer);

    fixture.receiver.accept_resources = true;
    fixture.sender.transfer_callbacks = 0U;
    assert(rns_runtime_link_send_resource(fixture.outbound, message, 64U,
                                           &options, &transfer) == RNS_OK);
    rns_runtime_resource_transfer_cancel(transfer);
    assert(rns_runtime_resource_transfer_state(transfer) ==
           RNS_RUNTIME_RESOURCE_CANCELLED);
    assert(fixture.sender.transfer_callbacks == 1U);
    for (size_t i = 0U; i < 20U; ++i) fixture_poll(&fixture);
    assert(fixture.receiver.receive_failures == 1U);
    rns_runtime_resource_transfer_destroy(transfer);

    static const uint8_t malformed[] = {0xc1U, 0xffU};
    assert(rns_runtime_link_send(fixture.outbound,
                                 RNS_LINK_CONTEXT_RESOURCE_ADV, malformed,
                                 sizeof malformed) == RNS_OK);
    assert(rns_runtime_link_send(fixture.receiver.accepted_link,
                                 RNS_LINK_CONTEXT_RESOURCE_REQ, malformed,
                                 sizeof malformed) == RNS_OK);
    for (size_t i = 0U; i < 20U; ++i) fixture_poll(&fixture);
    assert(fixture.receiver.packet_callbacks == 0U);
    assert(rns_runtime_link_state(fixture.outbound) == RNS_LINK_ACTIVE);
    fixture_destroy(&fixture);
}

static void test_timeout_and_teardown(void) {
    fixture_t fixture;
    rns_identity identity;
    uint8_t destination_hash[16];
    fixture_init(&fixture, &identity, destination_hash);
    static const uint8_t message[] = "timeout resource";
    rns_runtime_resource_options_t options = {
        .timeout_seconds = 0.002,
        .callback = transfer_changed,
        .callback_context = &fixture.sender};
    rns_runtime_resource_transfer_t *transfer = NULL;
    assert(rns_runtime_link_send_resource(fixture.outbound, message,
                                           sizeof message, &options,
                                           &transfer) == RNS_OK);
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 5000000L};
    assert(nanosleep(&pause, NULL) == 0);
    size_t processed = 0U;
    assert(rns_runtime_poll(fixture.initiator, 8U, &processed) == RNS_OK);
    assert(rns_runtime_resource_transfer_state(transfer) ==
           RNS_RUNTIME_RESOURCE_FAILED);
    assert(fixture.sender.transfer_status == RNS_ERROR_TIMEOUT);
    rns_runtime_resource_transfer_destroy(transfer);

    options.timeout_seconds = 1.0;
    fixture.sender.transfer_callbacks = 0U;
    assert(rns_runtime_link_send_resource(fixture.outbound, message,
                                           sizeof message, &options,
                                           &transfer) == RNS_OK);
    rns_runtime_link_destroy(fixture.outbound);
    fixture.outbound = NULL;
    assert(rns_runtime_resource_transfer_state(transfer) ==
           RNS_RUNTIME_RESOURCE_FAILED);
    assert(fixture.sender.transfer_callbacks == 1U);
    assert(fixture.sender.transfer_status == RNS_ERROR_INVALID_STATE);
    rns_runtime_resource_transfer_destroy(transfer);
    fixture_destroy(&fixture);
}

int main(void) {
    test_success_reject_cancel_and_malformed();
    test_timeout_and_teardown();
    return 0;
}
