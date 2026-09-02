#ifndef RETICULUM_RUNTIME_H
#define RETICULUM_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "reticulum/config.h"
#include "reticulum/node.h"
#include "reticulum/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rns_runtime rns_runtime_t;

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
rns_status_t rns_runtime_path_lookup(const rns_runtime_t *runtime,
                                     const uint8_t destination_hash[16],
                                     rns_path_entry *path);
size_t rns_runtime_path_snapshot(const rns_runtime_t *runtime,
                                 rns_path_entry *paths, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
