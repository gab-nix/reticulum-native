#include "reticulum/node.h"

#include "reticulum/packet.h"

#include <assert.h>
#include <string.h>

static double test_clock(void *context) { return *(double *)context; }

static void fill(uint8_t *output, size_t length, uint8_t value) {
    memset(output, value, length);
}

static size_t encode_data(const rns_node *node, const uint8_t destination[16],
                          uint8_t marker, uint8_t raw[RNS_MTU]) {
    rns_packet packet = {0};
    size_t raw_length = 0;
    packet.header_type = RNS_PACKET_HEADER_2;
    packet.transport_type = 1;
    packet.destination_type = 0;
    packet.packet_type = 0;
    memcpy(packet.transport_id, node->transport_id, 16);
    memcpy(packet.destination_hash, destination, 16);
    packet.data = &marker;
    packet.data_length = 1;
    assert(rns_packet_encode(&packet, raw, RNS_MTU, &raw_length));
    return raw_length;
}

static size_t encode_proof(const uint8_t packet_hash[32], uint8_t context,
                           uint8_t header_type, uint8_t marker,
                           size_t proof_length, uint8_t raw[RNS_MTU]) {
    uint8_t proof[96];
    rns_packet packet = {0};
    size_t raw_length = 0;
    assert(proof_length <= sizeof proof);
    fill(proof, proof_length, marker);
    packet.header_type = header_type;
    packet.transport_type = header_type == RNS_PACKET_HEADER_2 ? 1 : 0;
    packet.destination_type = 0;
    packet.packet_type = 3;
    if (header_type == RNS_PACKET_HEADER_2) fill(packet.transport_id, 16, 0x77);
    memcpy(packet.destination_hash, packet_hash, 16);
    packet.context = context;
    packet.data = proof;
    packet.data_length = proof_length;
    assert(rns_packet_encode(&packet, raw, RNS_MTU, &raw_length));
    return raw_length;
}

static void install_path(rns_node *node, const uint8_t destination[16],
                         uint64_t interface_id, uint8_t marker) {
    uint8_t next_hop[16], random_blob[10], announce_hash[32];
    fill(next_hop, sizeof next_hop, marker);
    fill(random_blob, sizeof random_blob, marker);
    random_blob[9] = marker;
    fill(announce_hash, sizeof announce_hash, (uint8_t)(marker + 1u));
    assert(rns_transport_consider_announce(&node->transport, destination,
                                           next_hop, interface_id, 0, 1,
                                           random_blob, announce_hash) ==
           RNS_PATH_INSERTED);
}

static void forward_data(rns_node *node, const uint8_t destination[16],
                         uint64_t ingress, uint8_t marker, uint8_t hash[32]) {
    uint8_t raw[RNS_MTU], output[RNS_MTU];
    size_t raw_length = encode_data(node, destination, marker, raw);
    rns_node_result result;
    rns_packet forwarded;
    assert(rns_packet_hash(raw, raw_length, hash));
    assert(rns_node_ingress(node, raw, raw_length, ingress, 0, output,
                            sizeof output, &result));
    assert(result.action == RNS_NODE_FORWARD);
    assert(result.forward_interface_id == 22);
    assert(rns_packet_decode(&forwarded, output, result.output_length));
    assert(forwarded.header_type == RNS_PACKET_HEADER_1);
    assert(forwarded.transport_type == 0 && forwarded.hops == 1);
}

static void forward_proof(rns_node *node, const uint8_t hash[32],
                          size_t proof_length, uint8_t marker) {
    uint8_t raw[RNS_MTU], output[RNS_MTU];
    size_t raw_length = encode_proof(hash, 0, RNS_PACKET_HEADER_1, marker,
                                     proof_length, raw);
    rns_node_result result;
    rns_packet forwarded;
    assert(rns_node_ingress(node, raw, raw_length, 22, 0, output,
                            sizeof output, &result));
    assert(result.action == RNS_NODE_FORWARD);
    assert(result.forward_interface_id == 11);
    assert(rns_packet_decode(&forwarded, output, result.output_length));
    assert(forwarded.header_type == RNS_PACKET_HEADER_1);
    assert(forwarded.packet_type == 3 && forwarded.context == 0);
    assert(forwarded.hops == 1 && forwarded.data_length == proof_length);
}

