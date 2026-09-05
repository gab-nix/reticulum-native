#include "reticulum/runtime.h"
#include "reticulum/announce.h"
#include "reticulum/destination.h"
#include "reticulum/packet.h"
#include "reticulum/udp.h"
#include "reticulum/hal.h"
#include <assert.h>
#include <string.h>

typedef struct { size_t count; bool response; uint8_t hash[16]; } observed_t;
static void accepted(rns_runtime_destination_t *destination,
    rns_runtime_link_t *link, void *context) {
    (void)destination; (void)link; (void)context;
}
static rns_status_t receive(const uint8_t *raw, size_t length,
    const rns_udp_address_t *source, void *context) {
    (void)source;
    observed_t *seen = context;
    rns_packet packet; rns_announce announce;
    assert(rns_packet_decode(&packet, raw, length));
    assert(packet.packet_type == 1u && packet.context == (seen->response ? 0x0bu : 0u));
    assert(memcmp(packet.destination_hash, seen->hash, 16u) == 0);
    assert(rns_announce_verify(seen->hash, packet.data, packet.data_length, packet.context_flag));
    assert(rns_announce_parse(&announce, packet.data, packet.data_length, packet.context_flag));
    assert(announce.has_ratchet && announce.ratchet[0] == 0x41u);
    assert(announce.app_data_length == 4u && memcmp(announce.app_data, "test", 4u) == 0);
    seen->count++;
    return RNS_OK;
}
static uint16_t reserve_port(void) {
    rns_udp_endpoint_t *endpoint = NULL; rns_udp_address_t address;
    assert(rns_udp_endpoint_create(&endpoint, RNS_UDP_IPV4) == RNS_OK);
    assert(rns_udp_bind(endpoint, "127.0.0.1", 0u) == RNS_OK);
    assert(rns_udp_local_address(endpoint, &address) == RNS_OK);
    rns_udp_endpoint_destroy(endpoint); return address.port;
}
static void pump(rns_runtime_t *runtime, rns_udp_endpoint_t *peer,
    observed_t *seen, size_t expected) {
    uint64_t start, now;
    assert(rns_hal_monotonic_ms(&start) == RNS_OK);
    do {
        size_t processed;
        assert(rns_runtime_poll(runtime, 8u, &processed) == RNS_OK);
        assert(rns_udp_poll(peer, 8u, receive, seen, &processed) == RNS_OK);
        assert(rns_hal_monotonic_ms(&now) == RNS_OK);
    } while (now - start < (expected == 0u ? 20u : 1000u) && seen->count < (expected == 0u ? 1u : expected));
    assert(seen->count == expected);
}
int main(void) {
    rns_udp_endpoint_t *peers[2] = {0};
    rns_config_t config; rns_config_init(&config); config.interface_count = 2u;
    for (size_t i = 0; i < 2u; ++i) {
        rns_udp_address_t address;
        assert(rns_udp_endpoint_create(&peers[i], RNS_UDP_IPV4) == RNS_OK);
        assert(rns_udp_bind(peers[i], "127.0.0.1", 0u) == RNS_OK);
        assert(rns_udp_local_address(peers[i], &address) == RNS_OK);
        config.interfaces[i].enabled = true; config.interfaces[i].type_set = true;
        config.interfaces[i].type = RNS_CONFIG_UDP;
        strcpy(config.interfaces[i].name, i == 0u ? "first" : "second");
        strcpy(config.interfaces[i].listen_ip, "127.0.0.1");
        strcpy(config.interfaces[i].forward_ip, "127.0.0.1");
        config.interfaces[i].listen_port = reserve_port();
        config.interfaces[i].forward_port = address.port;
    }
    rns_runtime_t *runtime = NULL;
    assert(rns_runtime_create(&runtime, &config, NULL) == RNS_OK);
    rns_identity identity; assert(rns_identity_generate(&identity));
    const char *aspects[] = {"delivery"};
    observed_t seen = {0};
    assert(rns_destination_hash(&identity, "lxmf", aspects, 1u, seen.hash));
    assert(rns_runtime_register_destination(runtime, seen.hash) == RNS_OK);
    uint8_t ratchet[32] = {0x41u};
    assert(rns_runtime_announce_with_ratchet(runtime, &identity, "lxmf", aspects,
        1u, ratchet, (const uint8_t *)"test", 4u) == RNS_OK);
    for (size_t i = 0; i < 2u; ++i) { seen.count = 0; pump(runtime, peers[i], &seen, 1u); }
    /* Startup announce was discarded: only an explicit request can find us. */
    rns_udp_address_t target;
    assert(rns_udp_resolve("127.0.0.1", config.interfaces[0].listen_port,
        RNS_UDP_IPV4, &target) == RNS_OK);
    const char *path_aspects[] = {"path", "request"};
    rns_packet request = {.destination_type = 2u};
    assert(rns_destination_hash(NULL, "rnstransport", path_aspects, 2u, request.destination_hash));
    uint8_t tag[16] = {1u}, body[48], raw[RNS_MTU];
    size_t body_length, raw_length;
    assert(rns_path_request_build(seen.hash, NULL, tag, sizeof tag, body, sizeof body, &body_length));
    request.data = body; request.data_length = body_length;
    assert(rns_packet_encode(&request, raw, sizeof raw, &raw_length));
    seen.response = true; seen.count = 0;
    assert(rns_udp_send_to(peers[0], &target, raw, raw_length) == RNS_OK);
    pump(runtime, peers[0], &seen, 1u);
    seen.count = 0; pump(runtime, peers[1], &seen, 0u); /* Ingress interface only. */
    assert(rns_udp_send_to(peers[0], &target, raw, raw_length) == RNS_OK);
    pump(runtime, peers[0], &seen, 0u); /* Exact duplicate suppressed. */
    tag[0]++;
    assert(rns_path_request_build(seen.hash, NULL, tag, sizeof tag, body, sizeof body, &body_length));
    assert(rns_packet_encode(&request, raw, sizeof raw, &raw_length));
    assert(rns_udp_send_to(peers[0], &target, raw, raw_length) == RNS_OK);
    pump(runtime, peers[0], &seen, 0u); /* A new tag cannot bypass cooldown. */
    assert(rns_hal_sleep_ms(1100u) == RNS_OK);
    tag[0]++;
    assert(rns_path_request_build(seen.hash, NULL, tag, sizeof tag, body, sizeof body, &body_length));
    assert(rns_packet_encode(&request, raw, sizeof raw, &raw_length));
    assert(rns_udp_send_to(peers[0], &target, raw, raw_length) == RNS_OK);
    pump(runtime, peers[0], &seen, 1u); /* Cooldown eventually permits recovery. */
    rns_runtime_destination_t *registration = NULL;
    assert(rns_runtime_register_link_destination(runtime, seen.hash, &identity,
        NULL, accepted, NULL, &registration) == RNS_OK);
    assert(rns_runtime_unregister_destination(runtime, seen.hash) == RNS_OK);
    assert(rns_hal_sleep_ms(1100u) == RNS_OK);
    seen.count = 0; tag[0]++;
    assert(rns_path_request_build(seen.hash, NULL, tag, sizeof tag, body, sizeof body, &body_length));
    assert(rns_packet_encode(&request, raw, sizeof raw, &raw_length));
    assert(rns_udp_send_to(peers[0], &target, raw, raw_length) == RNS_OK);
    pump(runtime, peers[0], &seen, 1u); /* Opaque registration keeps service alive. */
    rns_runtime_destination_destroy(registration);
    assert(rns_runtime_register_destination(runtime, seen.hash) == RNS_OK);
    assert(rns_hal_sleep_ms(1100u) == RNS_OK);
    seen.count = 0;
    tag[0]++;
    assert(rns_path_request_build(seen.hash, NULL, tag, sizeof tag, body, sizeof body, &body_length));
    assert(rns_packet_encode(&request, raw, sizeof raw, &raw_length));
    assert(rns_udp_send_to(peers[0], &target, raw, raw_length) == RNS_OK);
    pump(runtime, peers[0], &seen, 0u); /* Removed service cannot resurrect its cache. */
    rns_runtime_destroy(runtime);
    for (size_t i = 0; i < 2u; ++i) rns_udp_endpoint_destroy(peers[i]);
    return 0;
}
