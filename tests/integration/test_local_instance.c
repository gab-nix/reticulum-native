#include "reticulum/hal.h"
#include "reticulum/local.h"
#include "reticulum/packet.h"
#include "reticulum/tcp.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>

typedef struct capture {
    uint8_t data[RNS_MTU];
    size_t length;
    size_t calls;
} capture_t;

typedef struct fake_clock {
    double now;
} fake_clock_t;

static double fake_now(void *context) {
    return ((const fake_clock_t *)context)->now;
}

static rns_status_t capture_frame(const uint8_t *frame, size_t length,
                                  void *context) {
    capture_t *capture = context;
    assert(length <= sizeof(capture->data));
    memcpy(capture->data, frame, length);
    capture->length = length;
    capture->calls++;
    return RNS_OK;
}

static rns_status_t ignore_frame(const uint8_t *frame, size_t length,
                                 void *context) {
    (void)frame;
    (void)length;
    (void)context;
    return RNS_OK;
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

static uint16_t reserve_port(void) {
    rns_tcp_endpoint_t *listener = NULL;
    rns_udp_address_t address;
    assert(rns_tcp_endpoint_create(&listener, RNS_UDP_IPV4, 1024U) == RNS_OK);
    assert(rns_tcp_listen(listener, "127.0.0.1", 0U, 4) == RNS_OK);
    assert(rns_tcp_local_address(listener, &address) == RNS_OK);
    rns_tcp_endpoint_destroy(listener);
    return address.port;
}

static void poll_ok(rns_local_instance_t *instance, capture_t *capture) {
    rns_status_t status = rns_local_instance_poll(
        instance, capture != NULL ? capture_frame : ignore_frame, capture);
    assert(status == RNS_OK || status == RNS_ERROR_IO ||
           status == RNS_ERROR_NOT_FOUND);
}

static void await_clients(rns_local_instance_t *server,
                          rns_local_instance_t *first,
                          rns_local_instance_t *second,
                          size_t wanted) {
    uint64_t deadline = deadline_ms();
    rns_local_info_t server_info, first_info, second_info;
    memset(&second_info, 0, sizeof(second_info));
    while (before(deadline)) {
        poll_ok(server, NULL);
        poll_ok(first, NULL);
        if (second != NULL) poll_ok(second, NULL);
        assert(rns_local_instance_info(server, &server_info) == RNS_OK);
        assert(rns_local_instance_info(first, &first_info) == RNS_OK);
        if (second != NULL)
            assert(rns_local_instance_info(second, &second_info) == RNS_OK);
        if (server_info.connected_clients == wanted &&
            first_info.state == RNS_LOCAL_UP &&
            (second == NULL || second_info.state == RNS_LOCAL_UP))
            return;
    }
    assert(false);
}

int main(void) {
    static const uint8_t request[] = {0x01U, 0x7eU, 0x7dU, 0x04U};
    static const uint8_t reply[] = {0x10U, 0x20U, 0x30U};
    uint16_t port = reserve_port();
    fake_clock_t clock = {0.0};
    rns_local_options_t server_options = {
        .role = RNS_LOCAL_ROLE_AUTO, .port = port, .max_clients = 2U,
        .clock = fake_now, .clock_context = &clock};
    rns_local_options_t client_options = server_options;
    rns_local_instance_t *server = NULL, *first = NULL, *second = NULL;
    assert(rns_local_instance_create(&server, &server_options) == RNS_OK);
    assert(rns_local_instance_create(&first, &client_options) == RNS_OK);
    await_clients(server, first, NULL, 1U);

    rns_local_info_t info;
    assert(rns_local_instance_info(server, &info) == RNS_OK);
    assert(info.role == RNS_LOCAL_ROLE_SERVER && info.state == RNS_LOCAL_UP);
    assert(rns_local_instance_info(first, &info) == RNS_OK);
    assert(info.role == RNS_LOCAL_ROLE_CLIENT && info.connection_attempts == 1U);

    capture_t received = {{0U}, 0U, 0U};
    assert(rns_local_instance_send(first, request, sizeof(request)) == RNS_OK);
    uint64_t deadline = deadline_ms();
    while (received.calls == 0U && before(deadline)) {
        poll_ok(first, NULL);
        poll_ok(server, &received);
    }
    assert(received.calls == 1U && received.length == sizeof(request));
    assert(memcmp(received.data, request, sizeof(request)) == 0);

    memset(&received, 0, sizeof(received));
    assert(rns_local_instance_send(server, reply, sizeof(reply)) == RNS_OK);
    deadline = deadline_ms();
    while (received.calls == 0U && before(deadline)) {
        poll_ok(server, NULL);
        poll_ok(first, &received);
    }
    assert(received.calls == 1U && received.length == sizeof(reply));
    assert(memcmp(received.data, reply, sizeof(reply)) == 0);

    assert(rns_local_instance_create(&second, &client_options) == RNS_OK);
    await_clients(server, first, second, 2U);
    assert(rns_local_instance_info(server, &info) == RNS_OK);
    assert(info.connections_established == 2U);

    /* Destroying the elected server leaves both clients down but recoverable. */
    rns_local_instance_destroy(server);
    server = NULL;
    deadline = deadline_ms();
    do {
        poll_ok(first, NULL);
        poll_ok(second, NULL);
        assert(rns_local_instance_info(first, &info) == RNS_OK);
        rns_local_info_t second_info;
        assert(rns_local_instance_info(second, &second_info) == RNS_OK);
        if (info.state == RNS_LOCAL_DOWN &&
            second_info.state == RNS_LOCAL_DOWN)
            break;
    } while (before(deadline));
    assert(info.state == RNS_LOCAL_DOWN && info.connections_lost == 1U);
    assert(rns_local_instance_info(second, &info) == RNS_OK);
    assert(info.state == RNS_LOCAL_DOWN && info.connections_lost == 1U);

    server_options.role = RNS_LOCAL_ROLE_SERVER;
    assert(rns_local_instance_create(&server, &server_options) == RNS_OK);
    clock.now = 8.0;
    await_clients(server, first, second, 2U);
    assert(rns_local_instance_info(first, &info) == RNS_OK);
    assert(info.connections_established == 2U && info.last_error == RNS_OK);

    rns_local_instance_destroy(second);
    rns_local_instance_destroy(first);
    rns_local_instance_destroy(server);
    assert(rns_local_instance_create(NULL, &client_options) ==
           RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_local_instance_info(NULL, &info) == RNS_ERROR_INVALID_ARGUMENT);
    return 0;
}
