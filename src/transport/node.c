#include "reticulum/node.h"

#include "reticulum/announce.h"
#include "reticulum/link.h"
#include "reticulum/packet.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static void drop(rns_node_result *result, rns_node_reason reason) {
    result->action = RNS_NODE_DROP;
    result->reason = reason;
}

static int local_destination(const rns_node *node,
                             const uint8_t destination[16]) {
    for (size_t i = 0u; i < node->local_destination_count; ++i)
        if (memcmp(node->local_destinations + i * 16u, destination, 16u) == 0)
            return 1;
    return 0;
}

static int path_has_random_blob(const rns_path_entry *path,
                                const uint8_t random_blob[10]) {
    if (path == NULL || path->random_blob_count >
                            RNS_TRANSPORT_MAX_RANDOM_BLOBS)
        return 0;
    for (size_t i = 0u; i < path->random_blob_count; ++i)
        if (memcmp(path->random_blobs[i], random_blob, 10u) == 0) return 1;
    return 0;
}

int rns_node_init(rns_node *node, const rns_node_config *config) {
    if (node == NULL || config == NULL || config->max_hops == 0u ||
        config->local_destination_capacity == 0u ||
        config->local_destination_capacity > SIZE_MAX / 16u)
        return 0;
    memset(node, 0, sizeof *node);
    if (!rns_transport_init(&node->transport, &config->transport)) return 0;
    node->local_destinations = calloc(config->local_destination_capacity, 16u);
    if (node->local_destinations == NULL) {
        rns_transport_free(&node->transport);
        return 0;
    }
    memcpy(node->transport_id, config->transport_id, 16u);
    memcpy(node->path_request_destination, config->path_request_destination,
           16u);
    node->max_hops = config->max_hops;
    node->local_destination_capacity = config->local_destination_capacity;
    return 1;
}

void rns_node_free(rns_node *node) {
    if (node == NULL) return;
    rns_transport_free(&node->transport);
    free(node->local_destinations);
    memset(node, 0, sizeof *node);
}

int rns_node_register_destination(rns_node *node,
                                  const uint8_t destination_hash[16]) {
    if (node == NULL || destination_hash == NULL ||
        node->local_destinations == NULL)
        return 0;
    if (local_destination(node, destination_hash)) return 1;
    if (node->local_destination_count == node->local_destination_capacity)
        return 0;
    memcpy(node->local_destinations + node->local_destination_count * 16u,
           destination_hash, 16u);
    ++node->local_destination_count;
    return 1;
}

int rns_node_unregister_destination(rns_node *node,
                                    const uint8_t destination_hash[16]) {
    if (node == NULL || destination_hash == NULL ||
        node->local_destinations == NULL)
        return 0;
    for (size_t i = 0u; i < node->local_destination_count; ++i) {
        uint8_t *slot = node->local_destinations + i * 16u;
        if (memcmp(slot, destination_hash, 16u) != 0) continue;
        --node->local_destination_count;
        if (i != node->local_destination_count)
            memcpy(slot, node->local_destinations +
                             node->local_destination_count * 16u, 16u);
        memset(node->local_destinations + node->local_destination_count * 16u,
               0, 16u);
        return 1;
    }
    return 0;
}

static int encode_forward(const rns_packet *packet, uint8_t *output,
                          size_t output_capacity, rns_node_result *result) {
    if (output == NULL ||
        !rns_packet_encode(packet, output, output_capacity,
                           &result->output_length)) {
        result->output_length = 0u;
        drop(result, RNS_NODE_REASON_OUTPUT_TOO_SMALL);
        return 0;
    }
    result->hops = packet->hops;
    return 1;
}

