#include "reticulum/auto.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct harness {
    double now;
    rns_auto_emit_kind_t kind;
    rns_udp_address_t destination;
    uint32_t interface_index;
    uint8_t emitted[RNS_AUTO_HW_MTU];
    size_t emitted_length;
    size_t emit_count;
    uint8_t received[RNS_AUTO_HW_MTU];
    size_t received_length;
    size_t receive_count;
    rns_status_t emit_status;
} harness_t;

static double fake_clock(void *context) {
    return ((harness_t *)context)->now;
}

static rns_status_t fake_emit(rns_auto_emit_kind_t kind,
                              uint32_t interface_index,
                              const rns_udp_address_t *destination,
                              const uint8_t *data, size_t data_length,
                              void *context) {
    harness_t *harness = context;
    assert(destination != NULL && data != NULL);
    assert(data_length <= sizeof harness->emitted);
    harness->kind = kind;
    harness->destination = *destination;
    harness->interface_index = interface_index;
    memcpy(harness->emitted, data, data_length);
    harness->emitted_length = data_length;
    harness->emit_count++;
    return harness->emit_status;
}

static rns_status_t fake_receive(const uint8_t *packet, size_t packet_length,
                                 const rns_udp_address_t *source,
                                 uint32_t interface_index, void *context) {
    harness_t *harness = context;
    assert(source != NULL && source->family == RNS_UDP_IPV6);
    assert(interface_index != 0u);
    assert(packet_length <= sizeof harness->received);
    memcpy(harness->received, packet, packet_length);
    harness->received_length = packet_length;
    harness->receive_count++;
    return RNS_OK;
}

static rns_auto_endpoint_t *make_endpoint(harness_t *harness,
                                          size_t peer_capacity) {
    rns_auto_options_t options;
    rns_auto_options_init(&options);
    options.clock = fake_clock;
    options.emit = fake_emit;
    options.receive = fake_receive;
    options.callback_context = harness;
    options.peer_capacity = peer_capacity;
    rns_auto_endpoint_t *endpoint = NULL;
    assert(rns_auto_endpoint_create(&endpoint, &options) == RNS_OK);
    return endpoint;
}

static rns_udp_address_t link_local(uint16_t suffix, uint32_t scope_id) {
    rns_udp_address_t address = {0};
    address.family = RNS_UDP_IPV6;
    address.address[0] = 0xfeu;
    address.address[1] = 0x80u;
    address.address[14] = (uint8_t)(suffix >> 8u);
    address.address[15] = (uint8_t)suffix;
    address.scope_id = scope_id;
    return address;
}

static void test_pinned_discovery_vectors(void) {
    static const uint8_t expected_group[16] = {
        0xff, 0x12, 0x00, 0x00, 0xd7, 0x0b, 0xfb, 0x1c,
        0x16, 0xe4, 0x5e, 0x39, 0x48, 0x5e, 0x31, 0xe1};
    static const uint8_t expected_token[32] = {
        0xb8, 0x47, 0x67, 0xee, 0x14, 0x5b, 0x5e, 0x11,
        0xc2, 0x0d, 0xe7, 0x06, 0xb2, 0x14, 0xda, 0xd1,
        0x3f, 0x54, 0xdc, 0x33, 0x71, 0xce, 0x64, 0x84,
        0x6d, 0xec, 0xc1, 0x2e, 0x4a, 0x90, 0x64, 0x69};
    harness_t harness = {0};
    rns_auto_endpoint_t *endpoint = make_endpoint(&harness, 4u);
    rns_udp_address_t local = link_local(0x1234u, 7u);
    assert(rns_auto_add_local_interface(endpoint, &local, 7u) == RNS_OK);
    rns_udp_address_t group;
    assert(rns_auto_multicast_address(endpoint, &group) == RNS_OK);
    assert(group.family == RNS_UDP_IPV6 && group.port == 29716u);
    assert(memcmp(group.address, expected_group, sizeof expected_group) == 0);
    uint8_t token[32];
    assert(rns_auto_local_token(endpoint, 7u, token) == RNS_OK);
    assert(memcmp(token, expected_token, sizeof token) == 0);
    rns_auto_endpoint_destroy(endpoint);

    rns_auto_options_t options;
    rns_auto_options_init(&options);
    options.clock = fake_clock;
    options.emit = fake_emit;
    options.receive = fake_receive;
    options.callback_context = &harness;
    options.discovery_scope = RNS_AUTO_SCOPE_GLOBAL;
    options.multicast_type = RNS_AUTO_MULTICAST_PERMANENT;
    assert(rns_auto_endpoint_create(&endpoint, &options) == RNS_OK);
    assert(rns_auto_multicast_address(endpoint, &group) == RNS_OK);
    assert(group.address[0] == 0xffu && group.address[1] == 0x0eu);
    rns_auto_endpoint_destroy(endpoint);
}

