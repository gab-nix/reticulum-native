#ifndef RETICULUM_RESOURCE_H
#define RETICULUM_RESOURCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "reticulum/link.h"
#include "reticulum/status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RNS_RESOURCE_HASH_SIZE 32u
#define RNS_RESOURCE_MAPHASH_LEN 4u
#define RNS_RESOURCE_RANDOM_HASH_SIZE 4u
#define RNS_RESOURCE_REQUEST_ID_SIZE 16u
#define RNS_RESOURCE_PROOF_SIZE (2u * RNS_RESOURCE_HASH_SIZE)
#define RNS_RESOURCE_WINDOW 4u
/* Pinned Reticulum receivers may grow a fast-link request window to 75 parts.
 * A stream carrier must be able to queue the complete requested burst. */
#define RNS_RESOURCE_WINDOW_MAX 75u
/* One advertisement carries floor((Link MDU - 134) / 4) map hashes. */
#define RNS_RESOURCE_HASHMAP_MAX_ENTRIES 74u
#define RNS_RESOURCE_MAX_PARTS 18000u
#define RNS_RESOURCE_WIRE_OVERHEAD 36u
#define RNS_RESOURCE_PART_MAX 464u
#define RNS_RESOURCE_DEFAULT_MAX_SIZE (8u * 1024u * 1024u)
#define RNS_RESOURCE_SINGLE_SEGMENT_MAX_SIZE ((1024u * 1024u) - 1u)
#define RNS_RESOURCE_MAX_SIZE (8u * 1024u * 1024u)

#define RNS_RESOURCE_FLAG_ENCRYPTED 0x01u
#define RNS_RESOURCE_FLAG_COMPRESSED 0x02u
#define RNS_RESOURCE_FLAG_SPLIT 0x04u
#define RNS_RESOURCE_FLAG_REQUEST 0x08u
#define RNS_RESOURCE_FLAG_RESPONSE 0x10u
#define RNS_RESOURCE_FLAG_METADATA 0x20u

/* A parsed RESOURCE_ADV. hashmap borrows the advertisement bytes. */
typedef struct rns_resource_advertisement {
    size_t transfer_size;
    size_t data_size;
    size_t parts;
    size_t segment_index;
    size_t total_segments;
    uint8_t hash[RNS_RESOURCE_HASH_SIZE];
    uint8_t original_hash[RNS_RESOURCE_HASH_SIZE];
    uint8_t random_hash[RNS_RESOURCE_RANDOM_HASH_SIZE];
    uint8_t request_id[RNS_RESOURCE_REQUEST_ID_SIZE];
    bool has_request_id;
    const uint8_t *hashmap;
    size_t hashmap_length;
    uint8_t flags;
    bool encrypted;
    bool compressed;
    bool split;
    bool is_request;
    bool is_response;
    bool has_metadata;
} rns_resource_advertisement_t;

typedef struct rns_resource rns_resource_t;
typedef struct rns_resource_sender rns_resource_sender_t;

typedef struct rns_resource_sender_options {
    bool auto_compress;
    bool is_response;
    const uint8_t *request_id;
} rns_resource_sender_options_t;

rns_status_t rns_resource_advertisement_parse(
    const uint8_t *msgpack, size_t length,
    rns_resource_advertisement_t *advertisement);

/*
 * Begins receiving an advertised resource. max_size bounds the assembled
 * plaintext; 0 selects RNS_RESOURCE_DEFAULT_MAX_SIZE. The advertisement is
 * copied, so it need not outlive this call.
 */
rns_status_t rns_resource_accept(rns_resource_t **resource,
                                 const rns_resource_advertisement_t *advertisement,
                                 size_t max_size);
void rns_resource_destroy(rns_resource_t *resource);

/* Builds the next RESOURCE_REQ body for outstanding parts in the window. */
rns_status_t rns_resource_build_request(rns_resource_t *resource,
                                        uint8_t *output, size_t capacity,
                                        size_t *output_length);
