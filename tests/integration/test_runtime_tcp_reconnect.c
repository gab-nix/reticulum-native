#include "reticulum/config.h"
#include "reticulum/hal.h"
#include "reticulum/runtime.h"
#include "reticulum/tcp.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>

typedef struct fake_clock {
    double now;
} fake_clock_t;

static double read_fake_clock(void *context) {
    return ((const fake_clock_t *)context)->now;
}

static uint64_t deadline_ms(uint64_t interval) {
    uint64_t now = 0U;
    assert(rns_hal_monotonic_ms(&now) == RNS_OK);
    return now + interval;
}

static bool before_deadline(uint64_t deadline) {
    uint64_t now = 0U;
    assert(rns_hal_monotonic_ms(&now) == RNS_OK);
    return now < deadline;
}

static uint16_t reserve_tcp_port(void) {
    rns_tcp_endpoint_t *listener = NULL;
    rns_udp_address_t address;
    assert(rns_tcp_endpoint_create(&listener, RNS_UDP_IPV4, 2048U) == RNS_OK);
    assert(rns_tcp_listen(listener, "127.0.0.1", 0U, 4) == RNS_OK);
    assert(rns_tcp_local_address(listener, &address) == RNS_OK);
    rns_tcp_endpoint_destroy(listener);
    return address.port;
}

static void configure_tcp(rns_config_t *config,
                          rns_config_interface_type_t type, uint16_t port) {
    rns_config_init(config);
    config->interface_count = 1U;
    rns_config_interface_t *interface = &config->interfaces[0];
    (void)strcpy(interface->name, "lifecycle");
    interface->type = type;
    interface->type_set = true;
    interface->enabled = true;
    if (type == RNS_CONFIG_TCP_CLIENT) {
        (void)strcpy(interface->target_host, "127.0.0.1");
        interface->target_port = port;
    } else {
        (void)strcpy(interface->listen_ip, "127.0.0.1");
        interface->listen_port = port;
    }
}

static rns_status_t poll_runtime(rns_runtime_t *runtime) {
    size_t processed = 0U;
    rns_status_t status = rns_runtime_poll(runtime, 8U, &processed);
    assert(status == RNS_OK || status == RNS_ERROR_IO);
    return status;
}

static rns_tcp_endpoint_t *await_runtime_client(rns_runtime_t *runtime,
                                                rns_tcp_endpoint_t *listener) {
    rns_tcp_endpoint_t *peer = NULL;
    uint64_t deadline = deadline_ms(3000U);
    while (peer == NULL && before_deadline(deadline)) {
        (void)poll_runtime(runtime);
        rns_status_t status = rns_tcp_accept(listener, &peer, 2048U);
        assert(status == RNS_OK || status == RNS_ERROR_TIMEOUT);
    }
    assert(peer != NULL);
    rns_runtime_interface_info_t info;
    while (before_deadline(deadline)) {
        (void)poll_runtime(runtime);
        assert(rns_runtime_interface_info(runtime, 0U, &info) == RNS_OK);
        if (info.state == RNS_RUNTIME_INTERFACE_UP) break;
    }
    assert(info.state == RNS_RUNTIME_INTERFACE_UP);
    assert(info.last_error == RNS_OK);
    return peer;
}

