#include "reticulum/packet.h"
#include "reticulum/tcp.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct capture {
    uint8_t packet[RNS_MTU];
    size_t length;
    size_t calls;
} capture_t;

static rns_status_t capture_frame(const uint8_t *frame, size_t length, void *context) {
    capture_t *capture = context;
    assert(length <= sizeof(capture->packet));
    memcpy(capture->packet, frame, length);
    capture->length = length;
    capture->calls++;
    return RNS_OK;
}

static void count_state(rns_tcp_endpoint_t *endpoint,
                        rns_tcp_state_t previous,
                        rns_tcp_state_t current,
                        void *context) {
    size_t *changes = context;
    (void)endpoint;
    (void)previous;
    (void)current;
    (*changes)++;
}

int main(void) {
    static const uint8_t request[] = {0x01U, RNS_HDLC_FLAG, RNS_HDLC_ESCAPE, 0x04U};
    static const uint8_t reply[] = {0x10U, 0x20U, 0x30U};
    rns_tcp_endpoint_t *listener = NULL;
    rns_tcp_endpoint_t *client = NULL;
    rns_tcp_endpoint_t *server = NULL;
    rns_udp_address_t address;
    capture_t request_capture = {{0U}, 0U, 0U};
    capture_t reply_capture = {{0U}, 0U, 0U};
    size_t changes = 0U;
    size_t progress = 0U;
    size_t iteration;
    uint8_t oversized[RNS_MTU + 1U] = {0U};

    assert(rns_tcp_endpoint_create(&listener, RNS_UDP_IPV4, 2048U) == RNS_OK);
    assert(rns_tcp_endpoint_create(&client, RNS_UDP_IPV4, 2048U) == RNS_OK);
    rns_tcp_set_state_callback(client, count_state, &changes);
    assert(rns_tcp_listen(listener, "127.0.0.1", 0U, 4) == RNS_OK);
    assert(rns_tcp_local_address(listener, &address) == RNS_OK && address.port != 0U);
    assert(rns_tcp_connect(client, "127.0.0.1", address.port) == RNS_OK);

    for (iteration = 0U; iteration < 1000U && server == NULL; ++iteration) {
        rns_status_t accepted = rns_tcp_accept(listener, &server, 2048U);
        assert(accepted == RNS_OK || accepted == RNS_ERROR_TIMEOUT);
        if (rns_tcp_state(client) == RNS_TCP_CONNECTING) {
            rns_status_t connected = rns_tcp_finish_connect(client);
            assert(connected == RNS_OK || connected == RNS_ERROR_TIMEOUT);
        }
    }
    assert(server != NULL && rns_tcp_state(client) == RNS_TCP_CONNECTED);
    assert(changes >= 1U);
    assert(rns_tcp_queue_frame(client, request, sizeof(request)) == RNS_OK);
    assert(rns_tcp_queue_frame(client, oversized, sizeof(oversized)) == RNS_ERROR_OVERFLOW);
    assert(rns_tcp_flush(client, &progress) == RNS_OK && progress != 0U);
    for (iteration = 0U; iteration < 1000U && request_capture.calls == 0U; ++iteration) {
        assert(rns_tcp_poll_receive(server, capture_frame, &request_capture, &progress) == RNS_OK);
    }
    assert(request_capture.calls == 1U && request_capture.length == sizeof(request));
    assert(memcmp(request_capture.packet, request, sizeof(request)) == 0);

    assert(rns_tcp_queue_frame(server, reply, sizeof(reply)) == RNS_OK);
    assert(rns_tcp_flush(server, &progress) == RNS_OK);
    for (iteration = 0U; iteration < 1000U && reply_capture.calls == 0U; ++iteration) {
        assert(rns_tcp_poll_receive(client, capture_frame, &reply_capture, &progress) == RNS_OK);
    }
    assert(reply_capture.calls == 1U && memcmp(reply_capture.packet, reply, sizeof(reply)) == 0);

    rns_tcp_disconnect(client);
    assert(rns_tcp_state(client) == RNS_TCP_DISCONNECTED && changes >= 2U);
    rns_tcp_endpoint_destroy(server);
    rns_tcp_endpoint_destroy(client);
    rns_tcp_endpoint_destroy(listener);
    return 0;
}