/* Accepts one RESOURCE part payload, matching it against the hashmap. */
rns_status_t rns_resource_receive_part(rns_resource_t *resource,
                                       const uint8_t *data, size_t length);
/* Applies a RESOURCE_HMU body: resource hash followed by MessagePack
 * [zero-based map page, map-hash bytes]. */
rns_status_t rns_resource_apply_hashmap_update(rns_resource_t *resource,
                                               const uint8_t *update,
                                               size_t update_length);
bool rns_resource_waiting_for_hashmap(const rns_resource_t *resource);
bool rns_resource_parts_complete(const rns_resource_t *resource);
size_t rns_resource_received_parts(const rns_resource_t *resource);
size_t rns_resource_total_parts(const rns_resource_t *resource);
const uint8_t *rns_resource_request_id(const rns_resource_t *resource);
bool rns_resource_is_response(const rns_resource_t *resource);

/*
 * Decrypts, decompresses and verifies the assembled resource. link supplies
 * the resource-level token key and may be NULL for unencrypted resources.
 * Returns RNS_ERROR_UNSUPPORTED when the payload needs a decompressor that
 * was not built in.
 */
rns_status_t rns_resource_assemble(rns_resource_t *resource, const rns_link *link,
                                   uint8_t *output, size_t capacity,
                                   size_t *output_length);
/* Valid only after a successful assemble. */
rns_status_t rns_resource_build_proof(const rns_resource_t *resource,
                                      uint8_t output[RNS_RESOURCE_PROOF_SIZE]);

/* Prepares one bounded, single-segment Resource using the pinned Reticulum
 * wire representation. The source bytes are copied, optionally compressed
 * when that reduces their size, prefixed with an independent random value and
 * encrypted once as a complete stream before part hashing. */
rns_status_t rns_resource_sender_create(
    rns_resource_sender_t **sender, const rns_link *link,
    const uint8_t *data, size_t data_length,
    const rns_resource_sender_options_t *options);
void rns_resource_sender_destroy(rns_resource_sender_t *sender);
rns_status_t rns_resource_sender_advertisement(
    const rns_resource_sender_t *sender, uint8_t *output, size_t capacity,
    size_t *output_length);
/* Parses one RESOURCE_REQ body and returns matching part indexes. Unknown map
 * hashes are ignored exactly as the reference sender does. */
rns_status_t rns_resource_sender_requested_parts(
    const rns_resource_sender_t *sender, const uint8_t *request,
    size_t request_length, size_t *part_indexes, size_t capacity,
    size_t *part_count);
/* Builds the RESOURCE_HMU body requested by an exhausted-map request. */
rns_status_t rns_resource_sender_hashmap_update(
    const rns_resource_sender_t *sender, const uint8_t *request,
    size_t request_length, uint8_t *output, size_t capacity,
    size_t *output_length);
rns_status_t rns_resource_sender_part(
    const rns_resource_sender_t *sender, size_t part_index,
    const uint8_t **data, size_t *length);
rns_status_t rns_resource_sender_validate_proof(
    const rns_resource_sender_t *sender, const uint8_t *proof,
    size_t proof_length);
const uint8_t *rns_resource_sender_hash(const rns_resource_sender_t *sender);
size_t rns_resource_sender_total_parts(const rns_resource_sender_t *sender);
size_t rns_resource_sender_data_size(const rns_resource_sender_t *sender);
size_t rns_resource_sender_transfer_size(const rns_resource_sender_t *sender);
size_t rns_resource_sender_segment_index(const rns_resource_sender_t *sender);
size_t rns_resource_sender_total_segments(const rns_resource_sender_t *sender);
size_t rns_resource_sender_total_data_parts(const rns_resource_sender_t *sender);
rns_status_t rns_resource_sender_advance_segment(rns_resource_sender_t *sender,
                                                 const rns_link *link);

/* True when this build can decompress bz2 resource payloads. */
bool rns_resource_decompression_available(void);

#ifdef __cplusplus
}
#endif
#endif
