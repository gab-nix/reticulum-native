#include "reticulum/node.h"

#include "reticulum/announce.h"
#include "reticulum/packet.h"

#include <stdlib.h>
#include <string.h>

static int local_destination(const rns_node *node, const uint8_t hash[16]) {
    for (size_t i = 0; i < node->local_destination_count; ++i)
        if (memcmp(node->local_destinations + i * 16, hash, 16) == 0) return 1;
    return 0;
}

int rns_node_init(rns_node *node, const rns_node_config *config) {
    if (!node || !config || config->max_hops == 0 || config->local_destination_capacity == 0) return 0;
    memset(node, 0, sizeof(*node));
    if (!rns_transport_init(&node->transport, &config->transport)) return 0;
    node->local_destinations = calloc(config->local_destination_capacity, 16);
    if (!node->local_destinations) { rns_node_free(node); return 0; }
    memcpy(node->transport_id, config->transport_id, 16);
    memcpy(node->path_request_destination, config->path_request_destination, 16);
    node->max_hops = config->max_hops; node->local_destination_capacity = config->local_destination_capacity;
    return 1;
}

void rns_node_free(rns_node *node) {
    if (!node) return; rns_transport_free(&node->transport); free(node->local_destinations); memset(node, 0, sizeof(*node));
}

int rns_node_register_destination(rns_node *node, const uint8_t destination_hash[16]) {
    if (!node || !destination_hash || !node->local_destinations) return 0;
    if (local_destination(node, destination_hash)) return 1;
    if (node->local_destination_count == node->local_destination_capacity) return 0;
    memcpy(node->local_destinations + node->local_destination_count++ * 16, destination_hash, 16); return 1;
}

int rns_node_unregister_destination(rns_node *node, const uint8_t destination_hash[16]) {
    if (!node || !destination_hash) return 0;
    for (size_t i = 0; i < node->local_destination_count; ++i) {
        uint8_t *slot = node->local_destinations + i * 16;
        if (memcmp(slot, destination_hash, 16) == 0) {
            if (i + 1 < node->local_destination_count)
                memmove(slot, slot + 16, (node->local_destination_count - i - 1) * 16);
            node->local_destination_count--; return 1;
        }
    }
    return 0;
}

static int rewrite_packet(const rns_packet *packet, uint8_t hops, const uint8_t transport_id[16],
                          uint8_t *output, size_t capacity, size_t *length) {
    rns_packet rewritten = *packet;
    rewritten.header_type = RNS_PACKET_HEADER_2; rewritten.transport_type = 1; rewritten.hops = hops;
    memcpy(rewritten.transport_id, transport_id, 16);
    return rns_packet_encode(&rewritten, output, capacity, length);
}

static int announce_ingress(rns_node *node, const rns_packet *packet, const uint8_t packet_hash[32],
                            uint8_t received_hops, uint64_t interface_id, int32_t interface_gravity,
                            uint8_t *output, size_t output_capacity, rns_node_result *result) {
    rns_announce announce; uint8_t next_hop[16]; rns_path_update_result update;
    if (!rns_announce_parse(&announce, packet->data, packet->data_length, packet->context_flag) ||
        !rns_announce_verify(packet->destination_hash, packet->data, packet->data_length, packet->context_flag)) {
        result->reason = RNS_NODE_REASON_INVALID_ANNOUNCE; return 1;
    }
    if (local_destination(node, packet->destination_hash)) { result->action = RNS_NODE_DELIVER; return 1; }
    if (packet->header_type == RNS_PACKET_HEADER_2) memcpy(next_hop, packet->transport_id, 16);
    else memcpy(next_hop, packet->destination_hash, 16);
    update = rns_transport_consider_announce(&node->transport, packet->destination_hash, next_hop,
                                             interface_id, interface_gravity, received_hops,
                                             announce.random_blob, packet_hash);
    if (update == RNS_PATH_REJECTED) { result->reason = RNS_NODE_REASON_STALE_ANNOUNCE; return 1; }
    result->has_verified_announce = 1;
    result->received_interface_id = interface_id;
    result->path_update = update;
    result->announce_app_data = announce.app_data;
    result->announce_app_data_length = announce.app_data_length;
    result->announce_timebase = announce.timestamp;
    result->announce_has_ratchet = announce.has_ratchet;
    result->received_at = node->transport.config.clock(node->transport.config.clock_context);
    if (!rns_identity_from_public(&result->announce_identity, announce.public_key)) {
        result->has_verified_announce = 0;
        result->reason = RNS_NODE_REASON_INVALID_ANNOUNCE;
        return 1;
    }
    memcpy(result->next_hop, next_hop, 16);
    if (packet->context == RNS_NODE_PATH_RESPONSE_CONTEXT) { result->action = RNS_NODE_DELIVER; return 1; }
    if (!rewrite_packet(packet, received_hops, node->transport_id, output, output_capacity, &result->output_length)) {
        result->reason = RNS_NODE_REASON_OUTPUT_TOO_SMALL; return 1;
    }
    result->action = RNS_NODE_REBROADCAST; return 1;
}

