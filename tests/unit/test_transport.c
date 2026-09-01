#include "reticulum/transport.h"

#include <assert.h>
#include <string.h>

static double test_clock(void *context) { return *(double *)context; }
static void fill(uint8_t *out, size_t length, uint8_t value) { memset(out, value, length); }
static void blob(uint8_t out[10], uint8_t nonce, uint64_t timestamp) {
    memset(out, nonce, 5); for (size_t i = 10; i > 5; --i) { out[i-1] = (uint8_t)timestamp; timestamp >>= 8; }
}

int main(void) {
    double now = 100; rns_transport transport; rns_transport_config config = {2, 2, 3, 30, 5, test_clock, &now};
    uint8_t dst[16], hop[16], packet_hash[32], b1[10], b2[10], b3[10], request_body[48], tag[4] = {1,2,3,4};
    size_t request_length; rns_path_request request; const rns_path_entry *entry;
    fill(dst, 16, 1); fill(hop, 16, 2); fill(packet_hash, 32, 3); blob(b1, 1, 1000); blob(b2, 2, 1001); blob(b3, 3, 999);
    assert(rns_transport_init(&transport, &config));

    assert(rns_transport_consider_announce(&transport, dst, hop, 9, 1, 2, b1, packet_hash) == RNS_PATH_INSERTED);
    entry = rns_transport_lookup(&transport, dst); assert(entry && entry->hops == 2 && entry->announce_timebase == 1000);
    /* Duplicate/same path is ignored, but same emission on a higher-gravity interface wins. */
    assert(rns_transport_consider_announce(&transport, dst, hop, 10, 1, 2, b1, packet_hash) == RNS_PATH_REJECTED);
    assert(rns_transport_consider_announce(&transport, dst, hop, 10, 2, 2, b1, packet_hash) == RNS_PATH_UPDATED);
    /* Newer announce replaces even with a worse hop count; older does not while live. */
    assert(rns_transport_consider_announce(&transport, dst, hop, 11, 1, 7, b2, packet_hash) == RNS_PATH_UPDATED);
    assert(rns_transport_consider_announce(&transport, dst, hop, 12, 1, 1, b3, packet_hash) == RNS_PATH_REJECTED);
    assert(rns_transport_mark_unresponsive(&transport, dst));
    assert(rns_transport_consider_announce(&transport, dst, hop, 13, 1, 8, b2, packet_hash) == RNS_PATH_UPDATED);

    /* An expired path accepts an unheard announce even with an older timebase. */
    now = 131; assert(rns_transport_consider_announce(&transport, dst, hop, 14, 1, 9, b3, packet_hash) == RNS_PATH_UPDATED);
    assert(rns_transport_lookup(&transport, dst)); now = 162; assert(!rns_transport_lookup(&transport, dst));

    now = 200; assert(rns_transport_accept_packet_hash(&transport, packet_hash));
    assert(!rns_transport_accept_packet_hash(&transport, packet_hash)); now = 206; assert(rns_transport_accept_packet_hash(&transport, packet_hash));

    assert(rns_path_request_build(dst, hop, tag, sizeof(tag), request_body, sizeof(request_body), &request_length));
    assert(request_length == 36 && rns_path_request_parse(&request, request_body, request_length));
    assert(request.has_requesting_transport && request.tag_length == 4 && memcmp(request.tag, tag, 4) == 0);
    assert(rns_path_request_build(dst, NULL, tag, sizeof(tag), request_body, sizeof(request_body), &request_length));
    assert(request_length == 20 && rns_path_request_parse(&request, request_body, request_length));
    assert(!request.has_requesting_transport && !rns_path_request_parse(&request, request_body, 16));
    assert(!rns_path_request_build(dst, NULL, tag, 0, request_body, sizeof(request_body), &request_length));

    rns_transport_free(&transport); return 0;
}
