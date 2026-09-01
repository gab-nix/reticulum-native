#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "reticulum/tcp.h"

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

struct rns_tcp_endpoint {
    int descriptor;
    int native_family;
    rns_tcp_state_t state;
    uint8_t receive_storage[RNS_MTU];
    rns_hdlc_decoder_t decoder;
    uint8_t *send_queue;
    size_t send_capacity;
    size_t send_head;
    size_t send_length;
    rns_tcp_state_callback_t state_callback;
    void *state_context;
};

static int tcp_native_family(rns_udp_family_t family) {
    if (family == RNS_UDP_IPV4) {
        return AF_INET;
    }
    if (family == RNS_UDP_IPV6) {
        return AF_INET6;
    }
    return AF_UNSPEC;
}

static void set_state(rns_tcp_endpoint_t *endpoint, rns_tcp_state_t state) {
    rns_tcp_state_t previous = endpoint->state;
    if (previous != state) {
        endpoint->state = state;
        if (endpoint->state_callback != NULL) {
            endpoint->state_callback(endpoint, previous, state, endpoint->state_context);
        }
    }
}

static rns_status_t make_nonblocking(int descriptor) {
    int flags = fcntl(descriptor, F_GETFL, 0);
    if (flags < 0 || fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0) {
        return RNS_ERROR_IO;
    }
#ifdef SO_NOSIGPIPE
    {
        int enabled = 1;
        if (setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE,
                       &enabled, (socklen_t)sizeof(enabled)) != 0) {
            return RNS_ERROR_IO;
        }
    }
#endif
    return RNS_OK;
}

static ssize_t send_without_sigpipe(int descriptor, const void *data, size_t length) {
#ifdef MSG_NOSIGNAL
    return send(descriptor, data, length, MSG_NOSIGNAL);
#else
    return send(descriptor, data, length, 0);
#endif
}

static rns_status_t open_socket(rns_tcp_endpoint_t *endpoint) {
    if (endpoint->descriptor >= 0) {
        return RNS_OK;
    }
    endpoint->descriptor = socket(endpoint->native_family, SOCK_STREAM, 0);
    if (endpoint->descriptor < 0) {
        return RNS_ERROR_IO;
    }
    if (make_nonblocking(endpoint->descriptor) != RNS_OK) {
        (void)close(endpoint->descriptor);
        endpoint->descriptor = -1;
        return RNS_ERROR_IO;
    }
    return RNS_OK;
}

static rns_status_t allocate_endpoint(rns_tcp_endpoint_t **endpoint,
                                      int family,
                                      size_t send_queue_capacity) {
    rns_tcp_endpoint_t *created;

    if (endpoint == NULL || family == AF_UNSPEC || send_queue_capacity == 0U) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    created = calloc(1U, sizeof(*created));
    if (created == NULL) {
        return RNS_ERROR_NO_MEMORY;
    }
    created->send_queue = malloc(send_queue_capacity);
    if (created->send_queue == NULL) {
        free(created);
        return RNS_ERROR_NO_MEMORY;
    }
    created->descriptor = -1;
    created->native_family = family;
    created->send_capacity = send_queue_capacity;
    rns_hdlc_decoder_init(&created->decoder, created->receive_storage,
                          sizeof(created->receive_storage));
    *endpoint = created;
    return RNS_OK;
}

rns_status_t rns_tcp_endpoint_create(rns_tcp_endpoint_t **endpoint,
                                     rns_udp_family_t family,
                                     size_t send_queue_capacity) {
    return allocate_endpoint(endpoint, tcp_native_family(family), send_queue_capacity);
}

void rns_tcp_endpoint_destroy(rns_tcp_endpoint_t *endpoint) {
    if (endpoint != NULL) {
        if (endpoint->descriptor >= 0) {
            (void)close(endpoint->descriptor);
        }
        free(endpoint->send_queue);
        free(endpoint);
    }
}

void rns_tcp_set_state_callback(rns_tcp_endpoint_t *endpoint,
                                rns_tcp_state_callback_t callback,
                                void *context) {
    if (endpoint != NULL) {
        endpoint->state_callback = callback;
        endpoint->state_context = context;
    }
}

rns_tcp_state_t rns_tcp_state(const rns_tcp_endpoint_t *endpoint) {
    return endpoint == NULL ? RNS_TCP_DISCONNECTED : endpoint->state;
}

size_t rns_tcp_pending_bytes(const rns_tcp_endpoint_t *endpoint) {
    return endpoint == NULL ? 0U : endpoint->send_length;
}

size_t rns_tcp_malformed_frames(const rns_tcp_endpoint_t *endpoint) {
    return endpoint == NULL ? 0U : endpoint->decoder.malformed_frames;
}

size_t rns_tcp_oversized_frames(const rns_tcp_endpoint_t *endpoint) {
    return endpoint == NULL ? 0U : endpoint->decoder.oversized_frames;
}

