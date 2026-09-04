#ifndef RETICULUM_TRANSPORT_H
#define RETICULUM_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RNS_TRANSPORT_HASH_SIZE 16u
#define RNS_TRANSPORT_PACKET_HASH_SIZE 32u
#define RNS_TRANSPORT_RANDOM_BLOB_SIZE 10u
#define RNS_TRANSPORT_MAX_RANDOM_BLOBS 16u
#define RNS_PATH_REQUEST_MAX_TAG_SIZE 16u
#define RNS_TRANSPORT_REVERSE_TIMEOUT 480.0
#define RNS_TRANSPORT_LINK_TIMEOUT 900.0
#define RNS_TRANSPORT_LINK_PROOF_TIMEOUT_PER_HOP 6.0
#define RNS_TRANSPORT_DEFAULT_LINK_CAPACITY 64u

typedef double (*rns_transport_clock)(void *context);

typedef enum {
    RNS_PATH_REJECTED = 0,
    RNS_PATH_INSERTED = 1,
    RNS_PATH_UPDATED = 2
} rns_path_result;

typedef enum {
    RNS_REVERSE_MISSING = 0,
    RNS_REVERSE_MATCHED = 1,
    RNS_REVERSE_WRONG_INTERFACE = 2
} rns_reverse_result;

typedef enum {
    RNS_LINK_ROUTE_MISSING = 0,
    RNS_LINK_ROUTE_MATCHED = 1,
    RNS_LINK_ROUTE_WRONG_INTERFACE = 2,
    RNS_LINK_ROUTE_WRONG_HOPS = 3,
    RNS_LINK_ROUTE_NOT_VALIDATED = 4,
    RNS_LINK_ROUTE_INVALID_PROOF = 5
} rns_link_route_result;

typedef struct {
    size_t path_capacity;
    size_t dedupe_capacity;
    size_t reverse_capacity;
    size_t link_capacity;
    size_t random_blob_history;
    double path_lifetime;
    double dedupe_lifetime;
    double reverse_lifetime;
    double link_lifetime;
    double link_proof_timeout_per_hop;
    rns_transport_clock clock;
    void *clock_context;
} rns_transport_config;

typedef struct {
    uint8_t destination_hash[16];
    uint8_t next_hop[16];
    uint8_t announce_packet_hash[32];
    uint8_t identity_public_key[64];
    uint8_t random_blobs[RNS_TRANSPORT_MAX_RANDOM_BLOBS]
                        [RNS_TRANSPORT_RANDOM_BLOB_SIZE];
    uint64_t announce_timebase;
    uint64_t interface_id;
    int32_t interface_gravity;
    uint8_t hops;
    size_t random_blob_count;
    int has_identity;
    int unresponsive;
    double updated_at;
    double expires_at;
    int occupied;
} rns_path_entry;

typedef struct {
    uint8_t packet_hash[32];
    double seen_at;
    double expires_at;
    int occupied;
} rns_dedupe_entry;

typedef struct {
    uint8_t packet_hash[16];
    uint64_t received_interface_id;
    uint64_t outbound_interface_id;
    double created_at;
    double expires_at;
    int occupied;
} rns_reverse_entry;

typedef struct {
    uint8_t link_id[16];
    uint8_t next_hop[16];
    uint8_t destination_hash[16];
    uint8_t destination_public_key[64];
    uint64_t next_hop_interface_id;
    uint64_t received_interface_id;
    uint8_t remaining_hops;
    uint8_t taken_hops;
    int validated;
    double created_at;
    double updated_at;
    double proof_deadline;
    double expires_at;
    int occupied;
} rns_transport_link_entry;

typedef enum {
    RNS_TRANSPORT_TRANSACTION_NONE = 0,
    RNS_TRANSPORT_TRANSACTION_REVERSE,
    RNS_TRANSPORT_TRANSACTION_LINK
} rns_transport_transaction_kind;

/* A single synchronous forwarding mutation that can be rolled back when the
 * selected interface rejects the encoded packet. Treat as opaque state: only
 * commit or roll it back against the transport that created it. */
typedef struct {
    rns_transport_transaction_kind kind;
    size_t slot;
    union {
        rns_reverse_entry reverse;
        rns_transport_link_entry link;
    } previous;
} rns_transport_transaction;