static int handle_announce(rns_node *node, const rns_packet *packet,
                           const uint8_t packet_hash[32],
                           uint64_t received_interface_id,
                           int32_t interface_gravity, uint8_t *output,
                           size_t output_capacity, rns_node_result *result) {
    rns_announce announce;
    if (!rns_announce_parse(&announce, packet->data, packet->data_length,
                            packet->context_flag) ||
        !rns_announce_verify(packet->destination_hash, packet->data,
                             packet->data_length, packet->context_flag)) {
        drop(result, RNS_NODE_REASON_INVALID_ANNOUNCE);
        return 1;
    }
    int new_packet_hash =
        rns_transport_accept_packet_hash(&node->transport, packet_hash);
    if (!new_packet_hash) {
        const rns_path_entry *current = rns_transport_lookup(
            &node->transport, packet->destination_hash);
        int same_emission =
            path_has_random_blob(current, announce.random_blob);
        if (!same_emission ||
            (!current->unresponsive &&
             interface_gravity <= current->interface_gravity)) {
            drop(result, RNS_NODE_REASON_DUPLICATE);
            return 1;
        }
    }
    uint8_t next_hop[16] = {0};
    if (packet->header_type == RNS_PACKET_HEADER_2)
        memcpy(next_hop, packet->transport_id, 16u);
    else
        memcpy(next_hop, packet->destination_hash, 16u);
    rns_path_result update = rns_transport_consider_announce(
        &node->transport, packet->destination_hash, next_hop,
        received_interface_id, interface_gravity,
        (uint8_t)(packet->hops + 1u),
        announce.random_blob, packet_hash);
    if (update == RNS_PATH_REJECTED) {
        drop(result, new_packet_hash ? RNS_NODE_REASON_STALE_ANNOUNCE
                                     : RNS_NODE_REASON_DUPLICATE);
        return 1;
    }
    if (!rns_identity_from_public(&result->announce_identity,
                                  announce.public_key) ||
        !rns_transport_set_path_identity(&node->transport,
                                         packet->destination_hash,
                                         announce.public_key)) {
        drop(result, RNS_NODE_REASON_INVALID_ANNOUNCE);
        return 1;
    }
    result->has_verified_announce = 1;
    result->path_update = update;
    result->announce_timebase = announce.timestamp;
    result->announce_app_data = announce.app_data;
    result->announce_app_data_length = announce.app_data_length;
    result->announce_has_ratchet = announce.has_ratchet;
    result->announce_ratchet = announce.ratchet;
    rns_packet forwarded = *packet;
    forwarded.header_type = RNS_PACKET_HEADER_2;
    forwarded.transport_type = 1u;
    forwarded.hops = (uint8_t)(packet->hops + 1u);
    memcpy(forwarded.transport_id, node->transport_id, 16u);
    if (!encode_forward(&forwarded, output, output_capacity, result)) return 1;
    result->action = RNS_NODE_REBROADCAST;
    return 1;
}

static int handle_path_request(rns_node *node, const rns_packet *packet,
                               rns_node_result *result) {
    if (packet->context != RNS_NODE_PATH_REQUEST_CONTEXT ||
        !rns_path_request_parse(&result->path_request, packet->data,
                                packet->data_length)) {
        drop(result, RNS_NODE_REASON_BAD_PATH_REQUEST);
        return 1;
    }
    if (rns_transport_lookup(&node->transport,
                             result->path_request.destination_hash) == NULL) {
        drop(result, RNS_NODE_REASON_NO_PATH);
        return 1;
    }
    result->has_path_request = 1;
    result->action = RNS_NODE_PATH_RESPONSE;
    return 1;
}

static rns_node_reason link_reason(rns_link_route_result route) {
    switch (route) {
        case RNS_LINK_ROUTE_MISSING: return RNS_NODE_REASON_NO_LINK;
        case RNS_LINK_ROUTE_WRONG_INTERFACE:
            return RNS_NODE_REASON_WRONG_LINK_INTERFACE;
        case RNS_LINK_ROUTE_WRONG_HOPS:
            return RNS_NODE_REASON_WRONG_LINK_HOPS;
        case RNS_LINK_ROUTE_NOT_VALIDATED:
            return RNS_NODE_REASON_LINK_NOT_VALIDATED;
        case RNS_LINK_ROUTE_INVALID_PROOF:
            return RNS_NODE_REASON_INVALID_LINK_PROOF;
        case RNS_LINK_ROUTE_MATCHED: break;
    }
    return RNS_NODE_REASON_NONE;
}

