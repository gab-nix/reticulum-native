#include "reticulum/destination.h"
#include "reticulum/link.h"
#include "reticulum/node.h"
#include "reticulum/packet.h"

#include <assert.h>
#include <string.h>

static double test_clock(void *context) { return *(double *)context; }

static void fill(uint8_t *bytes, size_t length, uint8_t value) {
    memset(bytes, value, length);
}

static size_t encode_link_packet(const uint8_t link_id[16], uint8_t hops,
                                 uint8_t marker, uint8_t raw[RNS_MTU]) {
    rns_packet packet = {0};
    size_t length = 0;
    packet.destination_type = 3;
    packet.hops = hops;
    packet.context = 0x22;
    memcpy(packet.destination_hash, link_id, 16);
    packet.data = &marker;
    packet.data_length = 1;
    assert(rns_packet_encode(&packet, raw, RNS_MTU, &length));
    return length;
}

int main(void) {
    double now = 10.0;
    rns_identity destination;
    rns_link initiator, responder;
    rns_node node;
    rns_node_config config = {0};
    uint8_t destination_hash[16];
    static const char *const aspects[] = {"delivery"};
    assert(rns_identity_generate(&destination));
    assert(rns_destination_hash(&destination, "lxmf", aspects, 1,
                                destination_hash));

    config.transport.path_capacity = 2;
    config.transport.dedupe_capacity = 16;
    config.transport.reverse_capacity = 2;
    config.transport.link_capacity = 2;
    config.transport.random_blob_history = 2;
    config.transport.path_lifetime = 600;
    config.transport.dedupe_lifetime = 60;
    config.transport.reverse_lifetime = 8;
    config.transport.link_lifetime = 8;
    config.transport.link_proof_timeout_per_hop = 3;
    config.transport.clock = test_clock;
    config.transport.clock_context = &now;
    fill(config.transport_id, 16, 0x44);
    fill(config.path_request_destination, 16, 0x55);
    config.max_hops = 8;
    config.local_destination_capacity = 1;
    assert(rns_node_init(&node, &config));

    uint8_t next_hop[16], blob[10], announce_hash[32], public_key[64];
    fill(next_hop, sizeof next_hop, 0x33);
    fill(blob, sizeof blob, 0x11);
    fill(announce_hash, sizeof announce_hash, 0x12);
    assert(rns_transport_consider_announce(
               &node.transport, destination_hash, next_hop, 22, 0, 1, blob,
               announce_hash) == RNS_PATH_INSERTED);
    rns_identity_export_public(&destination, public_key);
    assert(rns_transport_set_path_identity(&node.transport, destination_hash,
                                           public_key));

    uint8_t request_payload[RNS_LINK_REQUEST_BYTES];
    uint8_t request_raw[RNS_MTU], forwarded_raw[RNS_MTU];
    size_t request_length = 0, forwarded_length = 0;
    assert(rns_link_initiator_init(&initiator, &destination, RNS_MTU, 10,
                                   test_clock, &now));
    assert(rns_link_build_request_payload(&initiator, request_payload));
    rns_packet request = {0};
    request.header_type = RNS_PACKET_HEADER_2;
    request.transport_type = 1;
    request.packet_type = 2;
    memcpy(request.transport_id, node.transport_id, 16);
    memcpy(request.destination_hash, destination_hash, 16);
    request.data = request_payload;
    request.data_length = sizeof request_payload;
    assert(rns_packet_encode(&request, request_raw, sizeof request_raw,
                             &request_length));
    assert(rns_link_initiator_set_request_packet(&initiator, request_raw,
                                                 request_length));

    rns_node_result result;
    request.data_length = 64;
    assert(rns_packet_encode(&request, request_raw, sizeof request_raw,
                             &request_length));
    assert(rns_node_ingress(&node, request_raw, request_length, 11, 0,
                            forwarded_raw, sizeof forwarded_raw, &result));
    assert(result.action == RNS_NODE_DROP &&
           result.reason == RNS_NODE_REASON_NO_PATH);
    request.data_length = sizeof request_payload;
    assert(rns_packet_encode(&request, request_raw, sizeof request_raw,
                             &request_length));
    assert(rns_node_ingress(&node, request_raw, request_length, 11, 0,
                            forwarded_raw, sizeof forwarded_raw, &result));
    assert(result.action == RNS_NODE_FORWARD &&
           result.forward_interface_id == 22);
    forwarded_length = result.output_length;
    const rns_transport_link_entry *entry = rns_transport_link_lookup(
        &node.transport, initiator.link_id);
    assert(entry != NULL && !entry->validated && entry->taken_hops == 1 &&
           entry->remaining_hops == 1);
    /* Simulate the selected interface rejecting the request. The pending
     * route must disappear. */
    assert(rns_node_complete_forward(&node, &result, 0));
    assert(rns_transport_link_lookup(&node.transport, initiator.link_id) ==
           NULL);
    now += 61;
    assert(rns_node_ingress(&node, request_raw, request_length, 11, 0,
                            forwarded_raw, sizeof forwarded_raw, &result));
    assert(result.action == RNS_NODE_FORWARD);
    forwarded_length = result.output_length;
    assert(rns_node_complete_forward(&node, &result, 1));
    entry = rns_transport_link_lookup(&node.transport, initiator.link_id);
    assert(entry != NULL && !entry->validated);

    /* Link traffic cannot pass before the destination proof authenticates it. */
    uint8_t link_raw[RNS_MTU], output[RNS_MTU];
    size_t link_length = encode_link_packet(initiator.link_id, 0, 0x71,
                                            link_raw);
    assert(rns_node_ingress(&node, link_raw, link_length, 11, 0, output,
                            sizeof output, &result));
    assert(result.action == RNS_NODE_DROP &&
           result.reason == RNS_NODE_REASON_LINK_NOT_VALIDATED);

    assert(rns_link_responder_accept(&responder, &destination, forwarded_raw,
                                     forwarded_length, 10, test_clock, &now));
    uint8_t proof[RNS_LINK_PROOF_BYTES];
    assert(rns_link_build_proof(&responder, proof));
    rns_packet proof_packet = {0};
    uint8_t proof_raw[RNS_MTU];
    size_t proof_length = 0;
    proof_packet.destination_type = 3;
    proof_packet.packet_type = 3;
    proof_packet.context = RNS_NODE_LINK_REQUEST_PROOF_CONTEXT;
    memcpy(proof_packet.destination_hash, initiator.link_id, 16);
    proof_packet.data = proof;
    proof_packet.data_length = sizeof proof;
    assert(rns_packet_encode(&proof_packet, proof_raw, sizeof proof_raw,
                             &proof_length));

    /* Wrong-side and forged proofs do not activate or consume the entry. */
    assert(rns_node_ingress(&node, proof_raw, proof_length, 11, 0, output,
                            sizeof output, &result));
    assert(result.reason == RNS_NODE_REASON_WRONG_LINK_INTERFACE);
    proof[0] ^= 1;
    assert(rns_packet_encode(&proof_packet, proof_raw, sizeof proof_raw,
                             &proof_length));
    assert(rns_node_ingress(&node, proof_raw, proof_length, 22, 0, output, 1,
                            &result));
    assert(result.action == RNS_NODE_DROP &&
           result.reason == RNS_NODE_REASON_OUTPUT_TOO_SMALL);
    entry = rns_transport_link_lookup(&node.transport, initiator.link_id);
    assert(entry != NULL && !entry->validated);
    assert(rns_node_ingress(&node, proof_raw, proof_length, 22, 0, output,
                            sizeof output, &result));
    assert(result.reason == RNS_NODE_REASON_INVALID_LINK_PROOF);
    proof[0] ^= 1;
    assert(rns_packet_encode(&proof_packet, proof_raw, sizeof proof_raw,
                             &proof_length));
    assert(rns_node_ingress(&node, proof_raw, proof_length, 22, 0, output,
                            sizeof output, &result));
    assert(result.action == RNS_NODE_FORWARD &&
           result.forward_interface_id == 11);
    assert(rns_node_complete_forward(&node, &result, 0));
    entry = rns_transport_link_lookup(&node.transport, initiator.link_id);
    assert(entry != NULL && !entry->validated);
    assert(rns_node_ingress(&node, proof_raw, proof_length, 22, 0, output,
                            sizeof output, &result));
    assert(result.action == RNS_NODE_FORWARD &&
           result.forward_interface_id == 11);
    assert(rns_node_complete_forward(&node, &result, 1));
    entry = rns_transport_link_lookup(&node.transport, initiator.link_id);
    assert(entry != NULL && entry->validated);

    /* Established packets go only to the opposite recorded interface. The
     * same raw link packet remains eligible because links own replay policy. */
    for (size_t repeat = 0; repeat < 2; ++repeat) {
        assert(rns_node_ingress(&node, link_raw, link_length, 11, 0, output,
                                sizeof output, &result));
        assert(result.action == RNS_NODE_FORWARD &&
               result.forward_interface_id == 22);
        rns_packet decoded;
        assert(rns_packet_decode(&decoded, output, result.output_length));
        assert(decoded.hops == 1);
    }
    link_length = encode_link_packet(initiator.link_id, 0, 0x72, link_raw);
    assert(rns_node_ingress(&node, link_raw, link_length, 22, 0, output,
                            sizeof output, &result));
    assert(result.action == RNS_NODE_FORWARD &&
           result.forward_interface_id == 11);
    link_length = encode_link_packet(initiator.link_id, 1, 0x73, link_raw);
    assert(rns_node_ingress(&node, link_raw, link_length, 11, 0, output,
                            sizeof output, &result));
    assert(result.reason == RNS_NODE_REASON_WRONG_LINK_HOPS);
    link_length = encode_link_packet(initiator.link_id, 0, 0x74, link_raw);
    assert(rns_node_ingress(&node, link_raw, link_length, 99, 0, output,
                            sizeof output, &result));
    assert(result.reason == RNS_NODE_REASON_WRONG_LINK_INTERFACE);

    now += 8;
    assert(rns_transport_link_lookup(&node.transport, initiator.link_id) ==
           NULL);
    assert(!rns_transport_forget_link(&node.transport, initiator.link_id));

    /* The pending table is bounded, evicts oldest-first, supports explicit
     * teardown and expires an unproved request at its per-hop deadline. */
    const rns_path_entry *path = rns_transport_lookup(&node.transport,
                                                       destination_hash);
    uint8_t first_id[16], second_id[16], third_id[16], pending_id[16];
    fill(first_id, sizeof first_id, 0xa1);
    fill(second_id, sizeof second_id, 0xa2);
    fill(third_id, sizeof third_id, 0xa3);
    fill(pending_id, sizeof pending_id, 0xa4);
    assert(path != NULL);
    assert(rns_transport_record_link_request(&node.transport, first_id, path,
                                             11, 1));
    now += 0.1;
    assert(rns_transport_record_link_request(&node.transport, second_id, path,
                                             11, 1));
    now += 0.1;
    rns_transport_transaction transaction;
    assert(rns_transport_record_link_request_transaction(
        &node.transport, third_id, path, 11, 1, &transaction));
    assert(rns_transport_transaction_rollback(&node.transport, &transaction));
    assert(rns_transport_link_lookup(&node.transport, first_id) != NULL);
    assert(rns_transport_link_lookup(&node.transport, third_id) == NULL);
    assert(rns_transport_record_link_request(&node.transport, third_id, path,
                                             11, 1));
    assert(rns_transport_link_lookup(&node.transport, first_id) == NULL);
    assert(rns_transport_link_lookup(&node.transport, second_id) != NULL);
    assert(rns_transport_forget_link(&node.transport, third_id));
    assert(rns_transport_link_lookup(&node.transport, third_id) == NULL);
    assert(rns_transport_record_link_request(&node.transport, pending_id, path,
                                             11, 1));
    now += 3;
    assert(rns_transport_link_lookup(&node.transport, pending_id) == NULL);
    rns_node_free(&node);
    return 0;
}
