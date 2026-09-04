#define _POSIX_C_SOURCE 200809L

#include "reticulum/destination.h"
#include "reticulum/packet.h"
#include "reticulum/runtime.h"
#include "reticulum/udp.h"

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct observation {
    rns_runtime_link_t *accepted_link;
    bool accept_resources;
    size_t accepted;
    size_t received;
    size_t receive_failures;
    rns_status_t receive_status;
    size_t packet_callbacks;
    uint8_t *received_data;
    size_t received_capacity;
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

static size_t find_last_key_before(const uint8_t *data, size_t limit,
                                   uint8_t key) {
    assert(limit >= 2U);
    for (size_t i = limit - 1U; i > 0U; --i)
        if (data[i - 1U] == 0xa1U && data[i] == key) return i;
    assert(false);
    return 0U;
}

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
    assert(advertisement != NULL);
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
        observation->receive_status = status;
        assert(data == NULL && data_length == 0U);
        return;
    }
    assert(data != NULL && data_length <= observation->received_capacity);
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
                         uint8_t destination_hash[16], size_t receive_capacity) {
    memset(fixture, 0, sizeof *fixture);
    fixture->receiver.accept_resources = true;
    fixture->receiver.received_data = malloc(receive_capacity);
    assert(fixture->receiver.received_data != NULL);
    fixture->receiver.received_capacity = receive_capacity;
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
        .max_incoming_resource_size = receive_capacity,
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
    free(fixture->receiver.received_data);
}

