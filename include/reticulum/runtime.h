#ifndef RETICULUM_RUNTIME_H
#define RETICULUM_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "reticulum/config.h"
#include "reticulum/identity.h"
#include "reticulum/link.h"
#include "reticulum/node.h"
#include "reticulum/resource.h"
#include "reticulum/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rns_runtime rns_runtime_t;
typedef struct rns_runtime_link rns_runtime_link_t;
typedef struct rns_runtime_destination rns_runtime_destination_t;
typedef struct rns_request_receipt rns_request_receipt_t;
typedef struct rns_packet_receipt rns_packet_receipt_t;
typedef struct rns_runtime_resource_transfer rns_runtime_resource_transfer_t;

#define RNS_RUNTIME_MAX_LINKS 16u
#define RNS_RUNTIME_MAX_DESTINATIONS 32u
#define RNS_RUNTIME_MAX_REQUESTS 8u
#define RNS_RUNTIME_MAX_PACKET_RECEIPTS 64u
#define RNS_REQUEST_DEFAULT_MAX_RESPONSE (8u * 1024u * 1024u)
/* Resource transfer contexts, advertised by peers that answer with a Resource. */
#define RNS_LINK_CONTEXT_RESOURCE 0x01u
#define RNS_LINK_CONTEXT_RESOURCE_ADV 0x02u
#define RNS_LINK_CONTEXT_RESOURCE_REQ 0x03u
#define RNS_LINK_CONTEXT_RESOURCE_PRF 0x05u
#define RNS_LINK_CONTEXT_RESOURCE_ICL 0x06u
#define RNS_LINK_CONTEXT_RESOURCE_RCL 0x07u
#define RNS_LINK_CONTEXT_CACHE_REQUEST 0x08u
#define RNS_LINK_CONTEXT_REQUEST 0x09u
#define RNS_LINK_CONTEXT_RESPONSE 0x0au
#define RNS_LINK_CONTEXT_KEEPALIVE 0xfau
#define RNS_LINK_CONTEXT_IDENTIFY 0xfbu
#define RNS_LINK_CONTEXT_CLOSE 0xfcu
#define RNS_LINK_CONTEXT_RTT 0xfeu
#define RNS_LINK_CONTEXT_PROOF 0xffu

typedef void (*rns_runtime_link_state_callback_t)(
    rns_runtime_link_t *link, rns_link_state state, rns_status_t reason,
    void *context);
typedef void (*rns_runtime_link_packet_callback_t)(
    rns_runtime_link_t *link, uint8_t packet_context,
    const uint8_t *plaintext, size_t plaintext_length, void *context);
typedef void (*rns_runtime_link_identified_callback_t)(
    rns_runtime_link_t *link, const rns_identity *remote_identity,
    void *context);
typedef bool (*rns_runtime_resource_accept_callback_t)(
    rns_runtime_link_t *link,
    const rns_resource_advertisement_t *advertisement, void *context);
/* Advertisement and completed data spans are immutable and callback-scoped. */
typedef void (*rns_runtime_resource_receive_callback_t)(
    rns_runtime_link_t *link, const uint8_t resource_hash[32],
    rns_status_t status, const uint8_t *data, size_t data_length,
    void *context);

typedef struct rns_runtime_link_options {
    double timeout_seconds;
    uint32_t mtu;
    /* Emit an explicit link-key proof before dispatching each authenticated
     * context-NONE application packet. LXMF delivery destinations use this to
     * match the upstream direct-delivery contract. */
    bool prove_data_packets;
    rns_runtime_link_state_callback_t state_callback;
    rns_runtime_link_packet_callback_t packet_callback;
    rns_runtime_link_identified_callback_t identified_callback;
    /* Application resources are accepted only when this callback returns
     * true. Request-response resources retain their receipt-specific policy. */
    rns_runtime_resource_accept_callback_t resource_accept_callback;
    rns_runtime_resource_receive_callback_t resource_receive_callback;
    size_t max_incoming_resource_size;
    void *callback_context;
} rns_runtime_link_options_t;

