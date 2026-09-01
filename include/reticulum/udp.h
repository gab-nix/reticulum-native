#ifndef RETICULUM_UDP_H
#define RETICULUM_UDP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "reticulum/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum rns_udp_family {
    RNS_UDP_IPV4 = 4,
    RNS_UDP_IPV6 = 6
} rns_udp_family_t;

typedef struct rns_udp_address {
    rns_udp_family_t family;
    uint8_t address[16];
    uint16_t port;
    uint32_t scope_id;
} rns_udp_address_t;

typedef struct rns_udp_endpoint rns_udp_endpoint_t;

typedef rns_status_t (*rns_udp_receive_callback_t)(const uint8_t *packet,
                                                   size_t packet_length,
                                                   const rns_udp_address_t *source,
                                                   void *context);

rns_status_t rns_udp_endpoint_create(rns_udp_endpoint_t **endpoint,
                                     rns_udp_family_t family);
void rns_udp_endpoint_destroy(rns_udp_endpoint_t *endpoint);

rns_status_t rns_udp_bind(rns_udp_endpoint_t *endpoint,
                          const char *host,
                          uint16_t port);
rns_status_t rns_udp_connect(rns_udp_endpoint_t *endpoint,
                             const char *host,
                             uint16_t port);
rns_status_t rns_udp_local_address(const rns_udp_endpoint_t *endpoint,
                                   rns_udp_address_t *address);
rns_status_t rns_udp_resolve(const char *host,
                             uint16_t port,
                             rns_udp_family_t family,
                             rns_udp_address_t *address);

rns_status_t rns_udp_send(rns_udp_endpoint_t *endpoint,
                          const uint8_t *packet,
                          size_t packet_length);
rns_status_t rns_udp_send_to(rns_udp_endpoint_t *endpoint,
                             const rns_udp_address_t *destination,
                             const uint8_t *packet,
                             size_t packet_length);
rns_status_t rns_udp_poll(rns_udp_endpoint_t *endpoint,
                          size_t max_datagrams,
                          rns_udp_receive_callback_t callback,
                          void *context,
                          size_t *received);

rns_status_t rns_udp_set_broadcast(rns_udp_endpoint_t *endpoint, bool enabled);
rns_status_t rns_udp_set_multicast_loop(rns_udp_endpoint_t *endpoint, bool enabled);
rns_status_t rns_udp_set_multicast_hops(rns_udp_endpoint_t *endpoint, uint8_t hops);
rns_status_t rns_udp_join_multicast(rns_udp_endpoint_t *endpoint,
                                    const char *group,
                                    uint32_t interface_index);
rns_status_t rns_udp_leave_multicast(rns_udp_endpoint_t *endpoint,
                                     const char *group,
                                     uint32_t interface_index);

#ifdef __cplusplus
}
#endif

#endif

