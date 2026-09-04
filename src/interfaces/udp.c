#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "reticulum/udp.h"

#include "reticulum/packet.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

struct rns_udp_endpoint {
    int descriptor;
    int native_family;
    bool connected;
    size_t datagram_limit;
};

static int native_family(rns_udp_family_t family) {
    if (family == RNS_UDP_IPV4) {
        return AF_INET;
    }
    if (family == RNS_UDP_IPV6) {
        return AF_INET6;
    }
    return AF_UNSPEC;
}

static rns_status_t socket_status(void) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return RNS_ERROR_TIMEOUT;
    }
    return RNS_ERROR_IO;
}

static rns_status_t address_from_native(const struct sockaddr *native,
                                        socklen_t native_length,
                                        rns_udp_address_t *address) {
    memset(address, 0, sizeof(*address));
    if (native->sa_family == AF_INET && native_length >= (socklen_t)sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *ipv4 = (const struct sockaddr_in *)native;
        address->family = RNS_UDP_IPV4;
        memcpy(address->address, &ipv4->sin_addr, sizeof(ipv4->sin_addr));
        address->port = ntohs(ipv4->sin_port);
        return RNS_OK;
    }
    if (native->sa_family == AF_INET6 && native_length >= (socklen_t)sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6 *ipv6 = (const struct sockaddr_in6 *)native;
        address->family = RNS_UDP_IPV6;
        memcpy(address->address, &ipv6->sin6_addr, sizeof(ipv6->sin6_addr));
        address->port = ntohs(ipv6->sin6_port);
        address->scope_id = ipv6->sin6_scope_id;
        return RNS_OK;
    }
    return RNS_ERROR_UNSUPPORTED;
}

static rns_status_t address_to_native(const rns_udp_address_t *address,
                                      struct sockaddr_storage *native,
                                      socklen_t *native_length) {
    memset(native, 0, sizeof(*native));
    if (address->family == RNS_UDP_IPV4) {
        struct sockaddr_in *ipv4 = (struct sockaddr_in *)native;
        ipv4->sin_family = AF_INET;
        ipv4->sin_port = htons(address->port);
        memcpy(&ipv4->sin_addr, address->address, sizeof(ipv4->sin_addr));
        *native_length = (socklen_t)sizeof(*ipv4);
        return RNS_OK;
    }
    if (address->family == RNS_UDP_IPV6) {
        struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)native;
        ipv6->sin6_family = AF_INET6;
        ipv6->sin6_port = htons(address->port);
        ipv6->sin6_scope_id = address->scope_id;
        memcpy(&ipv6->sin6_addr, address->address, sizeof(ipv6->sin6_addr));
        *native_length = (socklen_t)sizeof(*ipv6);
        return RNS_OK;
    }
    return RNS_ERROR_INVALID_ARGUMENT;
}

rns_status_t rns_udp_endpoint_create(rns_udp_endpoint_t **endpoint,
                                     rns_udp_family_t family) {
    rns_udp_endpoint_t *created;
    int family_value;
    int flags;

    if (endpoint == NULL) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    family_value = native_family(family);
    if (family_value == AF_UNSPEC) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    created = calloc(1U, sizeof(*created));
    if (created == NULL) {
        return RNS_ERROR_NO_MEMORY;
    }
    created->descriptor = socket(family_value, SOCK_DGRAM, 0);
    if (created->descriptor < 0) {
        free(created);
        return RNS_ERROR_IO;
    }
    flags = fcntl(created->descriptor, F_GETFL, 0);
    if (flags < 0 || fcntl(created->descriptor, F_SETFL, flags | O_NONBLOCK) < 0) {
        (void)close(created->descriptor);
        free(created);
        return RNS_ERROR_IO;
    }
    created->native_family = family_value;
    created->datagram_limit = RNS_MTU;
    *endpoint = created;
    return RNS_OK;
}