typedef enum rns_runtime_resource_state {
    RNS_RUNTIME_RESOURCE_ADVERTISED = 0,
    RNS_RUNTIME_RESOURCE_TRANSFERRING,
    RNS_RUNTIME_RESOURCE_COMPLETE,
    RNS_RUNTIME_RESOURCE_REJECTED,
    RNS_RUNTIME_RESOURCE_FAILED,
    RNS_RUNTIME_RESOURCE_CANCELLED
} rns_runtime_resource_state_t;

typedef void (*rns_runtime_resource_state_callback_t)(
    rns_runtime_resource_transfer_t *transfer,
    rns_runtime_resource_state_t state, rns_status_t status,
    size_t transferred_parts, size_t total_parts, void *context);

typedef struct rns_runtime_resource_options {
    double timeout_seconds;
    bool auto_compress;
    bool is_response;
    const uint8_t *request_id;
    rns_runtime_resource_state_callback_t callback;
    void *callback_context;
} rns_runtime_resource_options_t;

/* Called synchronously from rns_runtime_poll() after a structurally valid link
 * request has been accepted and its signed LRPROOF has been transmitted. The
 * link is in RNS_LINK_HANDSHAKE until its encrypted RTT confirmation arrives.
 * The accepted link is runtime-attached and caller-owned. */
typedef void (*rns_runtime_inbound_link_callback_t)(
    rns_runtime_destination_t *destination, rns_runtime_link_t *link,
    void *context);

typedef enum rns_request_state {
    RNS_REQUEST_PENDING = 0,
    RNS_REQUEST_COMPLETE,
    RNS_REQUEST_FAILED,
    RNS_REQUEST_CANCELLED
} rns_request_state_t;

typedef void (*rns_request_callback_t)(
    rns_request_receipt_t *receipt, rns_request_state_t state,
    rns_status_t status, const uint8_t *response, size_t response_length,
    void *context);

typedef struct rns_request_options {
    double timeout_seconds;
    size_t max_response_size;
    rns_request_callback_t callback;
    void *callback_context;
} rns_request_options_t;

typedef enum rns_packet_receipt_state {
    RNS_PACKET_RECEIPT_PENDING = 0,
    RNS_PACKET_RECEIPT_DELIVERED,
    RNS_PACKET_RECEIPT_FAILED,
    RNS_PACKET_RECEIPT_CANCELLED
} rns_packet_receipt_state_t;

typedef void (*rns_packet_receipt_callback_t)(
    rns_packet_receipt_t *receipt, rns_packet_receipt_state_t state,
    rns_status_t status, void *context);

typedef struct rns_packet_receipt_options {
    double timeout_seconds;
    rns_packet_receipt_callback_t callback;
    void *callback_context;
} rns_packet_receipt_options_t;

typedef enum rns_runtime_interface_state {
    RNS_RUNTIME_INTERFACE_DISABLED = 0,
    RNS_RUNTIME_INTERFACE_STARTING,
    RNS_RUNTIME_INTERFACE_UP,
    RNS_RUNTIME_INTERFACE_DOWN,
    RNS_RUNTIME_INTERFACE_UNSUPPORTED
} rns_runtime_interface_state_t;

typedef struct rns_runtime_interface_info {
    uint64_t id;
    char name[RNS_CONFIG_NAME_MAX];
    rns_config_interface_type_t type;
    rns_runtime_interface_state_t state;
    rns_status_t last_error;
    uint64_t packets_received;
    uint64_t packets_sent;
    uint64_t bytes_received;
    uint64_t bytes_sent;
    uint64_t packets_dropped;
} rns_runtime_interface_info_t;

typedef void (*rns_runtime_packet_callback_t)(rns_runtime_t *runtime,
                                               const uint8_t *packet,
                                               size_t packet_length,
                                               const rns_node_result *result,
                                               void *context);
typedef void (*rns_runtime_announce_callback_t)(rns_runtime_t *runtime,
                                                const rns_node_result *announce,
                                                void *context);

