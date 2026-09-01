#ifndef RETICULUM_TRANSPORT_H
#define RETICULUM_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#define RNS_TRANSPORT_HASH_SIZE 16u
#define RNS_TRANSPORT_PACKET_HASH_SIZE 32u
#define RNS_TRANSPORT_RANDOM_BLOB_SIZE 10u
#define RNS_TRANSPORT_MAX_RANDOM_BLOBS 16u
#define RNS_PATH_REQUEST_MAX_TAG_SIZE 16u

typedef double (*rns_monotonic_clock)(void *context);

typedef struct {
    size_t path_capacity;
    size_t dedupe_capacity;
    size_t random_blob_history;
    double path_lifetime;
    double dedupe_lifetime;
    rns_monotonic_clock clock;
    void *clock_context;
} rns_transport_config;

typedef struct {
    uint8_t destination_hash[16];
    uint8_t next_hop[16];
    uint8_t announce_packet_hash[32];
    uint8_t random_blobs[RNS_TRANSPORT_MAX_RANDOM_BLOBS][10];
    size_t random_blob_count;
    uint64_t announce_timebase;
    uint64_t interface_id;
    int32_t interface_gravity;
    uint8_t hops;
    int unresponsive;
    double updated_at;
    double expires_at;
    int occupied;
} rns_path_entry;

typedef struct {
    uint8_t hash[32];
    double seen_at;
    double expires_at;
    int occupied;
} rns_dedupe_entry;

typedef struct {
    rns_transport_config config;
    rns_path_entry *paths;
    rns_dedupe_entry *dedupe;
} rns_transport;

typedef enum {
    RNS_PATH_REJECTED = 0,
    RNS_PATH_INSERTED = 1,
    RNS_PATH_UPDATED = 2
} rns_path_update_result;

typedef struct {
    uint8_t destination_hash[16];
    uint8_t requesting_transport[16];
    uint8_t tag[16];
    size_t tag_length;
    int has_requesting_transport;
} rns_path_request;

int rns_transport_init(rns_transport *transport, const rns_transport_config *config);
void rns_transport_free(rns_transport *transport);
size_t rns_transport_expire(rns_transport *transport);
const rns_path_entry *rns_transport_lookup(rns_transport *transport, const uint8_t destination_hash[16]);
int rns_transport_mark_unresponsive(rns_transport *transport, const uint8_t destination_hash[16]);

rns_path_update_result rns_transport_consider_announce(
    rns_transport *transport, const uint8_t destination_hash[16],
    const uint8_t next_hop[16], uint64_t interface_id, int32_t interface_gravity,
    uint8_t hops, const uint8_t random_blob[10],
    const uint8_t announce_packet_hash[32]);

/* Returns 1 for a new hash (and records it), 0 for a live duplicate or error. */
int rns_transport_accept_packet_hash(rns_transport *transport, const uint8_t packet_hash[32]);

int rns_path_request_build(const uint8_t destination_hash[16],
                           const uint8_t requesting_transport[16],
                           const uint8_t *tag, size_t tag_length,
                           uint8_t *out, size_t capacity, size_t *out_length);
/* Presence of requesting_transport follows RNS framing: body lengths >32 include it. */
int rns_path_request_parse(rns_path_request *request, const uint8_t *body, size_t body_length);

#endif
