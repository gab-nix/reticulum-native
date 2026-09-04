#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "auto_posix.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

struct rns_auto_posix {
    rns_auto_endpoint_t *protocol;
    rns_udp_endpoint_t *discovery;
    rns_udp_endpoint_t *unicast;
    rns_udp_endpoint_t *data;
    rns_udp_endpoint_t *outbound;
    rns_auto_clock_callback_t clock;
    void *clock_context;
    rns_auto_receive_callback_t active_receive;
    void *active_context;
    size_t active_received;
};

static bool list_contains(const char *list, const char *name) {
    if (list == NULL || list[0] == '\0') return false;
    size_t name_length = strlen(name);
    const char *cursor = list;
    while (*cursor != '\0') {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == ',') ++cursor;
        const char *start = cursor;
        while (*cursor != '\0' && *cursor != ',') ++cursor;
        const char *end = cursor;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) --end;
        if ((size_t)(end - start) == name_length &&
            memcmp(start, name, name_length) == 0)
            return true;
    }
    return false;
}

static bool interface_allowed(const rns_config_interface_t *configuration,
                              const char *name) {
    if (strcmp(name, "lo0") == 0) return false;
#ifdef __APPLE__
    if ((strcmp(name, "awdl0") == 0 || strcmp(name, "llw0") == 0 ||
         strcmp(name, "en5") == 0) &&
        !list_contains(configuration->devices, name))
        return false;
#endif
    if (list_contains(configuration->ignored_devices, name)) return false;
    return configuration->devices[0] == '\0' ||
           list_contains(configuration->devices, name);
}

static rns_auto_scope_t parse_scope(const char *value) {
    if (strcmp(value, "admin") == 0) return RNS_AUTO_SCOPE_ADMIN;
    if (strcmp(value, "site") == 0) return RNS_AUTO_SCOPE_SITE;
    if (strcmp(value, "organisation") == 0)
        return RNS_AUTO_SCOPE_ORGANISATION;
    if (strcmp(value, "global") == 0) return RNS_AUTO_SCOPE_GLOBAL;
    return RNS_AUTO_SCOPE_LINK;
}

static double posix_clock(void *context) {
    rns_auto_posix_t *endpoint = context;
    return endpoint->clock(endpoint->clock_context);
}

static rns_status_t posix_emit(rns_auto_emit_kind_t kind,
                               uint32_t interface_index,
                               const rns_udp_address_t *destination,
                               const uint8_t *data, size_t data_length,
                               void *context) {
    rns_auto_posix_t *endpoint = context;
    if (kind == RNS_AUTO_EMIT_MULTICAST_DISCOVERY) {
        rns_status_t status = rns_udp_set_multicast_interface(
            endpoint->outbound, interface_index);
        if (status != RNS_OK) return status;
    }
    return rns_udp_send_to(endpoint->outbound, destination, data, data_length);
}

static rns_status_t posix_receive(const uint8_t *packet, size_t packet_length,
                                  const rns_udp_address_t *source,
                                  uint32_t interface_index, void *context) {
    rns_auto_posix_t *endpoint = context;
    if (endpoint->active_receive == NULL) return RNS_ERROR_INVALID_STATE;
    rns_status_t status = endpoint->active_receive(
        packet, packet_length, source, interface_index,
        endpoint->active_context);
    if (status == RNS_OK) endpoint->active_received++;
    return status;
}

static rns_status_t configure_socket(rns_udp_endpoint_t **output,
                                     uint16_t port, size_t limit,
                                     bool reuse) {
    rns_status_t status = rns_udp_endpoint_create(output, RNS_UDP_IPV6);
    if (status == RNS_OK && reuse)
        status = rns_udp_set_reuse(*output, true);
    if (status == RNS_OK)
        status = rns_udp_set_datagram_limit(*output, limit);
    if (status == RNS_OK && port != 0u)
        status = rns_udp_bind(*output, "::", port);
    return status;
}

