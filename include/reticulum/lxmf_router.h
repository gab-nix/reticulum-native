#ifndef RETICULUM_LXMF_ROUTER_H
#define RETICULUM_LXMF_ROUTER_H

#include "reticulum/lxmf.h"
#include "reticulum/lxmf_paper.h"
#include "reticulum/lxmf_store.h"
#include "reticulum/lxmf_tickets.h"
#include "reticulum/lxmf_propagation_session.h"
#include "reticulum/ratchet_store.h"
#include "reticulum/runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LXMF_DISPLAY_NAME_MAX 127u
#define LXMF_IDENTITY_PUBLIC_LENGTH 64u
#define LXMF_FEATURE_COMPRESSION 0x00000001u
#define LXMF_ANNOUNCE_FUNCTIONS_MAX 32u
#define LXMF_ANNOUNCE_EXTENSIONS_MAX 256u
#define LXMF_ROUTER_MAX_RECEIPTS 16u
#define LXMF_ROUTER_MAX_LINKS 16u
#define LXMF_ROUTER_MAX_RESOURCES 8u
#define LXMF_ROUTER_PROPAGATION_MAX_RETRIES 32u
#define LXMF_ROUTER_PROPAGATION_MAX_RETRY_BASE_MS 86400000u
#define LXMF_ROUTER_MAX_PAPER_TRANSIENTS 32u

typedef struct {
    char display_name[LXMF_DISPLAY_NAME_MAX + 1u];
    size_t display_name_len;
    bool has_stamp_cost;
    uint8_t stamp_cost;
    uint32_t features;
    /* Exact supported-function identifiers, including identifiers this
     * implementation does not understand. */
    uint8_t supported_functions[LXMF_ANNOUNCE_FUNCTIONS_MAX];
    size_t supported_function_count;
    /* Concatenated MessagePack objects after the three standard fields. */
    uint8_t extensions[LXMF_ANNOUNCE_EXTENSIONS_MAX];
    size_t extensions_len;
    size_t extension_count;
} lxmf_announce_data_t;

lxmf_status_t lxmf_announce_encode(const lxmf_announce_data_t *data,
                                   uint8_t *output, size_t capacity,
                                   size_t *output_len);
lxmf_status_t lxmf_announce_parse(const uint8_t *input, size_t input_len,
                                  lxmf_announce_data_t *data);

typedef uint64_t (*lxmf_clock_fn)(void *context);

typedef struct {
    bool used;
    uint8_t delivery_hash[LXMF_DESTINATION_LENGTH];
    uint8_t identity_public[LXMF_IDENTITY_PUBLIC_LENGTH];
    char display_name[LXMF_DISPLAY_NAME_MAX + 1u];
    size_t display_name_len;
    bool has_stamp_cost;
    uint8_t stamp_cost;
    uint32_t features;
    uint64_t last_seen;
} lxmf_contact_t;

typedef struct {
    lxmf_contact_t *entries;
    size_t capacity;
    size_t count;
    lxmf_clock_fn clock;
    void *clock_context;
} lxmf_contact_book_t;

typedef const rns_identity *(*lxmf_router_identity_resolver_fn)(
    void *context, const uint8_t destination[LXMF_DESTINATION_LENGTH]);
typedef bool (*lxmf_router_ratchet_resolver_fn)(
    void *context, const uint8_t destination[LXMF_DESTINATION_LENGTH],
    uint8_t ratchet_public[RNS_RATCHET_PUBLIC_SIZE]);
/* Return a verified advertised cost (0 means disabled). False means no known
 * requirement; 255 is rejected. The callback must not block. */
typedef bool (*lxmf_router_stamp_cost_resolver_fn)(
    void *context, const uint8_t destination[LXMF_DESTINATION_LENGTH],
    uint8_t *cost);
/* Application-owned block preferences. The source is a claimed delivery hash
 * for unverified senders, so false is not proof of trust. Must not block or
 * retain the borrowed hash. Trust never implicitly disables stamp checks. */
typedef bool (*lxmf_router_source_blocked_fn)(
    void *context, const uint8_t source[LXMF_SOURCE_LENGTH]);
typedef lxmf_status_t (*lxmf_router_send_fn)(void *context,
                                             const uint8_t *packet,
                                             size_t packet_length);
