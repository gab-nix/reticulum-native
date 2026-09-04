#ifndef RETICULUM_LOCAL_H
#define RETICULUM_LOCAL_H

#include <stddef.h>
#include <stdint.h>

#include "reticulum/config.h"
#include "reticulum/framing.h"
#include "reticulum/status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RNS_LOCAL_DEFAULT_PORT 37428U
#define RNS_LOCAL_MAX_CLIENTS 16U

typedef struct rns_local_instance rns_local_instance_t;

typedef enum rns_local_role {
    RNS_LOCAL_ROLE_AUTO = 0,
    RNS_LOCAL_ROLE_SERVER,
    RNS_LOCAL_ROLE_CLIENT
} rns_local_role_t;

typedef enum rns_local_state {
    RNS_LOCAL_STARTING = 0,
    RNS_LOCAL_UP,
    RNS_LOCAL_DOWN
} rns_local_state_t;

typedef double (*rns_local_clock_callback_t)(void *context);

typedef struct rns_local_options {
    rns_local_role_t role;
    uint16_t port;
    size_t max_clients;
    size_t send_queue_capacity;
    double reconnect_initial_seconds;
    double reconnect_max_seconds;
    rns_local_clock_callback_t clock;
    void *clock_context;
} rns_local_options_t;

typedef struct rns_local_info {
    rns_local_role_t role;
    rns_local_state_t state;
    rns_status_t last_error;
    size_t connected_clients;
    uint64_t connection_attempts;
    uint64_t connections_established;
    uint64_t connections_lost;
    uint64_t packets_received;
    uint64_t packets_sent;
    uint64_t packets_dropped;
    uint64_t bytes_received;
    uint64_t bytes_sent;
} rns_local_info_t;

/* Converts the pinned TCP shared-instance configuration. AF_UNIX instance
 * names are parsed but return RNS_ERROR_UNSUPPORTED until that backend exists. */
rns_status_t rns_local_options_from_config(const rns_config_t *config,
                                           rns_local_role_t role,
                                           rns_local_options_t *options);
/* The returned opaque instance is caller-owned. Frame spans passed during
 * poll are immutable and valid only for the callback. */
rns_status_t rns_local_instance_create(rns_local_instance_t **instance,
                                       const rns_local_options_t *options);
void rns_local_instance_destroy(rns_local_instance_t *instance);
rns_status_t rns_local_instance_poll(rns_local_instance_t *instance,
                                     rns_frame_callback_t callback,
                                     void *callback_context);
rns_status_t rns_local_instance_send(rns_local_instance_t *instance,
                                     const uint8_t *packet,
                                     size_t packet_length);
rns_status_t rns_local_instance_info(const rns_local_instance_t *instance,
                                     rns_local_info_t *info);

#ifdef __cplusplus
}
#endif

#endif