static int handle_link_packet(rns_node *node, const rns_packet *packet,
                              uint64_t received_interface_id, uint8_t *output,
                              size_t output_capacity, rns_node_result *result) {
    int link_request_proof =
        packet->packet_type == 3u &&
        packet->context == RNS_NODE_LINK_REQUEST_PROOF_CONTEXT;
    if (link_request_proof && packet->data_length != RNS_LINK_PROOF_BYTES) {
        drop(result, RNS_NODE_REASON_INVALID_LINK_PROOF);
        return 1;
    }
    uint8_t forwarded_hops = (uint8_t)(packet->hops + 1u);
    rns_packet forwarded = *packet;
    forwarded.hops = forwarded_hops;
    uint8_t encoded[RNS_MTU];
    size_t encoded_length = 0u;
    if (output == NULL ||
        !rns_packet_encode(&forwarded, encoded, sizeof encoded,
                           &encoded_length) ||
        output_capacity < encoded_length) {
        drop(result, RNS_NODE_REASON_OUTPUT_TOO_SMALL);
        return 1;
    }
    uint64_t forward_interface = 0u;
    rns_link_route_result route;
    if (link_request_proof) {
        route = rns_transport_accept_link_proof_transaction(
            &node->transport, packet->destination_hash, packet->data,
            packet->data_length, received_interface_id, forwarded_hops,
            &forward_interface, &result->transport_transaction);
    } else {
        route = rns_transport_route_link_transaction(
            &node->transport, packet->destination_hash,
            received_interface_id, forwarded_hops, &forward_interface,
            &result->transport_transaction);
    }
    if (route != RNS_LINK_ROUTE_MATCHED) {
        drop(result, link_reason(route));
        return 1;
    }
    memcpy(output, encoded, encoded_length);
    result->output_length = encoded_length;
    result->hops = forwarded_hops;
    result->forward_interface_id = forward_interface;
    result->action = RNS_NODE_FORWARD;
    return 1;
}

static int ordinary_proof(const rns_packet *packet) {
    return packet->packet_type == 3u && packet->destination_type == 0u;
}

static int handle_ordinary_proof(rns_node *node, const rns_packet *packet,
                                 const uint8_t packet_hash[32],
                                 uint64_t received_interface_id,
                                 uint8_t *output, size_t output_capacity,
                                 rns_node_result *result) {
    if (packet->header_type != RNS_PACKET_HEADER_1 || packet->context != 0u ||
        (packet->data_length != 64u && packet->data_length != 96u)) {
        drop(result, RNS_NODE_REASON_BAD_PROOF);
        return 1;
    }
    rns_packet forwarded = *packet;
    forwarded.hops = (uint8_t)(packet->hops + 1u);
    uint8_t encoded[RNS_MTU];
    size_t encoded_length = 0u;
    if (output == NULL ||
        !rns_packet_encode(&forwarded, encoded, sizeof encoded,
                           &encoded_length) ||
        output_capacity < encoded_length) {
        drop(result, RNS_NODE_REASON_OUTPUT_TOO_SMALL);
        return 1;
    }
    if (!rns_transport_accept_packet_hash(&node->transport, packet_hash)) {
        drop(result, RNS_NODE_REASON_DUPLICATE);
        return 1;
    }
    uint64_t forward_interface = 0u;
    rns_reverse_result reverse = rns_transport_consume_reverse(
        &node->transport, packet->destination_hash, received_interface_id,
        &forward_interface);
    if (reverse == RNS_REVERSE_MISSING) {
        drop(result, RNS_NODE_REASON_NO_REVERSE_PATH);
        return 1;
    }
    if (reverse == RNS_REVERSE_WRONG_INTERFACE) {
        drop(result, RNS_NODE_REASON_WRONG_REVERSE_INTERFACE);
        return 1;
    }
    memcpy(output, encoded, encoded_length);
    result->output_length = encoded_length;
    result->hops = forwarded.hops;
    result->forward_interface_id = forward_interface;
    result->action = RNS_NODE_FORWARD;
    return 1;
}