/* Called after a durable delivery-state change performed by the router. */
typedef void (*lxmf_router_delivery_callback_fn)(
    void *context, const uint8_t message_id[LXMF_MESSAGE_ID_LENGTH],
    lxmf_delivery_status_t status, lxmf_status_t result);
/* The message and its borrowed slices are valid only for the callback. */
typedef void (*lxmf_router_message_callback_fn)(
    void *context, const lxmf_store_message_t *message);
/* Called when a retained message's signature state changes. A message reported
 * as LXMF_SIGNATURE_FAILED has already been dropped from the store. */
typedef void (*lxmf_router_signature_callback_fn)(
    void *context, const uint8_t message_id[LXMF_MESSAGE_ID_LENGTH],
    lxmf_signature_state_t state);

/* Privacy-safe delivery diagnostics. This deliberately contains no message
 * title, content, fields, key material or packet bytes. */
typedef struct {
    uint8_t message_id[LXMF_MESSAGE_ID_LENGTH];
    lxmf_delivery_method_t method;
    lxmf_delivery_status_t state;
    lxmf_queue_reason_t queue_reason;
    lxmf_status_t result;
    uint32_t attempt;
} lxmf_router_event_t;

typedef void (*lxmf_router_event_callback_fn)(
    void *context, const lxmf_router_event_t *event);

typedef struct lxmf_router lxmf_router_t;

typedef struct {
    rns_identity *identity;
    lxmf_store_t *store;
    /* Optional durable ticket store and caller-owned wall clock. The router
     * never assumes monotonic time is Unix time. A wall clock is required when
     * a ticket store is supplied. */
    lxmf_ticket_store_t *ticket_store;
    /* Optional local private-ratchet history for opportunistic receive. */
    rns_ratchet_store_t *ratchet_store;
    lxmf_clock_fn wall_clock;
    void *wall_clock_context;
    /* Zero disables inbound stamp enforcement. Costs 1..254 accept either a
     * valid issued-ticket stamp or a proof-of-work stamp. */
    uint8_t inbound_stamp_cost;
    lxmf_router_source_blocked_fn is_source_blocked;
    void *source_policy_context;
    /* Maximum packed LXMF size across packet, Resource and deferred delivery.
     * Zero uses the store bound; nonzero must fit within that bound. */
    size_t max_incoming_message_size;
    /* When supplied, opportunistic sends use a Reticulum packet receipt and
     * only become DELIVERED after a valid proof. `send_packet` remains the
     * compatibility transport for callers that do not own a runtime. */
    rns_runtime_t *runtime;
    lxmf_router_identity_resolver_fn resolve_identity;
    void *resolve_context;
    /* Optional verified peer-ratchet resolver for opportunistic sends. */
    lxmf_router_ratchet_resolver_fn resolve_ratchet;
    void *ratchet_context;
    lxmf_router_stamp_cost_resolver_fn resolve_stamp_cost;
    void *stamp_cost_context;
    /* Zero selects 64 bounded preparation/search units per send attempt. */
    uint32_t stamp_work_units;
    lxmf_router_send_fn send_packet;
    void *send_context;
    lxmf_router_delivery_callback_fn delivery_callback;
    void *delivery_context;
    lxmf_router_message_callback_fn message_callback;
    void *message_context;
    lxmf_router_signature_callback_fn signature_callback;
    void *signature_context;
    lxmf_router_event_callback_fn event_callback;
    void *event_context;
    /* UNKNOWN retains the legacy opportunistic default. Applications that
     * require NomadNet's default forward-secret delivery select DIRECT. */
    lxmf_delivery_method_t preferred_delivery_method;
    /* Register the local lxmf.delivery destination for authenticated inbound
     * links. The router owns this registration until destroy. */
    bool accept_inbound_links;
    /* Zero selects the current durable packed-message bound. */
    size_t max_incoming_resource_size;
    /* Zero selects the runtime Resource default. */
    double resource_timeout_seconds;
    /* Optional verified propagation node. Supplying an identity requires a
     * runtime, wall clock, matching lxmf.propagation hash and cost 1..254. */
    const rns_identity *propagation_node_identity;
    uint8_t propagation_node_destination[LXMF_DESTINATION_LENGTH];
    uint8_t propagation_stamp_cost;
    uint32_t propagation_retry_limit; /* Zero selects five attempts. */
    uint64_t propagation_retry_base_ms; /* Zero selects ten seconds. */
} lxmf_router_config_t;
typedef struct {
    bool used;
    bool terminal;
    lxmf_router_t *router;
    rns_packet_receipt_t *receipt;
    rns_runtime_link_t *link;
    uint8_t message_id[LXMF_MESSAGE_ID_LENGTH];
    lxmf_delivery_method_t method;
    uint32_t attempt;
} lxmf_router_receipt_slot_t;

