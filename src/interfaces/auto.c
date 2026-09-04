#include "reticulum/auto.h"

#include "reticulum/crypto.h"
#include "reticulum/hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AUTO_DEFAULT_ANNOUNCE_INTERVAL 1.6
#define AUTO_DEFAULT_PEERING_TIMEOUT 22.0
#define AUTO_DEFAULT_INITIAL_WAIT 1.92
#define AUTO_DEDUPE_CAPACITY 48u
#define AUTO_DEDUPE_TTL 0.75

typedef struct auto_local_interface {
    rns_udp_address_t address;
    uint32_t interface_index;
    double next_announce;
    double last_echo;
} auto_local_interface_t;

typedef struct auto_peer {
    rns_auto_peer_info_t info;
    bool used;
} auto_peer_t;

typedef struct auto_dedupe {
    uint8_t hash[RNS_SHA256_SIZE];
    double received_at;
    bool used;
} auto_dedupe_t;

struct rns_auto_endpoint {
    rns_auto_options_t options;
    uint8_t group_id[RNS_AUTO_GROUP_ID_MAX];
    uint8_t multicast_address[16];
    auto_local_interface_t local[RNS_AUTO_MAX_LOCAL_INTERFACES];
    size_t local_count;
    auto_peer_t peers[RNS_AUTO_MAX_PEERS];
    size_t peer_count;
    auto_dedupe_t dedupe[AUTO_DEDUPE_CAPACITY];
    size_t dedupe_next;
    double online_at;
};

static double default_clock(void *context) {
    uint64_t milliseconds = 0u;
    (void)context;
    if (rns_hal_monotonic_ms(&milliseconds) != RNS_OK) return 0.0;
    return (double)milliseconds / 1000.0;
}

static bool valid_scope(rns_auto_scope_t scope) {
    return scope == RNS_AUTO_SCOPE_LINK || scope == RNS_AUTO_SCOPE_ADMIN ||
           scope == RNS_AUTO_SCOPE_SITE ||
           scope == RNS_AUTO_SCOPE_ORGANISATION ||
           scope == RNS_AUTO_SCOPE_GLOBAL;
}

static bool same_address(const rns_udp_address_t *left,
                         const rns_udp_address_t *right) {
    return left->family == RNS_UDP_IPV6 && right->family == RNS_UDP_IPV6 &&
           memcmp(left->address, right->address, 16u) == 0;
}

/* RFC 5952 form matches the source strings returned by Python's IPv6 socket. */
static bool ipv6_text(const uint8_t address[16], char output[40]) {
    uint16_t words[8];
    size_t best_start = 8u;
    size_t best_length = 0u;
    size_t position = 0u;
    for (size_t i = 0u; i < 8u; ++i)
        words[i] = (uint16_t)(((uint16_t)address[i * 2u] << 8u) |
                              address[i * 2u + 1u]);
    for (size_t i = 0u; i < 8u;) {
        if (words[i] != 0u) {
            ++i;
            continue;
        }
        size_t end = i + 1u;
        while (end < 8u && words[end] == 0u) ++end;
        if (end - i > best_length && end - i >= 2u) {
            best_start = i;
            best_length = end - i;
        }
        i = end;
    }
    for (size_t i = 0u; i < 8u;) {
        if (i == best_start) {
            if (position + 2u >= 40u) return false;
            output[position++] = ':';
            output[position++] = ':';
            i += best_length;
            continue;
        }
        if (position != 0u && output[position - 1u] != ':') {
            if (position + 1u >= 40u) return false;
            output[position++] = ':';
        }
        int written = snprintf(output + position, 40u - position, "%x",
                               (unsigned)words[i]);
        if (written <= 0 || (size_t)written >= 40u - position) return false;
        position += (size_t)written;
        ++i;
    }
    if (position == 0u || position >= 40u) return false;
    output[position] = '\0';
    return true;
}

static bool make_token(const rns_auto_endpoint_t *endpoint,
                       const uint8_t address[16],
                       uint8_t token[RNS_AUTO_PEERING_TOKEN_SIZE]) {
    char text[40];
    uint8_t input[RNS_AUTO_GROUP_ID_MAX + 39u];
    if (!ipv6_text(address, text)) return false;
    size_t text_length = strlen(text);
    memcpy(input, endpoint->group_id, endpoint->options.group_id_length);
    memcpy(input + endpoint->options.group_id_length, text, text_length);
    return rns_sha256(input, endpoint->options.group_id_length + text_length,
                      token) != 0;
}

static double endpoint_clock(const rns_auto_endpoint_t *endpoint) {
    return endpoint->options.clock(endpoint->options.callback_context);
}