static int handle_routed(rns_node *node, const rns_packet *packet,
                         const uint8_t packet_hash[32],
                         uint64_t received_interface_id, uint8_t *output,
                         size_t output_capacity, rns_node_result *result) {
    const rns_path_entry *path = rns_transport_lookup(
        &node->transport, packet->destination_hash);
    if (path == NULL) {
        drop(result, RNS_NODE_REASON_NO_PATH);
        return 1;
    }
    if (packet->packet_type == 2u &&
        (packet->data_length != RNS_LINK_REQUEST_BYTES ||
         !path->has_identity)) {
        drop(result, RNS_NODE_REASON_NO_PATH);
        return 1;
    }
    rns_packet forwarded = *packet;
    forwarded.hops = (uint8_t)(packet->hops + 1u);
    if (path->hops <= 1u) {
        forwarded.header_type = RNS_PACKET_HEADER_1;
        forwarded.transport_type = 0u;
        memset(forwarded.transport_id, 0, sizeof forwarded.transport_id);
    } else {
        forwarded.header_type = RNS_PACKET_HEADER_2;
        forwarded.transport_type = 1u;
        memcpy(forwarded.transport_id, path->next_hop, 16u);
    }
    if (!encode_forward(&forwarded, output, output_capacity, result)) return 1;
    result->forward_interface_id = path->interface_id;
    memcpy(result->next_hop, path->next_hop, 16u);
    if (packet->packet_type == 2u) {
        uint8_t link_id[16];
        if (!rns_link_id_from_request_packet(output, result->output_length,
                                             link_id) ||
            !rns_transport_record_link_request_transaction(
                &node->transport, link_id, path, received_interface_id,
                forwarded.hops, &result->transport_transaction)) {
            result->output_length = 0u;
            result->forward_interface_id = 0u;
            drop(result, RNS_NODE_REASON_NO_PATH);
            return 1;
        }
    } else if (!rns_transport_record_reverse_transaction(
                   &node->transport, packet_hash, received_interface_id,
                   path->interface_id, &result->transport_transaction)) {
        result->output_length = 0u;
        result->forward_interface_id = 0u;
        drop(result, RNS_NODE_REASON_NO_PATH);
        return 1;
    }
    result->action = RNS_NODE_FORWARD;
    return 1;
}

int rns_node_ingress(rns_node *node, const uint8_t *raw, size_t raw_length,
                     uint64_t received_interface_id, int32_t interface_gravity,
                     uint8_t *output, size_t output_capacity,
                     rns_node_result *result) {
    if (node == NULL || result == NULL) return 0;
    memset(result, 0, sizeof *result);
    result->received_interface_id = received_interface_id;
    result->received_at = node->transport.config.clock != NULL
                              ? node->transport.config.clock(
                                    node->transport.config.clock_context)
                              : 0.0;
    rns_packet packet;
    if (raw == NULL || raw_length > RNS_MTU ||
        !rns_packet_decode(&packet, raw, raw_length) ||
        !rns_packet_hash(raw, raw_length, result->packet_hash)) {
        drop(result, RNS_NODE_REASON_MALFORMED);
        return 1;
    }
    memcpy(result->destination_hash, packet.destination_hash, 16u);
    result->hops = packet.hops;
    if (packet.hops >= node->max_hops) {
        drop(result, RNS_NODE_REASON_MAX_HOPS);
        return 1;
    }

    if (ordinary_proof(&packet))
        return handle_ordinary_proof(node, &packet, result->packet_hash,
                                     received_interface_id, output,
                                     output_capacity, result);
    if (packet.packet_type == 1u)
        return handle_announce(node, &packet, result->packet_hash,
                               received_interface_id, interface_gravity,
                               output, output_capacity, result);
    if (local_destination(node, packet.destination_hash)) {
        if (!rns_transport_accept_packet_hash(&node->transport,
                                              result->packet_hash)) {
            drop(result, RNS_NODE_REASON_DUPLICATE);
            return 1;
        }
        result->action = RNS_NODE_DELIVER;
        return 1;
    }
    if (packet.destination_type == 3u)
        return handle_link_packet(node, &packet, received_interface_id,
                                  output, output_capacity, result);
    if (packet.header_type == RNS_PACKET_HEADER_2 &&
        memcmp(packet.transport_id, node->transport_id, 16u) != 0) {
        drop(result, RNS_NODE_REASON_NOT_FOR_US);
        return 1;
    }
    if (!rns_transport_accept_packet_hash(&node->transport,
                                          result->packet_hash)) {
        drop(result, RNS_NODE_REASON_DUPLICATE);
        return 1;
    }
    if (packet.destination_type == 2u &&
        memcmp(packet.destination_hash, node->path_request_destination,
               16u) == 0)
        return handle_path_request(node, &packet, result);
    return handle_routed(node, &packet, result->packet_hash,
                         received_interface_id, output, output_capacity,
                         result);
}

int rns_node_complete_forward(rns_node *node, rns_node_result *result,
                              int sent) {
    if (node == NULL || result == NULL || result->action != RNS_NODE_FORWARD)
        return 0;
    if (sent) {
        rns_transport_transaction_commit(&result->transport_transaction);
        return 1;
    }
    return rns_transport_transaction_rollback(
        &node->transport, &result->transport_transaction);
}