void rns_udp_endpoint_destroy(rns_udp_endpoint_t *endpoint) {
    if (endpoint != NULL) {
        (void)close(endpoint->descriptor);
        free(endpoint);
    }
}

static rns_status_t resolve_native(const char *host,
                                   uint16_t port,
                                   int family,
                                   bool passive,
                                   struct sockaddr_storage *address,
                                   socklen_t *address_length) {
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    char service[6];
    int resolution;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = family;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = passive ? AI_PASSIVE : 0;
    (void)snprintf(service, sizeof(service), "%u", (unsigned int)port);
    resolution = getaddrinfo(host, service, &hints, &result);
    if (resolution != 0 || result == NULL || result->ai_addrlen > sizeof(*address)) {
        if (result != NULL) {
            freeaddrinfo(result);
        }
        return RNS_ERROR_NOT_FOUND;
    }
    memcpy(address, result->ai_addr, result->ai_addrlen);
    *address_length = (socklen_t)result->ai_addrlen;
    freeaddrinfo(result);
    return RNS_OK;
}

rns_status_t rns_udp_bind(rns_udp_endpoint_t *endpoint, const char *host, uint16_t port) {
    struct sockaddr_storage address;
    socklen_t address_length;
    rns_status_t status;

    if (endpoint == NULL) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    status = resolve_native(host, port, endpoint->native_family, true,
                            &address, &address_length);
    if (status != RNS_OK) {
        return status;
    }
    return bind(endpoint->descriptor, (const struct sockaddr *)&address, address_length) == 0 ?
           RNS_OK : RNS_ERROR_IO;
}

rns_status_t rns_udp_connect(rns_udp_endpoint_t *endpoint, const char *host, uint16_t port) {
    struct sockaddr_storage address;
    socklen_t address_length;
    rns_status_t status;

    if (endpoint == NULL || host == NULL) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    status = resolve_native(host, port, endpoint->native_family, false,
                            &address, &address_length);
    if (status != RNS_OK) {
        return status;
    }
    if (connect(endpoint->descriptor, (const struct sockaddr *)&address, address_length) != 0) {
        return RNS_ERROR_IO;
    }
    endpoint->connected = true;
    return RNS_OK;
}

rns_status_t rns_udp_local_address(const rns_udp_endpoint_t *endpoint,
                                   rns_udp_address_t *address) {
    struct sockaddr_storage native;
    socklen_t length = (socklen_t)sizeof(native);

    if (endpoint == NULL || address == NULL) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    if (getsockname(endpoint->descriptor, (struct sockaddr *)&native, &length) != 0) {
        return RNS_ERROR_IO;
    }
    return address_from_native((const struct sockaddr *)&native, length, address);
}

rns_status_t rns_udp_set_datagram_limit(rns_udp_endpoint_t *endpoint,
                                        size_t maximum) {
    if (endpoint == NULL || maximum == 0u || maximum > RNS_UDP_DATAGRAM_MAX)
        return RNS_ERROR_INVALID_ARGUMENT;
    endpoint->datagram_limit = maximum;
    return RNS_OK;
}

rns_status_t rns_udp_resolve(const char *host, uint16_t port,
                             rns_udp_family_t family, rns_udp_address_t *address) {
    struct sockaddr_storage native;
    socklen_t length;
    rns_status_t status;

    if (host == NULL || address == NULL || native_family(family) == AF_UNSPEC) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    status = resolve_native(host, port, native_family(family), false, &native, &length);
    if (status != RNS_OK) {
        return status;
    }
    return address_from_native((const struct sockaddr *)&native, length, address);
}

static rns_status_t validate_packet(const rns_udp_endpoint_t *endpoint,
                                    const uint8_t *packet,
                                    size_t packet_length) {
    if (packet == NULL || packet_length == 0U) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    return packet_length <= endpoint->datagram_limit ? RNS_OK
                                                      : RNS_ERROR_OVERFLOW;
}

