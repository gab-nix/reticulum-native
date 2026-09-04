#ifndef RETICULUM_LXMF_STORE_H
#define RETICULUM_LXMF_STORE_H

#include "reticulum/lxmf.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LXMF_STORE_MAX_CONTENT 4096u
#define LXMF_STORE_MAX_MESSAGES 1024u
#define LXMF_STORE_MAX_FILE_SIZE (16u * 1024u * 1024u)
#define LXMF_STORE_PATH_MAX 1023u
/* Full direct/propagated representations, including opaque extension fields.
 * This is a storage limit, not a promise that each transport supports it. */
#define LXMF_STORE_MAX_PACKED (8u * 1024u * 1024u)
/* Messages retained without a verifiable signature are untrusted input. Both
 * the total retained and the number of distinct unknown senders are capped;
 * admitting one past a cap evicts an already retained one. */
#define LXMF_STORE_MAX_UNVERIFIED 32u
#define LXMF_STORE_MAX_UNVERIFIED_SOURCES 8u

typedef enum {
    LXMF_DELIVERY_QUEUED = 0,
    LXMF_DELIVERY_SENDING = 1,
    LXMF_DELIVERY_SENT = 2,
    LXMF_DELIVERY_DELIVERED = 3,
    LXMF_DELIVERY_FAILED = 4
} lxmf_delivery_status_t;

typedef enum {
    LXMF_DELIVERY_METHOD_UNKNOWN = 0,
    LXMF_DELIVERY_METHOD_DIRECT,
    LXMF_DELIVERY_METHOD_OPPORTUNISTIC,
    LXMF_DELIVERY_METHOD_PROPAGATED
} lxmf_delivery_method_t;

typedef enum {
    LXMF_QUEUE_REASON_NONE = 0,
    LXMF_QUEUE_REASON_PEER_IDENTITY,
    LXMF_QUEUE_REASON_PATH,
    LXMF_QUEUE_REASON_STAMP,
    LXMF_QUEUE_REASON_LINK,
    LXMF_QUEUE_REASON_RESOURCE,
    LXMF_QUEUE_REASON_PROPAGATION_NODE,
    LXMF_QUEUE_REASON_RETRY_BACKOFF,
    /* Explicit user cancellation; automatic polling must not retry. */
    LXMF_QUEUE_REASON_CANCELLED,
    LXMF_QUEUE_REASON_RETRY_EXHAUSTED
} lxmf_queue_reason_t;

#define LXMF_DELIVERY_PROGRESS_COMPLETE 1000000u

typedef struct {
    lxmf_delivery_method_t desired_method;
    lxmf_delivery_method_t actual_method;
    uint32_t attempts;
    lxmf_queue_reason_t queue_reason;
    /* Absolute application-clock time for the next eligible retry. Zero means
     * no scheduled deadline has been assigned yet. */
    uint64_t retry_at_ms;
    /* Integer millionths avoid platform-dependent floating-point storage. */
    uint32_t progress;
    bool has_proof_id;
    uint8_t proof_id[LXMF_MESSAGE_ID_LENGTH];
} lxmf_delivery_metadata_t;

typedef struct {
    uint8_t message_id[LXMF_MESSAGE_ID_LENGTH];
    uint8_t destination[LXMF_DESTINATION_LENGTH];
    uint8_t source[LXMF_SOURCE_LENGTH];
    double timestamp;
    lxmf_delivery_status_t status;
    lxmf_slice_t content;
    /* Whether the sender's signature was checked when the message was stored. */
    lxmf_signature_state_t signature_state;
    /* The original packed LXMF message, borrowed. Supplied on put so an
     * unverified signature can be checked again once the sender's identity
     * arrives; never populated by read or list, which leave it empty. Read it
     * back with lxmf_store_read_packed. */
    lxmf_slice_t packed;
    lxmf_delivery_metadata_t delivery;
} lxmf_store_message_t;

typedef struct { void *implementation; } lxmf_store_t;
typedef bool (*lxmf_store_list_fn)(void *context,
                                   const lxmf_store_message_t *message);

lxmf_status_t lxmf_store_open(lxmf_store_t *store, const char *path);
void lxmf_store_close(lxmf_store_t *store);
lxmf_status_t lxmf_store_put(lxmf_store_t *store,
                             const lxmf_store_message_t *message,
                             bool *inserted);
lxmf_status_t lxmf_store_update_status(
    lxmf_store_t *store, const uint8_t message_id[LXMF_MESSAGE_ID_LENGTH],
    lxmf_delivery_status_t status);
/* Records a new signature state for an already stored message. */
lxmf_status_t lxmf_store_update_signature(
    lxmf_store_t *store, const uint8_t message_id[LXMF_MESSAGE_ID_LENGTH],
    lxmf_signature_state_t state);
/* Atomically appends the complete mutable delivery metadata for a message. */
lxmf_status_t lxmf_store_update_delivery(
    lxmf_store_t *store, const uint8_t message_id[LXMF_MESSAGE_ID_LENGTH],
    const lxmf_delivery_metadata_t *delivery);
/* Drops a message from the index. The bytes leave the file on the next
 * compaction. */
lxmf_status_t lxmf_store_remove(
    lxmf_store_t *store, const uint8_t message_id[LXMF_MESSAGE_ID_LENGTH]);
/* Reads back the retained packed message. LXMF_ERR_FORMAT when none was
 * stored, LXMF_ERR_BOUNDS when the capacity is too small. */
lxmf_status_t lxmf_store_read_packed(
    lxmf_store_t *store, const uint8_t message_id[LXMF_MESSAGE_ID_LENGTH],
    uint8_t *packed, size_t capacity, size_t *packed_len);
/* Query retained length before allocating a caller-owned buffer. No bytes are
 * read; returns LXMF_ERR_FORMAT when no representation exists. */
lxmf_status_t lxmf_store_packed_size(
    lxmf_store_t *store, const uint8_t message_id[LXMF_MESSAGE_ID_LENGTH],
    size_t *packed_len);
/* Durably replaces a retained representation without deleting the old record.
 * The computed message ID, source and destination must remain unchanged. */
lxmf_status_t lxmf_store_update_packed(
    lxmf_store_t *store, const uint8_t message_id[LXMF_MESSAGE_ID_LENGTH],
    const uint8_t *packed, size_t packed_len);
lxmf_status_t lxmf_store_read_delivery(
    lxmf_store_t *store, const uint8_t message_id[LXMF_MESSAGE_ID_LENGTH],
    lxmf_delivery_metadata_t *delivery);
lxmf_status_t lxmf_store_read(
    lxmf_store_t *store, const uint8_t message_id[LXMF_MESSAGE_ID_LENGTH],
    lxmf_store_message_t *message, uint8_t *content, size_t content_capacity);
lxmf_status_t lxmf_store_list(lxmf_store_t *store, lxmf_store_list_fn callback,
                              void *context);
/* Writes path.tmp, fsyncs it, and atomically renames it over path. */
lxmf_status_t lxmf_store_compact(lxmf_store_t *store);
size_t lxmf_store_count(const lxmf_store_t *store);
/* Messages retained with a signature state other than verified. */
size_t lxmf_store_unverified_count(const lxmf_store_t *store);

#ifdef __cplusplus
}
#endif
#endif