void rns_auto_options_init(rns_auto_options_t *options) {
    static const uint8_t default_group[] = "reticulum";
    if (options == NULL) return;
    memset(options, 0, sizeof *options);
    options->group_id = default_group;
    options->group_id_length = sizeof default_group - 1u;
    options->discovery_scope = RNS_AUTO_SCOPE_LINK;
    options->multicast_type = RNS_AUTO_MULTICAST_TEMPORARY;
    options->discovery_port = RNS_AUTO_DEFAULT_DISCOVERY_PORT;
    options->data_port = RNS_AUTO_DEFAULT_DATA_PORT;
    options->peer_capacity = RNS_AUTO_DEFAULT_PEER_CAPACITY;
    options->announce_interval = AUTO_DEFAULT_ANNOUNCE_INTERVAL;
    options->peering_timeout = AUTO_DEFAULT_PEERING_TIMEOUT;
    options->reverse_peering_interval = AUTO_DEFAULT_ANNOUNCE_INTERVAL * 3.25;
    options->initial_wait = AUTO_DEFAULT_INITIAL_WAIT;
    options->clock = default_clock;
}

rns_status_t rns_auto_endpoint_create(rns_auto_endpoint_t **output,
                                      const rns_auto_options_t *options) {
    if (output == NULL || options == NULL || options->group_id == NULL ||
        options->group_id_length == 0u ||
        options->group_id_length > RNS_AUTO_GROUP_ID_MAX ||
        !valid_scope(options->discovery_scope) ||
        (options->multicast_type != RNS_AUTO_MULTICAST_PERMANENT &&
         options->multicast_type != RNS_AUTO_MULTICAST_TEMPORARY) ||
        options->discovery_port == 0u || options->discovery_port == UINT16_MAX ||
        options->data_port == 0u || options->peer_capacity == 0u ||
        options->peer_capacity > RNS_AUTO_MAX_PEERS ||
        options->announce_interval <= 0.0 || options->peering_timeout <= 0.0 ||
        options->reverse_peering_interval <= 0.0 || options->initial_wait < 0.0 ||
        options->clock == NULL || options->emit == NULL ||
        options->receive == NULL)
        return RNS_ERROR_INVALID_ARGUMENT;
    *output = NULL;
    rns_auto_endpoint_t *endpoint = calloc(1u, sizeof *endpoint);
    if (endpoint == NULL) return RNS_ERROR_NO_MEMORY;
    endpoint->options = *options;
    memcpy(endpoint->group_id, options->group_id, options->group_id_length);
    endpoint->options.group_id = endpoint->group_id;
    uint8_t hash[RNS_SHA256_SIZE];
    if (!rns_sha256(endpoint->group_id, endpoint->options.group_id_length,
                    hash)) {
        free(endpoint);
        return RNS_ERROR_CRYPTO;
    }
    endpoint->multicast_address[0] = 0xffu;
    endpoint->multicast_address[1] =
        (uint8_t)(((uint8_t)options->multicast_type << 4u) |
                  (uint8_t)options->discovery_scope);
    memcpy(endpoint->multicast_address + 4u, hash + 2u, 12u);
    endpoint->online_at = endpoint_clock(endpoint) + options->initial_wait;
    *output = endpoint;
    return RNS_OK;
}

void rns_auto_endpoint_destroy(rns_auto_endpoint_t *endpoint) {
    free(endpoint);
}

rns_status_t rns_auto_add_local_interface(rns_auto_endpoint_t *endpoint,
                                          const rns_udp_address_t *address,
                                          uint32_t interface_index) {
    if (endpoint == NULL || address == NULL || address->family != RNS_UDP_IPV6 ||
        interface_index == 0u || address->address[0] != 0xfeu ||
        address->address[1] != 0x80u)
        return RNS_ERROR_INVALID_ARGUMENT;
    for (size_t i = 0u; i < endpoint->local_count; ++i)
        if (endpoint->local[i].interface_index == interface_index ||
            same_address(&endpoint->local[i].address, address))
            return RNS_ERROR_INVALID_STATE;
    if (endpoint->local_count == RNS_AUTO_MAX_LOCAL_INTERFACES)
        return RNS_ERROR_OVERFLOW;
    auto_local_interface_t *local = &endpoint->local[endpoint->local_count++];
    local->address = *address;
    local->address.port = endpoint->options.discovery_port;
    local->address.scope_id = interface_index;
    local->interface_index = interface_index;
    local->next_announce = endpoint_clock(endpoint);
    return RNS_OK;
}