int main(void) {
    double now = 10;
    rns_node node;
    rns_node_config config = {0};
    uint8_t destination[16], raw[RNS_MTU], output[RNS_MTU];
    uint8_t explicit_hash[32], implicit_hash[32], wrong_hash[32], expired_hash[32];
    uint8_t evicted_hash[32], retained_hash[32], newest_hash[32];
    size_t raw_length;
    rns_node_result result;

    config.transport.path_capacity = 2;
    config.transport.dedupe_capacity = 32;
    config.transport.reverse_capacity = 2;
    config.transport.random_blob_history = 2;
    config.transport.path_lifetime = 600;
    config.transport.dedupe_lifetime = 60;
    config.transport.reverse_lifetime = 8;
    config.transport.clock = test_clock;
    config.transport.clock_context = &now;
    fill(config.transport_id, 16, 0x44);
    fill(config.path_request_destination, 16, 0x55);
    config.max_hops = 8;
    config.local_destination_capacity = 1;
    assert(rns_node_init(&node, &config));

    fill(destination, sizeof destination, 0x66);
    install_path(&node, destination, 22, 0x33);

    /* A runtime interface-send failure rolls back the just-recorded reverse
     * route instead of leaving proof state for a packet that never left. */
    raw_length = encode_data(&node, destination, 0xf0, raw);
    uint8_t failed_hash[32];
    assert(rns_packet_hash(raw, raw_length, failed_hash));
    assert(rns_node_ingress(&node, raw, raw_length, 11, 0, output,
                            sizeof output, &result));
    assert(result.action == RNS_NODE_FORWARD);
    assert(rns_node_complete_forward(&node, &result, 0));
    raw_length = encode_proof(failed_hash, 0, RNS_PACKET_HEADER_1, 0xf1, 64,
                              raw);
    assert(rns_node_ingress(&node, raw, raw_length, 22, 0, output,
                            sizeof output, &result));
    assert(result.reason == RNS_NODE_REASON_NO_REVERSE_PATH);

    /* Explicit and implicit ordinary proofs traverse the recorded hop. */
    forward_data(&node, destination, 11, 1, explicit_hash);
    forward_proof(&node, explicit_hash, 96, 0xa1);
    now += 0.1;
    forward_data(&node, destination, 11, 2, implicit_hash);
    raw_length = encode_proof(implicit_hash, 0, RNS_PACKET_HEADER_1, 0xa2,
                              64, raw);
    assert(rns_node_ingress(&node, raw, raw_length, 22, 0, output, 1,
                            &result));
    assert(result.action == RNS_NODE_DROP &&
           result.reason == RNS_NODE_REASON_OUTPUT_TOO_SMALL);
    /* Failed output encoding must not deduplicate the proof or consume the
     * reverse route. */
    forward_proof(&node, implicit_hash, 64, 0xa2);

    /* The same proof cannot loop after its packet hash was accepted. */
    raw_length = encode_proof(implicit_hash, 0, RNS_PACKET_HEADER_1, 0xa2,
                              64, raw);
    assert(rns_node_ingress(&node, raw, raw_length, 22, 0, output,
                            sizeof output, &result));
    assert(result.action == RNS_NODE_DROP &&
           result.reason == RNS_NODE_REASON_DUPLICATE);

    /* A proof from the wrong side consumes the path and cannot be retried. */
    now += 0.1;
    forward_data(&node, destination, 11, 3, wrong_hash);
    raw_length = encode_proof(wrong_hash, 0, RNS_PACKET_HEADER_1, 0xa3, 64,
                              raw);
    assert(rns_node_ingress(&node, raw, raw_length, 23, 0, output,
                            sizeof output, &result));
    assert(result.reason == RNS_NODE_REASON_WRONG_REVERSE_INTERFACE);
    raw_length = encode_proof(wrong_hash, 0, RNS_PACKET_HEADER_1, 0xa4, 64,
                              raw);
    assert(rns_node_ingress(&node, raw, raw_length, 22, 0, output,
                            sizeof output, &result));
    assert(result.reason == RNS_NODE_REASON_NO_REVERSE_PATH);

    /* Expiry is driven only by the injected monotonic clock. */
    now += 0.1;
    forward_data(&node, destination, 11, 4, expired_hash);
    now += 8;
    raw_length = encode_proof(expired_hash, 0, RNS_PACKET_HEADER_1, 0xa5, 96,
                              raw);
    assert(rns_node_ingress(&node, raw, raw_length, 22, 0, output,
                            sizeof output, &result));
    assert(result.reason == RNS_NODE_REASON_NO_REVERSE_PATH);

    /* Only ordinary header-1/context-none proof representations are eligible. */
    now += 61;
    forward_data(&node, destination, 11, 5, explicit_hash);
    raw_length = encode_proof(explicit_hash, RNS_NODE_RESOURCE_PROOF_CONTEXT,
                              RNS_PACKET_HEADER_1, 0xa6, 64, raw);
    assert(rns_node_ingress(&node, raw, raw_length, 22, 0, output,
                            sizeof output, &result));
    assert(result.reason == RNS_NODE_REASON_BAD_PROOF);
    raw_length = encode_proof(explicit_hash, 0, RNS_PACKET_HEADER_2, 0xa7, 64,
                              raw);
    assert(rns_node_ingress(&node, raw, raw_length, 22, 0, output,
                            sizeof output, &result));
    assert(result.reason == RNS_NODE_REASON_BAD_PROOF);
    raw_length = encode_proof(explicit_hash, 0, RNS_PACKET_HEADER_1, 0xa8, 63,
                              raw);
    assert(rns_node_ingress(&node, raw, raw_length, 22, 0, output,
                            sizeof output, &result));
    assert(result.reason == RNS_NODE_REASON_BAD_PROOF);
    assert(rns_node_ingress(&node, raw, 18, 22, 0, output, sizeof output,
                            &result));
    assert(result.reason == RNS_NODE_REASON_MALFORMED);

    /* A full table deterministically evicts its oldest live reverse path. */
    now += 0.1;
    forward_data(&node, destination, 11, 6, evicted_hash);
    now += 0.1;
    forward_data(&node, destination, 11, 7, retained_hash);
    now += 0.1;
    forward_data(&node, destination, 11, 8, newest_hash);
    raw_length = encode_proof(evicted_hash, 0, RNS_PACKET_HEADER_1, 0xb1, 64,
                              raw);
    assert(rns_node_ingress(&node, raw, raw_length, 22, 0, output,
                            sizeof output, &result));
    assert(result.reason == RNS_NODE_REASON_NO_REVERSE_PATH);
    forward_proof(&node, retained_hash, 64, 0xb2);
    forward_proof(&node, newest_hash, 96, 0xb3);

    rns_node_free(&node);
    return 0;
}