typedef struct rns_runtime_options {
    rns_runtime_packet_callback_t packet_callback;
    rns_runtime_announce_callback_t announce_callback;
    void *callback_context;
    size_t path_capacity;
    size_t dedupe_capacity;
    size_t local_destination_capacity;
} rns_runtime_options_t;

rns_status_t rns_runtime_create(rns_runtime_t **runtime,
                                const rns_config_t *config,
                                const rns_runtime_options_t *options);
void rns_runtime_destroy(rns_runtime_t *runtime);

/* Performs bounded non-blocking work. max_packets == 0 uses a bounded default. */
rns_status_t rns_runtime_poll(rns_runtime_t *runtime,
                              size_t max_packets,
                              size_t *packets_processed);
rns_status_t rns_runtime_send(rns_runtime_t *runtime,
                              size_t interface_index,
                              const uint8_t *packet,
                              size_t packet_length);
/*
 * Sends a packet towards its own destination, using the learned path. A
 * destination more than one hop away is re-addressed to its next hop with a
 * Header 2 transport header, which is what makes transport nodes forward it.
 * Returns RNS_ERROR_NOT_FOUND when no path is known.
 */
rns_status_t rns_runtime_send_routed(rns_runtime_t *runtime,
                                     const uint8_t *packet,
                                     size_t packet_length);
/* Sends a routed packet and tracks the explicit or implicit Reticulum proof
 * made by destination_identity. The receipt remains caller-owned until
 * destroyed, or until its runtime is destroyed. */
rns_status_t rns_runtime_send_routed_with_receipt(
    rns_runtime_t *runtime, const uint8_t *packet, size_t packet_length,
    const rns_identity *destination_identity,
    const rns_packet_receipt_options_t *options,
    rns_packet_receipt_t **receipt);
/* Emits a proof for an immutable packet result received by a runtime callback,
 * on the same interface. `explicit_proof` selects hash+signature instead of
 * the 64-byte implicit signature form. */
rns_status_t rns_runtime_prove_packet(rns_runtime_t *runtime,
                                      const rns_node_result *received,
                                      const rns_identity *identity,
                                      bool explicit_proof);
rns_packet_receipt_state_t rns_packet_receipt_state(
    const rns_packet_receipt_t *receipt);
const uint8_t *rns_packet_receipt_hash(const rns_packet_receipt_t *receipt);
double rns_packet_receipt_rtt(const rns_packet_receipt_t *receipt);
void rns_packet_receipt_cancel(rns_packet_receipt_t *receipt);
void rns_packet_receipt_destroy(rns_packet_receipt_t *receipt);
/*
 * Broadcasts a signed announce for the destination derived from identity and
 * the given aspects, so peers can discover and route back to it.
 */
rns_status_t rns_runtime_announce(rns_runtime_t *runtime,
                                  const rns_identity *identity,
                                  const char *app_name,
                                  const char *const *aspects,
                                  size_t aspect_count,
                                  const uint8_t *app_data,
                                  size_t app_data_length);

size_t rns_runtime_interface_count(const rns_runtime_t *runtime);
rns_status_t rns_runtime_interface_info(const rns_runtime_t *runtime,
                                        size_t interface_index,
                                        rns_runtime_interface_info_t *info);
rns_status_t rns_runtime_register_destination(rns_runtime_t *runtime,
                                              const uint8_t destination_hash[16]);
rns_status_t rns_runtime_unregister_destination(rns_runtime_t *runtime,
                                                const uint8_t destination_hash[16]);
/* Registers a SINGLE destination capable of accepting inbound links. The
 * identity is copied and must contain private key material. Link callbacks are
 * copied into each accepted link; accepted_callback receives its opaque
 * caller-owned handle. */
rns_status_t rns_runtime_register_link_destination(
    rns_runtime_t *runtime, const uint8_t destination_hash[16],
    const rns_identity *identity,
    const rns_runtime_link_options_t *link_options,
    rns_runtime_inbound_link_callback_t accepted_callback,
    void *callback_context, rns_runtime_destination_t **destination);
const uint8_t *rns_runtime_destination_hash(
    const rns_runtime_destination_t *destination);
