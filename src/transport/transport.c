#include "reticulum/transport.h"

#include <stdlib.h>
#include <string.h>

static double now_of(rns_transport *transport) {
    return transport->config.clock(transport->config.clock_context);
}

static uint64_t blob_timebase(const uint8_t blob[10]) {
    uint64_t value = 0;
    for (size_t i = 5; i < 10; ++i) value = (value << 8) | blob[i];
    return value;
}

int rns_transport_init(rns_transport *transport, const rns_transport_config *config) {
    if (!transport || !config || !config->clock || config->path_capacity == 0 ||
        config->dedupe_capacity == 0 || config->random_blob_history == 0 ||
        config->random_blob_history > RNS_TRANSPORT_MAX_RANDOM_BLOBS ||
        config->path_lifetime <= 0 || config->dedupe_lifetime <= 0) return 0;
    memset(transport, 0, sizeof(*transport)); transport->config = *config;
    transport->paths = calloc(config->path_capacity, sizeof(*transport->paths));
    transport->dedupe = calloc(config->dedupe_capacity, sizeof(*transport->dedupe));
    if (!transport->paths || !transport->dedupe) { rns_transport_free(transport); return 0; }
    return 1;
}

void rns_transport_free(rns_transport *transport) {
    if (!transport) return; free(transport->paths); free(transport->dedupe); memset(transport, 0, sizeof(*transport));
}

size_t rns_transport_expire(rns_transport *transport) {
    size_t removed = 0; double now;
    if (!transport || !transport->paths || !transport->dedupe) return 0; now = now_of(transport);
    for (size_t i = 0; i < transport->config.path_capacity; ++i)
        if (transport->paths[i].occupied && now >= transport->paths[i].expires_at) { memset(&transport->paths[i], 0, sizeof(transport->paths[i])); ++removed; }
    for (size_t i = 0; i < transport->config.dedupe_capacity; ++i)
        if (transport->dedupe[i].occupied && now >= transport->dedupe[i].expires_at) { memset(&transport->dedupe[i], 0, sizeof(transport->dedupe[i])); ++removed; }
    return removed;
}

static rns_path_entry *find_path(rns_transport *transport, const uint8_t hash[16]) {
    for (size_t i = 0; i < transport->config.path_capacity; ++i)
        if (transport->paths[i].occupied && memcmp(transport->paths[i].destination_hash, hash, 16) == 0) return &transport->paths[i];
    return NULL;
}

const rns_path_entry *rns_transport_lookup(rns_transport *transport, const uint8_t destination_hash[16]) {
    rns_path_entry *entry; double now;
    if (!transport || !destination_hash || !transport->paths) return NULL; now = now_of(transport); entry = find_path(transport, destination_hash);
    if (entry && now >= entry->expires_at) { memset(entry, 0, sizeof(*entry)); return NULL; }
    return entry;
}

int rns_transport_mark_unresponsive(rns_transport *transport, const uint8_t destination_hash[16]) {
    rns_path_entry *entry;
    if (!transport || !destination_hash) return 0; entry = find_path(transport, destination_hash);
    if (!entry || now_of(transport) >= entry->expires_at) return 0; entry->unresponsive = 1; return 1;
}

static int has_blob(const rns_path_entry *entry, const uint8_t blob[10]) {
    for (size_t i = 0; i < entry->random_blob_count; ++i)
        if (memcmp(entry->random_blobs[i], blob, 10) == 0) return 1;
    return 0;
}

static rns_path_entry *path_slot(rns_transport *transport, double now) {
    rns_path_entry *oldest = NULL;
    for (size_t i = 0; i < transport->config.path_capacity; ++i) {
        rns_path_entry *entry = &transport->paths[i];
        if (!entry->occupied || now >= entry->expires_at) return entry;
        if (!oldest || entry->updated_at < oldest->updated_at) oldest = entry;
    }
    return oldest;
}

static void append_blob(rns_transport *transport, rns_path_entry *entry, const uint8_t blob[10]) {
    size_t limit = transport->config.random_blob_history;
    if (has_blob(entry, blob)) return;
    if (entry->random_blob_count == limit) {
        memmove(entry->random_blobs, entry->random_blobs + 1, (limit - 1) * 10);
        entry->random_blob_count--;
    }
    memcpy(entry->random_blobs[entry->random_blob_count++], blob, 10);
}

static uint64_t entry_timebase(const rns_path_entry *entry) {
    uint64_t latest = 0;
    for (size_t i = 0; i < entry->random_blob_count; ++i) {
        uint64_t value = blob_timebase(entry->random_blobs[i]);
        if (value > latest) latest = value;
    }
    return latest;
}