static int path_request_ingress(rns_node *node, const rns_packet *packet, rns_node_result *result) {
    const rns_path_entry *path;
    if (!rns_path_request_parse(&result->path_request, packet->data, packet->data_length)) {
        result->reason = RNS_NODE_REASON_BAD_PATH_REQUEST; return 1;
    }
    result->has_path_request = 1; path = rns_transport_lookup(&node->transport, result->path_request.destination_hash);
    if (!path) { result->reason = RNS_NODE_REASON_NO_PATH; return 1; }
    memcpy(result->destination_hash, result->path_request.destination_hash, 16);
    memcpy(result->next_hop, path->next_hop, 16); result->action = RNS_NODE_PATH_RESPONSE; return 1;
}

int rns_node_ingress(rns_node *node, const uint8_t *raw, size_t raw_length,
                     uint64_t interface_id, int32_t interface_gravity,
                     uint8_t *output, size_t output_capacity, rns_node_result *result) {
    rns_packet packet; const rns_path_entry *path; uint8_t received_hops;
    if (!result) return 0; memset(result, 0, sizeof(*result)); result->action = RNS_NODE_DROP;
    result->received_interface_id = interface_id;
    if (!node || !raw || !rns_packet_decode(&packet, raw, raw_length) ||
        !rns_packet_hash(raw, raw_length, result->packet_hash)) { result->reason = RNS_NODE_REASON_MALFORMED; return 1; }
    memcpy(result->destination_hash, packet.destination_hash, 16);
    if (!rns_transport_accept_packet_hash(&node->transport, result->packet_hash)) { result->reason = RNS_NODE_REASON_DUPLICATE; return 1; }
    if (packet.hops >= node->max_hops) { result->reason = RNS_NODE_REASON_MAX_HOPS; return 1; }
    received_hops = (uint8_t)(packet.hops + 1); result->hops = received_hops;
    if (packet.packet_type == 1)
        return announce_ingress(node, &packet, result->packet_hash, received_hops, interface_id,
                                interface_gravity, output, output_capacity, result);
    if (memcmp(packet.destination_hash, node->path_request_destination, 16) == 0)
        return path_request_ingress(node, &packet, result);
    if (local_destination(node, packet.destination_hash)) { result->action = RNS_NODE_DELIVER; return 1; }
    if (packet.header_type == RNS_PACKET_HEADER_2 && memcmp(packet.transport_id, node->transport_id, 16) != 0) {
        result->reason = RNS_NODE_REASON_NOT_FOR_US; return 1;
    }
    path = rns_transport_lookup(&node->transport, packet.destination_hash);
    if (!path) { result->reason = RNS_NODE_REASON_NO_PATH; return 1; }
    if (!rewrite_packet(&packet, received_hops, path->next_hop, output, output_capacity, &result->output_length)) {
        result->reason = RNS_NODE_REASON_OUTPUT_TOO_SMALL; return 1;
    }
    memcpy(result->next_hop, path->next_hop, 16); result->action = RNS_NODE_FORWARD; return 1;
}