static rns_status_t resolve_tcp(const char *host,
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
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
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

rns_status_t rns_tcp_listen(rns_tcp_endpoint_t *endpoint,
                            const char *host,
                            uint16_t port,
                            int backlog) {
    struct sockaddr_storage address;
    socklen_t length;
    int reuse = 1;
    rns_status_t status;

    if (endpoint == NULL || backlog <= 0 || endpoint->state != RNS_TCP_DISCONNECTED) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    status = open_socket(endpoint);
    if (status != RNS_OK) {
        return status;
    }
    status = resolve_tcp(host, port, endpoint->native_family, true, &address, &length);
    if (status != RNS_OK) {
        return status;
    }
    if (setsockopt(endpoint->descriptor, SOL_SOCKET, SO_REUSEADDR,
                   &reuse, (socklen_t)sizeof(reuse)) != 0 ||
        bind(endpoint->descriptor, (const struct sockaddr *)&address, length) != 0 ||
        listen(endpoint->descriptor, backlog) != 0) {
        return RNS_ERROR_IO;
    }
    set_state(endpoint, RNS_TCP_LISTENING);
    return RNS_OK;
}

static rns_status_t native_address(const struct sockaddr *native,
                                   socklen_t length,
                                   rns_udp_address_t *address) {
    memset(address, 0, sizeof(*address));
    if (native->sa_family == AF_INET && length >= (socklen_t)sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *ipv4 = (const struct sockaddr_in *)native;
        address->family = RNS_UDP_IPV4;
        memcpy(address->address, &ipv4->sin_addr, sizeof(ipv4->sin_addr));
        address->port = ntohs(ipv4->sin_port);
        return RNS_OK;
    }
    if (native->sa_family == AF_INET6 && length >= (socklen_t)sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6 *ipv6 = (const struct sockaddr_in6 *)native;
        address->family = RNS_UDP_IPV6;
        memcpy(address->address, &ipv6->sin6_addr, sizeof(ipv6->sin6_addr));
        address->port = ntohs(ipv6->sin6_port);
        address->scope_id = ipv6->sin6_scope_id;
        return RNS_OK;
    }
    return RNS_ERROR_UNSUPPORTED;
}

rns_status_t rns_tcp_local_address(const rns_tcp_endpoint_t *endpoint,
                                   rns_udp_address_t *address) {
    struct sockaddr_storage native;
    socklen_t length = (socklen_t)sizeof(native);
    if (endpoint == NULL || address == NULL || endpoint->descriptor < 0) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    if (getsockname(endpoint->descriptor, (struct sockaddr *)&native, &length) != 0) {
        return RNS_ERROR_IO;
    }
    return native_address((const struct sockaddr *)&native, length, address);
}

rns_status_t rns_tcp_accept(rns_tcp_endpoint_t *listener,
                            rns_tcp_endpoint_t **connection,
                            size_t send_queue_capacity) {
    int accepted;
    rns_tcp_endpoint_t *created;
    rns_status_t status;

    if (listener == NULL || connection == NULL || listener->state != RNS_TCP_LISTENING) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    accepted = accept(listener->descriptor, NULL, NULL);
    if (accepted < 0) {
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? RNS_ERROR_TIMEOUT : RNS_ERROR_IO;
    }
    if (make_nonblocking(accepted) != RNS_OK) {
        (void)close(accepted);
        return RNS_ERROR_IO;
    }
    status = allocate_endpoint(&created, listener->native_family, send_queue_capacity);
    if (status != RNS_OK) {
        (void)close(accepted);
        return status;
    }
    created->descriptor = accepted;
    set_state(created, RNS_TCP_CONNECTED);
    *connection = created;
    return RNS_OK;
}

rns_status_t rns_tcp_connect(rns_tcp_endpoint_t *endpoint,
                             const char *host,
                             uint16_t port) {
    struct sockaddr_storage address;
    socklen_t length;
    rns_status_t status;

    if (endpoint == NULL || host == NULL || endpoint->state != RNS_TCP_DISCONNECTED) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    status = open_socket(endpoint);
    if (status != RNS_OK) {
        return status;
    }
    status = resolve_tcp(host, port, endpoint->native_family, false, &address, &length);
    if (status != RNS_OK) {
        return status;
    }
    if (connect(endpoint->descriptor, (const struct sockaddr *)&address, length) == 0) {
        set_state(endpoint, RNS_TCP_CONNECTED);
        return RNS_OK;
    }
    if (errno == EINPROGRESS || errno == EWOULDBLOCK) {
        set_state(endpoint, RNS_TCP_CONNECTING);
        return RNS_OK;
    }
    rns_tcp_disconnect(endpoint);
    return RNS_ERROR_IO;
}

rns_status_t rns_tcp_finish_connect(rns_tcp_endpoint_t *endpoint) {
    int error = 0;
    socklen_t length = (socklen_t)sizeof(error);

    if (endpoint == NULL) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    if (endpoint->state == RNS_TCP_CONNECTED) {
        return RNS_OK;
    }
    if (endpoint->state != RNS_TCP_CONNECTING) {
        return RNS_ERROR_INVALID_STATE;
    }
    if (getsockopt(endpoint->descriptor, SOL_SOCKET, SO_ERROR, &error, &length) != 0) {
        rns_tcp_disconnect(endpoint);
        return RNS_ERROR_IO;
    }
    if (error == 0) {
        set_state(endpoint, RNS_TCP_CONNECTED);
        return RNS_OK;
    }
    if (error == EINPROGRESS || error == EALREADY) {
        return RNS_ERROR_TIMEOUT;
    }
    rns_tcp_disconnect(endpoint);
    return RNS_ERROR_IO;
}

void rns_tcp_disconnect(rns_tcp_endpoint_t *endpoint) {
    if (endpoint != NULL) {
        if (endpoint->descriptor >= 0) {
            (void)close(endpoint->descriptor);
            endpoint->descriptor = -1;
        }
        endpoint->send_head = 0U;
        endpoint->send_length = 0U;
        rns_hdlc_decoder_reset(&endpoint->decoder);
        set_state(endpoint, RNS_TCP_DISCONNECTED);
    }
}

static void ring_write(rns_tcp_endpoint_t *endpoint, const uint8_t *data, size_t length) {
    size_t tail = (endpoint->send_head + endpoint->send_length) % endpoint->send_capacity;
    size_t first = endpoint->send_capacity - tail;
    if (first > length) {
        first = length;
    }
    memcpy(endpoint->send_queue + tail, data, first);
    memcpy(endpoint->send_queue, data + first, length - first);
    endpoint->send_length += length;
}

rns_status_t rns_tcp_queue_frame(rns_tcp_endpoint_t *endpoint,
                                 const uint8_t *packet,
                                 size_t packet_length) {
    uint8_t encoded[(RNS_MTU * 2U) + 2U];
    size_t encoded_length = 0U;
    rns_status_t status;

    if (endpoint == NULL || packet == NULL || packet_length == 0U) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    if (endpoint->state != RNS_TCP_CONNECTED) {
        return RNS_ERROR_INVALID_STATE;
    }
    if (packet_length > RNS_MTU) {
        return RNS_ERROR_OVERFLOW;
    }
    status = rns_hdlc_encode(packet, packet_length, encoded, sizeof(encoded), &encoded_length);
    if (status != RNS_OK) {
        return status;
    }
    if (encoded_length > endpoint->send_capacity - endpoint->send_length) {
        return RNS_ERROR_TIMEOUT;
    }
    ring_write(endpoint, encoded, encoded_length);
    return RNS_OK;
}

rns_status_t rns_tcp_flush(rns_tcp_endpoint_t *endpoint, size_t *bytes_sent) {
    size_t total = 0U;

    if (endpoint == NULL || bytes_sent == NULL) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    *bytes_sent = 0U;
    if (endpoint->state != RNS_TCP_CONNECTED) {
        return RNS_ERROR_INVALID_STATE;
    }
    while (endpoint->send_length != 0U) {
        size_t contiguous = endpoint->send_capacity - endpoint->send_head;
        ssize_t sent;
        if (contiguous > endpoint->send_length) {
            contiguous = endpoint->send_length;
        }
        sent = send_without_sigpipe(endpoint->descriptor,
                                    endpoint->send_queue + endpoint->send_head,
                                    contiguous);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            rns_tcp_disconnect(endpoint);
            return RNS_ERROR_IO;
        }
        if (sent == 0) {
            break;
        }
        endpoint->send_head = (endpoint->send_head + (size_t)sent) % endpoint->send_capacity;
        endpoint->send_length -= (size_t)sent;
        total += (size_t)sent;
    }
    *bytes_sent = total;
    return RNS_OK;
}

rns_status_t rns_tcp_poll_receive(rns_tcp_endpoint_t *endpoint,
                                  rns_frame_callback_t callback,
                                  void *context,
                                  size_t *bytes_received) {
    uint8_t input[2048];
    size_t total = 0U;

    if (endpoint == NULL || callback == NULL || bytes_received == NULL) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    *bytes_received = 0U;
    if (endpoint->state != RNS_TCP_CONNECTED) {
        return RNS_ERROR_INVALID_STATE;
    }
    for (;;) {
        ssize_t length = recv(endpoint->descriptor, input, sizeof(input), 0);
        if (length < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            rns_tcp_disconnect(endpoint);
            return RNS_ERROR_IO;
        }
        if (length == 0) {
            rns_tcp_disconnect(endpoint);
            return RNS_ERROR_IO;
        }
        total += (size_t)length;
        {
            rns_status_t status = rns_hdlc_decoder_feed(&endpoint->decoder, input,
                                                        (size_t)length, callback, context);
            if (status != RNS_OK) {
                *bytes_received = total;
                return status;
            }
        }
    }
    *bytes_received = total;
    return RNS_OK;
}