static void test_peer_lifecycle_and_raw_carrier(void) {
    harness_t left = {0};
    harness_t right = {0};
    rns_auto_endpoint_t *a = make_endpoint(&left, 2u);
    rns_auto_endpoint_t *b = make_endpoint(&right, 2u);
    rns_udp_address_t address_a = link_local(0x1234u, 3u);
    rns_udp_address_t address_b = link_local(0xabcdu, 4u);
    assert(rns_auto_add_local_interface(a, &address_a, 3u) == RNS_OK);
    assert(rns_auto_add_local_interface(b, &address_b, 4u) == RNS_OK);

    assert(rns_auto_poll(a) == RNS_OK);
    assert(left.kind == RNS_AUTO_EMIT_MULTICAST_DISCOVERY);
    assert(rns_auto_ingest_discovery(b, &address_a, 4u, left.emitted,
                                     left.emitted_length) ==
           RNS_ERROR_INVALID_STATE);
    left.now = right.now = 2.0;
    assert(rns_auto_online(a) && rns_auto_online(b));
    assert(rns_auto_poll(a) == RNS_OK);
    assert(rns_auto_ingest_discovery(b, &address_a, 4u, left.emitted,
                                     left.emitted_length) == RNS_OK);
    assert(rns_auto_poll(b) == RNS_OK);
    assert(rns_auto_ingest_discovery(a, &address_b, 3u, right.emitted,
                                     right.emitted_length) == RNS_OK);
    assert(rns_auto_peer_count(a) == 1u && rns_auto_peer_count(b) == 1u);

    static const uint8_t packet[] = {0x01u, 0x02u, 0x03u, 0x04u};
    size_t sent = 0u;
    assert(rns_auto_send(a, packet, sizeof packet, &sent) == RNS_OK);
    assert(sent == 1u && left.kind == RNS_AUTO_EMIT_DATA);
    assert(left.destination.port == RNS_AUTO_DEFAULT_DATA_PORT);
    assert(rns_auto_ingest_data(b, &address_a, 4u, left.emitted,
                                left.emitted_length) == RNS_OK);
    assert(right.receive_count == 1u &&
           memcmp(right.received, packet, sizeof packet) == 0);
    assert(rns_auto_ingest_data(b, &address_a, 4u, left.emitted,
                                left.emitted_length) == RNS_OK);
    assert(right.receive_count == 1u);
    right.now += 0.76;
    assert(rns_auto_ingest_data(b, &address_a, 4u, left.emitted,
                                left.emitted_length) == RNS_OK);
    assert(right.receive_count == 2u);

    left.now = 7.3;
    size_t previous_emits = left.emit_count;
    assert(rns_auto_poll(a) == RNS_OK);
    assert(left.emit_count >= previous_emits + 2u);
    assert(left.kind == RNS_AUTO_EMIT_MULTICAST_DISCOVERY ||
           left.kind == RNS_AUTO_EMIT_UNICAST_DISCOVERY);
    left.now = 25.1;
    assert(rns_auto_poll(a) == RNS_OK);
    assert(rns_auto_peer_count(a) == 0u);
    assert(rns_auto_send(a, packet, sizeof packet, &sent) ==
           RNS_ERROR_NOT_FOUND);
    rns_auto_endpoint_destroy(a);
    rns_auto_endpoint_destroy(b);
}

static void test_rejection_and_bounds(void) {
    harness_t harness = {0};
    rns_auto_endpoint_t *endpoint = make_endpoint(&harness, 1u);
    rns_udp_address_t local = link_local(1u, 2u);
    rns_udp_address_t peer = link_local(2u, 2u);
    assert(rns_auto_add_local_interface(endpoint, &local, 2u) == RNS_OK);
    rns_udp_address_t global = {0};
    global.family = RNS_UDP_IPV6;
    global.address[0] = 0x20u;
    global.address[1] = 0x01u;
    assert(rns_auto_add_local_interface(endpoint, &global, 3u) ==
           RNS_ERROR_INVALID_ARGUMENT);
    harness.now = 2.0;
    uint8_t invalid[32] = {0};
    assert(rns_auto_ingest_discovery(endpoint, &peer, 2u, invalid,
                                     sizeof invalid) == RNS_ERROR_CRYPTO);
    uint8_t token[32];
    harness_t peer_harness = {0};
    rns_auto_endpoint_t *peer_endpoint = make_endpoint(&peer_harness, 1u);
    assert(rns_auto_add_local_interface(peer_endpoint, &peer, 2u) == RNS_OK);
    peer_harness.now = 2.0;
    assert(rns_auto_local_token(peer_endpoint, 2u, token) == RNS_OK);
    assert(rns_auto_ingest_discovery(endpoint, &peer, 2u, token,
                                     sizeof token) == RNS_OK);
    rns_udp_address_t extra = link_local(3u, 2u);
    rns_auto_endpoint_t *extra_endpoint = make_endpoint(&peer_harness, 1u);
    assert(rns_auto_add_local_interface(extra_endpoint, &extra, 2u) == RNS_OK);
    assert(rns_auto_local_token(extra_endpoint, 2u, token) == RNS_OK);
    assert(rns_auto_ingest_discovery(endpoint, &extra, 2u, token,
                                     sizeof token) == RNS_ERROR_OVERFLOW);
    uint8_t oversized[RNS_AUTO_HW_MTU + 1u] = {0};
    size_t sent = 0u;
    assert(rns_auto_send(endpoint, oversized, sizeof oversized, &sent) ==
           RNS_ERROR_OVERFLOW);
    rns_auto_endpoint_destroy(extra_endpoint);
    rns_auto_endpoint_destroy(peer_endpoint);
    rns_auto_endpoint_destroy(endpoint);
}

int main(void) {
    test_pinned_discovery_vectors();
    test_peer_lifecycle_and_raw_carrier();
    test_rejection_and_bounds();
    return 0;
}
