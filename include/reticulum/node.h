#ifndef RETICULUM_NODE_H
#define RETICULUM_NODE_H

#include <stddef.h>
#include <stdint.h>

#include "reticulum/transport.h"

#define RNS_NODE_PATH_REQUEST_CONTEXT 0x00u
#define RNS_NODE_PATH_RESPONSE_CONTEXT 0x0bu

typedef enum {
    RNS_NODE_DROP = 0,
    RNS_NODE_DELIVER = 1,
    RNS_NODE_FORWARD = 2,
    RNS_NODE_REBROADCAST = 3,
    RNS_NODE_PATH_RESPONSE = 4
} rns_node_action;

typedef enum {
    RNS_NODE_REASON_NONE = 0,
    RNS_NODE_REASON_MALFORMED,
    RNS_NODE_REASON_DUPLICATE,
    RNS_NODE_REASON_MAX_HOPS,
    RNS_NODE_REASON_INVALID_ANNOUNCE,
    RNS_NODE_REASON_STALE_ANNOUNCE,
    RNS_NODE_REASON_NOT_FOR_US,
    RNS_NODE_REASON_NO_PATH,
    RNS_NODE_REASON_BAD_PATH_REQUEST,
    RNS_NODE_REASON_OUTPUT_TOO_SMALL
} rns_node_reason;

typedef struct {
    rns_transport_config transport;
    uint8_t transport_id[16];
    uint8_t path_request_destination[16];
    uint8_t max_hops;
    size_t local_destination_capacity;
} rns_node_config;

typedef struct {
    rns_transport transport;
    uint8_t transport_id[16];
    uint8_t path_request_destination[16];
    uint8_t max_hops;
    uint8_t *local_destinations;
    size_t local_destination_capacity;
    size_t local_destination_count;
} rns_node;

typedef struct {
    rns_node_action action;
    rns_node_reason reason;
    uint8_t packet_hash[32];
    uint8_t destination_hash[16];
    uint8_t next_hop[16];
    uint8_t hops;
    size_t output_length;
    rns_path_request path_request;
    int has_path_request;
} rns_node_result;

int rns_node_init(rns_node *node, const rns_node_config *config);
void rns_node_free(rns_node *node);
int rns_node_register_destination(rns_node *node, const uint8_t destination_hash[16]);
int rns_node_unregister_destination(rns_node *node, const uint8_t destination_hash[16]);

/* interface metadata is associated with paths learned from verified announces. */
int rns_node_ingress(rns_node *node, const uint8_t *raw, size_t raw_length,
                     uint64_t interface_id, int32_t interface_gravity,
                     uint8_t *output, size_t output_capacity,
                     rns_node_result *result);

#endif