rns_status_t rns_udp_send(rns_udp_endpoint_t *endpoint,
                          const uint8_t *packet, size_t packet_length) {
    ssize_t sent;
    rns_status_t status;

    if (endpoint == NULL) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    status = validate_packet(endpoint, packet, packet_length);
    if (status != RNS_OK) {
        return status;
    }
    if (!endpoint->connected) {
        return RNS_ERROR_INVALID_STATE;
    }
    sent = send(endpoint->descriptor, packet, packet_length, 0);
    if (sent < 0) {
        return socket_status();
    }
    return (size_t)sent == packet_length ? RNS_OK : RNS_ERROR_IO;
}

rns_status_t rns_udp_send_to(rns_udp_endpoint_t *endpoint,
                             const rns_udp_address_t *destination,
                             const uint8_t *packet, size_t packet_length) {
    struct sockaddr_storage native;
    socklen_t native_length;
    ssize_t sent;
    rns_status_t status;

    if (endpoint == NULL || destination == NULL ||
        native_family(destination->family) != endpoint->native_family) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    status = validate_packet(endpoint, packet, packet_length);
    if (status != RNS_OK) {
        return status;
    }
    status = address_to_native(destination, &native, &native_length);
    if (status != RNS_OK) {
        return status;
    }
    sent = sendto(endpoint->descriptor, packet, packet_length, 0,
                  (const struct sockaddr *)&native, native_length);
    if (sent < 0) {
        return socket_status();
    }
    return (size_t)sent == packet_length ? RNS_OK : RNS_ERROR_IO;
}

rns_status_t rns_udp_poll(rns_udp_endpoint_t *endpoint,
                          size_t max_datagrams,
                          rns_udp_receive_callback_t callback,
                          void *context,
                          size_t *received) {
    size_t count = 0U;

    if (endpoint == NULL || callback == NULL || received == NULL) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    *received = 0U;
    while (max_datagrams == 0U || count < max_datagrams) {
        uint8_t packet[RNS_UDP_DATAGRAM_MAX + 1U];
        struct sockaddr_storage native_source;
        socklen_t source_length = (socklen_t)sizeof(native_source);
        rns_udp_address_t source;
        ssize_t length = recvfrom(endpoint->descriptor, packet, sizeof(packet), 0,
                                  (struct sockaddr *)&native_source, &source_length);
        rns_status_t status;

        if (length < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            return RNS_ERROR_IO;
        }
        if ((size_t)length > endpoint->datagram_limit) {
            return RNS_ERROR_OVERFLOW;
        }
        status = address_from_native((const struct sockaddr *)&native_source,
                                     source_length, &source);
        if (status != RNS_OK) {
            return status;
        }
        status = callback(packet, (size_t)length, &source, context);
        if (status != RNS_OK) {
            return status;
        }
        ++count;
        *received = count;
    }
    return RNS_OK;
}

rns_status_t rns_udp_set_broadcast(rns_udp_endpoint_t *endpoint, bool enabled) {
    int value = enabled ? 1 : 0;
    if (endpoint == NULL || endpoint->native_family != AF_INET) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    return setsockopt(endpoint->descriptor, SOL_SOCKET, SO_BROADCAST,
                      &value, (socklen_t)sizeof(value)) == 0 ? RNS_OK : RNS_ERROR_IO;
}

