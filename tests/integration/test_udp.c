#include "reticulum/packet.h"
#include "reticulum/udp.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct capture {
    uint8_t packet[RNS_MTU];
    size_t length;
    size_t calls;
    rns_udp_address_t source;
} capture_t;

static rns_status_t receive_packet(const uint8_t *packet,
                                   size_t packet_length,
                                   const rns_udp_address_t *source,
                                   void *context) {
    capture_t *capture = context;
    memcpy(capture->packet, packet, packet_length);
    capture->length = packet_length;
    capture->source = *source;
    capture->calls++;
    return RNS_OK;
}

int main(void) {
    static const uint8_t message[] = {0x52U, 0x4eU, 0x53U, 0x01U};
    rns_udp_endpoint_t *receiver = NULL;
    rns_udp_endpoint_t *sender = NULL;
    rns_udp_address_t receiver_address;
    capture_t capture = {{0U}, 0U, 0U, {RNS_UDP_IPV4, {0U}, 0U, 0U}};
    uint8_t oversized[RNS_MTU + 1U] = {0U};
    size_t received = 0U;
    size_t attempt;

    assert(rns_udp_endpoint_create(&receiver, RNS_UDP_IPV4) == RNS_OK);
    assert(rns_udp_endpoint_create(&sender, RNS_UDP_IPV4) == RNS_OK);
    assert(rns_udp_bind(receiver, "127.0.0.1", 0U) == RNS_OK);
    assert(rns_udp_local_address(receiver, &receiver_address) == RNS_OK);
    assert(receiver_address.port != 0U);
    assert(rns_udp_connect(sender, "127.0.0.1", receiver_address.port) == RNS_OK);
    assert(rns_udp_send(sender, message, sizeof(message)) == RNS_OK);

    for (attempt = 0U; attempt < 100U && capture.calls == 0U; ++attempt) {
        assert(rns_udp_poll(receiver, 1U, receive_packet, &capture, &received) == RNS_OK);
    }
    assert(capture.calls == 1U && received == 1U);
    assert(capture.source.family == RNS_UDP_IPV4 && capture.source.port != 0U);
    assert(capture.length == sizeof(message));
    assert(memcmp(capture.packet, message, sizeof(message)) == 0);
    assert(rns_udp_send(sender, oversized, sizeof(oversized)) == RNS_ERROR_OVERFLOW);
    assert(rns_udp_set_broadcast(sender, true) == RNS_OK);

    rns_udp_endpoint_destroy(sender);
    rns_udp_endpoint_destroy(receiver);
    return 0;
}

