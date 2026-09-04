#include "reticulum/transport.h"

#include <assert.h>
#include <string.h>

static double test_clock(void *context) { return *(double *)context; }
static void fill(uint8_t *out, size_t length, uint8_t value) { memset(out, value, length); }
static void blob(uint8_t out[10], uint8_t nonce, uint64_t timestamp) {
    memset(out, nonce, 5); for (size_t i = 10; i > 5; --i) { out[i-1] = (uint8_t)timestamp; timestamp >>= 8; }
}

int main(void) {
    double now = 100; rns_transport transport;
    rns_transport_config config = {
        .path_capacity = 2, .dedupe_capacity = 2, .reverse_capacity = 2,
        .random_blob_history = 3, .path_lifetime = 30,
        .dedupe_lifetime = 5, .reverse_lifetime = 8,
        .clock = test_clock, .clock_context = &now
    };
    uint8_t dst[16], hop[16], packet_hash[32], b1[10], b2[10], b3[10], request_body[49], tag[16];
    size_t request_length; rns_path_request request; const rns_path_entry *entry;
    fill(dst, 16, 1); fill(hop, 16, 2); fill(packet_hash, 32, 3); blob(b1, 1, 1000); blob(b2, 2, 1001); blob(b3, 3, 999);
    assert(rns_transport_init(&transport, &config));
    fill(tag, sizeof tag, 4);

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

    assert(rns_path_request_build(dst, NULL, tag, 1, request_body,
                                  sizeof request_body, &request_length));
    assert(request_length == 17 && rns_path_request_parse(&request, request_body,
                                                           request_length));
    assert(!request.has_requesting_transport && request.tag_length == 1);
    assert(rns_path_request_build(dst, NULL, tag, sizeof tag, request_body,
                                  sizeof request_body, &request_length));
    assert(request_length == 32 && rns_path_request_parse(&request, request_body,
                                                           request_length));
    assert(!request.has_requesting_transport && request.tag_length == 16);
    assert(rns_path_request_build(dst, hop, tag, 1, request_body,
                                  sizeof request_body, &request_length));
    assert(request_length == 33 && rns_path_request_parse(&request, request_body,
                                                           request_length));
    assert(request.has_requesting_transport && request.tag_length == 1);
    assert(rns_path_request_build(dst, hop, tag, sizeof tag, request_body,
                                  sizeof request_body, &request_length));
    assert(request_length == 48 && rns_path_request_parse(&request, request_body,
                                                           request_length));
    assert(request.has_requesting_transport && request.tag_length == 16 &&
           memcmp(request.tag, tag, sizeof tag) == 0);
    assert(!rns_path_request_parse(&request, request_body, 16));
    assert(!rns_path_request_parse(&request, request_body, 49));
    assert(!rns_path_request_build(dst, NULL, tag, 0, request_body, sizeof(request_body), &request_length));
    assert(!rns_path_request_build(dst, NULL, tag, sizeof tag, request_body,
                                   31, &request_length));

    /* Rolling back a failed forward restores the exact evicted reverse slot. */
    uint8_t reverse_a[32], reverse_b[32], reverse_c[32];
    fill(reverse_a, sizeof reverse_a, 0xa1);
    fill(reverse_b, sizeof reverse_b, 0xa2);
    fill(reverse_c, sizeof reverse_c, 0xa3);
    assert(rns_transport_record_reverse(&transport, reverse_a, 1, 11));
    now += 0.1;
    assert(rns_transport_record_reverse(&transport, reverse_b, 2, 22));
    now += 0.1;
    rns_transport_transaction transaction;
    assert(rns_transport_record_reverse_transaction(
        &transport, reverse_c, 3, 33, &transaction));
    assert(rns_transport_transaction_rollback(&transport, &transaction));
    uint64_t reverse_interface = 0;
    assert(rns_transport_consume_reverse(&transport, reverse_a, 11,
                                         &reverse_interface) ==
           RNS_REVERSE_MATCHED);
    assert(reverse_interface == 1);

    rns_transport_free(&transport); return 0;
}