rns_status_t rns_auto_multicast_address(const rns_auto_endpoint_t *endpoint,
                                        rns_udp_address_t *address) {
    if (endpoint == NULL || address == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    memset(address, 0, sizeof *address);
    address->family = RNS_UDP_IPV6;
    memcpy(address->address, endpoint->multicast_address, 16u);
    address->port = endpoint->options.discovery_port;
    return RNS_OK;
}

rns_status_t rns_auto_local_token(const rns_auto_endpoint_t *endpoint,
                                  uint32_t interface_index,
                                  uint8_t token[RNS_AUTO_PEERING_TOKEN_SIZE]) {
    if (endpoint == NULL || token == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    for (size_t i = 0u; i < endpoint->local_count; ++i)
        if (endpoint->local[i].interface_index == interface_index)
            return make_token(endpoint, endpoint->local[i].address.address,
                              token)
                       ? RNS_OK
                       : RNS_ERROR_CRYPTO;
    return RNS_ERROR_NOT_FOUND;
}

static auto_peer_t *find_peer(rns_auto_endpoint_t *endpoint,
                              const rns_udp_address_t *source) {
    for (size_t i = 0u; i < endpoint->options.peer_capacity; ++i)
        if (endpoint->peers[i].used &&
            same_address(&endpoint->peers[i].info.address, source))
            return &endpoint->peers[i];
    return NULL;
}

rns_status_t rns_auto_ingest_discovery(
    rns_auto_endpoint_t *endpoint, const rns_udp_address_t *source,
    uint32_t interface_index, const uint8_t *token, size_t token_length) {
    if (endpoint == NULL || source == NULL || token == NULL ||
        source->family != RNS_UDP_IPV6 || interface_index == 0u)
        return RNS_ERROR_INVALID_ARGUMENT;
    if (token_length != RNS_AUTO_PEERING_TOKEN_SIZE)
        return RNS_ERROR_PROTOCOL;
    if (!rns_auto_online(endpoint)) return RNS_ERROR_INVALID_STATE;
    uint8_t expected[RNS_AUTO_PEERING_TOKEN_SIZE];
    if (!make_token(endpoint, source->address, expected))
        return RNS_ERROR_CRYPTO;
    if (memcmp(expected, token, sizeof expected) != 0)
        return RNS_ERROR_CRYPTO;
    double now = endpoint_clock(endpoint);
    for (size_t i = 0u; i < endpoint->local_count; ++i) {
        if (!same_address(&endpoint->local[i].address, source)) continue;
        endpoint->local[i].last_echo = now;
        return RNS_OK;
    }
    auto_peer_t *peer = find_peer(endpoint, source);
    if (peer == NULL) {
        for (size_t i = 0u; i < endpoint->options.peer_capacity; ++i)
            if (!endpoint->peers[i].used) {
                peer = &endpoint->peers[i];
                memset(peer, 0, sizeof *peer);
                peer->used = true;
                peer->info.address = *source;
                peer->info.address.port = endpoint->options.data_port;
                peer->info.address.scope_id = interface_index;
                peer->info.interface_index = interface_index;
                peer->info.last_outbound = now;
                endpoint->peer_count++;
                break;
            }
        if (peer == NULL) return RNS_ERROR_OVERFLOW;
    }
    peer->info.last_heard = now;
    peer->info.interface_index = interface_index;
    peer->info.address.scope_id = interface_index;
    return RNS_OK;
}

static bool duplicate_packet(rns_auto_endpoint_t *endpoint,
                             const uint8_t *packet, size_t packet_length,
                             double now) {
    uint8_t hash[RNS_SHA256_SIZE];
    if (!rns_sha256(packet, packet_length, hash)) return true;
    for (size_t i = 0u; i < AUTO_DEDUPE_CAPACITY; ++i)
        if (endpoint->dedupe[i].used &&
            now < endpoint->dedupe[i].received_at + AUTO_DEDUPE_TTL &&
            memcmp(endpoint->dedupe[i].hash, hash, sizeof hash) == 0)
            return true;
    auto_dedupe_t *record = &endpoint->dedupe[endpoint->dedupe_next];
    memcpy(record->hash, hash, sizeof hash);
    record->received_at = now;
    record->used = true;
    endpoint->dedupe_next = (endpoint->dedupe_next + 1u) % AUTO_DEDUPE_CAPACITY;
    return false;
}

rns_status_t rns_auto_ingest_data(rns_auto_endpoint_t *endpoint,
                                  const rns_udp_address_t *source,
                                  uint32_t interface_index,
                                  const uint8_t *packet,
                                  size_t packet_length) {
    if (endpoint == NULL || source == NULL || packet == NULL ||
        source->family != RNS_UDP_IPV6 || interface_index == 0u ||
        packet_length == 0u)
        return RNS_ERROR_INVALID_ARGUMENT;
    if (packet_length > RNS_AUTO_HW_MTU) return RNS_ERROR_OVERFLOW;
    if (!rns_auto_online(endpoint)) return RNS_ERROR_INVALID_STATE;
    auto_peer_t *peer = find_peer(endpoint, source);
    if (peer == NULL) return RNS_ERROR_NOT_FOUND;
    double now = endpoint_clock(endpoint);
    peer->info.last_heard = now;
    if (duplicate_packet(endpoint, packet, packet_length, now)) return RNS_OK;
    return endpoint->options.receive(packet, packet_length, source,
                                     interface_index,
                                     endpoint->options.callback_context);
}

rns_status_t rns_auto_send(rns_auto_endpoint_t *endpoint,
                           const uint8_t *packet, size_t packet_length,
                           size_t *peers_sent) {
    if (endpoint == NULL || packet == NULL || packet_length == 0u ||
        peers_sent == NULL)
        return RNS_ERROR_INVALID_ARGUMENT;
    *peers_sent = 0u;
    if (packet_length > RNS_AUTO_HW_MTU) return RNS_ERROR_OVERFLOW;
    if (!rns_auto_online(endpoint)) return RNS_ERROR_INVALID_STATE;
    rns_status_t first_error = RNS_OK;
    for (size_t i = 0u; i < endpoint->options.peer_capacity; ++i) {
        auto_peer_t *peer = &endpoint->peers[i];
        if (!peer->used) continue;
        rns_status_t status = endpoint->options.emit(
            RNS_AUTO_EMIT_DATA, peer->info.interface_index,
            &peer->info.address, packet, packet_length,
            endpoint->options.callback_context);
        if (status == RNS_OK)
            (*peers_sent)++;
        else if (first_error == RNS_OK)
            first_error = status;
    }
    if (*peers_sent == 0u)
        return first_error != RNS_OK ? first_error : RNS_ERROR_NOT_FOUND;
    return RNS_OK;
}

rns_status_t rns_auto_poll(rns_auto_endpoint_t *endpoint) {
    if (endpoint == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    double now = endpoint_clock(endpoint);
    rns_status_t first_error = RNS_OK;
    for (size_t i = 0u; i < endpoint->options.peer_capacity; ++i) {
        auto_peer_t *peer = &endpoint->peers[i];
        if (!peer->used) continue;
        if (now > peer->info.last_heard + endpoint->options.peering_timeout) {
            memset(peer, 0, sizeof *peer);
            endpoint->peer_count--;
            continue;
        }
        if (now > peer->info.last_outbound +
                      endpoint->options.reverse_peering_interval) {
            uint8_t token[RNS_AUTO_PEERING_TOKEN_SIZE];
            rns_status_t status = rns_auto_local_token(
                endpoint, peer->info.interface_index, token);
            rns_udp_address_t destination = peer->info.address;
            destination.port =
                (uint16_t)(endpoint->options.discovery_port + 1u);
            if (status == RNS_OK)
                status = endpoint->options.emit(
                    RNS_AUTO_EMIT_UNICAST_DISCOVERY,
                    peer->info.interface_index, &destination, token,
                    sizeof token, endpoint->options.callback_context);
            if (status != RNS_OK && first_error == RNS_OK) first_error = status;
            peer->info.last_outbound = now;
        }
    }
    for (size_t i = 0u; i < endpoint->local_count; ++i) {
        auto_local_interface_t *local = &endpoint->local[i];
        if (now < local->next_announce) continue;
        uint8_t token[RNS_AUTO_PEERING_TOKEN_SIZE];
        rns_udp_address_t destination = {0};
        destination.family = RNS_UDP_IPV6;
        memcpy(destination.address, endpoint->multicast_address, 16u);
        destination.port = endpoint->options.discovery_port;
        destination.scope_id = local->interface_index;
        rns_status_t status = rns_auto_local_token(
            endpoint, local->interface_index, token);
        if (status == RNS_OK)
            status = endpoint->options.emit(
                RNS_AUTO_EMIT_MULTICAST_DISCOVERY, local->interface_index,
                &destination, token, sizeof token,
                endpoint->options.callback_context);
        if (status != RNS_OK && first_error == RNS_OK) first_error = status;
        local->next_announce = now + endpoint->options.announce_interval;
    }
    return first_error;
}

bool rns_auto_online(const rns_auto_endpoint_t *endpoint) {
    return endpoint != NULL && endpoint->local_count != 0u &&
           endpoint_clock(endpoint) >= endpoint->online_at;
}

size_t rns_auto_peer_count(const rns_auto_endpoint_t *endpoint) {
    return endpoint != NULL ? endpoint->peer_count : 0u;
}

size_t rns_auto_peer_snapshot(const rns_auto_endpoint_t *endpoint,
                              rns_auto_peer_info_t *peers, size_t capacity) {
    if (endpoint == NULL || (capacity != 0u && peers == NULL)) return 0u;
    size_t count = 0u;
    for (size_t i = 0u;
         i < endpoint->options.peer_capacity && count < capacity; ++i)
        if (endpoint->peers[i].used) peers[count++] = endpoint->peers[i].info;
    return count;
}