rns_path_update_result rns_transport_consider_announce(
    rns_transport *transport, const uint8_t destination_hash[16], const uint8_t next_hop[16],
    uint64_t interface_id, int32_t interface_gravity, uint8_t hops,
    const uint8_t random_blob[10], const uint8_t announce_packet_hash[32]) {
    rns_path_entry *entry; double now; uint64_t timebase; int expired, duplicate, accept = 0;
    rns_path_update_result result;
    if (!transport || !destination_hash || !next_hop || !random_blob || !announce_packet_hash || !transport->paths) return RNS_PATH_REJECTED;
    now = now_of(transport); timebase = blob_timebase(random_blob); entry = find_path(transport, destination_hash);
    if (!entry) { entry = path_slot(transport, now); result = RNS_PATH_INSERTED; accept = 1; }
    else {
        result = RNS_PATH_UPDATED; expired = now >= entry->expires_at; duplicate = has_blob(entry, random_blob);
        if (hops <= entry->hops) {
            if (!duplicate && timebase > entry->announce_timebase) accept = 1;
            else if (timebase == entry->announce_timebase && interface_gravity > entry->interface_gravity) accept = 1;
        } else {
            if (expired && !duplicate) accept = 1;
            else if (!expired && !duplicate && timebase > entry->announce_timebase) accept = 1;
            else if (!expired && timebase == entry->announce_timebase && entry->unresponsive) accept = 1;
        }
    }
    if (!accept) return RNS_PATH_REJECTED;
    /* Preserve recently observed blobs when replacing an existing destination. */
    if (!entry->occupied || memcmp(entry->destination_hash, destination_hash, 16) != 0) memset(entry, 0, sizeof(*entry));
    memcpy(entry->destination_hash, destination_hash, 16); memcpy(entry->next_hop, next_hop, 16);
    memcpy(entry->announce_packet_hash, announce_packet_hash, 32); append_blob(transport, entry, random_blob);
    entry->announce_timebase = entry_timebase(entry); entry->interface_id = interface_id; entry->interface_gravity = interface_gravity;
    entry->hops = hops; entry->unresponsive = 0; entry->updated_at = now; entry->expires_at = now + transport->config.path_lifetime; entry->occupied = 1;
    return result;
}

int rns_transport_accept_packet_hash(rns_transport *transport, const uint8_t packet_hash[32]) {
    rns_dedupe_entry *slot = NULL, *oldest = NULL; double now;
    if (!transport || !packet_hash || !transport->dedupe) return 0; now = now_of(transport);
    for (size_t i = 0; i < transport->config.dedupe_capacity; ++i) {
        rns_dedupe_entry *entry = &transport->dedupe[i];
        if (entry->occupied && now < entry->expires_at && memcmp(entry->hash, packet_hash, 32) == 0) return 0;
        if (!entry->occupied || now >= entry->expires_at) { if (!slot) slot = entry; }
        else if (!oldest || entry->seen_at < oldest->seen_at) oldest = entry;
    }
    if (!slot) slot = oldest; memset(slot, 0, sizeof(*slot)); memcpy(slot->hash, packet_hash, 32);
    slot->seen_at = now; slot->expires_at = now + transport->config.dedupe_lifetime; slot->occupied = 1; return 1;
}

int rns_path_request_build(const uint8_t destination_hash[16], const uint8_t requesting_transport[16],
                           const uint8_t *tag, size_t tag_length, uint8_t *out, size_t capacity, size_t *out_length) {
    size_t length, offset = 0;
    if (!destination_hash || !tag || tag_length == 0 || tag_length > 16 || !out || !out_length) return 0;
    length = 16 + (requesting_transport ? 16 : 0) + tag_length; if (capacity < length) return 0;
    memcpy(out + offset, destination_hash, 16); offset += 16;
    if (requesting_transport) { memcpy(out + offset, requesting_transport, 16); offset += 16; }
    memcpy(out + offset, tag, tag_length); *out_length = length; return 1;
}

int rns_path_request_parse(rns_path_request *request, const uint8_t *body, size_t body_length) {
    int includes_requesting_transport = body_length > 32u; size_t offset = 16;
    if (!request || !body || body_length < 17u || body_length > 48u) return 0;
    memset(request, 0, sizeof(*request)); memcpy(request->destination_hash, body, 16);
    if (includes_requesting_transport) { memcpy(request->requesting_transport, body + offset, 16); offset += 16; request->has_requesting_transport = 1; }
    request->tag_length = body_length - offset; memcpy(request->tag, body + offset, request->tag_length); return 1;
}