static void test_success_reject_cancel_and_malformed(void) {
    fixture_t fixture;
    rns_identity identity;
    uint8_t destination_hash[16];
    fixture_init(&fixture, &identity, destination_hash, 2048U);
    rns_link synthetic_link = {0};
    synthetic_link.state = RNS_LINK_ACTIVE;
    synthetic_link.mode = RNS_LINK_MODE_AES256_CBC;
    synthetic_link.mtu = 500U;
    memset(synthetic_link.derived_key, 0x71,
           sizeof synthetic_link.derived_key);
    static const uint8_t synthetic_data[] = "out-of-sequence segment";
    rns_resource_sender_options_t synthetic_options = {.auto_compress = false};
    rns_resource_sender_t *synthetic = NULL;
    assert(rns_resource_sender_create(&synthetic, &synthetic_link,
                                      synthetic_data,
                                      sizeof synthetic_data,
                                      &synthetic_options) == RNS_OK);
    uint8_t malformed_segment[RNS_MTU];
    size_t malformed_segment_length = 0U;
    assert(rns_resource_sender_advertisement(
               synthetic, malformed_segment, sizeof malformed_segment,
               &malformed_segment_length) == RNS_OK);
    rns_resource_advertisement_t parsed_segment;
    assert(rns_resource_advertisement_parse(
               malformed_segment, malformed_segment_length,
               &parsed_segment) == RNS_OK);
    size_t map_offset =
        (size_t)(parsed_segment.hashmap - malformed_segment);
    size_t index_key = find_last_key_before(malformed_segment, map_offset, 'i');
    size_t total_key = find_last_key_before(malformed_segment, map_offset, 'l');
    size_t flags_key = find_last_key_before(malformed_segment, map_offset, 'f');
    malformed_segment[index_key + 1U] = 2U;
    malformed_segment[total_key + 1U] = 2U;
    malformed_segment[flags_key + 1U] |= RNS_RESOURCE_FLAG_SPLIT;
    assert(rns_runtime_link_send(fixture.outbound,
                                 RNS_LINK_CONTEXT_RESOURCE_ADV,
                                 malformed_segment,
                                 malformed_segment_length) == RNS_OK);
    for (size_t i = 0U; i < 20U; ++i) fixture_poll(&fixture);
    assert(fixture.receiver.accepted == 0U);
    rns_resource_sender_destroy(synthetic);
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
    /* A different valid advertisement received while this transfer is active
     * must not replace or orphan the original receiver. */
    for (size_t i = 0U; i < 100U && fixture.receiver.accepted == 0U; ++i) {
        size_t processed = 0U;
        assert(rns_runtime_poll(fixture.responder, 1U, &processed) == RNS_OK);
    }
    assert(fixture.receiver.accepted == 1U);
    rns_link unrelated_link = {0};
    unrelated_link.state = RNS_LINK_ACTIVE;
    unrelated_link.mtu = 500U;
    memset(unrelated_link.derived_key, 0x5a,
           sizeof unrelated_link.derived_key);
    static const uint8_t unrelated_data[] = "concurrent resource";
    rns_resource_sender_t *unrelated = NULL;
    assert(rns_resource_sender_create(&unrelated, &unrelated_link,
                                      unrelated_data,
                                      sizeof unrelated_data, NULL) == RNS_OK);
    uint8_t unrelated_advertisement[500U];
    size_t unrelated_advertisement_length = 0U;
    assert(rns_resource_sender_advertisement(
               unrelated, unrelated_advertisement,
               sizeof unrelated_advertisement,
               &unrelated_advertisement_length) == RNS_OK);
    assert(rns_runtime_link_send(fixture.outbound,
                                 RNS_LINK_CONTEXT_RESOURCE_ADV,
                                 unrelated_advertisement,
                                 unrelated_advertisement_length) == RNS_OK);
    for (size_t i = 0U; i < 20U; ++i) fixture_poll(&fixture);
    assert(fixture.receiver.accepted == 1U);
    rns_resource_sender_destroy(unrelated);
    for (size_t i = 0U; i < 1000U &&
         rns_runtime_resource_transfer_state(transfer) !=
             RNS_RUNTIME_RESOURCE_COMPLETE; ++i)
        fixture_poll(&fixture);
    assert(rns_runtime_resource_transfer_state(transfer) ==
           RNS_RUNTIME_RESOURCE_COMPLETE);
    /* One bounded request sends this three-part Resource, followed by its
     * terminal completion callback; duplicate requests are not progress. */
    assert(fixture.sender.transfer_callbacks == 2U);
    assert(fixture.sender.transfer_status == RNS_OK);
    assert(fixture.receiver.received == 1U);
    assert(fixture.receiver.received_length == sizeof message);
    assert(memcmp(fixture.receiver.received_data, message, sizeof message) == 0);
    assert(rns_runtime_resource_transfer_sent_parts(transfer) >=
           rns_runtime_resource_transfer_total_parts(transfer));
    assert(rns_runtime_resource_transfer_hash(transfer) != NULL);
    rns_runtime_resource_transfer_destroy(transfer);

    /* Policy acceptance followed by an internal size rejection is terminal
     * and observable by both applications. */
    uint8_t oversized[3000];
    memset(oversized, 0x33, sizeof oversized);
    fixture.sender.transfer_callbacks = 0U;
    assert(rns_runtime_link_send_resource(fixture.outbound, oversized,
                                           sizeof oversized, &options,
                                           &transfer) == RNS_OK);
    for (size_t i = 0U; i < 1000U &&
         rns_runtime_resource_transfer_state(transfer) ==
             RNS_RUNTIME_RESOURCE_ADVERTISED; ++i)
        fixture_poll(&fixture);
    assert(rns_runtime_resource_transfer_state(transfer) ==
           RNS_RUNTIME_RESOURCE_REJECTED);
    assert(fixture.receiver.receive_failures == 1U);
    assert(fixture.receiver.receive_status == RNS_ERROR_OVERFLOW);
    assert(fixture.sender.transfer_callbacks == 1U);
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
    assert(fixture.receiver.receive_failures == 2U);
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
    fixture_init(&fixture, &identity, destination_hash, 2048U);
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

static void test_multisegment_runtime_transfer(void) {
    fixture_t fixture;
    rns_identity identity;
    uint8_t destination_hash[16];
    size_t message_length = RNS_RESOURCE_SINGLE_SEGMENT_MAX_SIZE + 257U;
    fixture_init(&fixture, &identity, destination_hash, message_length);
    uint8_t *message = malloc(message_length);
    assert(message != NULL);
    for (size_t i = 0U; i < message_length; ++i)
        message[i] = (uint8_t)(i * 37U + i / 251U);
    rns_runtime_resource_options_t options = {
        .auto_compress = false,
        .timeout_seconds = 10.0,
        .callback = transfer_changed,
        .callback_context = &fixture.sender};
    rns_runtime_resource_transfer_t *transfer = NULL;
    assert(rns_runtime_link_send_resource(fixture.outbound, message,
                                           message_length, &options,
                                           &transfer) == RNS_OK);
    for (size_t i = 0U; i < 50000U &&
         rns_runtime_resource_transfer_state(transfer) !=
             RNS_RUNTIME_RESOURCE_COMPLETE; ++i)
        fixture_poll(&fixture);
    assert(rns_runtime_resource_transfer_state(transfer) ==
           RNS_RUNTIME_RESOURCE_COMPLETE);
    assert(fixture.receiver.accepted == 1U);
    assert(fixture.receiver.received == 1U);
    assert(fixture.receiver.received_length == message_length);
    assert(memcmp(fixture.receiver.received_data, message, message_length) == 0);
    rns_runtime_resource_transfer_destroy(transfer);
    free(message);
    fixture_destroy(&fixture);
}

int main(void) {
    test_success_reject_cancel_and_malformed();
    test_timeout_and_teardown();
    test_multisegment_runtime_transfer();
    return 0;
}