typedef struct {
    bool used;
    bool inbound;
    lxmf_router_t *router;
    rns_runtime_link_t *link;
    uint8_t destination[LXMF_DESTINATION_LENGTH];
} lxmf_router_link_slot_t;

typedef struct {
    bool used;
    bool terminal;
    lxmf_router_t *router;
    rns_runtime_resource_transfer_t *transfer;
    rns_runtime_link_t *link;
    uint8_t message_id[LXMF_MESSAGE_ID_LENGTH];
    uint32_t attempt;
} lxmf_router_resource_slot_t;

typedef struct {
    bool used;
    uint8_t message_id[LXMF_MESSAGE_ID_LENGTH];
    uint8_t transient_id[LXMF_MESSAGE_ID_LENGTH];
    uint8_t *encrypted;
    size_t encrypted_length;
    size_t encrypted_capacity;
    lxmf_stamp_job_t *stamp_job;
    lxmf_pn_session_t *session;
    uint32_t attempt;
} lxmf_router_propagation_slot_t;

struct lxmf_router {
    lxmf_router_config_t config;
    lxmf_router_receipt_slot_t receipts[LXMF_ROUTER_MAX_RECEIPTS];
    lxmf_router_link_slot_t links[LXMF_ROUTER_MAX_LINKS];
    lxmf_router_resource_slot_t resources[LXMF_ROUTER_MAX_RESOURCES];
    rns_runtime_destination_t *inbound_destination;
    /* One active worker bounds CPU and memory regardless of queue length. */
    lxmf_stamp_job_t *stamp_job;
    uint8_t stamp_message_id[LXMF_MESSAGE_ID_LENGTH];
    uint8_t stamp_cost;
    lxmf_router_propagation_slot_t propagation;
    rns_identity propagation_node;
    /* A bounded process-local fast path for scanner repeats. Durable replay
     * suppression remains keyed by the LXMF message ID in the message store. */
    uint8_t paper_transient_ids[LXMF_ROUTER_MAX_PAPER_TRANSIENTS]
                               [LXMF_MESSAGE_ID_LENGTH];
    uint8_t paper_message_ids[LXMF_ROUTER_MAX_PAPER_TRANSIENTS]
                             [LXMF_MESSAGE_ID_LENGTH];
    size_t paper_transient_count;
    size_t next_paper_transient;
};

typedef struct {
    size_t attempted;
    size_t sent;
    size_t failed;
    size_t deferred;
} lxmf_router_poll_result_t;

lxmf_status_t lxmf_router_init(lxmf_router_t *router,
                               const lxmf_router_config_t *config);
void lxmf_router_destroy(lxmf_router_t *router);
lxmf_status_t lxmf_router_send_message(lxmf_router_t *router,
                                       const uint8_t message_id[LXMF_MESSAGE_ID_LENGTH]);
/* Cancels an active stamp worker, packet receipt or Resource transfer. */
lxmf_status_t lxmf_router_cancel_message(
    lxmf_router_t *router,
    const uint8_t message_id[LXMF_MESSAGE_ID_LENGTH]);
/* Privacy-safe detailed progress for the active stamp job. PENDING means the
 * message does not currently own the worker. */
lxmf_status_t lxmf_router_stamp_progress(
    const lxmf_router_t *router,
    const uint8_t message_id[LXMF_MESSAGE_ID_LENGTH],
    lxmf_stamp_job_progress_t *progress);
/* Attempts queued or failed messages without blocking. Individual failures are
 * persisted in the store and counted in result; they do not fail the poll. */
lxmf_status_t lxmf_router_poll(lxmf_router_t *router, size_t max_messages,
                               lxmf_router_poll_result_t *result);
