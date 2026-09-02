#ifndef RETICULUM_RUNTIME_H
#define RETICULUM_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "reticulum/config.h"
#include "reticulum/identity.h"
#include "reticulum/link.h"
#include "reticulum/node.h"
#include "reticulum/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rns_runtime rns_runtime_t;
typedef struct rns_runtime_link rns_runtime_link_t;
typedef struct rns_request_receipt rns_request_receipt_t;

#define RNS_RUNTIME_MAX_LINKS 16u
#define RNS_RUNTIME_MAX_REQUESTS 8u
#define RNS_REQUEST_DEFAULT_MAX_RESPONSE (8u * 1024u * 1024u)
/* Resource transfer contexts, advertised by peers that answer with a Resource. */
#define RNS_LINK_CONTEXT_RESOURCE 0x01u
#define RNS_LINK_CONTEXT_RESOURCE_ADV 0x02u
#define RNS_LINK_CONTEXT_RESOURCE_ICL 0x06u
#define RNS_LINK_CONTEXT_REQUEST 0x09u
#define RNS_LINK_CONTEXT_RESPONSE 0x0au
#define RNS_LINK_CONTEXT_KEEPALIVE 0xfau
#define RNS_LINK_CONTEXT_CLOSE 0xfcu
#define RNS_LINK_CONTEXT_RTT 0xfeu
#define RNS_LINK_CONTEXT_PROOF 0xffu

typedef void (*rns_runtime_link_state_callback_t)(
    rns_runtime_link_t *link, rns_link_state state, rns_status_t reason,
    void *context);
typedef void (*rns_runtime_link_packet_callback_t)(
    rns_runtime_link_t *link, uint8_t packet_context,
    const uint8_t *plaintext, size_t plaintext_length, void *context);

typedef struct rns_runtime_link_options {
    double timeout_seconds;
    uint32_t mtu;
    rns_runtime_link_state_callback_t state_callback;
    rns_runtime_link_packet_callback_t packet_callback;
    void *callback_context;
} rns_runtime_link_options_t;

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

size_t rns_runtime_interface_count(const rns_runtime_t *runtime);
rns_status_t rns_runtime_interface_info(const rns_runtime_t *runtime,
                                        size_t interface_index,
                                        rns_runtime_interface_info_t *info);
rns_status_t rns_runtime_register_destination(rns_runtime_t *runtime,
                                              const uint8_t destination_hash[16]);
rns_status_t rns_runtime_unregister_destination(rns_runtime_t *runtime,
                                                const uint8_t destination_hash[16]);
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
rns_link_state rns_runtime_link_state(const rns_runtime_link_t *link);
const uint8_t *rns_runtime_link_id(const rns_runtime_link_t *link);
void rns_runtime_link_destroy(rns_runtime_link_t *link);
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