void rns_runtime_destination_destroy(rns_runtime_destination_t *destination);
/* Broadcasts an RNS path request. A responding peer will re-announce the
 * destination, allowing callers to resolve its current route and identity. */
rns_status_t rns_runtime_request_path(rns_runtime_t *runtime,
                                      const uint8_t destination_hash[16]);
rns_status_t rns_runtime_path_lookup(const rns_runtime_t *runtime,
                                     const uint8_t destination_hash[16],
                                     rns_path_entry *path);
size_t rns_runtime_path_snapshot(const rns_runtime_t *runtime,
                                 rns_path_entry *paths, size_t capacity);

/* The returned link is runtime-attached and must be destroyed before or with
 * the runtime. Callbacks run synchronously from rns_runtime_poll(). */
rns_status_t rns_runtime_link_open(
    rns_runtime_t *runtime, const uint8_t destination_hash[16],
    const rns_identity *destination_identity,
    const rns_runtime_link_options_t *options, rns_runtime_link_t **link);
rns_status_t rns_runtime_link_send(rns_runtime_link_t *link, uint8_t context,
                                   const uint8_t *plaintext,
                                   size_t plaintext_length);
/* Sends an encrypted link packet and tracks the explicit proof signed by the
 * remote link endpoint's ephemeral signing key. */
rns_status_t rns_runtime_link_send_with_receipt(
    rns_runtime_link_t *link, uint8_t context, const uint8_t *plaintext,
    size_t plaintext_length, const rns_packet_receipt_options_t *options,
    rns_packet_receipt_t **receipt);
/* Proves the authenticated link packet currently being delivered to the
 * packet callback. This is intentionally callback-scoped so callers cannot
 * accidentally prove stale or unauthenticated input. */
rns_status_t rns_runtime_link_prove_current_packet(rns_runtime_link_t *link);
/* Privately identifies the initiator over an active encrypted link. */
rns_status_t rns_runtime_link_identify(rns_runtime_link_t *link,
                                       const rns_identity *identity);
/* Borrowed identity valid for the link lifetime, or NULL until known. */
const rns_identity *rns_runtime_link_remote_identity(
    const rns_runtime_link_t *link);
rns_link_state rns_runtime_link_state(const rns_runtime_link_t *link);
const uint8_t *rns_runtime_link_id(const rns_runtime_link_t *link);
void rns_runtime_link_destroy(rns_runtime_link_t *link);
/* Starts one bounded single-segment Resource on an active link. The returned
 * transfer is caller-owned and must be destroyed after its terminal state. */
rns_status_t rns_runtime_link_send_resource(
    rns_runtime_link_t *link, const uint8_t *data, size_t data_length,
    const rns_runtime_resource_options_t *options,
    rns_runtime_resource_transfer_t **transfer);
rns_runtime_resource_state_t rns_runtime_resource_transfer_state(
    const rns_runtime_resource_transfer_t *transfer);
size_t rns_runtime_resource_transfer_sent_parts(
    const rns_runtime_resource_transfer_t *transfer);
size_t rns_runtime_resource_transfer_total_parts(
    const rns_runtime_resource_transfer_t *transfer);
const uint8_t *rns_runtime_resource_transfer_hash(
    const rns_runtime_resource_transfer_t *transfer);
void rns_runtime_resource_transfer_cancel(
    rns_runtime_resource_transfer_t *transfer);
void rns_runtime_resource_transfer_destroy(
    rns_runtime_resource_transfer_t *transfer);
rns_status_t rns_runtime_link_request(
    rns_runtime_link_t *link, const char *path, const uint8_t *data_msgpack,
    size_t data_msgpack_length, const rns_request_options_t *options,
    rns_request_receipt_t **receipt);
rns_request_state_t rns_request_receipt_state(
    const rns_request_receipt_t *receipt);
const uint8_t *rns_request_receipt_id(const rns_request_receipt_t *receipt);
void rns_request_receipt_cancel(rns_request_receipt_t *receipt);
void rns_request_receipt_destroy(rns_request_receipt_t *receipt);

#ifdef __cplusplus
}
#endif

#endif