static rns_status_t add_system_interfaces(
    rns_auto_posix_t *endpoint,
    const rns_config_interface_t *configuration) {
    struct ifaddrs *addresses = NULL;
    if (getifaddrs(&addresses) != 0) return RNS_ERROR_IO;
    rns_udp_address_t group;
    rns_status_t status = rns_auto_multicast_address(endpoint->protocol,
                                                     &group);
    rns_status_t first_error = status;
    uint32_t added_indices[RNS_AUTO_MAX_LOCAL_INTERFACES] = {0};
    size_t added = 0u;
    for (struct ifaddrs *item = addresses; item != NULL; item = item->ifa_next) {
        if (item->ifa_addr == NULL || item->ifa_addr->sa_family != AF_INET6 ||
            !interface_allowed(configuration, item->ifa_name))
            continue;
        const struct sockaddr_in6 *native =
            (const struct sockaddr_in6 *)item->ifa_addr;
        if (!IN6_IS_ADDR_LINKLOCAL(&native->sin6_addr)) continue;
        unsigned int index = if_nametoindex(item->ifa_name);
        if (index == 0u) continue;
        bool duplicate = false;
        for (size_t i = 0u; i < added; ++i)
            if (added_indices[i] == index) duplicate = true;
        if (duplicate || added == RNS_AUTO_MAX_LOCAL_INTERFACES) continue;
        rns_udp_address_t local = {0};
        local.family = RNS_UDP_IPV6;
        memcpy(local.address, &native->sin6_addr, 16u);
        local.scope_id = index;
        char group_text[64];
        /* The generic UDP API accepts text groups. inet_ntop is bounded. */
        if (inet_ntop(AF_INET6, group.address, group_text,
                      sizeof group_text) == NULL)
            status = RNS_ERROR_IO;
        else
            status = rns_udp_join_multicast(endpoint->discovery, group_text,
                                            index);
        if (status == RNS_OK)
            status = rns_auto_add_local_interface(endpoint->protocol, &local,
                                                  index);
        if (status == RNS_OK) added_indices[added++] = index;
        else if (first_error == RNS_OK) first_error = status;
    }
    freeifaddrs(addresses);
    if (added != 0u) return RNS_OK;
    return first_error != RNS_OK ? first_error : RNS_ERROR_NOT_FOUND;
}

rns_status_t rns_auto_posix_create(
    rns_auto_posix_t **output, const rns_config_interface_t *configuration,
    rns_auto_clock_callback_t clock, void *clock_context) {
    if (output == NULL || configuration == NULL || clock == NULL)
        return RNS_ERROR_INVALID_ARGUMENT;
    *output = NULL;
    rns_auto_posix_t *endpoint = calloc(1u, sizeof *endpoint);
    if (endpoint == NULL) return RNS_ERROR_NO_MEMORY;
    endpoint->clock = clock;
    endpoint->clock_context = clock_context;
    rns_auto_options_t options;
    rns_auto_options_init(&options);
    if (configuration->group_id[0] != '\0') {
        options.group_id = (const uint8_t *)configuration->group_id;
        options.group_id_length = strlen(configuration->group_id);
    }
    options.discovery_scope = parse_scope(configuration->discovery_scope);
    options.multicast_type =
        strcmp(configuration->multicast_address_type, "permanent") == 0
            ? RNS_AUTO_MULTICAST_PERMANENT
            : RNS_AUTO_MULTICAST_TEMPORARY;
    options.discovery_port = configuration->discovery_port != 0u
                                 ? configuration->discovery_port
                                 : RNS_AUTO_DEFAULT_DISCOVERY_PORT;
    options.data_port = configuration->data_port != 0u
                            ? configuration->data_port
                            : RNS_AUTO_DEFAULT_DATA_PORT;
    options.clock = posix_clock;
    options.emit = posix_emit;
    options.receive = posix_receive;
    options.callback_context = endpoint;
    rns_status_t status = rns_auto_endpoint_create(&endpoint->protocol,
                                                   &options);
    if (status == RNS_OK)
        status = configure_socket(&endpoint->discovery,
                                  options.discovery_port,
                                  RNS_AUTO_PEERING_TOKEN_SIZE, true);
    if (status == RNS_OK)
        status = configure_socket(&endpoint->unicast,
                                  (uint16_t)(options.discovery_port + 1u),
                                  RNS_AUTO_PEERING_TOKEN_SIZE, true);
    if (status == RNS_OK)
        status = configure_socket(&endpoint->data, options.data_port,
                                  RNS_AUTO_HW_MTU, true);
    if (status == RNS_OK)
        status = configure_socket(&endpoint->outbound, 0u,
                                  RNS_AUTO_HW_MTU, false);
    if (status == RNS_OK)
        status = add_system_interfaces(endpoint, configuration);
    if (status != RNS_OK) {
        rns_auto_posix_destroy(endpoint);
        return status;
    }
    *output = endpoint;
    return RNS_OK;
}