/* Updates the enforced inbound policy without rebuilding runtime links. */
lxmf_status_t lxmf_router_set_inbound_stamp_cost(lxmf_router_t *router,
                                                  uint8_t cost);
/* Atomically selects a verified outbound lxmf.propagation node. The router
 * copies only its public identity. Passing NULL/NULL/zero clears the node;
 * changing it while an upload is active returns LXMF_ERR_PENDING. */
lxmf_status_t lxmf_router_set_propagation_node(
    lxmf_router_t *router, const rns_identity *identity,
    const uint8_t destination[LXMF_DESTINATION_LENGTH], uint8_t stamp_cost);
/* Accepts a single encrypted opportunistic LXMF packet for the local identity.
 * Valid new messages are persisted as DELIVERED before message_callback runs.
 * A message signed by an identity the resolver does not yet hold cannot be
 * judged, so it is retained with LXMF_SIGNATURE_UNVERIFIED, bounded by the
 * store's retention caps, and reported like any other message. A message whose
 * signature fails against an identity the resolver does hold is forged and is
 * rejected with LXMF_ERR_SIGNATURE. */
lxmf_status_t lxmf_router_receive_packet(lxmf_router_t *router,
                                         const uint8_t *packet,
                                         size_t packet_length);

typedef struct {
    uint8_t transient_id[LXMF_MESSAGE_ID_LENGTH];
    uint8_t message_id[LXMF_MESSAGE_ID_LENGTH];
    uint8_t ratchet_id[RNS_RATCHET_ID_SIZE];
    bool used_ratchet;
    bool duplicate;
} lxmf_router_paper_result_t;

/* Imports the encrypted bytes carried by a paper message. Paper delivery is
 * recorded as propagated delivery, matching pinned LXMF 1.1.0 receive
 * semantics, but is exempt from inbound stamp cost enforcement. Blocking,
 * destination, size and signature policy are unchanged. When no private keys
 * are supplied, the router copies its configured ratchet-store history. */
lxmf_status_t lxmf_router_receive_paper(
    lxmf_router_t *router, const uint8_t *paper, size_t paper_length,
    const uint8_t *ratchet_private_keys, size_t ratchet_count,
    bool enforce_ratchets, lxmf_router_paper_result_t *result);

/* Decodes a bounded lxm:// URI and imports it through receive_paper. */
lxmf_status_t lxmf_router_receive_uri(
    lxmf_router_t *router, const char *uri, size_t uri_length,
    const uint8_t *ratchet_private_keys, size_t ratchet_count,
    bool enforce_ratchets, lxmf_router_paper_result_t *result);

typedef struct {
    size_t examined;
    size_t verified;
    size_t rejected;
    size_t pending;
} lxmf_router_verify_result_t;

/* Re-checks retained unverified messages, which callers invoke once an announce
 * has taught the resolver a new identity. A NULL source examines every retained
 * unverified message; otherwise only those from that source. Messages that now
 * verify are promoted durably to LXMF_SIGNATURE_VERIFIED; messages the now known
 * identity did not sign are dropped from the store. Messages whose signer is
 * still unknown are left retained and counted in pending. */
lxmf_status_t lxmf_router_verify_pending(lxmf_router_t *router,
                                         const uint8_t source[LXMF_SOURCE_LENGTH],
                                         lxmf_router_verify_result_t *result);

const char *lxmf_delivery_method_string(lxmf_delivery_method_t method);
const char *lxmf_queue_reason_string(lxmf_queue_reason_t reason);

lxmf_status_t lxmf_contact_book_init(lxmf_contact_book_t *book,
                                     lxmf_contact_t *storage, size_t capacity,
                                     lxmf_clock_fn clock, void *clock_context);
lxmf_status_t lxmf_contact_book_update(
    lxmf_contact_book_t *book,
    const uint8_t delivery_hash[LXMF_DESTINATION_LENGTH],
    const uint8_t identity_public[LXMF_IDENTITY_PUBLIC_LENGTH],
    const lxmf_announce_data_t *announce);
const lxmf_contact_t *lxmf_contact_book_lookup(
    const lxmf_contact_book_t *book,
    const uint8_t delivery_hash[LXMF_DESTINATION_LENGTH]);
size_t lxmf_contact_book_expire(lxmf_contact_book_t *book,
                                uint64_t max_age);

#ifdef __cplusplus
}
#endif
#endif