typedef struct {
    uint8_t destination_hash[16];
    uint8_t requesting_transport[16];
    uint8_t tag[RNS_PATH_REQUEST_MAX_TAG_SIZE];
    size_t tag_length;
    int has_requesting_transport;
} rns_path_request;

typedef struct {
    rns_transport_config config;
    rns_path_entry *paths;
    rns_dedupe_entry *dedupe;
    rns_reverse_entry *reverse_paths;
    rns_transport_link_entry *links;
} rns_transport;

int rns_transport_init(rns_transport *transport,
                       const rns_transport_config *config);
void rns_transport_free(rns_transport *transport);
size_t rns_transport_expire(rns_transport *transport);
const rns_path_entry *rns_transport_lookup(rns_transport *transport,
                                           const uint8_t destination_hash[16]);
int rns_transport_mark_unresponsive(rns_transport *transport,
                                    const uint8_t destination_hash[16]);
rns_path_result rns_transport_consider_announce(
    rns_transport *transport, const uint8_t destination_hash[16],
    const uint8_t next_hop[16], uint64_t interface_id,
    int32_t interface_gravity, uint8_t hops,
    const uint8_t random_blob[10], const uint8_t packet_hash[32]);
int rns_transport_accept_packet_hash(rns_transport *transport,
                                     const uint8_t packet_hash[32]);
int rns_transport_record_reverse(rns_transport *transport,
                                 const uint8_t packet_hash[32],
                                 uint64_t received_interface_id,
                                 uint64_t outbound_interface_id);
int rns_transport_record_reverse_transaction(
    rns_transport *transport, const uint8_t packet_hash[32],
    uint64_t received_interface_id, uint64_t outbound_interface_id,
    rns_transport_transaction *transaction);
rns_reverse_result rns_transport_consume_reverse(
    rns_transport *transport, const uint8_t packet_hash[16],
    uint64_t ingress_interface_id, uint64_t *forward_interface_id);
int rns_transport_set_path_identity(rns_transport *transport,
                                    const uint8_t destination_hash[16],
                                    const uint8_t public_key[64]);
int rns_transport_record_link_request(
    rns_transport *transport, const uint8_t link_id[16],
    const rns_path_entry *path, uint64_t received_interface_id,
    uint8_t taken_hops);
int rns_transport_record_link_request_transaction(
    rns_transport *transport, const uint8_t link_id[16],
    const rns_path_entry *path, uint64_t received_interface_id,
    uint8_t taken_hops, rns_transport_transaction *transaction);
const rns_transport_link_entry *rns_transport_link_lookup(
    rns_transport *transport, const uint8_t link_id[16]);
rns_link_route_result rns_transport_accept_link_proof(
    rns_transport *transport, const uint8_t link_id[16],
    const uint8_t *proof, size_t proof_length, uint64_t ingress_interface_id,
    uint8_t proof_hops, uint64_t *forward_interface_id);
rns_link_route_result rns_transport_accept_link_proof_transaction(
    rns_transport *transport, const uint8_t link_id[16],
    const uint8_t *proof, size_t proof_length, uint64_t ingress_interface_id,
    uint8_t proof_hops, uint64_t *forward_interface_id,
    rns_transport_transaction *transaction);
rns_link_route_result rns_transport_route_link(
    rns_transport *transport, const uint8_t link_id[16],
    uint64_t ingress_interface_id, uint8_t packet_hops,
    uint64_t *forward_interface_id);
rns_link_route_result rns_transport_route_link_transaction(
    rns_transport *transport, const uint8_t link_id[16],
    uint64_t ingress_interface_id, uint8_t packet_hops,
    uint64_t *forward_interface_id, rns_transport_transaction *transaction);
int rns_transport_forget_link(rns_transport *transport,
                              const uint8_t link_id[16]);
int rns_transport_transaction_rollback(
    rns_transport *transport, rns_transport_transaction *transaction);
void rns_transport_transaction_commit(
    rns_transport_transaction *transaction);

int rns_path_request_build(const uint8_t destination_hash[16],
                           const uint8_t requesting_transport[16],
                           const uint8_t *tag, size_t tag_length,
                           uint8_t *output, size_t output_capacity,
                           size_t *output_length);
int rns_path_request_parse(rns_path_request *request, const uint8_t *input,
                           size_t input_length);

#ifdef __cplusplus
}
#endif

#endif
