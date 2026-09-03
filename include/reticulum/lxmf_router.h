#ifndef RETICULUM_LXMF_ROUTER_H
#define RETICULUM_LXMF_ROUTER_H

#include "reticulum/lxmf.h"
#include "reticulum/lxmf_store.h"
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
    LXMF_QUEUE_REASON_RETRY_BACKOFF
} lxmf_queue_reason_t;

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
    /* When supplied, opportunistic sends use a Reticulum packet receipt and
     * only become DELIVERED after a valid proof. `send_packet` remains the
     * compatibility transport for callers that do not own a runtime. */
    rns_runtime_t *runtime;
    lxmf_router_identity_resolver_fn resolve_identity;
    void *resolve_context;
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
} lxmf_router_config_t;
typedef struct {
    bool used;
    bool terminal;
    lxmf_router_t *router;
    rns_packet_receipt_t *receipt;
    uint8_t message_id[LXMF_MESSAGE_ID_LENGTH];
    lxmf_delivery_method_t method;
} lxmf_router_receipt_slot_t;

typedef struct {
    bool used;
    bool inbound;
    lxmf_router_t *router;
    rns_runtime_link_t *link;
    uint8_t destination[LXMF_DESTINATION_LENGTH];
} lxmf_router_link_slot_t;

struct lxmf_router {
    lxmf_router_config_t config;
    lxmf_router_receipt_slot_t receipts[LXMF_ROUTER_MAX_RECEIPTS];
    lxmf_router_link_slot_t links[LXMF_ROUTER_MAX_LINKS];
    rns_runtime_destination_t *inbound_destination;
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
/* Attempts queued or failed messages without blocking. Individual failures are
 * persisted in the store and counted in result; they do not fail the poll. */
lxmf_status_t lxmf_router_poll(lxmf_router_t *router, size_t max_messages,
                               lxmf_router_poll_result_t *result);
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