rns_status_t rns_udp_set_reuse(rns_udp_endpoint_t *endpoint, bool enabled) {
    int value = enabled ? 1 : 0;
    if (endpoint == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    if (setsockopt(endpoint->descriptor, SOL_SOCKET, SO_REUSEADDR, &value,
                   (socklen_t)sizeof(value)) != 0)
        return RNS_ERROR_IO;
#ifdef SO_REUSEPORT
    if (setsockopt(endpoint->descriptor, SOL_SOCKET, SO_REUSEPORT, &value,
                   (socklen_t)sizeof(value)) != 0)
        return RNS_ERROR_IO;
#endif
    return RNS_OK;
}

rns_status_t rns_udp_set_multicast_loop(rns_udp_endpoint_t *endpoint, bool enabled) {
    if (endpoint == NULL) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    if (endpoint->native_family == AF_INET) {
        unsigned char value = enabled ? 1U : 0U;
        return setsockopt(endpoint->descriptor, IPPROTO_IP, IP_MULTICAST_LOOP,
                          &value, (socklen_t)sizeof(value)) == 0 ? RNS_OK : RNS_ERROR_IO;
    }
    {
        unsigned int value = enabled ? 1U : 0U;
        return setsockopt(endpoint->descriptor, IPPROTO_IPV6, IPV6_MULTICAST_LOOP,
                          &value, (socklen_t)sizeof(value)) == 0 ? RNS_OK : RNS_ERROR_IO;
    }
}

rns_status_t rns_udp_set_multicast_hops(rns_udp_endpoint_t *endpoint, uint8_t hops) {
    if (endpoint == NULL) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    if (endpoint->native_family == AF_INET) {
        unsigned char value = hops;
        return setsockopt(endpoint->descriptor, IPPROTO_IP, IP_MULTICAST_TTL,
                          &value, (socklen_t)sizeof(value)) == 0 ? RNS_OK : RNS_ERROR_IO;
    }
    {
        int value = (int)hops;
        return setsockopt(endpoint->descriptor, IPPROTO_IPV6, IPV6_MULTICAST_HOPS,
                          &value, (socklen_t)sizeof(value)) == 0 ? RNS_OK : RNS_ERROR_IO;
    }
}

rns_status_t rns_udp_set_multicast_interface(rns_udp_endpoint_t *endpoint,
                                             uint32_t interface_index) {
    if (endpoint == NULL || endpoint->native_family != AF_INET6 ||
        interface_index == 0u)
        return RNS_ERROR_INVALID_ARGUMENT;
    return setsockopt(endpoint->descriptor, IPPROTO_IPV6, IPV6_MULTICAST_IF,
                      &interface_index,
                      (socklen_t)sizeof(interface_index)) == 0
               ? RNS_OK
               : RNS_ERROR_IO;
}

static rns_status_t multicast_membership(rns_udp_endpoint_t *endpoint,
                                         const char *group,
                                         uint32_t interface_index,
                                         bool join) {
    if (endpoint == NULL || group == NULL) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    if (endpoint->native_family == AF_INET) {
        struct ip_mreq request;
        (void)interface_index;
        memset(&request, 0, sizeof(request));
        if (inet_pton(AF_INET, group, &request.imr_multiaddr) != 1) {
            return RNS_ERROR_INVALID_ARGUMENT;
        }
        request.imr_interface.s_addr = htonl(INADDR_ANY);
        return setsockopt(endpoint->descriptor, IPPROTO_IP,
                          join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP,
                          &request, (socklen_t)sizeof(request)) == 0 ? RNS_OK : RNS_ERROR_IO;
    }
    if (endpoint->native_family == AF_INET6) {
        struct ipv6_mreq request;
        memset(&request, 0, sizeof(request));
        if (inet_pton(AF_INET6, group, &request.ipv6mr_multiaddr) != 1) {
            return RNS_ERROR_INVALID_ARGUMENT;
        }
        request.ipv6mr_interface = interface_index;
        return setsockopt(endpoint->descriptor, IPPROTO_IPV6,
                          join ? IPV6_JOIN_GROUP : IPV6_LEAVE_GROUP,
                          &request, (socklen_t)sizeof(request)) == 0 ? RNS_OK : RNS_ERROR_IO;
    }
    return RNS_ERROR_UNSUPPORTED;
}

rns_status_t rns_udp_join_multicast(rns_udp_endpoint_t *endpoint,
                                    const char *group,
                                    uint32_t interface_index) {
    return multicast_membership(endpoint, group, interface_index, true);
}

rns_status_t rns_udp_leave_multicast(rns_udp_endpoint_t *endpoint,
                                     const char *group,
                                     uint32_t interface_index) {
    return multicast_membership(endpoint, group, interface_index, false);
}
