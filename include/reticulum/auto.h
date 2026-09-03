#ifndef RETICULUM_AUTO_H
#define RETICULUM_AUTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "reticulum/status.h"
#include "reticulum/udp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RNS_AUTO_HW_MTU 1196u
#define RNS_AUTO_DEFAULT_DISCOVERY_PORT 29716u
#define RNS_AUTO_DEFAULT_DATA_PORT 42671u
#define RNS_AUTO_DEFAULT_PEER_CAPACITY 32u
#define RNS_AUTO_MAX_PEERS 128u
#define RNS_AUTO_MAX_LOCAL_INTERFACES 32u
#define RNS_AUTO_GROUP_ID_MAX 128u
#define RNS_AUTO_PEERING_TOKEN_SIZE 32u

typedef enum rns_auto_scope {
    RNS_AUTO_SCOPE_LINK = 0x2,
    RNS_AUTO_SCOPE_ADMIN = 0x4,
    RNS_AUTO_SCOPE_SITE = 0x5,
    RNS_AUTO_SCOPE_ORGANISATION = 0x8,
    RNS_AUTO_SCOPE_GLOBAL = 0xe
} rns_auto_scope_t;

typedef enum rns_auto_multicast_type {
    RNS_AUTO_MULTICAST_PERMANENT = 0,
    RNS_AUTO_MULTICAST_TEMPORARY = 1
} rns_auto_multicast_type_t;

typedef enum rns_auto_emit_kind {
    RNS_AUTO_EMIT_MULTICAST_DISCOVERY = 0,
    RNS_AUTO_EMIT_UNICAST_DISCOVERY,
    RNS_AUTO_EMIT_DATA
} rns_auto_emit_kind_t;

typedef struct rns_auto_endpoint rns_auto_endpoint_t;

typedef double (*rns_auto_clock_callback_t)(void *context);
typedef rns_status_t (*rns_auto_emit_callback_t)(
    rns_auto_emit_kind_t kind, uint32_t interface_index,
    const rns_udp_address_t *destination, const uint8_t *data,
    size_t data_length, void *context);
typedef rns_status_t (*rns_auto_receive_callback_t)(
    const uint8_t *packet, size_t packet_length,
    const rns_udp_address_t *source, uint32_t interface_index, void *context);

typedef struct rns_auto_options {
    const uint8_t *group_id;
    size_t group_id_length;
    rns_auto_scope_t discovery_scope;
    rns_auto_multicast_type_t multicast_type;
    uint16_t discovery_port;
    uint16_t data_port;
    size_t peer_capacity;
    double announce_interval;
    double peering_timeout;
    double reverse_peering_interval;
    double initial_wait;
    rns_auto_clock_callback_t clock;
    rns_auto_emit_callback_t emit;
    rns_auto_receive_callback_t receive;
    void *callback_context;
} rns_auto_options_t;

typedef struct rns_auto_peer_info {
    rns_udp_address_t address;
    uint32_t interface_index;
    double last_heard;
    double last_outbound;
} rns_auto_peer_info_t;

void rns_auto_options_init(rns_auto_options_t *options);
rns_status_t rns_auto_endpoint_create(rns_auto_endpoint_t **endpoint,
                                      const rns_auto_options_t *options);
void rns_auto_endpoint_destroy(rns_auto_endpoint_t *endpoint);

/* Adds one IPv6 link-local carrier. The binary address is copied. */
rns_status_t rns_auto_add_local_interface(
    rns_auto_endpoint_t *endpoint, const rns_udp_address_t *address,
    uint32_t interface_index);

/* Discovery datagrams are exactly one SHA-256 token. */
rns_status_t rns_auto_ingest_discovery(
    rns_auto_endpoint_t *endpoint, const rns_udp_address_t *source,
    uint32_t interface_index, const uint8_t *token, size_t token_length);
rns_status_t rns_auto_ingest_data(
    rns_auto_endpoint_t *endpoint, const rns_udp_address_t *source,
    uint32_t interface_index, const uint8_t *packet, size_t packet_length);

/* Performs bounded timer work: startup, beacons, reverse discovery and peer
 * expiry. It never sleeps or owns a thread. */
rns_status_t rns_auto_poll(rns_auto_endpoint_t *endpoint);
/* Fans an unframed Reticulum packet out to every current peer. */
rns_status_t rns_auto_send(rns_auto_endpoint_t *endpoint,
                           const uint8_t *packet, size_t packet_length,
                           size_t *peers_sent);

bool rns_auto_online(const rns_auto_endpoint_t *endpoint);
size_t rns_auto_peer_count(const rns_auto_endpoint_t *endpoint);
size_t rns_auto_peer_snapshot(const rns_auto_endpoint_t *endpoint,
                              rns_auto_peer_info_t *peers, size_t capacity);
rns_status_t rns_auto_multicast_address(const rns_auto_endpoint_t *endpoint,
                                        rns_udp_address_t *address);
rns_status_t rns_auto_local_token(const rns_auto_endpoint_t *endpoint,
                                  uint32_t interface_index,
                                  uint8_t token[RNS_AUTO_PEERING_TOKEN_SIZE]);

#ifdef __cplusplus
}
#endif

#endif
