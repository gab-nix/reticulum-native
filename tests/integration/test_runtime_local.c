#include "reticulum/config.h"
#include "reticulum/destination.h"
#include "reticulum/hal.h"
#include "reticulum/packet.h"
#include "reticulum/runtime.h"
#include "reticulum/tcp.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>

typedef struct delivery {
    uint8_t expected[16];
    size_t calls;
} delivery_t;

static rns_status_t reserve_port(uint16_t *port) {
    rns_tcp_endpoint_t *listener = NULL;
    rns_udp_address_t address;
    rns_status_t status = rns_tcp_endpoint_create(
        &listener, RNS_UDP_IPV4, 1024U);
    if (status == RNS_OK)
        status = rns_tcp_listen(listener, "127.0.0.1", 0U, 4);
    if (status == RNS_OK)
        status = rns_tcp_local_address(listener, &address);
    if (status == RNS_OK) *port = address.port;
    rns_tcp_endpoint_destroy(listener);
    return status;
}

static void configure_shared(rns_config_t *config, uint16_t port) {
    rns_config_init(config);
    config->share_instance = true;
    config->share_instance_configured = true;
    config->shared_instance_type = RNS_CONFIG_SHARED_INSTANCE_TCP;
    config->shared_instance_port = port;
    config->instance_data_port = port;
}

static void receive_packet(rns_runtime_t *runtime, const uint8_t *packet,
                           size_t packet_length, const rns_node_result *result,
                           void *context) {
    delivery_t *delivery = context;
    rns_packet decoded;
    (void)runtime;
    assert(result != NULL);
    assert(rns_packet_decode(&decoded, packet, packet_length));
    if (decoded.packet_type != 0U) return;
    assert(memcmp(decoded.destination_hash, delivery->expected, 16U) == 0);
    delivery->calls++;
}

static uint64_t deadline_ms(void) {
    uint64_t now = 0U;
    assert(rns_hal_monotonic_ms(&now) == RNS_OK);
    return now + 3000U;
}

static bool before(uint64_t deadline) {
    uint64_t now = 0U;
    assert(rns_hal_monotonic_ms(&now) == RNS_OK);
    return now < deadline;
}

static void poll_runtime(rns_runtime_t *runtime) {
    size_t processed = 0U;
    rns_status_t status = rns_runtime_poll(runtime, 8U, &processed);
    assert(status == RNS_OK || status == RNS_ERROR_IO);
}

int main(void) {
    uint16_t port = 0U;
    assert(reserve_port(&port) == RNS_OK && port != 0U);
    rns_config_t server_config, client_config;
    configure_shared(&server_config, port);
    configure_shared(&client_config, port);

    delivery_t delivery = {{0U}, 0U};
    rns_runtime_options_t receiver_options = {
        .packet_callback = receive_packet,
        .callback_context = &delivery};
    rns_runtime_t *server = NULL, *client = NULL, *receiver = NULL;
    assert(rns_runtime_create(&server, &server_config, NULL) == RNS_OK);
    assert(rns_runtime_create(&client, &client_config, NULL) == RNS_OK);
    assert(rns_runtime_create(&receiver, &client_config, &receiver_options) ==
           RNS_OK);
    assert(rns_runtime_interface_count(server) == 1U);
    assert(rns_runtime_interface_count(client) == 1U);

    rns_runtime_interface_info_t server_info, client_info;
    uint64_t deadline = deadline_ms();
    while (before(deadline)) {
        poll_runtime(server);
        poll_runtime(client);
        poll_runtime(receiver);
        assert(rns_runtime_interface_info(server, 0U, &server_info) == RNS_OK);
        assert(rns_runtime_interface_info(client, 0U, &client_info) == RNS_OK);
        if (server_info.connections_established == 2U &&
            client_info.state == RNS_RUNTIME_INTERFACE_UP)
            break;
    }
    assert(server_info.type == RNS_CONFIG_TCP_SERVER);
    assert(client_info.type == RNS_CONFIG_TCP_CLIENT);
    assert(client_info.state == RNS_RUNTIME_INTERFACE_UP);

    rns_identity receiver_identity;
    static const char *const aspects[] = {"delivery"};
    assert(rns_identity_generate(&receiver_identity));
    assert(rns_destination_hash(&receiver_identity, "lxmf", aspects, 1U,
                                delivery.expected));
    assert(rns_runtime_register_destination(receiver, delivery.expected) ==
           RNS_OK);
    assert(rns_runtime_announce(receiver, &receiver_identity, "lxmf", aspects,
                                1U, NULL, 0U) == RNS_OK);
    deadline = deadline_ms();
    while (before(deadline)) {
        rns_path_entry path;
        poll_runtime(receiver);
        poll_runtime(server);
        poll_runtime(client);
        if (rns_runtime_path_lookup(client, delivery.expected, &path) == RNS_OK)
            break;
    }
    assert(rns_runtime_path_lookup(client, delivery.expected,
                                   &(rns_path_entry){0}) == RNS_OK);
    static const uint8_t body[] = "local runtime packet";
    rns_packet packet = {0};
    memcpy(packet.destination_hash, delivery.expected, 16U);
    packet.data = body;
    packet.data_length = sizeof(body) - 1U;
    uint8_t raw[RNS_MTU];
    size_t raw_length = 0U;
    assert(rns_packet_encode(&packet, raw, sizeof(raw), &raw_length));
    assert(rns_runtime_send_routed(client, raw, raw_length) == RNS_OK);
    deadline = deadline_ms();
    while (delivery.calls == 0U && before(deadline)) {
        poll_runtime(client);
        poll_runtime(server);
        poll_runtime(receiver);
    }
    assert(delivery.calls == 1U);
    assert(rns_runtime_interface_info(server, 0U, &server_info) == RNS_OK);
    assert(server_info.packets_received >= 2U);
    assert(rns_runtime_interface_info(client, 0U, &client_info) == RNS_OK);
    assert(client_info.packets_sent == 1U);

    rns_runtime_destroy(receiver);
    rns_runtime_destroy(client);
    rns_runtime_destroy(server);
    return 0;
}
