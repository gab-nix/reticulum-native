#ifndef RETICULUM_TCP_H
#define RETICULUM_TCP_H

#include <stddef.h>
#include <stdint.h>

#include "reticulum/framing.h"
#include "reticulum/status.h"
#include "reticulum/udp.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef struct rns_tcp_endpoint rns_tcp_endpoint_t;

typedef enum rns_tcp_state {
    RNS_TCP_DISCONNECTED = 0,
    RNS_TCP_CONNECTING,
    RNS_TCP_CONNECTED,
    RNS_TCP_LISTENING
} rns_tcp_state_t;

typedef void (*rns_tcp_state_callback_t)(rns_tcp_endpoint_t *endpoint,
                                         rns_tcp_state_t previous,
                                         rns_tcp_state_t current,
                                         void *context);

rns_status_t rns_tcp_endpoint_create(rns_tcp_endpoint_t **endpoint,
                                     rns_udp_family_t family,
                                     size_t send_queue_capacity);
void rns_tcp_endpoint_destroy(rns_tcp_endpoint_t *endpoint);
void rns_tcp_set_state_callback(rns_tcp_endpoint_t *endpoint,
                                rns_tcp_state_callback_t callback,
                                void *context);
rns_tcp_state_t rns_tcp_state(const rns_tcp_endpoint_t *endpoint);
size_t rns_tcp_pending_bytes(const rns_tcp_endpoint_t *endpoint);
size_t rns_tcp_malformed_frames(const rns_tcp_endpoint_t *endpoint);
size_t rns_tcp_oversized_frames(const rns_tcp_endpoint_t *endpoint);

rns_status_t rns_tcp_listen(rns_tcp_endpoint_t *endpoint,
                            const char *host,
                            uint16_t port,
                            int backlog);
rns_status_t rns_tcp_local_address(const rns_tcp_endpoint_t *endpoint,
                                   rns_udp_address_t *address);
rns_status_t rns_tcp_accept(rns_tcp_endpoint_t *listener,
                            rns_tcp_endpoint_t **connection,
                            size_t send_queue_capacity);

rns_status_t rns_tcp_connect(rns_tcp_endpoint_t *endpoint,
                             const char *host,
                             uint16_t port);
rns_status_t rns_tcp_finish_connect(rns_tcp_endpoint_t *endpoint);
void rns_tcp_disconnect(rns_tcp_endpoint_t *endpoint);

rns_status_t rns_tcp_queue_frame(rns_tcp_endpoint_t *endpoint,
                                 const uint8_t *packet,
                                 size_t packet_length);
rns_status_t rns_tcp_flush(rns_tcp_endpoint_t *endpoint, size_t *bytes_sent);
rns_status_t rns_tcp_poll_receive(rns_tcp_endpoint_t *endpoint,
                                  rns_frame_callback_t callback,
                                  void *context,
                                  size_t *bytes_received);

#ifdef __cplusplus
}
#endif

#endif
