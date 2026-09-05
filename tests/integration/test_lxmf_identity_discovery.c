#include "reticulum/lxmf_router.h"
#include "reticulum/udp.h"
#include "reticulum/hal.h"
#include "reticulum/destination.h"
#include "reticulum/packet.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint64_t fake_now(void *context) { return *(uint64_t *)context; }
static const rns_identity *unknown(void *context, const uint8_t destination[16]) {
    (void)destination; return *(const rns_identity **)context;
}
static rns_status_t observed(const uint8_t *raw, size_t length,
    const rns_udp_address_t *source, void *context) {
    (void)source;
    rns_packet packet;
    assert(rns_packet_decode(&packet, raw, length));
    assert(packet.transport_type == 0u && packet.destination_type == 2u);
    assert(packet.packet_type == 0u && packet.context == 0u);
    uint8_t expected[16]; const char *aspects[] = {"path", "request"};
    assert(rns_destination_hash(NULL, "rnstransport", aspects, 2u, expected));
    assert(memcmp(packet.destination_hash, expected, 16u) == 0);
    assert(packet.data_length == 32u && packet.data[0] == 0x21u);
    (*(size_t *)context)++;
    return RNS_OK;
}
static void receive_count(rns_udp_endpoint_t *endpoint, size_t expected) {
    size_t count = 0;
    uint64_t start, now;
    assert(rns_hal_monotonic_ms(&start) == RNS_OK);
    do {
        size_t received;
        assert(rns_udp_poll(endpoint, 8u, observed, &count, &received) == RNS_OK);
        assert(rns_hal_monotonic_ms(&now) == RNS_OK);
    } while (count < expected && now - start < 1000u);
    assert(count == expected);
}
int main(void) {
    rns_udp_endpoint_t *peers[2] = {0};
    rns_config_t config; rns_config_init(&config); config.interface_count = 2u;
    for (size_t i = 0; i < 2u; ++i) {
        rns_udp_address_t address;
        assert(rns_udp_endpoint_create(&peers[i], RNS_UDP_IPV4) == RNS_OK);
        assert(rns_udp_bind(peers[i], "127.0.0.1", 0u) == RNS_OK);
        assert(rns_udp_local_address(peers[i], &address) == RNS_OK);
        config.interfaces[i].enabled = true;
        config.interfaces[i].type_set = true;
        config.interfaces[i].type = RNS_CONFIG_UDP;
        strcpy(config.interfaces[i].name, i == 0 ? "first" : "second");
        strcpy(config.interfaces[i].listen_ip, "127.0.0.1");
        strcpy(config.interfaces[i].forward_ip, "127.0.0.1");
        config.interfaces[i].forward_port = address.port;
    }
    rns_runtime_t *runtime = NULL;
    assert(rns_runtime_create(&runtime, &config, NULL) == RNS_OK);
    rns_identity identity; assert(rns_identity_generate(&identity));
    char path[] = "/tmp/lxmf-discovery-XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0 && close(fd) == 0 && unlink(path) == 0);
    lxmf_store_t store = {0}; assert(lxmf_store_open(&store, path) == LXMF_OK);
    lxmf_store_message_t message = {0};
    message.message_id[0] = 1u; message.destination[0] = 0x21u;
    message.status = LXMF_DELIVERY_QUEUED;
    message.delivery.desired_method = LXMF_DELIVERY_METHOD_DIRECT;
    message.timestamp = 1.0;
    bool inserted;
    assert(lxmf_store_put(&store, &message, &inserted) == LXMF_OK && inserted);
    uint64_t now = 1000u;
    const rns_identity *known = NULL;
    lxmf_router_config_t options = {.identity = &identity, .store = &store,
        .runtime = runtime, .resolve_identity = unknown,
        .resolve_context = &known,
        .monotonic_clock = fake_now, .monotonic_clock_context = &now};
    lxmf_router_t router; assert(lxmf_router_init(&router, &options) == LXMF_OK);
    assert(lxmf_router_send_message(&router, message.message_id) == LXMF_ERR_PENDING);
    for (size_t i = 0; i < 2u; ++i) receive_count(peers[i], 1u);
    assert(lxmf_router_send_message(&router, message.message_id) == LXMF_ERR_PENDING);
    for (size_t i = 0; i < 2u; ++i) receive_count(peers[i], 0u);
    now += 15000u;
    assert(lxmf_router_send_message(&router, message.message_id) == LXMF_ERR_PENDING);
    for (size_t i = 0; i < 2u; ++i) receive_count(peers[i], 1u);
    known = &identity;
    now += 1u; /* Identity arrival bypasses the remaining discovery interval. */
    assert(lxmf_router_send_message(&router, message.message_id) == LXMF_ERR_PENDING);
    uint8_t content[1];
    lxmf_store_message_t resumed;
    assert(lxmf_store_read(&store, message.message_id, &resumed, content, sizeof content) == LXMF_OK);
    assert(resumed.delivery.queue_reason == LXMF_QUEUE_REASON_PATH);
    lxmf_router_destroy(&router); lxmf_store_close(&store);
    rns_runtime_destroy(runtime);
    for (size_t i = 0; i < 2u; ++i) rns_udp_endpoint_destroy(peers[i]);
    assert(unlink(path) == 0);
    return 0;
}
