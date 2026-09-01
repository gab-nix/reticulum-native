#include "reticulum/node.h"

#include "reticulum/announce.h"
#include "reticulum/destination.h"
#include "reticulum/identity.h"
#include "reticulum/packet.h"

#include <assert.h>
#include <string.h>

static double clock_now(void *context) { return *(double *)context; }
static void fill(uint8_t *p, size_t n, uint8_t v) { memset(p, v, n); }

int main(void) {
    double now = 10; rns_node node; rns_node_config config = {0}; rns_identity identity;
    uint8_t private_key[64], destination[16], name_hash[10], random_prefix[5] = {1,2,3,4,5};
    uint8_t announce_body[465], raw[500], output[500], context_flag, path_body[48], tag[4] = {7,8,9,10};
    size_t announce_length, raw_length, path_length; rns_packet packet = {0}, decoded; rns_node_result result;
    const char *aspects[] = {"delivery"};
    config.transport.path_capacity = 4; config.transport.dedupe_capacity = 8;
    config.transport.random_blob_history = 4; config.transport.path_lifetime = 60;
    config.transport.dedupe_lifetime = 10; config.transport.clock = clock_now; config.transport.clock_context = &now;
    fill(config.transport_id, 16, 0x77); fill(config.path_request_destination, 16, 0x88);
    config.max_hops = 128; config.local_destination_capacity = 2;
    assert(rns_node_init(&node, &config));
    for (size_t i = 0; i < sizeof(private_key); ++i) private_key[i] = (uint8_t)(i + 1);
    assert(rns_identity_from_private(&identity, private_key));
    assert(rns_destination_name_hash("lxmf", aspects, 1, name_hash));
    assert(rns_destination_hash(&identity, "lxmf", aspects, 1, destination));
    assert(rns_announce_build(&identity, destination, name_hash, random_prefix, 1234, NULL, NULL, 0,
                              announce_body, sizeof(announce_body), &announce_length, &context_flag));
    packet.header_type = 0; packet.destination_type = 0; packet.packet_type = 1; packet.context_flag = context_flag;
    memcpy(packet.destination_hash, destination, 16); packet.data = announce_body; packet.data_length = announce_length;
    assert(rns_packet_encode(&packet, raw, sizeof(raw), &raw_length));
    assert(rns_node_ingress(&node, raw, raw_length, 42, 1, output, sizeof(output), &result));
    assert(result.action == RNS_NODE_REBROADCAST && result.hops == 1 && result.output_length > raw_length);
    assert(rns_packet_decode(&decoded, output, result.output_length));
    assert(decoded.header_type == 1 && decoded.transport_type == 1 && decoded.hops == 1);
    assert(memcmp(decoded.transport_id, config.transport_id, 16) == 0);
    assert(rns_node_ingress(&node, raw, raw_length, 42, 1, output, sizeof(output), &result));
    assert(result.action == RNS_NODE_DROP && result.reason == RNS_NODE_REASON_DUPLICATE);

    /* A new header-1 data packet is routed using the learned destination path. */
    packet.packet_type = 0; packet.context_flag = 0; packet.data = (const uint8_t *)"hello"; packet.data_length = 5;
    assert(rns_packet_encode(&packet, raw, sizeof(raw), &raw_length));
    assert(rns_node_ingress(&node, raw, raw_length, 42, 1, output, sizeof(output), &result));
    assert(result.action == RNS_NODE_FORWARD && rns_packet_decode(&decoded, output, result.output_length));
    assert(decoded.header_type == 1 && decoded.hops == 1 && memcmp(decoded.transport_id, destination, 16) == 0);

    /* Header-2 packets addressed to another transport are not forwarded. */
    packet.header_type = 1; packet.transport_type = 1; fill(packet.transport_id, 16, 0x66);
    packet.data = (const uint8_t *)"other"; packet.data_length = 5;
    assert(rns_packet_encode(&packet, raw, sizeof(raw), &raw_length));
    assert(rns_node_ingress(&node, raw, raw_length, 42, 1, output, sizeof(output), &result));
    assert(result.action == RNS_NODE_DROP && result.reason == RNS_NODE_REASON_NOT_FOR_US);

    /* Local registration takes precedence over forwarding. */
    assert(rns_node_register_destination(&node, destination)); packet.header_type = 0; packet.transport_type = 0;
    packet.data = (const uint8_t *)"local"; packet.data_length = 5;
    assert(rns_packet_encode(&packet, raw, sizeof(raw), &raw_length));
    assert(rns_node_ingress(&node, raw, raw_length, 42, 1, NULL, 0, &result)); assert(result.action == RNS_NODE_DELIVER);
    assert(rns_node_unregister_destination(&node, destination));

    /* A path request for the learned route produces response metadata. */
    assert(rns_path_request_build(destination, NULL, tag, sizeof(tag), path_body, sizeof(path_body), &path_length));
    memset(&packet, 0, sizeof(packet)); packet.destination_type = 2; memcpy(packet.destination_hash, config.path_request_destination, 16);
    packet.data = path_body; packet.data_length = path_length;
    assert(rns_packet_encode(&packet, raw, sizeof(raw), &raw_length));
    assert(rns_node_ingress(&node, raw, raw_length, 42, 1, NULL, 0, &result));
    assert(result.action == RNS_NODE_PATH_RESPONSE && result.has_path_request && result.path_request.tag_length == 4);

    /* Hop ceiling and malformed announce rejection. */
    packet.hops = config.max_hops; packet.data = (const uint8_t *)"hop"; packet.data_length = 3;
    assert(rns_packet_encode(&packet, raw, sizeof(raw), &raw_length));
    assert(rns_node_ingress(&node, raw, raw_length, 42, 1, output, sizeof(output), &result)); assert(result.reason == RNS_NODE_REASON_MAX_HOPS);
    now += 11; memset(&packet, 0, sizeof(packet)); packet.packet_type = 1; memcpy(packet.destination_hash, destination, 16);
    packet.data = (const uint8_t *)"bad"; packet.data_length = 3; assert(rns_packet_encode(&packet, raw, sizeof(raw), &raw_length));
    assert(rns_node_ingress(&node, raw, raw_length, 42, 1, output, sizeof(output), &result)); assert(result.reason == RNS_NODE_REASON_INVALID_ANNOUNCE);
    rns_node_free(&node); return 0;
}