void rns_auto_posix_destroy(rns_auto_posix_t *endpoint) {
    if (endpoint == NULL) return;
    rns_udp_endpoint_destroy(endpoint->outbound);
    rns_udp_endpoint_destroy(endpoint->data);
    rns_udp_endpoint_destroy(endpoint->unicast);
    rns_udp_endpoint_destroy(endpoint->discovery);
    rns_auto_endpoint_destroy(endpoint->protocol);
    free(endpoint);
}

typedef struct discovery_context {
    rns_auto_posix_t *endpoint;
} discovery_context_t;

static rns_status_t discovery_receive(const uint8_t *packet,
                                      size_t packet_length,
                                      const rns_udp_address_t *source,
                                      void *context) {
    discovery_context_t *discovery = context;
    rns_status_t status = rns_auto_ingest_discovery(
        discovery->endpoint->protocol, source, source->scope_id, packet,
        packet_length);
    return status == RNS_OK || status == RNS_ERROR_PROTOCOL ||
                   status == RNS_ERROR_CRYPTO ||
                   status == RNS_ERROR_INVALID_STATE ||
                   status == RNS_ERROR_OVERFLOW
               ? RNS_OK
               : status;
}

static rns_status_t data_receive(const uint8_t *packet, size_t packet_length,
                                 const rns_udp_address_t *source,
                                 void *context) {
    discovery_context_t *data = context;
    rns_status_t status = rns_auto_ingest_data(
        data->endpoint->protocol, source, source->scope_id, packet,
        packet_length);
    return status == RNS_ERROR_NOT_FOUND || status == RNS_ERROR_INVALID_STATE
               ? RNS_OK
               : status;
}

rns_status_t rns_auto_posix_poll(rns_auto_posix_t *endpoint,
                                 size_t max_packets,
                                 rns_auto_receive_callback_t callback,
                                 void *context, size_t *packets_received) {
    if (endpoint == NULL || callback == NULL || packets_received == NULL)
        return RNS_ERROR_INVALID_ARGUMENT;
    endpoint->active_receive = callback;
    endpoint->active_context = context;
    endpoint->active_received = 0u;
    discovery_context_t receive_context = {endpoint};
    size_t ignored = 0u;
    rns_status_t status = rns_udp_poll(endpoint->discovery, max_packets,
                                       discovery_receive, &receive_context,
                                       &ignored);
    if (status == RNS_OK)
        status = rns_udp_poll(endpoint->unicast, max_packets,
                              discovery_receive, &receive_context, &ignored);
    if (status == RNS_OK)
        status = rns_udp_poll(endpoint->data, max_packets, data_receive,
                              &receive_context, &ignored);
    if (status == RNS_OK) status = rns_auto_poll(endpoint->protocol);
    *packets_received = endpoint->active_received;
    endpoint->active_receive = NULL;
    endpoint->active_context = NULL;
    return status == RNS_ERROR_INVALID_STATE ? RNS_OK : status;
}

rns_status_t rns_auto_posix_send(rns_auto_posix_t *endpoint,
                                 const uint8_t *packet, size_t packet_length,
                                 size_t *peers_sent) {
    if (endpoint == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    return rns_auto_send(endpoint->protocol, packet, packet_length, peers_sent);
}

bool rns_auto_posix_online(const rns_auto_posix_t *endpoint) {
    return endpoint != NULL && rns_auto_online(endpoint->protocol);
}

size_t rns_auto_posix_peer_count(const rns_auto_posix_t *endpoint) {
    return endpoint != NULL ? rns_auto_peer_count(endpoint->protocol) : 0u;
}