static void test_client_reconnect(void) {
    uint16_t port = reserve_tcp_port();
    rns_config_t config;
    configure_tcp(&config, RNS_CONFIG_TCP_CLIENT, port);
    fake_clock_t clock = {0.0};
    rns_runtime_options_t options = {
        .reconnect_clock = read_fake_clock,
        .reconnect_clock_context = &clock,
        .tcp_reconnect_initial_seconds = 1.0,
        .tcp_reconnect_max_seconds = 4.0};
    rns_runtime_t *runtime = NULL;
    assert(rns_runtime_create(&runtime, &config, &options) == RNS_OK);

    rns_runtime_interface_info_t info;
    uint64_t deadline = deadline_ms(3000U);
    do {
        (void)poll_runtime(runtime);
        assert(rns_runtime_interface_info(runtime, 0U, &info) == RNS_OK);
    } while (info.state != RNS_RUNTIME_INTERFACE_DOWN &&
             before_deadline(deadline));
    assert(info.state == RNS_RUNTIME_INTERFACE_DOWN);
    assert(info.last_error == RNS_ERROR_IO);
    assert(info.connection_attempts == 1U);
    assert(info.connections_established == 0U);

    /* A second refused dial doubles the next delay from one to two seconds. */
    clock.now = 1.0;
    deadline = deadline_ms(3000U);
    do {
        (void)poll_runtime(runtime);
        assert(rns_runtime_interface_info(runtime, 0U, &info) == RNS_OK);
    } while ((info.connection_attempts < 2U ||
              info.state != RNS_RUNTIME_INTERFACE_DOWN) &&
             before_deadline(deadline));
    assert(info.connection_attempts == 2U);
    assert(info.state == RNS_RUNTIME_INTERFACE_DOWN);

    rns_tcp_endpoint_t *listener = NULL;
    assert(rns_tcp_endpoint_create(&listener, RNS_UDP_IPV4, 2048U) == RNS_OK);
    assert(rns_tcp_listen(listener, "127.0.0.1", port, 4) == RNS_OK);
    clock.now = 2.0;
    (void)poll_runtime(runtime);
    assert(rns_runtime_interface_info(runtime, 0U, &info) == RNS_OK);
    assert(info.connection_attempts == 2U);
    assert(info.state == RNS_RUNTIME_INTERFACE_DOWN);
    clock.now = 3.0;
    rns_tcp_endpoint_t *first = await_runtime_client(runtime, listener);
    assert(rns_runtime_interface_info(runtime, 0U, &info) == RNS_OK);
    assert(info.connection_attempts == 3U);
    assert(info.connections_established == 1U);

    rns_tcp_endpoint_destroy(first);
    deadline = deadline_ms(3000U);
    do {
        (void)poll_runtime(runtime);
        assert(rns_runtime_interface_info(runtime, 0U, &info) == RNS_OK);
    } while (info.state != RNS_RUNTIME_INTERFACE_DOWN &&
             before_deadline(deadline));
    assert(info.state == RNS_RUNTIME_INTERFACE_DOWN);
    assert(info.connections_lost == 1U);
    assert(info.last_error == RNS_ERROR_IO);

    clock.now = 4.0;
    rns_tcp_endpoint_t *second = await_runtime_client(runtime, listener);
    assert(rns_runtime_interface_info(runtime, 0U, &info) == RNS_OK);
    assert(info.connection_attempts == 4U);
    assert(info.connections_established == 2U);
    rns_tcp_endpoint_destroy(second);
    rns_tcp_endpoint_destroy(listener);
    rns_runtime_destroy(runtime);
}

static rns_tcp_endpoint_t *connect_to_runtime_server(
    rns_runtime_t *runtime, uint16_t port, uint64_t expected_connections) {
    rns_tcp_endpoint_t *client = NULL;
    assert(rns_tcp_endpoint_create(&client, RNS_UDP_IPV4, 2048U) == RNS_OK);
    assert(rns_tcp_connect(client, "127.0.0.1", port) == RNS_OK);
    uint64_t deadline = deadline_ms(3000U);
    while (rns_tcp_state(client) != RNS_TCP_CONNECTED && before_deadline(deadline)) {
        rns_status_t status = rns_tcp_finish_connect(client);
        assert(status == RNS_OK || status == RNS_ERROR_TIMEOUT);
        (void)poll_runtime(runtime);
    }
    assert(rns_tcp_state(client) == RNS_TCP_CONNECTED);
    while (before_deadline(deadline)) {
        rns_runtime_interface_info_t info;
        (void)poll_runtime(runtime);
        assert(rns_runtime_interface_info(runtime, 0U, &info) == RNS_OK);
        if (info.connections_established >= expected_connections) break;
    }
    return client;
}

static void test_server_accepts_second_client(void) {
    uint16_t port = reserve_tcp_port();
    rns_config_t config;
    configure_tcp(&config, RNS_CONFIG_TCP_SERVER, port);
    rns_runtime_t *runtime = NULL;
    assert(rns_runtime_create(&runtime, &config, NULL) == RNS_OK);
    rns_tcp_endpoint_t *first = connect_to_runtime_server(runtime, port, 1U);
    rns_runtime_interface_info_t info;
    assert(rns_runtime_interface_info(runtime, 0U, &info) == RNS_OK);
    assert(info.state == RNS_RUNTIME_INTERFACE_UP);
    assert(info.connections_established == 1U);

    rns_tcp_endpoint_destroy(first);
    uint64_t deadline = deadline_ms(3000U);
    do {
        (void)poll_runtime(runtime);
        assert(rns_runtime_interface_info(runtime, 0U, &info) == RNS_OK);
    } while (info.connections_lost == 0U && before_deadline(deadline));
    assert(info.connections_lost == 1U);
    assert(info.state == RNS_RUNTIME_INTERFACE_UP);
    assert(info.last_error == RNS_OK);

    rns_tcp_endpoint_t *second = connect_to_runtime_server(runtime, port, 2U);
    assert(rns_runtime_interface_info(runtime, 0U, &info) == RNS_OK);
    assert(info.connections_established == 2U);
    assert(info.connections_lost == 1U);
    rns_tcp_endpoint_destroy(second);
    rns_runtime_destroy(runtime);
}

int main(void) {
    test_client_reconnect();
    test_server_accepts_second_client();
    return 0;
}
